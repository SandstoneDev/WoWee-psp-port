// Mounts - the world screen half (docs/pending/mounts_spellfx.md).
//
// Upstream, ported (logic unchanged unless marked PSP):
//   - EntitySpawner::processPendingMount: the own mount's model is loaded at
//     once when UNIT_FIELD_MOUNTDISPLAYID turns non-zero (here: the display's
//     model, textures and seat table out of creatures.pak - the PSP mount is a
//     unit of the creature renderer); heightOffset = attachment 0's z when
//     > 0.1, else the tight-bounds guess (baked into "p_<n>").
//   - AnimationController::setMounted: the mount's animation set discovered
//     from its sequences (jump start / loop / end by duration and id
//     proximity, rear-up, run, stand, fidgets), MountFSM configured through
//     CharacterAnimator, the camera pivot raised by heightOffset.
//   - AnimationController::updateMountedAnimation: MountFSM input from the
//     movement state, its mount animation played on the mount, the rider in
//     MOUNT (91), the mount's jump (triggerMountJump -> WorldScreen jump at
//     getMountJumpVelocity).
//   - applyMountPositioning (ground branch): the mount at the player's feet
//     and facing, the rider on the seat attachment (0, 5, 6, 7, 8 - the first
//     found) + 0.08, smoothed at 14 Hz while standing still and not carried;
//     without a seat: feet + heightOffset + bob.
//   - EntitySpawner::processPendingRemotePlayerMounts + Application's player
//     sync: another player's mount is drawn at their position with Stand /
//     Walk / Run by their movement (the creature renderer's own creature
//     sync does exactly that), the rider at position + riderHeight in MOUNT;
//     at most one mount model load per frame.
// no list of mounts any more (mount_displays.py read our world DB): every
// display of CreatureDisplayInfo is in creatures.pak with its model's points "p_<n>" (seat probe 0, 5, 6, 7, 8
// + the rider height guess), so whatever display the server puts in UNIT_FIELD_MOUNTDISPLAYID - a mount
// aura's creature, a taxi beast - loads; a model with a seat (attachment 0 "MountMain") also carries the
// mount jump set and rear-up.
// PSP differences: no mount pitch / roll lean (the creature and character
// draws take a yaw only), no flying-mount branch (the taxi branch is in, in.taxiFlight),
// no MountSoundManager, the mount's nextAnimation / aliasNext are not baked
// (their bonus terms in the jump discovery read as "none"), wowee.cfg
// mounts=0 draws nobody mounted (A/B).
#include "ui/psp/ui_world_screen.hpp"
#include "platform/psp/psp_prof.hpp"

#include "game/update_field_table.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_system.hpp"

#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <malloc.h>
#include <set>
#include <tuple>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace anim = wowee::rendering::anim;

