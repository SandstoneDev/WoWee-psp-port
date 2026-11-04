#ifdef NDEBUG
#undef NDEBUG
#endif
#include "rendering/psp/psp_wmo_group_cache.hpp"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace wowee::rendering::psp;
int main(){
    WmoGroupCache cache;
    unsigned queries=0;
    int room=52;
    auto camera=[&]{++queries;return room;};
    auto player=[&]{++queries;return 15;};
    for(int frame=0;frame<1800;++frame){
        assert(cache.resolve({1600,240,-40},4,camera)==52);
        assert(cache.resolve({1601,240,-41},4,player)==15);
    }
    assert(queries==2);
    // Any movement, even much less than a collision microstep, queries afresh.
    room=53;assert(cache.resolve({1600.0001f,240,-40},4,camera)==53);assert(queries==3);
    room=-1;assert(cache.resolve({1600,240,-40},3,camera)==-1);assert(queries==4);
    // A missing room must be recomputed once new geometry arrives at the same point.
    room=54;cache.invalidate();assert(cache.resolve({1600,240,-40},3,camera)==54);assert(queries==5);
    // Evicted collision cannot leave the old room selected.
    room=-1;cache.invalidate();assert(cache.resolve({1600,240,-40},3,camera)==-1);assert(queries==6);
    // Real movement/rotating cameras produce misses: bounded storage, fresh results.
    for(int i=0;i<1000;++i){room=i;assert(cache.resolve({float(i),0,0},4,camera)==i);}
    room=9001;assert(cache.resolve({0,0,0},4,camera)==9001);
    const float nan=std::numeric_limits<float>::quiet_NaN();
    room=5;assert(cache.resolve({nan,0,0},4,camera)==5);
    room=6;assert(cache.resolve({nan,0,0},4,camera)==6);
    std::puts("WMO group cache passed: exact positions, room changes, modes, loading/eviction invalidation, bounded storage.");
}
