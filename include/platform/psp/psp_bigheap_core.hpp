// PSP (docs/PSP_PORT_ARCHITECTURE.md): the big-block heap region - the allocator core and the router.
// Pure C++ (no PSP headers): the host test (tools/psp/host_tests/bigheap_host_test.sh) builds exactly this code.
//
// PSP: why. newlib's heap is one arena for everything. After a teleport between continents it held
// 17 MB of 41 in use and still no free 1.6 MB block: long-lived small objects sat between the holes
// the freed tile terrain / WMO groups / pak reads / textures left. BigHeapCore is a second arena for requests of at
// least `threshold` bytes only: its holes are big by construction and coalesce on free, so a tile's 1.6 MB blob finds
// room as long as the region has it free in total (minus at most the scatter of other big blocks).
//
// BigHeapCore: one region [base, base + size), blocks with boundary tags (16-byte header: size|used, size of the
// physically previous block, requested bytes, magic), immediate coalescing with both neighbours on free, best fit over
// one free list (ties: the lowest address - address-ordered best fit, the classic low-fragmentation policy),
// any power-of-two alignment (GE textures 16, some code 64). Offsets are 32-bit (the region is < 4 GB) so the same
// code runs on the 64-bit host. Not locked: BigHeapRouter locks around it.
//
// BigHeapRouter: malloc/free/realloc/calloc/memalign semantics over two pools - "small" (the old heap: newlib through
// the ops table) and "big" (BigHeapCore). Requests >= threshold go big; free/realloc/usable route by address range.
// Never fails where the old heap alone would have given the memory, as far as both pools allow:
//   big request, region has no fitting block   -> small heap (counted: fallbacks, first size kept)
//   small request, small heap empty            -> region (counted: spills, first size kept)
//   realloc across the threshold moves the block between the pools (a big block stays in place when it can).
// Disabled (bigheap=0 or no region), every call goes straight to the ops: the old behaviour exactly.
// No allocation inside, no logging inside (the PSP glue logs the counters from the main thread).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wowee::platform::psp {

class BigHeapCore {
public:
    static constexpr uint32_t kHdr = 16;       // header bytes; payloads stay 16-aligned
    static constexpr uint32_t kGran = 16;      // block sizes are multiples of this
    static constexpr uint32_t kMinBlock = 32;  // header + the two free-list links, rounded
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    static constexpr uint32_t kTailKeep = 256;  // allocHigh keeps a smaller alignment tail in the block
    static constexpr uint32_t kMagicUsed = 0xB16B10C5u;
    static constexpr uint32_t kMagicFree = 0xF4EEB10Cu;

    struct Stats {
        uint32_t usedBytes = 0;   // blocks in use incl. headers
        uint32_t reqBytes = 0;    // what callers asked for
        uint32_t usedBlocks = 0;
        uint32_t freeBlocks = 0;
        uint32_t peakUsedBytes = 0;
        uint32_t allocs = 0;
        uint32_t frees = 0;
        uint32_t fails = 0;       // alloc found no fitting block
        uint32_t badFrees = 0;    // free/realloc of a pointer that is not a live block (ignored)
    };

    bool init(void* mem, size_t bytes) {
        base_ = nullptr;
        size_ = 0;
        freeHead_ = kNone;
        stats_ = Stats{};
        if (!mem) return false;
        const uintptr_t a = reinterpret_cast<uintptr_t>(mem);
        const uintptr_t b = (a + (kGran - 1)) & ~static_cast<uintptr_t>(kGran - 1);
        const size_t lost = static_cast<size_t>(b - a);
        if (bytes < lost + kMinBlock) return false;
        size_t sz = (bytes - lost) & ~static_cast<size_t>(kGran - 1);
        if (sz > 0xFFFFFF00u) sz = 0xFFFFFF00u;
        base_ = reinterpret_cast<uint8_t*>(b);
        size_ = static_cast<uint32_t>(sz);
        hdr(0)->prevSize = 0;
        setSize(0, size_, false);
        hdr(0)->req = 0;
        link(0);
        return true;
    }

    bool ready() const { return base_ != nullptr; }
    const uint8_t* base() const { return base_; }
    uint32_t size() const { return size_; }
    const Stats& stats() const { return stats_; }

    bool contains(const void* p) const {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        return base_ && q >= base_ && q < base_ + size_;
    }

