// Dressing a character from its equipment (EBOOT runtime; also the PC equipment preview and matrix tools).
// Clean-room implementation (cleanroom/clean/equip) of the spec equipment_dress.md section 3 (3.3-3.6); declarations
// in psp_equipment.hpp, the body rules in psp_body_rules.hpp.
#include "rendering/psp/psp_equipment.hpp"

namespace wowee::rendering::psp {

namespace {

inline int sexIndex(uint8_t sex) { return sex != 0 ? 1 : 0; }

}  // namespace

void outfitOf(const Equipment& eq, uint8_t race, uint8_t sex, ItemBank& items, body::Outfit& out,
              uint32_t helmHide[7]) {
    // Outputs start empty.
    out = body::Outfit{};
    for (int k = 0; k < kHelmetHideKinds; ++k) helmHide[k] = 0;
    if (!items.isOpen()) return;

    const int sx = sexIndex(sex);
    const Equipment shown = shownEquipment(eq);

    // Performance hint only (no effect on results).
    uint32_t ids[kEquipmentSlots];
    int n = 0;
    for (int slot = 0; slot < kEquipmentSlots; ++slot) {
        if (body::kindOfSlot(slot) >= 0 && shown.displayId[slot] != 0) ids[n++] = shown.displayId[slot];
    }
    if (n > 0) items.prefetchRecords(ids, static_cast<uint32_t>(n));

    for (int slot = 0; slot < kEquipmentSlots; ++slot) {
        const int kind = body::kindOfSlot(slot);
        const uint32_t displayId = shown.displayId[slot];
        if (kind < 0 || displayId == 0) continue;
        const ItemDisplay* rec = items.get(displayId);
        if (rec == nullptr) continue;  // the kind stays not present

        body::Item& it = out.item[kind];
        it.present = true;
        it.geosetGroup[0] = rec->geosetGroup1;
        it.geosetGroup[2] = rec->geosetGroup3;
        for (int s = 0; s < body::kSections; ++s) it.texture[s] = rec->layer[sx][s];
        const ItemBodyInfo* tail = items.body(displayId);
        if (tail != nullptr) {
            it.geosetGroup[1] = tail->geosetGroup2;
            it.flags = tail->flags;
        } else {
            it.geosetGroup[1] = 0;
            it.flags = 0;
        }

        if (kind == body::kHead) {
            if (tail != nullptr && race != 0 && items.hasBodyRules()) {
                // The helm's HelmetGeosetVisData row for the wearer's sex; no row -> all 0 (no fallback).
                const HelmetVisRow* row = items.helmetVis(tail->helmetVis[sx]);
                if (row != nullptr) {
                    for (int k = 0; k < kHelmetHideKinds; ++k) helmHide[k] = row->hide[k];
                }
            } else if (rec->hidesHair[sx] != 0) {
                helmHide[0] = 0xFFFFFFFFu;  // hair hidden for every race
            }
        }
    }

    out.noFeet = (items.raceFlags(race) & kRaceFlagNoFeet) != 0;
    out.emblem = eq.emblem;
}

void applyEquipmentGeosets(std::unordered_set<uint16_t>& geosets, const Equipment& eq, uint8_t race, uint8_t sex,
                           ItemBank& items) {
    body::Outfit outfit;
    uint32_t helmHide[kHelmetHideKinds];
    outfitOf(eq, race, sex, items, outfit, helmHide);
    body::Dress dress;
    body::buildDress(outfit, dress);
    body::helmetGeosets(geosets, helmHide, race != 0 ? race : static_cast<uint8_t>(1));
    body::outfitGeosets(geosets, outfit, dress);
}

void applyEquipmentGeosets(std::unordered_set<uint16_t>& geosets, uint16_t hairScalp, bool hasHairStyle,
                           const Equipment& eq, uint8_t sex, ItemBank& items,
                           const std::unordered_set<uint16_t>& modelGeosets) {
    (void)hairScalp;
    (void)hasHairStyle;
    (void)modelGeosets;
    applyEquipmentGeosets(geosets, eq, 0, sex, items);
}

std::vector<uint16_t> equipmentLayers(const Equipment& eq, uint8_t race, uint8_t sex, ItemBank& items) {
    body::Outfit outfit;
    uint32_t helmHide[kHelmetHideKinds];
    outfitOf(eq, race, sex, items, outfit, helmHide);
    body::Dress dress;
    body::buildDress(outfit, dress);
    std::vector<uint16_t> out;
    for (int s = 0; s < body::kSections; ++s) {
        const body::Paste p = body::pasteOf(dress, s);
        for (int i = 0; i < p.count; ++i) out.push_back(p.id[i]);
    }
    return out;
}

std::vector<uint16_t> equipmentLayers(const Equipment& eq, uint8_t sex, ItemBank& items) {
    return equipmentLayers(eq, 0, sex, items);
}

uint16_t capeTexture(const Equipment& eq, uint8_t sex, ItemBank& items) {
    const Equipment shown = shownEquipment(eq);
    const uint32_t displayId = shown.displayId[14];
    if (displayId == 0 || !items.isOpen()) return kNoItemLayer;
    const ItemDisplay* rec = items.get(displayId);
    if (rec == nullptr) return kNoItemLayer;
    return rec->capeTexture[sexIndex(sex)];
}

}  // namespace wowee::rendering::psp
