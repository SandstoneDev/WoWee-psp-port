// Shared pieces of the glue screens (GlueXML of the original 3.3.5a ruRU client):
// AccountLogin, RealmList, CharacterSelect, GlueDialog.
//
//   - GLUE_STR(KEY): GlueStrings.lua of the running language from strings.pak (psp_strings.hpp);
//   - GlueArt: the screens' art from data/glue.pak (tools/psp/build_glue_pak.py) and a few
//     ui.pak entries other windows also use, loaded when a glue screen shows and released before
//     the world loads (PSP: memory) - every texture is optional (plain fills without it);
//   - glueButton(): the kit's glue button with a chosen font (GlueButtonSmallTemplate uses the
//     small font, the PSP labels must fit);
//   - GlueDialog: the modal message box (GlueDialog.xml: UI-DialogBox backdrop, GlueFontNormalLarge
//     text, up to two GlueDialogButtonTemplate buttons, the alert icon);
//   - the AuthHandler / WorldSession failure texts mapped to the client's own strings.
#pragma once

#include "game/character.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>
#include <string>

namespace wowee::ui::psp {

// GlueStrings.lua of the running language (strings.pak, GLUE_<KEY>; the keys:
// strings/cs_glue.inc).
#define GLUE_STR(key) CLIENT_STR(GLUE_##key)

// Glue font colours (GlueFontStyles.xml), 0xAABBGGRR.
inline constexpr uint32_t kGlueGold = 0xFF00C7FFu;     // GlueFontNormal (1, 0.78, 0)
inline constexpr uint32_t kGlueDisable = 0xFF808080u;  // GlueFontDisable (0.5)
inline constexpr uint32_t kGlueGreen = 0xFF1AFF1Au;    // GlueFontGreen / GREEN_FONT_COLOR (0.1, 1, 0.1)
inline constexpr uint32_t kGlueRed = 0xFF1A1AFFu;      // GlueFontRed / RED_FONT_COLOR (1, 0.1, 0.1)
inline constexpr uint32_t kGlueBlue = 0xFFFFBF00u;     // BLUE_FONT_COLOR (0, 0.749, 1)
inline constexpr uint32_t kGlueRealmDown = 0xFF808080u;

// The art of the glue screens. load() is cheap to call every frame (once per screen).
struct GlueArt {
    platform::psp::Texture csHl, rotL, rotR, alert, colTab, rlHl, rlFrame;
    platform::psp::Texture scrUp, scrDn, scrUpDis, scrDnDis;  // ui.pak (NPC frames' scroll arrows)
    // The blue Glue-Panel-Button-*-Blue (CharacterSelect_DeathKnightSwap), glue.pak gl_btn_*_b.
    platform::psp::Texture btnUpB, btnDownB, btnHlB;
    void* zones = nullptr;  // glue.pak "zones" (raw blob, see build_glue_pak.py)
    uint32_t zonesSize = 0;
    bool tried = false;
    ~GlueArt() { release(); }
    void load();
    void release();
    // AreaTable name (ruRU) of a zone id from SMSG_CHAR_ENUM; "" when unknown.
    const char* zoneName(uint32_t areaId) const;
};

// The death knight swap: CharacterSelect_DeathKnightSwap - while a death knight is selected the select
// screen's buttons take the blue Glue-Panel-Button art. glueButton() uses `art`'s blue faces until
// it is called again with null (the screen sets it around the buttons that swap).
void glueButtonsBlue(const GlueArt* art);

// The kit's glue button (Glue-Panel-Button art) with the label in `font`.
bool glueButton(UiRenderer& ui, const UiKit& kit, FocusRing& focus, uint32_t id, const Rect& r, const char* label,
                bool enabled = true, UiFont font = kFontNormal);

// Centred lines broken at spaces (and at '\n') to fit maxW; returns the y under the text.
float glueWrapped(UiRenderer& ui, const Font& f, float cx, float y, float maxW, const std::string& text, uint32_t col,
                  float lineH, bool draw = true);

// GlueDialog: one modal message. Buttons use ids kGlueDlgButton1 / 2 (the screen checks
// FocusRing::activated() against them); while shown the screen must not act on anything else.
inline constexpr uint32_t kGlueDlgButton1 = 0x7F01;
inline constexpr uint32_t kGlueDlgButton2 = 0x7F02;
class GlueDialog {
public:
    void show(std::string text, const char* button1, const char* button2 = nullptr, bool alert = false);
    // The language list's look for the notice after it - the screen dimmed as behind RealmList and a black
    // ground under the translucent backdrop (nothing shows through); hide() turns it off again.
    void setOpaque(bool on) { opaque_ = on; }
    void hide() {
        shown_ = false;
        opaque_ = false;
    }
    bool shown() const { return shown_; }
    const std::string& text() const { return text_; }
    // Declares the buttons (focus on button 1 when it first shows); returns true if drawn.
    void draw(UiRenderer& ui, const UiKit& kit, FocusRing& focus, const GlueArt* art);
    // A button that may act this frame: the id if it is one of the dialog's, else 0.
    uint32_t pressed(uint32_t activated) const;

private:
    std::string text_;
    const char* b1_ = nullptr;
    const char* b2_ = nullptr;
    bool alert_ = false;
    bool opaque_ = false;
    bool shown_ = false;
    bool focusSet_ = false;
};

// ChrClasses.dbc name / name_female of the running language (strings.pak DBC_CLASS_<id>[_FEMALE]), as
// GetCharacterInfo returns the class.
const char* className(game::Class cls, bool female);
// AuthHandler failure (upstream English) -> the AccountLogin dialog text of the original (GlueStrings LOGIN_*).
std::string loginError(const std::string& upstreamReason);
// Release + version line of AccountLoginVersion (VERSION_TEMPLATE), two lines.
void glueVersionText(UiRenderer& ui, const UiKit& kit, int build);
// The expansion logo at AccountLoginLogo / CharacterSelectLogo (TOPLEFT 3,-7, 256 x 128 units).
void glueLogo(UiRenderer& ui, const UiKit& kit);

} // namespace wowee::ui::psp
