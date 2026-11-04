#include "platform/psp/psp_frame_budget.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
using wowee::platform::psp::FrameBudget;
int main() {
    assert(wowee::platform::psp::frameIntervalForTarget(30)==2);
    assert(wowee::platform::psp::frameIntervalForTarget(60)==1);
    FrameBudget budget;
    // Idle vblank time is not fed to the controller. A 20 ms workload at a
    // 30 FPS presentation rate recovers to full detail rather than cutting it.
    for (int i=0;i<300;++i) budget.sample(20000,1.f/30,true);
    assert(budget.level()==0);
    budget.sample(500000,1.f,true); // one streaming stall does not collapse detail
    assert(budget.level()==0);
    for (int i=0;i<60;++i) budget.sample(20000,1.f/30,true);
    assert(budget.level()==0);
    for (int i=0;i<120;++i) budget.sample(40000,.04f,true);
    assert(budget.level()==4);
    assert(budget.distance()>=.5f && budget.cosmetic()>0 && budget.spells()>0);
    assert(budget.crowdLimit(24,8)==9);
    assert(budget.crowdLimit(5,8)==5); // never increase a low requested cap
    assert(budget.crowdLimit(0,8)==0); // NPCs Off stays Off
    for (int i=0;i<90;++i) budget.sample(20000,1.f/30,true);
    assert(budget.level()==4); // headroom needs several seconds before recovery
    for (int i=0;i<1200;++i) budget.sample(20000,1.f/30,true);
    assert(budget.level()==0);
    for (int i=0;i<120;++i) budget.sample(40000,.04f,true);
    assert(budget.level()>0);
    budget.sample(40000,.04f,false);
    assert(budget.level()==0 && budget.distance()==1 && budget.cosmetic()==1 && budget.spells()==1);
    assert(budget.crowdLimit(24,8)==24);
    budget.sample(0,0,true);
    assert(budget.level()==1); // re-enable begins at balanced quality
    for (int i=0;i<6;++i) budget.sample(110000,.1f,true);
    assert(budget.level()==5);
    assert(budget.crowdLimit(24,8)==2 && budget.crowdLimit(1,8)==1 && budget.crowdLimit(0,8)==0);
    assert(budget.distance()==.3f && budget.cosmetic()==0);
    const int level=budget.level();
    budget.sample(40000,std::numeric_limits<float>::quiet_NaN(),true);
    assert(budget.level()==level);
    // The same 20ms scene fits 30 FPS, but exceeds the 60 FPS work budget.
    FrameBudget fast;
    for (int i=0;i<300;++i) fast.sample(20000,1.f/60,true,60);
    assert(fast.targetFps()==60 && fast.level()==4);
    // Returning to 30 FPS retains the current tier, then recovers with hysteresis.
    fast.sample(0,0,true,30);
    assert(fast.level()==4 && fast.workUs()==0 && fast.targetFps()==30);
    for (int i=0;i<1200;++i) fast.sample(20000,1.f/30,true,30);
    assert(fast.level()==0);
    // One slow frame cannot trigger the 60 FPS emergency tier.
    fast.sample(110000,.1f,true,60);
    assert(fast.level()==0);
    for (int i=0;i<120;++i) fast.sample(40000,.04f,true,60);
    assert(fast.level()==5);
    for (int i=0;i<2500;++i) fast.sample(9000,1.f/60,true,60);
    assert(fast.level()==0);
    for (int target : {30,60}) {
        fast.sample(110000,.1f,false,target);
        assert(fast.level()==0 && fast.distance()==1 && fast.cosmetic()==1);
        assert(fast.targetFps()==target && fast.crowdLimit(24,8)==24);
    }
    // Ordinary headroom and occasional packet/card blips must not leave quality stuck low.
    FrameBudget recovery;
    for(int i=0;i<150;++i)recovery.sample(40000,.04f,true,30);
    assert(recovery.level()==4);
    for(int i=0;i<750;++i)recovery.sample(i%45==0?36000:26000,1.f/30,true,30);
    assert(recovery.level()==0 && recovery.distance()==1 && recovery.terrain()==1 && recovery.cosmetic()==1);
    for(int i=0;i<150;++i)recovery.sample(24000,.02f,true,60);
    assert(recovery.level()>0);
    for(int i=0;i<1500;++i)recovery.sample(13000,1.f/60,true,60);
    assert(recovery.level()==0);

    // Near-target work still has room for a cautious restore. Previously these
    // steady 30/60 FPS scenes remained permanently at their lowest tier.
    for(int target : {30,60}) {
        FrameBudget nearTarget;
        for(int i=0;i<150;++i)nearTarget.sample(40000,.04f,true,target);
        assert(nearTarget.level()>=4);
        const uint32_t work=target==30?29500u:14750u;
        for(int i=0;i<1200;++i)nearTarget.sample(work,1.f/target,true,target);
        assert(nearTarget.level()==0 && nearTarget.distance()==1 && nearTarget.terrain()==1 && nearTarget.cosmetic()==1);
        // If restored quality costs too much, the existing quick reduction wins.
        for(int i=0;i<60;++i)nearTarget.sample(target==30?36000:18000,1.f/target,true,target);
        assert(nearTarget.level()>0);
        const int reduced=nearTarget.level();
        for(int i=0;i<30;++i)nearTarget.sample(work,1.f/target,true,target);
        assert(nearTarget.level()>=reduced); // no instant restore while the average is still settling
        for(int i=0;i<1800;++i)nearTarget.sample(i%90==0?(target==30?34000:17000):work,1.f/target,true,target);
        assert(nearTarget.level()==0); // isolated network/card spikes cannot pin quality low
    }

}
