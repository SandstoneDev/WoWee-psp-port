// Sounds. Sound effects from data/sounds.pak (+ sounds_1.pak ... when
// the converter split the set, --max-pak-mb; tools/psp/converter
// psp_sound_conv, format include/rendering/psp/psp_sound_format.hpp), loaded
// on demand into a RAM cache and played through the effects mixer
// (psp_audio sfxPlay).
//
// Card open-file limit (0x80020320 SCE_KERNEL_ERROR_MFILE): the pak
// is NOT kept open. open() reads its TOC, the "entries" table and the surface
// grids of one map, and closes it. A clip that is not in RAM is read by a
// low-priority loader thread, which opens the pak for the reads (pakRead:
// the shared card lock, 64 KB pieces) and closes it again after 2 s without a
// request - a burst of first plays shares one open, and in steady state (the
// working set cached) no descriptor is held at all. The main thread never
// waits for the card: a clip still loading plays when it arrives if that is
// within lateMs, else the play is dropped (logged).
//
// Cache: least recently used clips are freed while the resident total is over
// the budget (wowee.cfg sfxbudget, KB), never one a voice may still be
// mixing (its play time + 150 ms has not passed).
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::platform::psp {

struct SoundBankStats {
    uint32_t clips = 0;        // in the pak
    uint32_t resident = 0;     // in RAM
    uint32_t residentKb = 0;
    uint32_t loads = 0;        // clips read from the card
    uint32_t loadFails = 0;
    uint32_t evictions = 0;
    uint32_t plays = 0;
    uint32_t latePlays = 0;    // played when their load arrived
    uint32_t drops = 0;        // not played: still loading after lateMs, failed, or no voice
};

class SoundBank {
public:
    SoundBank() = default;
    ~SoundBank();
    SoundBank(const SoundBank&) = delete;
    SoundBank& operator=(const SoundBank&) = delete;

    // Reads the TOC, "entries" and the "g_<mapId>_*" grids, closes the pak,
    // starts the loader thread. False when the pak is missing or bad.
    bool open(const char* absPath, uint32_t budgetKb, uint32_t mapId);
    void close();
    bool isOpen() const { return open_; }

    // Clip of a game path hash (soundPathHash), -1 when not baked.
    int find(uint32_t pathHash) const;
    uint32_t clipCount() const { return static_cast<uint32_t>(clips_.size()); }
    uint32_t clipHash(int index) const {
        return index >= 0 && static_cast<size_t>(index) < clips_.size() ? clips_[static_cast<size_t>(index)].hash : 0u;
    }

    // Plays clip `index` on a free (or the quietest) voice: gain 0..1, pan
    // -1..1, pitch = playback rate factor (upstream's pitch variation).
    // Returns false when it could not be started or queued.
    bool play(int index, float gain, float pan, float pitch);
    // Loads the clip ahead (no sound).
    void prefetch(int index);
    // Main thread, once per frame: finished loads, late plays, eviction.
    void update();

    // "entries" (SoundEntriesHeader...) and the surface grids, kept in RAM.
    const void* entriesBlob(uint32_t& size) const {
        size = entriesSize_;
        return entries_;
    }
    // Grid of ADT tile x_y (kSurfaceGridBytes, psp_sound_format.hpp) or nullptr. PSP: read from sounds.pak when
    // first asked (one ~9 KB read on a tile change) into a cache of kGridCache tiles - a full client's bake has a grid
    // for every tile of the map (687 for one continent = 6.3 MB), and all of them in RAM ran the heap out at world entry.
    const uint8_t* surfaceGrid(int tileX, int tileY) const;
    uint32_t gridCount() const { return static_cast<uint32_t>(gridOffsets_.size()); }

    uint32_t budgetKb = 1024;
    uint32_t lateMs = 250;
    const SoundBankStats& stats() const { return stats_; }

private:
    struct Clip {
        uint32_t hash = 0, offset = 0, size = 0;
        void* block = nullptr;           // SoundClipHeader + PCM
        uint32_t lastUsedMs = 0, busyUntilMs = 0;
        uint8_t state = 0;               // 0 absent, 1 loading, 2 resident, 3 failed
        uint8_t pak = 0;                 // part: 0 sounds.pak, k sounds_<k>.pak
    };
    static constexpr int kMaxPaks = 8;
    struct Late {
        int index = -1;
        float gain = 0, pan = 0, pitch = 1;
        uint32_t deadlineMs = 0;
    };
    bool request(int index);
    bool start(Clip& c, float gain, float pan, float pitch, uint32_t nowMs);
    void evict(uint32_t nowMs);
    static int loaderMain(unsigned int args, void* argp);
    int loaderLoop();

    bool open_ = false;
    char paths_[kMaxPaks][128] = {};
    int paks_ = 0;
    std::vector<Clip> clips_;            // sorted by hash
    std::unordered_map<uint32_t, uint32_t> gridOffsets_;  // tileX * 100 + tileY -> offset in sounds.pak
    static constexpr int kGridCache = 6;  // the player's tile and its neighbours (a corner touches 4)
    struct GridSlot {
        uint32_t key = ~0u;
        uint32_t lastUse = 0;
        bool ok = false;      // false: the read failed
        uint32_t failMs = 0;  // when it was tried (retried 2 s later)
        std::vector<uint8_t> data;
    };
    mutable GridSlot gridCache_[kGridCache];
    mutable uint32_t gridTick_ = 0, gridReads_ = 0, gridLogs_ = 0;
    void* entries_ = nullptr;
    uint32_t entriesSize_ = 0;
    uint32_t residentBytes_ = 0;
    Late late_[8];
    SoundBankStats stats_;
    uint32_t loadLogs_ = 0, failLogs_ = 0;

    // Loader thread rings (single producer, single consumer each).
    static constexpr uint32_t kRing = 64;
    struct Done {
        int32_t index;
        void* block;
        uint32_t ms;
    };
    int32_t req_[kRing] = {};
    Done done_[kRing] = {};
    volatile uint32_t reqW_ = 0, reqR_ = 0, doneW_ = 0, doneR_ = 0;
    volatile bool running_ = false;
    int thread_ = -1;
    int sema_ = -1;
    volatile int openFails_ = 0;
};

} // namespace wowee::platform::psp
