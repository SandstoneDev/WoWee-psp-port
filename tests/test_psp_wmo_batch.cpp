#include "rendering/psp/psp_wmo_batch_cache.hpp"
#include <cassert>
#include <cmath>
#include <set>
#include <tuple>
#include <cstdio>
using namespace wowee::rendering::psp;
std::vector<uint8_t> blob(unsigned start,bool indexed,unsigned count=4,uint8_t blend=0) {
    ChunkMeshHeader h{};h.flags=indexed?kChunkIndexed:0;h.vertexCount=count;h.batchCount=1;
    h.uvMin[0]=start*.1f;h.uvMin[1]=-2;h.uvScale[0]=2;h.uvScale[1]=4;
    h.batchOffset=sizeof(h);h.vertexOffset=h.batchOffset+sizeof(ModelBatch);h.indexOffset=h.vertexOffset+count*sizeof(ModelVertex);h.indexCount=count;
    std::vector<uint8_t>b(h.indexOffset+count*2);std::memcpy(b.data(),&h,sizeof(h));
    ModelBatch batch{};batch.indexCount=count;batch.blend=blend;std::memcpy(b.data()+h.batchOffset,&batch,sizeof(batch));
    for(unsigned i=0;i<count;++i){ModelVertex v{static_cast<uint16_t>(i*100),static_cast<uint16_t>(i*200),123,static_cast<int16_t>(start+i),static_cast<int16_t>(i%2),static_cast<int16_t>(i/2)};std::memcpy(b.data()+h.vertexOffset+i*sizeof(v),&v,sizeof(v));uint16_t ix=i;std::memcpy(b.data()+h.indexOffset+i*2,&ix,2);}
    return b;
}
using Tri=std::tuple<int,int,int>;
std::multiset<Tri> tris(const std::vector<WmoBatchVertex>& v){
    std::multiset<Tri> out;
    for(size_t i=2;i<v.size();++i){int a=v[i-2].x,b=v[i-1].x,c=v[i].x;if(a==b || a==c || b==c)continue;if(i&1)std::swap(a,b);out.emplace(a,b,c);}
    return out;
}
int main(){
    auto a=blob(0,true,5),b=blob(20,false,4),c=blob(40,true,5);
    WmoBatchCache cache;WmoBatchCache::Key keys[]={{a.data(),0},{b.data(),0},{c.data(),0}};
    const auto* merged=cache.build(keys,3,1);assert(merged);
    assert(merged->indices.size()==19 && merged->vertices.size()==14); // odd first strip adds a parity connector
    std::multiset<Tri> expected;expected.emplace(0,1,2);expected.emplace(2,1,3);expected.emplace(2,3,4);expected.emplace(20,21,22);expected.emplace(22,21,23);expected.emplace(40,41,42);expected.emplace(42,41,43);expected.emplace(42,43,44);
    std::vector<WmoBatchVertex> expanded;for(auto ix:merged->indices)expanded.push_back(merged->vertices[ix]);assert(tris(expanded)==expected);
    for(const auto& v:merged->vertices){unsigned base=v.x>=40?40:v.x>=20?20:0;unsigned i=v.x-base;assert(v.color==123 && v.y==i%2 && v.z==i/2);assert(std::fabs(v.u-(i*100/32768.f*2+base*.1f))<.00001f);assert(std::fabs(v.v-(i*200/32768.f*4-2))<.00001f);}
    assert(cache.find(keys,3,2)==merged); // no conversion or allocation on a hit
    const auto* pinned=merged->vertices.data();auto backup=merged->vertices;
    std::swap(keys[0],keys[1]);assert(cache.build(keys,3,2));std::swap(keys[1],keys[2]);assert(cache.build(keys,3,2));std::swap(keys[0],keys[2]);assert(cache.build(keys,3,2));
    std::swap(keys[0],keys[1]);assert(!cache.build(keys,3,2)); // all four slots referenced by this GE list
    assert(std::memcmp(pinned,backup.data(),backup.size()*sizeof(WmoBatchVertex))==0);
    assert(cache.bytes()<=65536);
    auto alpha=blob(60,false,4,kBlendAlpha);WmoBatchCache::Key bad[]={{a.data(),0},{alpha.data(),0}};assert(!cache.build(bad,2,3));
    auto huge=blob(100,true,960);bad[1]={huge.data(),0};assert(!cache.build(bad,2,3));
    cache.clear();assert(cache.bytes()==0 && !cache.find(keys,3,4));
    std::puts("WMO batches: identical triangles/colours/positions/UVs, strip parity, indexed/nonindexed, pinned GE buffers, memory cap, blend and oversize fallback passed.");
}
