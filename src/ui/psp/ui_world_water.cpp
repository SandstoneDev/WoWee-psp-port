// Transparent, masked liquid pass. No reflection target or per-frame heap allocations.
#include "ui/psp/ui_world_sounds.hpp"
#include "rendering/psp/psp_water_format.hpp"
#include "rendering/psp/psp_terrain.hpp"
#include "rendering/psp/psp_frustum.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_prof.hpp"
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::ui::psp {
namespace rp = wowee::rendering::psp;
namespace pp = wowee::platform::psp;
namespace {
constexpr float tileSize = 533.33333f, chunkSize = tileSize / 16, unit = chunkSize / 8;
struct WaterVertex { float u, v; uint32_t color; float x, y, z; };
struct Candidate { const uint8_t* grid; const rp::WaterPatch* patch; float x, y, distance; int chunk; };
// Main-thread-only scratch. Six cached grids cover four water tiles plus the player's tile.
Candidate candidates[256];
alignas(16) uint16_t ripple[16 * 16];
bool rippleReady = false;
uint32_t liquidColor(uint16_t type) {
    // MH2O stores LiquidType.dbc IDs, whereas legacy MCLQ uses categories.
    // Keep a common water tint until the baker exports a resolved material.
    (void)type;
    return 0x987C5930u;
}
void legacyPatch(const uint8_t* grid, int chunk, rp::WaterPatch& p) {
    p = {};
    p.chunk = static_cast<uint16_t>(chunk);
    const uint8_t* cells = grid + sizeof(rp::SurfaceGridHeader);
    float z = 0;
    std::memcpy(&z, cells + 256 * 32 + chunk * 4, 4);
    if (!rp::waterFinite(z) || std::fabs(z) > 100000) return;
    for (float& h : p.z) h = z;
    for (int i = 0; i < 64; ++i) {
        const int n = chunk * 64 + i;
        if ((cells[n / 2] >> ((n & 1) * 4)) & rp::kSurfCellWater) p.mask[i / 32] |= 1u << (i % 32);
    }
}
}
void WorldSounds::drawWater(const rp::TerrainCamera& cam, uint32_t fogColor, bool enabled) {
    if (!bank_.isOpen() || !enabled) return;
    PROF_SCOPE("water");
    PROF_GE_PASS("water");
    // < half an ADT: at most four intersecting tiles, so the six-slot cache cannot thrash.
    const float distance = std::min(cam.farZ, static_cast<float>(std::clamp(pp::configGetInt("waterview", 200), 40, 240)));
    const auto frustum = rp::makeFrustum(glm::perspective(glm::radians(cam.fovyDeg), 480.0f / 272.0f, cam.nearZ, cam.farZ) *
                                       glm::lookAt(glm::vec3(0), cam.forward, cam.up));
    int count = 0;
    auto offer = [&](const uint8_t* grid, const rp::WaterPatch* patch, int chunk, int tx, int ty) {
        rp::WaterPatch old;
        if (!patch) { legacyPatch(grid, chunk, old); patch = &old; }
        if (!(patch->mask[0] | patch->mask[1])) return;
        const float x = (32 - ty) * tileSize - (chunk / 16) * chunkSize;
        const float y = (32 - tx) * tileSize - (chunk % 16) * chunkSize;
        float lo = patch->z[0], hi = lo;
        for (float z : patch->z) { lo = std::min(lo, z); hi = std::max(hi, z); }
        const glm::vec3 center(x - chunkSize / 2 - cam.pos.x, y - chunkSize / 2 - cam.pos.y, (lo + hi) / 2 - cam.pos.z);
        const float d = glm::length(center), radius = chunkSize * 0.708f + (hi - lo) / 2;
        if (d - radius > distance || frustum.sphereOutside(center, radius)) return;
        Candidate c{grid, patch == &old ? nullptr : patch, x, y, d, chunk};
        if (count < 256) candidates[count++] = c;
        else {
            auto* farthest = std::max_element(candidates, candidates + count, [](const auto& a, const auto& b) { return a.distance < b.distance; });
            if (d < farthest->distance) *farthest = c;
        }
    };
    const int tx0 = static_cast<int>(std::floor(32 - (cam.pos.y + distance) / tileSize));
    const int tx1 = static_cast<int>(std::floor(32 - (cam.pos.y - distance) / tileSize));
    const int ty0 = static_cast<int>(std::floor(32 - (cam.pos.x + distance) / tileSize));
    const int ty1 = static_cast<int>(std::floor(32 - (cam.pos.x - distance) / tileSize));
    for (int tx = tx0; tx <= tx1; ++tx) for (int ty = ty0; ty <= ty1; ++ty) {
        const uint8_t* grid = bank_.surfaceGrid(tx, ty);
        if (!grid) continue;
        unsigned n = 0;
        const auto* patches = rp::waterPatches(grid, n);
        if (patches) for (unsigned i = 0; i < n; ++i) offer(grid, patches + i, patches[i].chunk, tx, ty);
        else for (int c = 0; c < 256; ++c) offer(grid, nullptr, c, tx, ty);
    }
    if (!count) return;
    std::sort(candidates, candidates + count, [](const auto& a, const auto& b) { return a.distance < b.distance; });
    // Prefer nearby water if actors have already filled the list. Keep 96 KB for effects and HUD.
    const uint32_t free = pp::displayListFree();
    const uint32_t budget = free > 96 * 1024 ? std::min<uint32_t>(free - 96 * 1024, 192u * 1024u) : 0;
    uint32_t used = 0;
    int selected = 0;
    for (; selected < count; ++selected) {
        const bool near = candidates[selected].distance < 50.0f;
        const uint32_t cost = near ? 289 * sizeof(WaterVertex) + 1536 * 2 + 512
                                   : 81 * sizeof(WaterVertex) + 384 * 2 + 512;
        if (used + cost > budget) break;
        used += cost;
    }
    count = selected;
    if (!count) return;
    if (!rippleReady) {
        for (int r = 0; r < 16; ++r) for (int c = 0; c < 16; ++c) {
            const unsigned v = 24 + ((r + c * 2) % 8) / 2;
            ripple[r * 16 + c] = static_cast<uint16_t>(v | ((v * 2) << 5) | (v << 11));
        }
        sceKernelDcacheWritebackRange(ripple, sizeof(ripple));
        rippleReady = true;
    }
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE); // transparent surface tests depth without writing it
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
    sceGuTexImage(0, 16, 16, 16, ripple);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexLevelMode(GU_TEXTURE_CONST, 0);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1, 1);
    const float phase = static_cast<float>((sceKernelGetSystemTimeWide() / 1000u) % 32000u) / 32000.0f;
    sceGuTexOffset(phase, phase * 0.5f);
    sceGuShadeModel(GU_SMOOTH);
    rp::lightBegin(cam, fogColor);
    sceGuFog(distance * 0.55f, distance * 0.98f, fogColor);
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    for (int i = count - 1; i >= 0; --i) { // back to front
        const auto& c = candidates[i];
        rp::WaterPatch old;
        const auto* p = c.patch;
        if (!p) { legacyPatch(c.grid, c.chunk, old); p = &old; }
        // 2.08 yd near cells keep diagonal edges below the GE guard-band
        // limit even at the narrowest supported FOV. Far cells use 4.17 yd.
        const int split = c.distance < 50.0f ? 2 : 1, side = 8 * split + 1;
        auto* vertices = static_cast<WaterVertex*>(sceGuGetMemory(side * side * sizeof(WaterVertex)));
        auto* indices = static_cast<uint16_t*>(sceGuGetMemory(384 * split * split * sizeof(uint16_t)));
        const uint32_t color = liquidColor(p->type);
        for (int r = 0; r < side; ++r) for (int col = 0; col < side; ++col) {
            const float rr = static_cast<float>(r) / split, cc = static_cast<float>(col) / split;
            const int ir = std::min(static_cast<int>(rr), 7), ic = std::min(static_cast<int>(cc), 7);
            vertices[r * side + col] = {rr * unit / 8, cc * unit / 8, color,
                c.x - rr * unit - cam.pos.x, c.y - cc * unit - cam.pos.y,
                rp::waterHeight(*p, ir, ic, rr - ir, cc - ic) - cam.pos.z};
        }
        int n = 0;
        for (int r = 0; r < side - 1; ++r) for (int col = 0; col < side - 1; ++col) if (rp::waterCell(*p, r / split, col / split)) {
            const uint16_t a = static_cast<uint16_t>(r * side + col), b = a + side, d = a + 1, e = b + 1;
            for (uint16_t v : {a, b, d, d, b, e}) indices[n++] = v;
        }
        sceGumDrawArray(GU_TRIANGLES, GU_INDEX_16BIT | GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, indices, vertices);
    }
    rp::lightEnd();
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexOffset(0, 0);
    pp::textureResetBinding();
}
} // namespace wowee::ui::psp
