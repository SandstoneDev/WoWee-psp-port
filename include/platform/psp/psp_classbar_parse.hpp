// Class bars: the packets and the small rules of the class bars and the vehicles - header-only over upstream's
// network::Packet, so tools/psp/host_tests/classbars_test.cpp checks them against bodies written the way AzerothCore
// 3.3.5 writes them. psp_world_session_classbars.cpp uses them.
//   SMSG_TOTEM_CREATED: u8 slot, u64 guid, u32 duration (ms), u32 spell (AzerothCore TotemPackets.cpp
//     TotemCreated::Write).
//   CMSG_TOTEM_DESTROYED (DestroyTotem): u8 slot (AzerothCore HandleTotemDestroyed).
//   SMSG_PLAYER_VEHICLE_DATA: packed guid, u32 vehicle id - a unit gets / loses its vehicle kit, 0 = removed
//     (AzerothCore Unit::Mount / Unit::Dismount, AuraEffect::HandleAuraSetVehicle send it to everyone near) - not
//     "the player entered a vehicle".
//   SMSG_CLIENT_CONTROL_UPDATE: packed guid, u8 allow (AzerothCore Player::SetClientControl).
//   CMSG_REQUEST_VEHICLE_EXIT / _PREV_SEAT / _NEXT_SEAT: no body.
//   CMSG_REQUEST_VEHICLE_SWITCH_SEAT: the vehicle's packed guid, int8 seat (AzerothCore
//     HandleChangeSeatsOnControlledVehicle).
//   CMSG_PLAYER_VEHICLE_ENTER, CMSG_CONTROLLER_EJECT_PASSENGER, CMSG_SPELLCLICK: u64.
#pragma once

#include "network/packet.hpp"

#include <cstdint>

