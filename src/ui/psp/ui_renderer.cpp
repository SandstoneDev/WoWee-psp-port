#include "ui/psp/ui_renderer.hpp"

#include "platform/psp/psp_display.hpp"

#include <pspgu.h>
#include <pspthreadman.h>
#include <psputils.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

inline int snap(float v) { return static_cast<int>(std::floor(v + 0.5f)); }

inline int16_t clampCoord(int v) {
    // Screen positions stay far inside the GE's 4096 window; clamp anyway so
    // a bad layout value never wraps into another framebuffer row.
    if (v < -1024) v = -1024;
    if (v > 2047) v = 2047;
    return static_cast<int16_t>(v);
}

} // namespace

void UiRenderer::beginFrame() {
    stagedCount_ = 0;
    runUnit_ = true;
    curTex_ = nullptr;
    curBlend_ = Blend::Alpha;
    quads_ = runs_ = 0;
    clipTop_ = 0;
    clip_[0] = Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)};

    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuShadeModel(GU_FLAT);
    pp::textureResetBinding();
    applyClip();
}

void UiRenderer::endFrame() {
    flush();
    sceGuScissor(0, 0, pp::kScreenW, pp::kScreenH);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    quadsLast_ = quads_;
    runsLast_ = runs_;
}

void UiRenderer::setState(const pp::Texture* t, Blend b) {
    if (t == curTex_ && b == curBlend_) return;
    flush();
    curTex_ = t;
    curBlend_ = b;
}

void UiRenderer::flush() {
    if (stagedCount_ == 0) return;
    const size_t bytes = static_cast<size_t>(stagedCount_) * sizeof(Vert);
    // The display list's last bytes stay free - past its end the GE runs garbage (psp_display.hpp).
    if (pp::displayListFree() < pp::kListReserveUi + bytes) {
        stagedCount_ = 0;
        runUnit_ = true;
        pp::displayListFull("ui");
        return;
    }
    auto* dst = static_cast<Vert*>(sceGuGetMemory(bytes));
    std::memcpy(dst, staged_, bytes);
    sceKernelDcacheWritebackRange(dst, bytes);

    pp::textureBind(curTex_);
    if (curBlend_ == Blend::Add) {
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFFu);
    } else {
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    }
    // a run of 1:1 quads from a bilinear texture samples nearest (the same picture where the
    // GE's gradient is exact, and still exact where it is not), then the texture's own filter is put back -
    // textureBind skips the filter while the texture stays bound.
    const bool nearest = runUnit_ && curTex_ && (curTex_->flags & pp::kTexLinear) && curTex_->levels <= 1;
    if (nearest) sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   stagedCount_, nullptr, dst);
    if (nearest) sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    ++runs_;
    stagedCount_ = 0;
    runUnit_ = true;
}

void UiRenderer::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                      uint32_t c) {
    x0 += ox_;  // movable windows
    x1 += ox_;
    y0 += oy_;
    y1 += oy_;
    if (bounds_ && !boundsHold_) {
        // inside a pushed clip only the visible part counts (a marquee's text runs
        // past its box; counted whole it widened the window and the screen clamp of the
        // movable windows pushed it aside). The screen clip (level 0) is not applied: a
        // window off the screen must still measure its full size.
        float bx0 = x0, by0 = y0, bx1 = x1, by1 = y1;
        if (clipTop_ > 0) {
            const Rect& c = clip_[clipTop_];
            bx0 = std::fmax(bx0, c.x);
            by0 = std::fmax(by0, c.y);
            bx1 = std::fmin(bx1, c.x + c.w);
            by1 = std::fmin(by1, c.y + c.h);
        }
        if (bx1 > bx0 && by1 > by0) {
            bx0_ = std::fmin(bx0_, bx0);
            by0_ = std::fmin(by0_, by0);
            bx1_ = std::fmax(bx1_, bx1);
            by1_ = std::fmax(by1_, by1);
        }
    }
    // CPU-side clip against the current clip rect so UVs stay correct and
    // nothing relies on the scissor alone.
    const Rect& cl = clip_[clipTop_];
    float cx0 = cl.x, cy0 = cl.y, cx1 = cl.x + cl.w, cy1 = cl.y + cl.h;
    if (x1 <= cx0 || y1 <= cy0 || x0 >= cx1 || y0 >= cy1 || x1 <= x0 || y1 <= y0) return;
    float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
    if (x0 < cx0) { u0 += (cx0 - x0) * du; x0 = cx0; }
    if (y0 < cy0) { v0 += (cy0 - y0) * dv; y0 = cy0; }
    if (x1 > cx1) { u1 -= (x1 - cx1) * du; x1 = cx1; }
    if (y1 > cy1) { v1 -= (y1 - cy1) * dv; y1 = cy1; }

    if (stagedCount_ + 2 > kMaxStaged) flush();
    Vert* v = &staged_[stagedCount_];
    v[0] = {static_cast<uint16_t>(snap(u0)), static_cast<uint16_t>(snap(v0)), c,
            clampCoord(snap(x0)), clampCoord(snap(y0)), 0, 0};
    v[1] = {static_cast<uint16_t>(snap(u1)), static_cast<uint16_t>(snap(v1)), c,
            clampCoord(snap(x1)), clampCoord(snap(y1)), 0, 0};
    // 1:1 = as many texels as pixels on both axes (mirrored too); one stretched quad makes the
    // whole run bilinear, as before.
    if (std::abs(v[1].u - v[0].u) != v[1].x - v[0].x || std::abs(v[1].v - v[0].v) != v[1].y - v[0].y) runUnit_ = false;
    stagedCount_ += 2;
    ++quads_;
}

