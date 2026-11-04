"""Run production PSP model collision routines with host Pak/kernel substitutes."""
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'src/rendering/psp/psp_models.cpp').read_text()
def function(signature):
    start=source.index(signature)
    body=source.index('{',start)
    depth=1; end=body+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'
helpers=source[source.index('struct ColView {'):source.index('glm::vec3 closestOnTri(')]
functions=''.join(function(s) for s in [
    'void ModelRenderer::loadCollision(', 'void ModelRenderer::retryCollisions(',
    'bool ModelRenderer::collisionReadyAt(', 'uint32_t bitsOf(', 'bool ModelRenderer::floorHeight(',
    'glm::vec3 closestOnTri(', 'void ModelRenderer::collideWalls('])
cpp=r'''
#include "rendering/psp/psp_model_instance.hpp"
#include "rendering/psp/psp_collision_math.hpp"
#include "rendering/psp/psp_collision_world_bounds.hpp"
#include "rendering/psp/psp_collision_bounds_index.hpp"
#include "rendering/psp/psp_collision_candidates.hpp"
#include "rendering/psp/psp_wall_collision_cache.hpp"
#include "rendering/psp/psp_bigwmo_vis.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <memory>
#include <new>
using namespace wowee::rendering::psp;
#define PROF_SCOPE(...) ((void)0)
#define PSP_LOGW(...) ((void)0)
namespace pp {
uint32_t clockMs=0; unsigned allocs=0,frees=0;
uint32_t logUptimeMs(){return clockMs;}
void pakFree(void* p){if(p)++frees;std::free(p);}
struct PakEntry {uint32_t size;};
struct Pak {
    std::vector<uint8_t> bytes; bool fail=false,exists=true; unsigned reads=0; PakEntry entry{};
    const PakEntry* find(const char*){entry.size=static_cast<uint32_t>(bytes.size());return exists?&entry:nullptr;}
    void* load(const PakEntry*){++reads;if(fail)return nullptr;void* p=std::malloc(bytes.size());assert(p);++allocs;std::memcpy(p,bytes.data(),bytes.size());return p;}
};
}
bool g_cpuFast=true;
constexpr float kPlayerRadius=.5f,kPlayerHeight=2.f,kWallStepHeight=.6f;
struct {unsigned colQueries=0,colMemoHits=0,colInsts=0;} g_cpuStats;
class ModelRenderer {
public:
    struct Model {const ModelHeader* h=nullptr;void* colBlob=nullptr;const CollisionHeader* col=nullptr;bool collisionPending=false;uint32_t collisionRetryMs=0;};
    using Inst=ModelInstance<Model>;using CollisionTransform=ModelCollisionTransform;
    std::vector<CollisionTransform> collisionTransforms_;
    struct FloorMemo {uint32_t key[3];uint32_t gen=0;float z=0;bool found=false;uint32_t placementId=0;};
    std::vector<Model> models_;std::vector<Inst> insts_;CollisionBoundsIndex collisionBounds_;
    std::vector<uint32_t> collisionOrder_;
    CollisionCandidates collisionCandidates_;
    mutable std::unique_ptr<WallCollisionCache> wallCache_;
    uint32_t pendingCollisionCount_=0,collisionBytes_=0,gridGen_=1;bool gridDirty_=false;
    mutable FloorMemo floorMemo_[4];mutable uint8_t floorMemoNext_=0;
    void loadCollision(pp::Pak&,uint16_t,Model&);
    void retryCollisions(pp::Pak&);
    bool collisionReadyAt(float,float,float)const;
    bool floorHeight(float,float,float,float&,uint32_t* =nullptr)const;
    void collideWalls(glm::vec3&,float)const;
    void flushGrid(){if(gridDirty_){++gridGen_;gridDirty_=false;}}
};
'''
main=r'''
std::vector<uint8_t> square() {
    std::vector<uint8_t> b(sizeof(CollisionHeader)+48+16+8+8);
    auto* h=reinterpret_cast<CollisionHeader*>(b.data());
    std::memcpy(h->magic,"WCOL",4);h->vertexCount=4;h->triangleCount=2;h->listCount=2;
    h->bmin[0]=h->bmin[1]=-5;h->bmax[0]=h->bmax[1]=5;h->bmin[2]=h->bmax[2]=0;
    h->cell=10;h->nx=h->ny=1;h->vertexOffset=sizeof(CollisionHeader);h->triangleOffset=h->vertexOffset+48;
    h->cellOffset=h->triangleOffset+16;h->listOffset=h->cellOffset+8;
    float vertices[]={-5,-5,0,5,-5,0,5,5,0,-5,5,0};std::memcpy(b.data()+h->vertexOffset,vertices,48);
    CollisionTri tris[]={{0,1,2,1,0},{0,2,3,1,0}};std::memcpy(b.data()+h->triangleOffset,tris,16);
    uint32_t cells[]={0,2},list[]={0,1};std::memcpy(b.data()+h->cellOffset,cells,8);std::memcpy(b.data()+h->listOffset,list,8);
    assert(collisionValid(b.data(),static_cast<uint32_t>(b.size())));return b;
}
int main(){
    // A visible object with a failed collision read must block readiness, retry,
    // and invalidate previous floor results after the collision arrives.
    ModelHeader dummy{};ModelRenderer recovery;recovery.models_.resize(1);
    auto& m=recovery.models_[0];m.h=&dummy;pp::Pak pak;pak.bytes=square();pak.fail=true;
    recovery.insts_.push_back({0,0,0,8,1.f,nullptr,&m});
    recovery.loadCollision(pak,0,m);assert(m.collisionPending&&!m.col&&recovery.pendingCollisionCount_==1);
    assert(!recovery.collisionReadyAt(0,0,0));assert(recovery.collisionReadyAt(100,100,0));
    pp::clockMs=999;recovery.retryCollisions(pak);assert(pak.reads==1);
    pak.fail=false;pp::clockMs=1000;recovery.retryCollisions(pak);
    assert(m.col&&!m.collisionPending&&recovery.pendingCollisionCount_==0&&recovery.gridGen_==2);
    assert(recovery.collisionReadyAt(0,0,0));pp::pakFree(m.colBlob);m.col=nullptr;
    // Invalid triangle/cell references are rejected and can recover later.
    auto* bad=reinterpret_cast<CollisionHeader*>(pak.bytes.data());bad->listCount=900;
    recovery.loadCollision(pak,0,m);assert(m.collisionPending&&!m.col);
    pak.exists=false;recovery.loadCollision(pak,0,m);assert(!m.collisionPending&&recovery.pendingCollisionCount_==0);
    assert(pp::allocs==pp::frees);

    auto bytes=square();ModelRenderer renderer;renderer.models_.resize(1);
    renderer.models_[0].col=reinterpret_cast<const CollisionHeader*>(bytes.data());
    std::vector<DoodadPlacement> placements(512);
    std::vector<CollisionBoundsIndex::Entry> bounds;
    std::mt19937 rng(81);std::uniform_real_distribution<float> angle(-.4f,.4f),scale(.5f,3.f);
    for(unsigned i=0;i<placements.size();++i){
        auto& p=placements[i];p.uniqueId=i+1;
        glm::mat4 matrix=glm::rotate(glm::mat4(1),angle(rng),glm::vec3(0,1,0));
        matrix=glm::rotate(matrix,angle(rng),glm::vec3(1,0,0));
        glm::mat3 a=glm::mat3(matrix)*glm::mat3(glm::vec3((i%2?-1:1)*scale(rng),0,0),glm::vec3(0,scale(rng),0),glm::vec3(0,0,1));
        glm::vec3 pos((i%32)*40.f,(i/32)*40.f,-41.f+(i%3)*10.f);
        for(int c=0;c<3;++c){p.position[c]=pos[c];for(int row=0;row<3;++row)p.axes[3*c+row]=a[c][row];}
        // Deliberately undersized render sphere: it must not exclude collision.
        ModelRenderer::Inst in{pos.x,pos.y,pos.z,.1f,1.f,&p,&renderer.models_[0]};
        ModelRenderer::CollisionTransform col;col.inverse=glm::inverse(a);
        collisionWorldBounds({-5,-5,0},{5,5,0},a,pos,col.collisionLo,col.collisionHi);
        renderer.collisionTransforms_.push_back(col);in.collision=renderer.collisionTransforms_.size();
        renderer.insts_.push_back(in);bounds.push_back({col.collisionLo,col.collisionHi,i});
        for(int corner=0;corner<8;++corner){
            glm::vec3 v=a*glm::vec3(corner&1?5.f:-5.f,corner&2?5.f:-5.f,0)+pos;
            for(int axis=0;axis<3;++axis)assert(v[axis]>=col.collisionLo[axis]&&v[axis]<=col.collisionHi[axis]);
        }
    }
    renderer.collisionBounds_.build(bounds);
    unsigned tests=0;uint64_t indexedVisits=0;
    for(unsigned i=0;i<placements.size();++i)for(float x:{-4.f,0.f,4.f})for(float y:{-4.f,0.f,4.f}){
        const auto& in=renderer.insts_[i];const auto a=axesOf(*in.p);
        const glm::vec3 target=a*glm::vec3(x,y,0)+glm::vec3(in.x,in.y,in.z);
        float indexed=0,brute=0;uint32_t owner=0;const auto before=g_cpuStats.colInsts;
        g_cpuFast=true;bool ok=renderer.floorHeight(target.x,target.y,target.z+1,indexed,&owner);
        indexedVisits+=g_cpuStats.colInsts-before;
        assert(ok&&owner==placements[i].uniqueId&&std::fabs(indexed-target.z)<.002f);
        g_cpuFast=false;assert(renderer.floorHeight(target.x,target.y,target.z+1,brute));assert(std::fabs(brute-indexed)<.0001f);++tests;
    }
    assert(indexedVisits<tests*3);
    // Index invalidation when an elevator changes placement, plus a cached miss.
    g_cpuFast=true;float z=0;assert(!renderer.floorHeight(-100,-100,10,z));
    assert(!renderer.floorHeight(-100,-100,10,z));
    assert(renderer.floorHeight(0,0,0,z));const float oldZ=z;
    placements[0].position[2]+=10;renderer.insts_[0].z+=10;
    renderer.collisionTransforms_[0].collisionLo.z+=10;renderer.collisionTransforms_[0].collisionHi.z+=10;
    bounds[0].lo.z+=10;bounds[0].hi.z+=10;renderer.collisionBounds_.build(bounds);++renderer.gridGen_;
    assert(renderer.floorHeight(0,0,0,z));assert(std::fabs(z-oldZ-10)<.002f);
    // A visible sphere smaller than the actual wall must not disable contact.
    ModelRenderer wall;wall.models_.resize(1);wall.models_[0].col=reinterpret_cast<const CollisionHeader*>(bytes.data());
    DoodadPlacement p{};glm::mat3 axes(glm::rotate(glm::mat4(1),glm::radians(90.f),glm::vec3(0,1,0)));
    for(int c=0;c<3;++c)for(int row=0;row<3;++row)p.axes[c*3+row]=axes[c][row];
    ModelRenderer::Inst w{0,0,0,.01f,1.f,&p,&wall.models_[0]};ModelRenderer::CollisionTransform wc;wc.inverse=glm::inverse(axes);w.collision=1;
    collisionWorldBounds({-5,-5,0},{5,5,0},axes,{0,0,0},wc.collisionLo,wc.collisionHi);
    wall.collisionTransforms_.push_back(wc);
    wall.insts_.push_back(w);wall.collisionOrder_.push_back(0);
    wall.collisionBounds_.build({{wc.collisionLo,wc.collisionHi,0}});
    glm::vec3 capsule(.1f,3.f,0);wall.collideWalls(capsule,0);
    assert(capsule.x>.49f&&capsule.x<.51f);
    assert(wall.wallCache_&&wall.wallCache_->builds==1&&wall.wallCache_->hits>0);
    const auto preparedBefore=wall.wallCache_->prepared;
    for(int i=0;i<500;++i){glm::vec3 q(.1f,3.f,0);wall.collideWalls(q,0);assert(q.x>.49f&&q.x<.51f);}
    assert(wall.wallCache_->prepared==preparedBefore); // no repeated transforms/list search
    std::printf("500 repeated wall queries reused %u prepared triangles without rebuilding geometry.\n",preparedBefore);
    // A moving placement invalidates prepared geometry, even without a generation change.
    p.position[0]=.2f;wc.collisionLo.x+=.2f;wc.collisionHi.x+=.2f;
    wall.collisionTransforms_[0].collisionLo=wc.collisionLo;wall.collisionTransforms_[0].collisionHi=wc.collisionHi;
    glm::vec3 shifted(.3f,3.f,0);wall.collideWalls(shifted,0);assert(std::fabs(shifted.x-.7f)<.001f);
    assert(wall.wallCache_->builds==2);
    ++wall.gridGen_;shifted={.3f,3.f,0};wall.collideWalls(shifted,0);assert(wall.wallCache_->builds==3);
    // More prepared triangles than the fixed cache can hold must use the full
    // resolver, never a truncated list. Duplicate references retain their order.
    auto denseBytes=square();auto listOffset=reinterpret_cast<CollisionHeader*>(denseBytes.data())->listOffset;
    denseBytes.resize(listOffset+200*sizeof(uint32_t));
    auto* denseHeader=reinterpret_cast<CollisionHeader*>(denseBytes.data());denseHeader->listCount=200;
    auto* denseCells=reinterpret_cast<uint32_t*>(denseBytes.data()+denseHeader->cellOffset);denseCells[1]=200;
    auto* denseList=reinterpret_cast<uint32_t*>(denseBytes.data()+listOffset);
    for(unsigned i=0;i<200;++i)denseList[i]=i%2;
    assert(collisionValid(denseBytes.data(),static_cast<uint32_t>(denseBytes.size())));
    wall.models_[0].col=denseHeader;++wall.gridGen_;
    glm::vec3 denseFast(.3f,3.f,0),denseFull=denseFast;
    g_cpuFast=true;wall.collideWalls(denseFast,0);assert(wall.wallCache_->overflows==1);
    g_cpuFast=false;wall.collideWalls(denseFull,0);assert(glm::length(denseFast-denseFull)<.00001f);
    const auto builds=wall.wallCache_->builds;
    g_cpuFast=true;denseFast={.3f,3.f,0};wall.collideWalls(denseFast,0);
    assert(wall.wallCache_->builds==builds); // memoized overflow avoids repeated failed preparation
    wall.models_[0].col=reinterpret_cast<const CollisionHeader*>(bytes.data());++wall.gridGen_;
    // Crossing a collision-grid boundary changes the key; retain duplicated
    // references in the original cell/list order rather than deduplicating them.
    auto multiBytes=square();const auto cellOffset=reinterpret_cast<CollisionHeader*>(multiBytes.data())->cellOffset;
    multiBytes.resize(cellOffset+16+16);auto* multi=reinterpret_cast<CollisionHeader*>(multiBytes.data());
    multi->nx=2;multi->bmax[0]=15;multi->listCount=4;multi->listOffset=cellOffset+16;
    uint32_t multiCells[]={0,2,2,2},multiList[]={0,1,0,1};
    std::memcpy(multiBytes.data()+cellOffset,multiCells,16);
    std::memcpy(multiBytes.data()+multi->listOffset,multiList,16);
    assert(collisionValid(multiBytes.data(),static_cast<uint32_t>(multiBytes.size())));
    WallCollisionCache multiCache;
    const auto* one=multiCache.get(multi,1,7,axes,{0,0,0},{-4,-4},{4,4});assert(one&&one->count==2);
    const auto* two=multiCache.get(multi,1,7,axes,{0,0,0},{-4,-4},{6,4});assert(two&&two->count==4);
    assert(two->triangles[0].a==two->triangles[2].a&&two->triangles[1].b==two->triangles[3].b);
    assert(multiCache.get(multi,1,7,axes,{0,0,0},{-3,-3},{3,3})==one&&multiCache.builds==2);
    // A hot entry survives a cold third instance (the old round-robin evicted it).
    WallCollisionCache lru;
    assert(lru.get(multi,1,1,axes,{0,0,0},{-4,-4},{4,4}));
    assert(lru.get(multi,1,2,axes,{0,0,0},{-4,-4},{4,4}));
    assert(lru.get(multi,1,1,axes,{0,0,0},{-4,-4},{4,4}));
    assert(lru.get(multi,1,3,axes,{0,0,0},{-4,-4},{4,4}));
    const auto lruBuilds=lru.builds;
    assert(lru.get(multi,1,1,axes,{0,0,0},{-4,-4},{4,4})&&lru.builds==lruBuilds);
    assert(lru.capacityMisses==1&&lru.coldMisses==2);
    assert(lru.get(multi,2,1,axes,{0,0,0},{-4,-4},{4,4})&&lru.generationMisses==1);
    assert(lru.get(multi,2,1,axes,{.1f,0,0},{-4,-4},{4,4})&&lru.transformMisses==1);
    assert(lru.get(multi,2,1,axes,{.1f,0,0},{-4,-4},{6,4})&&lru.cellMisses>0);
    // Moving capsule: cached candidate resolution must match the original
    // stable full scan, including successive pushes at corners and new cells.
    ModelRenderer town;town.models_.resize(1);town.models_[0].col=wall.models_[0].col;
    std::vector<DoodadPlacement> walls(128);std::vector<CollisionBoundsIndex::Entry> wallBounds;
    for(unsigned i=0;i<walls.size();++i){
        auto& wp=walls[i];
        glm::mat3 a(glm::rotate(glm::mat4(1),glm::radians(90.f),glm::vec3(i%2?1.f:0.f,i%2?0.f:1.f,0.f)));
        glm::vec3 pos(static_cast<float>(i/4)*30.f,static_cast<float>(i%4)*3.f,0);
        for(int c=0;c<3;++c)for(int r=0;r<3;++r)wp.axes[c*3+r]=a[c][r];
        for(int r=0;r<3;++r)wp.position[r]=pos[r];
        ModelRenderer::Inst in{pos.x,pos.y,pos.z,8,1.f,&wp,&town.models_[0]};
        ModelRenderer::CollisionTransform col;col.inverse=glm::inverse(a);
        collisionWorldBounds({-5,-5,0},{5,5,0},a,pos,col.collisionLo,col.collisionHi);
        town.collisionTransforms_.push_back(col);in.collision=town.collisionTransforms_.size();
        town.insts_.push_back(in);town.collisionOrder_.push_back(i);
        wallBounds.push_back({col.collisionLo,col.collisionHi,i});
    }
    town.collisionBounds_.build(wallBounds);
    std::uniform_real_distribution<float> offset(-6.f,6.f);
    uint64_t cachedVisits=0,scanVisits=0;
    for(unsigned q=0;q<12000;++q){
        const auto& in=town.insts_[q%walls.size()];
        glm::vec3 a(in.x+offset(rng),in.y+offset(rng),0),b=a;
        unsigned before=g_cpuStats.colInsts;g_cpuFast=true;town.collideWalls(a,0);
        cachedVisits+=g_cpuStats.colInsts-before;
        before=g_cpuStats.colInsts;g_cpuFast=false;town.collideWalls(b,0);
        scanVisits+=g_cpuStats.colInsts-before;
        assert(glm::length(a-b)<.00001f);
    }
    assert(cachedVisits<scanVisits/8);
    std::printf("12000 moving/corner wall queries match full scan; visits %llu -> %llu.\n",
        static_cast<unsigned long long>(scanVisits),static_cast<unsigned long long>(cachedVisits));
    std::printf("Collision recovery passed; %u transformed floor queries match brute force, %llu indexed visits.\n",tests,static_cast<unsigned long long>(indexedVisits));
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
glm=root.parent/'tooling/glm-1.0.1'
with tempfile.TemporaryDirectory(prefix='wowee-model-collision-') as temp:
    folder=Path(temp);src=folder/'collision.cpp';exe=folder/'collision.exe'
    src.write_text(cpp+helpers+functions+main)
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/I'+str(root/'include'),'/I'+str(glm),str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'collision.obj')]
    else:
        args=[compiler,'-std=c++20','-O2','-I'+str(root/'include'),'-I'+str(glm),str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
