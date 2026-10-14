// Tile streaming (design: include/ui/psp/ui_world_streaming.hpp,
// the streamer design). ui_world_screen.cpp only calls
// startStreaming() at the end of the constructor and updateStreaming() at the
// end of update().
//
// wowee.cfg:
//   stream=1          0 = pre-streaming behaviour (3x3 at entry only; A/B)
//   streamradius=1    window radius in tiles, 0 or 1 (0: the player's tile)
//   streambudget=4    ms of installs/evictions per frame (at least one step)
//   streammargin=64   yd past a border before the tiles behind are evicted
//   streamreserve=4096  KB of headroom a doodads job / top-up needs to start (total free + a 1 MB block -
//                     pp::heapGivesKb; was one block of 4 MB: the hardware fell through a capital city with 16 MB free)
//   terrainreserve=1024  a tile's terrain (the player's ground, the highest priority) starts with
//                     one block of its need (512 KB with the texture window, 2 MB without) + this many KB free in
//                     total; short - the memory governor is told (takeTerrainStarved) and frees lower priorities
//   streamthread=1    0 = reads on the main thread, one per frame (A/B)
//   streamprio=40     loader thread priority (main thread 32; larger = lower)
//   doodadradius=200  yd to a tile's square for its doodads (0 = every tile of the window);
//                     dropped past it + 96 yd. World entry (WorldScreen::loadDoodads) uses the same radius.
//   the memory budget:
//   streamjobkb=3072  KB of models (m_ + k_) one job reads at most, nearest the player first; the rest of the tile
//                     is deferred and read by top-up jobs (0 = the whole tile, as before)
//   tilereserve=1536  KB the heap must still give while a job's models are placed (else deferred, freed)
//   terrainidle=5000  ms without a read before the terrain pak's descriptor goes back (0 = held, as before)
//   Log: tile_load ... topup= deferred=, stream_terrain_fd, stream_frame ... deferred= topups=.
//   the terrain's chunk-texture window
//   (TerrainRenderer::texWindow, psp_terrain_window.hpp; WorldScreen::loadTerrain reads the keys below):
//   terraintex=1      1 = a tile job reads the tile's head (87 KB) and v4 detail table, the chunk textures come in
//                     per chunk row within terraintexyd of the player and go past it + terraintexband;
//                     0 = the whole 1.6 MB blob per tile, every texture resident (as before)
//   terraintexyd=0    yd (0 = auto: the far plane 450 + camera 15 + lead 64 = 529)
//   terraintexband=48 yd of hysteresis before a texture goes
//   terraintexreads=6 texture runs (one chunk row each) in flight at most
//   Log: terrain_tex (stream_frame cadence), terrain_tex_fail.
//
// Log (event-driven): stream_start, stream_queue n= next=, tile_load x= y= ms= kb=
// t= d= reads= miss= inst_us= heap_kb=, tile_evict x= y= kb= heap_kb=, tile_absent,
// stream_lowmem, stream_vram kb=, stream_read_fail; summary stream_frame every 10 s.
// Profiler zones: stream, stream.read, stream.install.
#include "ui/psp/ui_world_screen.hpp"
#include "ui/psp/ui_world_streaming.hpp"

#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_models.hpp"
#include "rendering/psp/psp_terrain.hpp"
#include "rendering/psp/psp_tile_budget.hpp"  // nearest first

#include <pspiofilemgr.h>
#include <pspkerneltypes.h>
#include <pspthreadman.h>
#include <psputils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {

constexpr float kTile = 533.33333f;       // upstream core::coords::TILE_SIZE
// (loader read pieces: pp::pakRead, 64 KB under the shared card lock)
constexpr uint32_t kVramStepBytes = 128 * 1024;
constexpr float kDoodadKeepYd = 96.0f;  // doodads are dropped past doodadYd_ + this (hysteresis)
constexpr float kDoodadCheckYd = 32.0f;  // the radius is checked again every 32 yd of movement
constexpr uint32_t kTerrainBlockKb = 2048;  // one tile's terrain blob + vertex block, in one piece
constexpr uint32_t kTerrainHeadBlockKb = 512;  // with the texture window - vertices 292 + head 85

uint32_t tileKey(int x, int y) { return (static_cast<uint32_t>(x & 0xFFFF) << 16) | static_cast<uint32_t>(y & 0xFFFF); }
int keyX(uint32_t k) { return static_cast<int16_t>(k >> 16); }
int keyY(uint32_t k) { return static_cast<int16_t>(k & 0xFFFF); }

uint32_t nowUs() { return sceKernelGetSystemTimeLow(); }
uint32_t heapKb() { return pp::heapUsedKb(); }  // both pools of the big-block heap

// the map's directory is worldMapDir (ui_world_screen.cpp: Map.dbc, every map) - was a
// four-continent table here.

inline void barrier() { __sync_synchronize(); }

// tile x_y's static spawn record ("s_<map>_<x>_<y>", our server's pak
// baked with --spawns) looked up once and remembered in `entries` (TileStreamer::SpawnEntry; size 0 = none).
template <typename Entries>
bool spawnLookup(rp::CreatureRenderer& creatures, uint32_t mapId, int x, int y, Entries& entries) {
    for (const auto& s : entries)
        if (s.x == x && s.y == y) return s.size != 0;
    uint32_t offset = 0, size = 0;
    if (!creatures.isOpen() || !creatures.spawnEntry(mapId, x, y, offset, size)) size = 0;
    entries.push_back({x, y, offset, size});
    return size != 0;
}

} // namespace

// ---------------------------------------------------------------------------
// StreamLoader

bool StreamLoader::start(bool useThread, int priority) {
    stop();
    quit_ = false;
    if (!useThread) return true;
    sema_ = sceKernelCreateSema("wowee_stream_sema", 0, 0, static_cast<int>(kRing), nullptr);
    if (sema_ < 0) {
        sema_ = -1;
        return false;
    }
    thread_ = sceKernelCreateThread("wowee_stream", &StreamLoader::threadMain, priority, 16 * 1024,
                                    PSP_THREAD_ATTR_USER, nullptr);
    if (thread_ < 0) {
        thread_ = -1;
        sceKernelDeleteSema(sema_);
        sema_ = -1;
        return false;
    }
    StreamLoader* self = this;
    if (sceKernelStartThread(thread_, sizeof(self), &self) < 0) {
        sceKernelDeleteThread(thread_);
        thread_ = -1;
        sceKernelDeleteSema(sema_);
        sema_ = -1;
        return false;
    }
    return true;
}

void StreamLoader::stop() {
    if (thread_ >= 0) {
        quit_ = true;
        barrier();
        sceKernelSignalSema(sema_, 1);
        SceUInt timeout = 3000000;  // a read in flight finishes first
        if (sceKernelWaitThreadEnd(thread_, &timeout) < 0) {
            sceKernelTerminateDeleteThread(thread_);
        } else {
            sceKernelDeleteThread(thread_);
        }
        thread_ = -1;
    }
    if (sema_ >= 0) {
        sceKernelDeleteSema(sema_);
        sema_ = -1;
    }
    Result r;
    while (pop(r))
        if (r.blob != r.req.dst || r.req.freeDst) std::free(r.blob);  // a dst buffer is the caller's
    for (uint32_t i = reqTail_; i != reqHead_; ++i)  // requests never read give their owned dst back
        if (reqs_[i % kRing].freeDst) std::free(reqs_[i % kRing].dst);
    reqHead_ = 0;
    reqTail_ = 0;
    resHead_ = 0;
    resTail_ = 0;
}

bool StreamLoader::push(const Request& r) {
    if (reqHead_ - resTail_ >= kRing) return false;
    reqs_[reqHead_ % kRing] = r;
    barrier();
    reqHead_ = reqHead_ + 1;
    if (thread_ >= 0) sceKernelSignalSema(sema_, 1);
    return true;
}

bool StreamLoader::pop(Result& out) {
    if (resTail_ == resHead_) return false;
    barrier();
    out = results_[resTail_ % kRing];
    barrier();
    resTail_ = resTail_ + 1;
    return true;
}

bool StreamLoader::pumpSync() { return thread_ < 0 && readOne(); }

int StreamLoader::threadMain(unsigned int /*args*/, void* argp) {
    StreamLoader* self = *static_cast<StreamLoader**>(argp);
    self->run();
    return 0;
}

void StreamLoader::run() {
    for (;;) {
        sceKernelWaitSema(sema_, 1, nullptr);
        barrier();
        if (quit_) break;
        readOne();
    }
}

// Loader side (thread, or the main thread in sync mode): no logging here.
bool StreamLoader::readOne() {
    if (reqTail_ == reqHead_) return false;
    barrier();
    const Request r = reqs_[reqTail_ % kRing];
    barrier();
    reqTail_ = reqTail_ + 1;
    const uint32_t alloc = (r.size + 63u) & ~63u;
    // into the caller's buffer when it gave one (the terrain pool) - no heap block.
    auto* block = static_cast<uint8_t*>(r.dst ? r.dst : memalign(64, alloc ? alloc : 64u));
    // Shared pak descriptors: the locked, chunked pak reader.
    const bool ok = block != nullptr && r.fd >= 0 && pp::pakRead(r.fd, r.offset, block, r.size);
    if (!ok) {
        if (block != r.dst || r.freeDst) std::free(block);  // an owned dst goes back with the failed read
        block = nullptr;
    } else {
        sceKernelDcacheWritebackRange(block, r.dst ? r.size : alloc);  // like Pak::load: the GE may read it at once
    }
    Result& res = results_[resHead_ % kRing];
    res.req = r;
    res.blob = block;
    barrier();
    resHead_ = resHead_ + 1;
    return true;
}

