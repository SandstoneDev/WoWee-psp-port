#include "rendering/psp/psp_fog_policy.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <cmath>
using namespace wowee::rendering::psp;
int main() {
    const auto low=fitFogRange(500,1000,worldFarClip(true));
    assert(std::abs(low.end-173.46f)<.01f);
    assert(std::abs(low.start/low.end-.5f)<.0001f);
    const auto zone=fitFogRange(20,80,worldFarClip(true));
    assert(zone.start==20 && zone.end==80); // authored range fitting remains unchanged
    const auto longer=fitFogRange(500,1000,worldFarClip(false));
    assert(longer.end>low.end);
    const auto invalid=fitFogRange(std::numeric_limits<float>::quiet_NaN(),-1,177);
    assert(std::isfinite(invalid.start) && invalid.start>=0 && invalid.start<invalid.end);
    const auto inverted=fitFogRange(500,80,177);
    assert(inverted.start<inverted.end);
    const auto narrow=fitFogRange(420,440,450);
    assert(narrow.end==440 && narrow.end-narrow.start>=440*.279f);
    // Both normal and adaptive model limits: no contrast at the cull edge,
    // monotonically reveal toward the camera, unchanged nearby geometry.
    const FogRange clearWorld{250,440};
    for(float limit:{40.f,80.f,160.f,256.f}){
        assert(objectDistanceVisibility(limit,limit)==0.f);
        assert(objectDistanceVisibility(0,limit)==1.f);
        float previous=0;
        for(int i=0;i<=100;++i){
            const float d=limit*(1.f-i/100.f);
            const float visibility=objectDistanceVisibility(d,limit);
            assert(visibility>=previous && visibility>=0 && visibility<=1);previous=visibility;
            const float start=objectFogStart(clearWorld,d,visibility);
            assert(std::isfinite(start) && start<=clearWorld.start);
            const float contrast=std::clamp((clearWorld.end-d)/(clearWorld.end-start),0.f,1.f);
            if(i==0)assert(contrast<.001f);
        }
        assert(objectFogStart(clearWorld,0,1)==clearWorld.start);
    }
    assert(objectResidentVisibility(0)==0 && objectResidentVisibility(200)==.5f);
    assert(objectResidentVisibility(400)==1 && objectResidentVisibility(5000)==1);
    // Unsigned ages remain correct through the millisecond counter wrap.
    const uint32_t born=0xFFFFFF00u,now=0x90u;
    assert(objectResidentVisibility(now-born)==1);
    // A model already hidden by normal zone fog needs no extra per-object state.
    assert(objectFogStart(clearWorld,500,.25f)==clearWorld.start);
}
