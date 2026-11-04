#include "rendering/animation/activity_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <cassert>
using namespace wowee::rendering;
int main(){
 ActivityFSM f;ActivityFSM::Input in;AnimCapabilitySet caps;
 f.startLooting();auto out=f.resolve(in,caps);assert(out.valid&&out.animId==anim::LOOT&&!out.loop);
 in.haveAnimState=true;in.currentAnimId=anim::LOOT;in.currentAnimDuration=1;in.currentAnimTime=1;
 out=f.resolve(in,caps);assert(out.valid&&out.animId==anim::LOOT_HOLD&&out.loop);
 f.stopLooting();in.currentAnimId=anim::LOOT_HOLD;in.currentAnimTime=0;
 out=f.resolve(in,caps);assert(out.valid&&out.animId==anim::LOOT_UP&&!out.loop);
 in.currentAnimId=anim::LOOT_UP;in.currentAnimTime=1;out=f.resolve(in,caps);assert(f.getState()==ActivityFSM::State::NONE);
 f.startLooting();in.moving=true;out=f.resolve(in,caps);assert(f.getState()==ActivityFSM::State::NONE);
}
