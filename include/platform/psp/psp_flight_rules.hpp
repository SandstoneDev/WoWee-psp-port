// Flying: the movement-flag rules of the own player's flight and swimming, the server's flag toggles and their ACKs,
// the speed and the direction of a step, and the locomotion animation of a flier or a swimmer. Pure functions over
// game::MovementInfo (server coordinates), shared by the session, the world screen and the host test
// (tools/psp/host_tests/flight_rules_test.cpp). Sources: upstream WoWee (MovementHandler, CameraController,
// AnimationController / MountFSM), AzerothCore (UnitDefines.h MovementFlags, MiscHandler.cpp
// HandleMoveFlagChangeOpcode), wowdev's AnimationData.dbc, and the port's own rules where neither says (marked "Own").
#pragma once

#include "game/world_packets.hpp"
#include "network/packet.hpp"

#include <cmath>
#include <cstdint>

namespace wowee::platform::psp::flight {

// MovementFlags (AzerothCore UnitDefines.h; upstream game::MovementFlags).
constexpr uint32_t kForward = 0x1, kBackward = 0x2, kStrafeLeft = 0x4, kStrafeRight = 0x8;
constexpr uint32_t kTurnLeft = 0x10, kTurnRight = 0x20, kPitchUp = 0x40, kPitchDown = 0x80, kWalking = 0x100;
constexpr uint32_t kOnTransport = 0x200, kLevitating = 0x400, kRoot = 0x800, kFalling = 0x1000, kFallingFar = 0x2000;
constexpr uint32_t kPendingRoot = 0x100000, kSwimming = 0x200000, kAscending = 0x400000, kDescending = 0x800000;
constexpr uint32_t kCanFly = 0x1000000, kFlying = 0x2000000, kSplineElevation = 0x4000000;
constexpr uint32_t kWaterWalk = 0x10000000, kFeatherFall = 0x20000000, kHover = 0x40000000;
constexpr uint32_t kDirections = kForward | kBackward | kStrafeLeft | kStrafeRight;
constexpr uint32_t kClimbs = kAscending | kDescending;
// MovementFlags2 (AzerothCore MOVEMENTFLAG2_NO_STRAFE, MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING).
constexpr uint16_t kFlags2NoStrafe = 0x1, kFlags2AlwaysAllowPitching = 0x20;

// The flags the server grants and takes with its SMSG_MOVE_* toggles; every movement packet we send carries them.
constexpr uint32_t kServerGranted = kCanFly | kWaterWalk | kFeatherFall | kHover | kLevitating;

inline bool swimOrFly(uint32_t f) { return (f & (kSwimming | kFlying)) != 0; }

// The pitch travels in the movement block while swimming / flying or with ALWAYS_ALLOW_PITCHING (AzerothCore's
// movement block writer and reader test exactly these flags).
inline bool pitchInPacket(uint32_t f, uint16_t f2) { return swimOrFly(f) || (f2 & kFlags2AlwaysAllowPitching); }

// --- The server's toggles ------------------------------------------------------------------------------------------

// SMSG_MOVE_SET_CAN_FLY / _UNSET, WATER_WALK / LAND_WALK, FEATHER_FALL / NORMAL_FALL, SET_HOVER / UNSET,
// GRAVITY_DISABLE / _ENABLE, SET / UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY (upstream MovementHandler's table).
enum class Toggle : uint8_t { CanFly, WaterWalk, FeatherFall, Hover, GravityOff, CanSwimToFly };

// The flag a toggle sets / clears (upstream handleForceMoveFlagChange's table; the swim-to-fly toggle changes no flag
// there - AzerothCore names its bit only "unknown" and ignores its ACK).
inline uint32_t toggleFlag(Toggle t) {
    switch (t) {
        case Toggle::CanFly: return kCanFly;
        case Toggle::WaterWalk: return kWaterWalk;
        case Toggle::FeatherFall: return kFeatherFall;
        case Toggle::Hover: return kHover;
        case Toggle::GravityOff: return kLevitating;
        default: return 0;
    }
}

// Upstream handleForceMoveFlagChange: the flag alone is set / cleared before the ACK. What follows from it (a flight
// that loses CAN_FLY ends, the mover falls) happens on the next frame and goes out with the next movement packet.
inline void applyToggle(game::MovementInfo& mi, Toggle t, bool on) {
    const uint32_t flag = toggleFlag(t);
    mi.flags = on ? (mi.flags | flag) : (mi.flags & ~flag);
}

// The gravity ACKs end after the movement block; every other toggle ACK has one more u32 (AzerothCore
// HandleMoveFlagChangeOpcode reads "uint32 isApplied" except for CMSG_MOVE_GRAVITY_DISABLE_ACK / _ENABLE_ACK).
inline bool ackHasApplied(Toggle t) { return t != Toggle::GravityOff; }

// A toggle's ACK: packed guid, the server's counter, the movement block, then (unless a gravity ACK) isApplied =
// 1 / 0 as the name says (AzerothCore reads it as a uint32).
inline network::Packet buildMoveAck(uint16_t wireOpcode, uint64_t guid, uint32_t counter, const game::MovementInfo& mi,
                                    bool withApplied, bool applied) {
    network::Packet p(wireOpcode);
    p.writePackedGuid(guid);
    p.writeUInt32(counter);
    game::MovementPacket::writeMovementPayload(p, mi);
    if (withApplied) p.writeUInt32(applied ? 1u : 0u);
    return p;
}

// --- Landing and water (upstream CameraController) -------------------------------------------------------------------

// updateThirdPersonCamera: a flight lands when the feet are on real ground (within 0.3 yd) and the jump key is not
// held.
constexpr float kLandHeight = 0.3f;
inline bool landing(bool flying, bool climbingUp, float feetZ, float groundZ) {
    return flying && !climbingUp && feetZ - groundZ < kLandHeight && feetZ - groundZ > -kLandHeight;
}

// moveFollowedCharacter: the water must be 1.15 yd deep (from the floor under the feet, or from the feet when no floor
// is found) to start swimming and stays a swim down to 0.85 yd (the hysteresis); the feet must be under the surface.
// A swimmer floats with the feet 1.45 yd under the surface (WATER_SURFACE_OFFSET).
constexpr float kSwimEnterDepth = 1.15f, kSwimExitDepth = 0.85f, kWaterSurfaceOffset = 1.45f;
inline bool inWater(bool liquid, float surfaceZ, float feetZ, bool haveFloor, float floorZ, bool swimming) {
    if (!liquid || !(feetZ < surfaceZ)) return false;
    const float depth = haveFloor ? surfaceZ - floorZ : surfaceZ - feetZ;
    return depth >= (swimming ? kSwimExitDepth : kSwimEnterDepth);
}

// --- A step ---------------------------------------------------------------------------------------------------------

// The speed of a step (upstream CameraController's flight / swim branches): the flight / swim speed, its back speed
// when moving backwards only; on the ground walk / back / run. Zero without a direction or climb flag.
struct Speeds {
    float walk = 2.5f, run = 7.0f, runBack = 4.5f, swim = 4.722f, swimBack = 2.5f, flight = 7.0f, flightBack = 4.5f;
};
inline float moveSpeed(uint32_t f, const Speeds& s) {
    if (!(f & (kDirections | kClimbs))) return 0.0f;
    const bool backOnly = (f & kBackward) && !(f & kForward);
    if (f & kFlying) return backOnly ? s.flightBack : s.flight;
    if (f & kSwimming) return backOnly ? s.swimBack : s.swim;
    if (f & kWalking) return s.walk;
    return backOnly ? s.runBack : s.run;
}

struct Vec3 {
    float x, y, z;
};

// The direction of a step (upstream CameraController's flight / swim branches): forward / back along the facing
// pitched up / down (the pitch only while swimming / flying), a strafe level and square to the facing (left =
// facing + 90 deg), summed - the caller adds the climb and normalises the whole.
inline Vec3 moveDirection(uint32_t f, float facing, float pitch) {
    const float c = std::cos(facing), s = std::sin(facing);
    Vec3 fwd{c, s, 0.0f};
    if (swimOrFly(f)) {
        const float cp = std::cos(pitch);
        fwd = Vec3{cp * c, cp * s, std::sin(pitch)};
    }
    Vec3 d{0.0f, 0.0f, 0.0f};
    auto add = [&](const Vec3& v, float k) {
        d.x += v.x * k;
        d.y += v.y * k;
        d.z += v.z * k;
    };
    if (f & kForward) add(fwd, 1.0f);
    if (f & kBackward) add(fwd, -1.0f);
    if (f & kStrafeLeft) add(Vec3{-s, c, 0.0f}, 1.0f);
    if (f & kStrafeRight) add(Vec3{s, -c, 0.0f}, 1.0f);
    return d;
}

// --- Animation ------------------------------------------------------------------------------------------------------

// AnimationData.dbc ids used here (wowdev; upstream rendering::anim).
constexpr uint32_t kAnimStand = 0, kAnimWalk = 4, kAnimSwimIdle = 41, kAnimSwim = 42, kAnimSwimLeft = 43,
                   kAnimSwimRight = 44, kAnimSwimBackwards = 45, kAnimFly = 135, kAnimHover = 193;

// A swimming unit (upstream LocomotionFSM SWIM / SWIM_IDLE and its swim-direction candidates): SwimLeft / SwimRight
// on a strafe, SwimBackwards backwards, else Swim; SwimIdle without a direction flag.
inline uint32_t swimAnim(uint32_t f) {
    if (!(f & kDirections)) return kAnimSwimIdle;
    if (f & (kStrafeLeft | kStrafeRight)) return (f & kStrafeLeft) ? kAnimSwimLeft : kAnimSwimRight;
    return (f & kBackward) ? kAnimSwimBackwards : kAnimSwim;
}

// AnimationData.dbc's Fallback column for the ids above and their chains: walked when a model lacks an animation.
// 0 ends (Stand).
inline uint32_t animFallback(uint32_t id) {
    switch (id) {
        case kAnimSwimBackwards: return kAnimSwim;
        case kAnimSwim: return kAnimWalk;
        case kAnimFly: return kAnimSwim;
        default: return kAnimStand;  // SwimIdle, SwimLeft, SwimRight, Hover, Walk, Run -> Stand
    }
}
template <typename Has>
inline uint32_t resolveAnim(uint32_t id, Has&& has) {
    for (int i = 0; i < 6; ++i) {
        if (has(id)) return id;
        if (id == kAnimStand) break;
        id = animFallback(id);
    }
    return kAnimStand;
}

// A flying unit (upstream: creatures and other players FlyForward moving / FlyIdle still, MountFSM's flyIdle = Hover,
// else Fly): moving Fly, still Hover, else Fly; a model with neither walks the fallback chain.
template <typename Has>
inline uint32_t flyAnim(bool moving, Has&& has) {
    if (moving) return resolveAnim(kAnimFly, has);
    if (has(kAnimHover)) return kAnimHover;
    if (has(kAnimFly)) return kAnimFly;
    return resolveAnim(kAnimHover, has);
}

}  // namespace wowee::platform::psp::flight
