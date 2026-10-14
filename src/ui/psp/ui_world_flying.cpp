// Flight and swimming in 3D - the world screen half (the rules: psp_flight_rules.hpp).
//
// After upstream CameraController / MovementHandler:
//   - with CAN_FLY (the server's toggle) the jump key takes off at once, on the ground or falling, and climbs from the
//     first frame: FLYING goes out with MSG_MOVE_START_ASCEND. In flight the jump key climbs, the sit key descends,
//     releasing either sends MSG_MOVE_STOP_ASCEND (the one stop for both).
//   - landing: flying with the feet on the ground (within 0.3 yd) and the jump key not held - FLYING and the climb
//     cleared, said with MSG_MOVE_HEARTBEAT. CAN_FLY taken in the air: the flight ends, the player falls.
//   - the water: swimming starts where the water is 1.15 yd deep (from the floor, else from the feet) and the feet are
//     under the surface, and ends below 0.85 yd (MSG_MOVE_START_SWIM / _STOP_SWIM; a flight that reaches the water
//     becomes a swim); the swimmer floats 1.45 yd under the surface; the climb keys swim up / down without climb
//     packets.
//   - a step swimming / flying goes along the facing pitched up / down (a strafe level) at the swim / flight speed (the
//     back speed when moving backwards only); the pitch travels in every movement packet.
//   - the animation: a swimmer plays the swim set; a flier Fly moving / Hover still, a flying mount its MountFSM flight
//     set; the rider sits in Mount.
// Own rules (the PSP): the pitch comes from the camera's tilt (no mouse look: R + stick tilts the orbit; at the camera's
// rest tilt the flight is level, 5 deg dead zone, 89 deg at most); a pitch change of more than 0.03 rad is said with an
// early heartbeat at most every 200 ms; the sit key descends (no sitting) while swimming or flying, mounted or not;
// the climb key held with the swimmer at the surface jumps out of the water (MSG_MOVE_JUMP); the water comes from the
// terrain's MH2O levels in sounds.pak's grids (no WMO liquids: city canals); no ceiling collision; the climb's
// vertical speed = the mode's speed.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_flight_rules.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace fr = wowee::platform::psp::flight;
namespace anim = wowee::rendering::anim;

namespace {

constexpr float kCamRestPitch = 0.1745f;    // the orbit's start tilt (pitch_): a level flight there
constexpr float kPitchDeadZone = 0.0873f;   // 5 deg either side of it
constexpr float kMaxFlightPitch = 1.5533f;  // 89 deg (own: just short of straight up / down)
constexpr float kCamMinPitchFree = -1.2f;   // the orbit under the player while flying / swimming
constexpr uint32_t kHeartbeatMs = 500, kPitchSendMs = 200;
constexpr float kPitchSendDelta = 0.03f;
constexpr uint32_t kClimb = fr::kAscending | fr::kDescending;
// The creature-renderer key of the own mount (ui_world_mount.cpp mountGuid: the player's guid tagged 0x4D).
constexpr uint64_t kMountGuidTag = 0x4D00000000000000ull;

float wrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

}  // namespace

float WorldScreen::flightCamMinPitch() const { return ownSwimOrFly() ? kCamMinPitchFree : -0.2f; }

void WorldScreen::flightControls(const pp::PadState& pad, bool combatMode) {
    const uint32_t held = pad.held & ~ctlMask_;  // held across a mode switch: dead until released
    const bool l = (held & PSP_CTRL_LTRIGGER) != 0, r = (held & PSP_CTRL_RTRIGGER) != 0;
    // travel mode: ✕ = the jump key (climbs), ○ = the sit key (descends); combat mode: START jumps.
    flyAscKey_ = combatMode ? (held & PSP_CTRL_START) != 0 : (!l && !r && (held & PSP_CTRL_CROSS));
    flyDescKey_ = !combatMode && !l && !r && (held & PSP_CTRL_CIRCLE);
}

void WorldScreen::flightSend(game::Opcode op) { sendMove(op, moving_); }

void WorldScreen::flightMoveInfo(game::MovementInfo& mi) {
    // The server's grants (CAN_FLY, WATER_WALK, FEATHER_FALL, HOVER, LEVITATING) stay in every packet (upstream keeps
    // them in its movementInfo); FLYING / SWIMMING ours; the climb flags only in flight (upstream sends no climb while
    // swimming).
    mi.flags |= session_.ownGrantedMoveFlags() & fr::kServerGranted;
    mi.flags |= flyFlags_ & (fr::kFlying | fr::kSwimming);
    if (flyFlags_ & fr::kFlying) mi.flags |= flyFlags_ & kClimb;
    if (fr::swimOrFly(mi.flags)) {
        mi.flags &= ~(fr::kFalling | fr::kFallingFar);
        mi.pitch = flyPitch_;
        sentPitch_ = flyPitch_;
    } else {
        mi.pitch = 0.0f;
    }
}

