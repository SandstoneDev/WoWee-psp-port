#pragma once
#include <cstdint>
namespace wowee::rendering::psp {
enum class SimpleSpell : uint8_t { None, Fire, Frost, Shadow, Corruption, Arcane, Nature, Holy, Physical };
inline SimpleSpell simpleSpell(uint32_t id) {
    switch (id) {
        case 3110: case 7799: case 7800: case 7801: case 7802: case 11762: case 11763: case 27267: case 47964: case 47965:
        case 133: case 143: case 145: case 3140: case 8400: case 8401: case 8402: case 10148: case 10149: case 10150:
        case 10151: case 25306: case 27070: case 38692: case 42832: case 42833: return SimpleSpell::Fire;
        case 116: case 205: case 837: case 7322: case 8406: case 8407: case 8408: case 10179: case 10180: case 10181:
        case 25304: case 27071: case 27072: case 38697: case 42841: case 42842: return SimpleSpell::Frost;
        case 686: case 695: case 705: case 1088: case 1106: case 7641: case 11659: case 11660: case 11661:
        case 25307: case 27209: case 47808: case 47809: return SimpleSpell::Shadow;
        case 172: case 6222: case 6223: case 7648: case 11671: case 11672: case 25311: case 27216:
        case 47812: case 47813: return SimpleSpell::Corruption;
        default: return SimpleSpell::None;
    }
}
// WotLK Spell.dbc effect/aura values (the same fields stored in SpellRec).
// Do not use the older upstream SpellEffect convenience enum for these values.
struct SimpleSpellStyle {
    SimpleSpell color = SimpleSpell::Arcane;
    bool damage = false, heal = false, channel = false, projectile = false, periodic = false;
};
inline SimpleSpellStyle simpleSpellStyle(uint32_t id, uint32_t school, uint32_t castMs, uint32_t attributesEx,
                                         const uint8_t effects[3], const uint16_t auras[3]) {
    SimpleSpellStyle s;
    if (school & 4u) s.color = SimpleSpell::Fire;
    else if (school & 16u) s.color = SimpleSpell::Frost;
    else if (school & 32u) s.color = SimpleSpell::Shadow;
    else if (school & 2u) s.color = SimpleSpell::Holy;
    else if (school & 8u) s.color = SimpleSpell::Nature;
    else if (school == 1u) s.color = SimpleSpell::Physical;
    for (int i=0;i<3;++i) {
        s.damage |= effects[i]==2 || effects[i]==9 || effects[i]==17 || effects[i]==31 || effects[i]==58;
        s.heal |= effects[i]==10 || effects[i]==67 || effects[i]==75 || effects[i]==136 || auras[i]==8;
        s.periodic |= auras[i]==3 || auras[i]==8 || auras[i]==53;
    }
    s.channel = (attributesEx & 0x44u) != 0;
    const auto family = simpleSpell(id);
    if (!school && family!=SimpleSpell::None) s.color=family;
    // Known missile ranks retain travel even with older/missing spell metadata.
    const bool knownMissile = family==SimpleSpell::Fire || family==SimpleSpell::Frost || family==SimpleSpell::Shadow;
    s.projectile = !s.channel && !s.heal && (knownMissile || (s.damage && castMs && s.color!=SimpleSpell::Physical));
    if (family==SimpleSpell::Corruption || (s.color==SimpleSpell::Shadow && s.periodic)) s.color=SimpleSpell::Corruption;
    return s;
}
} // namespace wowee::rendering::psp
