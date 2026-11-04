#pragma once
#include "rendering/psp/psp_collision_bounds_index.hpp"
#include "rendering/psp/psp_collision_stream_policy.hpp"
namespace wowee::rendering::psp {
// Planning and readiness must use exactly the same neighborhood. The callback
// receives the chunk id and squared local XY distance (no square root in holds).
template<class F> void collisionNeighborhood(const CollisionBoundsIndex& index,const glm::vec3& p,
                                             float scale,float radius,F&& visit) {
    const float r=(radius+.001f)/scale;
    index.query(p-glm::vec3(r,r,8.001f/scale),p+glm::vec3(r,r,3.001f/scale),
        [&](const CollisionBoundsIndex::Entry& e) {
            const float dx=std::max({e.lo.x-p.x,p.x-e.hi.x,0.f});
            const float dy=std::max({e.lo.y-p.y,p.y-e.hi.y,0.f});
            if(collisionStreamNearby(dx,dy,e.lo.z,e.hi.z,p.z,scale,radius)) visit(e.id,dx*dx+dy*dy);
        });
}
}
