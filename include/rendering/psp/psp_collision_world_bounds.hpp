#pragma once
#include <glm/glm.hpp>
#include <algorithm>
namespace wowee::rendering::psp {
// Collision bounds, rather than the visual mesh's possibly smaller sphere.
// Valid for rotation, reflection, nonuniform scale and shear.
inline void collisionWorldBounds(const glm::vec3& lo,const glm::vec3& hi,
                                 const glm::mat3& axes,const glm::vec3& pos,
                                 glm::vec3& outLo,glm::vec3& outHi) {
    outLo=outHi=pos;
    for(int a=0;a<3;++a) {
        const glm::vec3 x=axes[a]*lo[a], y=axes[a]*hi[a];
        outLo+=glm::min(x,y); outHi+=glm::max(x,y);
    }
    // Preserve shared-edge tolerance used by collisionRayTri.
    outLo-=glm::vec3(.002f); outHi+=glm::vec3(.002f);
}
}