namespace wowee::platform::psp::cbar {

// --- Totems ---------------------------------------------------------------------------------------------------------

// MAX_TOTEMS and the slots of Constants.lua (FIRE_TOTEM_SLOT 1, EARTH 2, WATER 3, AIR 4) - 0-based here.
constexpr int kTotemSlots = 4;
constexpr int kTotemFire = 0, kTotemEarth = 1, kTotemWater = 2, kTotemAir = 3;
// TOTEM_PRIORITIES: the order TotemFrame and MultiCastActionBarFrame lay the slots out.
constexpr int kTotemPriority[kTotemSlots] = {kTotemEarth, kTotemFire, kTotemWater, kTotemAir};

struct TotemCreated {
    uint8_t slot = 0;
    uint64_t guid = 0;
    uint32_t durationMs = 0, spellId = 0;
};
inline bool readTotemCreated(network::Packet& p, TotemCreated& out) {
    out = TotemCreated{};
    if (!p.hasRemaining(1 + 8 + 4 + 4)) return false;
    out.slot = p.readUInt8();
    out.guid = p.readUInt64();
    out.durationMs = p.readUInt32();
    out.spellId = p.readUInt32();
    return true;
}
inline void writeTotemDestroyed(network::Packet& p, uint8_t slot) { p.writeUInt8(slot); }

// TotemCategory -> the totem slots it fills, by the categories' names in TotemCategory.dbc (2 earth, 3 air, 4 fire,
// 5 water; 21 the four elements) and the slot numbers of Constants.lua; bit k = 0-based slot k.
inline uint8_t totemCategorySlots(uint32_t category) {
    switch (category) {
        case 2: return 1u << kTotemEarth;
        case 3: return 1u << kTotemAir;
        case 4: return 1u << kTotemFire;
        case 5: return 1u << kTotemWater;
        case 21: return 15;
        default: return 0;
    }
}
// MultiCastActionButton actions (actionbutton.lua ActionButton_CalculateAction: page NUM_ACTIONBAR_PAGES 6 +
// GetMultiCastBarOffset()). The offset is 6, page 12 = actions 133..144: the one page left inside AzerothCore's
// MAX_ACTION_BUTTONS 144 after the six bar pages and the bonus pages 7..11 (GetBonusBarOffset 1..5). 0-based:
// 132..143, 3 pages (the calls) of 4 (the totem slots, MultiCastActionBarFrame_Update: pageOffset + slot).
constexpr int kMultiCastBarOffset = 6;
constexpr int kMultiCastFirstAction = 132;
constexpr int kMultiCastActions = 12;
// The 0-based totem slot of a multicast action, -1 = not a multicast action.
inline int multiCastActionSlot(int action) {
    if (action < kMultiCastFirstAction || action >= kMultiCastFirstAction + kMultiCastActions) return -1;
    return (action - kMultiCastFirstAction) & 3;
}
// The action of a page (0..2) and a 0-based totem slot.
inline int multiCastAction(int page, int slot) { return kMultiCastFirstAction + page * 4 + slot; }
// TOTEM_MULTI_CAST_SUMMON_SPELLS / _RECALL_SPELLS (Constants.lua).
constexpr uint32_t kCallSpells[3] = {66842, 66843, 66844};  // Call of the Elements / Ancestors / Spirits
constexpr uint32_t kRecallSpell = 36936;                     // Totemic Recall
constexpr uint32_t kAttrEx7SummonPlayerTotem = 0x20;         // SPELL_ATTR7_CAN_BE_MULTI_CAST (AzerothCore)

// --- Vehicles --------------------------------------------------------------------------------------------------------

inline bool readPlayerVehicleData(network::Packet& p, uint64_t& guid, uint32_t& vehicleId) {
    guid = 0;
    vehicleId = 0;
    if (!p.hasFullPackedGuid()) return false;
    guid = p.readPackedGuid();
    if (!p.hasRemaining(4)) return false;
    vehicleId = p.readUInt32();
    return true;
}
inline bool readClientControl(network::Packet& p, uint64_t& guid, uint8_t& allow) {
    guid = 0;
    allow = 0;
    if (!p.hasFullPackedGuid()) return false;
    guid = p.readPackedGuid();
    if (!p.hasRemaining(1)) return false;
    allow = p.readUInt8();
    return true;
}
inline void writeSwitchSeat(network::Packet& p, uint64_t vehicle, int8_t seat) {
    p.writePackedGuid(vehicle);
    p.writeUInt8(static_cast<uint8_t>(seat));
}
inline void writeGuid64(network::Packet& p, uint64_t guid) { p.writeUInt64(guid); }

// VehicleSeat.dbc Flags as the Lua getters test them (AzerothCore DBCEnums.h VehicleSeatFlags).
constexpr uint32_t kSeatHidePassenger = 0x00000200;   // the passenger is not drawn
constexpr uint32_t kSeatAllowTurning = 0x00000400;
constexpr uint32_t kSeatCanControl = 0x00000800;      // UnitInVehicleControlSeat
constexpr uint32_t kSeatUsingVehicleControls = 0x00800000;  // IsUsingVehicleControls
constexpr uint32_t kSeatCanEnterOrExit = 0x02000000;  // CanExitVehicle, VehicleExit
constexpr uint32_t kSeatCanSwitch = 0x04000000;       // CanSwitchVehicleSeats / VehicleNextSeat / UnitSwitchToVehicleSeat
constexpr uint32_t kSeatHasVehicleUi = 0x20000000;    // UnitHasVehicleUI
// Vehicle.dbc Flags (AzerothCore VehicleDefines.h VehicleFlags).
constexpr uint32_t kVehicleAimAngle = 0x400;  // IsVehicleAimAngleAdjustable
constexpr uint32_t kVehicleAimPower = 0x800;  // IsVehicleAimPowerAdjustable
constexpr uint32_t kVehicleNoJump = 0x2;      // VEHICLE_FLAG_NO_JUMPING
// UnitVehicleSkin - the two skins of VehicleMenuBar.lua's SkinsData. Own rule: a vehicle whose creature type is
// Mechanical (AzerothCore CREATURE_TYPE_MECHANICAL 9) or not known yet gets "Mechanical" (VehicleMenuBar's own
// fallback), every other creature type "Natural".
constexpr uint32_t kCreatureTypeMechanical = 9;
inline bool vehicleSkinNatural(uint32_t creatureType) {
    return creatureType != 0 && creatureType != kCreatureTypeMechanical;
}
inline const char* vehicleSkinName(uint32_t creatureType) {
    return vehicleSkinNatural(creatureType) ? "Natural" : "Mechanical";
}

// --- The bonus bar -----------------------------------------------------------------------------------------------------

// GetBonusBarOffset: 5 while the override bar has a source, else the form's SpellShapeshiftForm.BonusActionBar (0
// without a form; upstream GameHandler::getBonusActionBarOffset). Offset 5 = page NUM_ACTIONBAR_PAGES + 5 = 11 =
// actions 121..132 (actionbutton.lua; VehicleMenuBar's buttons are alwaysBonus). Own rule for the sources, the first
// present fills the bar: the unit we move (a possessed unit, a vehicle we drive) when SMSG_PET_SPELLS is about it - its
// actions; the override spells of PLAYER_FIELD_BYTES2 (AzerothCore HandleAuraOverrideSpells) - OverrideSpellData's
// spells; the form's preset spells (SpellShapeshiftForm.PresetSpellID).
constexpr uint32_t kOverridePet = 1, kOverrideSpells = 2, kOverrideFormPreset = 4;
inline int bonusBarOffset(uint32_t overrideBits, int formBonus) { return overrideBits ? 5 : formBonus; }
// The bar the override fills: action 121..132 (1-based), slots 120 + 0..11.
constexpr int kOverrideFirstSlot = 120;
constexpr int kOverrideSlots = 12;

// --- The shapeshift bar ------------------------------------------------------------------------------------------------

// A known spell is on the shapeshift bar when it applies SPELL_AURA_MOD_SHAPESHIFT (36) or carries
// SPELL_ATTR2_USE_SHAPESHIFT_BAR (AttributesEx2 0x10, "show in stance bar"), unless it carries
// SPELL_ATTR2_NO_SHAPESHIFT_UI (0x2) - the attribute names and comments of AzerothCore's SharedDefines.h. The PSP bakes
// the list (text.pak "class_bars"); this is the rule it is baked by.
inline bool isStanceBarSpell(uint32_t attrEx2, const uint16_t (&aura)[3]) {
    if (attrEx2 & 0x2u) return false;
    if (attrEx2 & 0x10u) return true;
    return aura[0] == 36 || aura[1] == 36 || aura[2] == 36;
}
// Own rule for the bar's order: Spell.dbc StanceBarOrder ascending (AzerothCore DBCStructure.h m_stanceBarOrder);
// a spell without one (negative) after those; equal orders by spell id.
inline bool stanceBefore(uint32_t spellA, int32_t orderA, uint32_t spellB, int32_t orderB) {
    const int64_t ka = orderA < 0 ? INT64_MAX : orderA, kb = orderB < 0 ? INT64_MAX : orderB;
    return ka != kb ? ka < kb : spellA < spellB;
}

// --- Possession --------------------------------------------------------------------------------------------

// The aura types that make an own aura the possession (upstream lua_system_api.cpp possessAuraSpellId, AzerothCore
// SpellAuraDefines): MOD_POSSESS 2, MOD_CHARM 6, MOD_POSSESS_PET 128, AOE_CHARM 177.
inline bool isPossessAura(uint16_t aura) { return aura == 2 || aura == 6 || aura == 128 || aura == 177; }
// The cancel button is usable unless the spell has SPELL_ATTR0_NO_AURA_CANCEL (AzerothCore HandleCancelAuraOpcode).
constexpr uint32_t kAttr0NoAuraCancel = 0x80000000u;

}  // namespace wowee::platform::psp::cbar