// ---------------------------------------------------------------------------
// TileStreamer

TileStreamer::~TileStreamer() {
    loader_.stop();
    for (const StreamLoader::Request& r : backlog_)  // never pushed - an owned dst is still ours
        if (r.freeDst) std::free(r.dst);
    backlog_.clear();
    for (void* p : runsOut_) rp::TerrainRenderer::poolRunFree(p);  // the pool outlives the streamer
    runsOut_.clear();
    if (terrainPak_.isOpen()) pp::pakPrefetchDrop(terrainPak_.fileId());
    if (doodadPak_.isOpen()) pp::pakPrefetchDrop(doodadPak_.fileId());
    if (creatureFileId_) pp::pakPrefetchDrop(creatureFileId_);
    // The descriptors belong to the paks (shared): not closed here.
}

TileStreamer::TileRec* TileStreamer::findTile(int x, int y) {
    for (TileRec& t : tiles_)
        if (t.x == x && t.y == y) return &t;
    return nullptr;
}

bool TileStreamer::inWindow(int x, int y) const {
    return std::abs(x - curX_) <= radius_ && std::abs(y - curY_) <= radius_;
}

// Chebyshev yards from the player to the tile's square. Tile (x, y) covers
// server y in [(31 - x), (32 - x)] * kTile and server x in [(31 - y), (32 - y)] * kTile
// (upstream: tileX = 32 - y / TILE_SIZE, tileY = 32 - x / TILE_SIZE).
float TileStreamer::yardsTo(int x, int y, float px, float py) const {
    const float xLo = (31 - y) * kTile, xHi = (32 - y) * kTile;
    const float yLo = (31 - x) * kTile, yHi = (32 - x) * kTile;
    const float dx = std::max({0.0f, xLo - px, px - xHi});
    const float dy = std::max({0.0f, yLo - py, py - yHi});
    return std::max(dx, dy);
}

bool TileStreamer::wantDoodads(int x, int y, float px, float py) const {
    return doodadYd_ <= 0.0f || yardsTo(x, y, px, py) <= doodadYd_;
}

// A tile already in (its terrain) whose doodads are wanted now and not loaded.
bool TileStreamer::needsDoodads(const TileRec& t, float px, float py) const {
    if (t.doodads || !doodadPak_.isOpen() || !wantDoodads(t.x, t.y, px, py)) return false;
    char name[24];
    std::snprintf(name, sizeof(name), "d_%d_%d", t.x, t.y);
    return doodadPak_.find(name) != nullptr;
}

// A tile in whose terrain could not be loaded for want of memory (or a failed read) - its retry is due.
bool TileStreamer::needsTerrain(const TileRec& t) const {
    if (t.terrain || !t.terrainRetry || !terrainPak_.isOpen()) return false;
    return static_cast<int32_t>(pp::logUptimeMs() - t.terrainRetryMs) >= 0;
}

// Upstream streamTiles: every tile of the window that is not loaded, pending
// or failed, nearest first; and the tiles in whose doodads are wanted now.
void TileStreamer::computeQueue(float px, float py) {
    queue_.clear();
    for (int dx = -radius_; dx <= radius_; ++dx) {
        for (int dy = -radius_; dy <= radius_; ++dy) {
            const int x = curX_ + dx, y = curY_ + dy;
            if (x < 0 || x > 63 || y < 0 || y > 63) continue;
            const uint32_t k = tileKey(x, y);
            const TileRec* have = findTile(x, y);
            if ((have && !needsDoodads(*have, px, py) && !needsTerrain(*have)) ||
                std::find(absent_.begin(), absent_.end(), k) != absent_.end())
                continue;
            if (job_.active && job_.x == x && job_.y == y) continue;
            queue_.push_back(k);
        }
    }
    auto dist2 = [&](uint32_t k) {
        const float cx = (31.5f - keyY(k)) * kTile - px, cy = (31.5f - keyX(k)) * kTile - py;
        return cx * cx + cy * cy;
    };
    std::sort(queue_.begin(), queue_.end(), [&](uint32_t a, uint32_t b) { return dist2(a) < dist2(b); });
    if (!queue_.empty())
        PSP_LOGI(pp::tag::Gfx, "stream_queue n=%u tile=%d_%d next=%d_%d", static_cast<unsigned>(queue_.size()), curX_,
                 curY_, keyX(queue_.front()), keyY(queue_.front()));
}

void TileStreamer::request(const pp::Pak& pak, int fd, const char* name, uint8_t kind, uint32_t id, uint32_t key) {
    const pp::PakEntry* e = pak.find(name);
    if (!e) return;
    StreamLoader::Request r;
    r.fd = fd;
    r.fileId = pak.fileId();
    r.offset = e->offset;
    r.size = e->size;
    r.kind = kind;
    r.id = id;
    r.key = key;
    if (kind != kTerrain && kind != kDetail) {
        // A doodad blob (placements, model, collision, texture, impostor) read into the world arena
        // (it lives as long as its tile; parked, then owned by the model - free() finds the arena by address).
        r.dst = pp::worldArenaAlloc((e->size + 63u) & ~63u, 64);
        r.freeDst = r.dst != nullptr;
    }
    if (kind == kTerrain || kind == kDetail) ++job_.pendingTerrain;
    else ++job_.pendingDoodads;
    if (kind == kModel) job_.models.push_back(Job::ModelRef{static_cast<uint16_t>(id), e->offset, e->size});
    if (kind == kImpostor) job_.impostors.push_back(Job::ModelRef{static_cast<uint16_t>(id), e->offset, e->size});
    if (kind == kDoodads) {
        job_.doodadOffset = e->offset;
        job_.doodadSize = e->size;
    }
    if (backlog_.empty() && loader_.push(r)) return;
    backlog_.push_back(r);
}

// A slice of a pak file - a windowed tile's head (kTerrain), its detail table (kTerrainTail), a
// texture run with the job (kTerrainTex) or for the window (kChunkTex, no job).
void TileStreamer::requestRaw(const pp::Pak& pak, int fd, uint32_t offset, uint32_t size, uint8_t kind, uint32_t id,
                              uint32_t key, void* dst) {
    StreamLoader::Request r;
    r.dst = dst;
    r.fd = fd;
    r.fileId = pak.fileId();
    r.offset = offset;
    r.size = size;
    r.kind = kind;
    r.id = id;
    r.key = key;
    if (kind == kTerrain || kind == kTerrainTail || kind == kTerrainTex) ++job_.pendingTerrain;
    if (kind == kChunkTex) ++texInFlight_;
    if (backlog_.empty() && loader_.push(r)) return;
    backlog_.push_back(r);
}

bool TileStreamer::requestSpawns(int x, int y, uint32_t key) {
    for (const SpawnEntry& s : spawnEntries_) {
        if (s.x != x || s.y != y || !s.size || creatureFd_ < 0) continue;  // size 0 = none
        StreamLoader::Request r;
        r.fd = creatureFd_;
        r.fileId = creatureFileId_;
        r.offset = s.offset;
        r.size = s.size;
        r.kind = kSpawns;
        r.key = key;
        ++spawnsPending_;
        if (!backlog_.empty() || !loader_.push(r)) backlog_.push_back(r);
        return true;
    }
    return false;
}

// "d_" arrived (or a rescan): its models the renderer does not know yet, with
// their collision meshes. Placements de-duplicated by a neighbour may ask for
// a model it already has - modelKnown() skips those.
void TileStreamer::onDoodads(Env& env, const void* blobPtr, uint32_t size) {
    const auto* blob = static_cast<const uint8_t*>(blobPtr);
    const auto* h = reinterpret_cast<const rp::DoodadTileHeader*>(blob);
    if (!blob || size < sizeof(rp::DoodadTileHeader) || std::memcmp(h->magic, "WDOD", 4) != 0 ||
        h->version != rp::kModelFormatVersion || sizeof(rp::DoodadTileHeader) + h->count * sizeof(rp::DoodadPlacement) > size)
        return;  // loadTile rejects it too (and logs wdod_bad)
    const auto* p = reinterpret_cast<const rp::DoodadPlacement*>(blob + sizeof(rp::DoodadTileHeader));
    const uint32_t key = tileKey(job_.x, job_.y);
    // nearest the player first, at most jobKb_ of models a job - the install defers the rest
    // (ModelRenderer::LoadBudget parkedOnly), top-up jobs read it when there is room (was: every model of the tile).
    std::vector<uint32_t> order;
    rp::orderNearest(p, h->count, env.x, env.y, order);
    for (uint32_t i : order) {
        const uint16_t id = p[i].model;
        if (env.models.modelKnown(id)) continue;
        bool asked = false;
        for (const Job::ModelRef& m : job_.models) asked = asked || m.id == id;
        if (asked) continue;
        if (jobKb_ && !job_.models.empty() && job_.modelBytes >= jobKb_ * 1024u) break;
        requestModel(env, id, key);
    }
}

// "m_" and "k_" of a model for the job (onModel asks its textures and impostor).
void TileStreamer::requestModel(Env& env, uint16_t id, uint32_t key) {
    (void)env;
    char name[16];
    std::snprintf(name, sizeof(name), "m_%u", static_cast<unsigned>(id));
    const size_t before = job_.models.size();
    request(doodadPak_, doodadFd_, name, kModel, id, key);
    if (job_.models.size() == before) return;  // not in the pak: model() fails the same way
    job_.modelBytes += job_.models.back().size;
    std::snprintf(name, sizeof(name), "k_%u", static_cast<unsigned>(id));
    if (const pp::PakEntry* e = doodadPak_.find(name)) job_.modelBytes += e->size;
    request(doodadPak_, doodadFd_, name, kCollision, id, key);
}