namespace {

// The creature-renderer key of the mount under a player: the player's guid
// with a tag in the high byte (player guids have none; units have 0xF1..).
constexpr uint64_t kMountGuidTag = 0x4D00000000000000ull;
uint64_t mountGuid(uint64_t playerGuid) { return kMountGuidTag | (playerGuid & 0x00FFFFFFFFFFFFFFull); }

using Seq = rendering::psp::CreatureRenderer::SequenceInfo;

// upstream AnimationController::setMounted's discovery, over the baked
// sequences. PSP: nextAnimation / aliasNext are not baked - read as -1 / 0.
rendering::MountFSM::MountAnimSet discoverMountAnims(const std::vector<Seq>& sequences,
                                                     const std::function<bool(uint32_t)>& has) {
    auto findFirst = [&](std::initializer_list<uint32_t> candidates) -> uint32_t {
        for (uint32_t id : candidates)
            if (has(id)) return id;
        return 0;
    };
    constexpr int16_t kNextAnimation = -1;  // PSP: not baked
    constexpr uint32_t kAliasNext = 0;      // PSP: not baked
    auto discoverJumpSet = [&]() {
        const std::set<uint32_t> forbiddenIds = {53, 54, 16};
        auto scoreNear = [](int a, int b) -> int {
            const int d = std::abs(a - b);
            return (d <= 8) ? (20 - d) : 0;
        };
        auto isForbidden = [&](uint32_t id) { return forbiddenIds.count(id) != 0; };
        auto findSeqById = [&](uint32_t id) -> const Seq* {
            for (const auto& s : sequences)
                if (s.id == id) return &s;
            return nullptr;
        };
        const uint32_t runId = findFirst({anim::RUN, anim::WALK});
        const uint32_t standId = findFirst({anim::STAND});
        std::vector<uint32_t> loops;
        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            const bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop && seq.durationMs >= 350 && seq.durationMs <= 1000 && seq.id != runId && seq.id != standId)
                loops.push_back(seq.id);
        }
        uint32_t loop = 0;
        if (!loops.empty()) {
            uint32_t best = loops[0];
            int bestScore = -999;
            for (uint32_t id : loops) {
                int sc = 0;
                sc += scoreNear(static_cast<int>(id), 38);
                const auto* s = findSeqById(id);
                if (s) sc += (s->durationMs >= 500 && s->durationMs <= 800) ? 5 : 0;
                if (sc > bestScore) {
                    bestScore = sc;
                    best = id;
                }
            }
            loop = best;
        }
        uint32_t start = 0, end = 0;
        int bestStart = -999, bestEnd = -999;
        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            const bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop) continue;
            if (seq.durationMs >= 450 && seq.durationMs <= 1100) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (loop && (kNextAnimation == static_cast<int16_t>(loop) || kAliasNext == loop)) sc += 30;
                if (loop && scoreNear(kNextAnimation, static_cast<int>(loop)) > 0) sc += 10;
                if (sc > bestStart) {  // PSP: blendTime > 400 (-5) not compared (blend kept as ms, all <= 500)
                    bestStart = sc;
                    start = seq.id;
                }
            }
            if (seq.durationMs >= 650 && seq.durationMs <= 1600) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (kNextAnimation == static_cast<int16_t>(runId) || kNextAnimation == static_cast<int16_t>(standId))
                    sc += 10;
                if (kNextAnimation < 0) sc += 5;
                if (sc > bestEnd) {
                    bestEnd = sc;
                    end = seq.id;
                }
            }
        }
        return std::make_tuple(start, loop, end);
    };
    auto [discoveredStart, discoveredLoop, discoveredEnd] = discoverJumpSet();

    rendering::MountFSM::MountAnimSet m;
    m.jumpStart = discoveredStart > 0 ? discoveredStart : findFirst({anim::FALL, anim::JUMP_START});
    m.jumpLoop = discoveredLoop > 0 ? discoveredLoop : findFirst({anim::JUMP});
    m.jumpEnd = discoveredEnd > 0 ? discoveredEnd : findFirst({anim::JUMP_END});
    m.rearUp = findFirst({anim::MOUNT_SPECIAL, anim::RUN_RIGHT, anim::FALL});
    m.run = findFirst({anim::RUN, anim::WALK});
    m.stand = findFirst({anim::STAND});
    m.flyForward = findFirst({anim::FLY_FORWARD});
    m.flyIdle = findFirst({anim::FLY_IDLE, anim::FLY_FORWARD});
    m.flyLeft = findFirst({anim::RUN_LEFT});
    m.flyRight = findFirst({anim::RUN_RIGHT});
    for (const auto& seq : sequences) {
        const bool isLoop = (seq.flags & 0x01) == 0;
        const bool hasFrequency = seq.frequency > 0;
        const bool hasReplay = seq.replayMax > 0;
        const bool isStationary = std::fabs(seq.movingSpeed) < 0.05f;
        const bool reasonableDuration = seq.durationMs >= 400 && seq.durationMs <= 2500;
        const bool isDeathOrWound = (seq.id >= 5 && seq.id <= 9);
        const bool isAttackOrCombat = (seq.id >= 11 && seq.id <= 21);
        const bool isSpecial = (seq.id == 2 || seq.id == 3);
        if (!isLoop && (hasFrequency || hasReplay) && isStationary && reasonableDuration && !isDeathOrWound &&
            !isAttackOrCombat && !isSpecial)
            m.fidgets.push_back(seq.id);
    }
    if (m.run == 0) m.run = m.stand;
    return m;
}

}  // namespace

void WorldScreen::updateMount(float dtSec) {
    // once per world (a function static read it once per run: after a transfer the rebuilt
    // world kept mountsOn_'s default and mounts=0 was ignored).
    if (!mountCfgRead_) {
        mountCfgRead_ = true;
        mountsOn_ = pp::configGetInt("mounts", 1) != 0;
    }
    session_.updateMount(dtSec);
    const uint32_t want = mountsOn_ ? session_.ownMountDisplayId() : 0u;
    if (want == ownMountDisplay_) return;
    if (ownMountDisplay_) clearOwnMount();
    if (want) setOwnMount(want);
}

