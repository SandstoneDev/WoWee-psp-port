#include "rendering/psp/psp_marker_visibility.hpp"
#include <cassert>
#include <cstdio>
int main(){
    wowee::rendering::psp::MarkerVisibilityCache cache;int casts=0;bool wall=true;
    auto occluded=[&](const glm::vec3& origin,const glm::vec3& dir,float length){++casts;assert(length>0);return wall;};
    assert(!cache.visible(1,{0,0,0},{10,0,2},0,occluded));
    assert(!cache.visible(1,{0,0,0},{10,0,2},50,occluded));assert(casts==1);
    wall=false;assert(cache.visible(1,{0,0,0},{10,0,2},100,occluded));assert(casts==2);
    wall=true;assert(!cache.visible(1,{0,1,0},{10,0,2},101,occluded));assert(casts==3);
    wall=false;assert(cache.visible(1,{0,1,0},{10,1,2},102,occluded));assert(casts==4);
    for(unsigned i=2;i<100;++i)assert(cache.visible(i,{0,0,0},{10,0,2},103,occluded));
    assert(cache.visible(1000,{0,0,0},{0,0,0},103,occluded));
    std::puts("Quest markers: wall occlusion, short cache, camera/target invalidation and bounded replacement passed.");
}
