#pragma once
#include <cstdint>
namespace wowee::rendering::psp {
inline bool collisionDoodadsReady(bool tileKnown,bool geometryInstalled,bool required,bool loading) {
    if (tileKnown && geometryInstalled) return true; // top-up reads cannot invalidate resident collision
    return tileKnown && !loading && !required;
}
// Descriptor pressure limits optional scenery, never overlapping collision owners.
inline bool collisionOwnerOpenAllowed(bool tight,uint32_t openCount,uint32_t maxOpen,bool required) {
    return openCount<maxOpen && (!tight || openCount==0 || required);
}
// Walking waits only for a capsule-sized neighborhood. Wider reads are prefetch.
inline constexpr float kCollisionLandingRadius=1.5f;
inline constexpr float kCollisionMandatoryRadius=3.5f;
inline float collisionPrefetchRadius(float configured) {
    return configured<kCollisionMandatoryRadius ? kCollisionMandatoryRadius : configured>10.f ? 10.f : configured;
}
inline float collisionStreamPriority(float distance) {
    return distance<=kCollisionMandatoryRadius ? -10000.f+distance : 32.f+distance;
}
// Essential collision is a working-set requirement, not cosmetic detail.
inline bool collisionReadAllowed(bool pressure, bool collision, float priority) {
    return !pressure || (collision && priority < 0.0f);
}
inline bool collisionPlanFits(uint32_t used,uint32_t bytes,uint32_t budget,bool essential) {
    return essential || (used<=budget && bytes<=budget-used);
}
inline bool collisionBudgetFits(uint32_t used,uint32_t bytes,uint32_t budget) {
    return used<=budget && bytes<=budget-used;
}
// Bounds are WMO-local; radius and vertical margins are world yards.
inline bool collisionStreamNearby(float dx, float dy, float lowZ, float highZ,
                                  float feetZ, float scale, float radius) {
    return (dx*dx+dy*dy)*scale*scale <= radius*radius &&
           highZ >= feetZ-8.0f/scale && lowZ <= feetZ+3.0f/scale;
}
}