// the install's budget - the player's place, tilereserve, only what the loader parked.
rp::ModelRenderer::LoadBudget TileStreamer::installBudget(const Env& env) const {
    rp::ModelRenderer::LoadBudget b;
    b.x = env.x;
    b.y = env.y;
    b.reserveKb = tileReserveKb_;
    b.parkedOnly = true;
    b.nearRule = job_.nearRule;  // doodads in view may use the guard block
    return b;
}

// the terrain pak's descriptor - given back while nothing is read (terrainidle), taken again
// before a job (a rare reader: the card has ~10 descriptors, 3-4 free at world entry).
bool TileStreamer::ensureTerrainFd() {
    if (terrainFd_ >= 0) return true;
    if (!terrainPak_.ensureFd()) {
        if (++fdWaitLogs_ <= 20) PSP_LOGW(pp::tag::Fs, "stream_terrain_fd wait (no descriptor) open=%d", pp::pakOpenFds());
        return false;
    }
    terrainFd_ = terrainPak_.fd();
    return true;
}

void TileStreamer::releaseTerrainFdIfIdle() {
    if (!terrainIdleMs_ || terrainFd_ < 0 || job_.active || !backlog_.empty() || loader_.inFlight() != 0) return;
    if (pp::logUptimeMs() - lastTerrainReqMs_ < terrainIdleMs_) return;
    terrainPak_.releaseFd();
    terrainFd_ = -1;
    if (++terrainReleases_ <= 20)
        PSP_LOGI(pp::tag::Fs, "stream_terrain_fd released n=%u open=%d", static_cast<unsigned>(terrainReleases_),
                 pp::pakOpenFds());
}

// a top-up job - the nearest tile whose doodads are in but some deferred: their models (nearest
// first, jobKb_), placed by resumeTile. One a second at most; only with nothing cut by the memory governor and
// streamreserve KB allocatable (memoryOk).
bool TileStreamer::stepTopUp(Env& env) {
    if (!topUpOk_ || !doodadPak_.isOpen() || job_.active) return false;
    const uint32_t now = pp::logUptimeMs();
    if (static_cast<int32_t>(now - topUpNextMs_) < 0) return false;
    // (Seen on the emulator:) a tile with deferred placements the player can SEE first (by the near
    // rule: tilereserve KB of room, the guard block may be used - as the NPCs next to the player), then the nearest
    // with any (by the streamer's rule: streamreserve besides the guard). What stays deferred is out of sight.
    TileRec* best = nullptr;
    float bestYd = 0.0f;
    bool bestInView = false;
    for (TileRec& t : tiles_) {
        if (!t.doodads || !t.deferred || !wantDoodads(t.x, t.y, env.x, env.y)) continue;
        if (!env.models.deferredCount(t.x, t.y)) {
            t.deferred = false;
            continue;
        }
        const float yd = yardsTo(t.x, t.y, env.x, env.y);
        const bool inView = env.models.deferredInView(t.x, t.y, env.x, env.y) > 0;
        if (!best || (inView && !bestInView) || (inView == bestInView && yd < bestYd)) {
            best = &t;
            bestYd = yd;
            bestInView = inView;
        }
    }
    if (!best) return stepTexRetry(env, now);  // nothing deferred - missing textures, if any
    topUpNextMs_ = now + 1000u;
    if (!memoryOk(false, bestInView)) return false;
    job_ = Job{};
    job_.active = true;
    job_.x = best->x;
    job_.y = best->y;
    job_.startMs = now;
    job_.topUp = true;
    job_.nearRule = bestInView;  // its install places by the near rule too
    job_.wantDoodads = true;
    job_.deferredBefore = env.models.deferredCount(best->x, best->y);
    std::vector<uint16_t> ids;
    env.models.deferredModels(doodadPak_, best->x, best->y, env.x, env.y, jobKb_ ? jobKb_ * 1024u : 0xFFFFFFFFu, ids);
    const uint32_t key = tileKey(best->x, best->y);
    for (uint16_t id : ids) requestModel(env, id, key);
    ++topUps_;
    return true;
}

// (Black squares on the emulator in a village:) the textures drawn batches found missing (their
// read had no memory) - a top-up job reads them (into the world arena, parked) and its install loads them
// (ModelRenderer::retryTextures). Under the top-up's rules: one a second at most, with streamreserve KB of room.
bool TileStreamer::stepTexRetry(Env& env, uint32_t now) {
    if (!env.models.texMissingPending()) return false;
    TileRec* best = nullptr;  // the job's tile: the nearest with doodads in (the key only - no tile changes)
    float bestYd = 0.0f;
    for (TileRec& t : tiles_) {
        if (!t.doodads) continue;
        const float yd = yardsTo(t.x, t.y, env.x, env.y);
        if (!best || yd < bestYd) {
            best = &t;
            bestYd = yd;
        }
    }
    if (!best) return false;
    topUpNextMs_ = now + 1000u;
    if (!memoryOk(false, true)) return false;  // a drawn batch lacks it: on the screen (the near rule)
    std::vector<uint16_t> ids;
    env.models.takeMissingTextures(ids, 24);
    if (ids.empty()) return false;
    job_ = Job{};
    job_.active = true;
    job_.x = best->x;
    job_.y = best->y;
    job_.startMs = now;
    job_.topUp = true;
    job_.texRetry = true;
    job_.wantDoodads = true;
    job_.deferredBefore = env.models.deferredCount(best->x, best->y);
    const uint32_t key = tileKey(best->x, best->y);
    char name[16];
    for (uint16_t t : ids) {
        if (std::find(job_.textures.begin(), job_.textures.end(), t) != job_.textures.end()) continue;
        job_.textures.push_back(t);
        std::snprintf(name, sizeof(name), "x_%u", static_cast<unsigned>(t));
        request(doodadPak_, doodadFd_, name, kTexture, t, key);
    }
    ++texRetryJobs_;
    if (texRetryJobs_ <= 40)
        PSP_LOGI(pp::tag::Mem, "tex_retry n=%u tile=%d_%d jobs=%u free_kb=%u largest_kb=%u",
                 static_cast<unsigned>(job_.textures.size()), job_.x, job_.y, static_cast<unsigned>(texRetryJobs_),
                 static_cast<unsigned>(pp::heapFreeKb()), static_cast<unsigned>(pp::heapLargestKb()));
    return true;
}

// "m_" arrived (or a rescan): the textures of every batch, as model() loads them.
void TileStreamer::onModel(Env& env, uint16_t id, const void* blobPtr, uint32_t size) {
    const auto* blob = static_cast<const uint8_t*>(blobPtr);
    const auto* h = reinterpret_cast<const rp::ModelHeader*>(blob);
    if (!blob || size < sizeof(rp::ModelHeader) || std::memcmp(h->magic, "WMDL", 4) != 0 ||
        h->version != rp::kModelFormatVersion)
        return;
    const uint32_t batches = static_cast<uint32_t>(h->batchCount[0]) + h->batchCount[1] +
                             ((h->flags & rp::kModelLod2) ? h->lod2BatchCount : 0u);  // far LOD
    if (h->batchOffset + batches * sizeof(rp::ModelBatch) > size) return;
    const auto* b = reinterpret_cast<const rp::ModelBatch*>(blob + h->batchOffset);
    const uint32_t key = tileKey(job_.x, job_.y);
    char name[16];
    for (uint32_t i = 0; i < batches; ++i) {
        const uint16_t t = b[i].texture;
        if (t == rp::kNoTexture || env.models.textureKnown(t)) continue;
        if (std::find(job_.textures.begin(), job_.textures.end(), t) != job_.textures.end()) continue;
        job_.textures.push_back(t);
        std::snprintf(name, sizeof(name), "x_%u", static_cast<unsigned>(t));
        request(doodadPak_, doodadFd_, name, kTexture, t, key);
    }
    // a tree's impostor "i_<n>" (ModelRenderer::loadImpostor
    // reads it at install; without this it would be a synchronous card read).
    if (h->flags & rp::kModelFoliage) {
        for (const Job::ModelRef& m : job_.impostors)
            if (m.id == id) return;
        std::snprintf(name, sizeof(name), "i_%u", static_cast<unsigned>(id));
        request(doodadPak_, doodadFd_, name, kImpostor, id, key);
    }
}

// "i_" arrived (or a rescan): its atlas "x_<n>".
void TileStreamer::onImpostor(Env& env, const void* blobPtr, uint32_t size) {
    const auto* h = static_cast<const rp::ImpostorHeader*>(blobPtr);
    if (!blobPtr || size < sizeof(rp::ImpostorHeader) || std::memcmp(h->magic, "WIMP", 4) != 0) return;
    const uint16_t t = h->texture;
    if (env.models.textureKnown(t)) return;
    if (std::find(job_.textures.begin(), job_.textures.end(), t) != job_.textures.end()) return;
    job_.textures.push_back(t);
    char name[16];
    std::snprintf(name, sizeof(name), "x_%u", static_cast<unsigned>(t));
    request(doodadPak_, doodadFd_, name, kTexture, t, tileKey(job_.x, job_.y));
}

