// Tile streaming for the world screen (src/ui/psp/ui_world_streaming.cpp).
//
// Upstream TerrainManager rules (src/rendering/terrain_manager.cpp), PSP-sized:
//  - a window of (2R+1)^2 ADT tiles around the player's tile is kept loaded
//    (wowee.cfg streamradius, R = 1 by default: the 3x3 of world entry;
//    PSP: R is 0 or 1 - upstream loads R = 6, a 13x13 grid);
//  - crossing a tile border queues the missing tiles of the new window,
//    nearest first (upstream streamTiles: sorted by distance, pushed front);
//  - tiles outside the window are evicted once the player is more than
//    `streammargin` yards past the border that left them behind. PSP: upstream
//    unloads beyond unloadRadius = loadRadius + 3 tiles; 64 MB cannot hold
//    3 more rings, so the hysteresis is a band of yards (walking along a
//    border does not thrash), and a low-memory check (streamreserve) evicts
//    everything outside the window before a new tile starts;
//  - per-frame finalization budget (upstream advanceFinalization, 8 ms): every
//    install / eviction / eDRAM copy is one step, steps run until `streambudget`
//    ms are used (at least one per frame).
//
// PSP: upstream's worker threads parse ADTs; here a loader thread
// only reads pak entries (Memory Stick seek ~15 ms, a terrain tile 1.6 MB) in
// 64 KB pieces into pak-style blobs, the main thread parks them
// (pakPrefetchPut) and then calls the unchanged loaders - TerrainRenderer /
// ModelRenderer::loadTile, CreatureRenderer::appendStaticSpawns - whose
// Pak::load takes the parked blob instead of reading. A tile appears at once
// (terrain, then its doodads with every model/texture/collision mesh), never
// half-textured. Frees happen in update(), after frameEnd's sceGuSync, so the
// GE never reads freed memory. wowee.cfg stream=0 keeps the pre-streaming
// behaviour (3x3 at entry, nothing more).
#pragma once

#include "platform/psp/psp_log.hpp"  // logUptimeMs (nearStarvedMs)
#include "platform/psp/psp_pak.hpp"
#include "rendering/psp/psp_creatures.hpp"
#include "rendering/psp/psp_models.hpp"  // memory budget: ModelRenderer::LoadBudget

#include <cstdint>
#include <deque>
#include <vector>

namespace wowee::rendering::psp {
class TerrainRenderer;
class ModelRenderer;
} // namespace wowee::rendering::psp

namespace wowee::ui::psp {

// Reads pak entries on a low-priority kernel thread (or on the caller, one per
// pumpSync, with wowee.cfg streamthread=0). Single producer (main thread)
// and single consumer on each ring, no locks; the loader thread never logs,
// never touches renderer state and allocates only the blob (newlib malloc is
// locked by libcglue).
class StreamLoader {
public:
    struct Request {
        int fd = -1;
        uint32_t fileId = 0, offset = 0, size = 0;
        uint8_t kind = 0;
        uint32_t id = 0, key = 0;
        // Memory diet: read into this buffer (the caller's, e.g. the terrain pool) instead of a new
        // heap block; the result's blob is dst (or null when the read failed) and is never freed by the loader.
        void* dst = nullptr;
        bool freeDst = false;  // Dst belongs to the request until its result is taken (freed on a failed
                               // read or a stop) - the big WMOs' arena blocks
    };
    struct Result {
        Request req;
        void* blob = nullptr;  // null: read or allocation failed
    };
    static constexpr uint32_t kRing = 256;

    StreamLoader() = default;
    ~StreamLoader() { stop(); }
    StreamLoader(const StreamLoader&) = delete;
    StreamLoader& operator=(const StreamLoader&) = delete;

    bool start(bool useThread, int priority);
    void stop();
    bool threaded() const { return thread_ >= 0; }
    // Main thread. false when the ring is full (keep it for later).
    bool push(const Request& r);
    bool pop(Result& out);
    // Requests not handed back yet (queued + reading + done, unpopped).
    uint32_t inFlight() const { return reqHead_ - resTail_; }
    // No thread: performs one queued read on the caller. Returns false when idle.
    bool pumpSync();

private:
    static int threadMain(unsigned int args, void* argp);
    void run();
    bool readOne();

