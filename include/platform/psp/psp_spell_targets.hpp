// SpellCastTargets and the two cast requests that carry it - CMSG_CAST_SPELL and CMSG_USE_ITEM - in the field order
// AzerothCore reads them (WorldSession::HandleCastSpellOpcode / HandleUseItemOpcode, SpellCastTargets::Read).
//
//   - SpellCastTargets: u32 mask; packed guid of the object if mask & 0x18A02 (UNIT, CORPSE_ENEMY, GAMEOBJECT,
//     CORPSE_ALLY, UNIT_MINIPET); packed guid of the item if mask & 0x1010 (ITEM, TRADE_ITEM); SOURCE_LOCATION 0x20:
//     packed transport guid + x, y, z; DEST_LOCATION 0x40: the same; STRING 0x2000: a C string.
//   - the cast request: CMSG_CAST_SPELL = u8 castCount, u32 spellId, u8 castFlags, targets;
//     CMSG_USE_ITEM = u8 bag, u8 slot, u8 castCount, u32 spellId, u64 item guid, u32 glyph index, u8 castFlags,
//     targets; castFlags 0x2 adds the missile trajectory (AzerothCore HandleClientCastFlags) - not sent here.
//   - what a spell asks for: Spell.dbc Targets plus the explicit mask its implicit target implies, as AzerothCore
//     SpellImplicitTargetInfo::GetExplicitTargetMask computes it (spellTargetRequest below; PSP: spells.pak keeps
//     EffectImplicitTargetA of the first effect only); 0 = nothing to pick, the request goes out with mask 0.
//   - on the wire a picked unit is TARGET_FLAG_UNIT (0x2) - SpellCastTargets::Read takes an object guid for UNIT,
//     the ally / enemy / party / raid bits only say which units may be picked; a game object 0x800 (also for a
//     GAMEOBJECT_ITEM request); an item 0x10.
// Pure: no PSP calls, compiled by the host test (tools/psp/host_tests/spell_targets_host_test.sh).
#pragma once

#include "network/packet.hpp"

#include <cstdint>
#include <string>

namespace wowee::platform::psp {

// TARGET_FLAG_* (AzerothCore SpellInfo.h SpellCastTargetFlags).
enum SpellTargetFlag : uint32_t {
    kTargetFlagSelf = 0x00000000,
    kTargetFlagUnit = 0x00000002,
    kTargetFlagUnitRaid = 0x00000004,
    kTargetFlagUnitParty = 0x00000008,
    kTargetFlagItem = 0x00000010,
    kTargetFlagSourceLocation = 0x00000020,
    kTargetFlagDestLocation = 0x00000040,
    kTargetFlagUnitEnemy = 0x00000080,
    kTargetFlagUnitAlly = 0x00000100,
    kTargetFlagCorpseEnemy = 0x00000200,
    kTargetFlagUnitDead = 0x00000400,
    kTargetFlagGameObject = 0x00000800,
    kTargetFlagTradeItem = 0x00001000,
    kTargetFlagString = 0x00002000,
    kTargetFlagGameObjectItem = 0x00004000,
    kTargetFlagCorpseAlly = 0x00008000,
    kTargetFlagUnitMinipet = 0x00010000,
    kTargetFlagUnitPassenger = 0x00100000,
    // The groups SpellCastTargets::Read / Write test.
    kTargetMaskObjectGuid = 0x00018A02,  // the object guid is written
    kTargetMaskItemGuid = 0x00001010,    // the item guid is written
    // a unit may be picked: UNIT, RAID, PARTY, ENEMY, ALLY, MINIPET, PASSENGER
    kTargetMaskUnitPick = kTargetFlagUnit | kTargetFlagUnitRaid | kTargetFlagUnitParty | kTargetFlagUnitEnemy |
                          kTargetFlagUnitAlly | kTargetFlagUnitMinipet | kTargetFlagUnitPassenger,
};

struct SpellCastTargets {
    uint32_t mask = 0;
    uint64_t object = 0;  // unit / game object / corpse (0x18A02)
    uint64_t item = 0;    // item / trade item (0x1010; a trade slot is its index)
    uint64_t srcTransport = 0, dstTransport = 0;
    float src[3] = {0.0f, 0.0f, 0.0f}, dst[3] = {0.0f, 0.0f, 0.0f};  // server coordinates
    std::string text;     // STRING

    // Mask 0: the caster (a potion, food, the Hearthstone).
    static SpellCastTargets self() { return SpellCastTargets{}; }
    // TARGET_FLAG_UNIT (a bandage on a friend, a quest item on an NPC; the caster's own guid for a self-cast
    // that had to pick a unit - autoSelfCast).
    static SpellCastTargets unit(uint64_t guid);
    static SpellCastTargets gameObject(uint64_t guid);  // TARGET_FLAG_GAMEOBJECT (gathering, keys, quest objects)
    static SpellCastTargets corpse(uint64_t guid, bool ally);  // CORPSE_ALLY / CORPSE_ENEMY
    static SpellCastTargets itemTarget(uint64_t itemGuid);     // TARGET_FLAG_ITEM (stones, oils, enchants)
    static SpellCastTargets tradeItem(uint8_t tradeSlot);      // TARGET_FLAG_TRADE_ITEM
    static SpellCastTargets dest(float x, float y, float z, uint64_t transport = 0);  // DEST_LOCATION
    // Adders (a spell may carry several: a unit + a destination, source + destination).
    SpellCastTargets& withSource(float x, float y, float z, uint64_t transport = 0);
    SpellCastTargets& withDest(float x, float y, float z, uint64_t transport = 0);
    SpellCastTargets& withString(const std::string& s);

    // SpellCastTargets::Write.
    void write(network::Packet& p) const;
};

// What the spell asks the player to pick - Spell.dbc Targets with the explicit mask of the implicit target
// (AzerothCore GetExplicitTargetMask: e.g. 21 TARGET_UNIT_TARGET_ALLY -> UNIT_ALLY, 6 TARGET_UNIT_TARGET_ENEMY ->
// UNIT_ENEMY, 23 -> GAMEOBJECT, 16 TARGET_UNIT_DEST_AREA_ENEMY -> DEST_LOCATION). 0 = nothing to pick.
uint32_t spellTargetRequest(uint32_t spellTargets, uint32_t implicitTarget);

// The bodies after the opcode (the caller makes the packet with the realm's wire opcode).
void writeCastSpellBody(network::Packet& p, uint8_t castCount, uint32_t spellId, uint8_t castFlags,
                        const SpellCastTargets& t);
void writeUseItemBody(network::Packet& p, uint8_t bag, uint8_t slot, uint8_t castCount, uint32_t spellId,
                      uint64_t itemGuid, uint32_t glyphIndex, uint8_t castFlags, const SpellCastTargets& t);

}  // namespace wowee::platform::psp
