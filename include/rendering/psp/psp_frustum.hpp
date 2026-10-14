// View frustum for CPU culling on the PSP (camera-relative space).
#pragma once

#include <glm/glm.hpp>

namespace wowee::rendering::psp {

// 6 planes (normal.xyz, d), normals pointing inwards.
struct Frustum {
    glm::vec4 planes[6];
    bool sphereOutside(const glm::vec3& c, float r) const {
        for (const auto& p : planes)
            if (glm::dot(glm::vec3(p), c) + p.w < -r) return true;
        return false;
    }
};

// Gribb-Hartmann extraction from projection * view.
inline Frustum makeFrustum(const glm::mat4& vp) {
    Frustum f;
    const glm::mat4 m = glm::transpose(vp);
    f.planes[0] = m[3] + m[0];
    f.planes[1] = m[3] - m[0];
    f.planes[2] = m[3] + m[1];
    f.planes[3] = m[3] - m[1];
    f.planes[4] = m[3] + m[2];
    f.planes[5] = m[3] - m[2];
    for (auto& p : f.planes) p /= glm::length(glm::vec3(p));
    return f;
}

} // namespace wowee::rendering::psp