// An eviction freed models/textures the job's tile may need: ask again for
// what is no longer known (from the blobs parked so far).
void TileStreamer::rescanJob(Env& env) {
    if (!job_.active || !job_.wantDoodads || job_.doodadsDone) return;
    const uint32_t fid = doodadPak_.fileId();
    if (const void* d = pp::pakPrefetchPeek(fid, job_.doodadOffset)) onDoodads(env, d, job_.doodadSize);
    for (size_t i = 0; i < job_.models.size(); ++i) {
        const Job::ModelRef m = job_.models[i];  // onModel never grows models, but copy anyway
        if (const void* b = pp::pakPrefetchPeek(fid, m.offset)) onModel(env, m.id, b, m.size);
    }
    for (size_t i = 0; i < job_.impostors.size(); ++i) {
        const Job::ModelRef m = job_.impostors[i];
        if (const void* b = pp::pakPrefetchPeek(fid, m.offset)) onImpostor(env, b, m.size);
    }
}

void TileStreamer::drainResults(Env& env) {
    while (!backlog_.empty() && loader_.push(backlog_.front())) backlog_.pop_front();
    StreamLoader::Result r;
    while (loader_.pop(r)) {
        const StreamLoader::Request& q = r.req;
        if (q.kind == kChunkTex) {  // a window run - installed now, never a job's
            if (texInFlight_) --texInFlight_;
            const uint16_t first = static_cast<uint16_t>(q.id & 0xFFFFu), count = static_cast<uint16_t>(q.id >> 16);
            bool short_ = false;
            if (r.blob) {
                ++texReads_;
                const uint32_t nomem0 = env.terrain.texNomemCount();
                env.terrain.texInstall(keyX(q.key), keyY(q.key), first, count, r.blob, q.size);
                short_ = env.terrain.texNomemCount() != nomem0;  // a chunk found no 6 KB block
            } else {
                ++texReadFails_;  // no block for the run (the loader's memalign) or a failed read
                short_ = true;
                env.terrain.texCancel(keyX(q.key), keyY(q.key), first, count);
                if (++texLogs_ <= 20)
                    PSP_LOGW(pp::tag::Mem, "terrain_tex_fail tile=%d_%d first=%u n=%u size=%u heap_kb=%u free_kb=%u",
                             keyX(q.key), keyY(q.key), static_cast<unsigned>(first), static_cast<unsigned>(count),
                             static_cast<unsigned>(q.size), static_cast<unsigned>(heapKb()),
                             static_cast<unsigned>(pp::heapFreeKb()));
            }
            if (q.dst) {  // the pool's row buffer goes back
                rp::TerrainRenderer::poolRunFree(q.dst);
                runsOut_.erase(std::remove(runsOut_.begin(), runsOut_.end(), q.dst), runsOut_.end());
            } else {
                std::free(r.blob);
            }
            if (short_) {
                // The ground is short of memory: the governor frees lower priorities, the window waits a second
                // (no read-and-fail every frame).
                terrainStarved_ = true;
                texBackoffMs_ = pp::logUptimeMs() + 1000u;
            } else {
                texDirty_ = true;
            }
            continue;
        }
        const bool forJob = job_.active && q.key == tileKey(job_.x, job_.y) && q.kind != kSpawns;
        if (forJob) {
            ++job_.reads;
            job_.bytes += q.size;
        }
        if (!r.blob && ++failLogs_ <= 20)
            PSP_LOGW(pp::tag::Fs, "stream_read_fail kind=%u id=%u tile=%d_%d size=%u", static_cast<unsigned>(q.kind),
                     static_cast<unsigned>(q.id), keyX(q.key), keyY(q.key), static_cast<unsigned>(q.size));
        if (q.kind == kSpawns) {
            if (spawnsPending_) --spawnsPending_;
            TileRec* rec = findTile(keyX(q.key), keyY(q.key));
            if (!r.blob || !rec || !env.staticNpcs || !npcStatic_) {
                std::free(r.blob);
                if (rec) rec->spawnsKnown = true;
                continue;
            }
            // CreatureRenderer::appendStaticSpawns reads it through its own
            // Pak on the same file: the parked blob, no card access.
            pp::pakPrefetchPut(q.fileId, q.offset, r.blob);
            rec->spawns.clear();
            env.creatures.appendStaticSpawns(env.mapId, rec->x, rec->y, rec->spawns);
            if (void* left = pp::pakPrefetchTake(q.fileId, q.offset)) std::free(left);
            rec->spawnsKnown = true;
            spawnsDirty_ = true;
            continue;
        }
        if (!forJob) {  // a job that no longer exists (cannot happen: jobs finish after their reads)
            if (r.blob != q.dst || q.freeDst) std::free(r.blob);
            continue;
        }
        // A terrain-pool read (dst, not owned) is not parked; an owned dst (the world arena) is a blob as any.
        if (r.blob && (!q.dst || q.freeDst)) pp::pakPrefetchPut(q.fileId, q.offset, r.blob);
        switch (q.kind) {
            case kTerrain: {
                if (r.blob && env.terrain.texWindow) {
                    // The head - the detail table (v4) and the texture runs the window wants now
                    // (a tile near the player: a teleport) are read with the job.
                    rp::TerrainLayout layout;
                    const auto* head = static_cast<const uint8_t*>(r.blob);
                    if (q.dst) env.terrain.headStaged(job_.x, job_.y);  // in the pool's head buffer (no park)
                    if (rp::terrainLayout(head, q.size, job_.terrainSize, layout) && layout.detailOffset) {
                        // The v4 detail table, after the head in the same buffer (with the pool). The chunk textures
                        // come with the window's reads right after the install (texDirty_), nearest first.
                        uint8_t* tableDst = q.dst ? static_cast<uint8_t*>(q.dst) + rp::kTerrainHeadBytes : nullptr;
                        requestRaw(terrainPak_, terrainFd_, job_.terrainOffset + layout.detailOffset,
                                   rp::kTerrainDetailTableBytes, kTerrainTail, 0, q.key, tableDst);
                    }
                } else if (r.blob) {
                    std::vector<uint16_t> need;
                    env.terrain.missingDetails(r.blob, q.size, need);
                    char name[16];
                    for (uint16_t id : need) {
                        if (std::find(job_.details.begin(), job_.details.end(), id) != job_.details.end()) continue;
                        job_.details.push_back(id);
                        std::snprintf(name, sizeof(name), "dt_%u", static_cast<unsigned>(id));
                        request(terrainPak_, terrainFd_, name, kDetail, id, q.key);
                    }
                }
                --job_.pendingTerrain;
                break;
            }
            case kDetail: --job_.pendingTerrain; break;
            case kTerrainTail: {  // a windowed tile's v4 detail table - its "dt_" not resident yet
                if (r.blob && q.dst) env.terrain.tableStaged();
                if (r.blob) {
                    std::vector<uint16_t> need;
                    env.terrain.missingDetailsTable(static_cast<const uint8_t*>(r.blob), need);
                    char name[16];
                    for (uint16_t id : need) {
                        if (std::find(job_.details.begin(), job_.details.end(), id) != job_.details.end()) continue;
                        job_.details.push_back(id);
                        std::snprintf(name, sizeof(name), "dt_%u", static_cast<unsigned>(id));
                        request(terrainPak_, terrainFd_, name, kDetail, id, q.key);
                    }
                }
                --job_.pendingTerrain;
                break;
            }
            case kTerrainTex: --job_.pendingTerrain; break;  // parked, adopted by the install
            case kDoodads:
                if (r.blob) onDoodads(env, r.blob, q.size);
                --job_.pendingDoodads;
                break;
            case kModel:
                if (r.blob) onModel(env, static_cast<uint16_t>(q.id), r.blob, q.size);
                --job_.pendingDoodads;
                break;
            case kImpostor:
                if (r.blob) onImpostor(env, r.blob, q.size);
                --job_.pendingDoodads;
                break;
            default: --job_.pendingDoodads; break;  // collision, texture
        }
    }
    while (!backlog_.empty() && loader_.push(backlog_.front())) backlog_.pop_front();
}

