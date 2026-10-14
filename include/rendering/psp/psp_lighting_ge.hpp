// Lighting on the GE - fog, light tint, sky gradient, glow.
//
// The world keeps its baked vertex light (terrain, doodads, WMO:
// 0.5 + 0.5 N.sun of a fixed sun, WMO MOCV); what changes with the hour and
// the zone is applied over it here, per frame, without new vertex formats:
//   - fog: colour and start/end from the bands (upstream LightingManager);
//   - tint: the baked colour times (ambient + diffuse x sun) / (the baked
//     reference), as the GE's global ambient light over the vertex colour
//     (GU_LIGHTING with no lights, sceGuColorMaterial(GU_AMBIENT)) - or, as a
//     fallback, one full-screen multiply after the 3D (geometry only);
//   - sky: a screen-space grid coloured by upstream's skybox.frag gradient;
//   - glow: quarter-resolution copy of the frame added back "screen" style.
// The renderers call lightBegin/lightEnd where they set their fog (one line
// each); the world screen fills frameLight() per frame
// (src/ui/psp/ui_world_lighting.cpp).
#pragma once

#include "rendering/lighting_manager.hpp"  // upstream LightingParams
#include "rendering/psp/psp_sky_grid.hpp"  // SkyColors, skyColorAt, abgrFrom (host-testable)
#include "rendering/psp/psp_terrain.hpp"   // TerrainCamera

#include <cstdint>

namespace wowee::rendering::psp {

enum LightTintMode : int {
    kTintOff = 0,
    kTintGeAmbient = 1,  // GE ambient light over the vertex colour (no extra pass)
    kTintScreen = 2,     // one full-screen multiply after the 3D, sky excluded by depth
};

struct FrameLight {
    bool fogBands = false;             // fog from the bands; false = the caller's colour at 0.55..0.98 far
    uint32_t fogABGR = 0xFFE0B080u;
    float fogStart = 250.0f, fogEnd = 440.0f;  // yards, already fitted to the far plane
    int tint = kTintOff;
    uint32_t ambientABGR = 0xFFFFFFFFu;  // kTintGeAmbient: global ambient (= tint, or tint / 2 with twoX)
    bool twoX = false;                   // GU_FRAGMENT_2X: tints above 1.0 (untextured draws come out half)
    uint32_t screenABGR = 0xFFFFFFFFu;   // kTintScreen: the multiplier
};

FrameLight& frameLight();

// In place of `sceGuFog(far * 0.55, far * 0.98, fogColor); sceGuEnable(GU_FOG)`.
void lightBegin(const TerrainCamera& cam, uint32_t fogColorABGR);
// In place of `sceGuDisable(GU_FOG)`: fog, and the tint's GE state, off.
void lightEnd();
// cfg dither3d - the GE's 4 x 4 ordered dither (the sky's matrix) on every lit 3D
// pass, lightBegin .. lightEnd (terrain, models, creatures, characters). The 5551 frame keeps 5 bits a channel:
// the fog's long gradients and the baked vertex light show as bands (32 steps); dithered they read as the PC's
// smooth ramps. 0 (default) = undithered, as before; an A/B option for the PSP-3000.
void setDither3d(bool on);
bool dither3d();
// Per batch (ModelRenderer::setBatchState): an unlit material (WMO 0x01)
// keeps its baked colour, as upstream's shaders skip the light for it.
void lightBatch(bool unlit);

// Sky: upstream skybox.frag (3-band gradient, below-horizon darkening,
// Rayleigh-like warm scatter, soft Mie glow, horizon haze, moonlight) per
// vertex of a 9 x 35 screen grid (psp_sky_grid.hpp), drawn first with no depth.
SkyColors skyColorsFrom(const LightingParams& p);
void drawSky(const TerrainCamera& cam, const SkyColors& s);  // skyfast=0: the legacy grid

// skyfast=1: the same gradient as one pass that
// also lays the frame's depth (z = 0, what the clear wrote), so the world's
// frames skip frameBegin's colour + depth clear (psp_display frameSkipNextClear):
// one whole-screen pass less. 8 column strips on the 64-px eDRAM grid (34 row
// strips before), the grid's colours kept while the camera and the light hold.
//   skyFastPrepare - CPU: the strips (recomputed only when the key changes);
//                    false = degenerate camera, nothing to draw;
//   skyFastDraw    - GE: the strips, colour + depth (depth test ALWAYS);
//   skyClear       - GE: colour + depth in one fast clear (64-px strips), for
//                    the flat sky (sky=0) and the fallbacks;
//   skyBench       - GE, wowee.cfg skybench: `passes` extra whole-screen passes
//                    of one kind (1 the skyfast gradient, 2 the old frame clear,
//                    3 the fast clear) in the GE pass "sky.bench" - the
//                    hardware price of one pass = that pass / `passes`.
bool skyFastPrepare(const TerrainCamera& cam, const SkyColors& s);
void skyFastDraw();
void skyClear(uint32_t abgr);
void skyBench(int mode, int passes);
struct SkyCacheStats {
    uint32_t hits = 0, misses = 0;  // frames that reused / recomputed the grid
};
SkyCacheStats skyCacheStatsTake();  // since the last call

// After the 3D, before the HUD: the screen tint (kTintScreen) and the glow.
// glowPercent 0 = off; needs glowReserve() at world entry (before the models).
bool glowReserve();
bool glowAvailable();
void drawPost(int glowPercent);

// abgrFrom (0xAABBGGRR from 0..1 RGB): psp_sky_grid.hpp.

} // namespace wowee::rendering::psp
