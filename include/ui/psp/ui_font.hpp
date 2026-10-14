// Bitmap font baked offline from the game's own TTF (FRIZQT__ & co.) by
// tools/psp/build_ui_pak.py: ASCII + Cyrillic in a T4 alpha-ramp atlas, so
// the vertex colour tints it.
//
// Blob "WFNT" (little-endian, 32-byte header):
//   0 "WFNT"  4 u16 version(1), u16 glyphCount   8 i16 lineHeight, i16 ascent
//  12 u16 pixelSize, u16 reserved  16 u32 glyphOffset  20 u32 texOffset
//  24 u32 texSize  28 u32 reserved
//   glyph (16 B): u32 codepoint, u16 x, u16 y, u8 w, u8 h, i8 bearingX,
//                 i8 top (from line top), u8 advance, u8 pad[3]
// Glyphs are sorted by codepoint.
#pragma once

#include "platform/psp/psp_texture.hpp"

#include <cstdint>

namespace wowee::ui::psp {

struct Glyph {
    uint32_t codepoint;
    uint16_t x, y;
    uint8_t w, h;
    int8_t bearingX;
    int8_t top;
    uint8_t advance;
    uint8_t pad[3];
};
static_assert(sizeof(Glyph) == 16, "Glyph is a 16-byte record");

class Font {
public:
    bool load(void* blob, uint32_t size);  // takes ownership of blob
    void release();

    const Glyph* find(uint32_t cp) const;  // falls back to '?'
    // Every letter of the UTF-8 string has its own glyph (no '?' stand-in, no .notdef box) - the
    // language selector's names (a font baked for ASCII + Cyrillic has no "ç" or Hangul).
    bool hasGlyphs(const char* utf8) const;
    // The port's own strings are Russian - false for a font baked from a TTF without Cyrillic
    // (the enUS client's FRIZQT__.TTF: build_ui_pak.py's CHARSET letters all came out as the .notdef box).
    bool coversCyrillic() const { return hasGlyphs("ЖжЯяЩщ"); }
    int lineHeight() const { return lineHeight_; }
    int ascent() const { return ascent_; }
    int pixelSize() const { return pixelSize_; }
    const platform::psp::Texture& texture() const { return tex_; }

    // Width in pixels of a UTF-8 string (single line).
    int measure(const char* utf8) const;

private:
    platform::psp::Texture tex_;
    const Glyph* glyphs_ = nullptr;
    uint32_t count_ = 0;
    int lineHeight_ = 0;
    int ascent_ = 0;
    int pixelSize_ = 0;
    void* block_ = nullptr;
    // The .notdef box's metrics (w, h, bearing, top, advance) when many letters share them - the TTF had
    // no glyph for those (the baker draws the box for every CHARSET letter the font lacks).
    bool tofu_ = false;
    Glyph tofuGlyph_{};
    bool isTofu(const Glyph& g) const;
};

// Decodes one UTF-8 codepoint and advances *s. Invalid bytes yield U+FFFD.
uint32_t utf8Next(const char** s);

} // namespace wowee::ui::psp
