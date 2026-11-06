#include "ui/psp/ui_kit.hpp"
#include "ui/psp/ui_tutorial_controls.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {
const char* const kTexNames[] = {
    "bg_login", "logo_wotlk", "dlg_border", "dlg_bg", "tip_border", "tip_bg", "btn_up", "btn_down",
    "btn_hl", "cursor_point", "icon_sword", "icon_holybolt", "icon_fireball", "icon_frostbolt",
    "icon_heal", "icon_potion", "icon_hearth", "icon_mount", "icon_border",
};
const char* const kFontNames[] = {"font_small", "font_normal", "font_title", "font_tiny", "font_combatcrit"};
const char* const kPadNames[] = {"pad_square", "pad_triangle", "pad_circle", "pad_cross", "pad_up", "pad_down",
                                 "pad_left", "pad_right", "pad_dpad", "pad_l", "pad_r", "pad_select", "pad_start",
                                 "pad_stick"};
static_assert(sizeof(kPadNames) / sizeof(kPadNames[0]) == kPadCount, "pad glyph name table");
static_assert(sizeof(kTexNames) / sizeof(kTexNames[0]) == kTexCount, "texture name table");
static_assert(sizeof(kFontNames) / sizeof(kFontNames[0]) == kFontCount, "font name table");
} // namespace

