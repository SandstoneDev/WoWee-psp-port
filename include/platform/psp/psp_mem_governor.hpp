// Memory budget: the memory governor. PSP only - the original client has
// no such thing (a 2-4 GB address space); the PSP-3000 gives the build-117 code ~40 MB of heap.
//
// The caller (WorldScreen::updateMemoryGovernor, ui_world_memory.cpp) asks due() every frame; when it is due it
// probes the heap - one malloc of cutProbeKb() KB, written and freed (the volatile probe: gcc drops a malloc/free pair
// it can see through) - and, while a step is cut, one of restoreProbeKb() KB, and hands both answers to decide():
//   - the cut probe failed: the next step is cut, in this order - particles -> other characters' item models
//     (attachcap) -> the big WMOs' doodads -> the NPC view distance; at most one cut per cutGapMs (the freeing takes
//     frames: crowd yields, the NPC cache's LRU), at once again when the step had nothing to free (noteResult), and
// at once while not even emergencyProbeKb() KB is free (the caller's second probe);
//   - the restore probe passed for restoreHoldMs without one failed probe: the last cut step comes back (NPC distance
//     first, particles last), at most one per restoreGapMs. The restore probe asks for restoreKb plus what that step
//     freed when it was cut, so giving it back cannot drop the heap under the cut line at once (no flapping).
//   - Memory diet (docs/PSP_PORT_ARCHITECTURE.md): a consumer of higher priority waited for memory
//     since the last tick (starveMax: the deepest step it may have cut - lower priorities only): the next step is cut
//     as if the probe had failed (the cut gap holds), and nothing comes back while anyone starves. Probes are headroom
//     (total free + one block of min(n, 1 MB): pp::heapGivesKb), not one block of n.
// Pure logic, no PSP headers: tools/psp/host_tests/memory_budget_test.cpp runs it on the PC.
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

enum MemStep : uint8_t {
    kMemStepNone = 0,
    kMemStepParticles = 1,    // the world's particle emitters (WorldParticles: cache + directories + descriptor)
    kMemStepAttachments = 2,  // other characters' item models: attachcap halved, the farthest give way
    kMemStepWmoDoodads = 3,   // the big WMOs' doodads (ui_world_bigwmo.cpp pseudo-tiles)
    kMemStepNpcDistance = 4,  // NPC view distance and model budget (CreatureRenderer::viewDistance / memoryBudgetKb)
};
constexpr uint8_t kMemStepCount = 4;

inline const char* memStepName(uint8_t step) {
    switch (step) {
        case kMemStepParticles: return "particles";
        case kMemStepAttachments: return "attachments";
        case kMemStepWmoDoodads: return "wmo_doodads";
        case kMemStepNpcDistance: return "npc_distance";
        default: return "none";
    }
}

struct MemGovernorConfig {
    bool on = true;                  // wowee.cfg memgov (1; 0 = never cuts)
    uint32_t cutKb = 1024;           // govcut: a probe of this many KB failed -> cut the next step
    uint32_t restoreKb = 3072;       // govrestore: + what the step freed must pass for restoreHoldMs to give it back
    uint32_t tickMs = 500;           // probe cadence
    uint32_t cutGapMs = 2000;        // between two cuts
    uint32_t restoreHoldMs = 10000;  // the restore probe passes this long without a failed probe
    uint32_t restoreGapMs = 10000;   // between two restores, and after a cut
    uint32_t freedCapKb = 8192;      // what a step's freeing may add to the restore probe at most
    // govemerg - not even this many KB in one block: the next step is cut at once, no cut gap (a capital's
    // entry on the device's 40.9 MB: one cut per 2 s reached the NPCs after the heap had run out).
    uint32_t emergencyKb = 384;
    uint8_t maxLevel = kMemStepCount;  // govmax: the deepest step that may be cut (0 = none)
    bool starve = true;                // govstarve - a waiting consumer of higher priority cuts too (0 = off)
};

struct MemDecision {
    int8_t dir = 0;    // +1 cut `step`, -1 give `step` back, 0 nothing
    uint8_t step = 0;  // MemStep
};

class MemGovernor {
public:
    void configure(const MemGovernorConfig& c) {
        cfg_ = c;
        if (cfg_.maxLevel > kMemStepCount) cfg_.maxLevel = kMemStepCount;
        if (cfg_.tickMs == 0) cfg_.tickMs = 1;
    }
    const MemGovernorConfig& config() const { return cfg_; }

