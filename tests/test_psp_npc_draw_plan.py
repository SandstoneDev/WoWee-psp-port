"""Run production skinned-NPC submission with captured batch/state calls."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/rendering/psp/psp_creatures.cpp').read_text();start=s.index('void CreatureRenderer::drawSkinnedUnit(')
body=s.index('{',start);end=body+1;depth=1
while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
function=s[start:end]
cpp=r'''
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_npc_batch_plan.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace wowee::rendering::psp;
struct ScePspFMatrix4{float v[16];};
constexpr int GU_MODEL=0;unsigned matrices=0;
void sceGuSetMatrix(int,const ScePspFMatrix4*){++matrices;}
void sceGuTexScale(float,float){}void sceGuTexOffset(float,float){}
namespace pp {void textureBind(void*){}}
bool isBlended(uint8_t b){return b>=kBlendAlpha;}bool attachmentsEnabled(){return true;}
struct SkinView {const SkinHeader* h;const ModelBatch* batches;const uint16_t* geosets;};
struct CreatureRenderer {
 struct Skin {void* blob=reinterpret_cast<void*>(1);SkinView view{};uint32_t lod0Batches=3;} skin;
 struct Display {struct {int model=0;}h;std::vector<uint8_t> active{0,1,1,1,0};NpcBatchPlans batchPlans;} display;
 struct Anim {bool attach=true,helmHidesHair=false;std::vector<ScePspFMatrix4> ge{1};} anim;
 struct Visible {Display* d;Anim* anim;uint8_t lod=0;bool hull=false;ScePspFMatrix4 mtx{};};
 struct {unsigned batches=0,triangles=0;} stats_;
 struct Drawer {
  unsigned begins=0;std::vector<uint32_t> draws;
  void begin(){++begins;}
  uint32_t drawBatch(const SkinView&,const ModelBatch& b,const ScePspFMatrix4*){draws.push_back(b.indexStart);return 2;}
 } skinDrawer_;
 void* items_=reinterpret_cast<void*>(1);bool equipOn=true;
 std::vector<uint8_t> states;
 const Skin* skinOf(int)const{return &skin;}
 void setBatchState(const Display&,const ModelBatch&,uint8_t active){states.push_back(active);}
 void drawSkinnedUnit(const Visible&,bool);
};
FUNCTION
int main(){
 SkinHeader h{};h.batchCount=5;h.uvScale[0]=h.uvScale[1]=1;
 ModelBatch batches[5]{};for(unsigned i=0;i<5;++i)batches[i].indexStart=i;
 batches[3].blend=kBlendAlpha;uint16_t geosets[]={1,2,101,101,201};
 CreatureRenderer r;r.skin.view={&h,batches,geosets};
 CreatureRenderer::Visible v{&r.display,&r.anim};
 auto check=[&](std::vector<uint32_t> expected,bool blend=false){
  r.skinDrawer_.draws.clear();r.states.clear();const auto before=matrices;
  r.drawSkinnedUnit(v,blend);assert(r.skinDrawer_.draws==expected);
  assert(matrices-before==(expected.empty()?0:1));
 };
 check({1,2});r.anim.helmHidesHair=true;check({0,2});assert(r.states[0]==1);
 r.anim.helmHidesHair=false;check({},true);v.lod=1;check({});check({3},true);
 v.hull=true;check({},true);v.hull=false;
 r.anim.helmHidesHair=true;check({3},true);
 const auto builds=r.display.batchPlans.builds;
 for(int n=0;n<1000;++n){v.lod=0;r.anim.helmHidesHair=false;check({1,2});r.anim.helmHidesHair=true;check({0,2});v.lod=1;check({3},true);}
 assert(r.display.batchPlans.builds==builds); // fresh per-unit matrices/draws, shared selection only
 r.skin.lod0Batches=5;v.lod=0;r.anim.helmHidesHair=false;check({1,2});check({3},true);
 assert(r.display.batchPlans.builds>builds); // geometry/LOD split invalidates
 r.display.active[2]=0;r.display.batchPlans=NpcBatchPlans{};check({1});
 std::puts("NPC draw plans passed: identical batch order, hair/scalp, opaque/blended, LOD/hull, live matrices and 3000 submissions without rebuilding selections.");
}
'''.replace('FUNCTION',function)
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-npc-plan-') as tmp:
 p=Path(tmp);src=p/'plan.cpp';exe=p/'plan.exe';src.write_text(cpp)
 args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/I'+str(root/'include'),str(src),'/Fe:'+str(exe),'/Fo:'+str(p/'plan.obj')] if Path(compiler).stem.lower()=='cl' else [compiler,'-std=c++20','-O2','-I'+str(root/'include'),str(src),'-o',str(exe)]
 subprocess.run(args,cwd=tmp,check=True);subprocess.run([str(exe)],check=True)