void UiRenderer::fill(const Rect& r, uint32_t abgr) {
    setState(nullptr, Blend::Alpha);
    const float s = scale_;
    quad(r.x * s, r.y * s, (r.x + r.w) * s, (r.y + r.h) * s, 0, 0, 0, 0, abgr);
}

void UiRenderer::image(const pp::Texture& t, const Rect& r, uint32_t abgr, Blend blend) {
    image(t, r, SrcRect{0, 0, static_cast<float>(t.uw), static_cast<float>(t.uh)}, abgr, blend);
}

void UiRenderer::image(const pp::Texture& t, const Rect& r, const SrcRect& src, uint32_t abgr, Blend blend) {
    setState(&t, blend);
    const float s = scale_;
    quad(r.x * s, r.y * s, (r.x + r.w) * s, (r.y + r.h) * s, src.u, src.v, src.u + src.w, src.v + src.h, abgr);
}

void UiRenderer::nineSlice(const pp::Texture& t, const Rect& r, float edge, uint32_t abgr, Blend blend) {
    setState(&t, blend);
    const float s = scale_;
    const float W = t.uw, H = t.uh;
    const float sl = t.slice[0], st = t.slice[1], sr = t.slice[2], sb = t.slice[3];
    const float dl = sl * edge, dt = st * edge, dr = sr * edge, db = sb * edge;
    const float xs[4] = {r.x, r.x + dl, r.x + r.w - dr, r.x + r.w};
    const float ys[4] = {r.y, r.y + dt, r.y + r.h - db, r.y + r.h};
    const float us[4] = {0, sl, W - sr, W};
    const float vs[4] = {0, st, H - sb, H};
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            quad(xs[i] * s, ys[j] * s, xs[i + 1] * s, ys[j + 1] * s, us[i], vs[j], us[i + 1], vs[j + 1], abgr);
        }
    }
}

void UiRenderer::tile(const pp::Texture& t, const Rect& r, uint32_t abgr) {
    setState(&t, Blend::Alpha);
    pushClip(r);
    const float tw = t.uw, th = t.uh;
    for (float y = r.y; y < r.y + r.h; y += th) {
        for (float x = r.x; x < r.x + r.w; x += tw) {
            quad(x * scale_, y * scale_, (x + tw) * scale_, (y + th) * scale_, 0, 0, tw, th, abgr);
        }
    }
    popClip();
}

void UiRenderer::imageCircle(const pp::Texture& t, const Rect& r, uint32_t abgr) { fan(&t, r, abgr); }

void UiRenderer::fillCircle(const Rect& r, uint32_t abgr) { fan(nullptr, r, abgr); }

// A disc as one GU_TRIANGLE_FAN (centre + 33 rim vertices, the sprite vertex format in
// through mode). Drawn at once after the staged sprites (flushed first, so the order holds); the
// GE scissor of the current clip cuts it, a disc wholly outside the clip is skipped on the CPU.
void UiRenderer::fan(const pp::Texture* t, const Rect& r, uint32_t abgr) {
    constexpr int kSeg = 32;
    const float s = scale_;
    const float x0 = r.x * s + ox_, y0 = r.y * s + oy_, w = r.w * s, h = r.h * s;
    if (w <= 0.0f || h <= 0.0f) return;
    if (bounds_ && !boundsHold_) {  // as quad(): the visible part inside a pushed clip
        float bx0 = x0, by0 = y0, bx1 = x0 + w, by1 = y0 + h;
        if (clipTop_ > 0) {
            const Rect& c = clip_[clipTop_];
            bx0 = std::fmax(bx0, c.x);
            by0 = std::fmax(by0, c.y);
            bx1 = std::fmin(bx1, c.x + c.w);
            by1 = std::fmin(by1, c.y + c.h);
        }
        if (bx1 > bx0 && by1 > by0) {
            bx0_ = std::fmin(bx0_, bx0);
            by0_ = std::fmin(by0_, by0);
            bx1_ = std::fmax(bx1_, bx1);
            by1_ = std::fmax(by1_, by1);
        }
    }
    const Rect& cl = clip_[clipTop_];
    if (x0 + w <= cl.x || y0 + h <= cl.y || x0 >= cl.x + cl.w || y0 >= cl.y + cl.h) return;
    flush();
    const float cx = x0 + w * 0.5f, cy = y0 + h * 0.5f;
    const float tw = t ? static_cast<float>(t->uw) : 0.0f, th = t ? static_cast<float>(t->uh) : 0.0f;
    const size_t bytes = static_cast<size_t>(kSeg + 2) * sizeof(Vert);
    if (pp::displayListFree() < pp::kListReserveUi + bytes) {
        pp::displayListFull("ui");
        return;
    }
    auto* v = static_cast<Vert*>(sceGuGetMemory(bytes));
    v[0] = {static_cast<uint16_t>(snap(tw * 0.5f)), static_cast<uint16_t>(snap(th * 0.5f)), abgr,
            clampCoord(snap(cx)), clampCoord(snap(cy)), 0, 0};
    for (int i = 0; i <= kSeg; ++i) {
        const float a = static_cast<float>(i) * (6.28318531f / kSeg);
        const float c = std::cos(a), sn = std::sin(a);
        v[i + 1] = {static_cast<uint16_t>(snap(tw * 0.5f * (1.0f + c))), static_cast<uint16_t>(snap(th * 0.5f * (1.0f + sn))),
                    abgr, clampCoord(snap(cx + w * 0.5f * c)), clampCoord(snap(cy + h * 0.5f * sn)), 0, 0};
    }
    sceKernelDcacheWritebackRange(v, bytes);
    pp::textureBind(t);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_TRIANGLE_FAN, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, kSeg + 2,
                   nullptr, v);
    ++runs_;
    ++quads_;
}

