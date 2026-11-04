#include "rendering/psp/psp_fishing_visual.hpp"
#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/spell_classification.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace wowee::rendering;
int main(){
    CombatFSM f;AnimCapabilitySet caps;WeaponLoadout loadout;CombatFSM::Input in;
    f.startSpellCast(anim::FISHING_CAST,anim::FISHING_LOOP,true,0);
    in.haveAnimState=true;in.currentAnimId=anim::FISHING_CAST;in.currentAnimDuration=1.f;
    auto out=f.resolve(in,caps,loadout);assert(out.valid && out.animId==133 && !out.loop);
    in.currentAnimTime=1.f;out=f.resolve(in,caps,loadout);assert(out.valid && out.animId==134 && out.loop);
    in.currentAnimId=134;in.currentAnimTime=0;
    for(int i=0;i<100;++i){out=f.resolve(in,caps,loadout);assert(out.animId==134 && out.loop);}
    f.stopSpellCast();assert(f.getState()==CombatFSM::State::INACTIVE);
    f.startSpellCast(133,134,true,0);f.cancelSpellCast();assert(f.getState()==CombatFSM::State::INACTIVE);
    for(auto id:{7620u,7731u,7732u,18248u,33095u,51294u})assert(wowee::game::spellclass::isFishingCast(id));
    assert(!wowee::game::spellclass::isFishingCast(133)); // Fireball must not select fishing poses
    using namespace wowee::rendering::psp;
    assert(fishingBobberDip(10000,0)==0 && fishingBobberDip(1000,1000)==0);
    assert(std::fabs(fishingBobberDip(1450,1000)+.35f)<.00001f);
    assert(fishingBobberDip(1900,1000)==0 && fishingBobberDip(999,1000)==0);
    assert(fishingBobberDip(100,0xfffffff0u)<0); // uptime wrap
    glm::vec3 tip(1,2,3),bob(10,20,1);
    assert(glm::length(fishingLinePoint(tip,bob,0)-tip)<.00001f);
    assert(glm::length(fishingLinePoint(tip,bob,1)-bob)<.00001f);
    auto midpoint=fishingLinePoint(tip,bob,.5f);assert(midpoint.z<(tip.z+bob.z)*.5f);
    std::puts("Fishing: original cast-to-idle animation, channel cancellation, all ranks, server-bite dip and line endpoints passed.");
}