    Request reqs_[kRing];
    Result results_[kRing];
    volatile uint32_t reqHead_ = 0, reqTail_ = 0;  // main writes head, loader tail
    volatile uint32_t resHead_ = 0, resTail_ = 0;  // loader writes head, main tail
    volatile bool quit_ = false;
    int thread_ = -1;
    int sema_ = -1;
};

class TileStreamer {
public:
    // What the streamer drives, handed in each call (WorldScreen members).
    struct Env {
        rendering::psp::TerrainRenderer& terrain;
        rendering::psp::ModelRenderer& models;
        rendering::psp::CreatureRenderer& creatures;
        uint32_t mapId;
        bool staticNpcs;  // DB spawns are shown (npcstatic=1, no live units yet)
        float x, y;       // player, server coords
    };

    TileStreamer() = default;
    ~TileStreamer();
    TileStreamer(const TileStreamer&) = delete;
    TileStreamer& operator=(const TileStreamer&) = delete;

    bool active() const { return active_; }
    // Opens the paks, adopts the tiles world entry loaded, starts the loader.
    void start(Env& env, const char* mapDir);
    // Once per frame from WorldScreen::update (after movement).
    void update(Env& env, float dtSec);
    // big WMOs: the doodads pak the streamer holds open (a big WMO's doodads use its models), or null.
    platform::psp::Pak* doodadPak() { return active_ && doodadPak_.isOpen() ? &doodadPak_ : nullptr; }
    // A terrain tile was installed this frame (the screen clears "not baked").
    bool takeTerrainInstalled() {
        const bool t = terrainInstalled_;
        terrainInstalled_ = false;
        return t;
    }
    // ---- memory budget ----
    // Doodads by budget: a job reads at most jobKb_ of models (nearest the player first), the install places them
    // while the heap still gives tilereserve KB, the rest of the tile is deferred (ModelRenderer::LoadBudget) and
    // read by top-up jobs - one at a time, when streamreserve KB can be allocated and the memory governor has
    // nothing cut (setTopUp). The terrain pak's descriptor goes back while nothing is read (the card's limit).
    void setTopUp(bool on) { topUpOk_ = on; }
    // Memory diet: a tile's terrain could not start for want of memory since the last call - the
    // memory governor frees lower priorities for the ground (psp_mem_governor.hpp starveStepFor).
    // The landing hold: the doodads of the tile under (x, y) are in, or it has none / none are wanted.
    bool doodadsInAt(float x, float y);
    uint32_t lowStarves() const { return lowStarves_; }
    bool takeTerrainStarved() {
        const bool s = terrainStarved_;
        terrainStarved_ = false;
        return s;
    }
    uint32_t topUps() const { return topUps_; }
    bool terrainFdHeld() const { return terrainFd_ >= 0; }
    // world_flush - tpflush: everything streamed for the old place goes - the loader stopped (its
    // owned buffers back), the parked blobs dropped, the job and the tiles forgotten, the terrain renderer and the
    // doodads' renderer emptied (their tables set up again as at start) - and the loader starts again: the next
    // update queues the new place's window as at world entry. Returns the tiles it held. Nothing when not active.
    uint32_t flush(Env& env);
    // A teleport (any distance): the window's stream_ready line is logged again once the new window is in.
    void markTeleport() {
        readyLogged_ = false;
        readyLate_ = false;
        readyNearLogged_ = false;
        readySinceMs_ = 0;
    }
    uint32_t texRetries() const { return texRetryJobs_; }
    // Ms the doodads in view have waited with nothing in flight for them (0 = they are in / coming).
    uint32_t nearStarvedMs() const {
        return nearStarveSinceMs_ ? platform::psp::logUptimeMs() - (nearStarveSinceMs_ & ~1u) : 0u;
    }
    void nearStarveReset() { nearStarveSinceMs_ = 0; }
    // ---- memory budget (end) ----

private:
    // The terrain's chunk-texture window: kTerrainTail - a windowed tile's v4 detail table;
    // kTerrainTex - (unused since the terrain pool: the window reads every row); kChunkTex - a texture run
    // the window asked for (no job).
    enum Kind : uint8_t {
        kTerrain = 1, kDetail, kDoodads, kModel, kCollision, kTexture, kSpawns, kImpostor,
        kTerrainTail, kTerrainTex, kChunkTex
    };
    struct TileRec {
        int x, y;
        bool terrain = false, doodads = false;
        bool deferred = false;                  // memory budget: doodads in, some deferred (top-up wanted)
        bool spawnsKnown = false;               // s_ read (or none baked)
        // Its terrain could not be loaded for want of memory (or a read failed) - asked again after
        // terrainRetryMs (hardware hw2 crashed in loadTile instead).
        bool terrainRetry = false;
        uint32_t terrainRetryMs = 0;
        std::vector<rendering::psp::CreatureUnit> spawns;
    };
    struct Job {
        bool active = false;
        int x = 0, y = 0;
        uint32_t startMs = 0, reads = 0, bytes = 0, installUs = 0, misses = 0;
        int pendingTerrain = 0, pendingDoodads = 0;
        bool wantTerrain = false, wantDoodads = false;
        bool terrainDone = false, doodadsDone = false;
        bool topUp = false;                      // memory budget: the tile's deferred doodads (resumeTile)
        bool texRetry = false;                   // A top-up for the textures drawn batches found missing
        bool nearRule = false;                   // Doodads in view (memoryOk / install without the guard)
        uint32_t modelBytes = 0;                 // memory budget: m_ + k_ bytes requested (jobKb_)
        uint32_t deferredBefore = 0;             // memory budget: the tile's deferred placements at the start
        uint32_t doodadOffset = 0, doodadSize = 0;  // parked "d_" blob (rescan after evictions)
        std::vector<uint16_t> details, textures;    // requested
        struct ModelRef {
            uint16_t id;
            uint32_t offset, size;               // its "m_" entry (parked once read)
        };
        std::vector<ModelRef> models;            // requested
        std::vector<ModelRef> impostors;         // Requested "i_" (same id as its model)
        // The terrain entry (head / tail / runs are slices of it) and the texture runs read with the job.
        uint32_t terrainOffset = 0, terrainSize = 0;
    };
    struct SpawnEntry {
        int x, y;
        uint32_t offset, size;
    };

