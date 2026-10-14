// The sky gradient's math, apart from the GE code
// (psp_lighting_ge.cpp draws it), so the host test (tools/psp/host_tests/
// sky_test.cpp) runs the very same functions on the PC.
//
// The sky is upstream's
// skybox.frag evaluated per vertex of a screen grid and Gouraud-shaded by the
// GE between the vertices. Two grids, the same 35 rows (every 8 px):
//   legacy (wowee.cfg skyfast=0): 9 vertex columns every 60 px, drawn as 34 row
//     strips (one sceGuDrawArray per row);
//   fast   (skyfast=1): 9 vertex columns on the 64-px grid (0, 64, ... 448, 480),
//     drawn as 8 column strips. PSP: the GE's eDRAM in graphics mode keeps a
//     rectangle of the frame buffer per page (GE Users Manual §22.3.2, p116-117);
//     a 64-px (128-byte) column is the unit libgu's GU_FAST_CLEAR_BIT clears in,
//     and a 64 x 8 cell no longer straddles two of them as a 60-px one does.
#pragma once

#include "platform/psp/psp_display.hpp"  // kScreenW / kScreenH
#include "platform/psp/psp_vmath.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wowee::rendering::psp {

// skyTop, skyMiddle, skyBand1, skyBand2 of the light bands; the sun direction.
struct SkyColors {
    glm::vec3 zenith, mid, horizon, fringe;  // skyTop, skyMiddle, skyBand1, skyBand2
    glm::vec3 sunDir;                        // toward the sun (-directionalDir)
};

// 0xAABBGGRR from 0..1 RGB (clamped).
inline uint32_t abgrFrom(const glm::vec3& c, float alpha = 1.0f) {
    auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(alpha) << 24) | (q(c.b) << 16) | (q(c.g) << 8) | q(c.r);
}

// Upstream assets/shaders/skybox.frag.glsl, per vertex. PSP: pow() and
// exp() by multiplication (the soft terms only; the sharp pow(,64) sun disk
// needs per-pixel work and is left out), no volumetric fog.
inline glm::vec3 skyColorAt(const SkyColors& s, const glm::vec3& worldDir) {
    const float elev = worldDir.z;
    const float e = std::clamp(elev, 0.0f, 1.0f);
    glm::vec3 sky;
    if (e > 0.4f) {
        sky = glm::mix(s.mid, s.zenith, (e - 0.4f) / 0.6f);
    } else if (e > 0.05f) {
        sky = glm::mix(s.horizon, s.mid, (e - 0.05f) / 0.35f);
    } else {
        sky = glm::mix(s.fringe, s.horizon, e / 0.05f);
    }
    if (elev < 0.0f) {
        const float nadirFade = std::clamp(-elev * 3.0f, 0.0f, 1.0f);
        sky = glm::mix(s.fringe, s.fringe * 0.3f, nadirFade);
    }
    const float sunDot = std::max(glm::dot(worldDir, s.sunDir), 0.0f);
    const float sunAbove = std::clamp(s.sunDir.z, 0.0f, 1.0f);
    const float oneMinus = 1.0f - e;
    const float rayleigh = oneMinus * oneMinus * oneMinus * 0.15f;
    const glm::vec3 scatter = glm::mix(glm::vec3(0.8f, 0.45f, 0.15f), glm::vec3(0.3f, 0.5f, 1.0f), e);
    sky += scatter * (rayleigh * sunDot * sunAbove);
    float d2 = sunDot * sunDot;
    d2 *= d2;
    const float mieSoft = d2 * d2 * 0.1f;  // pow(sunDot, 8) * 0.1
    const glm::vec3 glow = glm::mix(glm::vec3(1.0f, 0.85f, 0.55f), glm::vec3(1.0f, 1.0f, 0.95f), e);
    sky += glow * (mieSoft * sunAbove);
    float h = oneMinus * oneMinus;  // exp(-12 e) ~ (1 - e)^12 near the horizon
    h *= h;
    const float haze = h * h * h * 0.06f;
    sky += s.horizon * (haze * sunAbove);
    if (s.sunDir.z < 0.0f) {
        const float moon = std::clamp(-s.sunDir.z * 0.5f, 0.0f, 0.15f);
        sky += glm::vec3(0.02f, 0.03f, 0.08f) * moon;
    }
    return sky;
}

constexpr int kSkyRows = 35;  // vertex rows every 8 px: 0, 8, ... 272
constexpr int kSkyCols = 9;   // vertex columns (both grids)

inline int skyRowY(int row) { return std::min(row * 8, platform::psp::kScreenH); }
inline int skyColumnX(bool fast, int col) {
    return fast ? std::min(col * 64, platform::psp::kScreenW) : col * 60;
}

