// Transports: MovementInfo's ONTRANSPORT block as the original
// 3.3.5a client writes it and TrinityCore / AzerothCore relay it (WorldSession::WriteMovementInfo): after the
// facing - the transport's packed guid, the place in its frame (x, y, z, o), the transport time, the seat (int8),
// and a second time when MOVEMENTFLAG2_INTERPOLATED_MOVEMENT (0x400) is set; then pitch, fall time, jump as without
// a transport (upstream MovementPacket::writeMovementPayload writes the same order). Header-only for the host test
// (tools/psp/host_tests/transport_parse_test.cpp).
#pragma once

#include "game/world_packets.hpp"
#include "network/packet.hpp"

#include <cmath>

namespace wowee::platform::psp {

inline void readTransportMoveBlock(network::Packet& packet, game::MovementInfo& info) {
    if (!packet.hasFullPackedGuid()) return;  // the whole guid (was: its mask byte only)
    info.transportGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(21)) return;
    info.transportX = packet.readFloat();
    info.transportY = packet.readFloat();
    info.transportZ = packet.readFloat();
    info.transportO = packet.readFloat();
    info.transportTime = packet.readUInt32();
    info.transportSeat = static_cast<int8_t>(packet.readUInt8());
    if ((info.flags2 & 0x0400u) && packet.hasRemaining(4)) info.transportTime2 = packet.readUInt32();
}

// SMSG_MONSTER_MOVE_TRANSPORT's head - the mover's packed guid, the transport's
// packed guid, the seat (int8); SMSG_MONSTER_MOVE's body follows. False when the packet ends inside it (a guid cut
// short would name some other object - a rider of nothing).
inline bool readMonsterMoveTransportHead(network::Packet& packet, uint64_t& mover, uint64_t& transport, int8_t& seat) {
    if (!packet.hasFullPackedGuid()) return false;
    mover = packet.readPackedGuid();
    if (!packet.hasFullPackedGuid()) return false;
    transport = packet.readPackedGuid();
    if (!packet.hasRemaining(1)) return false;
    seat = static_cast<int8_t>(packet.readUInt8());
    return true;
}

// An angle from the server (an orientation, a facing on a transport) as the
// transport code keeps it - finite, in [-pi, pi]. A NaN / inf there is 0 (it would reach the placements and the
// movement packets), a huge value is wrapped by fmod (subtracting 2 pi in a loop never ends at |a| > ~3e7, where it
// is below the float's step).
inline float cleanTransportAngle(float a) {
    constexpr float kPi = 3.14159265f, kTwoPi = 2.0f * kPi;
    if (!std::isfinite(a)) return 0.0f;
    if (a > kPi || a < -kPi) {
        a = std::fmod(a + kPi, kTwoPi);  // (-2 pi, 2 pi)
        if (a < 0.0f) a += kTwoPi;       // [0, 2 pi]
        a -= kPi;
    }
    return a;
}

}  // namespace wowee::platform::psp