    // Bytes a block of `n` payload bytes takes (header included), 0 when it can never fit.
    uint32_t blockNeed(size_t n) const {
        if (n > size_) return 0;
        uint32_t need = static_cast<uint32_t>(n) + kHdr;
        need = (need + (kGran - 1)) & ~(kGran - 1);
        return need < kMinBlock ? kMinBlock : need;
    }

    void* alloc(size_t n, size_t align = kGran) { return allocAt(n, align, false); }
    // The lowest free block that holds the request (address-ordered first fit) - the world arena's
    // low end (the big WMOs), the counterpart of allocHigh.
    void* allocLow(size_t n, size_t align = kGran) { return allocAt(n, align, true); }
    void* allocAt(size_t n, size_t align, bool lowest) {
        if (!base_) return nullptr;
        if (align == 0 || (align & (align - 1)) != 0 || align > size_) {  // memalign's contract: a power of two
            ++stats_.fails;
            return nullptr;
        }
        if (align < kGran) align = kGran;
        const uint32_t need = blockNeed(n);
        uint32_t o = kNone, lead = 0;
        if (!need || !find(need, static_cast<uint32_t>(align), o, lead, lowest)) {
            ++stats_.fails;
            return nullptr;
        }
        uint32_t s = blockSize(o);
        unlink(o);
        if (lead) {  // the gap before the aligned payload stays a free block of its own (>= kMinBlock)
            setSize(o, lead, false);
            hdr(o)->req = 0;
            link(o);
            o += lead;
            hdr(o)->prevSize = lead;
            s -= lead;
        }
        const uint32_t rem = s - need;
        if (rem >= kMinBlock) {
            setSize(o, need, true);
            const uint32_t t = o + need;
            setSize(t, rem, false);  // the block after it was used (free neighbours are always merged)
            hdr(t)->req = 0;
            link(t);
        } else {
            setSize(o, s, true);
        }
        Hdr* h = hdr(o);
        h->req = static_cast<uint32_t>(n);
        h->magic = kMagicUsed;
        stats_.usedBytes += blockSize(o);
        stats_.reqBytes += h->req;
        ++stats_.usedBlocks;
        ++stats_.allocs;
        if (stats_.usedBytes > stats_.peakUsedBytes) stats_.peakUsedBytes = stats_.usedBytes;
        return base_ + o + kHdr;
    }

    // Memory diet (docs/PSP_PORT_ARCHITECTURE.md): the other end - the highest free block that holds
    // the request, carved from its END (what is left of it stays free below). The world arena gives the big WMOs the
    // low end (alloc: address-ordered best fit) and the doodads / NPC models this one: the two kinds of churn (a city's
    // chunks as the camera moves, the tiles' and NPCs' blobs) do not interleave (emulator runs, a capital city:
    // 1.8 MB used of the arena in 501 holes, largest 855 KB).
    void* allocHigh(size_t n, size_t align = kGran) {
        if (!base_) return nullptr;
        if (align == 0 || (align & (align - 1)) != 0 || align > size_) {
            ++stats_.fails;
            return nullptr;
        }
        if (align < kGran) align = kGran;
        const uint32_t need = blockNeed(n);
        uint32_t best = kNone, bestU = 0;
        for (uint32_t f = freeHead_; need && f != kNone; f = links(f)[0]) {
            const uint32_t s = blockSize(f);
            if (s < need || (best != kNone && f < best)) continue;
            uint32_t u = 0;
            if (!highPlace(f, s, need, static_cast<uint32_t>(align), u)) continue;
            best = f;
            bestU = u;
        }
        if (best == kNone) {
            ++stats_.fails;
            return nullptr;
        }
        const uint32_t o = best, s = blockSize(o), u = bestU, end = o + s;
        const uint32_t lead = u - o, usedSz = end - u, tail = usedSz - need;
        unlink(o);
        if (lead) {  // the rest of the free block stays free below the new one
            setSize(o, lead, false);
            hdr(o)->req = 0;
            link(o);
        }
        // PSP (an emulator run: wmo_arena blocks=662 holes=662 - every high block left its 32..48-byte
        // alignment tail as a free block of its own): a tail below kTailKeep stays in the block (<= 63 bytes lost).
        if (tail >= kTailKeep) {  // alignment left a free gap at the end
            setSize(u, need, true);
            const uint32_t t = u + need;
            setSize(t, tail, false);
            hdr(t)->req = 0;
            link(t);
        } else {
            setSize(u, usedSz, true);
        }
        Hdr* h = hdr(u);
        h->req = static_cast<uint32_t>(n);
        h->magic = kMagicUsed;
        stats_.usedBytes += blockSize(u);
        stats_.reqBytes += h->req;
        ++stats_.usedBlocks;
        ++stats_.allocs;
        if (stats_.usedBytes > stats_.peakUsedBytes) stats_.peakUsedBytes = stats_.usedBytes;
        return base_ + u + kHdr;
    }

