#pragma once
#include <cstdint>

namespace wowee::rendering::psp {
// Bone evaluation only: animation clocks and movement still run every frame.
inline uint32_t npcPoseInterval(float distance,float baseDistance,bool moving,
                                bool combat,bool important,bool oneShot) {
    if (baseDistance<=0.f || important || combat || oneShot) return 1;
    if (moving) return distance>baseDistance*2.f?4u:distance>baseDistance?2u:1u;
    return distance>baseDistance*2.f?6u:distance>baseDistance*1.333333f?4u:
           distance>baseDistance?2u:1u;
}
inline bool npcPoseProtected(float distance,float baseDistance,bool valid,bool important) {
    return !valid || important || baseDistance<=0.f || distance<=baseDistance;
}
inline bool npcPoseDue(bool valid,bool dirty,uint32_t frame,uint32_t phase,
                       uint32_t interval,bool changed) {
    return !valid || (dirty && (changed || interval==1 || ((frame+phase)%interval)==0));
}
}
