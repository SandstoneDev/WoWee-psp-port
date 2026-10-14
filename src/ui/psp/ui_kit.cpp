#include "ui/psp/ui_kit.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {
const char* const kTexNames[] = {
    "bg_login", "logo_wotlk", "dlg_border", "dlg_bg", "tip_border", "tip_bg", "btn_up", "btn_down",
    "btn_hl", "cursor_point", "icon_sword", "icon_holybolt", "icon_fireball", "icon_frostbolt",
    "icon_heal", "icon_potion", "icon_hearth", "icon_mount", "icon_border",
};
const char* const kFontNames[] = {"font_small", "font_normal", "font_title", "font_tiny"};
const char* const kPadNames[] = {"pad_square", "pad_triangle", "pad_circle", "pad_cross", "pad_up", "pad_down",
                                 "pad_left", "pad_right", "pad_dpad", "pad_l", "pad_r", "pad_select", "pad_start",
                                 "pad_stick"};
static_assert(sizeof(kPadNames) / sizeof(kPadNames[0]) == kPadCount, "pad glyph name table");
static_assert(sizeof(kTexNames) / sizeof(kTexNames[0]) == kTexCount, "texture name table");
static_assert(sizeof(kFontNames) / sizeof(kFontNames[0]) == kFontCount, "font name table");
} // namespace

bool UiKit::load(pp::Pak& pak) {
    loadedBytes_ = 0;
    missing_ = 0;
    for (int i = 0; i < kTexCount; ++i) {
        const pp::PakEntry* e = pak.find(kTexNames[i]);
        void* blob = e ? pak.load(e) : nullptr;
        if (!blob || !pp::textureFromBlob(blob, e->size, tex_[i], true)) {
            if (blob) pp::pakFree(blob);
            ++missing_;
            PSP_LOGW(pp::tag::Gfx, "ui_tex_missing name=%s", kTexNames[i]);
            continue;
        }
        tex_[i].name = kTexNames[i];
        loadedBytes_ += e->size;
    }
    for (int i = 0; i < kFontCount; ++i) {
        const pp::PakEntry* e = pak.find(kFontNames[i]);
        void* blob = e ? pak.load(e) : nullptr;
        if (!blob || !font_[i].load(blob, e->size)) {
            ++missing_;
            PSP_LOGW(pp::tag::Gfx, "ui_font_missing name=%s", kFontNames[i]);
            continue;
        }
        loadedBytes_ += e->size;
    }
    int pads = 0;
    for (int i = 0; i < kPadCount; ++i) {  // optional: text fallbacks without them
        const pp::PakEntry* e = pak.find(kPadNames[i]);
        void* blob = e ? pak.load(e) : nullptr;
        if (!blob || !pp::textureFromBlob(blob, e->size, pad_[i], true)) {
            if (blob) pp::pakFree(blob);
            continue;
        }
        pad_[i].name = kPadNames[i];
        loadedBytes_ += e->size;
        ++pads;
    }
    PSP_LOGI(pp::tag::Gfx, "ui_pad_glyphs n=%d of %d", pads, static_cast<int>(kPadCount));
    return font_[kFontNormal].texture().pixels != nullptr;
}

bool UiKit::reloadFonts(pp::Pak& pak) {
    Font next[kFontCount];
    bool ok = true;
    for (int i = 0; i < kFontCount && ok; ++i) {
        const pp::PakEntry* e = pak.find(kFontNames[i]);
        void* blob = e ? pak.load(e) : nullptr;
        ok = blob && next[i].load(blob, e->size);  // load() frees the blob on failure
    }
    if (!ok || !next[kFontNormal].coversCyrillic()) {
        for (Font& f : next) f.release();
        return false;
    }
    for (int i = 0; i < kFontCount; ++i) {
        font_[i].release();
        font_[i] = next[i];  // the block moves over (Font frees only in release())
    }
    return true;
}

const char* UiKit::padLabel(PadGlyph g) {
    switch (g) {
        case kPadL: return "L";
        case kPadR: return "R";
        case kPadSelect: return "SELECT";
        case kPadStart: return "START";
        default: return nullptr;
    }
}

float UiKit::glyphWidth(PadGlyph g, float px) const {
    const char* label = padLabel(g);
    return label ? static_cast<float>(font_[kFontSmall].measure(label)) + 2.0f : px;
}

bool UiKit::glyph(UiRenderer& ui, PadGlyph g, const Rect& r, uint32_t abgr) const {
    if (padLabel(g)) return glyphFill(ui, g, r, abgr);
    // So the icons do not look stretched vertically: a whole-pixel square in the middle of r -
    // 10 x 10 less 12.5 % per side was 7.5 px, and the edges rounded to 7 wide, 8 high.
    const float side = std::max(1.0f, std::floor(std::min(r.w, r.h) * 0.75f + 0.5f));
    const float ox = ui.offsetX(), oy = ui.offsetY();  // whole screen pixels in a moved window too
    const float x = std::floor(ox + r.x + (r.w - side) * 0.5f + 0.5f) - ox;
    const float y = std::floor(oy + r.y + (r.h - side) * 0.5f + 0.5f) - oy;
    return glyphFill(ui, g, Rect{x, y, side, side}, abgr);
}