    // false: not a live block of this region (nothing changed).
    bool free(void* p) {
        uint32_t o;
        if (!liveBlock(p, o)) {
            ++stats_.badFrees;
            return false;
        }
        Hdr* h = hdr(o);
        uint32_t s = blockSize(o);
        stats_.usedBytes -= s;
        stats_.reqBytes -= h->req;
        --stats_.usedBlocks;
        ++stats_.frees;
        // The header stops being a live block now: a second free of the same pointer is caught (badFrees).
        h->magic = kMagicFree;
        h->sizeFlags &= ~1u;
        h->req = 0;
        const uint32_t nx = o + s;
        if (nx < size_ && !isUsed(nx)) {
            unlink(nx);
            s += blockSize(nx);
        }
        if (o != 0) {
            const uint32_t pv = o - h->prevSize;
            if (!isUsed(pv)) {
                unlink(pv);
                s += blockSize(pv);
                o = pv;
            }
        }
        setSize(o, s, false);
        hdr(o)->req = 0;
        link(o);
        return true;
    }

    // Payload bytes of a live block (0: not one).
    size_t usable(const void* p) const {
        uint32_t o;
        if (!liveBlock(p, o)) return 0;
        return blockSize(o) - kHdr;
    }

    // Shrinks (always) or grows into the free block right after it (when that is enough). false: unchanged.
    bool resizeInPlace(void* p, size_t n) {
        uint32_t o;
        if (!liveBlock(p, o)) {
            ++stats_.badFrees;
            return false;
        }
        const uint32_t need = blockNeed(n);
        if (!need) return false;
        Hdr* h = hdr(o);
        const uint32_t s = blockSize(o);
        const uint32_t nx = o + s;
        if (need <= s) {
            uint32_t rem = s - need;
            if (rem >= kMinBlock) {
                setSize(o, need, true);
                const uint32_t t = o + need;
                if (nx < size_ && !isUsed(nx)) {
                    unlink(nx);
                    rem += blockSize(nx);
                }
                setSize(t, rem, false);
                hdr(t)->req = 0;
                link(t);
                stats_.usedBytes -= s - need;
            }
        } else {
            if (nx >= size_ || isUsed(nx) || s + blockSize(nx) < need) return false;
            unlink(nx);
            const uint32_t total = s + blockSize(nx);
            const uint32_t rem = total - need;
            if (rem >= kMinBlock) {
                setSize(o, need, true);
                const uint32_t t = o + need;
                setSize(t, rem, false);
                hdr(t)->req = 0;
                link(t);
                stats_.usedBytes += need - s;
            } else {
                setSize(o, total, true);
                stats_.usedBytes += total - s;
            }
            if (stats_.usedBytes > stats_.peakUsedBytes) stats_.peakUsedBytes = stats_.usedBytes;
        }
        stats_.reqBytes = stats_.reqBytes - h->req + static_cast<uint32_t>(n);
        h->req = static_cast<uint32_t>(n);
        return true;
    }

    // Whether alloc(n, align) would succeed now (nothing changes).
    bool canAlloc(size_t n, size_t align = kGran) const {
        if (!base_) return false;
        if (align == 0 || (align & (align - 1)) != 0 || align > size_) return false;
        if (align < kGran) align = kGran;
        const uint32_t need = blockNeed(n);
        uint32_t o, lead;
        return need && find(need, static_cast<uint32_t>(align), o, lead);
    }

    // Largest payload one malloc (16-byte alignment) can get now.
    uint32_t largestFree() const {
        uint32_t best = 0;
        for (uint32_t o = freeHead_; o != kNone; o = links(o)[0]) {
            const uint32_t s = blockSize(o);
            if (s > best) best = s;
        }
        return best > kHdr ? best - kHdr : 0;
    }
    uint32_t freeBytes() const { return size_ - stats_.usedBytes; }

