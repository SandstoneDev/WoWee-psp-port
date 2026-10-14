// The big WMOs of the map - capitals and every WMO over the doodad bake's budget
//. The doodads pak's "wbig" record lists where they stand; a big WMO within reach of
// the camera is opened (rendering::psp::BigWmo: its pak's "!wmo" record, one descriptor given back while idle),
// its chunks, shells, collision, textures and doodad lists are read by a loader thread of its own (StreamLoader,
// as the tile streamer: the main thread installs between frames), drawn after the terrain and before the M2
// doodads (which take the groups the portals showed: ModelRenderer::groupFilter), and collide with the player and
// the camera next to the doodads (floorAt, collideWalls, cameraDistance).
// The WMO's doodads are the doodads pak's models ("wbig" stamp): one pseudo-tile of the doodad renderer, rebuilt
// when the resident doodad cells change, its models and textures read ahead by the same loader and parked
// (pakPrefetchPut) so the rebuild does not touch the card.
// wowee.cfg: wmo=0 off; wmobudget (KB, 3072), wmolod / wmoshell / wmoview (yd: 50 / 160 / 450), wmocolyd,
// wmodoodadyd, wmoportals, wmovis (2 walk + exterior seeds, 4 walk-outdoor, 1 walk, 3 upstream), wmodoodads, wmotexlod (KB), wmoprefetch
// (tenths of a second), wmoreads (reads in flight), wmoreserve (KB a probe must get, else no reads and a smaller budget),
// wmofarkb (KB of headroom a WMO the camera is outside of needs to open), wmofarwait (s;
// after this long waiting the far WMO opens on wmoreserve's headroom - a far city waits for room, never forever).
// Log: bigwmo_index, bigwmo_open, bigwmo_close, bigwmo_prime, bigwmo_frame (10 s), bigwmo_doodads.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_mem_governor.hpp"  // pp::bigWmoOpenKb (fair sharing)
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/psp/psp_bigwmo.hpp"

#include <pspkernel.h>

#include <malloc.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {
unsigned U(uint64_t v) { return static_cast<unsigned>(v); }  // %u of the PSP's uint32_t (long unsigned)
constexpr uint8_t kReqWmo = 1, kReqDoodad = 2;  // StreamLoader::Request::kind
constexpr int kDoodadTileA = -3000, kDoodadTileB = -3001;  // pseudo-tiles of models_ (outside the 0..63 grid)
constexpr uint32_t kMaxOpen = 2;
// Opening big WMOs under the descriptor and memory limits:
constexpr uint32_t kOpenRetryMs = 5000;             // an open that failed for want of a descriptor / memory / a read
constexpr unsigned kErrMFile = 0x80020320u;         // SCE_KERNEL_ERROR_MFILE: no descriptor left (seen on hardware)
constexpr uint32_t kIdleFdMs = 5000, kIdleFdTightMs = 1000;  // descriptor given back after this long without reads
constexpr uint32_t kDoodadFlightBytes = 256u * 1024u;  // doodad read-ahead in flight at most
constexpr uint32_t kDoodadParkSalt = 0x9E3779B9u;

bool due(uint32_t atMs, uint32_t nowMs) { return atMs == 0 || static_cast<int32_t>(nowMs - atMs) >= 0; }

// the read-ahead parks its blobs under a key of its own (not the doodads pak's fileId): the tile
// streamer drops everything parked under that fileId after each tile (ui_world_streaming.cpp stepInstall), which
// threw the WMO's read-ahead away - its models were then read on the main thread by setPlacements. They go under
// the pak's own key only for the setPlacements call.
uint32_t parkKeyOf(uint32_t fileId) {
    const uint32_t k = fileId ^ kDoodadParkSalt;
    return k ? k : 1u;
}

// as ui_world_transports.cpp heapFits - whether the heap still gives totalKb (in blocks of at
// most 512 KB) with one block of largestKb among them: the new doodad set's models load as separate pak blobs.
// Every block is written (gcc drops a malloc/free pair it can see through).
bool heapFitsKb(uint32_t totalKb, uint32_t largestKb) {
    std::array<void*, 64> held{};
    size_t n = 0;
    uint32_t got = 0;
    auto take = [&](uint32_t kb) {
        void* volatile p = pp::heapProbeAlloc(static_cast<size_t>(kb) * 1024u);  // as malloc, uncounted
        if (!p) return false;
        static_cast<volatile char*>(p)[0] = 0;
        held[n++] = p;
        got += kb;
        return true;
    };
    bool ok = take(std::max<uint32_t>(1u, largestKb));
    while (ok && got < totalKb && n < held.size()) ok = take(std::min<uint32_t>(512u, totalKb - got));
    ok = ok && got >= totalKb;
    for (size_t i = 0; i < n; ++i) std::free(held[i]);
    return ok;
}
}  // namespace

