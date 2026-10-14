// The memory governor (psp_mem_governor.hpp) and the
// descriptor budget's per-frame work (psp_fd_budget.hpp) for the world screen. PSP only - the original client has neither:
// it is a 32-bit Windows program with gigabytes to spare and no limit on open files worth the name.
//
// Twice a second one heap probe (govcut KB); short of memory the next step is cut, in this order:
//   1 particles     the world's emitters closed (WorldParticles: cache, directories, descriptor)  particlesGovernor
//   2 attachments   other characters' item models: attachcap down to govattachpct %, the farthest give way
//                   (remoteCrowdTurn sheds), what nobody holds freed (attachmentCacheTrim)
//   3 wmo_doodads   the big WMOs' doodads dropped, no new set                                       bigWmoDoodadsGovernor
//   4 npc_distance  NPC view distance and model budget down to govnpcpct % (their LRU frees the rest)
// and the last cut step comes back (4 first) when a probe of govrestore KB + what that step freed has passed for 10 s.
// While anything is cut the tile streamer reads no deferred doodads (setTopUp): the room goes to the cut steps first.
// The probes are headroom (total free + one block of
// min(n, 1 MB)), and a consumer of higher priority that waited for memory since the last tick cuts too - the ground
// (a terrain tile the streamer could not start, chunk textures without a block) up to step 4, the NPCs near the
// player (reads held, units waiting) up to step 3 - and holds every restore while it waits (psp_mem_governor.hpp).
//
// wowee.cfg (read at the first world update):
//   memgov=1          0 = never cuts (the probes and the status line stay)
//   govcut=2048       KB of headroom a probe must get, else the next step is cut (2048, was 1024 of one block)
//   govrestore=4096   KB (+ what the step freed) a probe must get for 10 s to give the last step back (was 3072)
//   govemerg=768      KB: not even this - the next step at once, no cut gap (768, was 384)
//   govstarve=1       a waiting consumer of higher priority cuts lower ones (0 = only the probe cuts)
//   govfragkb=1024    free >= 4 x govcut but the largest block under this - the heap is fragmented, not
//                     full: no more cuts (they free small pieces between pins, nothing merges), restores as the probe
//                     allows; one mem_frag line per episode (an emulator run sat at level 4 for minutes)
//   govmax=4          the deepest step that may be cut (0 = none)
//   govattachpct=50   attachcap while step 2 is cut (% of attachcap)
//   govnpcpct=60      NPC view distance / model budget while step 4 is cut (% of npcview / npcbudget)
//   fdidle=5000       ms without a read before creatures.pak gives its descriptor back (npcstatic=0; 0 = held)
// Log: mem_governor ev=config|cut|restore|status ...; the steps' own lines: particles_governor, attach_governor,
// bigwmo_doodads_governor, npc_governor; the descriptors: fd_budget (world entry), fd_relieve, pak_open_list.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_mem_governor.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/psp/psp_attachments.hpp"

#include <algorithm>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {

unsigned U(uint64_t v) { return static_cast<unsigned>(v); }  // %u of the PSP's uint32_t (long unsigned)

// The probes and the "heap in use" of the big-block heap (psp_bigheap.hpp). A governor probe passes when
// the headroom is there (pp::heapGivesKb: a block in the region or newlib, or newlib's total free) AND newlib has that
// many KB free in total (the doodads / NPCs / UI it protects are small objects) - never a contiguous demand on newlib
// while the region has room. heapUsedKb counts both pools. bigheap=0 (memory diet): the total free + one block
// of min(n, 1 MB) (was one malloc of n).
bool heapGivesKb(uint32_t kb) { return pp::heapGivesKb(kb) && pp::heapSmallRoomKb(kb); }

uint32_t heapUsedKb() { return pp::heapUsedKb(); }

}  // namespace