// A tile's terrain - the ground under the player, the highest priority - needs one
// block of terrainNeedKb + terrainreserve KB in total; when it cannot start, the governor hears of it
// (takeTerrainStarved) and frees lower priorities. Doodads (a tile in, a top-up) need streamreserve KB of headroom
// (pp::heapGivesKb: total free + one block of min(n, 1 MB); the probes write their block - gcc drops a malloc/free pair
// it can see through, and the gate then always said "yes": the hardware OOM of builds 74-77).
bool TileStreamer::memoryOk(bool terrain, bool nearDoodads) {
    if (terrain && rp::TerrainRenderer::pooled()) {
        // The terrain pool - a free tile slot, no heap (all in use: the caller evicts the tiles
        // outside the window first; the governor cannot help, so the ground does not "starve" here).
        if (rp::TerrainRenderer::poolTilesFree() > 0) {
            terrainSlotsReported_ = false;
            return true;
        }
        if (!terrainSlotsReported_) {
            terrainSlotsReported_ = true;
            PSP_LOGW(pp::tag::Mem, "stream_terrain_slots free=0 tiles=%u (outside the window evicted first)",
                     static_cast<unsigned>(tiles_.size()));
        }
        return false;
    }
    if (terrain) {
        if (pp::heapRoomFor(terrainNeedKb() * 1024u, terrainReserveKb_)) {
            terrainLowReported_ = false;
            return true;
        }
        terrainStarved_ = true;
        if (!terrainLowReported_) {
            terrainLowReported_ = true;
            PSP_LOGW(pp::tag::Mem, "stream_terrain_lowmem need_kb=%u reserve_kb=%u free_kb=%u largest_kb=%u tiles=%u",
                     static_cast<unsigned>(terrainNeedKb()), static_cast<unsigned>(terrainReserveKb_),
                     static_cast<unsigned>(pp::heapFreeKb()), static_cast<unsigned>(pp::heapLargestKb()),
                     static_cast<unsigned>(tiles_.size()));
        }
        return false;
    }
    if (!reserveKb_) return true;
    // (Seen on the emulator: a village kept 2 tiles' placements deferred with 6.6 MB free - the rule
    // below asks 8 MB): doodads the player can see (a tile within the view distance, a deferred placement in view, a
    // missing texture) need tilereserve KB of room + one block, and may use the guard block (as the NPCs next to him).
    if (nearDoodads && pp::heapGivesKb(tileReserveKb_ + 512u)) {
        lowMemReported_ = false;
        return true;
    }
    // (Seen on the emulator: doodads filled the heap to 1.3 MB free and shredded it): streamreserve KB
    // of headroom BESIDES the guard block (heapguardkb, one block) - the NPCs / big WMO chunks may use the guard.
    if (pp::heapGivesKb(reserveKb_ + pp::heapGuardKb()) && pp::heapGuardOk()) {
        lowMemReported_ = false;
        return true;
    }
    if (!lowMemReported_) {
        lowMemReported_ = true;
        PSP_LOGW(pp::tag::Mem, "stream_lowmem reserve_kb=%u heap_kb=%u tiles=%u", static_cast<unsigned>(reserveKb_),
                 static_cast<unsigned>(heapKb()), static_cast<unsigned>(tiles_.size()));
        pp::bigheapLogPools();  // which pool lacks the block
    }
    return false;
}

void TileStreamer::startJob(Env& env, int x, int y, bool doodadsOk) {
    job_ = Job{};
    job_.active = true;
    job_.x = x;
    job_.y = y;
    job_.startMs = pp::logUptimeMs();
    job_.nearRule = yardsTo(x, y, env.x, env.y) <= env.models.viewDistance;  // a tile in view
    const uint32_t key = tileKey(x, y);
    char name[32];
    std::snprintf(name, sizeof(name), "t_%d_%d", x, y);
    if (terrainPak_.isOpen() && terrainPak_.find(name) && !env.terrain.hasTile(x, y)) {
        job_.wantTerrain = true;
        lastTerrainReqMs_ = pp::logUptimeMs();  // the descriptor is in use
        const pp::PakEntry* e = terrainPak_.find(name);
        job_.terrainOffset = e->offset;  // the entry (the window reads slices of it)
        job_.terrainSize = e->size;
        if (env.terrain.texWindow) {
            // The head only (header + chunk records, 87 KB of the 1.6 MB) - into the terrain pool's
            // head buffer (one job at a time); the detail table is asked for when it arrives (drainResults).
            requestRaw(terrainPak_, terrainFd_, e->offset, std::min<uint32_t>(e->size, rp::kTerrainHeadBytes), kTerrain,
                       0, key, rp::TerrainRenderer::poolHead());
        } else {
            request(terrainPak_, terrainFd_, name, kTerrain, 0, key);
        }
    }
    std::snprintf(name, sizeof(name), "d_%d_%d", x, y);
    // A terrain job started on the ground's margin reads no doodads now (they would take the room
    // the ground was given) - the tile's doodads are asked again (needsDoodads) when streamreserve is there.
    if (doodadsOk && doodadPak_.isOpen() && doodadPak_.find(name) && !env.models.hasTile(x, y) &&
        wantDoodads(x, y, env.x, env.y)) {
        job_.wantDoodads = true;
        request(doodadPak_, doodadFd_, name, kDoodads, 0, key);
    }
    if (findTile(x, y)) {  // a tile already in - only its doodads (needsDoodads)
        if (!job_.wantTerrain && !job_.wantDoodads) job_.active = false;
        return;
    }
    const bool spawns = env.staticNpcs && npcStatic_ &&
                        spawnLookup(env.creatures, env.mapId, x, y, spawnEntries_);
    if (!job_.wantTerrain && !job_.wantDoodads && !spawns) {
        job_.active = false;
        absent_.push_back(key);
        PSP_LOGI(pp::tag::Gfx, "tile_absent x=%d y=%d (not baked)", x, y);
        return;
    }
    TileRec rec;
    rec.x = x;
    rec.y = y;
    tiles_.push_back(std::move(rec));
    const bool asked = spawns && env.staticNpcs && npcStatic_ && requestSpawns(x, y, key);
    tiles_.back().spawnsKnown = !asked;
}

// One install per call (upstream advanceFinalization phases): terrain as soon
// as its blob and detail textures are in, then the doodads with every model,
// texture and collision mesh they need, then the job is done.
bool TileStreamer::stepInstall(Env& env) {
    if (!job_.active) return false;
    TileRec* rec = findTile(job_.x, job_.y);
    if (job_.wantTerrain && !job_.terrainDone && job_.pendingTerrain <= 0) {
        const uint32_t t0 = nowUs(), reads0 = pp::pakReadCount();
        const bool ok = env.terrain.loadTile(terrainPak_, job_.x, job_.y);
        if (ok) texDirty_ = true;  // its chunk textures - the window's reads, nearest first
        pp::pakPrefetchDrop(terrainPak_.fileId());  // details already resident, or a bad blob
        job_.misses += pp::pakReadCount() - reads0;
        job_.installUs += nowUs() - t0;
        job_.terrainDone = true;
        if (rec) {
            rec->terrain = ok;
            // (Seen on hardware:) loadTile wrote through a failed allocation: no memory (or a failed read) - the
            // tile stays in the window without its ground and is asked again in 2 s; the governor frees for it.
            rec->terrainRetry = !ok && env.terrain.lastLoadNoMemory();
            if (rec->terrainRetry) {
                rec->terrainRetryMs = pp::logUptimeMs() + 2000u;
                terrainStarved_ = true;
                if (++terrainRetries_ <= 40)
                    PSP_LOGW(pp::tag::Mem, "tile_terrain_retry x=%d y=%d n=%u free_kb=%u largest_kb=%u", job_.x, job_.y,
                             static_cast<unsigned>(terrainRetries_), static_cast<unsigned>(pp::heapFreeKb()),
                             static_cast<unsigned>(pp::heapLargestKb()));
            }
        }
        if (ok) terrainInstalled_ = true;
        return true;
    }
    const bool terrainReady = !job_.wantTerrain || job_.terrainDone;
    if (job_.wantDoodads && !job_.doodadsDone && job_.pendingDoodads <= 0 && terrainReady) {
        const uint32_t t0 = nowUs(), reads0 = pp::pakReadCount();
        // by budget - what the loader parked, while tilereserve KB stay; the rest deferred.
        const rp::ModelRenderer::LoadBudget b = installBudget(env);
        bool ok = true;
        if (job_.texRetry) {  // the missing textures (parked by the loader)
            const uint32_t got = env.models.retryTextures(doodadPak_, job_.textures);
            if (texRetryJobs_ <= 40)
                PSP_LOGI(pp::tag::Mem, "tex_retry_done ok=%u of=%u still_missing=%u", static_cast<unsigned>(got),
                         static_cast<unsigned>(job_.textures.size()),
                         static_cast<unsigned>(env.models.texMissingPending()));
        } else if (job_.topUp) {
            env.models.resumeTile(doodadPak_, job_.x, job_.y, b);
        } else {
            ok = env.models.loadTile(doodadPak_, job_.x, job_.y, b);
        }
        env.models.flushGrid();  // collision queries read the grid before the next draw
        pp::pakPrefetchDrop(doodadPak_.fileId());  // models of doodads a neighbour already placed
        job_.misses += pp::pakReadCount() - reads0;
        job_.installUs += nowUs() - t0;
        job_.doodadsDone = true;
        if (rec) {
            if (!job_.topUp) rec->doodads = ok;
            rec->deferred = rec->doodads && env.models.deferredCount(job_.x, job_.y) > 0;
        }
        return true;
    }
    if (terrainReady && (!job_.wantDoodads || job_.doodadsDone)) {
        finishJob(env);
        return true;
    }
    return false;
}

void TileStreamer::finishJob(Env& env) {
    const TileRec* rec = findTile(job_.x, job_.y);
    ++loads_;
    const uint32_t deferred = env.models.deferredCount(job_.x, job_.y);
    PSP_LOGI(pp::tag::Gfx, "tile_load x=%d y=%d ms=%u kb=%u t=%d d=%d reads=%u miss=%u inst_us=%u heap_kb=%u topup=%d "
             "deferred=%u",
             job_.x, job_.y, static_cast<unsigned>(pp::logUptimeMs() - job_.startMs),
             static_cast<unsigned>(job_.bytes / 1024u), rec && rec->terrain ? 1 : 0, rec && rec->doodads ? 1 : 0,
             static_cast<unsigned>(job_.reads), static_cast<unsigned>(job_.misses), static_cast<unsigned>(job_.installUs),
             static_cast<unsigned>(heapKb()), job_.topUp ? 1 : 0, static_cast<unsigned>(deferred));
    // a top-up that placed nothing (the heap said no at once) waits 5 s before the next.
    if (job_.topUp && !job_.texRetry && deferred >= job_.deferredBefore) topUpNextMs_ = pp::logUptimeMs() + 5000u;
    (void)env;
    job_.active = false;
    job_.details.clear();
    job_.textures.clear();
    job_.models.clear();
    job_.impostors.clear();
}

