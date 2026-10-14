// Impostor view choice and cell UVs (forest LOD), shared by the PSP
// renderer (src/rendering/psp/psp_impostors.cpp) and the converter's self
// test (tools/psp/converter/forest_selftest.hpp), so the PC checks the exact
// convention the PSP draws with. Format: psp_model_format.hpp (WIMP).
#pragma once

#include "rendering/psp/psp_model_format.hpp"

#include <cmath>
#include <cstdint>

namespace wowee::rendering::psp {

// atan2 without libm's cost (|error| < 0.01 rad: picks one of 4..16 views).
inline float impostorAtan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx < 1e-20f) return 0.0f;
    const float a = mn / mx;
    const float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0.0f) r = 3.14159274f - r;
    return y < 0.0f ? -r : r;
}

// (tx, ty): world ground-plane direction from the model towards the camera;
// (xx, xy): the placement's model x axis in world (any length). View i was
// baked from model-space direction angle 2*pi*i/views.
inline int impostorView(float tx, float ty, float xx, float xy, int views) {
    const float a = (impostorAtan2(ty, tx) - impostorAtan2(xy, xx)) * (static_cast<float>(views) / 6.28318531f);
    int view = static_cast<int>(std::floor(a + 0.5f)) % views;
    return view < 0 ? view + views : view;
}

// Inner rectangle of a view's cell, u16 texture coordinates (32768 = 1.0 in
// GE 3D mode with sceGuTexScale 1); v0 is the top (zMax).
struct ImpostorCellUv {
    uint16_t u0, v0, u1, v1;
};

inline ImpostorCellUv impostorCellUv(const ImpostorHeader& h, int view) {
    const uint32_t rows = (static_cast<uint32_t>(h.views) + h.cols - 1) / h.cols;
    const uint32_t texW = static_cast<uint32_t>(h.cols) * h.cellSize, texH = rows * h.cellSize;
    const uint32_t cx = static_cast<uint32_t>(view % h.cols) * h.cellSize;
    const uint32_t cy = static_cast<uint32_t>(view / h.cols) * h.cellSize;
    return ImpostorCellUv{static_cast<uint16_t>(((cx + h.margin) << 15) / texW),
                          static_cast<uint16_t>(((cy + h.margin) << 15) / texH),
                          static_cast<uint16_t>(((cx + h.cellSize - h.margin) << 15) / texW),
                          static_cast<uint16_t>(((cy + h.cellSize - h.margin) << 15) / texH)};
}

} // namespace wowee::rendering::psp
