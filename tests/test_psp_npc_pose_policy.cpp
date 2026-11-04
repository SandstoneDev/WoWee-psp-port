#include "rendering/psp/psp_npc_pose_policy.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace wowee::rendering::psp;
int main() {
    for(float d:{0.f,20.f,45.f})assert(npcPoseInterval(d,45,false,false,false,false)==1);
    assert(npcPoseInterval(70,45,true,false,false,false)==2);
    assert(npcPoseInterval(110,45,true,false,false,false)==4);
    assert(npcPoseInterval(70,45,false,false,false,false)==4);
    assert(npcPoseInterval(110,45,false,false,false,false)==6);
    assert(npcPoseInterval(110,45,false,true,false,false)==1); // combat
    assert(npcPoseInterval(110,45,false,false,true,false)==1); // target/mount/portrait
    assert(npcPoseInterval(110,45,false,false,false,true)==1); // one-shot
    assert(npcPoseInterval(110,0,false,false,false,false)==1); // disabled throttle
    assert(npcPoseDue(false,false,1,0,6,false)); // initial pose never skipped
    assert(npcPoseDue(true,true,1,0,6,true)); // death/locomotion transition immediate
    assert(!npcPoseDue(true,false,6,0,6,false)); // unchanged frozen pose reused
    // Adaptive caps cannot drop full-rate nearby poses, even in a crowded scene.
    for(float d:{0.f,5.f,10.f,20.f})assert(npcPoseProtected(d,20,true,false));
    assert(!npcPoseProtected(21,20,true,false));
    assert(npcPoseProtected(90,20,false,false)); // unposed models must initialize
    assert(npcPoseProtected(90,20,true,true)); // target/portrait/mount
    assert(npcPoseProtected(90,0,true,false)); // explicit unlimited mode
    unsigned old=0,reused=0;
    for(unsigned frame=0;frame<120;++frame)for(unsigned guid=0;guid<60;++guid){
        if((frame+guid)%4==0)++old;
        if(npcPoseDue(true,true,frame,guid,6,false))++reused;
    }
    assert(old==1800&&reused==1200); // 33% fewer far-idle bone evaluations
    std::printf("NPC poses passed: far idle %u -> %u evaluations, protected actors full rate.\n",old,reused);
}
