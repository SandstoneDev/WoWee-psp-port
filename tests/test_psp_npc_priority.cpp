#include "rendering/psp/psp_npc_priority.hpp"
#include <algorithm>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vector>
using namespace wowee::rendering::psp;
int main() {
    std::vector<NpcPriority> units={
        {false,false,1,99}, // off-screen target/near NPC
        {false,true,900,30}, // visible but distant mount
        {false,true,9,20},
        {false,true,4,11},
        {false,true,4,10}
    };
    std::sort(units.begin(),units.end(),npcPriorityBefore);
    assert(units[0].guid==10 && units[1].guid==11 && units[2].guid==20);
    assert(units[3].guid==30 && units[4].guid==99);
    // Distance supplied by the caller is from the player, not the orbit camera.
    assert(npcPriorityBefore({false,true,4,1},{false,true,25,2}));
    assert(npcPriorityBefore({true,false,100,3},{false,true,1,4}));
    assert(!npcPriorityBefore(units[0],units[0]));
    assert(npcWithinDrawBudget(true,true,false,40,24));
    assert(!npcWithinDrawBudget(true,false,false,40,24));
    assert(!npcWithinDrawBudget(false,true,false,40,24));
    assert(npcWithinDrawBudget(false,false,true,40,24));
}
