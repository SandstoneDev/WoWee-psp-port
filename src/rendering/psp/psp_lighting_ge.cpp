// Lighting on the GE (see include/rendering/psp/psp_lighting_ge.hpp).
#include "rendering/psp/psp_lighting_ge.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_vmath.hpp"
#include "platform/psp/psp_texture.hpp"

#include <pspge.h>
#include <pspgu.h>
#include <psputils.h>  // sceKernelDcacheWritebackRange

#include <algorithm>
#include <cmath>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;

namespace {

FrameLight g_frame;
bool g_dither3d = false;  // cfg dither3d
// the sky's 4 x 4 matrix (kSkyDither below) for the lit 3D passes.
const ScePspIMatrix4 kWorldDither = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
bool g_lit = false;       // GU_LIGHTING on between lightBegin and lightEnd
bool g_unlitNow = false;  // lightBatch(true) in effect

// Glow buffer: 120 x 68 (a quarter of 480 x 272 each way) in a 128-wide
// 5551 block at the top of eDRAM; 72 rows so bilinear reads stay inside.
constexpr int kGlowW = 120, kGlowH = 68, kGlowStride = 128, kGlowRows = 72;
uint32_t g_glowOff = 0;   // VRAM-relative, 0 = none

struct SkyVert {
    uint32_t color;
    int16_t x, y, z;
    int16_t pad;
};
static_assert(sizeof(SkyVert) == 12, "8888 + s16 xyz vertex, 4-aligned");

struct RectVert {
    uint32_t color;
    int16_t x, y, z;
    int16_t pad;
};

struct TexVert {
    int16_t u, v;
    int16_t x, y, z;
};
static_assert(sizeof(TexVert) == 10, "s16 uv + s16 xyz vertex");

// skyfast=1 - the sky's 8 column strips (35 rows
// x 2 vertices each), kept between frames. Rewritten only by skyFastPrepare in
// a later frame, after frameEnd's sceGuSync (the GE is done with the last
// frame's list), then written back from the D-cache before the draws are put
// in the list (it is not list memory, which libgu writes uncached).
alignas(64) SkyVert g_fastStrips[kSkyFastStrips][kSkyFastStripVerts];
SkyGridKey g_fastKey;
bool g_fastValid = false;
SkyCacheStats g_skyStats;

}  // namespace

FrameLight& frameLight() { return g_frame; }

void setDither3d(bool on) { g_dither3d = on; }
bool dither3d() { return g_dither3d; }

void lightBegin(const TerrainCamera& cam, uint32_t fogColorABGR) {
    const FrameLight& f = g_frame;
    if (f.fogBands) {
        sceGuFog(f.fogStart, f.fogEnd, f.fogABGR);
    } else {
        sceGuFog(cam.farZ * 0.55f, cam.farZ * 0.98f, fogColorABGR);
    }
    sceGuEnable(GU_FOG);
    if (g_dither3d) {  // dither3d
        sceGuSetDither(&kWorldDither);
        sceGuEnable(GU_DITHER);
    }
    if (f.tint == kTintGeAmbient) {
        // Vertex colour = material ambient; no lights, so the lit colour is
        // emissive (0) + global ambient x vertex colour: the baked light
        // times the tint, alpha kept (ambient alpha 0xFF).
        sceGuEnable(GU_LIGHTING);
        for (int i = 0; i < 4; ++i) sceGuDisable(GU_LIGHT0 + i);
        sceGuModelColor(0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
        sceGuColorMaterial(GU_AMBIENT);
        sceGuAmbient(f.ambientABGR);
        if (f.twoX) sceGuEnable(GU_FRAGMENT_2X);
        g_lit = true;
        g_unlitNow = false;
    }
}

void lightEnd() {
    sceGuDisable(GU_FOG);
    if (g_dither3d) sceGuDisable(GU_DITHER);  // the HUD and the next frame's clear stay undithered
    if (g_lit) {
        sceGuDisable(GU_LIGHTING);
        if (g_frame.twoX) sceGuDisable(GU_FRAGMENT_2X);
        g_lit = false;
        g_unlitNow = false;
    }
}

void lightBatch(bool unlit) {
    if (!g_lit || unlit == g_unlitNow) return;
    g_unlitNow = unlit;
    sceGuAmbient(unlit ? (g_frame.twoX ? 0xFF808080u : 0xFFFFFFFFu) : g_frame.ambientABGR);
}

// --- Sky ----------------------------------------------------------------------

SkyColors skyColorsFrom(const LightingParams& p) {
    SkyColors s;
    s.zenith = p.skyTopColor;
    s.mid = p.skyMiddleColor;
    s.horizon = p.skyBand1Color;
    s.fringe = p.skyBand2Color;
    // Upstream Skybox::render: the sun is opposite the light's direction.
    const float lenSq = glm::dot(p.directionalDir, p.directionalDir);
    s.sunDir = lenSq > 1e-8f ? -p.directionalDir * (1.0f / std::sqrt(lenSq)) : glm::vec3(0.0f, 0.0f, 1.0f);
    return s;
}

// Upstream assets/shaders/skybox.frag.glsl per vertex: skyColorAt, now in
// psp_sky_grid.hpp with the grid (the host test runs it).

namespace {

const ScePspIMatrix4 kSkyDither = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

// The gradient's GE state: untextured, smooth, dithered (5551 frame: the
// gradient bands without it).
void skyStateBegin() {
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    sceGuShadeModel(GU_SMOOTH);
    sceGuSetDither(&kSkyDither);
    sceGuEnable(GU_DITHER);
}

}  // namespace

void drawSky(const TerrainCamera& cam, const SkyColors& s) {
    // Screen grid: 9 columns (60 px) x 35 rows (8 px), colour per vertex.
    SkyView view;
    if (!skyViewFrom(cam.forward, cam.up, cam.fovyDeg, view)) return;  // looking straight up/down: keep the clear colour
    static uint32_t colors[kSkyRows][kSkyCols];
    skyGridColors(view, s, false, colors);  // 315 per frame
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    skyStateBegin();
    for (int strip = 0; strip < kSkyLegacyStrips; ++strip) {  // one row of cells per strip
        auto* v = static_cast<SkyVert*>(sceGuGetMemory(kSkyLegacyStripVerts * sizeof(SkyVert)));
        for (int i = 0; i < kSkyLegacyStripVerts; ++i) {
            const SkyStripVertex g = skyStripVertex(false, strip, i);
            v[i] = {colors[g.row][g.col], static_cast<int16_t>(skyColumnX(false, g.col)),
                    static_cast<int16_t>(skyRowY(g.row)), 0, 0};
        }
        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, kSkyLegacyStripVerts,
                       nullptr, v);
    }
    sceGuDisable(GU_DITHER);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_DEPTH_TEST);
}

