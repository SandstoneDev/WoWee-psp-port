// PSP (docs/PSP_PORT_ARCHITECTURE.md): the big-block heap region on the PSP - the link-time wrappers of
// malloc/free/realloc/calloc/memalign/aligned_alloc/malloc_usable_size (-Wl,--wrap=..., psp/CMakeLists.txt) route
// requests >= bigheapmin KB into one region taken from newlib's heap once at boot (psp_bigheap_core.hpp), and the
// heap probes / accounting of the whole program see both pools through the functions below.
//
// wowee.cfg (read once by bigheapInit):
//   bigheap=2          the default (memory diet): 2 = the SMALL-object arena (psp_small_arena.hpp):
//                      requests of at most smallarenamax bytes in an arena of smallarenakb KB taken from newlib at
//                      boot, the big blocks keep all of newlib (its holes coalesce when the world goes); 1 = the
//                      big-block region below; 0 = the wrappers pass straight through to newlib: the old heap exactly
//   wmoarenakb=7168    KB of the world arena - the big WMOs (low end: headers, chunks, shells, collision,
//                      textures) and the doodads pak's / NPC models' blobs (high end; 0 = off)
//   heapbigstats=1     heap_big_allocs every 10 s (newlib blocks >= 64 KB alive by size class)
//   smallarenakb=1536  KB of the small-object arena (bigheap=2); full - small requests go to newlib (small_arena_full;
//                      the small_arena lines after mem at= give its peak for tuning)
//   smallarenamax=2048 bytes: the largest request the arena takes (16 .. 4096)
//   heapblockkb=256    the headroom probe's block (bigheap 0 / 2): free in total + one block of min(n, this)
//   bigheapkb=20480    KB of the region (taken from the heap at boot; at most the heap's largest block - 8 MB).
//                      ~ half the PSP-3000 heap: a world entry holds ~17 MB of terrain (9 tiles x 1.9 MB) + collision,
//                      big WMO groups, pak reads, TOCs in big blocks, ~20 MB of doodads / NPCs / UI in small ones
//   bigheapmin=256     KB: requests of at least this go to the region
//   bigheapfloor=1024  KB newlib must keep free IN TOTAL after a probe that passes on newlib's total free
//                      (heapGivesKb's third way; never a contiguous demand - see BigHeapRouter::gives)
// Log: bigheap_init (boot), bigheap_full / bigheap_spill (once each, the first request that changed pool),
// "bigheap big_used_kb= ..." after mem at= / mem_watch / oom new failed / stream_lowmem, bigheap_peak at exit.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wowee::platform::psp {

// After configLoad and the emulator's ballast (main_psp.cpp): takes the region, turns the routing on. Main thread,
// before any other thread allocates.
// ballastKb: the emulator's ballast (main_psp.cpp) - newlib holds it, the bigheap line leaves it out of small_used_kb.
void bigheapInit(uint32_t ballastKb);
bool bigheapOn();

struct HeapPools {
    bool on = false;
    uint32_t usedKb = 0;          // both pools, what callers hold (the region's own reservation not counted)
    uint32_t smallUsedKb = 0;     // newlib minus the region
    uint32_t smallLargestKb = 0;  // the largest block newlib gives now (probed; 0 when not asked)
    uint32_t bigKb = 0;           // region size
    uint32_t bigUsedKb = 0;
    uint32_t bigLargestKb = 0;    // the largest block the region gives now (exact)
    uint32_t bigPeakKb = 0;
    uint32_t bigBlocks = 0;
    uint32_t bigFreeBlocks = 0;
    uint32_t fallbacks = 0;       // big requests the region could not hold -> newlib
    uint32_t spills = 0;          // small requests newlib could not hold -> region
    uint32_t badFrees = 0;
};
HeapPools heapPools(bool probeSmallLargest);