struct WorldScreen::MemGovState {
    pp::MemGovernor gov;
    uint32_t attachPct = 50, npcPct = 60, fdIdleMs = 5000;
    bool npcStatic = false;
    // Step 4: the NPC settings when it was cut, and what the cut set (given back only if nobody changed them since).
    float npcViewBase = 0.0f, npcViewCut = 0.0f;
    uint32_t npcBudgetBase = 0, npcBudgetCut = 0;
    uint32_t nextLogMs = 0;
    bool lastCutOk = true;
    uint32_t changeLogs = 0;
    // who waited for memory since the last tick - the NPCs' held frames, the textures' failed blocks.
    uint32_t lastHeapHeld = 0, lastTexNomem = 0, starveNpc = 0, starveTerrain = 0;
    uint32_t fragKb = 1024, fragEpisodes = 0, lastLargestKb = 0;
    bool frag = false;
    // near_starve_flush - the doodads in view waited nearstarvems with nothing in flight for them.
    uint32_t nearStarveMs = 3000, nearFlushNextMs = 0;
};

void WorldScreen::destroyMemGovState(MemGovState* p) {
    rp::setAttachmentGovernorCap(0);  // the cap is the attachments module's (static): not past this world
    delete p;
}

// The step's own action (cut = true: cut; false: give back). KB freed now (0 for a step that frees over the next
// frames: the crowd's give-ways, the NPC cache's LRU).
uint32_t WorldScreen::memGovernorApply(uint8_t step, bool cut) {
    MemGovState& s = *memGov_;
    switch (step) {
        case pp::kMemStepParticles:
            return particlesGovernor(cut);
        case pp::kMemStepAttachments: {
            if (!cut) {
                rp::setAttachmentGovernorCap(0);
                PSP_LOGI(pp::tag::Mem, "attach_governor on cap_kb=%u in_use_kb=%u", U(rp::attachmentCapBytes() / 1024u),
                         U(rp::attachmentInUseBytes() / 1024u));
                return 0;
            }
            rp::setAttachmentGovernorCap(0);
            const uint32_t base = rp::attachmentCapBytes();
            const uint32_t govCap = std::max<uint32_t>(64u * 1024u, base / 100u * s.attachPct);
            rp::setAttachmentGovernorCap(govCap);
            const uint32_t before = heapUsedKb();
            rp::attachmentCacheTrim();  // what nobody holds; the holders past the cap give way (remoteCrowdTurn)
            const uint32_t after = heapUsedKb();
            PSP_LOGI(pp::tag::Mem, "attach_governor off cap_kb=%u->%u in_use_kb=%u trimmed_kb=%u", U(base / 1024u),
                     U(govCap / 1024u), U(rp::attachmentInUseBytes() / 1024u), U(before > after ? before - after : 0u));
            return before > after ? before - after : 0u;
        }
        case pp::kMemStepWmoDoodads:
            return bigWmoDoodadsGovernor(cut);
        case pp::kMemStepNpcDistance: {
            if (!cut) {
                // Given back as it was - unless the settings menu set new values meanwhile (those stay).
                if (creatures_.viewDistance == s.npcViewCut) creatures_.viewDistance = s.npcViewBase;
                if (creatures_.memoryBudgetKb == s.npcBudgetCut) creatures_.memoryBudgetKb = s.npcBudgetBase;
                PSP_LOGI(pp::tag::Mem, "npc_governor on view=%.0f budget_kb=%u npc_kb=%u", creatures_.viewDistance,
                         U(creatures_.memoryBudgetKb), U(creatures_.stats().memoryKb));
                return 0;
            }
            s.npcViewBase = creatures_.viewDistance;
            s.npcBudgetBase = creatures_.memoryBudgetKb;
            s.npcViewCut = std::max(20.0f, s.npcViewBase * static_cast<float>(s.npcPct) / 100.0f);
            s.npcBudgetCut = std::max<uint32_t>(1024u, s.npcBudgetBase / 100u * s.npcPct);
            creatures_.viewDistance = s.npcViewCut;
            creatures_.memoryBudgetKb = s.npcBudgetCut;
            PSP_LOGI(pp::tag::Mem, "npc_governor off view=%.0f->%.0f budget_kb=%u->%u npc_kb=%u", s.npcViewBase,
                     s.npcViewCut, U(s.npcBudgetBase), U(s.npcBudgetCut), U(creatures_.stats().memoryKb));
            return 0;
        }
        default:
            return 0;
    }
}