struct WorldScreen::BigWmoState {
    bool on = true, doodadsOn = true;
    bool govCut = false;  // the memory governor dropped the doodads (bigWmoDoodadsGovernor)
    rp::BigWmoConfig cfg;
    std::vector<rp::BigWmoPlacement> placements;
    uint32_t stamp = 0;
    struct Inst {
        std::unique_ptr<rp::BigWmo> wmo;
        uint32_t placement = 0;
        uint32_t groupBase = 0;
        uint32_t farSinceMs = 0;   // when it left the reach (closed 5 s later)
        uint32_t idleSinceMs = 0;  // no read since (descriptor given back after 5 s)
        uint32_t blankSinceMs = 0; // outside, nothing drawn since (its budget trimmed after 5 s)
    };
    std::vector<Inst> open;
    StreamLoader loader;
    bool loaderOn = false;
    uint32_t maxReads = 6;
    uint32_t reserveKb = 1024, nextProbeMs = 0, lowMemCount = 0;  // wowee.cfg wmoreserve
    // A WMO the camera is outside of opens only while the heap gives wmofarkb (1 MB of it in one block),
    // never at world entry (priming) - the NPCs next to the player come first.
    uint32_t farOpenKb = 3072, farHeldLogs = 0;
    // per placement, since when it has waited from outside (0 = not waiting); after
    // wmofarwait it opens on the near reserve (pp::bigWmoOpenKb).
    std::vector<uint32_t> farWaitSinceMs;
    uint32_t farWaitCapMs = 30000, lateOpens = 0;
    bool priming = false;
    bool lowMem = false;
    std::vector<rp::BigWmo::Request> reqs;
    uint32_t frame = 0, nextLogMs = 0, opens = 0, closes = 0;
    // Doodads: the set models_ holds as a pseudo-tile, and the next one while its models are read ahead.
    std::vector<uint32_t> mask;  // bit per (groupBase + group)
    std::vector<rp::DoodadPlacement> placed, next;
    bool nextBuilding = false, nextDirty = false;
    uint32_t nextPending = 0, doodadTile = 0, nextAtMs = 0, doodadLogs = 0;
    std::vector<uint16_t> askedModels, askedTextures;
    // Opens under the descriptor and memory limits:
    std::vector<uint8_t> dead;              // per placement: its pak is missing or bad - never opened again
    std::vector<uint32_t> openRetryMs;      // per placement: an open failed for want of a descriptor / memory / a read
    uint32_t openRetryLogs = 0;
    bool fdTight = false;                   // descriptors short: one WMO open, its descriptor back after 1 s idle
    int fdFree = -1, fdBase = 0;            // world entry: free descriptors (probe) and pak descriptors open then
    std::vector<StreamLoader::Request> ddQueue;  // read-ahead waiting for room (ring, bytes in flight, memory)
    uint32_t ddFlightBytes = 0;             // read-ahead bytes in flight (every generation)
    uint32_t ddGen = 0;                     // read-ahead generation (Request::key >> 8): an abandoned one's are freed
    uint32_t ddPark = 0;                    // the private key its blobs are parked under (parkKeyOf)
    std::vector<uint32_t> ddParked;         // their offsets
    uint32_t ddLowMemLogs = 0;
    // Frees what the read-ahead parked and forgets its queue (a new set, low memory, the last WMO closed).
    void dropReadAhead() {
        for (uint32_t off : ddParked)
            if (void* b = pp::pakPrefetchTake(ddPark, off)) std::free(b);
        ddParked.clear();
        ddQueue.clear();
        nextPending = 0;
        nextBuilding = false;
        ++ddGen;
    }
};

void WorldScreen::destroyBigWmoState(BigWmoState* p) {
    if (p) {
        p->loader.stop();
        p->dropReadAhead();  // the parked blobs live in psp_pak's table, not in the state
    }
    delete p;
}

// World entry (loadDoodads, the doodads pak open): the "wbig" record.
void WorldScreen::loadBigWmoIndex(pp::Pak& pak) {
    if (!pp::configGetInt("wmo", 1)) return;
    const pp::PakEntry* e = pak.find("wbig");
    if (!e) return;
    void* blob = pak.load(e);
    const auto* h = static_cast<const rp::BigWmoIndexHeader*>(blob);
    if (!blob || e->size < sizeof(*h) || std::memcmp(h->magic, "WBGI", 4) != 0 || h->version != rp::kBigWmoVersion ||
        sizeof(*h) + uint64_t{h->count} * sizeof(rp::BigWmoPlacement) > e->size) {
        PSP_LOGW(pp::tag::Gfx, "bigwmo_index bad");
        if (blob) pp::pakFree(blob);
        return;
    }
    bigWmo_ = std::unique_ptr<BigWmoState, void (*)(BigWmoState*)>(new BigWmoState, &WorldScreen::destroyBigWmoState);
    BigWmoState& s = *bigWmo_;
    const auto* p = reinterpret_cast<const rp::BigWmoPlacement*>(static_cast<const uint8_t*>(blob) + sizeof(*h));
    s.placements.assign(p, p + h->count);
    s.dead.assign(s.placements.size(), 0);
    s.openRetryMs.assign(s.placements.size(), 0);
    s.farWaitSinceMs.assign(s.placements.size(), 0);
    s.stamp = h->stamp;
    pp::pakFree(blob);
    s.cfg.budgetBytes = static_cast<uint32_t>(std::clamp(pp::configGetInt("wmobudget", 3072), 512, 16384)) * 1024u;
    s.cfg.lod.lodDistance = static_cast<float>(std::clamp(pp::configGetInt("wmolod", 50), 0, 1000));
    s.cfg.lod.shellDistance = static_cast<float>(std::clamp(pp::configGetInt("wmoshell", 160), 50, 2000));
    s.cfg.visMode = std::clamp(pp::configGetInt("wmovis", rp::PortalVisibility::kVisWalkExt), 0, 4);
    s.cfg.lod.viewDistance = static_cast<float>(std::clamp(pp::configGetInt("wmoview", 450), 50, 2000));
    s.cfg.collisionYd = static_cast<float>(std::clamp(pp::configGetInt("wmocolyd", 40), 10, 200));
    s.cfg.doodadYd = static_cast<float>(std::clamp(pp::configGetInt("wmodoodadyd", 160), 0, 600));
    s.cfg.portals = pp::configGetInt("wmoportals", 1) != 0;
    s.cfg.texLodBytes = static_cast<uint32_t>(std::max(0, pp::configGetInt("wmotexlod", 16))) * 1024u;
    s.cfg.prefetchSec = static_cast<float>(std::clamp(pp::configGetInt("wmoprefetch", 15), 0, 100)) * 0.1f;
    s.doodadsOn = pp::configGetInt("wmodoodads", 1) != 0;
    s.maxReads = static_cast<uint32_t>(std::clamp(pp::configGetInt("wmoreads", 6), 1, 64));
    s.reserveKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("wmoreserve", 1024), 0, 16384));
    s.farOpenKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("wmofarkb", 3072), 0, 16384));
    s.farWaitCapMs = static_cast<uint32_t>(std::clamp(pp::configGetInt("wmofarwait", 30), 0, 3600)) * 1000u;  // B121
    PSP_LOGI(pp::tag::Gfx, "bigwmo_index n=%u stamp=%08x budget_kb=%u lod=%.0f shell=%.0f view=%.0f portals=%d vis=%d",
             static_cast<unsigned>(s.placements.size()), static_cast<unsigned>(s.stamp),
             static_cast<unsigned>(s.cfg.budgetBytes / 1024u), s.cfg.lod.lodDistance, s.cfg.lod.shellDistance,
             s.cfg.lod.viewDistance, s.cfg.portals ? 1 : 0, s.cfg.visMode);
    for (const auto& pl : s.placements)
        PSP_LOGI(pp::tag::Gfx, "bigwmo_index pak=%s uid=%u tile=%d_%d box=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)", pl.pak,
                 static_cast<unsigned>(pl.uniqueId), pl.tileX, pl.tileY, pl.bmin[0], pl.bmin[1], pl.bmin[2], pl.bmax[0],
                 pl.bmax[1], pl.bmax[2]);
}