void UiRenderer::text(const Font& f, float x, float y, const char* utf8, uint32_t abgr, Align align) {
    if (!utf8 || !*utf8) return;
    const float s = scale_;
    float px = x * s;
    if (align != Align::Left) {
        float w = static_cast<float>(f.measure(utf8));
        px -= (align == Align::Center) ? w * 0.5f : w;
    }
    px = static_cast<float>(snap(px));
    const float py = static_cast<float>(snap(y * s));
    setState(&f.texture(), Blend::Alpha);
    const char* p = utf8;
    while (*p) {
        uint32_t cp = utf8Next(&p);
        const Glyph* g = f.find(cp);
        if (!g) continue;
        if (g->w && g->h) {
            float gx = px + g->bearingX, gy = py + g->top;
            quad(gx, gy, gx + g->w, gy + g->h, g->x, g->y, g->x + g->w, g->y + g->h, abgr);
        }
        px += g->advance;
    }
}

void UiRenderer::textShadow(const Font& f, float x, float y, const char* utf8, uint32_t abgr, Align align) {
    const float o = 1.0f / scale_;
    text(f, x + o, y + o, utf8, (abgr & 0xFF000000u), align);
    text(f, x, y, utf8, abgr, align);
}

void UiRenderer::textMarquee(const Font& f, const Rect& box, const char* utf8, uint32_t abgr) {
    const float w = static_cast<float>(f.measure(utf8)) / scale_;
    const float y = box.y + (box.h - f.lineHeight() / scale_) * 0.5f;
    if (w <= box.w) {
        textShadow(f, box.x + box.w * 0.5f, y, utf8, abgr, Align::Center);
        return;
    }
    constexpr float kPause = 1.5f, kSpeed = 25.0f;
    const float travel = w - box.w;
    const float cycle = kPause + travel / kSpeed + kPause;
    const float t = std::fmod(static_cast<float>(sceKernelGetSystemTimeWide() % 3600000000ull) / 1e6f, cycle);
    const float off = t < kPause ? 0.0f : std::fmin(travel, (t - kPause) * kSpeed);
    pushClip(box);
    textShadow(f, box.x - off, y, utf8, abgr, Align::Left);
    popClip();
}

void UiRenderer::pushClip(const Rect& r) {
    if (clipTop_ + 1 >= kClipDepth) return;
    const Rect& p = clip_[clipTop_];
    Rect n{r.x * scale_ + ox_, r.y * scale_ + oy_, r.w * scale_, r.h * scale_};
    float x0 = std::fmax(n.x, p.x), y0 = std::fmax(n.y, p.y);
    float x1 = std::fmin(n.x + n.w, p.x + p.w), y1 = std::fmin(n.y + n.h, p.y + p.h);
    flush();
    clip_[++clipTop_] = Rect{x0, y0, std::fmax(0.0f, x1 - x0), std::fmax(0.0f, y1 - y0)};
    applyClip();
}

void UiRenderer::popClip() {
    if (clipTop_ == 0) return;
    flush();
    --clipTop_;
    applyClip();
}

void UiRenderer::applyClip() {
    const Rect& c = clip_[clipTop_];
    int x0 = snap(c.x), y0 = snap(c.y), x1 = snap(c.x + c.w), y1 = snap(c.y + c.h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > pp::kScreenW) x1 = pp::kScreenW;
    if (y1 > pp::kScreenH) y1 = pp::kScreenH;
    // pspsdk takes width/height and computes end = x + w - 1, so an empty
    // rect must not reach it; quads are already rejected on the CPU then.
    if (x1 <= x0 || y1 <= y0) {
        sceGuScissor(0, 0, 1, 1);
        return;
    }
    sceGuScissor(x0, y0, x1 - x0, y1 - y0);
}

} // namespace wowee::ui::psp
