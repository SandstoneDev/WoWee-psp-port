// SpellCastTargets and the cast requests in the order AzerothCore reads them
// (include/platform/psp/psp_spell_targets.hpp).
#include "platform/psp/psp_spell_targets.hpp"

namespace wowee::platform::psp {

SpellCastTargets SpellCastTargets::unit(uint64_t guid) {
    SpellCastTargets t;
    t.mask = kTargetFlagUnit;
    t.object = guid;
    return t;
}

SpellCastTargets SpellCastTargets::gameObject(uint64_t guid) {
    SpellCastTargets t;
    t.mask = kTargetFlagGameObject;
    t.object = guid;
    return t;
}

SpellCastTargets SpellCastTargets::corpse(uint64_t guid, bool ally) {
    SpellCastTargets t;
    t.mask = ally ? kTargetFlagCorpseAlly : kTargetFlagCorpseEnemy;
    t.object = guid;
    return t;
}

SpellCastTargets SpellCastTargets::itemTarget(uint64_t itemGuid) {
    SpellCastTargets t;
    t.mask = kTargetFlagItem;
    t.item = itemGuid;
    return t;
}

SpellCastTargets SpellCastTargets::tradeItem(uint8_t tradeSlot) {
    SpellCastTargets t;
    t.mask = kTargetFlagTradeItem;
    t.item = tradeSlot;  // the trade slot as the "guid" (AzerothCore SpellCastTargets::Update, TRADE_SLOT_NONTRADED = 6)
    return t;
}

SpellCastTargets SpellCastTargets::dest(float x, float y, float z, uint64_t transport) {
    SpellCastTargets t;
    t.withDest(x, y, z, transport);
    return t;
}

SpellCastTargets& SpellCastTargets::withSource(float x, float y, float z, uint64_t transport) {
    mask |= kTargetFlagSourceLocation;
    srcTransport = transport;
    src[0] = x;
    src[1] = y;
    src[2] = z;
    return *this;
}

SpellCastTargets& SpellCastTargets::withDest(float x, float y, float z, uint64_t transport) {
    mask |= kTargetFlagDestLocation;
    dstTransport = transport;
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
    return *this;
}

SpellCastTargets& SpellCastTargets::withString(const std::string& s) {
    mask |= kTargetFlagString;
    text = s;
    return *this;
}

// The targets block, in the order SpellCastTargets::Read takes it.
void SpellCastTargets::write(network::Packet& p) const {
    p.writeUInt32(mask);
    if (mask & kTargetMaskObjectGuid) p.writePackedGuid(object);
    if (mask & kTargetMaskItemGuid) p.writePackedGuid(item);
    if (mask & kTargetFlagSourceLocation) {
        p.writePackedGuid(srcTransport);
        p.writeFloat(src[0]);
        p.writeFloat(src[1]);
        p.writeFloat(src[2]);
    }
    if (mask & kTargetFlagDestLocation) {
        p.writePackedGuid(dstTransport);
        p.writeFloat(dst[0]);
        p.writeFloat(dst[1]);
        p.writeFloat(dst[2]);
    }
    if (mask & kTargetFlagString) p.writeString(text);
}

// What the spell asks for: AzerothCore SpellImplicitTargetInfo::GetExplicitTargetMask for the implicit target (its
// reference / object / check types from SpellImplicitTargetInfo::_data), added to the Targets column.
uint32_t spellTargetRequest(uint32_t m, uint32_t implicit) {
    switch (implicit) {
        // TARGET_REFERENCE_TYPE_TARGET, an object: a game object, or the item / game object of a lock.
        case 23: m |= kTargetFlagGameObject; break;                   // TARGET_GAMEOBJECT_TARGET
        case 26: m |= kTargetFlagGameObjectItem; break;               // TARGET_GAMEOBJECT_ITEM_TARGET
        // TARGET_REFERENCE_TYPE_TARGET, a unit or a destination at it: by the check type.
        case 6: case 53: m |= kTargetFlagUnitEnemy; break;            // TARGET_CHECK_ENEMY
        case 21: case 45: m |= kTargetFlagUnitAlly; break;            // TARGET_CHECK_ALLY
        case 35: m |= kTargetFlagUnitParty; break;                    // TARGET_CHECK_PARTY
        case 57: m |= kTargetFlagUnitRaid; break;                     // TARGET_CHECK_RAID
        case 95: m |= kTargetFlagUnitPassenger; break;                // TARGET_CHECK_PASSENGER
        case 25: case 61: case 63: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 74:
        case 75: case 90:
            m |= kTargetFlagUnit;                                     // the default / RAID_CLASS check: any unit
            break;
        // TARGET_REFERENCE_TYPE_SRC: a source location.
        case 7: case 11: case 15: case 30: case 33: case 51: m |= kTargetFlagSourceLocation; break;
        // TARGET_REFERENCE_TYPE_DEST: a destination.
        case 8: case 16: case 28: case 29: case 31: case 34: case 52: case 78: case 79: case 80: case 81: case 82:
        case 83: case 84: case 85: case 86: case 87: case 88: case 91: case 107:
            m |= kTargetFlagDestLocation;
            break;
        case 89: m |= kTargetFlagSourceLocation | kTargetFlagDestLocation; break;  // TARGET_DEST_TRAJ
        default: break;  // the caster's own reference: nothing to pick
    }
    return m;
}

void writeCastSpellBody(network::Packet& p, uint8_t castCount, uint32_t spellId, uint8_t castFlags,
                        const SpellCastTargets& t) {
    p.writeUInt8(castCount);
    p.writeUInt32(spellId);
    p.writeUInt8(castFlags);
    t.write(p);
}

void writeUseItemBody(network::Packet& p, uint8_t bag, uint8_t slot, uint8_t castCount, uint32_t spellId,
                      uint64_t itemGuid, uint32_t glyphIndex, uint8_t castFlags, const SpellCastTargets& t) {
    p.writeUInt8(bag);
    p.writeUInt8(slot);
    p.writeUInt8(castCount);
    p.writeUInt32(spellId);
    p.writeUInt64(itemGuid);
    p.writeUInt32(glyphIndex);
    p.writeUInt8(castFlags);
    t.write(p);
}

}  // namespace wowee::platform::psp