// drops the doodads of one tile of the window past doodadYd_ + kDoodadKeepYd (its terrain stays).
// Never the tile being loaded.
bool TileStreamer::stepEvictDoodads(Env& env) {
    if (doodadYd_ <= 0.0f || !doodadPak_.isOpen()) return false;
    for (TileRec& t : tiles_) {
        if (!t.doodads || (job_.active && job_.x == t.x && job_.y == t.y)) continue;
        if (yardsTo(t.x, t.y, env.x, env.y) <= doodadYd_ + kDoodadKeepYd) continue;
        const uint32_t freed = env.models.unloadTile(doodadPak_, t.x, t.y);
        t.doodads = false;
        t.deferred = false;  // the deferred ones went with the tile
        ++doodadEvictions_;
        PSP_LOGI(pp::tag::Gfx, "tile_doodads_evict x=%d y=%d kb=%u heap_kb=%u", t.x, t.y,
                 static_cast<unsigned>(freed / 1024u), static_cast<unsigned>(heapKb()));
        rescanJob(env);
        return true;
    }
    return false;
}

// Evicts one tile outside the window: past the hysteresis band, or any when
// memory is short. Never the tile being loaded.
bool TileStreamer::stepEvict(Env& env, bool lowMem) {
    const float keep = radius_ * kTile + marginYd_;
    for (size_t i = 0; i < tiles_.size(); ++i) {
        TileRec& t = tiles_[i];
        if (inWindow(t.x, t.y) || (job_.active && job_.x == t.x && job_.y == t.y)) continue;
        if (!lowMem && yardsTo(t.x, t.y, env.x, env.y) <= keep) continue;
        const int x = t.x, y = t.y;
        uint32_t freed = env.terrain.unloadTile(x, y);
        if (doodadPak_.isOpen()) freed += env.models.unloadTile(doodadPak_, x, y);
        if (!t.spawns.empty()) spawnsDirty_ = true;
        texDirty_ = true;
        tiles_.erase(tiles_.begin() + static_cast<std::ptrdiff_t>(i));
        ++evictions_;
        PSP_LOGI(pp::tag::Gfx, "tile_evict x=%d y=%d kb=%u low_mem=%d tiles=%u heap_kb=%u", x, y,
                 static_cast<unsigned>(freed / 1024u), lowMem ? 1 : 0, static_cast<unsigned>(tiles_.size()),
                 static_cast<unsigned>(heapKb()));
        rescanJob(env);
        return true;
    }
    return false;
}

void TileStreamer::rebuildSpawns(Env& env) {
    spawnsDirty_ = false;
    if (!env.staticNpcs || !npcStatic_) return;
    std::vector<rp::CreatureUnit> all;
    for (const TileRec& t : tiles_) all.insert(all.end(), t.spawns.begin(), t.spawns.end());
    env.creatures.setUnits(all.data(), all.size());
}

void TileStreamer::start(Env& env, const char* mapDir) {
    if (!pp::configGetInt("stream", 1)) {
        PSP_LOGI(pp::tag::Gfx, "stream_off (wowee.cfg stream=0: tiles of world entry only)");
        return;
    }
    if (!mapDir) return;
    radius_ = std::clamp(pp::configGetInt("streamradius", 1), 0, 1);
    budgetMs_ = static_cast<float>(std::clamp(pp::configGetInt("streambudget", 4), 1, 30));
    marginYd_ = static_cast<float>(std::clamp(pp::configGetInt("streammargin", 64), 0, 266));
    reserveKb_ = static_cast<uint32_t>(std::clamp(pp::configGetInt("streamreserve", 4096), 0, 32768));
    terrainReserveKb_ = static_cast<uint32_t>(std::clamp(pp::configGetInt("terrainreserve", 1024), 0, 16384));  // B121
    doodadYd_ = static_cast<float>(std::clamp(pp::configGetInt("doodadradius", 200), 0, 2000));
    // doodads by budget, the terrain pak's descriptor back when idle.
    jobKb_ = static_cast<uint32_t>(std::clamp(pp::configGetInt("streamjobkb", 3072), 0, 65536));
    tileReserveKb_ = static_cast<uint32_t>(std::clamp(pp::configGetInt("tilereserve", 1536), 0, 32768));
    terrainIdleMs_ = static_cast<uint32_t>(std::clamp(pp::configGetInt("terrainidle", 5000), 0, 600000));
    texMaxRuns_ = static_cast<uint32_t>(std::clamp(pp::configGetInt("terraintexreads", 6), 1, 64));
    lastTerrainReqMs_ = pp::logUptimeMs();
    checkX_ = env.x;
    checkY_ = env.y;
    npcStatic_ = pp::configGetInt("npcstatic", 0) != 0;  // our server's DB spawns, off by default
    const bool useThread = pp::configGetInt("streamthread", 1) != 0;
    const int prio = std::clamp(pp::configGetInt("streamprio", 40), 17, 111);
    useThread_ = useThread;  // flush() starts the loader again
    prio_ = prio;
    readySinceMs_ = pp::logUptimeMs() | 1u;
    const uint32_t t0 = nowUs();

    char rel[64], path[256];
    std::snprintf(rel, sizeof(rel), "data/terrain_%s.pak", mapDir);
    pp::fsPath(path, sizeof(path), rel);
    if (terrainPak_.open(path)) terrainFd_ = terrainPak_.fd();
    std::snprintf(rel, sizeof(rel), "data/doodads_%s.pak", mapDir);
    pp::fsPath(path, sizeof(path), rel);
    if (doodadPak_.open(path)) {
        doodadFd_ = doodadPak_.fd();
        doodadPak_.setWorldArena(true);  // the doodads' blobs in the world arena first
    }
    if (!terrainPak_.isOpen() || terrainFd_ < 0) {
        PSP_LOGW(pp::tag::Gfx, "stream_off terrain pak missing");
        return;
    }
    if (doodadFd_ < 0) doodadPak_.close();
    // DB spawns per tile ("s_<map>_<x>_<y>"): only their places in the pak are
    // kept; CreatureRenderer reads them through its own Pak (parked blobs).
    // our server's spawns only (npcstatic=1, a pak baked with --spawns); the
    // pak's TOC is not scanned any more (tens of thousands of records): a tile's record is looked up when the
    // tile is (spawnLookup below), through the renderer's index on the same descriptor.
    if (env.creatures.isOpen() && env.staticNpcs && npcStatic_) {
        creatureFileId_ = env.creatures.pakFileId();
        creatureFd_ = env.creatures.pakFd();
    }
    if (doodadPak_.isOpen()) {
        // Model/texture tables at full size (no reallocation later) and eDRAM
        // placement from scratch, all of it now, behind the world load.
        env.models.streamBegin(doodadPak_);
        const uint32_t kb = env.models.streamVramStep(0xFFFFFFFFu) / 1024u;
        PSP_LOGI(pp::tag::Gfx, "stream_vram kb=%u (entry)", static_cast<unsigned>(kb));
    }
    if (!loader_.start(useThread, prio)) {
        PSP_LOGW(pp::tag::Gfx, "stream_thread_fail (reads on the main thread)");
        loader_.start(false, prio);
    }

    // Adopt what world entry loaded (WorldScreen::loadTerrain: 3x3).
    curX_ = static_cast<int>(std::floor(32.0f - env.y / kTile));
    curY_ = static_cast<int>(std::floor(32.0f - env.x / kTile));
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            const int x = curX_ + dx, y = curY_ + dy;
            const bool t = env.terrain.hasTile(x, y), d = env.models.hasTile(x, y);
            if (!t && !d) continue;
            TileRec rec;
            rec.x = x;
            rec.y = y;
            rec.terrain = t;
            rec.doodads = d;
            // world entry could not load its terrain (no memory) - the streamer asks again.
            char tname[24];
            std::snprintf(tname, sizeof(tname), "t_%d_%d", x, y);
            rec.terrainRetry = !t && terrainPak_.find(tname) != nullptr;
            rec.deferred = d && env.models.deferredCount(x, y) > 0;  // world entry's budget
            tiles_.push_back(std::move(rec));
            // The spawns loadCreatures already set, again per tile (evictable).
            const bool asked = env.staticNpcs && npcStatic_ &&
                               spawnLookup(env.creatures, env.mapId, x, y, spawnEntries_) &&
                               requestSpawns(x, y, tileKey(x, y));
            tiles_.back().spawnsKnown = !asked;
        }
    }
    active_ = true;
    PSP_LOGI(pp::tag::Gfx, "stream_start radius=%d doodad_yd=%.0f budget_ms=%.0f margin=%.0f thread=%d prio=%d tiles=%u "
             "spawn_tiles=%u ms=%u",
             radius_, doodadYd_, budgetMs_, marginYd_, loader_.threaded() ? 1 : 0, prio, static_cast<unsigned>(tiles_.size()),
             static_cast<unsigned>(spawnEntries_.size()), static_cast<unsigned>((nowUs() - t0) / 1000u));
    computeQueue(env.x, env.y);
    pp::logFlush();
}