namespace {
float boxYards(const rp::BigWmoPlacement& p, const glm::vec3& q) {
    return rp::boxDistance(q, glm::vec3(p.bmin[0], p.bmin[1], p.bmin[2]), glm::vec3(p.bmax[0], p.bmax[1], p.bmax[2]));
}
}  // namespace

// Opens / closes by reach, drives the loader, installs, rebuilds the doodad set. Main thread, after movement.
void WorldScreen::updateBigWmos(float dtSec) {
    if (!bigWmo_) return;
    BigWmoState& s = *bigWmo_;
    PROF_SCOPE("bigwmo.update");
    ++s.frame;
    const uint32_t now = pp::logUptimeMs();
    const glm::vec3 player(playerX_, playerY_, playerZ_);
    const glm::vec3 cam = camInit_ ? camPosForHud_ : player + glm::vec3(0.0f, 0.0f, 2.0f);
    const float reach = s.cfg.lod.viewDistance + 50.0f;
    pp::wmoArenaSetBigOpen(static_cast<uint32_t>(s.open.size()));  // the world arena keeps room for them
    // Close what left the reach 5 s ago (nothing in flight).
    for (size_t i = 0; i < s.open.size();) {
        BigWmoState::Inst& in = s.open[i];
        const bool far = boxYards(s.placements[in.placement], cam) > reach + 100.0f;
        if (!far) in.farSinceMs = 0;
        else if (!in.farSinceMs) in.farSinceMs = now;
        if (far && now - in.farSinceMs > 5000 && in.wmo->inFlight() == 0) {
            PSP_LOGI(pp::tag::Gfx, "bigwmo_close name=%s", in.wmo->name());
            ++s.closes;
            s.open.erase(s.open.begin() + static_cast<std::ptrdiff_t>(i));
            s.nextDirty = true;
            continue;
        }
        ++i;
    }
    // descriptors short - an open failed with 0x80020320, or (fd_probe at world entry) at most one
    // is left for the other paks the game opens on demand (ui.pak, tutorial.pak, the minimap, the music):
    // one WMO open, and a descriptor back after 1 s without reads instead of 5 s.
    if (!s.fdTight && s.fdFree >= 0 && s.fdFree - (pp::pakOpenFds() - s.fdBase) <= 1) {
        s.fdTight = true;
        PSP_LOGW(pp::tag::Fs, "bigwmo_fd_tight free_at_entry=%d pak_fds=%d", s.fdFree, pp::pakOpenFds());
    }
    // Open the nearest within reach (at most kMaxOpen; the nearest first - was the first by index
    // -, one while descriptors are short; a pak missing or bad is never tried again, one that failed for want of a
    // descriptor / memory / a read 5 s later - was: any failure hid the WMO until the game restarted).
    while (s.open.size() < (s.fdTight ? 1u : kMaxOpen)) {
        int pick = -1;
        float pickYd = reach;
        for (uint32_t pi = 0; pi < s.placements.size(); ++pi) {
            if (s.dead[pi] || !due(s.openRetryMs[pi], now)) continue;
            bool already = false;
            for (const auto& in : s.open) already = already || in.placement == pi;
            const float yd = already ? 1e30f : boxYards(s.placements[pi], cam);
            if (!already && yd > reach) s.farWaitSinceMs[pi] = 0;  // out of reach - its wait starts over
            if (s.govCut && yd > 0.0f) continue;  // the governor's step 3 - only the one the camera is in
            if (yd <= pickYd) {
                pickYd = yd;
                pick = static_cast<int>(pi);
            }
        }
        if (pick < 0) break;
        const uint32_t pi = static_cast<uint32_t>(pick);
        // from outside its box - not at world entry, and only with room to spare (asked again in 5 s).
        // wmofarkb of headroom (pieces of <= 512 KB, one 1 MB block) - but after wmofarwait s of
        // waiting the near reserve (wmoreserve) is enough: the lowest priority waits for room, never forever.
        if (pickYd > 0.0f) {
            uint32_t& since = s.farWaitSinceMs[pi];
            const uint32_t waited = since ? now - since : 0u;
            pp::FarWmoRule rule;
            rule.farKb = s.farOpenKb;
            rule.nearKb = s.reserveKb;
            rule.waitCapMs = s.farWaitCapMs;
            const uint32_t needKb = pp::bigWmoOpenKb(pickYd, s.priming, waited, rule);
            // A far city is the lowest priority: the guard block too, until it waited wmofarwait (then the near rule).
            const bool late = needKb != 0xFFFFFFFFu && needKb != rule.farKb;
            const bool ok = needKb != 0xFFFFFFFFu && (!needKb || heapFitsKb(needKb, std::min<uint32_t>(1024u, needKb))) &&
                            (late || (pp::heapGuardOk() && pp::heapFreeKb() >= needKb + pp::heapGuardKb()));
            if (!ok) {
                if (!since && !s.priming) since = now | 1u;
                s.openRetryMs[pi] = (now + kOpenRetryMs) | 1u;
                if (++s.farHeldLogs <= 20)
                    PSP_LOGI(pp::tag::Mem, "bigwmo_far_wait name=%s yd=%.0f priming=%d need_kb=%u waited_s=%u heap_kb=%u "
                             "free_kb=%u",
                             s.placements[pi].pak, pickYd, s.priming ? 1 : 0, U(needKb == 0xFFFFFFFFu ? 0u : needKb),
                             U(waited / 1000u), U(pp::heapUsedKb()), U(pp::heapFreeKb()));
                continue;
            }
            if (since && rule.waitCapMs && waited >= rule.waitCapMs) {
                ++s.lateOpens;
                PSP_LOGI(pp::tag::Mem, "bigwmo_far_open_late name=%s yd=%.0f waited_s=%u need_kb=%u free_kb=%u",
                         s.placements[pi].pak, pickYd, U(waited / 1000u), U(needKb), U(pp::heapFreeKb()));
            }
            since = 0;
        }
        BigWmoState::Inst in;
        in.wmo = std::make_unique<rp::BigWmo>();
        in.placement = pi;
        in.wmo->cfg = s.cfg;
        char rel[96], path[256];
        std::snprintf(rel, sizeof(rel), "data/%s", s.placements[pi].pak);
        pp::fsPath(path, sizeof(path), rel);
        const auto& pl = s.placements[pi];
        if (!in.wmo->open(path, pl.matrix, pl.uniqueId, pl.doodadSet, s.stamp)) {
            const rp::BigWmo::OpenError err = in.wmo->openError();
            if (err == rp::BigWmo::kOpenRetry) {
                s.openRetryMs[pi] = (now + kOpenRetryMs) | 1u;
                if (static_cast<unsigned>(in.wmo->fdError()) == kErrMFile) s.fdTight = true;
                if (++s.openRetryLogs <= 20)
                    PSP_LOGW(pp::tag::Gfx, "bigwmo_open_retry %s err=%08x in_ms=%u", rel,
                             static_cast<unsigned>(in.wmo->fdError()), U(kOpenRetryMs));
            } else {
                s.dead[pi] = 1;  // missing / bad pak: never again
                if (err == rp::BigWmo::kOpenMissing) PSP_LOGW(pp::tag::Gfx, "bigwmo_missing %s", rel);
            }
            continue;
        }
        ++s.opens;
        s.open.push_back(std::move(in));
        s.nextDirty = true;
    }
    pp::Pak* dpak = stream_.doodadPak();
    if (s.open.empty()) {
        if (s.nextBuilding || !s.ddParked.empty()) s.dropReadAhead();
        if (!s.placed.empty() && dpak) {  // the last one closed: its doodads go too
            models_.unloadTile(*dpak, kDoodadTileA, kDoodadTileA);
            models_.unloadTile(*dpak, kDoodadTileB, kDoodadTileB);
            models_.flushGrid();
            s.placed.clear();
        }
        return;
    }
    if (!s.loaderOn) {
        const bool useThread = pp::configGetInt("streamthread", 1) != 0;
        const int prio = std::clamp(pp::configGetInt("streamprio", 40), 17, 111);
        s.loaderOn = s.loader.start(useThread, prio) || s.loader.start(false, prio);
    }
    // Group bases (the doodads' filter bits).
    uint32_t base = 0;
    for (auto& in : s.open) {
        in.groupBase = base;
        base += in.wmo->tables().h->groupCount;
    }
    // Finished reads.
    StreamLoader::Result res;
    uint32_t popped = 0;
    while (popped < 32 && s.loader.pop(res)) {
        ++popped;
        if (res.req.kind == kReqWmo) {
            rp::BigWmo* w = nullptr;
            for (auto& in : s.open)
                if (in.wmo->fileId() == res.req.fileId && in.placement == res.req.id) w = in.wmo.get();
            if (w) w->install(res.req.key, res.blob, res.req.size);
            else std::free(res.blob);
        } else if (res.req.kind == kReqDoodad) {
            // bytes in flight of every generation; a result of an abandoned read-ahead is freed.
            s.ddFlightBytes -= std::min(s.ddFlightBytes, res.req.size);
            const bool current = s.nextBuilding && (res.req.key >> 8) == (s.ddGen & 0xFFFFFFu);
            if (current && s.nextPending) --s.nextPending;
            if (!current || !res.blob || !dpak || dpak->fileId() != res.req.fileId) {
                std::free(res.blob);
                continue;
            }
            // A model: its textures next (WMDL batches), then parked for ModelRenderer::model (Pak::load).
            if ((res.req.key & 0xFFu) == 'm') {
                const auto* mh = static_cast<const rp::ModelHeader*>(res.blob);
                if (res.req.size >= sizeof(*mh) && std::memcmp(mh->magic, "WMDL", 4) == 0) {
                    const uint32_t nb = static_cast<uint32_t>(mh->batchCount[0]) + mh->batchCount[1] +
                                        ((mh->flags & rp::kModelLod2) ? mh->lod2BatchCount : 0u);
                    if (mh->batchOffset + nb * sizeof(rp::ModelBatch) <= res.req.size) {
                        const auto* b = reinterpret_cast<const rp::ModelBatch*>(static_cast<const uint8_t*>(res.blob) + mh->batchOffset);
                        for (uint32_t i = 0; i < nb; ++i) {
                            const uint16_t t = b[i].texture;
                            if (t == rp::kNoTexture || models_.textureKnown(t) ||
                                std::find(s.askedTextures.begin(), s.askedTextures.end(), t) != s.askedTextures.end())
                                continue;
                            char nm[16];
                            std::snprintf(nm, sizeof(nm), "x_%u", static_cast<unsigned>(t));
                            if (const pp::PakEntry* e = dpak->find(nm)) {
                                // queued (issued below as there is room) - was pushed at once, and a
                                // full ring lost it after askedTextures had it: read later on the main thread.
                                s.askedTextures.push_back(t);
                                StreamLoader::Request rq;
                                rq.fileId = dpak->fileId();
                                rq.offset = e->offset;
                                rq.size = e->size;
                                rq.kind = kReqDoodad;
                                rq.id = t;
                                rq.key = static_cast<uint32_t>('x') | ((s.ddGen & 0xFFFFFFu) << 8);
                                s.ddQueue.push_back(rq);
                            }
                        }
                    }
                }
            }
            pp::pakPrefetchPut(s.ddPark, res.req.offset, res.blob);  // its own key
            s.ddParked.push_back(res.req.offset);
        } else {
            std::free(res.blob);
        }
    }
    // Low memory (hardware: ~44 MB heap, ~43 used at world entry): a probe of wmoreserve KB, as the tile
    // streamer's streamreserve, twice a second. While it fails: no new reads, and the budget drops to what is
    // resident minus 256 KB (the plan evicts the furthest); it comes back when the probe passes.
    if (now >= s.nextProbeMs) {
        s.nextProbeMs = now + 500;
        // headroom in either pool - or room in the WMOs' own arena (its reads land there); never while the
        // landing hold waits for this WMO's floor (seen on the emulator: bigwmo_lowmem at a teleport into a capital
        // city, the old tiles not evicted yet).
        const bool ok = !s.reserveKb || tpHold_.active || pp::heapGivesKb(s.reserveKb) || pp::wmoArenaHasKb(256);
        if (!ok && !s.lowMem) {
            ++s.lowMemCount;
            PSP_LOGW(pp::tag::Mem, "bigwmo_lowmem reserve_kb=%u heap_kb=%u n=%u", U(s.reserveKb), U(pp::heapUsedKb()),
                     U(s.lowMemCount));
        }
        s.lowMem = !ok;
    }
    // Plan + reads per WMO.
    uint32_t inFlight = s.loader.inFlight();
    for (auto& in : s.open) {
        rp::BigWmo& w = *in.wmo;
        // (Seen on the emulator: a capital city's WMO open from a nearby village - out=1 chunks=0, up to wmobudget
        // of its chunks / textures kept for nothing on the screen): a WMO the camera is outside of that has drawn nothing
        // (no chunk, no shell) for 5 s keeps only its header + 256 KB - the rest goes by the LRU below; in sight again,
        // its budget is back the next frame (the chunks come with the reads, as at its opening).
        const rp::BigWmoStats& ws = w.stats();
        const bool blank = ws.outside && ws.chunks == 0 && ws.shellCells == 0 && !tpHold_.active;
        if (!blank) in.blankSinceMs = 0;
        else if (!in.blankSinceMs) in.blankSinceMs = now | 1u;
        const bool idleBlank = in.blankSinceMs && now - in.blankSinceMs > 5000u;
        if (s.lowMem) {
            const uint32_t held = ws.residentBytes + ws.textureBytes + ws.headerBytes;
            w.cfg.budgetBytes = std::max<uint32_t>(512u * 1024u, held > 256u * 1024u ? held - 256u * 1024u : held);
        } else {
            w.cfg.budgetBytes = s.cfg.budgetBytes;
        }
        if (idleBlank) w.cfg.budgetBytes = std::min(w.cfg.budgetBytes, ws.headerBytes + 256u * 1024u);
        // The landing hold waits for this WMO's floor - its reads go past the low-memory gate.
        const bool landing = tpHold_.active;
        const uint32_t room = s.lowMem && !landing ? 0u : (inFlight < s.maxReads ? s.maxReads - inFlight : 0u);
        w.update(cam, player, dtSec, s.frame, s.reqs, room);
        if (!s.reqs.empty() && !w.ensureFd()) {
            // No descriptor now (hardware: 3-4 free): the reads wait for the next plan (cancel - no
            // read failed, no back-off; ensureFd tries the card at most once a second).
            for (const auto& rq : s.reqs) w.cancel(rq.key);
            s.reqs.clear();
            if (static_cast<unsigned>(w.fdError()) == kErrMFile && !s.fdTight) {
                s.fdTight = true;
                PSP_LOGW(pp::tag::Fs, "bigwmo_fd_tight mfile name=%s pak_fds=%d", w.name(), pp::pakOpenFds());
            }
        }
        for (const auto& rq : s.reqs) {
            StreamLoader::Request r;
            r.fd = w.fd();
            r.fileId = w.fileId();
            r.offset = rq.offset;
            r.size = rq.size;
            r.kind = kReqWmo;
            r.id = in.placement;
            r.key = rq.key;
            // Read into the big WMOs' arena (the chunk / texture blobs of every size used to come
            // and go in newlib: on the emulator, in a capital city, the largest block was 512-1536 KB with 6 MB free). Full: the
            // loader's own newlib block, as before. The blob is the WMO's (free() finds the arena by address).
            r.dst = pp::wmoArenaAlloc((rq.size + 63u) & ~63u, 64);
            r.freeDst = r.dst != nullptr;
            if (!s.loader.push(r)) {
                std::free(r.dst);
                w.cancel(rq.key);  // ring full: asked again later (not a failed read)
                continue;
            }
            ++inFlight;
        }
        if (!s.reqs.empty() || w.inFlight()) in.idleSinceMs = now;
        else if (now - in.idleSinceMs > (s.fdTight ? kIdleFdTightMs : kIdleFdMs)) w.releaseFd();
        if (w.takeDoodadsChanged()) s.nextDirty = true;
    }
    if (!s.loader.threaded()) s.loader.pumpSync();

    // Doodads: a new set when the resident doodad cells changed (at most once a second), its models read ahead.
    // not while memory is low (the set shown stays; tried again when the probe passes).
    if (s.doodadsOn && !s.govCut && dpak && s.nextDirty && !s.nextBuilding && now >= s.nextAtMs && !s.lowMem) {
        s.nextDirty = false;
        s.nextAtMs = now + 1000;
        s.next.clear();
        for (const auto& in : s.open) {
            const rp::BigWmo& w = *in.wmo;
            if (!w.doodadsUsable()) continue;
            const glm::mat4& M = w.placement();
            const glm::mat3 A(M);
            const float sc = std::max({glm::length(A[0]), glm::length(A[1]), glm::length(A[2])});
            w.forDoodads([&](const rp::BigWmoDoodad& d) {
                rp::DoodadPlacement p{};
                p.uniqueId = rp::bigWmoPartId(w.uniqueId(), 0x20000u + d.index);
                p.model = d.model;
                p.reserved = d.group == 0xFFFF ? 0 : static_cast<uint16_t>(std::min<uint32_t>(in.groupBase + d.group + 1u, 0xFFFFu));
                const glm::vec3 pos = glm::vec3(M * glm::vec4(d.position[0], d.position[1], d.position[2], 1.0f));
                const glm::mat3 ax = A * glm::mat3(glm::vec3(d.axes[0], d.axes[1], d.axes[2]), glm::vec3(d.axes[3], d.axes[4], d.axes[5]),
                                                   glm::vec3(d.axes[6], d.axes[7], d.axes[8]));
                for (int a = 0; a < 3; ++a) p.position[a] = pos[a];
                for (int c = 0; c < 3; ++c)
                    for (int r = 0; r < 3; ++r) p.axes[c * 3 + r] = ax[c][r];
                const glm::vec3 sc3 = glm::vec3(M * glm::vec4(d.sphere[0], d.sphere[1], d.sphere[2], 1.0f));
                p.sphere[0] = sc3.x;
                p.sphere[1] = sc3.y;
                p.sphere[2] = sc3.z;
                p.sphere[3] = d.sphere[3] * sc;
                s.next.push_back(p);
            });
        }
        // Read ahead the models the doodad renderer does not have (their collision and impostors too).
        // queued and issued below as the loader has room (was: pushed at once - a full ring lost
        // the rest silently), under a generation (an abandoned read-ahead's results are freed) and its own park key.
        s.dropReadAhead();
        s.ddPark = parkKeyOf(dpak->fileId());
        s.askedModels.clear();
        s.askedTextures.clear();
        uint32_t needBytes = 0, largest = 0;
        for (const auto& p : s.next) {
            if (models_.modelKnown(p.model) || std::find(s.askedModels.begin(), s.askedModels.end(), p.model) != s.askedModels.end())
                continue;
            s.askedModels.push_back(p.model);
            for (char kind : {'m', 'k', 'i'}) {
                char nm[16];
                std::snprintf(nm, sizeof(nm), "%c_%u", kind, static_cast<unsigned>(p.model));
                const pp::PakEntry* e = dpak->find(nm);
                if (!e || pp::pakPrefetchPeek(dpak->fileId(), e->offset)) continue;  // the streamer has it parked
                StreamLoader::Request rq;
                rq.fileId = dpak->fileId();
                rq.offset = e->offset;
                rq.size = e->size;
                rq.kind = kReqDoodad;
                rq.id = p.model;
                rq.key = static_cast<uint32_t>(kind) | ((s.ddGen & 0xFFFFFFu) << 8);
                s.ddQueue.push_back(rq);
                needBytes += e->size;
                largest = std::max(largest, e->size);
            }
        }
        // the new set's models stay as the doodad renderer's - they must fit the heap next to the
        // reserve the probe keeps (wmoreserve); else the set shown stays and the next try is 5 s later.
        if (needBytes && !heapFitsKb(needBytes / 1024u + 1u + s.reserveKb, largest / 1024u + 1u)) {
            if (++s.ddLowMemLogs <= 20)
                PSP_LOGW(pp::tag::Mem, "bigwmo_doodads_lowmem need_kb=%u models=%u heap_kb=%u", U(needBytes / 1024u),
                         U(s.askedModels.size()), U(pp::heapUsedKb()));
            s.dropReadAhead();
            s.nextDirty = true;
            s.nextAtMs = now + 5000;
        } else {
            s.nextBuilding = true;
        }
    }
    // the read-ahead goes out after the WMO's own reads (pushed above), at most kDoodadFlightBytes
    // in flight; a full ring keeps the rest queued for the next frame; low memory abandons it (its blobs freed).
    if (s.nextBuilding && s.lowMem) {
        s.dropReadAhead();
        s.nextDirty = true;
        s.nextAtMs = now + 5000;
    }
    if (s.nextBuilding && dpak) {
        size_t qi = 0;
        for (; qi < s.ddQueue.size(); ++qi) {
            StreamLoader::Request rq = s.ddQueue[qi];
            if (s.ddFlightBytes && s.ddFlightBytes + rq.size > kDoodadFlightBytes) break;
            rq.fd = dpak->fd();
            if (rq.fd < 0 || rq.fileId != dpak->fileId()) continue;  // no descriptor: the renderer reads it itself
            // The WMO's doodad models / textures into the world arena (parked, then the model's).
            rq.dst = pp::worldArenaAlloc((rq.size + 63u) & ~63u, 64);
            rq.freeDst = rq.dst != nullptr;
            if (!s.loader.push(rq)) {
                std::free(rq.dst);
                break;
            }
            s.ddFlightBytes += rq.size;
            ++s.nextPending;
        }
        s.ddQueue.erase(s.ddQueue.begin(), s.ddQueue.begin() + static_cast<std::ptrdiff_t>(qi));
    }
    if (s.nextBuilding && s.nextPending == 0 && s.ddQueue.empty() && dpak) {
        s.nextBuilding = false;
        const int newTile = s.doodadTile ? kDoodadTileA : kDoodadTileB, oldTile = s.doodadTile ? kDoodadTileB : kDoodadTileA;
        // the parked blobs under the pak's own key for setPlacements (Pak::load takes them there);
        // a blob the streamer parked for its own tile is kept (it serves as well), ours freed; after the call what
        // it did not take is freed (it was dropped by the next tile's pakPrefetchDrop before).
        const uint32_t fid = dpak->fileId();
        size_t kept = 0;
        for (uint32_t off : s.ddParked) {
            void* b = pp::pakPrefetchTake(s.ddPark, off);
            if (!b) continue;
            if (pp::pakPrefetchPeek(fid, off)) {
                std::free(b);
                continue;
            }
            pp::pakPrefetchPut(fid, off, b);
            s.ddParked[kept++] = off;
        }
        s.ddParked.resize(kept);
        models_.setPlacements(*dpak, newTile, newTile, s.next.data(), static_cast<uint32_t>(s.next.size()));
        for (uint32_t off : s.ddParked)
            if (void* b = pp::pakPrefetchTake(fid, off)) std::free(b);
        s.ddParked.clear();
        models_.unloadTile(*dpak, oldTile, oldTile);
        models_.flushGrid();
        s.doodadTile ^= 1u;
        s.placed.swap(s.next);
        if (++s.doodadLogs <= 40)
            PSP_LOGI(pp::tag::Gfx, "bigwmo_doodads n=%u models_read=%u heap_kb=%u", static_cast<unsigned>(s.placed.size()),
                     static_cast<unsigned>(s.askedModels.size()), static_cast<unsigned>(pp::heapUsedKb()));
    }
    // The 10 s line.
    if (now >= s.nextLogMs) {
        s.nextLogMs = now + 10000;
        for (const auto& in : s.open) {
            const rp::BigWmoStats& st = in.wmo->stats();
            PSP_LOGI(pp::tag::Gfx,
                     "bigwmo_frame name=%s grp=%d out=%d vis=%u/%u walked=%u chunks=%u near=%u lod=%u shells=%u miss=%u "
                     "tris=%u draws=%u binds=%u mats=%u cpu_us=%u res_kb=%u tex_kb=%u col_kb=%u hdr_kb=%u reads=%u evict=%u "
                     "cut=%u inflight=%u fd=%d heap_kb=%u gskip=%u bad=%u fdtight=%d",
                     in.wmo->name(), st.camGroup, st.outside ? 1 : 0, U(st.visibleGroups),
                     U(in.wmo->tables().h->groupCount), U(st.walked), U(st.chunks), U(st.nearChunks), U(st.lodChunks),
                     U(st.shellCells), U(st.missing), U(st.triangles), U(st.draws), U(st.binds), U(st.matrices),
                     U(st.cpuUs), U(st.residentBytes / 1024u), U(st.textureBytes / 1024u), U(st.collisionBytes / 1024u),
                     U(st.headerBytes / 1024u), U(st.reads), U(st.evictions), U(st.overBudget), U(in.wmo->inFlight()),
                     in.wmo->fd(), U(pp::heapUsedKb()), U(st.guardSkipped), U(st.badBlobs),
                     s.fdTight ? 1 : 0);
            // the interior groups' batches drawn by their baked light (no day/night tint) - a line of
            // its own (bigwmo_frame is cut at the log's line length). inlit=1: some were drawn this frame.
            const int cg = st.camGroup;
            const bool camIn = cg >= 0 && static_cast<uint32_t>(cg) < in.wmo->tables().h->groupCount &&
                               (in.wmo->tables().groups[cg].flags & rp::kWmoGroupInterior) != 0;
            PSP_LOGI(pp::tag::Gfx, "bigwmo_light name=%s inlit=%d cam_in=%d in=%u tint=%u", in.wmo->name(),
                     st.interiorDraws ? 1 : 0, camIn ? 1 : 0, U(st.interiorDraws), U(st.tintedDraws));
        }
    }
}