bool WorldScreen::flightWaterFloor(float x, float y, float maxZ, float& z) const {
    if (!(session_.ownGrantedMoveFlags() & fr::kWaterWalk)) return false;
    // PSP: only near the own player (floorAt also serves the camera and other players' prediction).
    if (std::fabs(x - playerX_) > 8.0f || std::fabs(y - playerY_) > 8.0f) return false;
    float level = 0.0f;
    if (!sounds_.waterAt(x, y, level) || level > maxZ) return false;
    z = level;
    return true;
}

bool WorldScreen::flightMovement(const pp::PadState& pad, float dtSec) {
    PROF_SCOPE("move.fly");
    if (dtSec > 0.1f) dtSec = 0.1f;
    const uint32_t nowMs = pp::logUptimeMs();
    const uint32_t granted = session_.ownGrantedMoveFlags();
    const bool alive = session_.life() == pp::WorldSession::Life::Alive;
    if (controlsScheme_ < 2) {  // controls=1: START is the jump key, no descend key
        flyAscKey_ = (pad.held & PSP_CTRL_START);
        flyDescKey_ = false;
    }
    if (!alive || taxiLocksControls()) flyAscKey_ = flyDescKey_ = false;
    auto startOwnFall = [&]() {  // a fall from rest (the ground code below takes it on)
        airborne_ = true;
        vz_ = 0.0f;
        jumpVelocity_ = 0.0f;
        fallStartMs_ = nowMs;
    };

    // The server's toggles since the last frame (the session has ACKed them).
    if (session_.moveToggleSeq() != flySeenToggle_) {
        flySeenToggle_ = session_.moveToggleSeq();
        if (!flyInit_) {  // logged in (or transferred) in the air: the create block's FLYING
            flyInit_ = true;
            if ((session_.ownCreateMoveFlags() & fr::kFlying) && (granted & fr::kCanFly)) {
                flyFlags_ |= fr::kFlying;
                airborne_ = false;
            }
        }
        if ((flyFlags_ & fr::kFlying) && !(granted & fr::kCanFly)) {
            // CAN_FLY taken in the air (upstream setFlyingActive(false)): the flight ends, gravity takes over.
            flyFlags_ &= ~(fr::kFlying | kClimb);
            flyPitch_ = 0.0f;
            startOwnFall();
            PSP_LOGI(pp::tag::Net, "fly_lost_can_fly z=%.1f", static_cast<double>(playerZ_));
        }
        if ((granted ^ flyGranted_) & fr::kCanFly)
            PSP_LOGI(pp::tag::Net, "fly_can_fly=%d mount=%u", (granted & fr::kCanFly) ? 1 : 0,
                     static_cast<unsigned>(ownMountDisplay_));
        flyGranted_ = granted;
    }
    if (!alive && (flyFlags_ & fr::kFlying)) {  // dead in the air: down (the server keeps the corpse)
        flyFlags_ &= ~(fr::kFlying | kClimb);
        flyPitch_ = 0.0f;
        startOwnFall();
    }

    // The water (every frame): upstream's depth test, from the floor under the feet.
    float liquid = 0.0f;
    const bool haveLiquid = sounds_.waterAt(playerX_, playerY_, liquid);
    const float floorUnder = haveLiquid ? floorAt(playerX_, playerY_, playerZ_ + 1.0f) : -1e30f;
    const bool haveFloor = floorUnder > -1e29f;
    const float depth = haveLiquid ? liquid - (haveFloor ? floorUnder : playerZ_) : 0.0f;
    const bool swimmingNow = (flyFlags_ & fr::kSwimming) != 0;
    const bool water =
        fr::inWater(haveLiquid, liquid, playerZ_, haveFloor, floorUnder, swimmingNow) && !transportRiding();
    if (!swimmingNow && water) {
        flyFlags_ = (flyFlags_ & ~(fr::kFlying | kClimb)) | fr::kSwimming;  // a flight ends in the water
        airborne_ = false;
        vz_ = 0.0f;
        if (sitting_) setSitting(false, false);
        flightSend(game::Opcode::MSG_MOVE_START_SWIM);
        if (++flyLogs_ <= 40)
            PSP_LOGI(pp::tag::Net, "swim_start depth=%.2f h=%.2f z=%.1f", static_cast<double>(depth),
                     static_cast<double>(fr::kSwimEnterDepth), static_cast<double>(playerZ_));
    } else if (swimmingNow) {
        const bool atSurface = haveLiquid && playerZ_ >= liquid - fr::kWaterSurfaceOffset - 0.05f;
        if (alive && water && flyAscKey_ && atSurface) {
            // Own rule: the climb key held at the surface jumps out of the water (MSG_MOVE_JUMP; the swim ends).
            flyFlags_ &= ~(fr::kSwimming | kClimb);
            flyPitch_ = 0.0f;
            airborne_ = true;
            vz_ = mountActive_ ? 6.0f : 7.96f;  // as the ground jump (updateMovement)
            jumpVelocity_ = vz_;
            fallStartMs_ = nowMs;
            if (!mountActive_) animator_.onEvent(rendering::AnimEvent::JUMP);
            moving_ = wantMove_;
            sendMove(game::Opcode::MSG_MOVE_JUMP, wantMove_);
            if (++flyLogs_ <= 40) PSP_LOGI(pp::tag::Net, "swim_jump_out depth=%.2f", static_cast<double>(depth));
        } else if (!water) {
            flyFlags_ &= ~(fr::kSwimming | kClimb);  // the swim ends
            flyPitch_ = 0.0f;
            startOwnFall();
            flightSend(game::Opcode::MSG_MOVE_STOP_SWIM);
            if (++flyLogs_ <= 40)
                PSP_LOGI(pp::tag::Net, "swim_stop depth=%.2f liquid=%d", static_cast<double>(depth), haveLiquid ? 1 : 0);
        }
    }

    // Take-off and landing (upstream moveFollowedCharacter / updateThirdPersonCamera).
    if (granted & fr::kCanFly) {
        const float ground = floorAt(playerX_, playerY_, playerZ_ + fr::kLandHeight);
        const float gap = ground > -1e29f ? playerZ_ - ground : 99.0f;
        if (!(flyFlags_ & (fr::kFlying | fr::kSwimming)) && alive && !transportRiding()) {
            if (flyAscKey_ && !taxiLocksControls()) {
                // The jump key held: off at once, climbing from this frame (the climb below sends
                // MSG_MOVE_START_ASCEND with FLYING).
                flyFlags_ |= fr::kFlying;
                airborne_ = false;
                vz_ = 0.0f;
                flyPitch_ = 0.0f;
                jumpReq_ = false;
                if (sitting_) setSitting(false, false);
                if (++flyLogs_ <= 40)
                    PSP_LOGI(pp::tag::Net, "fly_take_off key=1 gap=%.2f z=%.1f speed=%.1f", static_cast<double>(gap),
                             static_cast<double>(playerZ_), static_cast<double>(session_.serverFlightSpeed()));
            }
        } else if ((flyFlags_ & fr::kFlying) && ground > -1e29f && fr::landing(true, flyAscKey_, playerZ_, ground)) {
            flyFlags_ &= ~(fr::kFlying | kClimb);  // landing (upstream setFlightAirborne(false))
            flyPitch_ = 0.0f;
            airborne_ = false;
            vz_ = 0.0f;
            playerZ_ = ground;
            flightSend(game::Opcode::MSG_MOVE_HEARTBEAT);
            nextHeartbeatMs_ = nowMs + kHeartbeatMs;
            if (++flyLogs_ <= 40)
                PSP_LOGI(pp::tag::Net, "fly_land gap=%.2f z=%.1f", static_cast<double>(gap), static_cast<double>(playerZ_));
        }
    }
    if (!(flyFlags_ & (fr::kFlying | fr::kSwimming))) return false;  // on foot / falling: the ground code
    jumpReq_ = false;  // the jump key climbs here (a stale one would jump on the next landing)
    mountJumpKey_ = mountJumpGo_ = false;
    airborne_ = false;
    vz_ = 0.0f;

    // Input (as updateMovement): the stick relative to the camera (the character turns to it), R + the face
    // buttons on controls=1.
    const bool lHeld = (pad.held & PSP_CTRL_LTRIGGER) != 0;
    const bool rHeld = (pad.held & PSP_CTRL_RTRIGGER) != 0;
    const bool mouseAutorun = lHeld && rHeld;
    const bool drivesFacing = controlsScheme_ < 2 && rHeld;
    const bool stick = controlsScheme_ >= 2;
    const bool stickAutorun = stick && autorun_ && !stickMove_;
    const bool nowForward = stick ? (moveY_ < 0.0f || stickAutorun) : ((pad.held & PSP_CTRL_TRIANGLE) || mouseAutorun);
    const bool nowBackward = stick ? moveY_ > 0.0f : (pad.held & PSP_CTRL_CROSS) != 0;
    const float fwd = stick ? (stickAutorun ? 1.0f : -moveY_)
                            : (nowForward ? 1.0f : 0.0f) - (nowBackward ? 1.0f : 0.0f);
    const float side = stick ? moveX_
                             : ((pad.held & PSP_CTRL_CIRCLE) ? 1.0f : 0.0f) - ((pad.held & PSP_CTRL_SQUARE) ? 1.0f : 0.0f);
    const float yawBase = stickAutorun ? facing_ : camYaw_;
    const glm::vec2 fdir(std::cos(yawBase), std::sin(yawBase));
    const glm::vec2 rdir(fdir.y, -fdir.x);
    glm::vec2 dir = fdir * fwd + rdir * side;
    const bool wantMove = alive && !taxiLocksControls() && glm::dot(dir, dir) > 1e-4f;
    if (drivesFacing) facing_ = camYaw_;
    uint32_t flags = 0;
    if (wantMove) {
        if (!drivesFacing) {
            flags = fr::kForward;
            dir = glm::normalize(dir);
            facing_ = std::atan2(dir.y, dir.x);
        } else {
            if (fwd > 0.0f) flags |= fr::kForward;
            if (fwd < 0.0f) flags |= fr::kBackward;
            if (side > 0.0f) flags |= fr::kStrafeRight;
            if (side < 0.0f) flags |= fr::kStrafeLeft;
        }
        // The pitch follows the camera while moving (upstream: while the camera steers), holds otherwise.
        float t = -(pitch_ - kCamRestPitch);
        t = std::fabs(t) <= kPitchDeadZone ? 0.0f : (t > 0.0f ? t - kPitchDeadZone : t + kPitchDeadZone);
        flyPitch_ = std::clamp(t, -kMaxFlightPitch, kMaxFlightPitch);
    }
    wantMove_ = wantMove;
    animBackward_ = drivesFacing && fwd < 0.0f;
    animStrafe_ = (drivesFacing && fwd == 0.0f) ? side : 0.0f;

    // The climb keys: the flags always (the step uses them); the packets only in flight (upstream: START_ASCEND /
    // START_DESCEND / STOP_ASCEND while airborne, none while swimming).
    const uint32_t climbWant = flyAscKey_ ? fr::kAscending : flyDescKey_ ? fr::kDescending : 0u;
    const uint32_t climbNow = flyFlags_ & kClimb;
    if (climbWant != climbNow) {
        flyFlags_ = (flyFlags_ & ~kClimb) | climbWant;
        if (flyFlags_ & fr::kFlying)
            flightSend(!climbWant                    ? game::Opcode::MSG_MOVE_STOP_ASCEND
                       : climbWant == fr::kAscending ? game::Opcode::MSG_MOVE_START_ASCEND
                                                     : game::Opcode::MSG_MOVE_START_DESCEND);
    }

    // The step: the flight / swim speed along the pitched facing, the climb added and the whole normalised.
    fr::Speeds sp;
    sp.walk = session_.serverWalkSpeed();
    sp.run = session_.serverRunSpeed();
    sp.runBack = session_.serverRunBackSpeed();
    sp.swim = session_.serverSwimSpeed();
    sp.swimBack = session_.serverSwimBackSpeed();
    sp.flight = session_.serverFlightSpeed();
    sp.flightBack = session_.serverFlightBackSpeed();
    const uint32_t mf = flags | flyFlags_;
    const float speed = fr::moveSpeed(mf, sp);
    fr::Vec3 d = fr::moveDirection(mf, facing_, flyPitch_);
    if (flyFlags_ & fr::kAscending) d.z += 1.0f;
    if (flyFlags_ & fr::kDescending) d.z -= 1.0f;
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    glm::vec3 to(playerX_, playerY_, playerZ_);
    if (len > 1e-4f && speed > 0.0f) to += glm::vec3(d.x, d.y, d.z) * (speed * dtSec / len);

    // Walls as on foot, the ground holds a flier / swimmer up (a step onto a floor, never into the terrain unless a
    // structure's floor is there - a tunnel), a swimmer stays under the surface.
    glm::vec3 wallTo(to.x, to.y, playerZ_);
    if (to.x != playerX_ || to.y != playerY_) {
        models_.collideWalls(wallTo, playerZ_);
        bigWmoWalls(wallTo, playerZ_);
        transportWalls(wallTo, playerZ_);
        float tz0 = 0.0f;
        if (!(terrain_.heightAt(wallTo.x, wallTo.y, tz0) || wmoOnly_ || transportRiding())) {
            wallTo.x = playerX_;  // off the loaded tiles: stay
            wallTo.y = playerY_;
        }
    }
    float nz = to.z;
    if ((flyFlags_ & fr::kSwimming) && haveLiquid) nz = std::min(nz, liquid - fr::kWaterSurfaceOffset);
    bool onTerrain = false;
    float floorZ = floorAt(wallTo.x, wallTo.y, nz + 0.6f, &onTerrain);
    if (floorZ > -1e29f && floorZ > nz) nz = floorZ;
    float tz = 0.0f;
    if (terrain_.heightAt(wallTo.x, wallTo.y, tz) && tz > nz && !(floorZ > -1e29f && !onTerrain)) {
        // Into the hillside: a slope is flown up along it (a step's rise at this speed), a cliff stops the step.
        const float maxRise = std::max(0.6f, 1.5f * speed * dtSec);
        if (tz - nz <= maxRise) {
            nz = tz;
        } else {
            wallTo.x = playerX_;
            wallTo.y = playerY_;
            if (terrain_.heightAt(wallTo.x, wallTo.y, tz) && tz > nz) nz = tz;
        }
    }
    playerX_ = wallTo.x;
    playerY_ = wallTo.y;
    playerZ_ = nz;

    // The server (as updateMovement's: START / STOP / HEARTBEAT / SET_FACING); the pitch rides in every packet.
    const uint32_t oldFlags = moveFlags_;
    moveFlags_ = flags;
    const bool wasMoving = moving_;
    float df = std::fabs(wrapPi(facing_ - sentFacing_));
    if (!wantMove) {
        if (moving_) {
            moving_ = false;
            sendMove(game::Opcode::MSG_MOVE_STOP, false);
            nextHeartbeatMs_ = nowMs + kHeartbeatMs;
        } else if ((flyFlags_ & kClimb) && nowMs >= nextHeartbeatMs_) {
            nextHeartbeatMs_ = nowMs + kHeartbeatMs;
            sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, false);
        } else if (df > 0.1f && nowMs >= nextHeartbeatMs_) {
            nextHeartbeatMs_ = nowMs + 200;
            sendMove(game::Opcode::MSG_MOVE_SET_FACING, false);
        }
    } else if (!wasMoving || flags != oldFlags) {
        moving_ = true;
        nextHeartbeatMs_ = nowMs + kHeartbeatMs;
        const auto start = (flags & fr::kForward)      ? game::Opcode::MSG_MOVE_START_FORWARD
                           : (flags & fr::kBackward)   ? game::Opcode::MSG_MOVE_START_BACKWARD
                           : (flags & fr::kStrafeLeft) ? game::Opcode::MSG_MOVE_START_STRAFE_LEFT
                                                       : game::Opcode::MSG_MOVE_START_STRAFE_RIGHT;
        sendMove(start, true);
    } else if (df > 0.1f) {
        sendMove(game::Opcode::MSG_MOVE_SET_FACING, true);
    } else if (nowMs >= nextHeartbeatMs_) {
        nextHeartbeatMs_ = nowMs + kHeartbeatMs;
        sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, true);
    }
    // Own rule: a pitch change is said with an early heartbeat (0.03 rad or more, at most every 200 ms).
    if (std::fabs(flyPitch_ - sentPitch_) > kPitchSendDelta && nowMs >= nextPitchMs_) {
        nextPitchMs_ = nowMs + kPitchSendMs;
        nextHeartbeatMs_ = nowMs + kHeartbeatMs;
        sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, moving_);
    }
    // One sample every 10 s for the hardware log.
    if (nowMs >= flyFrameLogMs_) {
        flyFrameLogMs_ = nowMs + 10000u;
        PSP_LOGI(pp::tag::Net, "fly_frame flags=%08X pitch=%.2f speed=%.2f z=%.1f depth=%.2f h=%.2f",
                 static_cast<unsigned>(granted | flyFlags_ | moveFlags_), static_cast<double>(flyPitch_),
                 static_cast<double>(speed), static_cast<double>(playerZ_), static_cast<double>(depth),
                 static_cast<double>(fr::kWaterSurfaceOffset));
    }
    return true;
}

