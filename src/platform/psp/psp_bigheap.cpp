// the big-block heap region on the PSP - see psp_bigheap.hpp
// (keys, log lines) and psp_bigheap_core.hpp (allocator, routing).
//
// PSP: the wrappers. psp/CMakeLists.txt links with -Wl,--wrap=malloc,free,realloc,calloc,memalign,aligned_alloc,
// malloc_usable_size: every reference to those symbols in every object and archive (libstdc++'s operator new /
// delete included - they call malloc / free / memalign) resolves to __wrap_*, and __real_* is newlib's own. newlib's
// reentrant _malloc_r family is NOT wrapped: newlib's internals (stdio buffers, strdup) keep their memory in newlib,
// and a free() of it lands there by address. Until bigheapInit (static constructors, the boot) and with bigheap=0 the
// wrappers call __real_* straight away - the old heap exactly.
//
// The region: one newlib block (__real_memalign) taken right after the emulator's ballast, before the audio / loader
// / network threads exist - so it lies low in the heap with the boot's long-lived objects below it, and the emulator
// and the PSP-3000 carve it from the same heap size. sceKernelAllocPartitionMemory was the alternative: the user
// partition outside newlib's heap is only PSP_HEAP_THRESHOLD_SIZE_KB (2.5 MB, kept for netconf / OSK / stacks) - the
// region would have to come out of the link-time heap size, fixed for every wowee.cfg, and bigheap=0 could not give it
// back. Taking it from the heap keeps bigheapkb a run-time key and bigheap=0 exactly the old layout.
//
// The lock: a LwMutex (user mode; no syscall when uncontended - libcglue's own malloc lock is one too). Allocating
// threads: main, the tile/model loader, the audio decoder, the network pump. Interrupts-off was not needed: the
// critical sections walk a free list of big blocks only (tens of entries).
#include "platform/psp/psp_bigheap.hpp"

#include "platform/psp/psp_bigheap_core.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_small_arena.hpp"  // bigheap=2, the small-object arena
#include "platform/psp/psp_log.hpp"

#include <pspthreadman.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

extern "C" {
void* __real_malloc(size_t n);
void __real_free(void* p);
void* __real_realloc(void* p, size_t n);
void* __real_calloc(size_t a, size_t b);
void* __real_memalign(size_t align, size_t n);
void* __real_aligned_alloc(size_t align, size_t n);
size_t __real_malloc_usable_size(void* p);
}

