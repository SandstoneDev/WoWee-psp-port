#include "platform/psp/psp_rest_triggers.hpp"
#include <cassert>
#include <initializer_list>
#include <cstdio>
using namespace wowee::platform::psp;
int main(){
    // Brill inn login/transfer replay, including the first sample during grace.
    for(bool first:{false,true})for(bool cooldown:{false,true}) {
        assert(!suppressAreaTrigger(719,first,cooldown,false));
        assert(!suppressAreaTrigger(562,first,cooldown,false));
    }
    assert(isRestAreaTrigger(4108) && isRestAreaTrigger(3547));
    assert(!isRestAreaTrigger(0) && !isRestAreaTrigger(45));
    assert(suppressAreaTrigger(45,true,false,false)); // portal spawn suppression remains
    assert(suppressAreaTrigger(45,false,true,false));
    assert(suppressAreaTrigger(45,false,false,true));
    assert(!suppressAreaTrigger(45,false,false,false));
    assert(showRestStatus(true,false,false)); // even when controller combat mode is selected
    assert(!showRestStatus(false,false,false));
    assert(!showRestStatus(true,true,false));
    assert(!showRestStatus(true,false,true));
    std::puts("Inn notifications bypass portal grace; portal protection and real-combat status priority passed.");
}