    // A probe is due (every tickMs; the first call at once).
    bool due(uint32_t nowMs) const { return cfg_.on && (!started_ || reached(nowMs, nextTickMs_)); }
    uint32_t cutProbeKb() const { return cfg_.cutKb; }
    // 0 while nothing is cut (no restore probe needed).
    uint32_t restoreProbeKb() const {
        if (!level_) return 0;
        const uint32_t f = freedKb_[level_] < cfg_.freedCapKb ? freedKb_[level_] : cfg_.freedCapKb;
        return cfg_.restoreKb + f;
    }

    // Asked only after a failed cut probe (0: no emergency probe).
    uint32_t emergencyProbeKb() const { return cfg_.emergencyKb < cfg_.cutKb ? cfg_.emergencyKb : 0u; }

    // cutOk: the cut probe passed; restoreOk: the restore probe passed (ignored while nothing is cut);
    // emergencyOk: the emergency probe passed (asked only when the cut probe failed);
    // starveMax: a consumer waited for memory since the last tick - the deepest step it may have cut
    // (starveStepFor: lower priorities than its own), 0 = nobody waited.
    MemDecision decide(uint32_t nowMs, bool cutOk, bool restoreOk, bool emergencyOk = true, uint8_t starveMax = 0) {
        MemDecision d;
        started_ = true;
        nextTickMs_ = nowMs + cfg_.tickMs;
        if (!cfg_.on) return d;
        ++ticks_;
        if (cutOk && starveMax && cfg_.starve) {
            // The heap passes the cut line, but a consumer of higher priority could not get its block plus
            // its margin - free a lower priority for it (one step per cut gap), and give nothing back meanwhile.
            holding_ = false;
            ++starveTicks_;
            const uint8_t cap = starveMax < cfg_.maxLevel ? starveMax : cfg_.maxLevel;
            if (level_ < cap && reached(nowMs, nextCutMs_)) {
                ++level_;
                freedKb_[level_] = 0;
                nextCutMs_ = nowMs + cfg_.cutGapMs;
                nextRestoreMs_ = nowMs + cfg_.restoreGapMs;
                ++cuts_;
                ++starveCuts_;
                ++stepCuts_[level_];
                d.dir = 1;
                d.step = level_;
            }
            return d;
        }
        if (!cutOk) {
            holding_ = false;
            ++lowTicks_;
            if (!emergencyOk) ++emergencyTicks_;
            if (level_ < cfg_.maxLevel && (!emergencyOk || reached(nowMs, nextCutMs_))) {
                ++level_;
                freedKb_[level_] = 0;
                nextCutMs_ = nowMs + cfg_.cutGapMs;
                nextRestoreMs_ = nowMs + cfg_.restoreGapMs;
                ++cuts_;
                ++stepCuts_[level_];
                d.dir = 1;
                d.step = level_;
            }
            return d;
        }
        if (!level_ || !restoreOk) {
            holding_ = false;
            return d;
        }
        if (!holding_) {
            holding_ = true;
            restoreSinceMs_ = nowMs;
        }
        if (nowMs - restoreSinceMs_ >= cfg_.restoreHoldMs && reached(nowMs, nextRestoreMs_)) {
            d.dir = -1;
            d.step = level_;
            freedKb_[level_] = 0;
            --level_;
            holding_ = false;
            nextRestoreMs_ = nowMs + cfg_.restoreGapMs;
            ++restores_;
        }
        return d;
    }

    // What cutting `step` freed (KB, measured by the caller). `async`: the step frees over the next frames (crowd
    // yields, the NPC cache's LRU) - nothing freed now is not "nothing to free". A step with nothing to free and not
    // async lets the next cut come at the next tick (no cut gap).
    void noteResult(uint8_t step, uint32_t freedKb, bool async) {
        if (step == 0 || step > kMemStepCount) return;
        freedKb_[step] += freedKb;
        if (!freedKb && !async && step == level_) nextCutMs_ = 0;
    }