void WorldScreen::setOwnMount(uint32_t displayId) {
    PROF_SCOPE("mount.load");
    ownMountDisplay_ = displayId;
    mountActive_ = false;
    if (!creatures_.isOpen() || !creatures_.hasDisplay(displayId)) {
        // upstream: no model path -> no mount instance, the rider stays on foot.
        // every CreatureDisplayInfo row is baked - missing = a helper model, no model
        // path, or an older creatures.pak (rebake it).
        PSP_LOGW(pp::tag::Gfx, "mount_missing display=%u (not in creatures.pak)", static_cast<unsigned>(displayId));
        return;
    }
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    uint32_t bytes = 0;
    if (!creatures_.preload(displayId, &bytes)) {
        PSP_LOGW(pp::tag::Gfx, "mount_missing display=%u load_failed", static_cast<unsigned>(displayId));
        return;
    }
    float seatZ = -1.0f, riderHeight = 1.8f;
    const bool seats = creatures_.mountHeights(displayId, seatZ, riderHeight);
    // upstream processPendingMount: attachment 0's z when it is a seat (> 0.1), else the bounds guess.
    mountHeightOffset_ = seatZ > 0.1f ? seatZ : riderHeight;
    std::vector<rendering::psp::CreatureRenderer::SequenceInfo> seqs;
    creatures_.displaySequences(displayId, seqs);
    const rendering::MountFSM::MountAnimSet set = discoverMountAnims(
        seqs, [&](uint32_t id) { return creatures_.displayHasAnimation(displayId, id); });
    animator_.configureMountFSM(set, false);
    if (sitting_) setSitting(false, false);
    mountActive_ = true;
    mountSeatInit_ = false;
    mountSeatFound_ = false;
    mountBob_ = 0.0f;
    mountJumpGo_ = false;
    lastAnimRequest_ = UINT32_MAX;
    creatures_.playDriven(mountGuid(session_.playerGuid()), set.stand ? set.stand : anim::STAND, true);
    PSP_LOGI(pp::tag::Gfx,
             "mount_set display=%u kb=%u ms=%u seats=%d seat_z=%.2f off=%.2f jump=%u/%u/%u rear=%u fidgets=%u seqs=%u",
             static_cast<unsigned>(displayId), static_cast<unsigned>(bytes / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u), seats ? 1 : 0,
             static_cast<double>(seatZ), static_cast<double>(mountHeightOffset_), static_cast<unsigned>(set.jumpStart),
             static_cast<unsigned>(set.jumpLoop), static_cast<unsigned>(set.jumpEnd), static_cast<unsigned>(set.rearUp),
             static_cast<unsigned>(set.fidgets.size()), static_cast<unsigned>(seqs.size()));
    PSP_LOGI(pp::tag::Gfx, "mount_rider has_mount_anim=%d run_speed=%.2f", player_.hasAnimation(anim::MOUNT) ? 1 : 0,
             static_cast<double>(session_.serverRunSpeed()));
    if (++mountSetups_ <= 3) pp::logMemory("mount_set");  // the budget line for the hardware run
}

void WorldScreen::clearOwnMount() {
    PSP_LOGI(pp::tag::Gfx, "mount_clear display=%u active=%d run_speed=%.2f", static_cast<unsigned>(ownMountDisplay_),
             mountActive_ ? 1 : 0, static_cast<double>(session_.serverRunSpeed()));
    if (mountActive_) animator_.clearMountFSM();
    mountActive_ = false;
    ownMountDisplay_ = 0;
    mountHeightOffset_ = 0.0f;
    mountBob_ = 0.0f;
    mountJumpKey_ = mountJumpGo_ = false;
    mountSeatInit_ = false;
    lastAnimRequest_ = UINT32_MAX;  // the animator's next output is played again
}

