#include "rendering/psp/psp_world_decoration.hpp"
#include <cassert>
#include <initializer_list>
#include <cstdio>
int main(){using namespace wowee::rendering::psp;
 assert(excludedGroundDecoration(kModelGroundDetail));
 assert(excludedGroundDecoration(kModelGroundDetail|kModelFoliage|kModelGrassPatch));
 for(auto flags:{0u,static_cast<unsigned>(kModelBuilding),static_cast<unsigned>(kModelFoliage)})assert(!excludedGroundDecoration(flags));
 uint32_t next=0;unsigned scans=0;
 for(uint32_t ms=0;ms<1000;++ms)if(pollStreamReadiness(ms,next))++scans;
 assert(scans==4);assert(!pollStreamReadiness(999,next));assert(pollStreamReadiness(1000,next));
 next=0xfffffff0u;assert(pollStreamReadiness(0xfffffff0u,next));assert(!pollStreamReadiness(5,next));assert(pollStreamReadiness(234,next));
 std::puts("Readiness checks limited to four per second; old-pack decoration excluded, buildings/trees retained.");
}
