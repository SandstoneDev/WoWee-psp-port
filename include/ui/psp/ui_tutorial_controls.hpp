// Tutorials - THE table of PSP controls lines that replace the
// keyboard / mouse sentences of the original tutorial texts (TUTORIAL<id>).
//
// One row per tutorial id; one column per control scheme (wowee.cfg
// "controls": 1 = the current scheme (docs/PSP_PORT_ARCHITECTURE.md);
// 2 = the mode scheme of docs/PSP_CONTROLS.md, the default). Edit
// only this table when the controls change.
//
//   line  - replaces the first key/mouse sentence (the others are dropped);
//           appended as its own paragraph when the text has none
//   mouse - glyphs drawn where the original shows the mouse picture
//   {name} - a PSP button glyph (UiKit::glyph, ui.pak "pad_*"): square
//           triangle circle cross up down left right dpad l r select start stick
//
// Ids with no row keep the original sentences (log tutorial_subst_missing):
// `build_tutorial_pak.py --dump` lists them marked [K] - add a row here.
// Pure data (no PSP headers): tools/psp/host_tests/tutorial_test.cpp checks
// every token. The lines are the port's own strings - ids
// here, the en / ru texts in ps_b8_port.inc (TUT_*, the same glyph tokens in both).
#pragma once

#include "platform/psp/psp_strings.hpp"  // PS (the lines' ids; declarations only)

#include <cstdint>
#include <cstring>

