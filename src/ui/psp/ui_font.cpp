#include "ui/psp/ui_font.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {
template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
} // namespace

uint32_t utf8Next(const char** s) {
    auto* p = reinterpret_cast<const uint8_t*>(*s);
    uint32_t c = *p;
    int extra = 0;
    if (c < 0x80) {
        extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
        c &= 0x1F; extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F; extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07; extra = 3;
    } else {
        *s += 1;
        return 0xFFFD;
    }
    ++p;
    for (int i = 0; i < extra; ++i, ++p) {
        if ((*p & 0xC0) != 0x80) {
            *s = reinterpret_cast<const char*>(p);
            return 0xFFFD;
        }
        c = (c << 6) | (*p & 0x3F);
    }
    *s = reinterpret_cast<const char*>(p);
    return c;
}

bool Font::load(void* blob, uint32_t size) {
    release();
    auto* b = static_cast<uint8_t*>(blob);
    if (!b || size < 32 || std::memcmp(b, "WFNT", 4) != 0 || rd<uint16_t>(b + 4) != 1) {
        PSP_LOGE(pp::tag::Gfx, "wfnt_bad size=%u", static_cast<unsigned>(size));
        pp::pakFree(blob);
        return false;
    }
    count_ = rd<uint16_t>(b + 6);
    lineHeight_ = rd<int16_t>(b + 8);
    ascent_ = rd<int16_t>(b + 10);
    pixelSize_ = rd<uint16_t>(b + 12);
    uint32_t glyphOff = rd<uint32_t>(b + 16);
    uint32_t texOff = rd<uint32_t>(b + 20);
    uint32_t texSize = rd<uint32_t>(b + 24);
    if (glyphOff + count_ * sizeof(Glyph) > size || texOff % 64 || texOff + texSize > size ||
        !pp::textureFromBlob(b + texOff, texSize, tex_, false)) {
        PSP_LOGE(pp::tag::Gfx, "wfnt_layout_bad glyphs=%u", static_cast<unsigned>(count_));
        pp::pakFree(blob);
        count_ = 0;
        return false;
    }
    glyphs_ = reinterpret_cast<const Glyph*>(b + glyphOff);
    block_ = blob;
    // The .notdef box - the metrics a quarter of the glyphs share (a TTF without Cyrillic gave all 70-odd
    // Cyrillic letters of the baker's CHARSET the same box; real letters never share all five values that often).
    tofu_ = false;
    for (uint32_t i = 0; i < count_ && !tofu_; ++i) {
        const Glyph& g = glyphs_[i];
        if (g.w == 0 || g.h == 0) continue;
        uint32_t same = 0;
        for (uint32_t k = 0; k < count_; ++k) {
            const Glyph& o = glyphs_[k];
            same += o.w == g.w && o.h == g.h && o.bearingX == g.bearingX && o.top == g.top && o.advance == g.advance;
        }
        if (same >= 24 && same * 4 >= count_) {
            tofu_ = true;
            tofuGlyph_ = g;
        }
    }
    return true;
}

bool Font::isTofu(const Glyph& g) const {
    return tofu_ && g.w == tofuGlyph_.w && g.h == tofuGlyph_.h && g.bearingX == tofuGlyph_.bearingX &&
           g.top == tofuGlyph_.top && g.advance == tofuGlyph_.advance;
}

void Font::release() {
    if (block_) pp::pakFree(block_);
    block_ = nullptr;
    glyphs_ = nullptr;
    count_ = 0;
    tofu_ = false;
    tex_ = pp::Texture{};
}

const Glyph* Font::find(uint32_t cp) const {
    uint32_t lo = 0, hi = count_;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (glyphs_[mid].codepoint < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo < count_ && glyphs_[lo].codepoint == cp) return &glyphs_[lo];
    return cp == '?' ? nullptr : find('?');
}

bool Font::hasGlyphs(const char* utf8) const {
    if (!glyphs_) return false;
    while (utf8 && *utf8) {
        const uint32_t cp = utf8Next(&utf8);
        if (cp == ' ') continue;
        const Glyph* g = find(cp);
        if (!g || g->codepoint != cp || isTofu(*g)) return false;  // the '?' stand-in or the .notdef box
    }
    return true;
}

int Font::measure(const char* utf8) const {
    int w = 0;
    while (utf8 && *utf8) {
        const Glyph* g = find(utf8Next(&utf8));
        if (g) w += g->advance;
    }
    return w;
}

} // namespace wowee::ui::psp