    TileRec* findTile(int x, int y);
    bool inWindow(int x, int y) const;
    float yardsTo(int x, int y, float px, float py) const;
    void computeQueue(float px, float py);
    void request(const platform::psp::Pak& pak, int fd, const char* name, uint8_t kind, uint32_t id, uint32_t key);
    bool requestSpawns(int x, int y, uint32_t key);
    void drainResults(Env& env);
    void onDoodads(Env& env, const void* blob, uint32_t size);
    void onModel(Env& env, uint16_t id, const void* blob, uint32_t size);
    void onImpostor(Env& env, const void* blob, uint32_t size);  // Its atlas texture
    void rescanJob(Env& env);
    void startJob(Env& env, int x, int y, bool doodadsOk);
    bool stepInstall(Env& env);
    bool stepEvict(Env& env, bool lowMem);
    // PSP (the whole eastern continent: a village's 3x3 tiles of doodads, 23 MB of models, textures and collision,
    // ran out of memory at world entry): doodads only for the tiles of the window within doodadYd_ (Chebyshev yards
    // to the tile's square; wowee.cfg doodadradius, 0 = the whole window as before), dropped past doodadYd_ +
    // kDoodadKeepYd; the terrain keeps its 3x3.
    bool wantDoodads(int x, int y, float px, float py) const;
    bool needsDoodads(const TileRec& t, float px, float py) const;
    bool needsTerrain(const TileRec& t) const;  // Its terrain failed for want of memory, retry due
    uint32_t terrainRetries_ = 0, retryScanMs_ = 0;
    bool stepEvictDoodads(Env& env);
    void finishJob(Env& env);
    void rebuildSpawns(Env& env);
    // Terrain = the tile's ground (one block of terrainNeedKb + terrainreserve KB in total, sets
    // terrainStarved_ when short); else doodads (streamreserve KB of headroom).
    // nearDoodads - doodads the player can see (the near rule: tilereserve + one block, guard usable).
    bool memoryOk(bool terrain, bool nearDoodads = false);
    bool terrainStarved_ = false, terrainLowReported_ = false;
    uint32_t lowStarves_ = 0;  // Doodad jobs that could not start for want of memory (mem_share starve_low=)
    uint32_t terrainReserveKb_ = 1024;
    // memory budget
    bool stepTopUp(Env& env);
    bool stepTexRetry(Env& env, uint32_t now);  // Missing doodad textures read again
    // "stream_ready near= full= tiles= doodads=a/b deferred= in_view= texmiss= ms=" - when what is in
    // view is in, when the whole window is, after 45 s without it (+ stream_ready_deferred: tile:deferred/in_view@yd).
    void checkReady(Env& env);
    bool readyLogged_ = false, readyLate_ = false, readyNearLogged_ = false;
    uint32_t readySinceMs_ = 0, texRetryJobs_ = 0, flushes_ = 0, nearStarveSinceMs_ = 0;
    bool useThread_ = true;
    int prio_ = 40;
    void requestModel(Env& env, uint16_t id, uint32_t key);
    rendering::psp::ModelRenderer::LoadBudget installBudget(const Env& env) const;
    bool ensureTerrainFd();
    void releaseTerrainFdIfIdle();
    // ---- The terrain's chunk-texture window ----
    // A raw slice of a pak file for the loader (the window's head / tail / texture runs are not pak entries).
    void requestRaw(const platform::psp::Pak& pak, int fd, uint32_t offset, uint32_t size, uint8_t kind, uint32_t id,
                    uint32_t key, void* dst = nullptr);
    // Frees the chunk textures the player left behind and asks for the ones ahead (terraintexreads runs in flight),
    // when the player moved terraintexstep yd, a run came back or a tile came / went - at least once a second.
    void stepTextures(Env& env);
    uint32_t terrainNeedKb() const;  // one block a tile's terrain needs (head + vertices, or the whole blob)
    uint32_t texInFlight_ = 0, texMaxRuns_ = 6, texNextMs_ = 0, texReads_ = 0, texReadFails_ = 0, texLogs_ = 0;
    uint32_t texBackoffMs_ = 0;  // no texture reads before this (one found no memory)
    // The terrain pool's row buffers handed to the loader (given back on the result, or by the
    // destructor after the loader stopped: the pool outlives this streamer).
    std::vector<void*> runsOut_;
    bool terrainSlotsReported_ = false;
    float texCheckX_ = -1e9f, texCheckY_ = -1e9f, texStepYd_ = 8.0f;
    bool texDirty_ = true, texMore_ = false;
    bool topUpOk_ = true;
    uint32_t jobKb_ = 3072, tileReserveKb_ = 1536, terrainIdleMs_ = 5000;
    uint32_t topUps_ = 0, topUpNextMs_ = 0, lastTerrainReqMs_ = 0, terrainReleases_ = 0, deferredLogs_ = 0;
    uint32_t fdWaitLogs_ = 0;

