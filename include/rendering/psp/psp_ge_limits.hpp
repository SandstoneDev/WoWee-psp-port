// The GE's screen-space limits as numbers - for the bakers (how long a triangle edge may
// be near the camera) and the renderers (which mesh to draw near the camera). Pure math, no PSP headers: the host
// test tools/psp/host_tests/ge_limits_test.cpp runs this very header against a software model of the GE's near clip
// and discard rules.
//
// What the hardware does (Sony, not ours):
//  - Graphics Engine Users Manual sec. 10 "Clipping" p58-60: with clipping enabled (CMD_CLE, GU_CLIP_PLANES) the GE
//    clips only against the near plane; a primitive with ANY vertex outside 0 <= Xs, Ys < 4096 after the viewport
//    transform is discarded whole - "you must divide the primitives in advance".
//  - Hardware-Restriction p16: a line or triangle whose vertices differ by >= 0xAAAA (2730.625 px, 12.4 fixed
//    point) in screen Y draws garbage (PPSSPP "GE bit-exact", 2026: extra pixels along the long edge, one per
//    4-pixel span, as if a 17-bit field overflowed). X and Z are not affected.
// Our viewport (psp_display.cpp): centre 2048 x 2048, 480 x 272, square pixels - the window reaches 2048 px each
// way from the screen centre.
#pragma once

#include <cmath>

namespace wowee::rendering::psp::ge {

constexpr float kCentre = 2048.0f;      // sceGuViewport(2048, 2048, ...)
constexpr float kReach = 2047.0f;       // |Xs - 2048|, |Ys - 2048| that stay inside [0, 4096) (Q12.4 margin)
constexpr float kMaxSpanY = 2730.625f;  // Hardware-Restriction p16: 0xAAAA in 12.4

// The world camera's default vertical field of view in degrees (TerrainCamera::fovyDeg, cfg fov 90); the horizontal
// one follows the aspect. One constant for the PSP renderers and the PC tools that project like them. The value
// awaits observation (round 2: camera_fov).
constexpr float kWorldFovYDeg = 54.0f;

// Focal length in pixels: screen pixels per unit of x/z (and y/z) for a vertical field of view over a viewport
// `heightPx` tall (the world: 272 at kWorldFovYDeg).
inline float focalPx(float fovyDeg, float heightPx = 272.0f) {
    return heightPx * 0.5f / std::tan(fovyDeg * (3.14159265f / 360.0f));
}

// The longest triangle edge, in units of the near plane distance, that is safe anywhere: if any part of the
// triangle is on the screen, no vertex of it (after the near clip) leaves the 4096 window and its height stays
// under kMaxSpanY. Worst case (host test): a visible point p on the screen's edge, at depth near + t, and a vertex
// clipped at the near plane beside it - its x/z reaches half/f + L*sqrt(1 + (half/f)^2)/near, so
//     L/near <= (2047 - half) / sqrt(f^2 + half^2)        for half = 240 (x) and 136 (y).
// fovy 54 (cfg fov 90): 5.03;  fovy 30 (cfg fov 50, the narrowest): 3.22;  fovy 72 (cfg fov 120): 5.94.
// The height rule never binds first here (an edge this short spans < 1400 px).
inline float safeEdgePerNear(float fovyDeg, float widthPx = 480.0f, float heightPx = 272.0f) {
    const float f = focalPx(fovyDeg, heightPx);
    const float hx = widthPx * 0.5f, hy = heightPx * 0.5f;
    const float lx = (kReach - hx) / std::sqrt(f * f + hx * hx);
    const float ly = (kReach - hy) / std::sqrt(f * f + hy * hy);
    return lx < ly ? lx : ly;
}

// A bounding sphere (camera space: x right, y up, z = depth along the view direction; radius r) whose every
// triangle is safe whatever its size: wholly in front of the near plane (no clipping), every point's x/z and y/z
// inside the window, and any two points' screen Y closer than kMaxSpanY (all within +-kMaxSpanY/2 of the centre).
// Conservative: false means "may be unsafe" - draw the mesh split for the near plane (kModelNearSplit LOD 0).
inline bool sphereSafe(float cx, float cy, float cz, float r, float nearZ, float focal) {
    const float zMin = cz - r;
    if (!(zMin >= nearZ)) return false;  // part of it may cross the near plane (or NaN)
    const float ax = std::fabs(cx) + r, ay = std::fabs(cy) + r;
    return ax * focal <= kReach * zMin && ay * focal <= (kMaxSpanY * 0.5f) * zMin;
}

}  // namespace wowee::rendering::psp::ge
