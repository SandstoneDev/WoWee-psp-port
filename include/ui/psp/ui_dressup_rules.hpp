// Dressing room: the dressing room's rules without the PSP - what TryOn does to the equipment
// (src/ui/psp/ui_world_dressup.cpp) and where a chat line's item links stand once it is wrapped
// (src/ui/psp/ui_world_chat.cpp). Header-only: tools/psp/host_tests/dressup_rules_test.cpp checks them on the PC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::ui::psp::dressup {

// The equipment slot (game::EquipSlot order, the 19 visible ones) an item of this InventoryType is worn in;
// -1: none (not equippable, a bag, ammo, a quiver).
inline int slotOfInventoryType(uint8_t t) {
    switch (t) {
        case 1: return 0;           // HEAD
        case 2: return 1;           // NECK
        case 3: return 2;           // SHOULDERS
        case 4: return 3;           // BODY (shirt)
        case 5: case 20: return 4;  // CHEST, ROBE
        case 6: return 5;           // WAIST
        case 7: return 6;           // LEGS
        case 8: return 7;           // FEET
        case 9: return 8;           // WRISTS
        case 10: return 9;          // HANDS
        case 11: return 10;         // FINGER (FINGER1)
        case 12: return 12;         // TRINKET (TRINKET1)
        case 16: return 14;         // CLOAK
        case 19: return 18;         // TABARD
        case 13: case 17: case 21: return 15;           // WEAPON, 2HWEAPON, WEAPONMAINHAND
        case 14: case 22: case 23: return 16;           // SHIELD, WEAPONOFFHAND, HOLDABLE
        case 15: case 25: case 26: case 28: return 17;  // RANGED, THROWN, RANGEDRIGHT, RELIC
        default: return -1;
    }
}

// DressUpModel:TryOn - the item stands in for what is worn in its slot (upstream __WoweeTryOn replaces what has the
// same InventoryType; the slot catches a robe over a chest piece too). The hands: a two-hander empties the off hand,
// an off-hand item empties a two-handed main hand, a ranged weapon (not a relic) empties both so the model holds it
// (PSP: the own player's attachment rules show a ranged weapon only with the main hand empty). The arrays are the 19
// visible slots (rendering::psp::Equipment). The slot used, -1 when the item has none (nothing changes).
inline int tryOn(uint32_t (&displayId)[19], uint8_t (&invType)[19], uint32_t display, uint8_t type) {
    const int slot = slotOfInventoryType(type);
    if (slot < 0 || display == 0) return -1;
    auto clear = [&](int s) {
        displayId[s] = 0;
        invType[s] = 0;
    };
    displayId[slot] = display;
    invType[slot] = type;
    if (type == 17) clear(16);
    if (slot == 16 && invType[15] == 17) clear(15);
    if (slot == 17 && type != 28) {
        clear(15);
        clear(16);
    }
    return slot;
}

// Where the item links of a chat line stand after it was wrapped: `text` = the line as written, `lines` = its wrapped
// pieces (in order; a break may drop one space), `shown` = each link's text as the line shows it ("[name]") in the
// message's order, `skip` = bytes of `text` before the message (the "[Name] says: " head - a name can read like a
// link). A link is looked for from where the last one ended; one not in this line is looked for in the next; a link
// found after it means it was cut by the wrap (its halves are no link any more) and it is dropped.
struct LinkPlace {
    size_t link = 0;    // index into `shown`
    size_t line = 0;    // index into `lines`
    size_t offset = 0;  // byte offset in that line
};
inline std::vector<LinkPlace> placeLinks(const std::string& text, const std::vector<std::string>& lines,
                                         const std::vector<std::string>& shown, size_t skip = 0) {
    std::vector<LinkPlace> out;
    size_t next = 0;  // the first link not placed yet
    size_t pos = 0;   // where the line starts in `text`
    for (size_t i = 0; i < lines.size() && next < shown.size(); ++i) {
        const size_t at0 = text.find(lines[i], pos);  // at pos, or after the space the break dropped
        const size_t start = at0 == std::string::npos ? pos : at0;
        pos = start + lines[i].size();
        size_t from = skip > start ? skip - start : 0;
        if (from > lines[i].size()) from = lines[i].size();
        for (size_t k = next; k < shown.size(); ++k) {
            if (shown[k].empty()) continue;
            const size_t at = lines[i].find(shown[k], from);
            if (at == std::string::npos) continue;
            out.push_back(LinkPlace{k, i, at});
            from = at + shown[k].size();
            next = k + 1;
        }
    }
    return out;
}

}  // namespace wowee::ui::psp::dressup