// A swimming mount plays the swim set (in the air the MountFSM's flight set takes it: updateMountedAnimation).
bool WorldScreen::flightMountAnim() {
    const uint64_t key = kMountGuidTag | (session_.playerGuid() & 0x00FFFFFFFFFFFFFFull);
    if (!ownSwimming()) {
        if (flyMountDriven_) {  // MountFSM takes the mount back (its current animation differs from its pick)
            flyMountDriven_ = false;
            flyMountAnim_ = 0;
        }
        return false;
    }
    const uint32_t display = ownMountDisplay_;
    const uint32_t want = fr::resolveAnim(fr::swimAnim(moveFlags_),
                                          [&](uint32_t id) { return creatures_.displayHasAnimation(display, id); });
    if (!flyMountDriven_ || want != flyMountAnim_) {
        creatures_.playDriven(key, want, true);
        flyMountAnim_ = want;
        flyMountDriven_ = true;
        if (++flyLogs_ <= 40)
            PSP_LOGI(pp::tag::Gfx, "fly_mount_anim display=%u anim=%u flags=%08X", static_cast<unsigned>(display),
                     static_cast<unsigned>(want), static_cast<unsigned>(moveFlags_ | flyFlags_));
    }
    // The rider sits in Mount (as updateMountedAnimation).
    uint32_t cur = 0;
    float t = 0.0f, dur = 0.0f;
    const bool lost = !player_.getAnimationState(cur, t, dur) || (cur != anim::MOUNT && player_.hasAnimation(anim::MOUNT));
    if (lastAnimRequest_ != anim::MOUNT || lost) {
        player_.playAnimation(anim::MOUNT, true);
        lastAnimRequest_ = anim::MOUNT;
        lastAnimLoop_ = true;
    }
    mountBob_ = 0.0f;
    return true;
}