namespace wowee::ui::psp {

constexpr int kTutorialSchemes = 2;

using platform::psp::PS;

struct TutorialControlsRow {
    uint16_t id;
    PS line[kTutorialSchemes];  // The port's own strings (ps_b8_port.inc TUT_*) - text by ps()
    const char* mouse[kTutorialSchemes];  // nullptr = draw nothing there
};

// Ids: TUTORIAL_DATA of the 3.3.5a TutorialFrame.lua (name in the comment).
inline constexpr TutorialControlsRow kTutorialControls[] = {
    {1,  // QuestGiver
     {PS::TUT_QUESTGIVER_L, PS::TUT_QUESTGIVER},
     {"{l}{up}", "{up}"}},
    {2,  // Movement
     {PS::TUT_MOVE_1, PS::TUT_MOVE_2},
     {nullptr, nullptr}},
    {3,  // LookAround
     {PS::TUT_LOOK_1, PS::TUT_LOOK_2},
     {"{stick}", "{r}{stick}"}},
    {4,  // QuestGiver (second)
     {PS::TUT_QUESTGIVER_L, PS::TUT_QUESTGIVER},
     {"{l}{up}", "{up}"}},
    {5,  // Combat1
     {PS::TUT_COMBAT_1, PS::TUT_COMBAT_2},
     {"{l}{right}", "{square}"}},
    {6,  // Spells
     {PS::TUT_BAR_1, PS::TUT_BAR_2},
     {nullptr, nullptr}},
    {7,  // LootPig
     {PS::TUT_LOOT_L, PS::TUT_LOOT},
     {"{l}{up}", "{up}"}},
    {8,  // LootedItem
     {PS::TUT_BAGS, PS::TUT_BAGS},
     {nullptr, nullptr}},
    {9,  // OnUseItem
     {PS::TUT_USE_ITEM, PS::TUT_USE_ITEM},
     {"{cross}", "{cross}"}},
    {10,  // Bags
     {PS::TUT_BAGS, PS::TUT_BAGS},
     {nullptr, nullptr}},
    {13,  // Talents
     {PS::TUT_TALENTS, PS::TUT_TALENTS},
     {nullptr, nullptr}},
    {14,  // Trainer
     {PS::TUT_TRAINER_L, PS::TUT_TRAINER},
     {"{l}{up}", "{up}"}},
    {15,  // SpellBook
     {PS::TUT_SPELLBOOK, PS::TUT_SPELLBOOK},
     {nullptr, nullptr}},
    {17,  // Reply
     {PS::TUT_REPLY, PS::TUT_REPLY},
     {nullptr, nullptr}},
    {20,  // Vendor
     {PS::TUT_VENDOR_L, PS::TUT_VENDOR},
     {"{l}{up}", "{up}"}},
    {21,  // QuestLog
     {PS::TUT_QUESTLOG, PS::TUT_QUESTLOG},
     {nullptr, nullptr}},
    {24,  // Equip
     {PS::TUT_EQUIP, PS::TUT_EQUIP},
     {nullptr, nullptr}},
    {25,  // Death
     {PS::TUT_DEATH, PS::TUT_DEATH},
     {nullptr, nullptr}},
    {33,  // Jumping
     {PS::TUT_JUMP_1, PS::TUT_JUMP_2},
     {nullptr, nullptr}},
    {34,  // QuestComplete
     {PS::TUT_QUESTDONE_L, PS::TUT_QUESTDONE},
     {"{l}{up}", "{up}"}},
    {35,  // Travel
     {PS::TUT_FLIGHT_L, PS::TUT_FLIGHT},
     {"{l}{up}", "{up}"}},
    {36,  // DamagedItems
     {PS::TUT_REPAIR_L, PS::TUT_REPAIR},
     {"{l}{up}", "{up}"}},
    {37,  // BrokenItems
     {PS::TUT_REPAIR_L, PS::TUT_REPAIR},
     {"{l}{up}", "{up}"}},
    {38,  // Professions
     {PS::TUT_PROF_TRAINER_L, PS::TUT_PROF_TRAINER},
     {"{l}{up}", "{up}"}},
    {40,  // Spellbook2
     {PS::TUT_SPELLBOOK2, PS::TUT_SPELLBOOK2},
     {nullptr, nullptr}},
    {53,  // Mounts
     {PS::TUT_MOUNT_1, PS::TUT_MOUNT_2},
     {nullptr, nullptr}},
    {58,  // FullBags
     {PS::TUT_FULLBAGS, PS::TUT_FULLBAGS},
     {nullptr, nullptr}},
    {59,  // FreeingUpBags
     {PS::TUT_BAGS, PS::TUT_BAGS},
     {"{cross}", "{cross}"}},
    {60,  // Hotbar
     {PS::TUT_BAR_1, PS::TUT_BAR_2},
     {nullptr, nullptr}},
};

// The Movement layout's key pictures (keyDataN.command) get a PSP glyph
// instead of the bound key's letter: TURNLEFT, MOVEBACKWARD, TURNRIGHT,
// MOVEFORWARD, JUMP (TutorialKeyCommand order); nullptr = hide the key.
inline constexpr const char* kTutorialKeyGlyph[kTutorialSchemes][5] = {
    {"{square}", "{cross}", "{circle}", "{triangle}", "{start}"},
    {nullptr, nullptr, nullptr, "{stick}", "{cross}"},
};

// Glyph token names in PadGlyph order (ui_kit.hpp).
inline constexpr const char* kTutorialGlyphNames[] = {"square", "triangle", "circle", "cross", "up", "down", "left",
                                                      "right", "dpad", "l", "r", "select", "start", "stick"};
constexpr int kTutorialGlyphCount = sizeof(kTutorialGlyphNames) / sizeof(kTutorialGlyphNames[0]);

inline const TutorialControlsRow* tutorialControls(uint32_t id) {
    for (const auto& r : kTutorialControls)
        if (r.id == id) return &r;
    return nullptr;
}

// "{name}" at s (s points at '{'): the glyph index, -1 when not a token;
// *len = the token's length.
inline int tutorialGlyphToken(const char* s, int* len) {
    if (!s || *s != '{') return -1;
    const char* e = std::strchr(s, '}');
    if (!e) return -1;
    const size_t n = static_cast<size_t>(e - s - 1);
    for (int g = 0; g < kTutorialGlyphCount; ++g)
        if (std::strlen(kTutorialGlyphNames[g]) == n && std::strncmp(s + 1, kTutorialGlyphNames[g], n) == 0) {
            if (len) *len = static_cast<int>(n + 2);
            return g;
        }
    return -1;
}

} // namespace wowee::ui::psp
