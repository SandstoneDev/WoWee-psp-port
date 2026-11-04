// WSRF v2 keeps the v1 prefix and appends a sparse liquid mesh table.
#pragma once
#include "rendering/psp/psp_sound_format.hpp"
#include <cmath>
#include <cstring>
namespace wowee::rendering::psp {
struct WaterTable { char magic[4]; uint16_t version, count; }; // WWTR, 1
struct WaterPatch {
    uint16_t chunk, type;
    uint32_t mask[2]; // chunk-wide 8x8, LSB first
    float z[81]; // row*9+col; row decreases server X, col decreases Y
};
static_assert(sizeof(WaterTable) == 8 && sizeof(WaterPatch) == 336);
constexpr uint32_t kMaxWaterPatches = 1024;
constexpr uint32_t kMaxSurfaceBytes = kSurfaceGridBytes + sizeof(WaterTable) + kMaxWaterPatches * sizeof(WaterPatch);
inline bool waterCell(const WaterPatch& p, int row, int col) {
    const unsigned i = static_cast<unsigned>(row * 8 + col);
    return (p.mask[i / 32] & (1u << (i % 32))) != 0;
}
inline bool waterFinite(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return (u & 0x7F800000u) != 0x7F800000u;
}
inline bool surfaceGridValid(const void* blob, uint32_t size) {
    if (!blob || size < kSurfaceGridBytes) return false;
    const auto* b = static_cast<const uint8_t*>(blob);
    SurfaceGridHeader h;
    std::memcpy(&h, b, sizeof(h));
    if (std::memcmp(h.magic, "WSRF", 4)) return false;
    if (h.version == 1) return size == kSurfaceGridBytes;
    if (h.version != 2 || size < kSurfaceGridBytes + sizeof(WaterTable)) return false;
    WaterTable t;
    std::memcpy(&t, b + kSurfaceGridBytes, sizeof(t));
    if (std::memcmp(t.magic, "WWTR", 4) || t.version != 1 || t.count > kMaxWaterPatches ||
        size != kSurfaceGridBytes + sizeof(t) + t.count * sizeof(WaterPatch)) return false;
    const auto* p = reinterpret_cast<const WaterPatch*>(b + kSurfaceGridBytes + sizeof(t));
    for (unsigned i = 0; i < t.count; ++i) {
        if (p[i].chunk >= 256) return false;
        for (float z : p[i].z) if (!waterFinite(z) || std::fabs(z) > 100000.0f) return false;
    }
    return true;
}
inline const WaterPatch* waterPatches(const uint8_t* grid, unsigned& count) {
    if (reinterpret_cast<const SurfaceGridHeader*>(grid)->version != 2) { count = 0; return nullptr; }
    const auto* t = reinterpret_cast<const WaterTable*>(grid + kSurfaceGridBytes);
    count = t->count;
    return reinterpret_cast<const WaterPatch*>(t + 1);
}
// Same diagonal as the renderer: (00,10,01), (01,10,11).
inline float waterHeight(const WaterPatch& p, int row, int col, float r, float c) {
    const float a = p.z[row * 9 + col], b = p.z[(row + 1) * 9 + col];
    const float d = p.z[row * 9 + col + 1], e = p.z[(row + 1) * 9 + col + 1];
    return r + c <= 1.0f ? a + (b - a) * r + (d - a) * c
                         : e + (d - e) * (1.0f - r) + (b - e) * (1.0f - c);
}
} // namespace wowee::rendering::psp