// upstream AnimationController::updateMountedAnimation (ground mounts).
bool WorldScreen::updateMountedAnimation(float dtSec) {
    if (!mountActive_) return false;
    if (flightMountAnim()) return true;  // in the water: the swim set
    PROF_SCOPE("mount.fsm");
    const uint64_t key = mountGuid(session_.playerGuid());
    auto& mountFSM = animator_.getMountFSM();
    rendering::MountFSM::Input in;
    in.moving = wantMove_;
    in.movingBackward = animBackward_;
    in.strafeLeft = animStrafe_ < 0.0f;
    in.strafeRight = animStrafe_ > 0.0f;
    in.grounded = !airborne_;
    in.jumpKeyPressed = mountJumpKey_;
    in.flying = ownFlying();  // upstream: the flight set (Fly / Hover / RunLeft / RunRight) while airborne
    in.ascending = (flyFlags_ & 0x400000u) != 0;
    in.descending = (flyFlags_ & 0x800000u) != 0;
    in.swimming = false;  // the water: flightMountAnim
    in.taxiFlight = session_.isOnTaxiFlight();  // MountFSM's taxi branch (Fly / Hover / Run)
    in.deltaTime = dtSec;
    in.characterYaw = facing_ * 57.29578f;  // upstream: degrees
    in.haveMountState = creatures_.animationState(key, in.curMountAnim, in.curMountTime, in.curMountDuration);
    const rendering::MountFSM::Output out = mountFSM.evaluate(in);
    if (out.mountAnimChanged) creatures_.playDriven(key, out.mountAnimId, out.mountAnimLoop);
    // The rider sits in Mount (upstream: 3.3.5 has no rider flight poses).
    uint32_t cur = 0;
    float t = 0.0f, dur = 0.0f;
    // (Without MOUNT in characters.pak the fallback is played once, not restarted every frame.)
    const bool lost = !player_.getAnimationState(cur, t, dur) || (cur != anim::MOUNT && player_.hasAnimation(anim::MOUNT));
    if (lastAnimRequest_ != anim::MOUNT || lost) {
        player_.playAnimation(anim::MOUNT, true);
        lastAnimRequest_ = anim::MOUNT;
        lastAnimLoop_ = true;
    }
    if (out.triggerMountJump) mountJumpGo_ = true;  // updateMovement takes off next frame
    mountBob_ = out.mountBob;
    // One sample every 10 s for the hardware log.
    const uint32_t nowMs = pp::logUptimeMs();
    if (nowMs >= mountFrameLogMs_) {
        mountFrameLogMs_ = nowMs + 10000u;
        uint32_t remote = 0;
        for (const auto& [g, r] : remotes_) remote += r.mountReady ? 1u : 0u;
        PSP_LOGI(pp::tag::Gfx, "mount_frame anim=%u state=%d seat=%d remote=%u npc_kb=%u heap_kb=%u run=%.1f",
                 static_cast<unsigned>(in.curMountAnim), static_cast<int>(mountFSM.getState()), mountSeatFound_ ? 1 : 0,
                 static_cast<unsigned>(remote), static_cast<unsigned>(creatures_.stats().memoryKb),
                 static_cast<unsigned>(pp::heapUsedKb()), static_cast<double>(session_.serverRunSpeed()));
    }
    return true;
}

void WorldScreen::appendMountUnits(std::vector<rendering::psp::CreatureUnit>& out) {
    if (mountActive_) {
        rendering::psp::CreatureUnit u;
        u.guid = mountGuid(session_.playerGuid());
        u.displayId = ownMountDisplay_;
        u.x = playerX_;
        u.y = playerY_;
        u.z = shownZ_;
        u.orientation = facing_;
        u.moving = wantMove_;
        u.mount = u.driven = u.rawScale = true;  // upstream: the mount instance at scale 1.0
        out.push_back(u);
    }
    for (const auto& [guid, r] : remotes_) {
        if (!r.mountReady || !r.model) continue;
        const float dx = r.pos.x - playerX_, dy = r.pos.y - playerY_;
        if (dx * dx + dy * dy > remoteView_ * remoteView_) continue;  // the rider is not drawn either
        rendering::psp::CreatureUnit u;
        u.guid = mountGuid(guid);
        u.displayId = r.mountDisplay;
        u.x = r.pos.x;
        u.y = r.pos.y;
        u.z = r.pos.z;
        u.orientation = r.yaw;
        u.moving = r.movingNow;
        u.walking = r.walkingNow;
        // PSP (npcanimspeed): the mount's stride at the rider's speed.
        if (r.movingNow && !r.walkingNow) u.speed = session_.unitRunSpeed(guid);
        u.mount = u.rawScale = true;
        if (flightRemoteSwims(guid)) {  // a swimmer's mount plays the swim set (Swim / SwimIdle)
            u.swimming = true;
            u.speed = 0.0f;
        } else if (flightRemoteFlies(guid)) {  // a flier's mount: Fly / Hover (upstream remote mount sync)
            u.flying = true;
            u.speed = 0.0f;
        }
        out.push_back(u);
    }
}