namespace wowee::platform::psp {

namespace {

void* smallMalloc(size_t n) { return __real_malloc(n); }
void smallFree(void* p) { __real_free(p); }
void* smallRealloc(void* p, size_t n) { return __real_realloc(p, n); }
void* smallCalloc(size_t a, size_t b) { return __real_calloc(a, b); }
void* smallMemalign(size_t a, size_t n) { return __real_memalign(a, n); }
size_t smallUsable(void* p) { return __real_malloc_usable_size(p); }

SceLwMutexWorkarea g_mutex;
void lockMutex(void*) { sceKernelLockLwMutex(&g_mutex, 1, nullptr); }
void unlockMutex(void*) { sceKernelUnlockLwMutex(&g_mutex, 1); }

// constinit: the wrappers run before any constructor (crt0, libstdc++'s own init) - the router must be valid from the
// first malloc, so it is constant-initialized (off) and only bigheapInit turns it on.
constinit BigHeapRouter g_router;
uint32_t g_regionChunkBytes = 0;  // what the region takes in newlib's mallinfo (its block + header)
uint32_t g_floorBytes = 1024u * 1024u;
uint32_t g_heapTotalBytes = 0;   // newlib's size at boot (in use + the largest block, before the region)
uint32_t g_ballastBytes = 0;     // the emulator's ballast (held in newlib for the whole run)
constexpr uint32_t kBlockCapBytes = 1024u * 1024u;  // heapGivesKb's third way: a block of min(n, 1 MB) must be there
// Memory budget (measured in the emulator: 14 MB free, largest 640 KB - every probe that asked for 1 MB in
// one piece said no for good: the governor sat at level 4, the big WMOs read nothing): bigheap=0/2's headroom probe
// asks one block of min(n, heapblockkb) besides the total.
uint32_t g_probeBlockCapBytes = 256u * 1024u;
uint32_t g_guardBytes = 4096u * 1024u;  // heapguardkb
bool g_loggedFull = false, g_loggedSpill = false, g_loggedBad = false;
// bigheap=2 - small requests (<= smallarenamax bytes) in an arena of their own.
SmallArena g_small;
bool g_smallOn = false;
uint32_t g_smallMax = 1024;
uint32_t g_smallFallbacks = 0, g_smallFirstFallback = 0;
bool g_loggedSmallFull = false, g_loggedSmallBad = false;
// In the emulator a capital city's chunk / texture blobs of every size shredded newlib (mem_frag free
// 9.4 MB, largest 896 KB): the big WMOs' data in an arena of their own (wmoarenakb), frees routed by address.
BigHeapCore g_wmo;
bool g_wmoOn = false, g_mutexOn = false;
uint32_t g_wmoBigOpen = 0, g_worldKeepBytes = 1024u * 1024u, g_guardRefusals = 0;
uint32_t g_wmoFallbacks = 0;
// heapbigstats - newlib blocks >= 64 KB alive by size class (heap_big_allocs, the mem_share cadence).
bool g_bigStats = false;
constexpr int kBigClasses = 6;  // 64-128 KB, 128-256, 256-512, 512 KB-1 MB, 1-2 MB, >= 2 MB
uint32_t g_bigN[kBigClasses] = {}, g_bigKb[kBigClasses] = {}, g_bigAllocs = 0, g_bigFrees = 0;  // racy: stats only
inline int bigClass(size_t u) {
    int c = 0;
    for (size_t lim = 128u * 1024u; c < kBigClasses - 1 && u >= lim; lim <<= 1) ++c;
    return c;
}

unsigned U(uint32_t v) { return static_cast<unsigned>(v); }

// The largest block newlib gives now, KB (64 KB steps; written: gcc drops a malloc/free pair it can see through).
uint32_t realLargestKb() {
    uint32_t lo = 0, hi = 64u * 1024u;
    while (hi - lo > 64u) {
        const uint32_t mid = (lo + hi) / 2u;
        void* volatile p = __real_malloc(static_cast<size_t>(mid) * 1024u);
        if (p) {
            static_cast<volatile char*>(p)[0] = 1;
            __real_free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool realGives(size_t bytes) {
    void* volatile p = __real_malloc(bytes ? bytes : 1u);
    if (!p) return false;
    static_cast<volatile char*>(p)[0] = 0;
    __real_free(p);
    return true;
}

}  // namespace

// bigheap=2 - the small-object arena (psp_small_arena.hpp) taken from newlib now, low.
static void smallArenaInit(uint32_t largestKb) {
    // 1536 KB (was 5120): an emulator autopilot run peaked at 638 KB over all 19 blocks.
    const uint32_t kb = static_cast<uint32_t>(std::clamp(configGetInt("smallarenakb", 1536), 256, 16384));
    g_smallMax = static_cast<uint32_t>(std::clamp(configGetInt("smallarenamax", 2048), 16,
                                                  static_cast<int>(SmallArena::kMaxSize)));
    if (kb + 8192u > largestKb) {
        PSP_LOGE(tag::Mem, "bigheap_init on=0 mode=2 arena_kb=%u heap_largest_kb=%u (too big)", U(kb), U(largestKb));
        return;
    }
    if (sceKernelCreateLwMutex(&g_mutex, "wowee_bigheap", 0, 0, nullptr) < 0) {
        PSP_LOGE(tag::Mem, "bigheap_init on=0 mode=2 lwmutex_failed");
        return;
    }
    g_mutexOn = true;
    void* region = __real_memalign(64, static_cast<size_t>(kb) * 1024u);
    if (!region || !g_small.init(region, static_cast<size_t>(kb) * 1024u)) {
        if (region) __real_free(region);
        sceKernelDeleteLwMutex(&g_mutex);
        g_mutexOn = false;
        PSP_LOGE(tag::Mem, "bigheap_init on=0 mode=2 region_failed arena_kb=%u", U(kb));
        return;
    }
    g_regionChunkBytes = static_cast<uint32_t>(__real_malloc_usable_size(region)) + 8u;  // + newlib's chunk header
    g_smallOn = true;
    PSP_LOGI(tag::Mem, "bigheap_init on=2 small_arena_kb=%u max_bytes=%u pages=%u base=%p heap_largest_kb=%u", U(kb),
             U(g_smallMax), U(g_small.stats().pages), region, U(largestKb));
}

// The big WMOs' arena - taken now, after the other boot blocks, kept for the run.
static void wmoArenaInit() {
    // 7168 (was 4608) - the NPC models moved in too (npcarena; 2.4-3.6 MB in a busy village / the capitals).
    const uint32_t kb = static_cast<uint32_t>(std::clamp(configGetInt("wmoarenakb", 7168), 0, 16384));
    g_bigStats = configGetInt("heapbigstats", 1) != 0;
    // 1024 (was 3072) - the WMO's resident set is capped by wmobudget; this is only room for its churn.
    g_worldKeepBytes = static_cast<uint32_t>(std::clamp(configGetInt("worldarenawmokb", 1024), 0, 16384)) * 1024u;
    if (!kb) {
        PSP_LOGI(tag::Mem, "wmo_arena off (wmoarenakb=0)");
        return;
    }
    if (!g_mutexOn) {
        if (sceKernelCreateLwMutex(&g_mutex, "wowee_bigheap", 0, 0, nullptr) < 0) return;
        g_mutexOn = true;
    }
    void* region = __real_memalign(64, static_cast<size_t>(kb) * 1024u);
    if (!region || !g_wmo.init(region, static_cast<size_t>(kb) * 1024u)) {
        if (region) __real_free(region);
        PSP_LOGE(tag::Mem, "wmo_arena_fail kb=%u", U(kb));
        return;
    }
    g_wmoOn = true;
    PSP_LOGI(tag::Mem, "wmo_arena kb=%u base=%p", U(kb), region);
}

void bigheapInit(uint32_t ballastKb) {
    struct WmoArenaAfter {
        ~WmoArenaAfter() { wmoArenaInit(); }  // whichever mode returns, the WMO arena last (above the others)
    } wmoAfter;
    // the big-block region (bigheap=1) is off - the emulator autopilot ran out of memory in a dungeon with it
    // on (small blocks cut to 64-128 KB pieces, the region full). Memory diet: bigheap=2 by default - the
    // SMALL objects get an arena of their own instead (the big blocks keep all of newlib).
    const int mode = std::clamp(configGetInt("bigheap", 2), 0, 2);
    const bool want = mode == 1;
    g_probeBlockCapBytes = static_cast<uint32_t>(std::clamp(configGetInt("heapblockkb", 256), 16, 4096)) * 1024u;
    g_guardBytes = static_cast<uint32_t>(std::clamp(configGetInt("heapguardkb", 4096), 0, 16384)) * 1024u;
    const uint32_t minKb = static_cast<uint32_t>(std::clamp(configGetInt("bigheapmin", 256), 16, 8192));
    const uint32_t floorKb = static_cast<uint32_t>(std::clamp(configGetInt("bigheapfloor", 1024), 0, 16384));
    uint32_t regionKb = static_cast<uint32_t>(std::clamp(configGetInt("bigheapkb", 20480), 0, 65536));
    g_floorBytes = floorKb * 1024u;
    g_ballastBytes = ballastKb * 1024u;
    const uint32_t largestKb = realLargestKb();
    g_heapTotalBytes = static_cast<uint32_t>(mallinfo().uordblks) + largestKb * 1024u;
    // At least 8 MB stay with newlib whatever the key says (boot, UI, sessions, every small object of the world).
    const uint32_t capKb = largestKb > 8192u + 1024u ? largestKb - 8192u - 64u : 0u;
    if (regionKb > capKb) regionKb = capKb;
    if (mode == 2) {
        smallArenaInit(largestKb);
        return;
    }
    if (!want || regionKb < 1024u) {
        PSP_LOGI(tag::Mem, "bigheap_init on=0 want=%d region_kb=%u heap_largest_kb=%u", want ? 1 : 0, U(regionKb),
                 U(largestKb));
        return;
    }
    if (sceKernelCreateLwMutex(&g_mutex, "wowee_bigheap", 0, 0, nullptr) < 0) {
        PSP_LOGE(tag::Mem, "bigheap_init on=0 lwmutex_failed");
        return;
    }
    g_mutexOn = true;
    const size_t bytes = static_cast<size_t>(regionKb) * 1024u;
    void* region = __real_memalign(64, bytes);
    if (!region) {
        PSP_LOGE(tag::Mem, "bigheap_init on=0 region_failed region_kb=%u heap_largest_kb=%u", U(regionKb), U(largestKb));
        sceKernelDeleteLwMutex(&g_mutex);
        g_mutexOn = false;
        return;
    }
    static_cast<volatile char*>(region)[0] = 0;
    g_regionChunkBytes = static_cast<uint32_t>(__real_malloc_usable_size(region)) + 8u;  // + newlib's chunk header
    SmallHeapOps ops{&smallMalloc, &smallFree, &smallRealloc, &smallCalloc, &smallMemalign, &smallUsable};
    BigHeapLockOps lk;
    lk.lock = &lockMutex;
    lk.unlock = &unlockMutex;
    g_router.setOps(ops, lk);
    if (!g_router.enable(region, bytes, static_cast<size_t>(minKb) * 1024u)) {
        __real_free(region);
        g_regionChunkBytes = 0;
        PSP_LOGE(tag::Mem, "bigheap_init on=0 core_init_failed");
        return;
    }
    PSP_LOGI(tag::Mem, "bigheap_init on=1 region_kb=%u min_kb=%u floor_kb=%u base=%p max_kb=%u->%u", U(regionKb),
             U(minKb), U(floorKb), region, U(largestKb), U(realLargestKb()));
    PSP_LOGI(tag::Mem, "bigheap_small heap_kb=%u small_kb=%u ballast_kb=%u free_kb=%u", U(g_heapTotalBytes / 1024u),
             U((g_heapTotalBytes - g_regionChunkBytes) / 1024u), U(ballastKb), U(heapSmallFreeKb()));
}

bool bigheapOn() { return g_router.on(); }

HeapPools heapPools(bool probeSmallLargest) {
    HeapPools h;
    h.on = g_router.on();
    const uint32_t raw = static_cast<uint32_t>(mallinfo().uordblks);
    const uint32_t small = raw > g_regionChunkBytes ? raw - g_regionChunkBytes : 0u;
    h.smallUsedKb = small / 1024u;
    if (probeSmallLargest) h.smallLargestKb = realLargestKb();
    if (h.on) {
        const BigHeapCore::Stats s = g_router.stats();
        const BigHeapRouter::Counters c = g_router.counters();
        h.bigKb = g_router.regionBytes() / 1024u;
        h.bigUsedKb = s.usedBytes / 1024u;
        h.bigPeakKb = s.peakUsedBytes / 1024u;
        h.bigBlocks = s.usedBlocks;
        h.bigFreeBlocks = s.freeBlocks;
        h.badFrees = s.badFrees;
        h.bigLargestKb = g_router.bigLargest() / 1024u;
        h.fallbacks = c.fallbacks;
        h.spills = c.spills;
        h.usedKb = (small + s.usedBytes) / 1024u;
    } else {
        h.usedKb = h.smallUsedKb;
    }
    return h;
}

uint32_t heapUsedBytes() {
    const uint32_t raw = static_cast<uint32_t>(mallinfo().uordblks);
    if (g_smallOn) {  // the arena's block is not "in use", what lives in it is
        const uint32_t nl = raw > g_regionChunkBytes ? raw - g_regionChunkBytes : 0u;
        lockMutex(nullptr);
        const uint32_t in = g_small.stats().usedBytes;
        unlockMutex(nullptr);
        return nl + in;
    }
    if (!g_router.on()) return raw;
    const uint32_t small = raw > g_regionChunkBytes ? raw - g_regionChunkBytes : 0u;
    return small + g_router.stats().usedBytes;
}
uint32_t heapUsedKb() { return heapUsedBytes() / 1024u; }
uint32_t heapRawUsedKb() { return static_cast<uint32_t>(mallinfo().uordblks / 1024); }
uint32_t heapSmallLargestKb() { return realLargestKb(); }

uint32_t heapLargestKb() {
    const uint32_t small = realLargestKb();
    const uint32_t big = g_router.on() ? g_router.bigLargest() / 1024u : 0u;
    return std::max(small, big);
}

uint32_t heapSmallFreeKb() {
    if (!g_router.on()) return 0;
    const uint32_t used = static_cast<uint32_t>(mallinfo().uordblks);  // the region's block and the ballast included
    return g_heapTotalBytes > used ? (g_heapTotalBytes - used) / 1024u : 0u;
}

bool heapSmallRoomKb(uint32_t kb) { return !g_router.on() || heapSmallFreeKb() >= kb; }

// total free bytes - newlib's size at boot (g_heapTotalBytes, measured by bigheapInit with either
// key) minus what it holds now, plus the region's free when it is on.
uint32_t heapFreeBytes() {
    const uint32_t used = static_cast<uint32_t>(mallinfo().uordblks);  // the region's block and the ballast included
    uint32_t f = g_heapTotalBytes > used ? g_heapTotalBytes - used : 0u;
    if (g_router.on()) {
        const uint32_t region = g_router.regionBytes(), inRegion = g_router.stats().usedBytes;
        f += region > inRegion ? region - inRegion : 0u;
    }
    return f;
}
uint32_t heapFreeKb() { return heapFreeBytes() / 1024u; }

bool heapGivesBytes(uint32_t bytes) {
    if (!bytes) return true;
    // headroom, not one block of the whole size (that was "the old probe": on a
    // fragmented heap it said no with megabytes free - NPC models and the far city waited forever, hardware hw5).
    if (!g_router.on()) return heapFreeBytes() >= bytes && realGives(std::min<uint32_t>(bytes, g_probeBlockCapBytes));
    return g_router.gives(bytes, g_floorBytes, static_cast<size_t>(heapSmallFreeKb()) * 1024u, kBlockCapBytes);
}
bool heapGivesKb(uint32_t kb) { return heapGivesBytes(kb * 1024u); }

// ---- The big WMOs' arena ---------------------------------------------------------------------------------
void* wmoArenaAlloc(size_t n, size_t align) {
    if (!g_wmoOn) return nullptr;
    lockMutex(nullptr);
    void* p = g_wmo.allocLow(n, align < 16 ? 16 : align);  // the low end (doodads / NPCs: the high end)
    if (!p) ++g_wmoFallbacks;
    unlockMutex(nullptr);
    return p;
}
bool wmoArenaOn() { return g_wmoOn; }
void wmoArenaSetBigOpen(uint32_t n) { g_wmoBigOpen = n; }
void* worldArenaAlloc(size_t n, size_t align) {
    if (!g_wmoOn) return nullptr;
    lockMutex(nullptr);
    void* p = nullptr;
    // While a big WMO is open it keeps worldarenawmokb of its arena (its chunks come and go as the camera moves).
    if (!g_wmoBigOpen || g_wmo.freeBytes() >= n + g_worldKeepBytes) p = g_wmo.allocHigh(n, align < 16 ? 16 : align);
    unlockMutex(nullptr);
    return p;
}
WorldArenaInfo worldArenaInfo() {
    WorldArenaInfo w;
    if (!g_wmoOn) return w;
    lockMutex(nullptr);
    const BigHeapCore::Stats s = g_wmo.stats();
    w.usedKb = s.usedBytes / 1024u;
    w.usedBlocks = s.usedBlocks;
    w.freeBlocks = s.freeBlocks;
    w.largestKb = g_wmo.largestFree() / 1024u;
    w.sizeKb = g_wmo.size() / 1024u;
    unlockMutex(nullptr);
    return w;
}
uint32_t worldArenaReset(char* list, size_t listBytes) {
    if (list && listBytes) list[0] = 0;
    if (!g_wmoOn) return 0;
    uint32_t offs[8], reqs[8], n = 0;  // collected under the lock, printed after it (snprintf may allocate)
    lockMutex(nullptr);
    const uint32_t live = g_wmo.stats().usedBlocks;
    if (!live) {
        g_wmo.resetIfEmpty();
    } else {
        g_wmo.forEachUsed([&](uint32_t off, uint32_t req) {
            if (n >= 8) return;
            offs[n] = off;
            reqs[n] = req;
            ++n;
        });
    }
    unlockMutex(nullptr);
    size_t at = 0;
    for (uint32_t i = 0; i < n && list && at + 24 < listBytes; ++i) {
        const int w = std::snprintf(list + at, listBytes - at, "+%u:%u", U(offs[i] / 1024u), U(reqs[i]));
        if (w > 0) at += static_cast<size_t>(w);
    }
    return live;
}
bool wmoArenaHasKb(uint32_t kb) {
    if (!g_wmoOn) return false;
    lockMutex(nullptr);
    const bool ok = g_wmo.canAlloc(static_cast<size_t>(kb) * 1024u, 64);
    unlockMutex(nullptr);
    return ok;
}
void wmoArenaLog() {
    if (!g_wmoOn) return;
    lockMutex(nullptr);
    const BigHeapCore::Stats s = g_wmo.stats();
    const uint32_t largest = g_wmo.largestFree();
    unlockMutex(nullptr);
    PSP_LOGI(tag::Mem, "wmo_arena used_kb=%u peak_kb=%u of=%u largest_kb=%u blocks=%u holes=%u fb=%u bad=%u",
             U(s.usedBytes / 1024u), U(s.peakUsedBytes / 1024u), U(g_wmo.size() / 1024u), U(largest / 1024u),
             U(s.usedBlocks), U(s.freeBlocks), U(g_wmoFallbacks), U(s.badFrees));
}

void heapBigStatsLog() {
    if (!g_bigStats) return;
    char b[160];
    int n = std::snprintf(b, sizeof(b), "heap_big_allocs allocs=%u frees=%u", U(g_bigAllocs), U(g_bigFrees));
    static const char* const kName[kBigClasses] = {"64k", "128k", "256k", "512k", "1m", "2m"};
    for (int c = 0; c < kBigClasses && n > 0 && n < static_cast<int>(sizeof(b)); ++c)
        n += std::snprintf(b + n, sizeof(b) - static_cast<size_t>(n), " %s=%u/%u", kName[c], U(g_bigN[c]), U(g_bigKb[c]));
    PSP_LOGI(tag::Mem, "%s", b);
}

bool heapGuardOk() {
    if (!g_guardBytes) return true;
    const uint32_t largest = g_router.on() ? std::max(g_router.bigLargest(), realLargestKb() * 1024u)
                                           : realLargestKb() * 1024u;
    if (largest < g_guardBytes || largest >= g_guardBytes + 1024u * 1024u) return true;  // lost / room above it
    ++g_guardRefusals;
    return false;
}
uint32_t heapGuardRefusals() { return g_guardRefusals; }
uint32_t heapGuardKb() { return g_guardBytes / 1024u; }

bool heapRoomFor(uint32_t blockBytes, uint32_t marginKb) {
    const uint64_t want = static_cast<uint64_t>(blockBytes) + static_cast<uint64_t>(marginKb) * 1024u;
    if (heapFreeBytes() < want) return false;
    if (!blockBytes) return true;
    if (!g_router.on()) return realGives(blockBytes);
    return g_router.givesBlock(blockBytes);
}

bool heapGivesBlockKb(uint32_t kb) {
    if (!kb) return true;
    if (!g_router.on()) return realGives(static_cast<size_t>(kb) * 1024u);
    return g_router.givesBlock(static_cast<size_t>(kb) * 1024u);
}

void* heapProbeAlloc(size_t bytes) {
    if (!g_router.on()) return __real_malloc(bytes);
    return g_router.probeAlloc(bytes);
}

// the small-object arena in one line (bigheap=2).
static void smallArenaLog(const char* what) {
    lockMutex(nullptr);
    const SmallArena::Stats s = g_small.stats();
    unlockMutex(nullptr);
    PSP_LOGI(tag::Mem, "small_arena at=%s used_kb=%u peak_kb=%u pages=%u/%u peak_pages=%u fb=%u fails=%u bad=%u "
             "free_kb=%u largest_kb=%u", what, U(s.usedBytes / 1024u), U(s.peakUsedBytes / 1024u), U(s.pagesUsed),
             U(s.pages), U(s.peakPagesUsed), U(g_smallFallbacks), U(s.fails), U(s.badFrees), U(heapFreeKb()),
             U(realLargestKb()));
}

void bigheapLogPools() {
    if (g_smallOn) {
        smallArenaLog("pools");
        return;
    }
    if (!g_router.on()) return;
    const HeapPools h = heapPools(true);
    const uint32_t ballastKb = g_ballastBytes / 1024u;
    PSP_LOGI(tag::Mem, "bigheap big_used_kb=%u big_max_kb=%u small_used_kb=%u small_max_kb=%u free_kb=%u",
             U(h.bigUsedKb), U(h.bigLargestKb), U(h.smallUsedKb > ballastKb ? h.smallUsedKb - ballastKb : 0u),
             U(h.smallLargestKb), U(heapSmallFreeKb()));
}

void bigheapLogPeak() {
    if (g_smallOn) {
        smallArenaLog("peak");
        return;
    }
    if (!g_router.on()) return;
    const HeapPools h = heapPools(false);
    PSP_LOGI(tag::Mem, "bigheap_peak big_kb=%u big_peak_kb=%u big_used_kb=%u holes=%u fb=%u sp=%u bad=%u", U(h.bigKb),
             U(h.bigPeakKb), U(h.bigUsedKb), U(h.bigFreeBlocks), U(h.fallbacks), U(h.spills), U(h.badFrees));
}

void bigheapPoll() {
    if (g_smallOn) {  // the arena's first fallback / bad free, once each
        if (!g_loggedSmallFull && g_smallFallbacks) {
            g_loggedSmallFull = true;
            PSP_LOGW(tag::Mem, "small_arena_full first_bytes=%u fb=%u (small requests to newlib: smallarenakb)",
                     U(g_smallFirstFallback), U(g_smallFallbacks));
        }
        if (!g_loggedSmallBad && g_small.stats().badFrees) {
            g_loggedSmallBad = true;
            PSP_LOGE(tag::Mem, "bigheap_badfree n=%u (small arena: a double free or no block)",
                     U(g_small.stats().badFrees));
        }
        return;
    }
    if (!g_router.on() || (g_loggedFull && g_loggedSpill && g_loggedBad)) return;
    const BigHeapRouter::Counters c = g_router.counters();
    const BigHeapCore::Stats s = g_router.stats();
    if (!g_loggedFull && c.fallbacks) {
        g_loggedFull = true;
        PSP_LOGW(tag::Mem, "bigheap_full first_kb=%u fb=%u big_used_kb=%u big_max_kb=%u holes=%u",
                 U(c.firstFallbackBytes / 1024u), U(c.fallbacks), U(s.usedBytes / 1024u),
                 U(g_router.bigLargest() / 1024u), U(s.freeBlocks));
    }
    if (!g_loggedSpill && c.spills) {
        g_loggedSpill = true;
        PSP_LOGW(tag::Mem, "bigheap_spill first_bytes=%u sp=%u big_used_kb=%u small_used_kb=%u", U(c.firstSpillBytes),
                 U(c.spills), U(s.usedBytes / 1024u), U(heapPools(false).smallUsedKb));
    }
    if (!g_loggedBad && s.badFrees) {
        g_loggedBad = true;
        PSP_LOGE(tag::Mem, "bigheap_badfree n=%u (free/realloc of a region pointer that is no live block)",
                 U(s.badFrees));
    }
}

} // namespace wowee::platform::psp

// ---- the link-time wrappers (psp/CMakeLists.txt -Wl,--wrap=...) ----------------------------------------------------
namespace bh = wowee::platform::psp;

// bigheap=2 - requests of at most smallarenamax bytes (alignment <= 16) from the
// small-object arena, the rest (and a small request the arena cannot hold: counted) from newlib; free / realloc /
// usable by address. Never fails where newlib alone would have given the memory.
namespace {
// newlib blocks >= 64 KB counted by size class while alive (heapbigstats; the usable size on both
// sides, so a block leaves the class it entered).
inline void bigNote(void* p, bool alloc) {
    if (!bh::g_bigStats || !p) return;
    const size_t u = __real_malloc_usable_size(p);
    if (u < 64u * 1024u) return;
    const int c = bh::bigClass(u);
    const uint32_t kb = static_cast<uint32_t>(u / 1024u);
    if (alloc) {
        ++bh::g_bigAllocs;
        ++bh::g_bigN[c];
        bh::g_bigKb[c] += kb;
    } else {
        ++bh::g_bigFrees;
        if (bh::g_bigN[c]) --bh::g_bigN[c];
        bh::g_bigKb[c] -= bh::g_bigKb[c] >= kb ? kb : bh::g_bigKb[c];
    }
}
inline bool inWmo(const void* p) { return bh::g_wmoOn && p && bh::g_wmo.contains(p); }
void wmoRelease(void* p) {
    bh::lockMutex(nullptr);
    bh::g_wmo.free(p);
    bh::unlockMutex(nullptr);
}
void* smallAlloc(size_t n) {
    bh::lockMutex(nullptr);
    void* p = bh::g_small.alloc(n);
    if (!p && bh::g_smallFallbacks++ == 0) bh::g_smallFirstFallback = static_cast<uint32_t>(n);
    bh::unlockMutex(nullptr);
    return p;
}
void smallRelease(void* p) {
    bh::lockMutex(nullptr);
    bh::g_small.free(p);
    bh::unlockMutex(nullptr);
}
size_t smallUsableOf(void* p) {
    bh::lockMutex(nullptr);
    const size_t u = bh::g_small.usable(p);
    bh::unlockMutex(nullptr);
    return u;
}
inline bool smallWants(size_t n) { return n <= bh::g_smallMax; }
}  // namespace

extern "C" void* __wrap_malloc(size_t n) {
    if (bh::g_smallOn) {
        if (smallWants(n))
            if (void* p = smallAlloc(n)) return p;
        void* p = __real_malloc(n);
        if (n >= 64u * 1024u) bigNote(p, true);
        return p;
    }
    if (!bh::g_router.on()) return __real_malloc(n);
    return bh::g_router.malloc(n);
}

extern "C" void __wrap_free(void* p) {
    if (inWmo(p)) {  // the big WMOs' arena (any mode)
        wmoRelease(p);
        return;
    }
    if (bh::g_smallOn) {
        if (p && bh::g_small.contains(p)) {
            smallRelease(p);
        } else {
            bigNote(p, false);
            __real_free(p);
        }
        return;
    }
    if (!bh::g_router.on()) {
        __real_free(p);
        return;
    }
    bh::g_router.free(p);
}

extern "C" void* __wrap_realloc(void* p, size_t n) {
    if (inWmo(p)) {  // in place when it fits, else moved out (the arena's blocks are the WMOs' only)
        bh::lockMutex(nullptr);
        const bool ok = n && bh::g_wmo.resizeInPlace(p, n);
        const size_t have = bh::g_wmo.usable(p);
        bh::unlockMutex(nullptr);
        if (ok) return p;
        if (n == 0) {
            wmoRelease(p);
            return __wrap_malloc(0);
        }
        void* q = __wrap_malloc(n);
        if (!q) return nullptr;
        std::memcpy(q, p, have < n ? have : n);
        wmoRelease(p);
        return q;
    }
    if (bh::g_smallOn) {
        if (!p) return __wrap_malloc(n);
        if (!bh::g_small.contains(p)) {  // a newlib block stays in newlib
            bigNote(p, false);
            void* q = __real_realloc(p, n);
            bigNote(q ? q : p, true);
            return q;
        }
        const size_t have = smallUsableOf(p);
        if (n && n <= have) return p;  // fits its class
        if (n == 0) {
            smallRelease(p);
            return __wrap_malloc(0);
        }
        void* q = __wrap_malloc(n);
        if (!q) return nullptr;  // p stays valid
        std::memcpy(q, p, have < n ? have : n);
        smallRelease(p);
        return q;
    }
    if (!bh::g_router.on()) return __real_realloc(p, n);
    return bh::g_router.realloc(p, n);
}

extern "C" void* __wrap_calloc(size_t a, size_t b) {
    if (bh::g_smallOn) {
        if (b && a > static_cast<size_t>(-1) / b) return nullptr;
        const size_t n = a * b;
        if (smallWants(n))
            if (void* p = smallAlloc(n)) {
                std::memset(p, 0, n);
                return p;
            }
        return __real_calloc(a, b);
    }
    if (!bh::g_router.on()) return __real_calloc(a, b);
    return bh::g_router.calloc(a, b);
}

extern "C" void* __wrap_memalign(size_t align, size_t n) {
    if (bh::g_smallOn) {
        if (align <= 16 && smallWants(n))  // the arena's blocks are 16-aligned
            if (void* p = smallAlloc(n)) return p;
        void* p = __real_memalign(align, n);
        if (n >= 64u * 1024u) bigNote(p, true);
        return p;
    }
    if (!bh::g_router.on()) return __real_memalign(align, n);
    return bh::g_router.memalign(align, n);
}

extern "C" void* __wrap_aligned_alloc(size_t align, size_t n) {
    if (bh::g_smallOn) return __wrap_memalign(align, n);
    if (!bh::g_router.on()) return __real_aligned_alloc(align, n);
    return bh::g_router.memalign(align, n);
}

extern "C" size_t __wrap_malloc_usable_size(void* p) {
    if (inWmo(p)) {
        bh::lockMutex(nullptr);
        const size_t u = bh::g_wmo.usable(p);
        bh::unlockMutex(nullptr);
        return u;
    }
    if (bh::g_smallOn) {
        if (p && bh::g_small.contains(p)) return smallUsableOf(p);
        return __real_malloc_usable_size(p);
    }
    if (!bh::g_router.on()) return __real_malloc_usable_size(p);
    return bh::g_router.usable(p);
}
