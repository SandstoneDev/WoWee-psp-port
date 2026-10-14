// Shared PSP UI kit: the textures and fonts from ui.pak plus the client-styled
// widgets every screen uses (backdrop panel, glue button, tooltip, cursor).
#pragma once

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_focus.hpp"
#include "ui/psp/ui_font.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>

namespace wowee::ui::psp {

// Colours are 0xAABBGGRR.
inline constexpr uint32_t kColWhite = 0xFFFFFFFFu;
inline constexpr uint32_t kColGold = 0xFF00D1FFu;   // NORMAL_FONT_COLOR (1, .82, 0)
inline constexpr uint32_t kColGrey = 0xFFB0B0B0u;
inline constexpr uint32_t kColGreen = 0xFF40FF40u;
inline constexpr uint32_t kColRed = 0xFF4040FFu;

enum UiTex {
    kTexBg, kTexLogo, kTexDlgBorder, kTexDlgBg, kTexTipBorder, kTexTipBg, kTexBtnUp, kTexBtnDown,
    kTexBtnHl, kTexCursor, kTexIconSword, kTexIconHolyBolt, kTexIconFireball, kTexIconFrostbolt,
    kTexIconHeal, kTexIconPotion, kTexIconHearth, kTexIconMount, kTexIconBorder, kTexCount
};
// kFontTiny (FRIZQT 8 px) for the dense blocks of the framed windows (PaperDoll stats: 13-unit rows).
enum UiFont { kFontSmall, kFontNormal, kFontTitle, kFontTiny, kFontCount };
// PSP button glyphs (ui.pak "pad_*", Kenney Input Prompts, CC0).
enum PadGlyph {
    kPadSquare, kPadTriangle, kPadCircle, kPadCross, kPadUp, kPadDown, kPadLeft, kPadRight, kPadDpad,
    kPadL, kPadR, kPadSelect, kPadStart, kPadStick, kPadCount
};

class UiKit {
public:
    bool load(platform::psp::Pak& pak);
    // The fonts again from another language's ui.pak, kept only when that font covers Cyrillic
    // (the port's own strings) - the art stays the running language's. False: nothing changed.
    bool reloadFonts(platform::psp::Pak& pak);
    void release();

    const platform::psp::Texture& tex(UiTex t) const { return tex_[t]; }
    bool has(UiTex t) const { return tex_[t].pixels != nullptr; }
    // A font missing from an old ui.pak falls back to the small one (font_tiny).
    const Font& font(UiFont f) const { return font_[f].texture().pixels ? font_[f] : font_[kFontSmall]; }
    uint32_t loadedBytes() const { return loadedBytes_; }
    int missing() const { return missing_; }

    // Widgets. `focus` receives the stop; returns true when focused.
    void background(UiRenderer& ui, bool dim) const;
    void panel(UiRenderer& ui, const Rect& r) const;
    bool button(UiRenderer& ui, FocusRing& focus, uint32_t id, const Rect& r, const char* label,
                bool enabled = true) const;
    // Sits 44 px above anchor.y; left-aligned to the anchor, or centred on it.
    void tooltip(UiRenderer& ui, const Rect& anchor, const char* title, const char* line,
                 bool centered = false) const;
    void cursor(UiRenderer& ui, const FocusRing& focus) const;
    // A PSP button glyph in r (square); false when ui.pak has none (the caller
    // falls back to text).
    bool glyph(UiRenderer& ui, PadGlyph g, const Rect& r, uint32_t abgr = 0xFFFFFFFFu) const;
    // The art is baked cropped to its visible box (ui.pak "pad_*", tight);
    // glyph() keeps the old look (the button fills the middle 3/4 of r, as the
    // Kenney margins did), glyphFill() fills r - for glyphs inline in text.
    bool glyphFill(UiRenderer& ui, PadGlyph g, const Rect& r, uint32_t abgr = 0xFFFFFFFFu) const;
    // PSP (owner: the icons are too small to read): L, R, SELECT and START are written as
    // text (the small font, gold, centred in r - wider than r for SELECT / START); null for the rest.
    static const char* padLabel(PadGlyph g);
    // The width a glyph takes in a line of text: px for an icon, the label's width for a label.
    float glyphWidth(PadGlyph g, float px) const;
    bool hasGlyph(PadGlyph g) const { return padLabel(g) != nullptr || pad_[g].pixels != nullptr; }

private:
    platform::psp::Texture tex_[kTexCount];
    Font font_[kFontCount];
    platform::psp::Texture pad_[kPadCount];
    uint32_t loadedBytes_ = 0;
    int missing_ = 0;
};

} // namespace wowee::ui::psp
