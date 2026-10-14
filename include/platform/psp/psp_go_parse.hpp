// Game object use: the parts of the game object packets the PSP reads beyond upstream's parsers,
// header only (the session psp_world_session_gouse.cpp and the host test tools/psp/host_tests/gameobject_use_test.cpp).
#pragma once

#include "network/packet.hpp"

#include <cstdint>

namespace wowee::platform::psp {

// SMSG_GAMEOBJECT_QUERY_RESPONSE after the 24 data fields upstream's parser reads (3.3.5a: AzerothCore / TrinityCore
// QueryHandler write `float size`, `uint32 questItems[MAX_GAMEOBJECT_QUEST_ITEMS = 6]`, `int32 unk` - the client's
// cache entry 0x78 = the quest items the GameTooltip reads). Missing fields stay as they were; a size out of reason
// becomes 1.
inline void readGoQueryTail(network::Packet& p, float& size, uint32_t (&questItems)[6]) {
    if (p.hasRemaining(4)) size = p.readFloat();
    for (uint32_t& q : questItems)
        if (p.hasRemaining(4)) q = p.readUInt32();
    if (!(size > 0.001f && size < 1000.0f)) size = 1.0f;
}

// CMSG_ALTER_APPEARANCE (ApplyBarberShopStyle): AzerothCore WorldSession::HandleAlterAppearance reads
// Hair (BarberShopStyle id, type 0), Color, FacialHair (BarberShopStyle id, type 2), SkinColor (type 3, 0 = none).
inline void writeAlterAppearance(network::Packet& p, uint32_t hair, uint32_t color, uint32_t facial, uint32_t skin) {
    p.writeUInt32(hair);
    p.writeUInt32(color);
    p.writeUInt32(facial);
    p.writeUInt32(skin);
}

}  // namespace wowee::platform::psp