bool UiKit::glyphFill(UiRenderer& ui, PadGlyph g, const Rect& r, uint32_t abgr) const {
    if (const char* label = padLabel(g)) {
        const Font& f = font_[kFontSmall];
        ui.textShadow(f, r.x + r.w * 0.5f, r.y + (r.h - static_cast<float>(f.lineHeight())) * 0.5f + 1.0f, label,
                      abgr == 0xFFFFFFFFu ? kColGold : abgr, Align::Center);
        return true;
    }
    if (g < 0 || g >= kPadCount || !pad_[g].pixels) return false;
    ui.image(pad_[g], r, abgr);
    return true;
}

void UiKit::release() {
    for (auto& t : tex_) pp::textureRelease(t);
    for (auto& t : pad_) pp::textureRelease(t);
    for (auto& f : font_) f.release();
}

void UiKit::background(UiRenderer& ui, bool dim) const {
    const Rect full{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)};
    if (has(kTexBg)) ui.image(tex_[kTexBg], full);
    if (dim) ui.fill(full, 0x60000000u);
}

void UiKit::panel(UiRenderer& ui, const Rect& r) const {
    // The client's backdrop: tiled background inset by the edge, then the edge 9-slice.
    if (has(kTexDlgBg)) ui.tile(tex_[kTexDlgBg], Rect{r.x + 6, r.y + 6, r.w - 12, r.h - 12}, 0xF0FFFFFFu);
    else ui.fill(r, 0xE0000000u);
    if (has(kTexDlgBorder)) ui.nineSlice(tex_[kTexDlgBorder], r);
}

bool UiKit::button(UiRenderer& ui, FocusRing& focus, uint32_t id, const Rect& r, const char* label,
                   bool enabled) const {
    const bool focused = enabled && focus.addStop(id, r);
    const bool pressed = focused && focus.isPressed(id);
    const pp::Texture& face = tex_[pressed && has(kTexBtnDown) ? kTexBtnDown : kTexBtnUp];
    const uint32_t tint = enabled ? kColWhite : 0xFF808080u;
    if (face.pixels) ui.nineSlice(face, r, 1.0f, tint);
    else ui.fill(r, 0xFF101060u);
    // The client's highlight art sits on black: it is meant to be added, not blended.
    if (focused && has(kTexBtnHl)) ui.nineSlice(tex_[kTexBtnHl], r, 1.0f, kColWhite, Blend::Add);
    const Font& f = font_[kFontNormal];
    const float ty = r.y + (r.h - f.lineHeight()) * 0.5f + (pressed ? 1.0f : 0.0f);
    const uint32_t col = !enabled ? kColGrey : focused ? kColWhite : kColGold;
    ui.textShadow(f, r.x + r.w * 0.5f, ty, label, col, Align::Center);
    return focused;
}

void UiKit::tooltip(UiRenderer& ui, const Rect& anchor, const char* title, const char* line,
                    bool centered) const {
    struct Hold {  // not part of the window's bounds
        UiRenderer& u;
        explicit Hold(UiRenderer& x) : u(x) { u.boundsHold(true); }
        ~Hold() { u.boundsHold(false); }
    } hold(ui);
    const Font& ft = font_[kFontNormal];
    const Font& fs = font_[kFontSmall];
    float w = static_cast<float>(ft.measure(title));
    float w2 = line && *line ? static_cast<float>(fs.measure(line)) : 0.0f;
    if (w2 > w) w = w2;
    const bool oneLine = !line || !*line;  // GameTooltip:SetText - the title alone, a lower box
    Rect r{anchor.x, anchor.y - 44.0f, w + 16.0f, oneLine ? 22.0f : 38.0f};
    if (centered) r.x = anchor.x + (anchor.w - r.w) * 0.5f;
    if (r.x < 4) r.x = 4;
    if (r.x + r.w > pp::kScreenW - 4) r.x = pp::kScreenW - 4 - r.w;
    if (has(kTexTipBg)) ui.tile(tex_[kTexTipBg], Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xE0FFFFFFu);
    else ui.fill(r, 0xE0100000u);
    if (has(kTexTipBorder)) ui.nineSlice(tex_[kTexTipBorder], r);
    ui.text(ft, r.x + 8, r.y + 5, title, kColWhite);
    if (!oneLine) ui.text(fs, r.x + 8, r.y + 21, line, kColGreen);
}

void UiKit::cursor(UiRenderer& ui, const FocusRing& focus) const {
    if (focus.cursorVisible() && has(kTexCursor)) {
        ui.image(tex_[kTexCursor], Rect{focus.cursorX(), focus.cursorY(), 24, 24});
    }
}

} // namespace wowee::ui::psp