// Bytes / KB in use in both pools (replaces mallinfo().uordblks: that counts the region's reservation as used and
// misses what is inside it).
uint32_t heapUsedBytes();
uint32_t heapUsedKb();
// newlib's own numbers (the region counts as used) - the heap_budget anchor at boot.
uint32_t heapRawUsedKb();
uint32_t heapSmallLargestKb();
// The largest block one malloc gives now, KB (either pool, 64 KB steps on the small heap).
uint32_t heapLargestKb();
// Whether `kb` KB / `bytes` of headroom are there now: one block in the region or in newlib, or that much of newlib's
// total free with bigheapfloor KB left and a 1 MB block somewhere (BigHeapRouter::gives). bigheap=0 (memory
// diet; was one malloc of the whole size): that much free in total (heapFreeKb) and one block of min(size, 1 MB)
// - a fragmented heap with 11 MB free and a 3 MB largest block (hardware, a capital city) is not "full"; a caller that
// is about to allocate one big block asks heapRoomFor / heapGivesBlockKb for it.
bool heapGivesKb(uint32_t kb);
bool heapGivesBytes(uint32_t bytes);
// The heap's total free KB now - newlib's size at boot minus what it holds (the emulator's ballast
// counts as held), plus the region's free with bigheap=1.
uint32_t heapFreeKb();
uint32_t heapFreeBytes();
// The fair-sharing rule: one block of `blockBytes` now (the read about to happen) AND `marginKb` KB
// still free in total after it - the room left for the next consumers, never a contiguous margin.
bool heapRoomFor(uint32_t blockBytes, uint32_t marginKb);
// PSP (after an emulator autopilot run: the doodads filled the heap down to 1.3 MB free and left
// it shredded): the guard block - one block of heapguardkb KB (4096) must stay free for the LOW priorities to load
// (doodads, top-ups, a far big WMO); the ground (terrain pool), the NPCs near the player and the big WMO the camera is in
// may use it. A heap that keeps one big hole stays a heap with big holes.
// PSP (an emulator run: 13.5 MB free, the largest 2.7 MB - the guard could never be met, the doodads of the
// last place never loaded): the guard holds the heap's last big piece - while one >= heapguardkb exists, the low
// priorities may not cut it below that (largest >= guard + 1 MB, or no such piece at all: "lost", nothing to keep).
// Each refusal is counted (heapGuardRefusals: mem_share starve_low=).
bool heapGuardOk();
uint32_t heapGuardRefusals();
// PSP (an emulator run: a capital city's blobs shredded newlib): the big WMOs' arena (wmoarenakb, 4608 KB,
// taken at boot, kept for the run): BigHeapCore over one block; free / realloc / malloc_usable_size of a pointer in
// it are routed by address, so a blob from it is freed with free() / pakFree / textureRelease as any other. Null:
// off or full (the caller falls back to newlib). wmo_arena line: wmoArenaLog.
void* wmoArenaAlloc(size_t bytes, size_t align);
bool wmoArenaOn();
bool wmoArenaHasKb(uint32_t kb);  // one block of kb KB free in the WMOs' arena
// PSP (an emulator run: the arena idled through a village while the doodads filled newlib to 1.4 MB
// free): the same arena is the WORLD arena - the doodads' pak blobs take it too (a low priority: while no big WMO is
// open, or while the arena keeps worldarenawmokb (1024) free besides the request) - from the arena's HIGH end, the big
// WMOs from its low end (BigHeapCore::allocHigh / allocLow), so the two kinds of churn do not interleave. Null: off /
// full / kept for the WMO.
void* worldArenaAlloc(size_t bytes, size_t align);
void wmoArenaSetBigOpen(uint32_t n);  // the big WMOs open now (WorldScreen::updateBigWmos)
void wmoArenaLog();
// world_flush: the world arena now (zeros when off).
struct WorldArenaInfo {
    uint32_t usedKb = 0, usedBlocks = 0, freeBlocks = 0, largestKb = 0, sizeKb = 0;
};
WorldArenaInfo worldArenaInfo();
// After a world flush every owner has given its blocks back: the arena back to one free block. Returns the blocks
// still in use (0 = reset done); with any, nothing is reset (a live block is never dropped) and `list` gets up to
// eight "+offset_kb:bytes" of them for the world_flush_leak line.
uint32_t worldArenaReset(char* list, size_t listBytes);
// heap_big_allocs - newlib blocks >= 64 KB alive by size class (heapbigstats=1; a measurement aid).
void heapBigStatsLog();
uint32_t heapGuardKb();
// Whether ONE block of `kb` KB is there now (region, else newlib) - a tile's terrain blob. bigheap=0: the old probe.
bool heapGivesBlockKb(uint32_t kb);
// newlib's total free KB (its size at boot minus what it holds, the region's block and the ballast included).
uint32_t heapSmallFreeKb();
// The memory governor's small-heap condition: newlib has `kb` KB free in total. Always true with bigheap=0.
bool heapSmallRoomKb(uint32_t kb);
// malloc() routing without the pool-change counters - for probes that hold several blocks at once; free with free().
void* heapProbeAlloc(size_t bytes);

// The pools in one log line of its own (lines stop at 120 characters - the mem lines are full already), right after
// mem at= / mem_watch / oom new failed / stream_lowmem: "bigheap big_used_kb= big_max_kb= small_used_kb=
// small_max_kb= free_kb=" (small_used_kb without the emulator's ballast, free_kb = newlib's total free, small_max_kb:
// binary search over newlib, ~10 mallocs). fb / sp: bigheap_full / bigheap_spill / bigheap_peak. Nothing with
// bigheap=0.
void bigheapLogPools();
// "bigheap_peak big_kb= big_peak_kb= big_used_kb= holes= fb= sp= bad=" (mem_peak's companion at exit).
void bigheapLogPeak();
// Logs bigheap_full / bigheap_spill the first time either happened (main thread; the wrappers never log).
void bigheapPoll();

} // namespace wowee::platform::psp
