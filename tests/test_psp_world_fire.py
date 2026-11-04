"""Exercise production seasonal-fire discovery and persistent-aura pruning."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
def function(file,sig):
 s=(root/file).read_text();a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
prune=function('src/rendering/psp/psp_spellfx.cpp','void SpellFxRenderer::keepAuraTargets(')
sync=function('src/ui/psp/ui_world_spellfx.cpp','void syncWorldFire(')
cpp=r'''
#include "rendering/psp/psp_world_effect_policy.hpp"
#include <glm/glm.hpp>
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <new>
#include <vector>
using namespace wowee;
namespace game{enum class ObjectType{UNIT,PLAYER};enum class UF{OBJECT_FIELD_ENTRY};int fieldIndex(UF){return 0;}}
namespace pp{uint32_t now=100;uint32_t logUptimeMs(){return now;}struct WorldSession{static uint32_t auraNowMs(){return now;}};}
namespace wowee::rendering::psp {
struct SpellFxConfig{uint32_t cacheBytes=0;int maxActive=0,maxSprites=0,maxPx=0;};
struct SpellFxRenderer{
 struct Active{uint64_t auraTarget;};std::vector<Active> active_;int opens=0;
 bool open(const SpellFxConfig& cfg){++opens;assert(cfg.maxActive==8&&cfg.maxSprites==64);return true;}
 void removeAt(size_t i){active_.erase(active_.begin()+i);}
 void keepAuraTargets(const uint64_t*,size_t);
 template<class Host>bool aura(uint32_t spell,uint32_t visual,uint64_t,uint64_t target,int32_t duration,bool,Host&){
  assert(isHallowsFireAura(spell)&&visual==9470&&duration);for(const auto& a:active_)if(a.auraTarget==target)return true;
  active_.push_back({target});assert(active_.size()<=8);return true;}
};}
using rendering::psp::SpellFxRenderer;
'''+prune+r'''
struct Entity{uint32_t entry=23686;glm::vec3 position{0};game::ObjectType getType(){return game::ObjectType::UNIT;}uint32_t getField(int){return entry;}};
struct Aura{uint32_t spellId=42074;uint64_t casterGuid=0;int32_t getRemainingMs(uint32_t)const{return -1;}};
struct Screen {
 struct {struct Values{int particles=1;}v;Values& values(){return v;}}settings_;
 struct Session{
  struct Entities{std::map<uint64_t,std::shared_ptr<Entity>> rows;const auto& getEntities(){return rows;}}e;
  std::map<uint64_t,std::vector<Aura>> auras;Entities& entities(){return e;}
  const std::vector<Aura>* unitAuras(uint64_t id){auto i=auras.find(id);return i==auras.end()?nullptr:&i->second;}
 }session_;
 struct {struct Info{struct {uint32_t visual=9470;}rec;}info;Info* get(uint32_t){return &info;}}spellDb_;
 float playerX_=0,playerY_=0,shownZ_=0;
 bool unitPosition(uint64_t id,glm::vec3& at,float& height){auto i=session_.e.rows.find(id);if(i==session_.e.rows.end())return false;at=i->second->position;height=2;return true;}
};
struct FireUi{Screen w;std::unique_ptr<SpellFxRenderer> worldFire;uint32_t nextFireSyncMs=0;
'''+sync+r'''
};
int main(){
 assert(rendering::psp::isHallowsFireHelper(23686)&&!rendering::psp::isHallowsFireHelper(123));
 FireUi ui;ui.syncWorldFire();assert(!ui.worldFire);
 for(uint64_t i=1;i<=30;++i){auto e=std::make_shared<Entity>();e->position.x=float(i);ui.w.session_.e.rows[i]=e;ui.w.session_.auras[i]={{}};}
 pp::now=700;ui.syncWorldFire();assert(ui.worldFire&&ui.worldFire->active_.size()==8);
 for(const auto& a:ui.worldFire->active_)assert(a.auraTarget<=8);
 ui.w.session_.auras[1].clear();pp::now+=600;ui.syncWorldFire();assert(ui.worldFire->active_.size()==8&&ui.worldFire->opens==1);
 for(const auto& a:ui.worldFire->active_)assert(a.auraTarget!=1&&a.auraTarget<=9);
 ui.w.session_.auras.clear();pp::now+=600;ui.syncWorldFire();assert(!ui.worldFire); // extinguished/disappeared: no invented flame
 ui.w.session_.auras[1]={{}};ui.w.session_.e.rows[1]->entry=123;pp::now+=600;ui.syncWorldFire();assert(!ui.worldFire);
 ui.w.session_.e.rows[1]->entry=23686;pp::now+=600;ui.syncWorldFire();assert(ui.worldFire);
 ui.w.settings_.v.particles=0;ui.syncWorldFire();assert(!ui.worldFire); // frees the optional renderer
 puts("World fire passed: only live event helpers/auras, nearest-eight budget, persistent refresh, extinguishing, no vendor substitution and particles-off release.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-world-fire-') as temp:
 folder=Path(temp);src=folder/'fire.cpp';exe=folder/'fire.exe';src.write_text(cpp)
 includes=[root/'include',root.parent/'tooling/glm-1.0.1']
 if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/O2']+['/I'+str(p) for p in includes]+[str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'fire.obj')]
 else:args=[compiler,'-std=c++20','-O2']+['-I'+str(p) for p in includes]+[str(src),'-o',str(exe)]
 subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