void WorldScreen::updateMemoryGovernor(float dtSec) {
    (void)dtSec;
    if (!memGov_) {
        auto* p = new (std::nothrow) MemGovState;
        if (!p) return;
        memGov_ = std::unique_ptr<MemGovState, void (*)(MemGovState*)>(p, &WorldScreen::destroyMemGovState);
        pp::MemGovernorConfig c;
        c.on = pp::configGetInt("memgov", 1) != 0;
        // 2048 / 4096 / 768 for the device's 41.3 MB heap (were 1024 / 3072 / 384 -
        // the autopilot used to set these by hand); the probes are headroom now, not one block.
        c.cutKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("govcut", 2048), 64, 16384));
        c.restoreKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("govrestore", 4096), 128, 32768));
        c.emergencyKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("govemerg", 768), 0, 16384));
        c.starve = pp::configGetInt("govstarve", 1) != 0;
        if (c.restoreKb <= c.cutKb) c.restoreKb = c.cutKb + 512u;  // the restore line stays above the cut line
        c.maxLevel = static_cast<uint8_t>(std::clamp(pp::configGetInt("govmax", pp::kMemStepCount), 0,
                                                     static_cast<int>(pp::kMemStepCount)));
        p->gov.configure(c);
        p->attachPct = static_cast<uint32_t>(std::clamp(pp::configGetInt("govattachpct", 50), 10, 100));
        p->npcPct = static_cast<uint32_t>(std::clamp(pp::configGetInt("govnpcpct", 60), 20, 100));
        p->fdIdleMs = static_cast<uint32_t>(std::clamp(pp::configGetInt("fdidle", 5000), 0, 600000));
        p->npcStatic = pp::configGetInt("npcstatic", 0) != 0;
        p->fragKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("govfragkb", 1024), 0, 16384));
        p->nearStarveMs = static_cast<uint32_t>(std::clamp(pp::configGetInt("nearstarvems", 3000), 0, 600000));  // B121
        PSP_LOGI(pp::tag::Mem, "mem_governor ev=config on=%d cut_kb=%u restore_kb=%u emerg_kb=%u starve=%d max=%u "
                 "attach_pct=%u npc_pct=%u fd_cap=%d fd_free=%d heap_kb=%u",
                 c.on ? 1 : 0, U(c.cutKb), U(c.restoreKb), U(c.emergencyKb), c.starve ? 1 : 0,
                 U(p->gov.config().maxLevel), U(p->attachPct), U(p->npcPct), pp::pakFdCapacity(), pp::pakFdFree(),
                 U(heapUsedKb()));
    }
    MemGovState& s = *memGov_;
    PROF_SCOPE("memgov");
    const uint32_t now = pp::logUptimeMs();
    // Descriptors: an open that found none on another thread - a rare pak gives its back now (main thread); the
    // creatures pak is one of them without the DB spawns.
    pp::pakFdService();
    if (!s.npcStatic && s.fdIdleMs) creatures_.releaseIdleFd(now, s.fdIdleMs);
    // The tile streamer's top-up jobs (deferred doodads) only while nothing is cut.
    stream_.setTopUp(s.gov.level() == 0);
    // (Seen on the emulator: a village's own tile kept 96 placements in view deferred - free 4.9 MB,
    // a same-window teleport, so no world_flush): what the player can see waited nearstarvems with nothing in flight
    // for it - memory held for what he cannot see goes (near_starve_flush), at most every 10 s.
    if (s.nearStarveMs && stream_.nearStarvedMs() > s.nearStarveMs && static_cast<int32_t>(now - s.nearFlushNextMs) >= 0) {
        s.nearFlushNextMs = now + 10000u;
        nearStarveFlush();
    }
    // Step 4 cut and the settings menu changed the NPC distance / budget: cut from the new values.
    if (s.gov.level() >= pp::kMemStepNpcDistance &&
        (creatures_.viewDistance != s.npcViewCut || creatures_.memoryBudgetKb != s.npcBudgetCut)) {
        memGovernorApply(pp::kMemStepNpcDistance, true);
    }
    if (s.gov.due(now)) {
        bool cutOk = heapGivesKb(s.gov.cutProbeKb());
        const uint32_t restoreKb = s.gov.restoreProbeKb();
        const bool restoreOk = cutOk && restoreKb && heapGivesKb(restoreKb);
        const bool emergencyOk = cutOk || heapGivesKb(s.gov.emergencyProbeKb());
        // who waited since the last tick - the ground first (a terrain job the streamer could not
        // start, a chunk texture without its block), then the NPCs near the player (a read held, units waiting).
        uint8_t starveMax = 0;
        const char* why = cutOk ? "starve" : "probe";
        const uint32_t texNomem = terrain_.texNomemCount(), heapHeld = creatures_.stats().heapHeld;
        // A landing hold waiting over a second for the floor counts as the ground waiting too.
        const bool landingWaits = tpHold_.active && now - tpHold_.startMs > 1000u;
        if (stream_.takeTerrainStarved() || texNomem != s.lastTexNomem || landingWaits) {
            starveMax = pp::starveStepFor(pp::kPrioTerrain);
            ++s.starveTerrain;
            if (cutOk) why = "terrain";
        } else if (heapHeld != s.lastHeapHeld && creatures_.stats().waiting > 0) {
            starveMax = pp::starveStepFor(pp::kPrioNpcNear);
            ++s.starveNpc;
            if (cutOk) why = "npc";
        }
        s.lastTexNomem = texNomem;
        s.lastHeapHeld = heapHeld;
        // fragmented, not full - plenty free in total, no big block. Cutting more frees small pieces
        // between the pins; nothing merges. No cuts while it lasts (restores as the probe allows), one line an episode.
        // (Seen on the emulator in a capital city: 7.7 MB free, largest 384 KB for 90 s and no mem_frag - the check ran
        // only when the probe failed or someone starved): every tick, free >= 2 x govcut.
        if (s.fragKb) {
            const uint32_t freeKb = pp::heapFreeKb();
            s.lastLargestKb = pp::heapLargestKb();
            const bool frag = freeKb >= 2u * s.gov.cutProbeKb() && s.lastLargestKb < s.fragKb;
            if (frag && !s.frag && ++s.fragEpisodes <= 50)
                PSP_LOGW(pp::tag::Mem, "mem_frag free_kb=%u largest_kb=%u level=%u heap_kb=%u n=%u (no cuts while it lasts)",
                         U(freeKb), U(s.lastLargestKb), U(s.gov.level()), U(heapUsedKb()), U(s.fragEpisodes));
            if (!frag && s.frag)
                PSP_LOGI(pp::tag::Mem, "mem_frag_end free_kb=%u largest_kb=%u", U(freeKb), U(s.lastLargestKb));
            s.frag = frag;
        }
        if (s.frag) {
            cutOk = true;
            starveMax = 0;
        }
        const pp::MemDecision d = s.gov.decide(now, cutOk, restoreOk, emergencyOk, starveMax);
        s.lastCutOk = cutOk;
        if (d.dir) {
            const uint32_t before = heapUsedKb();
            const uint32_t freedKb = memGovernorApply(d.step, d.dir > 0);
            if (d.dir > 0)
                s.gov.noteResult(d.step, freedKb,
                                 d.step == pp::kMemStepAttachments || d.step == pp::kMemStepNpcDistance);
            if (++s.changeLogs <= 200)
                PSP_LOGI(pp::tag::Mem, "mem_governor ev=%s why=%s level=%u step=%s probe_kb=%u heap_kb=%u->%u "
                         "freed_kb=%u free_kb=%u cuts=%u restores=%u",
                         d.dir > 0 ? "cut" : "restore", d.dir > 0 ? why : "room", U(s.gov.level()),
                         pp::memStepName(d.step), U(d.dir > 0 ? s.gov.cutProbeKb() : restoreKb), U(before),
                         U(heapUsedKb()), U(freedKb), U(pp::heapFreeKb()), U(s.gov.cuts()), U(s.gov.restores()));
        }
    }
    if (static_cast<int32_t>(now - s.nextLogMs) >= 0) {
        s.nextLogMs = now + 10000u;
        pp::bigheapPoll();  // bigheap_full / bigheap_spill, once each (the pools: mem_watch's bigheap line)
        uint32_t mfiles = 0, relieved = 0;
        int rare = 0;
        pp::pakFdStats(mfiles, relieved, rare);
        // The sharing in a line of its own (the status line is cut at 120 characters already).
        PSP_LOGI(pp::tag::Mem, "mem_share free_kb=%u largest_kb=%u frag=%d guard=%d starve_low=%u starve_terrain=%u "
                 "starve_npc=%u starve_cuts=%u npc_held=%u tex_nomem=%u",
                 U(pp::heapFreeKb()), U(pp::heapLargestKb()), s.frag ? 1 : 0, pp::heapLargestKb() >= pp::heapGuardKb() ? 1 : 0,
                 U(stream_.lowStarves() + pp::heapGuardRefusals()), U(s.starveTerrain), U(s.starveNpc),
                 U(s.gov.starveCuts()), U(creatures_.stats().heapHeld), U(terrain_.texStats().nomem));
        pp::wmoArenaLog();     // the big WMOs' arena
        pp::heapBigStatsLog();  // newlib blocks >= 64 KB alive by size class (heapbigstats)
        PSP_LOGI(pp::tag::Mem, "mem_governor ev=status level=%u cut_ok=%d heap_kb=%u ticks=%u low=%u emerg=%u cuts=%u "
                 "restores=%u deferred=%u topups=%u attach_kb=%u/%u npc_kb=%u fd_open=%d fd_free=%d fd_rare=%d mfile=%u "
                 "relieved=%u",
                 U(s.gov.level()), s.lastCutOk ? 1 : 0, U(heapUsedKb()), U(s.gov.ticks()), U(s.gov.lowTicks()),
                 U(s.gov.emergencyTicks()),
                 U(s.gov.cuts()), U(s.gov.restores()), U(models_.deferredTotal()), U(stream_.topUps()),
                 U(rp::attachmentInUseBytes() / 1024u), U(rp::attachmentCapBytes() / 1024u),
                 U(creatures_.stats().memoryKb), pp::pakOpenFds(), pp::pakFdFree(), rare, U(mfiles), U(relieved));
    }
}

