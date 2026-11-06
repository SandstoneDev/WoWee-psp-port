#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee::rendering::psp {

// A fixed studio face portrait, independent of the gameplay camera and animation.
// Frame around the authored head attachment, not full-body bounds or shoulders.
struct PortraitCamera {
    glm::vec3 centre, eye;
    static constexpr float fovy = 35.0f;
    static constexpr float nearZ = 0.05f, farZ = 60.0f;
};

inline PortraitCamera characterPortraitCamera(float height, glm::vec3 head) {
    const float h = std::isfinite(height) ? std::max(0.8f, height) : 2.0f;
    if (!std::isfinite(head.x) || !std::isfinite(head.y) || !std::isfinite(head.z) ||
        head.z < h * .4f || head.z > h * 1.3f || std::fabs(head.x) > h || std::fabs(head.y) > h)
        head = glm::vec3(0, 0, h * .83f);
    PortraitCamera out;
    out.centre = head - glm::vec3(0, 0, h * .02f);
    const float halfFrame = h * .15f;
    const float distance = halfFrame / std::tan(glm::radians(PortraitCamera::fovy * .5f));
    // Slight three-quarter angle, with a level camera rather than the world orbit.
    out.eye = out.centre + glm::vec3(distance * .978148f, -distance * .207912f, 0);
    return out;
}

} // namespace wowee::rendering::psp