bool UiKit::load(pp::Pak& pak) {
    ++fontRevision_;
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
        if(i==kFontCombatCrit && !blob)continue; // optional in older packs
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
    ++fontRevision_;
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

const char* UiKit::glyphLabel(PadGlyph g) const {
    // PSP: keep SELECT readable in the same text style as the shoulder buttons.
    if(g==kPadL || g==kPadR || g==kPadSelect || g==kPadStart) return padLabel(g);
    return g>=0 && g<kPadCount && pad_[g].pixels ? nullptr : padLabel(g);
}

float UiKit::glyphWidth(PadGlyph g, float px) const {
    if(const char* label=glyphLabel(g)) return static_cast<float>(font_[kFontSmall].measure(label))+2.f;
    if(g>=0 && g<kPadCount && pad_[g].pixels && pad_[g].uh)
        return px*static_cast<float>(pad_[g].uw)/pad_[g].uh;
    return px;
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
    if (const char* label = glyphLabel(g)) {
        const Font& f = font_[kFontSmall];
        ui.textShadow(f, r.x + r.w * 0.5f, r.y + (r.h - static_cast<float>(f.lineHeight())) * 0.5f + 1.0f, label,
                      abgr == 0xFFFFFFFFu ? kColGold : abgr, Align::Center);
        return true;
    }
    if(g>=kPadSquare && g<=kPadCross && !pad_[g].pixels) {
        const float side=std::max(2.f,std::min(r.w,r.h)-1.f);
        const float x=r.x+(r.w-side)*.5f,y=r.y+(r.h-side)*.5f;
        const uint32_t colors[]={0xFFB080FFu,0xFF80E080u,0xFF8080FFu,0xFFFFC080u};
        const uint32_t color=abgr==0xFFFFFFFFu?colors[g]:abgr;
        for(int pass=0;pass<2;++pass) {
        const float offset=pass==0?1.f:0.f;
        const uint32_t ink=pass==0?(((color>>24)*192u/255u)<<24):color;
        auto line=[&](float x0,float y0,float x1,float y1){
            const int n=std::max(1,static_cast<int>(std::ceil(std::max(std::fabs(x1-x0),std::fabs(y1-y0)))));
            for(int i=0;i<=n;++i){const float f=static_cast<float>(i)/n;ui.fill(Rect{std::floor(x+offset+x0+(x1-x0)*f),std::floor(y+offset+y0+(y1-y0)*f),1,1},ink);}
        };
        if(g==kPadSquare){line(0,0,side,0);line(side,0,side,side);line(side,side,0,side);line(0,side,0,0);}
        else if(g==kPadTriangle){line(side*.5f,0,side,side);line(side,side,0,side);line(0,side,side*.5f,0);}
        else if(g==kPadCross){line(0,0,side,side);line(side,0,0,side);}
        else for(int i=0;i<24;++i){const float a=i*6.2831853f/24,b=(i+1)*6.2831853f/24;line((1+std::cos(a))*side*.5f,(1+std::sin(a))*side*.5f,(1+std::cos(b))*side*.5f,(1+std::sin(b))*side*.5f);}
        }
        return true;
    }
    if (g < 0 || g >= kPadCount || !pad_[g].pixels) return false;
    // Shoulder/START/SELECT art is wider than the face buttons. Fit it without
    // stretching when a caller supplies a square slot (e.g. action-bar keys).
    const float aspect=pad_[g].uh ? static_cast<float>(pad_[g].uw)/pad_[g].uh : 1.f;
    const float w=std::min(r.w,r.h*aspect),h=w/aspect;
    const Rect icon{r.x+(r.w-w)*.5f,r.y+(r.h-h)*.5f,w,h};
    // A small silhouette shadow leaves the surrounding interface visible.
    const uint32_t shadowAlpha=((abgr>>24)*192u/255u)<<24;
    ui.image(pad_[g],Rect{icon.x+1.f,icon.y+1.f,w,h},shadowAlpha);
    ui.image(pad_[g],icon,abgr);
    return true;
}

namespace {
// Short stack text runs keep hints allocation-free, including long translations.
template<class Text,class Glyph>
void hintRuns(const char* text,Text&& emitText,Glyph&& emitGlyph) {
    if(!text) return;
    const char* p=text;
    while(*p) {
        int length=0;
        const int glyph=tutorialGlyphToken(p,&length);
        if(glyph>=0) {emitGlyph(static_cast<PadGlyph>(glyph));p+=length;continue;}
        const char* begin=p;
        do {++p;} while(*p && p-begin<255 && tutorialGlyphToken(p,nullptr)<0);
        // Do not split a UTF-8 code point between stack buffers.
        if(p-begin==255) while(p>begin && (static_cast<unsigned char>(*p)&0xC0)==0x80) --p;
        char run[256];const size_t count=static_cast<size_t>(p-begin);
        std::memcpy(run,begin,count);run[count]=0;emitText(run);
    }
}
}

float UiKit::controlHintWidth(const Font& f,const char* text) const {
    const float px=std::clamp(static_cast<float>(f.lineHeight()),8.f,12.f);
    float total=0;
    hintRuns(text,[&](const char* run){total+=f.measure(run);},[&](PadGlyph g){
        const char* label=padLabel(g);if(!label)label=kTutorialGlyphNames[g];
        total+=(hasGlyph(g)?glyphWidth(g,px):static_cast<float>(f.measure(label)))+2.f;
    });
    return total;
}

void UiKit::controlHint(UiRenderer& ui,const Font& f,float x,float y,const char* text,
                        uint32_t abgr,Align align) const {
    const float px=std::clamp(static_cast<float>(f.lineHeight()),8.f,12.f);
    auto fallback=[&](PadGlyph g) {const char* label=padLabel(g);return label?label:kTutorialGlyphNames[g];};
    auto width=[&](PadGlyph g) {
        return (hasGlyph(g)?glyphWidth(g,px):static_cast<float>(f.measure(fallback(g))))+2.f;
    };
    if(align!=Align::Left) {
        float total=0;
        hintRuns(text,[&](const char* run){total+=f.measure(run);},[&](PadGlyph g){total+=width(g);});
        x-=align==Align::Center ? total*.5f : total;
    }
    hintRuns(text,[&](const char* run){ui.textShadow(f,x,y,run,abgr);x+=f.measure(run);},
        [&](PadGlyph g){
            const float w=width(g);
            if(!glyphFill(ui,g,Rect{x+1.f,y+(f.lineHeight()-px)*.5f,w-2.f,px}))
                ui.textShadow(f,x+1.f,y,fallback(g),abgr);
            x+=w;
        });
}

void UiKit::release() {
    ++fontRevision_;
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