    // world_flush: an EMPTY region back to one free block (counters kept). false when a block is
    // still in use - a leak the caller reports (forEachUsed); a live block is never dropped.
    bool resetIfEmpty() {
        if (!base_ || stats_.usedBlocks) return false;
        const Stats keep = stats_;
        uint8_t* const b = base_;
        const uint32_t sz = size_;
        init(b, sz);
        stats_.peakUsedBytes = keep.peakUsedBytes;
        stats_.allocs = keep.allocs;
        stats_.frees = keep.frees;
        stats_.fails = keep.fails;
        stats_.badFrees = keep.badFrees;
        return true;
    }
    // Every block in use, by address: f(offset, requested bytes).
    template <class F>
    void forEachUsed(F&& f) const {
        if (!base_) return;
        for (uint32_t o = 0; o < size_;) {
            const uint32_t sz = blockSize(o);
            if (sz < kMinBlock) return;  // a broken chain (check() says so): stop
            if (isUsed(o)) f(o, hdr(o)->req);
            o += sz;
        }
    }

    // Walks every block: sizes add up, boundary tags agree, no two free neighbours, the free list holds exactly the
    // free blocks, the counters match. For the host test (and a debug check on the PSP).
    bool check() const {
        if (!base_) return true;
        uint32_t o = 0, prev = 0, used = 0, usedBlocks = 0, freeBlocks = 0;
        bool prevFree = false;
        while (o < size_) {
            const uint32_t s = blockSize(o);
            if (s < kMinBlock || (s & (kGran - 1)) || o + s > size_) return false;
            if (hdr(o)->prevSize != prev) return false;
            const bool u = isUsed(o);
            if (u) {
                if (hdr(o)->magic != kMagicUsed) return false;
                used += s;
                ++usedBlocks;
                prevFree = false;
            } else {
                if (prevFree || hdr(o)->magic != kMagicFree) return false;
                ++freeBlocks;
                prevFree = true;
            }
            prev = s;
            o += s;
        }
        if (o != size_) return false;
        uint32_t listed = 0, back = kNone;
        for (uint32_t f = freeHead_; f != kNone; f = links(f)[0]) {
            if (f >= size_ || isUsed(f) || links(f)[1] != back || ++listed > freeBlocks) return false;
            back = f;
        }
        return listed == freeBlocks && used == stats_.usedBytes && usedBlocks == stats_.usedBlocks &&
               freeBlocks == stats_.freeBlocks;
    }

private:
    struct Hdr {
        uint32_t sizeFlags;  // block bytes (multiple of 16) | 1 when in use
        uint32_t prevSize;   // bytes of the physically previous block (0: the first block)
        uint32_t req;        // requested bytes (in use)
        uint32_t magic;      // kMagicUsed / kMagicFree
    };
    static_assert(sizeof(Hdr) == kHdr, "header is 16 bytes");

    Hdr* hdr(uint32_t o) const { return reinterpret_cast<Hdr*>(base_ + o); }
    uint32_t* links(uint32_t o) const { return reinterpret_cast<uint32_t*>(base_ + o + kHdr); }  // [0] next, [1] prev
    uint32_t blockSize(uint32_t o) const { return hdr(o)->sizeFlags & ~(kGran - 1); }
    bool isUsed(uint32_t o) const { return (hdr(o)->sizeFlags & 1u) != 0; }

