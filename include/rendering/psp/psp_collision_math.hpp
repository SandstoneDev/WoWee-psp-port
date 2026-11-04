#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
namespace wowee::rendering::psp {
// Edge tolerance prevents shared-edge cracks; local differences retain precision.
inline float collisionRayTri(const glm::vec3& o, const glm::vec3& d,
                             const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 ab = b - a, ac = c - a, pv = glm::cross(d, ac);
    const float det = glm::dot(ab, pv);
    if (std::fabs(det) < 1e-9f) return -1.0f;
    const float inv = 1.0f / det;
    const glm::vec3 tv = o - a, qv = glm::cross(tv, ab);
    const float u = glm::dot(tv, pv) * inv, v = glm::dot(d, qv) * inv;
    constexpr float eps = 1e-5f;
    if (u < -eps || v < -eps || u > 1.0f + eps || u + v > 1.0f + eps) return -1.0f;
    const float t = glm::dot(ac, qv) * inv;
    return t >= -1e-4f ? std::max(t, 0.0f) : -1.0f;
}
// A standing player's feet have area. Quantized neighboring chunks can leave
// centimetre-wide seams that a single zero-width ray misses. Admit support at
// triangle edges within a small foot radius, without filling real ledges.
inline float collisionFloorRayTri(const glm::vec3& o,const glm::vec3& d,
                                  const glm::vec3& a,const glm::vec3& b,const glm::vec3& c,float radius){
    const float direct=collisionRayTri(o,d,a,b,c);
    if(direct>=0.f || radius<=0.f)return direct;
    const float d2=glm::dot(d,d);
    if(d2<1e-12f)return -1.f;
    const glm::vec3 vertices[]={a,b,c};float best=-1.f;
    for(int i=0;i<3;++i){
        const glm::vec3 start=vertices[i]-o,edge=vertices[(i+1)%3]-vertices[i];
        const glm::vec3 flat=start-d*(glm::dot(start,d)/d2);
        const glm::vec3 along=edge-d*(glm::dot(edge,d)/d2);
        const float length2=glm::dot(along,along);
        const float u=length2>1e-12f?std::clamp(-glm::dot(flat,along)/length2,0.f,1.f):0.f;
        const glm::vec3 lateral=flat+along*u;
        if(glm::dot(lateral,lateral)>radius*radius)continue;
        const float hit=glm::dot(start+edge*u,d)/d2;
        if(hit>=-1e-4f && (best<0 || hit<best))best=std::max(hit,0.f);
    }
    return best;
}
inline bool collisionWalkable(const glm::vec3& n) {
    const float n2 = glm::dot(n, n);
    return n2 > 1e-18f && n.z * n.z >= 0.4131759f * n2; // cos(50)^2
}
struct MovementSlices { int count; float dt; };
// A falling player's horizontal motion can cross an uphill floor even when the
// endpoint is above the old feet height. Cover the walkable slope over this
// substep, rather than firing the floor ray from below the ramp. Stationary
// falls keep the original small vertical tolerance; never reach a whole storey.
inline float landingFloorProbe(float beforeZ,float nextZ,float dx,float dy) {
    const float rise=std::min(0.6f,std::sqrt(dx*dx+dy*dy)*1.1918f);
    return std::max(beforeZ,nextZ)+rise+0.05f;
}
inline MovementSlices movementSlices(float dt, float speed) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    speed = std::fabs(speed);
    // At the CPU cap reduce simulated time, never enlarge spatial steps.
    if (speed > 0.0f) dt = std::min(dt, 16.0f / speed);
    const int n = std::clamp(static_cast<int>(std::ceil(std::max(dt * 60.0f, dt * speed / 0.25f))), 1, 64);
    return {n, dt / n};
}
} // namespace wowee::rendering::psp
