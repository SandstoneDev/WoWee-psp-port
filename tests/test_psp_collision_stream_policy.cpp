#include "rendering/psp/psp_collision_stream_policy.hpp"
#include <cassert>
#include <initializer_list>
using namespace wowee::rendering::psp;
int main() {
    // Undercity and its entrance wall overlap: after city collision is ready,
    // the wall still needs a header even in the PSP low-descriptor mode.
    assert(collisionOwnerOpenAllowed(true,1,2,true));
    assert(!collisionOwnerOpenAllowed(true,1,2,false));
    assert(collisionOwnerOpenAllowed(true,0,2,false));
    assert(!collisionOwnerOpenAllowed(true,2,2,true));
    assert(collisionOwnerOpenAllowed(false,1,2,false));
    assert(collisionDoodadsReady(true,true,true,true)); // resident tile + top-up job: keep walking
    assert(collisionDoodadsReady(true,true,true,false));
    assert(!collisionDoodadsReady(true,false,true,true)); // initial geometry must arrive
    assert(!collisionDoodadsReady(false,false,false,false));
    assert(collisionDoodadsReady(true,false,false,false)); // a tile with no props
    // Memory pressure must not deadlock the movement-ready check: essential
    // collision can issue, while visual and distant collision reads defer.
    assert(collisionReadAllowed(true,true,collisionStreamPriority(1.5f)));
    assert(!collisionReadAllowed(true,true,collisionStreamPriority(8.f)));
    assert(!collisionReadAllowed(true,false,-10000.f));
    assert(collisionReadAllowed(false,false,100.f));
    // A nearby collision chunk larger than the visual budget remains in the
    // plan. It cannot be cut while collisionReadyAt still requires it.
    assert(collisionPlanFits(900,200,1000,true));
    assert(!collisionPlanFits(900,200,1000,false));
    assert(kCollisionLandingRadius<kCollisionMandatoryRadius);
    assert(collisionPrefetchRadius(40)==10.f);
    assert(collisionPrefetchRadius(0)==kCollisionMandatoryRadius);
    assert(collisionStreamPriority(2)<0 && collisionStreamPriority(8)>32);
    // An oversized rejected blob must not spend the remaining visual budget.
    uint32_t used=100;
    assert(!collisionBudgetFits(used,950,1000));
    assert(collisionBudgetFits(used,400,1000));
    used+=400;
    assert(collisionBudgetFits(used,500,1000));
    assert(!collisionBudgetFits(1100,1,1000));
    assert(collisionStreamNearby(0,0,-1,0,0,1,12));
    assert(!collisionStreamNearby(0,0,-100,-60,0,1,12)); // underground city does not block courtyard
    assert(!collisionStreamNearby(0,0,20,40,0,1,12));
    assert(!collisionStreamNearby(13,0,-1,0,0,1,12));
    assert(collisionStreamNearby(6,0,-1,0,0,2,12)); // scaled placement
    assert(!collisionStreamNearby(7,0,-1,0,0,2,12));
    for (float scale : {0.5f,1.f,2.f})
        for (int d=0;d<=12;++d)
            assert(!collisionStreamNearby(d/scale,0,-1,0,0,scale,12) ||
                    collisionStreamNearby(d/scale,0,-1,0,0,scale,14));
}