void TileStreamer::update(Env& env, float /*dtSec*/) {
    if (!active_) return;
    PROF_SCOPE("stream");
    const uint32_t t0 = nowUs();
    // Live units replace the DB spawns (WorldScreen::syncCreatures): drop ours.
    if (!env.staticNpcs || !npcStatic_) {
        for (TileRec& t : tiles_) {
            if (t.spawns.empty()) continue;
            std::vector<rp::CreatureUnit>().swap(t.spawns);
        }
        spawnsDirty_ = false;
    }
    {
        PROF_SCOPE("stream.read");
        drainResults(env);
    }
    if (env.terrain.texWindow) {
        PROF_SCOPE("stream.tex");
        stepTextures(env);
    }
    const int tx = static_cast<int>(std::floor(32.0f - env.y / kTile));
    const int ty = static_cast<int>(std::floor(32.0f - env.x / kTile));
    if (tx != curX_ || ty != curY_) {
        curX_ = tx;
        curY_ = ty;
        checkX_ = env.x;
        checkY_ = env.y;
        computeQueue(env.x, env.y);
    } else if (doodadYd_ > 0.0f && std::fabs(env.x - checkX_) + std::fabs(env.y - checkY_) > kDoodadCheckYd) {
        checkX_ = env.x;  // the doodad radius moves inside a tile too
        checkY_ = env.y;
        computeQueue(env.x, env.y);
    } else if (queue_.empty() && !job_.active && static_cast<int32_t>(pp::logUptimeMs() - retryScanMs_) >= 0) {
        retryScanMs_ = pp::logUptimeMs() + 1000u;  // a tile's ground whose retry is due
        for (const TileRec& t : tiles_)
            if (inWindow(t.x, t.y) && needsTerrain(t)) {
                computeQueue(env.x, env.y);
                break;
            }
    }
    const uint32_t budgetUs = static_cast<uint32_t>(budgetMs_ * 1000.0f);
    for (int steps = 0;; ++steps) {
        if (steps > 0 && nowUs() - t0 >= budgetUs) break;
        bool did = false;
        {
            PROF_SCOPE("stream.install");
            did = stepEvict(env, false) || stepEvictDoodads(env) || stepInstall(env);
        }
        if (!did && !loader_.threaded()) {
            PROF_SCOPE("stream.read");
            if (loader_.pumpSync()) {
                drainResults(env);
                did = true;
            }
        }
        if (!did && !job_.active) {
            while (!queue_.empty()) {
                const uint32_t k = queue_.front();
                const TileRec* have = findTile(keyX(k), keyY(k));  // in, but its doodads wanted now
                if ((have && !needsDoodads(*have, env.x, env.y) && !needsTerrain(*have)) || !inWindow(keyX(k), keyY(k))) {
                    queue_.erase(queue_.begin());
                    continue;
                }
                // A tile without its terrain goes by the ground's rule (its block + terrainreserve),
                // one in with its doodads wanted by streamreserve's headroom.
                char tname[32];
                std::snprintf(tname, sizeof(tname), "t_%d_%d", keyX(k), keyY(k));
                const bool needTerrain = !env.terrain.hasTile(keyX(k), keyY(k)) && terrainPak_.find(tname);
                // A tile within the doodads' view distance loads them by the near rule.
                const bool nearTile = yardsTo(keyX(k), keyY(k), env.x, env.y) <= env.models.viewDistance;
                if (!memoryOk(needTerrain, nearTile)) {
                    if (!needTerrain) {
                        // (Seen on the emulator: tiles=6 queue=7 for minutes - the doodads at the queue's head
                        // could not start and the terrain behind them never had its turn): a tile that only wants its
                        // doodads waits in the queue, the next one may go (terrain first by its own rule).
                        ++lowStarves_;
                        bool other = false;
                        for (size_t qi = 1; qi < queue_.size() && !other; ++qi) {
                            char nm[32];
                            std::snprintf(nm, sizeof(nm), "t_%d_%d", keyX(queue_[qi]), keyY(queue_[qi]));
                            if (!env.terrain.hasTile(keyX(queue_[qi]), keyY(queue_[qi])) && terrainPak_.find(nm) &&
                                inWindow(keyX(queue_[qi]), keyY(queue_[qi]))) {
                                std::swap(queue_[0], queue_[qi]);  // that tile's ground first
                                other = true;
                            }
                        }
                        if (other) continue;
                    }
                    PROF_SCOPE("stream.install");
                    did = stepEvict(env, true);  // anything outside the window, margin or not
                    break;
                }
                // a tile without its terrain - the terrain pak's descriptor first (given back idle).
                if (!env.terrain.hasTile(keyX(k), keyY(k)) && !ensureTerrainFd()) break;
                queue_.erase(queue_.begin());
                startJob(env, keyX(k), keyY(k), !needTerrain || memoryOk(false, nearTile));
                did = true;
                break;
            }
            // nothing queued - the deferred doodads of a tile in (top-up).
            if (!did && queue_.empty()) {
                PROF_SCOPE("stream.install");
                did = stepTopUp(env);
            }
        }
        if (!did && spawnsDirty_ && spawnsPending_ == 0) {
            PROF_SCOPE("stream.install");
            rebuildSpawns(env);
            did = true;
        }
        if (!did) {
            PROF_SCOPE("stream.install");
            const uint32_t copied = env.models.streamVramStep(kVramStepBytes);
            if (copied) {
                vramCopied_ += copied;
                vramBusy_ = true;
                did = true;
            } else if (vramBusy_) {
                vramBusy_ = false;
                PSP_LOGI(pp::tag::Gfx, "stream_vram kb=%u", static_cast<unsigned>(vramCopied_ / 1024u));
                vramCopied_ = 0;
            }
        }
        if (!did) break;
    }
    releaseTerrainFdIfIdle();
    checkReady(env);           // stream_ready
    const uint32_t nowMs = pp::logUptimeMs();
    if (nowMs >= nextLogMs_) {
        nextLogMs_ = nowMs + 10000;
        uint32_t withDoodads = 0;
        for (const TileRec& t : tiles_) withDoodads += t.doodads ? 1u : 0u;
        PSP_LOGI(pp::tag::Gfx, "stream_frame tile=%d_%d tiles=%u doodad_tiles=%u queue=%u job=%d inflight=%u parked=%u "
                 "loads=%u evicts=%u doodad_evicts=%u heap_kb=%u deferred=%u topups=%u topup_ok=%d terrain_fd=%d",
                 curX_, curY_, static_cast<unsigned>(tiles_.size()), static_cast<unsigned>(withDoodads),
                 static_cast<unsigned>(queue_.size()), job_.active ? 1 : 0,
                 static_cast<unsigned>(loader_.inFlight() + backlog_.size()), static_cast<unsigned>(pp::pakPrefetchCount()),
                 static_cast<unsigned>(loads_), static_cast<unsigned>(evictions_),
                 static_cast<unsigned>(doodadEvictions_), static_cast<unsigned>(heapKb()),
                 static_cast<unsigned>(env.models.deferredTotal()), static_cast<unsigned>(topUps_), topUpOk_ ? 1 : 0,
                 terrainFd_ >= 0 ? 1 : 0);
        if (env.terrain.texWindow) {
            const rp::TerrainRenderer::TexStats ts = env.terrain.texStats();
            PSP_LOGI(pp::tag::Gfx, "terrain_tex chunks=%u/%u kb=%u asked=%u inflight=%u reads=%u fails=%u installs=%u "
                     "frees=%u nomem=%u keep_yd=%.0f",
                     static_cast<unsigned>(ts.resident), static_cast<unsigned>(ts.chunks),
                     static_cast<unsigned>(ts.residentBytes / 1024u), static_cast<unsigned>(ts.requested),
                     static_cast<unsigned>(texInFlight_), static_cast<unsigned>(texReads_),
                     static_cast<unsigned>(texReadFails_), static_cast<unsigned>(ts.installs),
                     static_cast<unsigned>(ts.frees), static_cast<unsigned>(ts.nomem), env.terrain.texRule.keepYd);
        }
    }
}

// The chunk-texture window's reads - frees behind, runs ahead (nearest first), at most
// terraintexreads in flight. Not while the terrain pak has no descriptor (taken again here when runs are wanted).
void TileStreamer::stepTextures(Env& env) {
    const uint32_t now = pp::logUptimeMs();
    const float moved = std::fabs(env.x - texCheckX_) + std::fabs(env.y - texCheckY_);
    const bool due = texDirty_ || texMore_ || moved >= texStepYd_ || static_cast<int32_t>(now - texNextMs_) >= 0;
    if (!due || texInFlight_ >= texMaxRuns_) return;
    if (static_cast<int32_t>(now - texBackoffMs_) < 0) return;  // a texture found no memory a moment ago
    texDirty_ = false;
    texCheckX_ = env.x;
    texCheckY_ = env.y;
    texNextMs_ = now + 1000u;
    std::vector<rp::TerrainRenderer::TexRequest> reqs;
    uint32_t room = texMaxRuns_ - texInFlight_;
    if (rp::TerrainRenderer::pooled()) room = std::min(room, rp::TerrainRenderer::poolRunsFree());
    if (!room) return;
    texMore_ = env.terrain.texStep(env.x, env.y, room, reqs);
    if (reqs.empty()) return;
    if (!ensureTerrainFd()) {  // no descriptor now: asked again at the next step
        for (const auto& q : reqs) env.terrain.texCancel(q.tileX, q.tileY, q.first, q.count);
        texMore_ = false;
        return;
    }
    lastTerrainReqMs_ = now;
    for (const auto& q : reqs) {
        // read into a pool row buffer (no heap block per read); none - the loader's own block.
        void* dst = rp::TerrainRenderer::poolRunAlloc();
        if (dst) runsOut_.push_back(dst);
        requestRaw(terrainPak_, terrainFd_, q.offset, q.size, kChunkTex,
                   static_cast<uint32_t>(q.first) | (static_cast<uint32_t>(q.count) << 16), tileKey(q.tileX, q.tileY),
                   dst);
    }
}