// The own player flying on foot (upstream's flying unit: Fly moving, Hover still, through AnimationData's fallbacks).
bool WorldScreen::flightOwnAnim() {
    if (!ownFlying() || mountActive_) return false;
    const uint32_t want = fr::flyAnim(wantMove_, [&](uint32_t id) { return player_.hasAnimation(id); });
    uint32_t cur = 0;
    float t = 0.0f, dur = 0.0f;
    const bool drifted = player_.getAnimationState(cur, t, dur) && cur != want && player_.hasAnimation(want);
    if (lastAnimRequest_ != want || drifted) {
        player_.playAnimation(want, true);
        lastAnimRequest_ = want;
        lastAnimLoop_ = true;
    }
    return true;
}

bool WorldScreen::flightRemoteKeepsZ(uint64_t guid) const { return fr::swimOrFly(session_.remoteMoveFlags(guid)); }

bool WorldScreen::flightRemoteSwims(uint64_t guid) const { return (session_.remoteMoveFlags(guid) & fr::kSwimming) != 0; }

bool WorldScreen::flightRemoteFlies(uint64_t guid) const {
    const uint32_t f = session_.remoteMoveFlags(guid);
    return (f & fr::kFlying) && !(f & fr::kSwimming);
}

// Another player swimming (the swim set) or flying (Fly / Hover), as upstream's remote player sync.
bool WorldScreen::flightRemoteAnim(uint64_t guid, RemotePlayer& r, bool moving) {
    const uint32_t f = session_.remoteMoveFlags(guid);
    if (!fr::swimOrFly(f)) {
        if (r.flySwim) {  // out of the water / air: the ground logic picks Stand / Walk / Run again
            r.flySwim = false;
            r.wasMoving = !moving;
            r.airborne = false;
        }
        return false;
    }
    rendering::psp::CharacterModel& m = *r.model;
    auto has = [&](uint32_t id) { return m.hasAnimation(id); };
    const uint32_t want = (f & fr::kSwimming) ? fr::resolveAnim(fr::swimAnim(moving ? f : (f & ~fr::kDirections)), has)
                                              : fr::flyAnim(moving, has);
    if (!r.flySwim || r.anim != want) {
        m.playAnimation(want, true);
        r.anim = want;
        r.flySwim = true;
    }
    r.airborne = false;
    r.sittingDown = false;
    session_.takeJumpHint(guid);
    m.update(lastDt_);
    return true;
}

}  // namespace wowee::ui::psp
