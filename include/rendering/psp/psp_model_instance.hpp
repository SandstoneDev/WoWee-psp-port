#pragma once
#include "rendering/psp/psp_model_format.hpp"
#include <glm/glm.hpp>
#include <cstdint>

namespace wowee::rendering::psp {
struct ModelCollisionTransform {
    glm::mat3 inverse{1.0f};
    glm::vec3 collisionLo{0.0f},collisionHi{0.0f};
};
// Render-only placements (notably grass) need no 60-byte collision transform.
// Index + 1, rather than a pointer, survives collision-vector reallocation.
template<class Model> struct ModelInstance {
    float x,y,z,r,reach;
    const DoodadPlacement* p;
    const Model* m;
    uint32_t collision=0;
};
}