    bool active_ = false;
    bool terrainInstalled_ = false;
    int radius_ = 1;
    float marginYd_ = 64.0f;
    float budgetMs_ = 4.0f;
    uint32_t reserveKb_ = 4096;
    platform::psp::Pak terrainPak_, doodadPak_;
    int terrainFd_ = -1, doodadFd_ = -1, creatureFd_ = -1;
    uint32_t creatureFileId_ = 0;
    std::vector<SpawnEntry> spawnEntries_;
    StreamLoader loader_;
    std::deque<StreamLoader::Request> backlog_;  // waiting for ring space
    std::vector<TileRec> tiles_;
    std::vector<uint32_t> absent_;               // keys with nothing baked
    std::vector<uint32_t> queue_;                // keys, nearest first
    int curX_ = -1000, curY_ = -1000;            // player tile of the last queue
    Job job_;
    bool npcStatic_ = true;                      // wowee.cfg npcstatic
    uint32_t spawnsPending_ = 0;
    bool spawnsDirty_ = false;
    bool lowMemReported_ = false;
    bool vramBusy_ = false;
    uint32_t vramCopied_ = 0;
    uint32_t nextLogMs_ = 0;
    uint32_t failLogs_ = 0;
    uint32_t evictions_ = 0, loads_ = 0;
    float doodadYd_ = 0.0f;                      // wowee.cfg doodadradius (0 = the whole window)
    float checkX_ = 0.0f, checkY_ = 0.0f;        // where the doodad radius was last checked
    uint32_t doodadEvictions_ = 0;
};

} // namespace wowee::ui::psp
