#pragma once
#include <cstdint>
namespace wowee::rendering::psp {
struct NpcPriority {
    bool ownMount=false;
    bool inViewport=false;
    float playerDistanceSq=0;
    uint64_t guid=0;
};
inline bool npcWithinDrawBudget(bool allVisible, bool inViewport, bool mandatory, uint32_t drawn, uint32_t limit) {
    return mandatory || (allVisible && inViewport) || drawn < limit;
}
inline bool npcPriorityBefore(const NpcPriority& a,const NpcPriority& b) {
    // Only the controlled rider's body is mandatory; other mounts and selected
    // off-screen targets cannot displace the nearest visible NPCs.
    if (a.ownMount!=b.ownMount) return a.ownMount;
    if (a.inViewport!=b.inViewport) return a.inViewport;
    if (a.playerDistanceSq!=b.playerDistanceSq) return a.playerDistanceSq<b.playerDistanceSq;
    return a.guid<b.guid; // stable ties avoid model-slot flicker
}
}