// The landing hold, ui_teleport_hold.hpp: the tile under (x, y) has its doodads in (or none to load).
bool TileStreamer::doodadsInAt(float x, float y) {
    const int tx = static_cast<int>(std::floor(32.0f - y / kTile)), ty = static_cast<int>(std::floor(32.0f - x / kTile));
    if (job_.active && job_.x == tx && job_.y == ty && job_.wantDoodads && !job_.doodadsDone) return false;
    const TileRec* t = findTile(tx, ty);
    if (!t) return false;
    return t->doodads || !needsDoodads(*t, x, y);
}

// The window is in - every baked tile of it with its ground, every tile within doodadradius with its
// doodads (none deferred), no missing doodad texture, no chunk-texture read in flight, nothing queued. Logged once per
// place (start, teleport, flush): "stream_ready near=1 full=1 tiles=<n> doodads=<in>/<wanted> ...". near=1 is logged
// once as soon as what the player can SEE is in (the autotest's final block waits for it); a window not in after 45 s
// logs one line with the counts; stream_ready_deferred lists the tiles with deferred placements (deferred/in view@yd).
void TileStreamer::checkReady(Env& env) {
    if (readyLogged_) {
        nearStarveSinceMs_ = 0;
        return;
    }
    const uint32_t now = pp::logUptimeMs();
    if (!readySinceMs_) readySinceMs_ = now | 1u;
    // near: what the player can see is in - every baked tile with its ground, every doodad tile within the view
    // distance loaded, no deferred placement in view, no missing texture. full: all of the window as well.
    uint32_t baked = 0, ground = 0, want = 0, have = 0, deferred = 0, nearMissing = 0, inView = 0;
    char name[32], list[96];
    size_t at = 0;
    list[0] = 0;
    for (int dx = -radius_; dx <= radius_; ++dx)
        for (int dy = -radius_; dy <= radius_; ++dy) {
            const int x = curX_ + dx, y = curY_ + dy;
            const TileRec* rec = findTile(x, y);
            std::snprintf(name, sizeof(name), "t_%d_%d", x, y);
            if (terrainPak_.find(name)) {
                ++baked;
                if (rec && rec->terrain) ++ground;
            }
            std::snprintf(name, sizeof(name), "d_%d_%d", x, y);
            if (doodadPak_.isOpen() && wantDoodads(x, y, env.x, env.y) && doodadPak_.find(name)) {
                ++want;
                const float yd = yardsTo(x, y, env.x, env.y);
                const bool in = rec && rec->doodads;
                if (in) ++have;
                else if (yd <= env.models.viewDistance) ++nearMissing;
                const uint32_t dn = in ? env.models.deferredCount(x, y) : 0u;
                if (dn) {
                    ++deferred;
                    const uint32_t iv = env.models.deferredInView(x, y, env.x, env.y);
                    inView += iv;
                    if (at + 28 < sizeof(list)) {  // tile:deferred/in_view@yards
                        const int w = std::snprintf(list + at, sizeof(list) - at, "%s%d_%d:%u/%u@%.0f", at ? "," : "",
                                                    x, y, static_cast<unsigned>(dn), static_cast<unsigned>(iv), yd);
                        if (w > 0) at += static_cast<size_t>(w);
                    }
                }
            }
        }
    const uint32_t texMissing = env.models.texMissingPending();
    const bool idle = !job_.active && queue_.empty() && backlog_.empty() && texInFlight_ == 0;
    const bool near = ground == baked && nearMissing == 0 && inView == 0 && texMissing == 0;
    const bool full = idle && ground == baked && have == want && deferred == 0 && texMissing == 0;
    const uint32_t waited = now - (readySinceMs_ & ~1u);
    // Logged when what is in view is in (once), when the whole window is (once, the end), and after 45 s without it.
    // (Seen on the emulator:) what is in view cannot come in - nothing in flight for it and still not
    // near: the time it has been so (WorldScreen frees out-of-view memory after nearstarvems: near_starve_flush).
    if (near || job_.active || !queue_.empty()) nearStarveSinceMs_ = 0;
    else if (!nearStarveSinceMs_) nearStarveSinceMs_ = now | 1u;
    const bool nearNew = near && !readyNearLogged_;
    const bool late = !readyLate_ && waited >= 45000u;
    if (!full && !nearNew && !late) return;
    PSP_LOGI(pp::tag::Gfx, "stream_ready near=%d full=%d tiles=%u doodads=%u/%u deferred=%u in_view=%u texmiss=%u ms=%u",
             near ? 1 : 0, full ? 1 : 0, static_cast<unsigned>(ground), static_cast<unsigned>(have),
             static_cast<unsigned>(want), static_cast<unsigned>(deferred), static_cast<unsigned>(inView),
             static_cast<unsigned>(texMissing), static_cast<unsigned>(waited));
    if (deferred)
        PSP_LOGI(pp::tag::Gfx, "stream_ready_deferred heap_kb=%u free_kb=%u tiles=%s", static_cast<unsigned>(heapKb()),
                 static_cast<unsigned>(pp::heapFreeKb()), list);
    if (near) readyNearLogged_ = true;
    if (full) readyLogged_ = true;
    if (late) readyLate_ = true;
}

// see ui_world_streaming.hpp.
uint32_t TileStreamer::flush(Env& env) {
    if (!active_) return 0;
    const uint32_t held = static_cast<uint32_t>(tiles_.size());
    loader_.stop();  // results and requests never read give their owned buffers back
    for (const StreamLoader::Request& r : backlog_)
        if (r.freeDst) std::free(r.dst);
    backlog_.clear();
    for (void* p : runsOut_) rp::TerrainRenderer::poolRunFree(p);
    runsOut_.clear();
    if (terrainPak_.isOpen()) pp::pakPrefetchDrop(terrainPak_.fileId());
    if (doodadPak_.isOpen()) pp::pakPrefetchDrop(doodadPak_.fileId());
    if (creatureFileId_) pp::pakPrefetchDrop(creatureFileId_);
    job_ = Job{};
    tiles_.clear();
    queue_.clear();
    texInFlight_ = 0;
    texDirty_ = true;
    texMore_ = false;
    texBackoffMs_ = 0;
    texCheckX_ = texCheckY_ = -1e9f;
    terrainStarved_ = false;
    terrainSlotsReported_ = false;
    lowMemReported_ = false;
    spawnsPending_ = 0;
    spawnsDirty_ = env.staticNpcs && npcStatic_;  // the old place's DB spawns go (rebuildSpawns: none left)
    vramBusy_ = false;
    vramCopied_ = 0;
    topUpNextMs_ = 0;
    env.terrain.clear();  // tiles, chunk textures and details back (the pool's slots)
    env.models.clear();   // models, textures, collision, placements (their arena / heap blocks)
    if (doodadPak_.isOpen()) env.models.streamBegin(doodadPak_);  // the tables at full size, eDRAM from scratch
    if (!loader_.start(useThread_, prio_)) loader_.start(false, prio_);
    curX_ = curY_ = -1000;  // the next update queues the new window (as at world entry)
    checkX_ = env.x;
    checkY_ = env.y;
    readyLogged_ = false;
    readyLate_ = false;
    readyNearLogged_ = false;
    readySinceMs_ = pp::logUptimeMs() | 1u;
    ++flushes_;
    return held;
}

uint32_t TileStreamer::terrainNeedKb() const {
    // With the texture window a tile's terrain is its vertex block + head (the textures come in
    // 6 KB blocks), else the whole blob + vertices.
    return pp::configGetInt("terraintex", 1) != 0 ? kTerrainHeadBlockKb : kTerrainBlockKb;
}

// ---------------------------------------------------------------------------
// WorldScreen hooks

void WorldScreen::startStreaming() {
    const uint32_t mapId = session_.worldEntry().mapId;
    TileStreamer::Env env{terrain_, models_, creatures_, mapId, creatures_.isOpen() && !liveCreatures_, playerX_,
                          playerY_};
    const std::string dir = worldMapDir(mapId);  // any map (a dungeon streams its virtual tiles)
    stream_.start(env, dir.empty() ? nullptr : dir.c_str());
    if (terrain_.texWindow && !stream_.active() && !dir.empty()) {
        // No streamer to keep the texture window moving - every texture of the tiles in, as before
        // the window (the window's rule with no reach limit).
        char rel[64], path[256];
        std::snprintf(rel, sizeof(rel), "data/terrain_%s.pak", dir.c_str());
        pp::fsPath(path, sizeof(path), rel);
        pp::Pak pak;
        terrain_.texRule = rp::TexWindowRule{1.0e9f, 2.0e9f};
        const uint32_t n = pak.open(path) ? terrain_.texSyncLoad(pak, playerX_, playerY_) : 0u;
        PSP_LOGW(pp::tag::Gfx, "terrain_tex_all (no streamer) chunks=%u", static_cast<unsigned>(n));
    }
}

void WorldScreen::updateStreaming(float dtSec) {
    if (!stream_.active()) return;
    TileStreamer::Env env{terrain_, models_, creatures_, session_.worldEntry().mapId,
                          creatures_.isOpen() && !liveCreatures_, playerX_, playerY_};
    stream_.update(env, dtSec);
    if (stream_.takeTerrainInstalled() && loadStatus_ == OWN_STR(MAP_TILES_NOT_BAKED)) loadStatus_.clear();
}

} // namespace wowee::ui::psp
