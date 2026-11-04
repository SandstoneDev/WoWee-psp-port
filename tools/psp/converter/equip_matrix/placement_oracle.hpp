// Placement oracle of the PC comparison tool (equip matrix): clean-room implementation (cleanroom/clean/equip) of the
// spec weapon_placement.md section 3.5 - an independent statement of the placement table for a single worn weapon.
// PC tool code; header-only.
//
// Known, required difference from the runtime (reviewer decision in the spec): inventory type 28 (relic) in the
// ranged state - runtime left hand (2), oracle right hand (1) unless the item is a bow (class 2, subclass 2).
#pragma once

#include <cstdint>

#include "rendering/psp/psp_item_format.hpp"

namespace wowee::pspconv {

inline uint16_t expectedSheathPoint(int slot, uint8_t invType, uint8_t sheatheType, uint32_t itemClass,
                                    uint32_t itemSubClass, uint8_t state) {
    using ::wowee::rendering::psp::kNoSheathPoint;
    using ::wowee::rendering::psp::sheathPointOf;
    if (slot == 15 || slot == 16) {
        if (state == 1) {
            if (slot == 15) return 1;
            return invType == 14 ? 0 : 2;
        }
        if (state == 0) return sheathPointOf(slot == 15 ? 0 : 1, sheatheType);
        return kNoSheathPoint;
    }
    if (slot == 17) {
        if (state == 2) return (itemClass == 2 && itemSubClass == 2) ? 2 : 1;
        return kNoSheathPoint;
    }
    return kNoSheathPoint;
}

}  // namespace wowee::pspconv
