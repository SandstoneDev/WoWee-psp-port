// Body texture layers, equipment geosets and helm visibility (EBOOT runtime + converter + PC tools).
//
// Clean-room implementation (cleanroom/clean/equip, integrated 2026-10-07) from the behavioural specs:
//   body_layers.md       section 2, rules R1-R7      (kSections ... pasteOf)
//   outfit_geosets.md    sections 2, 3.1, 3.2        (outfitGeosets)
//   helmet_visibility.md sections 2, 3               (helmetGeosets)
//   equipment_dress.md   section 3.2                 (kindOfSlot)
// The items.pak side (records, "b_rules") is psp_item_format.hpp; the dress of a character from its equipment is
// psp_equipment.hpp (outfitOf, applyEquipmentGeosets, equipmentLayers).
//
// Header-only, platform-free, no file I/O, no exceptions, no RTTI. Only allocation: the std::unordered_set the
// geoset functions are given.
#pragma once

#include <cstdint>
#include <unordered_set>

#include "rendering/psp/psp_emblem_format.hpp"  // emblem::Emblem, the emblem layer slots and codes

namespace wowee::rendering::psp::body {

constexpr int kSections = 8;
constexpr int kLayers = 7;
constexpr int kKinds = 11;
constexpr uint16_t kNone = 0xFFFF;  // no texture (same value as psp_item_format.hpp kNoItemLayer)

enum Kind : int {
    kHead = 0,
    kShoulder = 1,
    kShirt = 2,
    kChest = 3,
    kBelt = 4,
    kLegs = 5,
    kFeet = 6,
    kWrists = 7,
    kHands = 8,
    kTabard = 9,
    kCape = 10,
};

// Column order of the 8 ItemDisplayInfo texture columns.
enum Section : int {
    kArmUpper = 0,
    kArmLower = 1,
    kHand = 2,
    kTorsoUpper = 3,
    kTorsoLower = 4,
    kLegUpper = 5,
    kLegLower = 6,
    kFoot = 7,
};

constexpr uint16_t kFlagDress = 0x4;        // ItemDisplayInfo Flags bit: the chest item is a dress
constexpr uint16_t kFlagGuildTabard = 0x1;  // ItemDisplayInfo Flags bit: the tabard carries the guild emblem

struct Item {
    bool present = false;
    uint16_t geosetGroup[3] = {0, 0, 0};
    uint16_t flags = 0;
    uint16_t texture[kSections] = {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
};

struct Outfit {
    Item item[kKinds];
    bool noFeet = false;
    ::wowee::rendering::psp::emblem::Emblem emblem;  // default: invalid (all five values -1)
};

struct Dress {
    uint16_t layer[kSections][kLayers] = {
        {kNone, kNone, kNone, kNone, kNone, kNone, kNone}, {kNone, kNone, kNone, kNone, kNone, kNone, kNone},
        {kNone, kNone, kNone, kNone, kNone, kNone, kNone}, {kNone, kNone, kNone, kNone, kNone, kNone, kNone},
        {kNone, kNone, kNone, kNone, kNone, kNone, kNone}, {kNone, kNone, kNone, kNone, kNone, kNone, kNone},
        {kNone, kNone, kNone, kNone, kNone, kNone, kNone}, {kNone, kNone, kNone, kNone, kNone, kNone, kNone},
    };
    bool dress = false;
    uint8_t part[kSections][kLayers] = {};  // emblem::Part of each slot

    // Bit l set when layer[section][l] != kNone (0 for a section outside 0..7).
    uint8_t mask(int section) const {
        if (section < 0 || section >= kSections) return 0;
        uint8_t m = 0;
        for (int l = 0; l < kLayers; ++l) {
            if (layer[section][l] != kNone) m = static_cast<uint8_t>(m | (1u << l));
        }
        return m;
    }
};

struct Paste {
    bool underwear = false;
    uint8_t count = 0;
    uint16_t id[kLayers] = {kNone, kNone, kNone, kNone, kNone, kNone, kNone};
    uint8_t part[kLayers] = {0, 0, 0, 0, 0, 0, 0};

