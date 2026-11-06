#pragma once
#include <algorithm>
#include <cstdint>

namespace wowee::rendering::psp {
// Baked meshes have no normals. At the horizon, using only the flat-ground
// sun angle drops all directional light even on vertical surfaces. Tirisfal's
// evening DBC colors are already dim; retain a modest diffuse fill there.
inline float outdoorDiffuseWeight(uint32_t zone, float sunZ, float moonGain) {
    const float direct = sunZ >= 0.f ? sunZ : -sunZ * moonGain;
    if (zone != 85u) return direct;
    const float night = std::clamp(-sunZ * 5.f, 0.f, 1.f);
    const float fill = .45f * (1.f - night * (1.f - moonGain));
    return std::max(direct, fill);
}
}