// near_starve_flush - see ui_world_screen.hpp.
void WorldScreen::nearStarveFlush() {
    const uint32_t heapBefore = heapUsedKb();
    const pp::WorldArenaInfo a0 = pp::worldArenaInfo();
    // NPC models / textures / skins no unit drew for ~3 s (out of view or range); the in-view ones stay.
    const uint32_t npcKb = creatures_.releaseUnused(90) / 1024u;
    if (creatures_.isOpen()) pp::pakPrefetchDrop(creatures_.pakFileId());  // the NPCs' read-ahead
    // A big WMO the camera is outside of that has drawn nothing for 5 s: closed (not opened again for a minute).
    const uint32_t wmoKb = bigWmoCloseBlank(60000u);
    stream_.nearStarveReset();
    const uint32_t heapAfter = heapUsedKb();
    const pp::WorldArenaInfo a1 = pp::worldArenaInfo();
    PSP_LOGI(pp::tag::Mem, "near_starve_flush freed_kb=%u npc_kb=%u wmo_kb=%u arena_kb=%u->%u free_kb=%u largest_kb=%u",
             U(heapBefore > heapAfter ? heapBefore - heapAfter : 0u), U(npcKb), U(wmoKb), U(a0.usedKb), U(a1.usedKb),
             U(pp::heapFreeKb()), U(pp::heapLargestKb()));
}