// --- skyfast=1 ------------------------------------

bool skyFastPrepare(const TerrainCamera& cam, const SkyColors& s) {
    SkyView view;
    if (!skyViewFrom(cam.forward, cam.up, cam.fovyDeg, view)) return false;
    const SkyGridKey key = skyGridKey(cam.forward, cam.up, cam.fovyDeg, s, true);
    if (g_fastValid && key == g_fastKey) {
        ++g_skyStats.hits;  // the same picture as last frame: no CPU at all
        return true;
    }
    static uint32_t colors[kSkyRows][kSkyCols];
    skyGridColors(view, s, true, colors);
    // Column strip c: (x_c, y_row), (x_c+1, y_row) for every row - triangles
    // (TL, TR, BL), (TR, BL, BR): the cell's diagonal runs top-right to
    // bottom-left, as in the row strips (no change to the interpolation).
    for (int strip = 0; strip < kSkyFastStrips; ++strip) {
        for (int i = 0; i < kSkyFastStripVerts; ++i) {
            const SkyStripVertex g = skyStripVertex(true, strip, i);
            g_fastStrips[strip][i] = {colors[g.row][g.col], static_cast<int16_t>(skyColumnX(true, g.col)),
                                      static_cast<int16_t>(skyRowY(g.row)), 0, 0};  // z = 0: what the clear wrote
        }
    }
    sceKernelDcacheWritebackRange(g_fastStrips, sizeof(g_fastStrips));
    g_fastKey = key;
    g_fastValid = true;
    ++g_skyStats.misses;
    return true;
}

namespace {

// The strips `times` times (skyFastDraw once, skyBench mode 1 more often):
// colour and depth on every pixel - depth test ALWAYS, depth writes on (a
// disabled depth test writes no depth, GE Users Manual §17.6 p97).
void skyFastStrips(int times) {
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_ALWAYS);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_CULL_FACE);  // never on here; the column strips wind the other way than the row strips
    skyStateBegin();
    for (int n = 0; n < times; ++n) {
        for (int strip = 0; strip < kSkyFastStrips; ++strip) {
            sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, kSkyFastStripVerts,
                           nullptr, g_fastStrips[strip]);
        }
    }
    sceGuDisable(GU_DITHER);
    sceGuDepthFunc(GU_GEQUAL);  // the frame's rule (psp_display: reversed depth, cleared to 0)
}

}  // namespace

void skyFastDraw() {
    if (g_fastValid) skyFastStrips(1);
}

void skyClear(uint32_t abgr) {
    sceGuDisable(GU_DITHER);  // clear mode dithers too (GE Users Manual §19.6 p107); a flat sky must stay flat
    sceGuClearColor(abgr);
    sceGuClearDepth(0);
    // libgu's fast clear: 64-px strips (one 480 x 272 sprite without the bit).
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT | GU_FAST_CLEAR_BIT);
}

void skyBench(int mode, int passes) {
    if (mode == 1) {
        if (g_fastValid) skyFastStrips(passes);
        return;
    }
    if (mode != 2 && mode != 3) return;
    sceGuClearColor(0xFF000000u);
    sceGuClearDepth(0);
    for (int n = 0; n < passes; ++n) {
        // 2: the frame clear skyfast drops (psp_display frameBegin); 3: the fast clear.
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT | (mode == 3 ? GU_FAST_CLEAR_BIT : 0));
    }
}