// the memory governor's third step - the big WMOs'
// doodads (both pseudo-tiles, the read-ahead) dropped and no new set built until it gives the step back (then the
// next update builds one). The WMOs themselves stay (their budget is wmobudget). PSP only. KB freed (unloadTile).
uint32_t WorldScreen::bigWmoDoodadsGovernor(bool cut) {
    if (!bigWmo_) return 0;
    BigWmoState& s = *bigWmo_;
    s.govCut = cut;
    s.nextDirty = true;  // given back: a set at the next update; cut: nothing is built meanwhile
    if (!cut) {
        PSP_LOGI(pp::tag::Mem, "bigwmo_doodads_governor on");
        return 0;
    }
    uint32_t freed = 0;
    const size_t n = s.placed.size();
    if (s.nextBuilding || !s.ddParked.empty()) s.dropReadAhead();
    if (pp::Pak* dpak = stream_.doodadPak()) {
        freed += models_.unloadTile(*dpak, kDoodadTileA, kDoodadTileA);
        freed += models_.unloadTile(*dpak, kDoodadTileB, kDoodadTileB);
        models_.flushGrid();
    }
    s.placed.clear();
    // An open WMO that draws nothing (no chunk, no shell - out of sight or held by bigwmo_lowmem) with the
    // camera outside it closes too: its "!wmo" header (a capital city's ~0.7 MB, open from a nearby village - its box
    // reaches within 52 yd) went to nothing while the NPCs next to the player had no room. updateBigWmos opens it
    // again only with the camera inside its box while this step is cut.
    uint32_t closed = 0, headerKb = 0;
    for (size_t i = 0; i < s.open.size();) {
        rp::BigWmo& w = *s.open[i].wmo;
        const rp::BigWmoStats& st = w.stats();
        if (st.outside && st.chunks == 0 && st.shellCells == 0 && w.inFlight() == 0) {
            const uint32_t before = pp::heapUsedBytes();
            headerKb += st.headerBytes / 1024u;
            PSP_LOGI(pp::tag::Gfx, "bigwmo_close name=%s governor=1", w.name());
            s.open.erase(s.open.begin() + static_cast<std::ptrdiff_t>(i));
            const uint32_t after = pp::heapUsedBytes();
            freed += before > after ? before - after : 0u;
            ++s.closes;
            ++closed;
            continue;
        }
        ++i;
    }
    if (closed && s.open.empty() && (s.nextBuilding || !s.ddParked.empty())) s.dropReadAhead();
    PSP_LOGI(pp::tag::Mem, "bigwmo_doodads_governor off n=%u closed=%u header_kb=%u freed_kb=%u heap_kb=%u",
             static_cast<unsigned>(n), U(closed), U(headerKb), static_cast<unsigned>(freed / 1024u),
             static_cast<unsigned>(pp::heapUsedKb()));
    return freed / 1024u;
}

