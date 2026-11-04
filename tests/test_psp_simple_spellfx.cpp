#include "rendering/psp/psp_simple_spellfx.hpp"
#include "platform/psp/psp_spellfx_events.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
using namespace wowee::rendering::psp;
namespace pp=wowee::platform::psp;
struct Sink:pp::SpellFxSink { int count=0;pp::SpellFxEvent last; void push(const pp::SpellFxEvent& e) override {++count;last=e;} };
int main() {
    assert(simpleSpell(172)==SimpleSpell::Corruption && simpleSpell(47813)==SimpleSpell::Corruption);
    assert(simpleSpell(3110)==SimpleSpell::Fire && simpleSpell(47965)==SimpleSpell::Fire);
    assert(simpleSpell(42842)==SimpleSpell::Frost && simpleSpell(47809)==SimpleSpell::Shadow);
    assert(simpleSpell(8690)==SimpleSpell::None);
    SimpleSpellFx fx;glm::vec3 target(12,0,1);
    auto locate=[&](uint64_t guid,glm::vec3& out){if(guid!=2)return false;out=target;return true;};
    assert(fx.launch(3110,{0,0,1},target,2));
    fx.advance(.25f,locate);assert(fx.effects[0].phase==SimpleSpellFx::Missile);
    assert(std::fabs(fx.effects[0].pos.x-6)<.001f);
    target={6,6,1};fx.advance(.25f,locate);
    assert(fx.effects[0].phase==SimpleSpellFx::Impact && fx.effects[0].pos==target);
    fx.advance(.25f,locate);fx.advance(.25f,locate);assert(fx.active()==0);
    fx.aura(172,1,2,18000,target);fx.advance(.25f,locate);assert(fx.active()==1);
    target.x=8;fx.advance(.25f,locate);assert(fx.effects[0].pos==target);
    fx.aura(172,1,2,18000,target);assert(fx.active()==1 && fx.effects[0].elapsed==0); // refresh, no duplicate
    fx.aura(172,1,2,0,target);assert(fx.active()==0); // dispel
    fx.aura(172,1,2,100,target);fx.advance(.2f,locate);assert(fx.active()==0); // duration expiry
    fx.launch(133,{0,0,0},target,99);fx.advance(.1f,locate);assert(fx.active()==0); // despawn
    assert(!fx.launch(172,{0,0,0},target,2)); // instant curse is never a bolt
    assert(fx.launch(999999,{0,0,0},target,2,SimpleSpell::Corruption)); // mixed direct/periodic shadow metadata
    fx=SimpleSpellFx{};
    fx.limit=3;for(int i=0;i<30;++i)fx.launch(133,{0,0,0},target,2);assert(fx.active()==3);
    const uint8_t damage[3]={2,0,0}, heal[3]={10,0,0}, apply[3]={6,0,0};
    const uint16_t none[3]={}, dot[3]={3,0,0}, leech[3]={53,0,0};
    for (uint32_t school : {1u,2u,4u,8u,16u,32u,64u}) {
        auto style=simpleSpellStyle(999999,school,2000,0,damage,none);
        assert(style.color!=SimpleSpell::None && style.damage);
        assert(style.projectile == (school!=1));
        auto healing=simpleSpellStyle(999999,school,2000,0,heal,none);
        assert(healing.heal && !healing.projectile);
    }
    auto channel=simpleSpellStyle(689,32,0,0x4004,apply,leech);assert(channel.channel && channel.periodic && !channel.projectile);
    auto curse=simpleSpellStyle(99999,32,0,0,apply,dot);assert(curse.periodic && curse.color==SimpleSpell::Corruption);
    fx=SimpleSpellFx{};
    fx.startCast(99999,SimpleSpell::Arcane,2,target,3000);assert(fx.active()==1);
    fx.cancelCast(3);assert(fx.active()==1);fx.cancelCast(2);assert(fx.active()==0);
    fx.channel(689,SimpleSpell::Shadow,2,2,target,target,10);fx.channelTime(2,100);fx.advance(.2f,locate);assert(fx.active()==0);
    fx.aura(99999,1,2,600000,target,SimpleSpell::Holy,false);assert(fx.effects[0].duration==6);
    fx.aura(99999,1,2,0,target,SimpleSpell::Holy,false);assert(fx.active()==0);
    fx.area(5740,SimpleSpell::Fire,2,{3,4,0},8,true);fx.advance(.1f,locate);assert(fx.effects[0].pos==glm::vec3(3,4,0));
    fx.channelTime(2,0);assert(fx.active()==0);
    fx.burst(2061,SimpleSpell::Holy,target,true);assert(fx.effects[0].heal);
    Sink sink;pp::spellFxSink()=&sink;
    pp::spellFxAura(2,8690,1,1000);assert(sink.count==1); // all spell families are forwarded
    pp::spellFxAura(2,172,1,18000);assert(sink.count==2 && sink.last.targets[0]==2 && sink.last.auraMs==18000);
    pp::spellFxAura(2,172,1,0);assert(sink.last.auraMs==0);
    pp::spellFxAura(2,99999,0,5000);assert(sink.last.guid==2); // omitted caster follows the aura owner
    pp::spellFxSink()=nullptr;
    std::puts("all-school spell styles, projectiles, auras, channels, ground effects and budgets passed");
}
