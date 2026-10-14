// PSP: npc-walk - Walk or Run for a unit that moves on a server spline (SMSG_MONSTER_MOVE /
// SMSG_MONSTER_MOVE_TRANSPORT). Header-only for the host test (tools/psp/host_tests/npc_walk_test.cpp).
//
// A 3.3.5a server does not say "walk" for a spline. AzerothCore's MoveSplineInit::SetWalk (waypoints, random
// movement, home) only picks the walk SPEED for the spline ("not add walk flag to unit", MoveSplineInit::Launch);
// the unit's MOVEMENTFLAG_WALKING is not touched (no SMSG_SPLINE_MOVE_SET_WALK_MODE, none in the create block) and
// its MoveSplineFlag has no walk bit at all (0x1000 is CanSwim there, never set). The stock client still shows
// those NPCs walking, so it must take the gait from the spline's velocity: at the unit's walk speed it walks. The
// split is half way between the unit's walk and run speeds (a snared runner gets a lower run speed from
// SMSG_SPLINE_SET_RUN_SPEED and keeps running). Servers that do send a walk bit in the spline (MaNGOS family:
// MoveSplineFlag::Walkmode 0x1000) agree with the speed anyway.
#pragma once

#include <cmath>
#include <cstdint>

namespace wowee::platform::psp {

// Path length of a spline over its duration, yd/s (3D: the server times the spline by its 3D length); 0 when the
// spline is a turn on the spot or has no duration.
inline float splinePathSpeed(float length, uint32_t durationMs) {
    if (durationMs == 0 || !(length > 0.05f)) return 0.0f;
    return length * 1000.0f / static_cast<float>(durationMs);
}

// True: Walk. speed 0 (unknown) is never a walk - the caller keeps what it had.
inline bool splineSpeedIsWalk(float speed, float walkSpeed, float runSpeed) {
    if (!(speed > 0.0f) || !std::isfinite(speed)) return false;
    if (!(walkSpeed > 0.0f)) walkSpeed = 2.5f;  // the client's default walk speed
    // Half way to the run speed; a unit that runs no faster than it walks (or nearly) gets 25 % over its walk.
    const float limit = runSpeed > walkSpeed * 1.5f ? 0.5f * (walkSpeed + runSpeed) : walkSpeed * 1.25f;
    return speed <= limit;
}

}  // namespace wowee::platform::psp
