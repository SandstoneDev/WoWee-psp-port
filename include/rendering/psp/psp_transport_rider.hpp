#pragma once

#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>

namespace wowee::rendering::psp {

inline glm::vec3 transportRiderWorld(const glm::vec3& position, float yaw, const glm::vec3& local) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return position + glm::vec3(c*local.x-s*local.y, s*local.x+c*local.y, local.z);
}

inline bool transportDeckContact(bool grounded, float feetZ, float floorZ) {
    // Contact is evaluated against the previous frame BEFORE transport motion.
    // This tolerates collision/position quantization without capturing a fall.
    return grounded && std::isfinite(feetZ) && std::isfinite(floorZ) && std::fabs(feetZ-floorZ) <= 0.25f;
}

inline uint32_t transportPartId(uint64_t guid, uint32_t part, uint32_t flip = 0) {
    const uint32_t salt = static_cast<uint32_t>(guid) * uint32_t{2654435761u} ^ static_cast<uint32_t>(guid >> 32);
    return ((salt + part * uint32_t{40503u}) | uint32_t{0x40000000u}) ^ flip;
}

inline bool transportOwnsPart(uint64_t guid, uint32_t parts, uint32_t flip, uint32_t placementId) {
    for (uint32_t part = 0; part < parts; ++part)
        if (transportPartId(guid,part,flip) == placementId) return true;
    return false;
}

}  // namespace wowee::rendering::psp
