"""Exercise production model exclusion and placement admission with a legacy grass pack."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/rendering/psp/psp_models.cpp').read_text(encoding='utf-8')
def function(sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
methods=''.join(function(sig) for sig in ['const ModelRenderer::Model* ModelRenderer::model(', 'uint32_t ModelRenderer::placeBudgeted(', 'void ModelRenderer::forgetFailed('])
cpp=r'''#include "rendering/psp/psp_world_decoration.hpp"
#include "rendering/psp/psp_tile_budget.hpp"
#include <vector>
#include <unordered_set>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
using namespace wowee::rendering::psp;
#define PSP_LOGE(...) do{}while(0)
namespace pp {
unsigned frees=0;
uint32_t logUptimeMs(){return 0;}
void pakFree(void* p){++frees;std::free(p);}
struct PakEntry{uint32_t size=80;};
struct Pak{ModelHeader header{};PakEntry entry;unsigned reads=0;
 const PakEntry* find(const char*){return &entry;}
 void* load(const PakEntry*){++reads;void* p=std::malloc(80);std::memcpy(p,&header,80);return p;}
};}
bool isBlended(uint8_t){return false;}
void lodRange(const ModelHeader&,uint8_t,uint32_t& first,uint32_t& count){first=count=0;}
bool probe(uint32_t){return true;}
struct ModelRenderer{
 struct Model{bool tried=false,bad=false,excluded=false,collisionPending=false;uint32_t collisionRetryMs=0,readyMs=0;
 void* blob=nullptr;const ModelHeader* h=nullptr;const ModelVertex* verts=nullptr;const uint16_t* indices=nullptr;const ModelBatch* batches=nullptr;uint8_t blendMask=0;};
 struct Tex{bool tried=false,ok=false;};std::vector<Model> models_;std::vector<Tex> textures_;
 std::unordered_set<uint32_t> seen_;std::vector<const DoodadPlacement*> instances_;unsigned textureLoads=0,memoryBytes_=0;
 struct{unsigned models=0;}stats_;struct{unsigned probes=0,stops=0;}budgetStats_;
 using BudgetProbe=bool(*)(uint32_t);inline static BudgetProbe budgetProbe=probe,budgetNearProbe=probe;
 struct LoadBudget{float x=0,y=0;uint32_t reserveKb=3072,stepKb=512;bool nearRule=false,parkedOnly=false;};
 void loadCollision(pp::Pak&,uint16_t,Model&){}
 void loadImpostor(pp::Pak&,uint16_t,Model&){}
 void loadTexture(pp::Pak&,uint16_t,bool,bool){++textureLoads;}
 uint32_t modelNeedBytes(const pp::Pak&,uint16_t,bool& parked)const{parked=true;return 0;}
 const Model* model(pp::Pak&,uint16_t,bool=false);
 uint32_t placeBudgeted(pp::Pak&,const DoodadPlacement*,const std::vector<uint32_t>&,const LoadBudget&,std::vector<uint32_t>&);
 void forgetFailed();
};
'''
main=r'''int main(){
 pp::Pak pak;std::memcpy(pak.header.magic,"WMDL",4);pak.header.version=kModelFormatVersion;pak.header.vertexOffset=80;
 pak.header.flags=kModelGroundDetail|kModelGrassPatch;ModelRenderer renderer;
 assert(!renderer.model(pak,0));assert(renderer.models_[0].excluded && pak.reads==1 && pp::frees==1);
 renderer.forgetFailed();assert(!renderer.model(pak,0));assert(pak.reads==1 && renderer.textureLoads==0);
 std::vector<DoodadPlacement> plants(20000);std::vector<uint32_t> order,left;
 for(unsigned i=0;i<plants.size();++i){plants[i].uniqueId=i+1;order.push_back(i);}
 assert(renderer.placeBudgeted(pak,plants.data(),order,{},left)==0);
 assert(left.empty() && renderer.seen_.empty() && renderer.instances_.empty() && pak.reads==1);
 pak.header.flags=kModelBuilding;
 auto* building=renderer.model(pak,1);assert(building && !renderer.models_[1].excluded);
 DoodadPlacement fence{};fence.model=1;fence.uniqueId=999999;std::vector<uint32_t> one{0};
 assert(renderer.placeBudgeted(pak,&fence,one,{},left)==1);
 assert(renderer.instances_.size()==1 && left.empty());pp::pakFree(renderer.models_[1].blob);
 std::puts("Legacy grass: one header load, zero textures/instances/deferred retries for 20,000 placements; buildings preserved.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-no-decoration-') as temp:
 src=Path(temp)/'test.cpp';exe=Path(temp)/'test.exe';src.write_text(cpp+methods+main,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O2','-I'+str(root/'include'),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
