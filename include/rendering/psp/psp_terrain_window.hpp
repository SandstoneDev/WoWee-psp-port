// Memory diet: the terrain's chunk-texture window - pure logic, no
// PSP headers (tools/psp/host_tests/terrain_window_host_test.sh runs it on the PC).
//
// PSP: a WTER tile is 1.6 MB, 1.5 MB of it the 256 chunk textures (CLUT + 64x64 T8 + mips). The GE draws a chunk only
// while its bounding sphere is nearer than the far plane (TerrainCamera::farZ, 450 yd from the camera; the fog ends at
// 0.98 of it), so of the 2304 chunks of the 3x3 window only the ~550-870 within ~450-530 yd of the player ever reach
// the screen. The window keeps the textures of the chunks whose sphere is within keepYd of the player (far plane +
// camera distance + a lead for the reads) and frees those past dropYd (hysteresis); the tile itself (vertex block,
// heights, chunk records) stays whole - heightAt / chunkAt / collision do not change. The original client keeps every
// texture of its loaded ADTs (a 2-4 GB address space); it has no such window.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wowee::rendering::psp {

// WTER layout (psp_terrain.hpp): the 32-byte header, 256 chunk records of 340 bytes, then (v2+, textured) 256 texture
// blocks, then (v4) 256 u16 detail ids.
constexpr uint32_t kTerrainHeaderBytes = 32;
constexpr uint32_t kTerrainChunkRecordBytes = 340;
constexpr uint32_t kTerrainHeadBytes = kTerrainHeaderBytes + 256u * kTerrainChunkRecordBytes;  // 87 072
constexpr uint32_t kTerrainDetailTableBytes = 512;

struct TerrainLayout {
    uint16_t version = 0;
    bool textured = false;
    uint32_t texBase = kTerrainHeadBytes;  // first texture block (inside the entry)
    uint32_t texBytes = 0;                 // one chunk's block: 4608 (v2) or 5888 (v3+: 32x32 + 16x16 mips)
    uint32_t detailOffset = 0;             // v4: the detail id table (inside the entry), 0 = none
};

// The layout from the first bytes of a WTER entry (at least the 32-byte header) and the entry's size; false when it is
// no WTER blob of version 1..4 with 256 chunks or the entry is too short for it (loadTile logs wter_bad).
inline bool terrainLayout(const uint8_t* head, uint32_t headSize, uint32_t entrySize, TerrainLayout& out) {
    out = TerrainLayout{};
    if (!head || headSize < kTerrainHeaderBytes || entrySize < kTerrainHeadBytes) return false;
    if (std::memcmp(head, "WTER", 4) != 0) return false;
    uint16_t version = 0, flags = 0;
    uint32_t chunks = 0;
    std::memcpy(&version, head + 4, 2);
    std::memcpy(&flags, head + 6, 2);
    std::memcpy(&chunks, head + 12, 4);
    if (version < 1 || version > 4 || chunks != 256) return false;
    out.version = version;
    out.textured = version >= 2 && (flags & 1u) != 0;
    out.texBytes = out.textured ? (version >= 3 ? 512u + 4096u + 1024u + 256u : 512u + 4096u) : 0u;
    const uint32_t texEnd = out.texBase + 256u * out.texBytes;
    if (out.textured && version >= 4) out.detailOffset = texEnd;
    const uint32_t need = texEnd + (out.detailOffset ? kTerrainDetailTableBytes : 0u);
    return entrySize >= need;
}

// Chunk record i's sphere as TerrainRenderer::draw culls it: centre (baseX - 4u, baseY - 4u), radius 4u * sqrt 2 + half
// the height range (u: the header's unit size, f32 at +16; heights s16 1/32 yd at +16 of the record).
inline void terrainChunkSphere(const uint8_t* record, float unit, float& cx, float& cy, float& radius) {
    float baseX = 0.0f, baseY = 0.0f;
    std::memcpy(&baseX, record + 0, 4);
    std::memcpy(&baseY, record + 4, 4);
    int16_t hmin = 32767, hmax = -32768;
    for (int k = 0; k < 81; ++k) {
        int16_t h = 0;
        std::memcpy(&h, record + 16 + k * 2, 2);
        hmin = std::min(hmin, h);
        hmax = std::max(hmax, h);
    }
    const float half = unit * 4.0f;
    cx = baseX - half;
    cy = baseY - half;
    radius = half * 1.4143f + static_cast<float>(hmax - hmin) / 32.0f * 0.5f;
}

struct TexWindowRule {
    float keepYd = 530.0f;  // a chunk whose sphere is nearer than this (2D, from the player) wants its texture
    float dropYd = 578.0f;  // past this its texture is freed (keepYd .. dropYd: left as it is)
};

// Far plane 450 yd + the camera's distance behind the player (camera zoom up to 15 yd) + `leadYd` for the reads
// (wowee.cfg terraintexyd overrides keepYd; terraintexdrop the band).
inline TexWindowRule texWindowRule(float farZ, float leadYd, float bandYd) {
    TexWindowRule r;
    r.keepYd = farZ + 15.0f + std::max(0.0f, leadYd);
    r.dropYd = r.keepYd + std::max(8.0f, bandYd);
    return r;
}

// 1: wants its texture; -1: its texture goes; 0: leave it. (cx, cy, radius): the chunk's sphere as the draw culls it.
inline int texWindowWant(float cx, float cy, float radius, float px, float py, const TexWindowRule& w) {
    const float dx = cx - px, dy = cy - py;
    const float d2 = dx * dx + dy * dy;
    const float keep = w.keepYd + radius, drop = w.dropYd + radius;
    if (d2 < keep * keep) return 1;
    if (d2 > drop * drop) return -1;
    return 0;
}

struct TexRun {
    uint16_t first = 0, count = 0;  // chunk indices first .. first + count - 1 (one chunk row: same index / 16)
    float d2 = 0.0f;                // the nearest chunk's squared distance (nearest runs first)
};

// Runs of chunks that want a texture and have none (`need[i]` != 0), consecutive in the entry: one chunk row (16
// chunks) at most, so one read of count * texBytes. d2[i]: chunk i's squared distance from the player. Appends to out.
inline void texWindowRuns(const uint8_t* need, const float* d2, std::vector<TexRun>& out) {
    for (uint32_t row = 0; row < 16; ++row) {
        uint32_t c = 0;
        while (c < 16) {
            const uint32_t i = row * 16u + c;
            if (!need[i]) {
                ++c;
                continue;
            }
            TexRun r;
            r.first = static_cast<uint16_t>(i);
            r.d2 = d2[i];
            while (c < 16 && need[row * 16u + c]) {
                r.d2 = std::min(r.d2, d2[row * 16u + c]);
                ++r.count;
                ++c;
            }
            out.push_back(r);
        }
    }
}

inline void texSortRuns(std::vector<TexRun>& runs) {
    std::stable_sort(runs.begin(), runs.end(), [](const TexRun& a, const TexRun& b) { return a.d2 < b.d2; });
}

}  // namespace wowee::rendering::psp