SkyCacheStats skyCacheStatsTake() {
    const SkyCacheStats s = g_skyStats;
    g_skyStats = SkyCacheStats{};
    return s;
}

// --- Post: screen tint, glow --------------------------------------------------

bool glowReserve() {
    if (g_glowOff) return true;
    g_glowOff = pp::vramReserveTop(kGlowStride * kGlowRows * 2);
    return g_glowOff != 0;
}

bool glowAvailable() { return g_glowOff != 0; }

void drawPost(int glowPercent) {
    const FrameLight& f = g_frame;
    pp::textureResetBinding();
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_ALPHA_TEST);
    if (f.tint == kTintScreen && f.screenABGR != 0xFFFFFFFFu) {
        // frame x tint on geometry only: at z = 1 with LEQUAL the cleared
        // sky (depth 0) fails, anything drawn (depth > 0) passes.
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_OTHER_COLOR, GU_FIX, 0, 0);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(GU_LEQUAL);
        sceGuDepthMask(GU_TRUE);
        for (int x = 0; x < pp::kScreenW; x += 64) {  // 64 px strips: the GE's page-friendly width
            auto* v = static_cast<RectVert*>(sceGuGetMemory(2 * sizeof(RectVert)));
            v[0] = {f.screenABGR, static_cast<int16_t>(x), 0, 1, 0};
            v[1] = {f.screenABGR, static_cast<int16_t>(std::min(x + 64, pp::kScreenW)),
                    static_cast<int16_t>(pp::kScreenH), 1, 0};
            sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, v);
        }
        sceGuDepthFunc(GU_GEQUAL);
        sceGuDepthMask(GU_FALSE);
        sceGuDisable(GU_BLEND);
    }
    if (glowPercent <= 0 || !g_glowOff) return;
    // Glow: the frame at a quarter of its size (bilinear), then stretched
    // back over it as "screen" (dst + src x (1 - dst): no clipping to white)
    // at glowPercent. Upstream: no bloom pass (tonemap shoulder, FXAA, sun
    // shafts); the 3.3.5a client's "full screen glow" blurs the frame and adds it.
    uint8_t* vram = static_cast<uint8_t*>(sceGeEdramGetAddr());
    void* frame = pp::displayDrawBuffer();
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
    // A: frame -> glow buffer.
    sceGuDrawBufferList(GU_PSM_5551, reinterpret_cast<void*>(static_cast<uintptr_t>(g_glowOff)), kGlowStride);
    sceGuScissor(0, 0, kGlowW, kGlowH);
    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    sceGuTexImage(0, 512, 512, 512, vram + reinterpret_cast<uintptr_t>(frame));
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFlush();
    // the frame was drawn just now - texture reads wait for the drawing to finish (CMD_TSYNC,
    // GE Users Manual §13.8 "recursive drawing"; the flush only empties the 8 KB texture cache, which the GE's
    // own drawing never updates).
    sceGuTexSync();
    for (int x = 0; x < kGlowW; x += 8) {
        auto* v = static_cast<TexVert*>(sceGuGetMemory(2 * sizeof(TexVert)));
        const int x1 = std::min(x + 8, kGlowW);
        v[0] = {static_cast<int16_t>(x * 4), 0, static_cast<int16_t>(x), 0, 0};
        v[1] = {static_cast<int16_t>(x1 * 4), static_cast<int16_t>(pp::kScreenH), static_cast<int16_t>(x1),
                static_cast<int16_t>(kGlowH), 0};
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, v);
    }
    // B: glow buffer -> frame, screen blend at glowPercent (vertex-less colour).
    sceGuDrawBufferList(GU_PSM_5551, frame, 512);
    sceGuScissor(0, 0, pp::kScreenW, pp::kScreenH);
    sceGuTexImage(0, kGlowStride, 128, kGlowStride, vram + g_glowOff);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFlush();
    sceGuTexSync();  // the glow buffer was drawn just now (as above)
    const float k =std::clamp(static_cast<float>(glowPercent) / 100.0f, 0.0f, 1.0f);
    sceGuColor(abgrFrom(glm::vec3(k)));
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_ONE_MINUS_OTHER_COLOR, GU_FIX, 0, 0xFFFFFFFFu);
    for (int x = 0; x < pp::kScreenW; x += 32) {
        auto* v = static_cast<TexVert*>(sceGuGetMemory(2 * sizeof(TexVert)));
        const int x1 = std::min(x + 32, pp::kScreenW);
        v[0] = {static_cast<int16_t>(x / 4), 0, static_cast<int16_t>(x), 0, 0};
        v[1] = {static_cast<int16_t>(x1 / 4), static_cast<int16_t>(kGlowH), static_cast<int16_t>(x1),
                static_cast<int16_t>(pp::kScreenH), 0};
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, v);
    }
    sceGuDisable(GU_BLEND);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_DEPTH_TEST);
    pp::textureResetBinding();
}

} // namespace wowee::rendering::psp