// world_flush: every big WMO closed - the loader stopped first (its requests' arena
// blocks back), the read-ahead and both doodad pseudo-tiles dropped, every open WMO's chunks / textures / header freed.
// The placements index stays; updateBigWmos opens what the new place has in reach (as at world entry). KB freed.
uint32_t WorldScreen::bigWmoFlush() {
    if (!bigWmo_) return 0;
    BigWmoState& s = *bigWmo_;
    const uint32_t before = pp::heapUsedBytes();
    s.loader.stop();
    s.loaderOn = false;
    s.dropReadAhead();
    s.ddFlightBytes = 0;  // the reads in flight went with the loader
    if (pp::Pak* dpak = stream_.doodadPak()) {
        models_.unloadTile(*dpak, kDoodadTileA, kDoodadTileA);
        models_.unloadTile(*dpak, kDoodadTileB, kDoodadTileB);
        models_.flushGrid();
    }
    s.placed.clear();
    s.next.clear();
    s.mask.clear();
    s.askedModels.clear();
    s.askedTextures.clear();
    s.reqs.clear();
    s.nextDirty = true;
    s.closes += static_cast<uint32_t>(s.open.size());
    for (const auto& in : s.open) PSP_LOGI(pp::tag::Gfx, "bigwmo_close name=%s flush=1", in.wmo->name());
    s.open.clear();  // ~BigWmo: chunks, textures, collision, header, descriptor
    std::fill(s.openRetryMs.begin(), s.openRetryMs.end(), 0u);
    std::fill(s.farWaitSinceMs.begin(), s.farWaitSinceMs.end(), 0u);
    pp::wmoArenaSetBigOpen(0);
    const uint32_t after = pp::heapUsedBytes();
    return before > after ? (before - after) / 1024u : 0u;
}