// upstream applyMountPositioning, ground branch.
glm::vec3 WorldScreen::ownRiderPos() {
    const glm::vec3 feet(playerX_, playerY_, shownZ_);
    if (!mountActive_) return feet;
    glm::vec3 seat;
    mountSeatFound_ = creatures_.seatPoint(mountGuid(session_.playerGuid()), seat);
    if (mountSeatFound_) {
        const glm::vec3 target = seat + glm::vec3(0.0f, 0.0f, 0.08f);
        constexpr float kSeatCarriedStepSq = 0.05f * 0.05f;
        const glm::vec3 step = target - mountSeatLast_;
        const bool carried = mountSeatInit_ && glm::dot(step, step) > kSeatCarriedStepSq;
        mountSeatLast_ = target;
        if (wantMove_ || carried) {
            mountSeatInit_ = false;
            mountSeatSmoothed_ = target;
        } else if (!mountSeatInit_) {
            mountSeatSmoothed_ = target;
            mountSeatInit_ = true;
        } else {
            const float alpha = 1.0f - std::exp(-14.0f * std::max(lastDt_, 0.001f));
            mountSeatSmoothed_ += (target - mountSeatSmoothed_) * alpha;
        }
        return mountSeatSmoothed_;
    }
    mountSeatInit_ = false;
    return feet + glm::vec3(0.0f, 0.0f, mountHeightOffset_ + mountBob_);
}

// upstream setRemotePlayerMountDisplayId -> processPendingRemotePlayerMounts:
// one model load per frame; a failed load leaves the rider on foot.
void WorldScreen::updateRemoteMount(uint64_t guid, RemotePlayer& r, const game::Entity& e) {
    static const uint16_t fMount = game::fieldIndex(game::UF::UNIT_FIELD_MOUNTDISPLAYID);
    const uint32_t md = mountsOn_ ? e.getField(fMount) : 0u;
    if (md != r.mountDisplay) {
        r.mountDisplay = md;
        if (r.mountReady) r.model->playAnimation(anim::STAND, true);  // off the mount: on foot again
        r.mountReady = false;
        r.riderHeight = 1.8f;        // 0 marks a display that cannot load (not retried)
        r.wasMoving = !r.movingNow;  // the next frame re-picks Run / Stand
        if (!md && ++mountLogs_ <= 60)
            PSP_LOGI(pp::tag::Gfx, "mount_remote guid=%08X display=0", static_cast<unsigned>(guid & 0xFFFFFFFFu));
    }
    if (!md || r.mountReady || r.riderHeight == 0.0f || remoteMountLoadedThisFrame_ || !creatures_.isOpen()) return;
    if (!creatures_.hasDisplay(md)) {
        // Not in the pak: the rider stays on foot (upstream: no model path), not retried.
        if (++mountLogs_ <= 60)
            PSP_LOGW(pp::tag::Gfx, "mount_missing display=%u remote=%08X", static_cast<unsigned>(md),
                     static_cast<unsigned>(guid & 0xFFFFFFFFu));
        r.riderHeight = 0.0f;
        return;
    }
    remoteMountLoadedThisFrame_ = true;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    uint32_t bytes = 0;
    const bool ok = creatures_.preload(md, &bytes);
    float seatZ = -1.0f;
    r.riderHeight = 1.8f;
    creatures_.mountHeights(md, seatZ, r.riderHeight);
    r.mountReady = ok;
    if (++mountLogs_ <= 60)
        PSP_LOGI(pp::tag::Gfx, "mount_remote guid=%08X display=%u ok=%d h=%.2f kb=%u ms=%u",
                 static_cast<unsigned>(guid & 0xFFFFFFFFu), static_cast<unsigned>(md), ok ? 1 : 0,
                 static_cast<double>(r.riderHeight), static_cast<unsigned>(bytes / 1024u),
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    if (!ok) r.riderHeight = 0.0f;  // do not retry every frame
}

}  // namespace wowee::ui::psp
