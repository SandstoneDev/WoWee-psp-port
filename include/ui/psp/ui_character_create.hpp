// Character creation: CharacterCreate of the original (GlueXML CharacterCreate.xml / .lua, audit #7 of
// the glue screens) on 480 x 272 - the race icons by faction, gender, the class icons (unavailable ones
// desaturated), the race / class description, the five customizations with arrows, RANDOMIZE,
// the name, ACCEPT / BACK, the model in its starting outfit on the race's scene.
//
// CharCreateArt: glue.pak cc_* (tools/psp/build_glue_pak.py char_create) - the icon atlases, the
// customization ranges (CharSections rows offered at creation), CharStartOutfit, the GlueStrings texts.
// Loaded when the creation screen opens, released when it closes (glue screens only: the world is not
// loaded, the heap is free; `char_create_art kb=` logs it).
#pragma once

#include "game/character.hpp"
#include "platform/psp/psp_texture.hpp"

#include <cstdint>

namespace wowee::ui::psp {

struct CharCreateArt {
    platform::psp::Texture races, racesRound, classes, classesGrey, gender, hl, shadow;
    platform::psp::Texture arrowL, arrowLDown, arrowLHl, arrowR, arrowRDown, arrowRHl;
    void* custom = nullptr;  // cc_custom
    uint32_t customSize = 0;
    void* outfits = nullptr;  // cc_outfits
    uint32_t outfitsSize = 0;
    void* text = nullptr;  // cc_text
    uint32_t textSize = 0;
    bool tried = false;

    // CharSections ranges of one race and sex (counts; 0 = none offered).
    struct Ranges {
        uint8_t skins = 1, faces = 1, hairStyles = 1, hairColors = 1, facials = 1;
        char hairKey[12] = "NORMAL";    // ChrRaces HairCustomization (HAIR_<key>_STYLE / _COLOR)
        char facialKey[12] = "NORMAL";  // ChrRaces FacialHairCustomization[sex] (FACIAL_HAIR_<key>)
    };

    ~CharCreateArt() { release(); }
    void load();
    void release();
    bool loaded() const { return tried; }
    // GlueStrings.lua (ruRU) by key; "" when the pak has none.
    const char* str(const char* key) const;
    // GetFlavorText: the _FEMALE text for a female character when there is one.
    const char* flavor(const char* key, bool female) const;
    Ranges ranges(game::Race race, bool female) const;
    // CharStartOutfit of race / class / sex into `c.equipment` (equipment slots, as SMSG_CHAR_ENUM sends).
    void outfit(game::Character& c) const;
};

} // namespace wowee::ui::psp
