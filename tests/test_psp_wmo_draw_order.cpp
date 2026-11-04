#ifdef NDEBUG
#undef NDEBUG
#endif
#include "rendering/psp/psp_wmo_draw_order.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace wowee::rendering::psp;
struct Item {
    float dist;uint16_t texture,cell;const void* blob;uint8_t blend;unsigned id;uint32_t depthBand;
    Item(float d,uint16_t t,uint16_t c,const void* b,uint8_t mode,unsigned i)
        :dist(d),texture(t),cell(c),blob(b),blend(mode),id(i),depthBand(wmoDepthBucket(d)){}
};
int main(){
    std::vector<Item> opaque;
    for(unsigned i=0;i<8;++i)opaque.push_back({4.f+i*40.f,static_cast<uint16_t>(7-i),0,nullptr,kBlendOpaque,i});
    auto old=opaque,ordered=opaque;
    std::sort(old.begin(),old.end(),[](const auto& a,const auto& b){return wmoDrawBefore(a,b,false);});
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return wmoDrawBefore(a,b,true);});
    // Simulate fully overlapping opaque surfaces: depth writes must reduce
    // successful farther fragments without removing any geometry submissions.
    auto fragments=[](const auto& list){float depth=10000;unsigned n=0;for(const auto& i:list)if(i.dist<depth){depth=i.dist;++n;}return n;};
    assert(old.size()==ordered.size()&&fragments(old)==8&&fragments(ordered)==1);
    std::vector<Item> mixed=opaque;
    mixed.push_back({9,0,0,nullptr,kBlendAlpha,90});
    mixed.push_back({100,0,0,nullptr,kBlendAlpha,91});
    mixed.push_back({2,8,0,nullptr,kBlendAlphaKey,92});
    mixed.push_back({80,0,0,nullptr,kBlendAdd,93});
    for(const auto& a:mixed)for(const auto& b:mixed){
        assert(!wmoDrawBefore(a,a,true));
        if(wmoDrawBefore(a,b,true))assert(!wmoDrawBefore(b,a,true));
        for(const auto& c:mixed)if(wmoDrawBefore(a,b,true)&&wmoDrawBefore(b,c,true))assert(wmoDrawBefore(a,c,true));
    }
    std::sort(mixed.begin(),mixed.end(),[](const auto& a,const auto& b){return wmoDrawBefore(a,b,true);});
    assert(mixed[9].id==91&&mixed[10].id==93&&mixed[11].id==90);
    Item a{2,9,0,nullptr,kBlendOpaque,0},b{28,1,0,nullptr,kBlendOpaque,1};
    assert(wmoDrawBefore(b,a,true)); // retain texture batching within a depth band
    assert(wmoDepthBucket(-1)==0&&wmoDepthBucket(31.9f)==0&&wmoDepthBucket(32)==1);
    std::puts("WMO order passed: opaque occlusion simulation, alpha-key depth, transparency order, texture runs, strict ordering.");
}
