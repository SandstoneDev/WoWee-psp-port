#include "rendering/psp/psp_tile_budget.hpp"
#include <cassert>
#include <cstdio>
int main(){
    using namespace wowee::rendering::psp;
    TileBudgetMeter meter;meter.reserveBytes=kPlacementReserveBytes;meter.stepBytes=128*1024;
    unsigned free=4*1024*1024,placed=0;
    auto probe=[&](unsigned bytes){return free>=bytes;};
    // Every mesh is already resident. Placement/index allocations must still stop at headroom.
    for(unsigned i=0;i<20000;++i){if(!meter.admit(kPlacementBudgetBytes,probe))break;free-=kPlacementBudgetBytes;++placed;}
    assert(placed>0 && placed<20000 && free>=kPlacementReserveBytes && meter.stopped);
    std::printf("Resident-mesh placements stopped at %u with %u KiB free.\n",placed,free/1024);
}