// The strips' vertices, in the order handed to the GE (GU_TRIANGLE_STRIP):
// which grid vertex (row, col) sits at strip position i. Both split every cell
// on its top-right / bottom-left diagonal, so they shade alike.
//   legacy: row strip `strip` (0..33), i = 0..17: (col i/2, row strip + i%2);
//   fast:   column strip `strip` (0..7), i = 0..69: (col strip + i%2, row i/2).
struct SkyStripVertex {
    int row, col;
};
constexpr int kSkyLegacyStrips = kSkyRows - 1, kSkyLegacyStripVerts = kSkyCols * 2;
constexpr int kSkyFastStrips = kSkyCols - 1, kSkyFastStripVerts = kSkyRows * 2;
inline SkyStripVertex skyStripVertex(bool fast, int strip, int i) {
    return fast ? SkyStripVertex{i >> 1, strip + (i & 1)} : SkyStripVertex{strip + (i & 1), i >> 1};
}

// The camera basis the sky has always used (drawSky).
struct SkyView {
    glm::vec3 f, r, u;
    float tanX = 0.0f, tanY = 0.0f;
};

// false: a degenerate camera (no forward, or looking straight along `up`).
inline bool skyViewFrom(const glm::vec3& forward, const glm::vec3& up, float fovyDeg, SkyView& v) {
    const float aspect = static_cast<float>(platform::psp::kScreenW) / platform::psp::kScreenH;
    v.tanY = std::tan(fovyDeg * 0.5f * 3.14159265f / 180.0f);
    v.tanX = v.tanY * aspect;
    v.f = forward;
    const float fl = glm::dot(v.f, v.f);
    if (!(fl > 1e-8f)) return false;
    v.f *= 1.0f / std::sqrt(fl);
    v.r = glm::cross(v.f, up);
    const float rl = glm::dot(v.r, v.r);
    if (!(rl > 1e-8f)) return false;  // looking straight up/down
    v.r *= 1.0f / std::sqrt(rl);
    v.u = glm::cross(v.r, v.f);
    return true;
}

// The world direction through screen pixel position (sx, sy).
inline glm::vec3 skyDirAt(const SkyView& v, float sx, float sy) {
    const float ndcY = 1.0f - sy / (platform::psp::kScreenH * 0.5f);
    const float ndcX = sx / (platform::psp::kScreenW * 0.5f) - 1.0f;
    glm::vec3 d = v.f + v.r * (ndcX * v.tanX) + v.u * (ndcY * v.tanY);
    d *= platform::psp::vmath::rsqrt(glm::dot(d, d));  // 315 per computed grid: vrsq
    return d;
}

// ABGR of every vertex of a grid, [row][col].
inline void skyGridColors(const SkyView& v, const SkyColors& s, bool fast, uint32_t (&out)[kSkyRows][kSkyCols]) {
    for (int row = 0; row < kSkyRows; ++row) {
        const float sy = static_cast<float>(skyRowY(row));
        for (int col = 0; col < kSkyCols; ++col) {
            out[row][col] = abgrFrom(skyColorAt(s, skyDirAt(v, static_cast<float>(skyColumnX(fast, col)), sy)));
        }
    }
}

// Cache key of a computed grid (skyfast=1 reuses the grid while it holds):
// the inputs quantised far below what the 8-bit vertex colours (and the 5-bit
// 5551 frame) can show - colours to 1/1024, directions to 1/8192 (0.007 deg,
// 0.04 px), the field of view to 1/16 deg. A camera standing still or moving
// straight, under a light changing slower than that, keeps it.
struct SkyGridKey {
    int32_t q[24] = {};
};

inline bool operator==(const SkyGridKey& a, const SkyGridKey& b) { return std::memcmp(a.q, b.q, sizeof(a.q)) == 0; }

inline SkyGridKey skyGridKey(const glm::vec3& forward, const glm::vec3& up, float fovyDeg, const SkyColors& s,
                             bool fast) {
    SkyGridKey k;
    int n = 0;
    auto put = [&](float v, float scale) {
        const float x = v * scale;
        k.q[n++] = static_cast<int32_t>(x >= 0.0f ? x + 0.5f : x - 0.5f);
    };
    auto put3 = [&](const glm::vec3& v, float scale) {
        put(v.x, scale);
        put(v.y, scale);
        put(v.z, scale);
    };
    put3(forward, 8192.0f);
    put3(up, 8192.0f);
    put(fovyDeg, 16.0f);
    put3(s.zenith, 1024.0f);
    put3(s.mid, 1024.0f);
    put3(s.horizon, 1024.0f);
    put3(s.fringe, 1024.0f);
    put3(s.sunDir, 8192.0f);
    k.q[n++] = fast ? 1 : 0;
    return k;
}

} // namespace wowee::rendering::psp
