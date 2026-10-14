// Inspect: SMSG_INSPECT_TALENT read as the servers write it
// (AzerothCore / TrinityCore 3.3.5) - pure (network::Packet only), so the host test (tools/psp/host_tests/inspect_parse_test.cpp)
// runs the same code as the PSP session (psp_world_session_inspect.cpp).
//
// After the packed guid:
//   - the talents (servers: Player::BuildPlayerTalentsInfoData): unspent u32, group count u8,
//     active group u8, then per group a u8 count of (talent id u32, rank u8 from zero) and a u8 count of u16 glyphs;
//   - the items (servers: Player::BuildEnchantmentsInfoData): a u32 mask of the 19 equipment
//     slots, per set slot: the entry u32, a u16 mask of the 12
//     enchantment slots and a u16 per set bit, a u16 (servers: 0), the creator's packed guid, a u32 (servers: 0).
// Upstream (SocialHandler::handleInspectResults) stops after the enchantments: the next slot's entry is read from
// the u16 / guid / u32 of the one before, every item after the first comes out wrong.
#pragma once

#include "network/packet.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace wowee::platform::psp {

struct InspectItemRecord {      // one slot of BuildEnchantmentsInfoData
    uint32_t entry = 0;         // the item entry (0 = the slot is empty)
    uint16_t enchant[12] = {};  // by EnchantmentSlot: 0 permanent, 1 temporary, 2..4 sockets, 5 bonus, 6 prismatic
    uint16_t randomProperty = 0;
    uint64_t creator = 0;       // ITEM_FIELD_CREATOR
    uint32_t suffixFactor = 0;
};

struct InspectTalentRead {
    uint32_t unspent = 0;
    uint8_t groups = 0, active = 0;
    std::unordered_map<uint32_t, uint8_t> ranks;  // the active group: talent id -> rank (1..5)
    uint32_t points = 0;                          // spent in the active group
    bool talentsOk = false, gearOk = false;
    std::array<InspectItemRecord, 19> slots{};
    int items = 0;
};

// Everything after the packed guid. False when the talents do not read (gearOk then false too).
inline bool readInspectTalent(network::Packet& packet, InspectTalentRead& out) {
    out = InspectTalentRead{};
    if (!packet.hasRemaining(6)) return false;
    out.unspent = packet.readUInt32();
    out.groups = packet.readUInt8();
    out.active = packet.readUInt8();
    for (uint8_t g = 0; g < out.groups; ++g) {
        if (!packet.hasRemaining(1)) return false;
        const uint8_t count = packet.readUInt8();
        for (uint8_t t = 0; t < count; ++t) {
            if (!packet.hasRemaining(5)) return false;
            const uint32_t talentId = packet.readUInt32();
            const uint8_t rank = packet.readUInt8();
            if (g == out.active) {
                out.ranks[talentId] = static_cast<uint8_t>(rank + 1u);
                out.points += rank + 1u;
            }
        }
        if (!packet.hasRemaining(1)) return false;
        const uint8_t glyphs = packet.readUInt8();
        for (uint8_t k = 0; k < glyphs; ++k) {
            if (!packet.hasRemaining(2)) return false;
            packet.readUInt16();
        }
    }
    out.talentsOk = true;
    if (!packet.hasRemaining(4)) return true;
    uint32_t mask = packet.readUInt32();
    for (int slot = 0; slot < 19 && mask; ++slot, mask >>= 1) {
        if (!(mask & 1u)) continue;
        if (!packet.hasRemaining(6)) return true;
        InspectItemRecord& s = out.slots[static_cast<size_t>(slot)];
        s.entry = packet.readUInt32();
        uint16_t enchMask = packet.readUInt16();
        for (int k = 0; enchMask && k < 16; ++k, enchMask >>= 1) {
            if (!(enchMask & 1u)) continue;
            if (!packet.hasRemaining(2)) return true;
            const uint16_t id = packet.readUInt16();
            if (k < 12) s.enchant[k] = id;
        }
        if (!packet.hasRemaining(2)) return true;
        s.randomProperty = packet.readUInt16();
        if (!packet.hasFullPackedGuid()) return true;
        s.creator = packet.readPackedGuid();
        if (!packet.hasRemaining(4)) return true;
        s.suffixFactor = packet.readUInt32();
        ++out.items;
    }
    out.gearOk = true;
    return true;
}

}  // namespace wowee::platform::psp
