// A big pak read without its TOC in RAM (creatures and items from the DBCs: tens of
// thousands of records; Pak keeps 16 B + the name per record and stops at 65 535 records).
//
// Disk format: the WPAK v1 of psp_pak.hpp, unchanged, written with
//   - the TOC sorted by name (strcmp byte order, stable: equal names keep their order - Pak::find gives the
//     first of equal names, so does this), and
//   - one more raw record, the LAST one, named "~fence" ('~' sorts after every name the bakers use):
//       FenceHeader, u32 nameOffset[fences] (from the blob start), NUL-terminated names;
//       fence i = the name of TOC record i * step (i < fences = ceil(count / step); count includes "~fence").
//   tools/psp/converter/conv_common.hpp writePakSorted() writes it. A plain Pak opens such a pak as before.
// RAM: the fence blob (~count / step names, ~12 KB for 100 000 records at step 128) + two TOC blocks
// (step x 64 B each) as a cache. A lookup: a binary search on the fence, at most one read of `step` disk
// records (8 KB at 128), a binary search in it. Main thread only (one scratch); the reads go through
// pakRead (thread-safe, shared with the tile streamer).
#pragma once

#include "platform/psp/psp_pak.hpp"

#include <cstdint>
#include <cstring>

namespace wowee::platform::psp {

struct PakFenceHeader {
    char magic[4];       // "WFNC"
    uint32_t version;    // 1
    uint32_t step;       // TOC records per fence (power of two not required)
    uint32_t fences;
    uint32_t records;    // the TOC count, "~fence" included
    uint32_t reserved;
};
static_assert(sizeof(PakFenceHeader) == 24, "WFNC header is 24 bytes");

inline constexpr const char* kPakFenceName = "~fence";

// The lookup on its own, for the PSP and the host test: `fenceName(i)` gives fence i, `readBlock(first,
// n, dst)` reads disk records first .. first + n - 1. Fills `out` with the first record named `name`.
template <typename FenceNameFn, typename ReadBlockFn>
bool pakIndexLookup(const char* name, uint32_t records, uint32_t step, uint32_t fences, FenceNameFn&& fenceName,
                    ReadBlockFn&& readBlock, PakDiskEntry* scratch, PakDiskEntry& out) {
    if (!name || records == 0 || step == 0 || fences == 0) return false;
    // The last fence whose name is < name: equal names may start in the block before the fence that equals
    // the name (a run of equal names crossing a block boundary).
    uint32_t lo = 0, hi = fences;  // first fence >= name
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (std::strcmp(fenceName(mid), name) < 0) lo = mid + 1;
        else hi = mid;
    }
    uint32_t block = lo == 0 ? 0 : lo - 1;
    for (; block < fences; ++block) {
        const uint32_t first = block * step;
        if (first >= records) return false;
        const uint32_t n = records - first < step ? records - first : step;
        if (!readBlock(first, n, scratch)) return false;
        uint32_t a = 0, b = n;  // first record >= name in the block
        while (a < b) {
            const uint32_t mid = (a + b) / 2;
            scratch[mid].name[sizeof(scratch[mid].name) - 1] = '\0';
            if (std::strcmp(scratch[mid].name, name) < 0) a = mid + 1;
            else b = mid;
        }
        if (a < n) {
            scratch[a].name[sizeof(scratch[a].name) - 1] = '\0';
            if (std::strcmp(scratch[a].name, name) != 0) return false;
            out = scratch[a];
            return true;
        }
        // Every record of the block is < name: the name, if any, starts the next block.
    }
    return false;
}

class PakIndex {
public:
    PakIndex() = default;
    ~PakIndex();
    PakIndex(const PakIndex&) = delete;
    PakIndex& operator=(const PakIndex&) = delete;

    // False for a missing file, a pak that is not WPAK v1, or one without the "~fence" record (an old or
    // unsorted pak: open it with Pak).
    bool open(const char* absPath);
    void close();
    bool isOpen() const { return records_ > 0; }  // also while the descriptor is given back
    int fd() const { return fd_; }  // -1 while given back (releaseFd)
    // On hardware 3 descriptors were free at world entry (wotlk.pak / ui.pak failed with 0x80020320): an index
    // read only now and then (items.pak: dressing) gives its descriptor back; find()/load() reopen it. Only for an
    // index one thread uses through find()/load() (not the creatures', whose fd() the streamer reads).
    void releaseFd();
    void releaseIfIdle(uint32_t nowMs, uint32_t idleMs);
    // The descriptor budget: a rare index (Pak::setRare, psp_pak.hpp) - pakFdRelieve may ask for its
    // descriptor when an open failed for want of one; main thread only, fd() never handed to another thread.
    void setRare(bool on);
    // Memory diet: load() takes its block from the world arena's high end first (NPC models).
    void setWorldArena(bool on) { worldArena_ = on; }
    uint32_t fileId() const { return fileId_; }
    uint32_t count() const { return records_; }

    // The first record named `name` (one TOC block read unless cached). `out.name` points into this index
    // and stays valid until the next find(); copy the record if it has to live longer.
    bool find(const char* name, PakEntry& out);
    // Reads an entry like Pak::load (64-byte aligned heap block, D-cache written back; a blob the tile
    // streamer parked for this file is handed out instead). Free with pakFree().
    void* load(const PakEntry& e);

    // RAM held while open (the fence + the block cache) and lookup counters (log lines).
    uint32_t ramBytes() const { return ramBytes_; }
    uint32_t lookups() const { return lookups_; }
    uint32_t blockReads() const { return blockReads_; }

private:
    bool rare_ = false;          // the descriptor budget
    bool worldArena_ = false;    // setWorldArena
    static uint32_t fdLastUse(void* self);  // FdRegistry hooks
    static void fdRelease(void* self);
    bool readBlock(uint32_t first, uint32_t n, PakDiskEntry* dst);
    bool ensureFd();             // reopens after releaseFd
    char path_[96] = {};         // for ensureFd
    uint32_t lastUseMs_ = 0;     // for releaseIfIdle
    const char* fenceName(uint32_t i) const;

    int fd_ = -1;
    uint32_t fileId_ = 0;
    uint32_t records_ = 0, tocOffset_ = 0, step_ = 0, fences_ = 0;
    uint8_t* fence_ = nullptr;   // the "~fence" blob
    uint32_t fenceSize_ = 0;
    PakDiskEntry* blocks_ = nullptr;  // two cached TOC blocks of step_ records, then one scratch block
    uint32_t cachedFirst_[2] = {~0u, ~0u};
    uint32_t cachedN_[2] = {0, 0};
    uint32_t lru_ = 0;           // the cache slot to replace next
    char name_[48] = {};         // find()'s result name
    uint32_t ramBytes_ = 0, lookups_ = 0, blockReads_ = 0;
};

} // namespace wowee::platform::psp
