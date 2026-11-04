#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <glm/glm.hpp>
namespace wowee::rendering::psp {
// Visual fallback for packs with a static bobber, triggered only by the server bite.
inline float fishingBobberDip(uint32_t now,uint32_t bite) {
    if(!bite)return 0.f;
    const uint32_t age=now-bite;
    if(age>=900u)return 0.f;
    const float t=age/900.f;
    return -.35f*std::sin(3.14159265f*t);
}
inline glm::vec3 fishingLinePoint(const glm::vec3& tip,const glm::vec3& bobber,float t) {
    const float sag=std::min(.35f,glm::length(bobber-tip)*.012f);
    return tip+(bobber-tip)*t-glm::vec3(0,0,4.f*sag*t*(1.f-t));
}
}
