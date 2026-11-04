#include "rendering/psp/psp_collision_candidates.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <limits>
using namespace wowee::rendering::psp;
int main() {
    CollisionBoundsIndex index;
    index.build({{{-1,-1,-40},{1,1,-39},3},{{-1,-1,60},{1,1,61},1},{{90,90,0},{91,91,1},7}});
    CollisionCandidates cache;
    const auto* s=cache.get(index,1,0,0,0,0);
    assert(s&&s->count==2&&s->ids[0]==1&&s->ids[1]==3); // stacked floors, stable ordering
    for(int i=0;i<200;++i)assert(cache.get(index,1,-.5f,-.5f,.5f,.5f));
    assert(cache.builds==1&&cache.hits==200);
    assert(cache.get(index,1,90,90,91,91)->ids[0]==7);
    assert(cache.get(index,1,0,0,0,0)->count==2&&cache.builds==2); // shared floor/wall column survives camera query
    index.build({{{-1,-1,70},{1,1,71},5}});
    assert(cache.get(index,2,0,0,0,0)->ids[0]==5); // moved/streamed world invalidates immediately
    assert(cache.get(index,2,10,10,10,10)->count==0); // moving beyond cached bounds refreshes
    std::vector<CollisionBoundsIndex::Entry> dense;
    for(uint32_t i=0;i<200;++i)dense.push_back({{-1,-1,0},{1,1,1},i});
    index.build(dense);
    assert(!cache.get(index,3,0,0,0,0)); // no truncation: full scan fallback
    const auto builds=cache.builds;
    assert(!cache.get(index,3,0,0,0,0)&&cache.builds==builds);
    assert(!cache.get(index,3,std::numeric_limits<float>::quiet_NaN(),0,0,0));
    std::puts("Candidate cache passed: reuse, stacked floors, generation invalidation, boundaries, overflow fallback.");
}