// near_starve_flush: the open big WMOs the camera is outside of that drew nothing for 5 s (blank:
// see updateBigWmos) close - not opened again for retryMs. KB freed.
uint32_t WorldScreen::bigWmoCloseBlank(uint32_t retryMs) {
    if (!bigWmo_) return 0;
    BigWmoState& s = *bigWmo_;
    const uint32_t now = pp::logUptimeMs();
    const uint32_t before = pp::heapUsedBytes() + pp::worldArenaInfo().usedKb * 1024u;
    for (size_t i = 0; i < s.open.size();) {
        BigWmoState::Inst& in = s.open[i];
        if (in.blankSinceMs && now - in.blankSinceMs > 5000u && in.wmo->inFlight() == 0) {
            PSP_LOGI(pp::tag::Gfx, "bigwmo_close name=%s blank=1", in.wmo->name());
            if (in.placement < s.openRetryMs.size()) s.openRetryMs[in.placement] = (now + retryMs) | 1u;
            s.open.erase(s.open.begin() + static_cast<std::ptrdiff_t>(i));
            ++s.closes;
            s.nextDirty = true;
            continue;
        }
        ++i;
    }
    pp::wmoArenaSetBigOpen(static_cast<uint32_t>(s.open.size()));
    const uint32_t after = pp::heapUsedBytes() + pp::worldArenaInfo().usedKb * 1024u;
    return before > after ? (before - after) / 1024u : 0u;
}