// world_flush - see ui_world_screen.hpp. Main thread, from applyTeleport (between
// frames: the GE finished the last frame's reads at frameEnd's sceGuSync).
void WorldScreen::worldFlush(const char* why, float distYd) {
    if (!stream_.active()) {  // stream=0: nobody would load the new place - keep what world entry loaded
        PSP_LOGI(pp::tag::Mem, "world_flush why=%s skipped=1 (no streamer)", why);
        return;
    }
    const uint32_t t0 = pp::logUptimeMs();
    const uint32_t heapBefore = heapUsedKb();
    const pp::WorldArenaInfo a0 = pp::worldArenaInfo();
    // 1. The big WMOs (their doodads are two pseudo-tiles of models_; their reads in their own loader).
    const uint32_t wmoKb = bigWmoFlush();
    // 2. The game objects' models (rebuilt for the objects in range by the next updateGameObjects).
    uint32_t goKb = 0;
    if (goState_ > 0) {
        goKb = goModels_.memoryKb();
        goModels_.clear();
        goModels_.reserveTables(goPak_);
        goPlaced_.clear();
        goTile_ = 0;
        goNextMs_ = 0;
        goSeenVersion_ = ~0u;
    }
    // 3. The NPC models' cache (after a far teleport no unit of the old place is in range) and their read-ahead.
    const uint32_t npcKb = creatures_.releaseModels() / 1024u;
    if (creatures_.isOpen()) pp::pakPrefetchDrop(creatures_.pakFileId());
    // 4. The world's emitters (the new place's open with the next update).
    const uint32_t fxKb = particlesFlush();
    // 5. The streamer: its loader, parked blobs, job and tiles; the terrain and the doodads emptied.
    TileStreamer::Env env{terrain_, models_, creatures_, session_.worldEntry().mapId,
                          creatures_.isOpen() && !liveCreatures_, playerX_, playerY_};
    const uint32_t tiles = stream_.flush(env);
    // 6. Everything in the terrain pool and the world arena belonged to the old place: both must be empty now.
    uint32_t pt = 0, pl = 0, pr = 0;
    rp::TerrainRenderer::poolUsage(pt, pl, pr);
    const uint32_t poolLost = rp::TerrainRenderer::poolReset();
    char leak[160];
    const uint32_t live = pp::worldArenaReset(leak, sizeof(leak));
    const pp::WorldArenaInfo a1 = pp::worldArenaInfo();
    const uint32_t heapAfter = heapUsedKb();
    ++worldFlushes_;
    PSP_LOGI(pp::tag::Mem, "world_flush why=%s dist=%.0f ms=%u freed_kb=%u arena_used_before=%u arena_used_after=%u", why,
             distYd, U(pp::logUptimeMs() - t0), U(heapBefore > heapAfter ? heapBefore - heapAfter : 0u),
             U(a0.usedKb), U(a1.usedKb));
    PSP_LOGI(pp::tag::Mem, "world_flush_heap heap_used_before=%u heap_used_after=%u largest_after=%u free_kb=%u n=%u",
             U(heapBefore), U(heapAfter), U(pp::heapLargestKb()), U(pp::heapFreeKb()), U(worldFlushes_));
    PSP_LOGI(pp::tag::Mem, "world_flush_parts tiles=%u wmo_kb=%u go_kb=%u npc_kb=%u fx_kb=%u pool_lost=%u parked=%u "
             "arena_largest_kb=%u",
             U(tiles), U(wmoKb), U(goKb), U(npcKb), U(fxKb), U(poolLost), U(pp::pakPrefetchCount()), U(a1.largestKb));
    if (poolLost)
        PSP_LOGW(pp::tag::Mem, "world_flush_leak pool tex=%u tiles=%u runs=%u (slots reset)", U(pt), U(pl), U(pr));
    if (live)
        PSP_LOGW(pp::tag::Mem, "world_flush_leak arena_blocks=%u used_kb=%u at=%s", U(live), U(a1.usedKb), leak);
    pp::wmoArenaLog();
    pp::logFlush();
}

}  // namespace wowee::ui::psp
