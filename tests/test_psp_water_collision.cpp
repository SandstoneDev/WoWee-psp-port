#include "rendering/psp/psp_collision_math.hpp"
#include "rendering/psp/psp_water_format.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>
using namespace wowee::rendering::psp;
int main() {
    const glm::vec3 a(0,0,0), b(1,0,0), c(0,1,0), down(0,0,-1);
    assert(collisionRayTri({0.5f,0.5f,10},down,a,b,c) == 10);
    assert(collisionRayTri({0.5f,0.500001f,10},down,a,b,c) >= 0); // roundoff on shared seam
    assert(collisionRayTri({0.5f,0.51f,10},down,a,b,c) < 0);
    assert(collisionRayTri({0,0,10},down,a,a,a) < 0);
    assert(collisionRayTri({0,0,-1},down,a,b,c) < 0); // floor above probe
    assert(collisionRayTri({0,0,10},down,c,b,a) == 10); // reversed winding
    assert(collisionWalkable({0,0,1}) && collisionWalkable({0,0,-1}));
    assert(!collisionWalkable({1,0,0}) && !collisionWalkable({0,0,0}));
    for (float speed : {0.f,7.f,14.f,40.f,1000.f}) for (float dt : {0.f,.016f,.033f,.1f,1.f}) {
        auto s=movementSlices(dt,speed);
        assert(s.count>=1 && s.count<=64 && s.dt*speed<=.25001f && s.dt<=1.f/60+.00001f);
    }
    // A thin wall skipped by a 1.4 yd endpoint step must be reached on the approach side.
    auto slices=movementSlices(.1f,14);
    float x=-.7f;
    for(int i=0;i<slices.count;i++) {
        float to=x+14*slices.dt;
        if (std::fabs(to)<.5f) to = to < 0 ? -.5f : .5f;
        x=to;
    }
    assert(x<0);
    std::vector<uint8_t> grid(kSurfaceGridBytes);
    SurfaceGridHeader h{{'W','S','R','F'},1,0}; std::memcpy(grid.data(),&h,sizeof(h));
    assert(surfaceGridValid(grid.data(),grid.size()));
    grid.pop_back(); assert(!surfaceGridValid(grid.data(),grid.size()));
    grid.resize(kSurfaceGridBytes+sizeof(WaterTable)+sizeof(WaterPatch));
    h.version=2;std::memcpy(grid.data(),&h,sizeof(h));
    WaterTable t{{'W','W','T','R'},1,1};std::memcpy(grid.data()+kSurfaceGridBytes,&t,sizeof(t));
    auto* p=reinterpret_cast<WaterPatch*>(grid.data()+kSurfaceGridBytes+sizeof(t));
    *p={};p->chunk=255;p->mask[0]=1;p->mask[1]=0x80000000u;
    p->z[0]=0;p->z[9]=2;p->z[1]=4;p->z[10]=20;
    assert(waterCell(*p,0,0)&&waterCell(*p,7,7)&&!waterCell(*p,0,1));
    assert(std::fabs(waterHeight(*p,0,0,.25f,.25f)-1.5f)<1e-5f);
    assert(std::fabs(waterHeight(*p,0,0,.75f,.75f)-11.5f)<1e-5f);
    assert(surfaceGridValid(grid.data(),grid.size()));
    assert(!surfaceGridValid(grid.data(),grid.size()-1));
    p->chunk=256;assert(!surfaceGridValid(grid.data(),grid.size()));p->chunk=0;
    p->z[0]=std::numeric_limits<float>::quiet_NaN();assert(!surfaceGridValid(grid.data(),grid.size()));
    std::puts("collision/water regressions passed");
}
