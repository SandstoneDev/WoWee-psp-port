// Immediate-mode 2D renderer for the PSP UI on the GE.
//
// Quads are staged and flushed as GU_SPRITES runs, one run per (texture,
// blend, clip) state. Vertex memory comes from the display list and is
// written back from the D-cache before the draw; the GE reads it later in
// the frame (GU_DIRECT lags the CPU on hardware), so a run is never edited
// after submission.
//
// Coordinates are screen pixels (480x272) scaled by uiScale, then snapped
// with ONE rounding rule (floor(v + 0.5)) - a second rule (truncation) gave
// one-pixel seams in the Derma port.
#pragma once

#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_font.hpp"

#include <cstdint>

namespace wowee::ui::psp {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// Sub-rectangle of a texture in content texels.
struct SrcRect {
    float u = 0, v = 0, w = 0, h = 0;
};

enum class Blend : uint8_t { Alpha, Add };

enum class Align : uint8_t { Left, Center, Right };

class UiRenderer {
public:
    void beginFrame();
    void endFrame();

    void setScale(float s) { scale_ = s; }
    float scale() const { return scale_; }

    void fill(const Rect& r, uint32_t abgr);
    // Whole content area of the texture stretched into r.
    void image(const platform::psp::Texture& t, const Rect& r, uint32_t abgr = 0xFFFFFFFFu,
               Blend blend = Blend::Alpha);
    void image(const platform::psp::Texture& t, const Rect& r, const SrcRect& src,
               uint32_t abgr = 0xFFFFFFFFu, Blend blend = Blend::Alpha);
    // 9-slice using the texture's slice margins; `edge` scales the border
    // thickness (1 = the art's own texel size).
    void nineSlice(const platform::psp::Texture& t, const Rect& r, float edge = 1.0f,
                   uint32_t abgr = 0xFFFFFFFFu, Blend blend = Blend::Alpha);
    // Tiles the texture over r (for the client's backdrop backgrounds).
    void tile(const platform::psp::Texture& t, const Rect& r, uint32_t abgr = 0xFFFFFFFFu);
    // The texture's whole content stretched over r, clipped to the ellipse inscribed in r
    // (a triangle fan) - the round portraits (SetPortraitTexture is circular: the square 3D portrait
    // stuck out of the unit frame's ring). fillCircle: the same disc, untextured.
    void imageCircle(const platform::psp::Texture& t, const Rect& r, uint32_t abgr = 0xFFFFFFFFu);
    void fillCircle(const Rect& r, uint32_t abgr);

    // Single line of UTF-8 text; y is the top of the line box.
    void text(const Font& f, float x, float y, const char* utf8, uint32_t abgr, Align align = Align::Left);
    void textShadow(const Font& f, float x, float y, const char* utf8, uint32_t abgr,
                    Align align = Align::Left);

    // Text in a box: centered when it fits; otherwise clipped to the box and
    // scrolled like a ticker (1.5 s pause, ~25 px/s to the end, pause, again)
    // - long names in the unit frames (owner's request).
    void textMarquee(const Font& f, const Rect& box, const char* utf8, uint32_t abgr);

    void pushClip(const Rect& r);
    void popClip();

    // Movable windows: everything drawn - and every clip rect - is
    // shifted by (dx, dy) UI units until setOffset(0, 0).
    void setOffset(float dx, float dy) {
        ox_ = dx * scale_;
        oy_ = dy * scale_;
    }
    // Bounds (UI units, offset included, before clipping) of what is drawn
    // between boundsBegin() and boundsEnd(): a window's extent, whatever it draws.
    void boundsBegin() {
        bounds_ = true;
        bx0_ = by0_ = 1e9f;
        bx1_ = by1_ = -1e9f;
    }
    // The current window offset (UI units) - to keep a rect for drawing after the window.
    float offsetX() const { return ox_ / scale_; }
    float offsetY() const { return oy_ / scale_; }
    // Tooltips and popups drawn inside a window do not count toward its bounds (they grew the
    // window, and the screen clamp of the movable windows threw it about on the hardware).
    void boundsHold(bool on) { boundsHold_ = on; }
    Rect boundsEnd() {
        bounds_ = false;
        if (bx1_ < bx0_) return Rect{};
        return Rect{bx0_ / scale_, by0_ / scale_, (bx1_ - bx0_) / scale_, (by1_ - by0_) / scale_};
    }

    // Diagnostics for the log/HUD.
    uint32_t quadsLastFrame() const { return quadsLast_; }
    uint32_t runsLastFrame() const { return runsLast_; }

private:
    struct Vert {
        uint16_t u, v;
        uint32_t color;
        int16_t x, y, z, pad;
    };
    static_assert(sizeof(Vert) == 16, "GE sprite vertex must be 16 bytes");

    static constexpr int kMaxStaged = 512;  // vertices per run before a forced flush
    static constexpr int kClipDepth = 8;

    void setState(const platform::psp::Texture* t, Blend b);
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c);
    void flush();
    void applyClip();
    void fan(const platform::psp::Texture* t, const Rect& r, uint32_t abgr);  // imageCircle

    Vert staged_[kMaxStaged];
    int stagedCount_ = 0;
    const platform::psp::Texture* curTex_ = nullptr;
    Blend curBlend_ = Blend::Alpha;
    Rect clip_[kClipDepth];
    int clipTop_ = 0;
    float scale_ = 1.0f;
    float ox_ = 0.0f, oy_ = 0.0f;  // setOffset, screen pixels
    bool bounds_ = false, boundsHold_ = false;
    float bx0_ = 0, by0_ = 0, bx1_ = 0, by1_ = 0;
    uint32_t quads_ = 0, runs_ = 0, quadsLast_ = 0, runsLast_ = 0;
    // GE audit: every quad of the staged run maps texels 1:1 - drawn GU_NEAREST even from a GU_LINEAR
    // texture (a 1:1 bilinear sprite lands on texel centres only when its area is a power of two - PPSSPP
    // "GE bit-exact"; nearest is exact at 1:1 whatever the area).
    bool runUnit_ = true;
};

} // namespace wowee::ui::psp
