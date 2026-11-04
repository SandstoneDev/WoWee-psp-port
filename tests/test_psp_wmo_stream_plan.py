"""Run production WMO planning with host I/O and synthetic capital-size tables."""
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'src/rendering/psp/psp_bigwmo.cpp').read_text()
header=(root/'include/rendering/psp/psp_bigwmo.hpp').read_text()
declarations=header[header.index('struct BigWmoConfig {'):header.index('// FNV of')].replace('private:', 'public:')
def function(text,signature):
    begin=text.index(signature);body=text.index('{',begin);end=body+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]+'\n'
methods=''.join(function(source,s) for s in [
    'bool due(', 'const ChunkMeshHeader* meshHeader(', 'bool meshValid(', 'const uint16_t* meshTextures(',
    'BigWmo::Res* BigWmo::resOf(', 'void BigWmo::freeRes(', 'void BigWmo::setCollisionProbe(',
    'BigWmoBlobRef BigWmo::planRef(', 'void BigWmo::want(', 'void BigWmo::planCollision(',
    'void BigWmo::update(', 'void BigWmo::install(', 'void BigWmo::cancel(', 'bool BigWmo::collisionReadyAt('])
baseline=''
if len(sys.argv)>2:
    old=Path(sys.argv[2]).read_text()
    baseline=function(old,'void BigWmo::update(').replace('BigWmo::update(', 'BigWmo::baselineUpdate(')
    baseline=baseline.replace('planDirty_ = false;', 'planDirty_ = false; ++stats_.spatialPlans;')
    baseline='#define HAVE_REFERENCE\n'+baseline
    declarations=declarations.replace('    BigWmo() = default;', '    void baselineUpdate(const glm::vec3&,const glm::vec3&,float,uint32_t,std::vector<Request>&,uint32_t,bool=false);\n    BigWmo() = default;')