    // Size + flag of block o, and the next block's back tag.
    void setSize(uint32_t o, uint32_t s, bool used) {
        hdr(o)->sizeFlags = s | (used ? 1u : 0u);
        if (o + s < size_) hdr(o + s)->prevSize = s;
    }
    void link(uint32_t o) {
        hdr(o)->magic = kMagicFree;
        uint32_t* l = links(o);
        l[0] = freeHead_;
        l[1] = kNone;
        if (freeHead_ != kNone) links(freeHead_)[1] = o;
        freeHead_ = o;
        ++stats_.freeBlocks;
    }
    void unlink(uint32_t o) {
        uint32_t* l = links(o);
        if (l[1] != kNone) links(l[1])[0] = l[0];
        else freeHead_ = l[0];
        if (l[0] != kNone) links(l[0])[1] = l[1];
        --stats_.freeBlocks;
    }
    // Gap before the first payload address of block o aligned to `align`: 0, or >= kMinBlock (a free block of its own).
    uint32_t leadFor(uint32_t o, uint32_t align) const {
        const uintptr_t pay = reinterpret_cast<uintptr_t>(base_ + o + kHdr);
        const uintptr_t ap = (pay + (align - 1)) & ~static_cast<uintptr_t>(align - 1);
        uint32_t lead = static_cast<uint32_t>(ap - pay);
        while (lead != 0 && lead < kMinBlock) lead += align;
        return lead;
    }
    // allocHigh: the place of a `need`-byte block at the end of free block f (size s), its payload aligned:
    // u = its offset; the gap before it 0 or a free block of its own (>= kMinBlock).
    bool highPlace(uint32_t f, uint32_t s, uint32_t need, uint32_t align, uint32_t& u) const {
        const uintptr_t b = reinterpret_cast<uintptr_t>(base_);
        uintptr_t pay = (b + f + s - need + kHdr) & ~static_cast<uintptr_t>(align - 1);
        for (;;) {
            if (pay < b + f + kHdr) return false;
            const uint32_t uu = static_cast<uint32_t>(pay - b - kHdr);
            const uint32_t lead = uu - f;
            if (lead == 0 || lead >= kMinBlock) {
                u = uu;
                return true;
            }
            if (pay < align) return false;
            pay -= align;
        }
    }
    // Best fit: the smallest free block that holds need (+ the alignment gap); ties - the lowest address.
    // lowest (allocLow): the lowest-address block that holds it instead.
    bool find(uint32_t need, uint32_t align, uint32_t& outO, uint32_t& outLead, bool lowest = false) const {
        uint32_t best = kNone, bestSize = 0, bestLead = 0;
        for (uint32_t o = freeHead_; o != kNone; o = links(o)[0]) {
            const uint32_t s = blockSize(o);
            if (s < need) continue;
            const uint32_t lead = align > kGran ? leadFor(o, align) : 0u;
            if (static_cast<uint64_t>(lead) + need > s) continue;
            if (lowest) {
                if (best == kNone || o < best) {
                    best = o;
                    bestSize = s;
                    bestLead = lead;
                }
                continue;
            }
            if (best == kNone || s < bestSize || (s == bestSize && o < best)) {
                best = o;
                bestSize = s;
                bestLead = lead;
                if (s == need && lead == 0) break;  // exact
            }
        }
        if (best == kNone) return false;
        outO = best;
        outLead = bestLead;
        return true;
    }
    bool liveBlock(const void* p, uint32_t& o) const {
        if (!contains(p)) return false;
        const uint32_t off = static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base_);
        if (off < kHdr || (off & (kGran - 1)) != 0) return false;
        o = off - kHdr;
        const Hdr* h = hdr(o);
        return (h->sizeFlags & 1u) && h->magic == kMagicUsed && blockSize(o) >= kMinBlock && o + blockSize(o) <= size_;
    }

    uint8_t* base_ = nullptr;
    uint32_t size_ = 0;
    uint32_t freeHead_ = kNone;
    Stats stats_;
};

// The old heap, as the router calls it (on the PSP: newlib's __real_* entry points; on the host: a test heap).
struct SmallHeapOps {
    void* (*malloc)(size_t);
    void (*free)(void*);
    void* (*realloc)(void*, size_t);
    void* (*calloc)(size_t, size_t);
    void* (*memalign)(size_t, size_t);
    size_t (*usable)(void*);
};

struct BigHeapLockOps {
    void (*lock)(void*) = nullptr;
    void (*unlock)(void*) = nullptr;
    void* ctx = nullptr;
};

class BigHeapRouter {
public:
    struct Counters {
        uint32_t bigAllocs = 0;      // requests >= threshold served by the region
        uint32_t fallbacks = 0;      // requests >= threshold the region could not hold -> small heap
        uint32_t spills = 0;         // requests < threshold the small heap could not hold -> region
        uint32_t moves = 0;          // realloc moved a block between the pools
        uint32_t failed = 0;         // neither pool had it
        uint32_t firstFallbackBytes = 0;
        uint32_t firstSpillBytes = 0;
    };