    uint8_t level() const { return level_; }
    uint32_t cuts() const { return cuts_; }
    uint32_t restores() const { return restores_; }
    uint32_t ticks() const { return ticks_; }
    uint32_t lowTicks() const { return lowTicks_; }  // probes whose cut probe failed
    uint32_t emergencyTicks() const { return emergencyTicks_; }  // Of them, the emergency probe failed too
    uint32_t starveTicks() const { return starveTicks_; }  // Ticks a waiting consumer held restores / cut
    uint32_t starveCuts() const { return starveCuts_; }    // Cuts made for a waiting consumer
    uint32_t stepCuts(uint8_t step) const { return step <= kMemStepCount ? stepCuts_[step] : 0u; }
    uint32_t freedKb(uint8_t step) const { return step <= kMemStepCount ? freedKb_[step] : 0u; }
    bool holding() const { return holding_; }

private:
    static bool reached(uint32_t nowMs, uint32_t atMs) { return atMs == 0 || static_cast<int32_t>(nowMs - atMs) >= 0; }
    MemGovernorConfig cfg_;
    bool started_ = false;
    bool holding_ = false;  // the restore probe has passed since restoreSinceMs_
    uint8_t level_ = 0;
    uint32_t nextTickMs_ = 0, nextCutMs_ = 0, nextRestoreMs_ = 0, restoreSinceMs_ = 0;
    uint32_t freedKb_[kMemStepCount + 1] = {};
    uint32_t stepCuts_[kMemStepCount + 1] = {};
    uint32_t cuts_ = 0, restores_ = 0, ticks_ = 0, lowTicks_ = 0, emergencyTicks_ = 0;
    uint32_t starveTicks_ = 0, starveCuts_ = 0;
};

// ---- Memory diet (docs/PSP_PORT_ARCHITECTURE.md): fair sharing ------------------------------------
// Who gets memory first when it is tight (higher first): the player's ground (terrain tiles of the window, chunk
// textures near the player) > NPCs near the player > the big WMO the camera is in (+ its doodads) > doodads of tiles
// in (top-ups) > a far big WMO (a city in sight) > luxuries (world particles, other characters' item models). Every
// consumer asks pp::heapRoomFor(its block, its margin): the block it is about to read and that much left IN TOTAL for
// the next ones - margins grow as the priority falls, so a lower priority stops first and never takes the room a
// higher one needs next (a terrain tile 0.5 MB with the texture window, a big WMO header, a window's ui.pak art 128 KB+,
// Lua). A consumer that waited asks the governor to free lower priorities (starveStepFor) instead of everyone waiting.
enum MemPrio : uint8_t { kPrioTerrain = 0, kPrioNpcNear = 1, kPrioWmoNear = 2, kPrioDoodads = 3, kPrioWmoFar = 4 };

// The deepest governor step a consumer that waited may have cut for it - only steps of lower priority than its own:
// the ground -> up to the NPC view distance (4); NPCs near the player -> up to the big WMOs' doodads (3: particles,
// other characters' items, the big WMOs' doodads and the open big WMOs that draw nothing); the big WMO the camera is in
// -> particles and items (2); doodad top-ups and a far big WMO -> nothing (they wait, they are the lowest).
inline uint8_t starveStepFor(MemPrio p) {
    switch (p) {
        case kPrioTerrain: return kMemStepNpcDistance;
        case kPrioNpcNear: return kMemStepWmoDoodads;
        case kPrioWmoNear: return kMemStepAttachments;
        default: return 0;
    }
}

// A big WMO's open (ui_world_bigwmo.cpp): the camera inside its box - no probe (it is the ground the player stands on);
// outside - farKb of headroom (wowee.cfg wmofarkb), but after waitCapMs of waiting (wmofarwait) the near reserve
// (wmoreserve) is enough: a far city waits for room, never forever (hardware: a capital waited 100 s with 2.7 MB
// free). At world entry (priming) only the one the camera is in. Returns the KB to probe, 0 = none, 0xFFFFFFFF = not now.
struct FarWmoRule {
    uint32_t farKb = 3072;
    uint32_t nearKb = 1024;
    uint32_t waitCapMs = 30000;
};
inline uint32_t bigWmoOpenKb(float yd, bool priming, uint32_t waitedMs, const FarWmoRule& r) {
    if (yd <= 0.0f) return 0;
    if (priming) return 0xFFFFFFFFu;
    if (r.waitCapMs && waitedMs >= r.waitCapMs) return r.nearKb;
    return r.farKb;
}

}  // namespace wowee::platform::psp
