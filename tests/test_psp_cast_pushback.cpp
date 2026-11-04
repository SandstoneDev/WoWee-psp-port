// Standalone regression using the production combat state machine.
#include "rendering/animation/combat_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <cassert>
using wowee::rendering::CombatFSM;
namespace anim = wowee::rendering::anim;
int main() {
    CombatFSM fsm;
    fsm.startSpellCast(anim::SPELL_PRECAST,anim::SPELL,true,anim::SPELL_CAST);
    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    assert(fsm.getState()==CombatFSM::State::SPELL_PRECAST);
    fsm.setState(CombatFSM::State::SPELL_CASTING);
    for (int i=0;i<5;++i) fsm.triggerHitReaction(anim::COMBAT_WOUND);
    assert(fsm.getState()==CombatFSM::State::SPELL_CASTING);
    fsm.stopSpellCast();
    assert(fsm.getState()==CombatFSM::State::SPELL_FINALIZE);
    fsm.cancelSpellCast();assert(fsm.getState()==CombatFSM::State::INACTIVE);
    fsm.startSpellCast(0,123,true,0);fsm.stopSpellCast();
    assert(fsm.getState()==CombatFSM::State::INACTIVE); // no fabricated release for gathering
    fsm.startSpellCast(0,123,true,129);fsm.stopSpellCast();
    assert(fsm.getState()==CombatFSM::State::SPELL_FINALIZE);
    fsm.cancelSpellCast();assert(fsm.getState()==CombatFSM::State::INACTIVE);
    fsm.reset();
    fsm.triggerHitReaction(anim::COMBAT_WOUND);
    assert(fsm.getState()==CombatFSM::State::HIT_REACTION);
}