    void setOps(const SmallHeapOps& ops, const BigHeapLockOps& lock = BigHeapLockOps{}) {
        ops_ = ops;
        lock_ = lock;
    }
    // The region (taken once, kept for the whole run) and the threshold in bytes. false: the router stays off.
    bool enable(void* region, size_t bytes, size_t threshold) {
        on_ = false;
        if (!core_.init(region, bytes)) return false;
        threshold_ = threshold < 1 ? 1 : threshold;
        on_ = true;
        return true;
    }
    bool on() const { return on_; }
    size_t threshold() const { return threshold_; }
    bool inRegion(const void* p) const { return on_ && core_.contains(p); }

    void* malloc(size_t n) {
        if (!on_) return ops_.malloc(n);
        if (n >= threshold_) {
            if (void* p = bigAlloc(n, BigHeapCore::kGran)) return p;
            void* p = ops_.malloc(n);
            note(p, n, true);
            return p;
        }
        void* p = ops_.malloc(n);
        if (p) return p;
        p = bigAlloc(n, BigHeapCore::kGran);
        note(p, n, false);
        return p;
    }

    void* memalign(size_t align, size_t n) {
        if (!on_ || align == 0 || (align & (align - 1)) != 0) return ops_.memalign(align, n);
        if (n >= threshold_) {
            if (void* p = bigAlloc(n, align)) return p;
            void* p = ops_.memalign(align, n);
            note(p, n, true);
            return p;
        }
        void* p = ops_.memalign(align, n);
        if (p) return p;
        p = bigAlloc(n, align);
        note(p, n, false);
        return p;
    }

    void* calloc(size_t a, size_t b) {
        if (!on_) return ops_.calloc(a, b);
        if (b && a > static_cast<size_t>(-1) / b) return nullptr;
        const size_t n = a * b;
        if (n < threshold_) {
            if (void* p = ops_.calloc(a, b)) return p;
            void* p = bigAlloc(n, BigHeapCore::kGran);
            note(p, n, false);
            if (p) std::memset(p, 0, n);
            return p;
        }
        if (void* p = bigAlloc(n, BigHeapCore::kGran)) {
            std::memset(p, 0, n);
            return p;
        }
        void* p = ops_.calloc(a, b);
        note(p, n, true);
        return p;
    }

    void free(void* p) {
        if (!p) return;
        if (inRegion(p)) {
            lock();
            core_.free(p);
            unlock();
            return;
        }
        ops_.free(p);
    }

    size_t usable(void* p) {
        if (!p) return 0;
        if (inRegion(p)) {
            lock();
            const size_t u = core_.usable(p);
            unlock();
            return u;
        }
        return ops_.usable(p);
    }

    void* realloc(void* p, size_t n) {
        if (!on_) return ops_.realloc(p, n);
        if (!p) return malloc(n);
        if (inRegion(p)) {
            if (n == 0) {  // a valid pointer the caller frees later (never NULL with p freed: some code keeps p then)
                free(p);
                return malloc(0);
            }
            if (n >= threshold_) {
                lock();
                const bool ok = core_.resizeInPlace(p, n);
                unlock();
                if (ok) return p;
                void* q = malloc(n);  // region elsewhere, else the small heap (counted)
                if (!q) return nullptr;
                copyFree(q, p, n, true);
                return q;
            }
            // Shrunk below the threshold: back to the small heap; it has no room - shrink in place.
            if (void* q = ops_.malloc(n)) {
                copyFree(q, p, n, true);
                countMove();
                return q;
            }
            lock();
            core_.resizeInPlace(p, n);
            unlock();
            return p;
        }
        if (n >= threshold_) {
            if (void* q = bigAlloc(n, BigHeapCore::kGran)) {
                copyFree(q, p, n, false);
                countMove();
                return q;
            }
            void* q = ops_.realloc(p, n);
            note(q, n, true);
            return q;
        }
        if (void* q = ops_.realloc(p, n)) return q;
        if (n == 0) return nullptr;
        void* q = bigAlloc(n, BigHeapCore::kGran);  // small heap full: the region (p stays valid when this fails)
        note(q, n, false);
        if (!q) return nullptr;
        copyFree(q, p, n, false);
        return q;
    }

