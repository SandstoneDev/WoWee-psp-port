// PSP (a hardware test: ".tele" to a capital city - the player appeared UNDER a city district,
// fell through the big WMO whose floor collision was not read yet): the landing hold - pure logic, host-tested
// (tools/psp/host_tests/memory_budget_host_test.sh).
//
// The client does not drop a teleported player into a world it has not loaded: it shows the world only when the
// ground under the new position is there. PSP: after teleport_applied (and at world entry) the player's position is
// held - no gravity, no fall, movement input ignored, no heartbeat with a falling flag - until the collision under the
// player is ready: inside a big WMO's box, that WMO open with its collision chunks around the player resident (a floor
// of the terrain below the city must not catch the player); then a floor at the server's height (terrain, doodad, big
// WMO, transport), or the tile under the player in with its terrain AND its doodads (an inn's upper floor is a doodad
// pak model); or tphold ms passed.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace wowee::ui::psp {

// PSP (an emulator run: the test character entered the world as a ghost and stood still in a capital city -
// the stick walked only the living): who walks - upstream lets everyone but a corpse move (a ghost runs back to its
// body, jumps). `life` as WorldSession::Life: 0 alive, 1 dead (the corpse), 2 ghost.
inline bool lifeWalks(int life) { return life != 1; }

// world_flush: whether a teleport from (ox, oy) to (nx, ny) (server coordinates, yards) leaves the
// stream window - the new window of (2 * radius + 1)^2 tiles shares no tile with the old one (tile = floor(32 - y /
// 533.33), floor(32 - x / 533.33), as TileStreamer). Then everything streamed for the old place can go at once.
inline bool teleportLeavesWindow(float ox, float oy, float nx, float ny, int radius) {
    constexpr float kTile = 533.33333f;
    const int otx = static_cast<int>(std::floor(32.0f - oy / kTile)), oty = static_cast<int>(std::floor(32.0f - ox / kTile));
    const int ntx = static_cast<int>(std::floor(32.0f - ny / kTile)), nty = static_cast<int>(std::floor(32.0f - nx / kTile));
    return std::abs(ntx - otx) > 2 * radius || std::abs(nty - oty) > 2 * radius;
}

struct TeleportHoldState {
    bool terrainLoaded = false;  // the terrain tile under the player (or a WMO-only map)
    bool doodadsLoaded = false;  // the doodads of that tile are in (or it has none)
    bool insideBigWmo = false;   // the position is inside a big WMO's box
    bool wmoReady = false;       // that WMO is open and its collision around the player resident
    bool floorNear = false;      // a floor within 2 yd below the server's height (or 0.6 above) exists now
};

enum class TeleportHoldWhy : uint8_t { kHold = 0, kFloor, kTimeout };

inline TeleportHoldWhy teleportHoldDecide(const TeleportHoldState& s, uint32_t elapsedMs, uint32_t holdMs) {
    const bool timeout = elapsedMs >= holdMs;
    if (s.insideBigWmo && !s.wmoReady) return timeout ? TeleportHoldWhy::kTimeout : TeleportHoldWhy::kHold;
    if (s.floorNear) return TeleportHoldWhy::kFloor;
    if (s.terrainLoaded && s.doodadsLoaded) return TeleportHoldWhy::kFloor;  // nothing at that height: a real fall
    return timeout ? TeleportHoldWhy::kTimeout : TeleportHoldWhy::kHold;
}

}  // namespace wowee::ui::psp