// World entry, after startStreaming: a big WMO the player stands in gets its near chunks and collision now, under
// the loading screen (synchronous reads) - else the first frames would fall through its floors.
void WorldScreen::primeBigWmos() {
    if (!bigWmo_) return;
    BigWmoState& s = *bigWmo_;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    s.priming = true;  // only the one the camera is in (the rest after the world shows, with room)
    updateBigWmos(0.0f);  // opens what is within reach
    s.priming = false;
    {
        // the descriptors left for the rest of the game once the WMOs within reach are open (the
        // world entry's fd_probe line, at most 8 opens of audio.pak) - updateBigWmos goes "tight" when they are used up.
        char probe[256];
        pp::fsPath(probe, sizeof(probe), "data/audio.pak");
        s.fdFree = pp::pakProbeFreeFiles(probe, 8);
        s.fdBase = pp::pakOpenFds();
        PSP_LOGI(pp::tag::Fs, "bigwmo_fd free=%d pak_fds=%d open=%u", s.fdFree, s.fdBase, U(s.open.size()));
    }
    uint32_t reads = 0, kb = 0;
    for (auto& in : s.open) {
        rp::BigWmo& w = *in.wmo;
        const glm::vec3 player(playerX_, playerY_, playerZ_);
        // Rounds of plan + read until nothing is asked (or 4 s of the loading screen).
        for (int round = 0; round < 64 && sceKernelGetSystemTimeWide() - t0 < 4000000u; ++round) {
            std::vector<rp::BigWmo::Request> rq;
            // The frame stays (groups count as recently seen at world entry: the nearest chunks of every group).
            w.update(player + glm::vec3(0.0f, 0.0f, 2.0f), player, 0.0f, s.frame, rq, 64);
            if (rq.empty()) break;
            if (!w.ensureFd()) {
                // the round's requests back (was: lost with requested set - inFlight never reached
                // 0, so the WMO never closed and its descriptor never went back).
                for (const auto& r : rq) w.cancel(r.key);
                break;
            }
            for (const auto& r : rq) {
                void* blob = pp::wmoArenaAlloc((r.size + 63u) & ~63u, 64);  // the WMOs' arena first
                if (!blob) blob = memalign(64, (r.size + 63u) & ~63u);
                if (blob && pp::pakRead(w.fd(), r.offset, blob, r.size)) {
                    sceKernelDcacheWritebackRange(blob, (r.size + 63u) & ~63u);
                    w.install(r.key, blob, r.size);
                    ++reads;
                    kb += r.size / 1024u;
                } else {
                    std::free(blob);
                    w.install(r.key, nullptr, 0);
                }
            }
        }
        PSP_LOGI(pp::tag::Gfx, "bigwmo_prime name=%s reads=%u kb=%u ms=%u", w.name(), static_cast<unsigned>(reads),
                 static_cast<unsigned>(kb), static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    }
}

// Before the M2 pass: the WMOs (opaque, alpha-tested, blended), then the doodads' group filter.
void WorldScreen::drawBigWmos(const rp::TerrainCamera& cam, uint32_t fogABGR) {
    models_.groupFilter = nullptr;
    if (!bigWmo_ || bigWmo_->open.empty()) return;
    BigWmoState& s = *bigWmo_;
    uint32_t bits = 0;
    for (auto& in : s.open) {
        in.wmo->draw(cam, fogABGR);
        bits = std::max(bits, in.groupBase + in.wmo->tables().h->groupCount);
    }
    s.mask.assign((bits + 31u) / 32u, 0u);
    for (const auto& in : s.open) {
        const rp::PortalVisibility& v = in.wmo->visibility();
        const uint32_t n = in.wmo->tables().h->groupCount;
        for (uint32_t g = 0; g < n; ++g)
            if (v.isVisible(g)) s.mask[(in.groupBase + g) >> 5] |= 1u << ((in.groupBase + g) & 31u);
    }
    models_.groupFilter = s.mask.data();
    models_.groupFilterBits = bits;
}

// The landing hold: inside a big WMO's box - its collision around the player must be resident first.
void WorldScreen::bigWmoLanding(float x, float y, float z, bool& inside, bool& ready, uint32_t& resident,
                                uint32_t& wanted) {
    inside = ready = false;
    resident = wanted = 0;
    if (!bigWmo_) return;
    BigWmoState& s = *bigWmo_;
    const glm::vec3 p(x, y, z);
    for (uint32_t pi = 0; pi < s.placements.size(); ++pi) {
        if (s.dead[pi] || boxYards(s.placements[pi], p) > 0.0f) continue;
        inside = true;
        for (const auto& in : s.open) {
            if (in.placement != pi) continue;
            uint32_t w = 0, r = 0;
            ready = in.wmo->collisionReadyAt(x, y, 12.0f, w, r);
            wanted += w;
            resident += r;
            if (!ready) return;
        }
        if (!ready) return;  // not open yet (updateBigWmos opens the one the player is in without a probe)
    }
}

bool WorldScreen::bigWmoFloor(float x, float y, float probeZ, float& outZ) const {
    if (!bigWmo_) return false;
    bool found = false;
    float best = -1e30f;
    for (const auto& in : bigWmo_->open) {
        float z;
        if (in.wmo->floorHeight(x, y, probeZ, z) && z > best) {
            best = z;
            found = true;
        }
    }
    if (found) outZ = best;
    return found;
}

void WorldScreen::bigWmoWalls(glm::vec3& to, float feetZ) const {
    if (!bigWmo_) return;
    for (const auto& in : bigWmo_->open) in.wmo->collideWalls(to, feetZ);
}

float WorldScreen::bigWmoRay(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const {
    float best = maxDist;
    if (!bigWmo_) return best;
    for (const auto& in : bigWmo_->open) best = std::min(best, in.wmo->raycast(origin, dir, best));
    return best;
}

}  // namespace wowee::ui::psp