    // ---- probes (no counters) ------------------------------------------------------------------------------------
    bool bigFits(size_t n, size_t align = BigHeapCore::kGran) {
        if (!on_) return false;
        lock();
        const bool ok = core_.canAlloc(n, align);
        unlock();
        return ok;
    }
    // Whether the small heap gives n bytes in one block now (allocated, written, freed: a compiler cannot drop it).
    bool smallGives(size_t n) {
        void* volatile p = ops_.malloc(n ? n : 1);
        if (!p) return false;
        static_cast<volatile char*>(p)[0] = 0;
        ops_.free(p);
        return true;
    }
    // Fix after the emulator's streaming deadlock: whether n bytes of headroom are there now, in the form
    // allocations take - one block of n in the region or in the small heap, or n spread over the small heap
    // (smallFree = its TOTAL free bytes, the caller's estimate) with `floor` bytes left after it and one block of
    // min(n, blockCap) somewhere (so any single allocation up to blockCap still succeeds). Never asks the small heap
    // for more than blockCap in one piece once the region holds n: a fragmented small heap cannot block a probe while
    // the region has room (the first version wanted min(n, floor) contiguous in the small heap besides - after a
    // teleport the small heap was cut into 1.6 MB pieces with 15 MB of region free, and the streamer waited forever).
    // Off: the old probe exactly (malloc + write + free).
    bool gives(size_t n, size_t floor, size_t smallFree, size_t blockCap) {
        if (!on_) return smallGives(n);
        if (bigFits(n) || smallGives(n)) return true;
        if (smallFree < n || smallFree - n < floor) return false;
        const size_t c = n < blockCap ? n : blockCap;
        return bigFits(c) || smallGives(c);
    }
    // Whether one block of n bytes is there now (the region, else the small heap) - what a terrain blob needs.
    bool givesBlock(size_t n) {
        if (!on_) return smallGives(n);
        return bigFits(n) || smallGives(n);
    }
    // An allocation routed as malloc() but not counted (multi-block probes that hold what they take).
    void* probeAlloc(size_t n) {
        if (!on_) return ops_.malloc(n);
        if (n >= threshold_) {
            if (void* p = bigAlloc(n, BigHeapCore::kGran)) return p;
            return ops_.malloc(n);
        }
        if (void* p = ops_.malloc(n)) return p;
        return bigAlloc(n, BigHeapCore::kGran);
    }

    // ---- accounting ---------------------------------------------------------------------------------------------
    BigHeapCore::Stats stats() {
        lock();
        const BigHeapCore::Stats s = core_.stats();
        unlock();
        return s;
    }
    uint32_t bigLargest() {
        if (!on_) return 0;
        lock();
        const uint32_t l = core_.largestFree();
        unlock();
        return l;
    }
    uint32_t regionBytes() const { return on_ ? core_.size() : 0; }
    Counters counters() {
        lock();
        const Counters c = counters_;
        unlock();
        return c;
    }
    bool check() {
        lock();
        const bool ok = core_.check();
        unlock();
        return ok;
    }

private:
    void lock() {
        if (lock_.lock) lock_.lock(lock_.ctx);
    }
    void unlock() {
        if (lock_.unlock) lock_.unlock(lock_.ctx);
    }
    void* bigAlloc(size_t n, size_t align) {
        lock();
        void* p = core_.alloc(n, align);
        if (p) ++counters_.bigAllocs;
        unlock();
        return p;
    }
    // A request served by the other pool (or by none).
    void note(void* p, size_t n, bool bigRequest) {
        lock();
        if (!p) {
            ++counters_.failed;
        } else if (bigRequest) {
            if (counters_.fallbacks++ == 0) counters_.firstFallbackBytes = static_cast<uint32_t>(n);
        } else {
            if (counters_.spills++ == 0) counters_.firstSpillBytes = static_cast<uint32_t>(n);
        }
        unlock();
    }
    void countMove() {
        lock();
        ++counters_.moves;
        unlock();
    }
    // Copies min(old usable, n) bytes from old to q and frees old (in whichever pool it lives).
    void copyFree(void* q, void* old, size_t n, bool oldInRegion) {
        size_t have;
        if (oldInRegion) {
            lock();
            have = core_.usable(old);
            unlock();
        } else {
            have = ops_.usable(old);
        }
        std::memcpy(q, old, have < n ? have : n);
        free(old);
    }

    BigHeapCore core_;
    SmallHeapOps ops_{};
    BigHeapLockOps lock_{};
    Counters counters_{};
    size_t threshold_ = static_cast<size_t>(-1);
    bool on_ = false;
};

}  // namespace wowee::platform::psp