cpp=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>
#include <glm/glm.hpp>
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_bigwmo_vis.hpp"
#include "rendering/psp/psp_collision_neighborhood.hpp"
#include "rendering/psp/psp_wmo_group_cache.hpp"
#define PROF_SCOPE(...) ((void)0)
#define PSP_LOGW(...) ((void)0)
namespace wowee::platform::psp {
uint32_t now=100;
uint32_t logUptimeMs(){return now;}
bool textureFromBlob(void* blob,uint32_t,Texture& t,bool){t.block=blob;return true;}
uint32_t textureDropTopMip(Texture&,uint32_t){return 0;}
void textureRelease(Texture& t){std::free(t.block);t=Texture{};}
}
namespace wowee::rendering::psp {
namespace pp=wowee::platform::psp;
struct ScePspFMatrix4 {float v[16];};struct TerrainCamera {};
constexpr uint32_t kKeyShift=24,kReadRetryMs=5000;
bool colValid(const void* p,uint32_t n){return p&&n>0;}
'''
main=r'''
void BigWmo::close() {
    for(auto* v:{&mesh_,&near_,&lod_,&col_,&shell_,&cellDoodads_})for(auto& r:*v){std::free(r.blob);r.blob=nullptr;}
    for(auto& t:tex_)if(t.ok)pp::textureRelease(t.t);
}
struct Scene {
    BigWmoHeader h{};std::vector<BigWmoCell> cells;std::vector<BigWmoChunk> chunks;
    std::vector<BigWmoBlobRef> lod,near,textures;
    BigWmoGroup group{};
    Scene():cells(27),chunks(2281),lod(2281),near(2281),textures(8) {
        h.chunkCount=2281;h.cellCount=27;h.groupCount=1;h.textureCount=8;h.step=1;
        group.flags=kWmoGroupExterior;
        for(uint32_t c=0;c<27;++c) {
            auto& cl=cells[c];cl.center[0]=c*400.f;cl.chunkStart=c*85;cl.chunkCount=std::min(85u,2281u-c*85);
            cl.bmin[0]=cl.center[0]-1;cl.bmax[0]=cl.center[0]+340;cl.bmin[1]=-1;cl.bmax[1]=1;cl.bmin[2]=-1;cl.bmax[2]=1;
            cl.shell={6000000+c*256,64};cl.doodads={7000000+c*256,80};
        }
        for(uint32_t i=0;i<chunks.size();++i) {
            auto& k=chunks[i];k.cell=i/85;k.flags=kChunkSeenOutside;
            k.bmin[0]=(i%85)*4-1;k.bmax[0]=(i%85)*4+1;k.bmin[1]=-1;k.bmax[1]=1;k.bmin[2]=-1;k.bmax[2]=1;
            k.mesh={1000+i*256,64};k.collision={2000000+i*256,16};
            near[i]={3000000+i*256,64};lod[i]={4000000+i*256,64};
        }
        for(uint32_t i=0;i<8;++i)textures[i]={5000000+i*256,128};
    }
    void bind(BigWmo& w) {
        w.tables_.h=&h;w.tables_.cells=cells.data();w.tables_.chunks=chunks.data();w.tables_.groups=&group;
        w.tables_.textures=textures.data();w.tables_.chunkLod=lod.data();w.tables_.chunkNear=near.data();
        w.mesh_.resize(h.chunkCount);w.near_.resize(h.chunkCount);w.lod_.resize(h.chunkCount);w.col_.resize(h.chunkCount);
        w.shell_.resize(h.cellCount);w.cellDoodads_.resize(h.cellCount);w.tex_.resize(h.textureCount);w.groupSeen_.resize(1);
        w.vis_.outside=true;w.cfg.lod.viewDistance=40;w.cfg.prefetchSec=0;w.headerSize_=100;w.cfg.budgetBytes=1000000;
        w.doodadsOk_=true;
        std::vector<CollisionBoundsIndex::Entry> bounds;
        for(uint32_t c=0;c<h.chunkCount;++c){glm::vec3 lo,hi;w.tables_.chunkBox(c,lo,hi);bounds.push_back({lo,hi,c});}
        w.collisionBounds_.build(bounds);
    }
};
void* meshBlob(uint16_t texture=0) {
    auto* p=static_cast<uint8_t*>(std::calloc(1,64));auto* h=reinterpret_cast<ChunkMeshHeader*>(p);
    std::memcpy(h->magic,"WBCK",4);h->version=kBigWmoVersion;h->textureCount=1;h->textureOffset=56;h->batchOffset=h->vertexOffset=64;
    std::memcpy(p+56,&texture,2);return p;
}
std::set<uint32_t> wanted(const BigWmo& w) {
    std::set<uint32_t> result;
    for(uint32_t kind:{1u,2u,3u,4u,5u,7u}) {
        const auto& v=kind==1?w.mesh_:kind==2?w.near_:kind==3?w.lod_:kind==4?w.col_:kind==5?w.shell_:w.cellDoodads_;
        for(uint32_t i=0;i<v.size();++i)if(v[i].wanted)result.insert(kind<<24|i);
    }
    for(uint32_t i=0;i<w.tex_.size();++i)if(w.tex_[i].wanted)result.insert(6u<<24|i);
    return result;
}
int run() {
    Scene scene;BigWmo w;scene.bind(w);std::vector<BigWmo::Request> requests;
    w.update({0,0,0},{0,0,0},1.f/30,100,requests,1000);
    assert(w.stats_.spatialPlans==1&&requests.size()>0&&w.stats_.planCandidates<50);
    for(auto r:requests)w.cancel(r.key);
    // Completions discover texture dependencies without scanning/sorting cells.
    for(uint32_t i=0;i<10;++i) {
        w.install(1u<<24|i,meshBlob(i%8),64);
        w.update({0,0,0},{0,0,0},1.f/30,101+i,requests,1000);
        assert(w.tex_[i%8].wanted&&w.stats_.spatialPlans==1);
        assert(std::is_sorted(w.planScratch_.begin(),w.planScratch_.end(),[](auto a,auto b){return a.prio<b.prio;}));
        std::set<uint32_t> unique;for(auto p:w.planScratch_)assert(unique.insert(p.key).second);
        for(auto r:requests)w.cancel(r.key);
    }
    // A cancelled texture is retried next frame, with no whole-building plan.
    w.update({0,0,0},{0,0,0},1.f/30,111,requests,1000);
    assert(std::any_of(requests.begin(),requests.end(),[](auto r){return r.key==6u<<24;}));
    // A new collision chunk just across a vertical boundary enters immediately.
    std::vector<CollisionBoundsIndex::Entry> boxes={{{-1,-1,-1},{1,1,1},0},{{-1,-1,3.015f},{1,1,4},20}};
    w.collisionBounds_.build(boxes);w.collisionDirty_=true;
    glm::vec3 blocked(0,0,.03f);w.setCollisionProbe(&blocked);
    w.update({0,0,0},{0,0,0},1.f/30,112,requests,1000,true);
    assert(w.col_[20].wanted&&w.col_[20].prio<0&&w.stats_.spatialPlans==1);
    assert(std::any_of(requests.begin(),requests.end(),[](auto r){return r.key==(4u<<24|20);}));
    // Budget cuts/restores act on cached candidates; mandatory collision wins.
    w.cfg.budgetBytes=100;w.update({0,0,0},{0,0,0},1.f/30,113,requests,1000,true);
    assert(w.col_[20].wanted&&!w.mesh_[0].wanted&&w.stats_.spatialPlans==1);
    w.cfg.budgetBytes=1000000;w.update({0,0,0},{0,0,0},1.f/30,114,requests,1000,false);
    assert(w.mesh_[0].wanted&&w.tex_[0].wanted&&w.stats_.spatialPlans==1);
    // A failed read retries when due, even if no movement dirties the scene.
    w.install(6u<<24,nullptr,128);w.update({0,0,0},{0,0,0},1.f/30,115,requests,1000);
    assert(std::none_of(requests.begin(),requests.end(),[](auto r){return r.key==6u<<24;}));
    pp::now=5101;w.update({0,0,0},{0,0,0},1.f/30,116,requests,1000);
    assert(std::any_of(requests.begin(),requests.end(),[](auto r){return r.key==6u<<24;}));
    assert(w.stats_.spatialPlans==1);
    // Settings and teleports invalidate spatial candidates immediately.
    w.cfg.lod.viewDistance=12;w.update({0,0,0},{0,0,0},1.f/30,117,requests,0);
    assert(w.stats_.spatialPlans==2&&!w.mesh_[10].wanted);
    w.update({400,0,0},{400,0,0},1.f/30,118,requests,1000);
    assert(w.stats_.spatialPlans==3&&w.mesh_[85].wanted&&!w.mesh_[0].wanted);
    // Lazy eviction searches installed resources, and can restore the same key.
    w.cfg.budgetBytes=100;w.update({400,0,0},{400,0,0},1.f/30,119,requests,0);
    assert(!w.mesh_[0].blob&&w.residentKeys_.empty());
    // Test every planned key against the saved original planner when supplied.
#ifdef HAVE_REFERENCE
    BigWmo fast,original;scene.bind(fast);scene.bind(original);
    fast.update({0,0,0},{0,0,0},1.f/30,100,requests,0);
    original.baselineUpdate({0,0,0},{0,0,0},1.f/30,100,requests,0);
    assert(wanted(fast)==wanted(original));
    for(uint32_t i=0;i<10;++i) {
        fast.install(1u<<24|i,meshBlob(i%8),64);
        original.install(1u<<24|i,meshBlob(i%8),64);original.planDirty_=true;
        fast.update({0,0,0},{0,0,0},1.f/30,101+i,requests,0);
        original.baselineUpdate({0,0,0},{0,0,0},1.f/30,101+i,requests,0);
        assert(wanted(fast)==wanted(original));
    }
    assert(fast.stats_.spatialPlans==1&&original.stats_.spatialPlans==11);
    std::printf("Reference comparison: identical desired assets after 10 completions; full spatial plans %u -> %u.\n",
        original.stats_.spatialPlans,fast.stats_.spatialPlans);
    // Movement, LOD/shell thresholds, and budget changes preserve selection.
    for(uint32_t i=0;i<400;++i) {
        glm::vec3 p((i%80)*7.f,static_cast<float>(i%3),0);
        fast.cfg.budgetBytes=original.cfg.budgetBytes=5000+(i%5)*10000;
        fast.cfg.lod.viewDistance=original.cfg.lod.viewDistance=40.f+(i%4)*100.f;
        original.planDirty_=true;
        fast.update(p,p,1.f/30,200+i,requests,0);
        original.baselineUpdate(p,p,1.f/30,200+i,requests,0);
        assert(wanted(fast)==wanted(original));
    }
    std::puts("400 movement/LOD/budget snapshots match the original planner's selected asset sets.");
#endif
    std::printf("WMO streaming passed: 2281 chunks, local working set, texture dependencies, cancellations, exact collision probes, budgets, pressure, retries, settings, teleport, eviction.\n");
    return 0;
}
} // namespace
int main(){return wowee::rendering::psp::run();}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-wmo-plan-') as temp:
    folder=Path(temp);src=folder/'plan.cpp';exe=folder/'plan.exe'
    src.write_text(cpp+declarations+methods+baseline+main)
    glm=root.parent/'tooling/glm-1.0.1'
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/I'+str(root/'include'),'/I'+str(glm),str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'plan.obj')]
    else:args=[compiler,'-std=c++20','-O2','-I'+str(root/'include'),'-I'+str(glm),str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