    // Equal when underwear and count are equal and the first `count` entries of part and id are pairwise equal.
    bool operator==(const Paste& o) const {
        if (underwear != o.underwear || count != o.count) return false;
        const int n = count <= kLayers ? count : kLayers;
        for (int i = 0; i < n; ++i) {
            if (id[i] != o.id[i] || part[i] != o.part[i]) return false;
        }
        return true;
    }
    bool operator!=(const Paste& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------------------------------------------
// Equipment slot -> outfit kind (equipment_dress.md 3.2); -1 for none.
inline int kindOfSlot(int equipSlot) {
    switch (equipSlot) {
        case 0: return kHead;
        case 2: return kShoulder;
        case 3: return kShirt;
        case 4: return kChest;
        case 5: return kBelt;
        case 6: return kLegs;
        case 7: return kFeet;
        case 8: return kWrists;
        case 9: return kHands;
        case 14: return kCape;
        case 18: return kTabard;
        default: return -1;
    }
}

namespace layers_detail {

// R4: base slot per kind x section; -1 = the kind never paints the section.
inline constexpr int8_t kBaseSlot[kKinds][kSections] = {
    //  AU  AL   H  TU  TL  LU  LL   F
    {-1, -1, -1, -1, -1, -1, -1, -1},  // head
    {-1, -1, -1, -1, -1, -1, -1, -1},  // shoulder
    {0, 0, -1, 0, 0, -1, -1, -1},      // shirt
    {1, 1, -1, 1, 1, 1, 1, -1},        // chest
    {-1, -1, -1, -1, 5, 2, -1, -1},    // belt
    {-1, -1, -1, -1, -1, 0, 0, -1},    // legs
    {-1, -1, -1, -1, -1, -1, 2, 0},    // feet
    {-1, 2, -1, -1, -1, -1, -1, -1},   // wrists
    {-1, 3, 0, -1, -1, -1, -1, -1},    // hands
    {-1, -1, -1, 4, 4, -1, -1, -1},    // tabard
    {-1, -1, -1, -1, -1, -1, -1, -1},  // cape
};

// R3: precedence order; a later kind wins a shared (section, slot). Cape before tabard.
inline constexpr int kPrecedence[kKinds] = {kHead, kShoulder, kShirt, kChest, kBelt, kLegs,
                                            kFeet, kWrists,   kHands, kCape,  kTabard};

// R4 + R5: the slot `kind` paints in `section`, or -1.
inline int slotFor(const Outfit& o, int kind, int section) {
    const int base = kBaseSlot[kind][section];
    if (base < 0) return -1;
    if (section == kFoot && o.noFeet) return -1;
    const Item& it = o.item[kind];
    if (section == kArmLower) {
        if (kind == kChest && it.geosetGroup[0] != 0) return 5;  // long sleeve
        if (kind == kHands && it.geosetGroup[0] != 0) return 6;  // gauntlet cuff
    } else if (section == kLegLower) {
        if (kind == kChest && it.geosetGroup[2] != 0) return 4;  // robe skirt
        if (kind == kLegs && it.geosetGroup[2] != 0) {           // kilt
            const Item& chest = o.item[kChest];
            return (chest.present && chest.geosetGroup[2] != 0) ? 3 : 4;
        }
        if (kind == kFeet && it.geosetGroup[0] != 0) return 3;   // high boots
    }
    return base;
}

}  // namespace layers_detail

// R1-R6: fills every field of `d`.
inline void buildDress(const Outfit& o, Dress& d) {
    namespace em = ::wowee::rendering::psp::emblem;
    // R2
    for (int s = 0; s < kSections; ++s) {
        for (int l = 0; l < kLayers; ++l) {
            d.layer[s][l] = kNone;
            d.part[s][l] = em::kItem;
        }
    }
    // R1
    const Item& chest = o.item[kChest];
    d.dress = chest.present && (chest.flags & kFlagDress) != 0;
    // R3-R5
    for (int p = 0; p < kKinds; ++p) {
        const int kind = layers_detail::kPrecedence[p];
        const Item& it = o.item[kind];
        if (!it.present) continue;
        for (int s = 0; s < kSections; ++s) {
            const uint16_t tex = it.texture[s];
            if (tex == kNone) continue;
            const int slot = layers_detail::slotFor(o, kind, s);
            if (slot < 0) continue;
            d.layer[s][slot] = tex;
            d.part[s][slot] = em::kItem;
        }
    }
    // R6: guild emblem on the tabard
    const Item& tabard = o.item[kTabard];
    const em::Emblem& e = o.emblem;
    if (tabard.present && (tabard.flags & kFlagGuildTabard) != 0 && e.valid() && e.background >= 0 &&
        e.background < 65535 && em::codeFits(e.borderStyle, e.borderColor) && em::codeFits(e.style, e.color)) {
        const int sections[2] = {kTorsoUpper, kTorsoLower};
        for (int s : sections) {
            d.layer[s][em::kLayerBackground] = static_cast<uint16_t>(e.background);
            d.part[s][em::kLayerBackground] = em::kBackground;
            d.layer[s][em::kLayerBorder] = em::code(e.borderStyle, e.borderColor);
            d.part[s][em::kLayerBorder] = em::kBorder;
            d.layer[s][em::kLayerEmblem] = em::code(e.style, e.color);
            d.part[s][em::kLayerEmblem] = em::kEmblem;
        }
    }
}

// R7: the paste description of one section (a default Paste for a section outside 0..7).
inline Paste pasteOf(const Dress& d, int section) {
    Paste p;
    if (section < 0 || section >= kSections) return p;
    const uint16_t* l = d.layer[section];
    int lo = 0;
    int hi = 0;
    bool underwear = false;
    switch (section) {
        case kArmUpper: lo = 0; hi = 1; break;
        case kArmLower: lo = 0; hi = 6; break;
        case kHand: lo = 0; hi = 0; break;
        case kTorsoUpper:
            lo = 0;
            hi = 4;
            underwear = l[0] == kNone && l[1] == kNone && l[2] == kNone;
            break;
        case kTorsoLower: lo = 0; hi = 6; break;
        case kLegUpper:
            if (d.dress) {
                lo = 1;
                hi = 1;
            } else {
                lo = 0;
                hi = 2;
            }
            underwear = d.dress || (l[0] == kNone && l[1] == kNone);
            break;
        case kLegLower:
            if (d.dress) {
                lo = 1;
                hi = 3;
            } else {
                lo = 0;
                hi = 5;
            }
            break;
        case kFoot: lo = 0; hi = 0; break;
        default: break;
    }
    p.underwear = underwear;
    for (int i = lo; i <= hi; ++i) {
        if (l[i] == kNone) continue;
        p.id[p.count] = l[i];
        p.part[p.count] = d.part[section][i];
        ++p.count;
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// outfit_geosets.md 3.1 / 3.2

// Equipment groups E = {4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 18}.
inline bool isEquipmentGeosetGroup(unsigned group) {
    switch (group) {
        case 4: case 5: case 6: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 18:
            return true;
        default:
            return false;
    }
}

inline void outfitGeosets(std::unordered_set<uint16_t>& vis, const Outfit& o, const Dress& d) {
    // 3.1: drop every id of an equipment group; other groups stay as they are.
    for (auto it = vis.begin(); it != vis.end();) {
        if (isEquipmentGeosetGroup(*it / 100u)) {
            it = vis.erase(it);
        } else {
            ++it;
        }
    }
    auto add = [&vis](unsigned id) { vis.insert(static_cast<uint16_t>(id & 0xFFFFu)); };

    const Item& shirt = o.item[kShirt];
    const Item& chest = o.item[kChest];
    const Item& belt = o.item[kBelt];
    const Item& legs = o.item[kLegs];
    const Item& feet = o.item[kFeet];
    const Item& hands = o.item[kHands];
    const Item& tabard = o.item[kTabard];
    const Item& cape = o.item[kCape];

    // 3.2 conditions
    const bool cuff = hands.present && hands.geosetGroup[0] != 0;
    const bool chestSleeve = chest.present && chest.geosetGroup[0] != 0;
    const bool armFree = (d.mask(kArmLower) & 0x7Eu) == 0;  // none of bits 1..6
    const bool shirtSleeve = armFree && shirt.present && shirt.geosetGroup[0] != 0;
    const bool robe = chest.present && chest.geosetGroup[2] != 0;
    const bool kilt = !robe && legs.present && legs.geosetGroup[2] != 0 && !d.dress;
    const bool skirt = robe || kilt;
    const bool boots = feet.present && feet.geosetGroup[0] != 0;
    const bool kneepads = legs.present && legs.geosetGroup[1] != 0 && !d.dress;
    const bool tabardShown = !skirt && tabard.present && tabard.geosetGroup[0] != 0;
    const bool trousers = !robe && !d.dress && legs.present;
    const bool longTrousers = trousers && legs.geosetGroup[0] > 2;

    // 4 gloves
    add(cuff ? 401u + hands.geosetGroup[0] : 401u);
    // 5 boots
    if (!skirt) add(boots ? 501u + feet.geosetGroup[0] : 501u);
    // 6
    add(601u);
    // 8 sleeves
    add(801u);
    if (chestSleeve && !cuff) add(801u + chest.geosetGroup[0]);
    if (shirtSleeve) add(801u + shirt.geosetGroup[0]);
    // 9 kneepads
    add(901u);
    if (kneepads && !skirt && !boots) add(901u + legs.geosetGroup[1]);
    // 10 shirt
    add(1001u);
    if (shirt.present && shirt.geosetGroup[1] != 0 && !robe && !tabardShown) add(1001u + shirt.geosetGroup[1]);
    // 11 trousers
    if (!skirt) add(1101u);
    {
        const unsigned g0 = legs.geosetGroup[0];
        if (trousers && (g0 > 2 || ((g0 == 1 || g0 == 2) && !tabardShown))) add(1101u + g0);
    }
    // 12 tabard
    add(1201u);
    if (tabardShown) add(1201u + tabard.geosetGroup[0]);
    // 13 legs / skirt
    if (robe) {
        add(1301u + chest.geosetGroup[2]);
    } else if (longTrousers) {
        // nothing of group 13
    } else if (kilt) {
        add(1301u + legs.geosetGroup[2]);
    } else {
        add(1301u);
    }
    // 14
    add(1401u);
    // 15 cloak
    add((cape.present && cape.geosetGroup[0] != 0) ? 1501u + cape.geosetGroup[0] : 1501u);
    // 18 belt
    add((belt.present && belt.geosetGroup[0] != 0) ? 1801u + belt.geosetGroup[0] : 1801u);
}

// ---------------------------------------------------------------------------------------------------------------
// helmet_visibility.md 3

namespace helm_detail {
inline constexpr uint16_t kGroup[7] = {0, 1, 2, 3, 7, 16, 17};
inline constexpr uint16_t kReplacement[7] = {1, 101, 201, 301, 701, 1601, 1701};
}  // namespace helm_detail

inline void helmetGeosets(std::unordered_set<uint16_t>& vis, const uint32_t* hide, uint8_t race) {
    if (hide == nullptr || race >= 32) return;
    const uint32_t bit = 1u << race;
    for (int k = 0; k < 7; ++k) {
        if ((hide[k] & bit) == 0) continue;
        const unsigned group = helm_detail::kGroup[k];
        for (auto it = vis.begin(); it != vis.end();) {
            if (*it != 0 && *it / 100u == group) {
                it = vis.erase(it);
            } else {
                ++it;
            }
        }
        vis.insert(helm_detail::kReplacement[k]);
    }
}

}  // namespace wowee::rendering::psp::body
