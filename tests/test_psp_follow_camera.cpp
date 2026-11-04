#include "ui/psp/psp_follow_camera.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
using wowee::ui::psp::SmartFollowCamera;
int main() {
    const auto step=SmartFollowCamera::follow;
    assert(step(1,0,.1f,true,false,false)==1); // idle preserves view
    assert(step(1,0,.1f,true,true,true)==1); // R manual orbit wins
    assert(step(1,0,.1f,false,false,true)==1);
    assert(step(1,0,0,true,false,true)==1);
    const float first=step(1,0,1.f/60,true,false,true);
    assert(first<1 && first>.95f); // immediate but old-style smooth, not a snap
    assert(std::abs(first-std::exp(-1.6f/60))<.00001f);
    const float wrapped=step(3.13f,-3.13f,.016f,true,false,true);
    assert(wrapped>3.13f && wrapped<3.16f);
    float a=1.57f,b=a;
    for(int i=0;i<120;++i)a=step(a,0,1.f/60,true,false,true);
    for(int i=0;i<240;++i)b=step(b,0,1.f/120,true,false,true);
    assert(std::abs(a-b)<.0001f && std::abs(a)<.07f);
    assert(SmartFollowCamera::movementBasis(1,2,false)==1);
    assert(SmartFollowCamera::movementBasis(1.5f,2,false)==1.5f); // every frame uses current view
    assert(SmartFollowCamera::movementBasis(1,2,true)==2); // autorun keeps facing
    // Each frame: steer first, then follow current character facing, in every direction.
    float view=.5f;
    for(int i=0;i<120;++i) {
        const float input=i*.08f;
        const float basis=SmartFollowCamera::movementBasis(view,0,false);
        const float facing=basis+input;
        float delta=std::remainder(facing-view,6.28318530718f);
        if (std::abs(std::abs(delta)-3.14159265359f)<.0001f) delta=3.14159265359f;
        const float expected=view+delta*(1-std::exp(-1.6f/60));
        view=step(view,facing,1.f/60,true,false,true);
        assert(std::abs(view-expected)<.0011f);
    }
    // Straight-down movement keeps a stable world direction while the camera
    // completes its half-turn, instead of chasing a heading that moves with it.
    wowee::ui::psp::ReverseStickTurn reverse;
    float camera=.7f;
    const float initial=camera;
    for (int i=0;i<300;++i) {
        const float basis=reverse.basis(camera,0,false,true,0,1,false);
        assert(std::abs(basis-initial)<.00001f);
        const float heading=basis+3.14159265359f;
        camera=step(camera,heading,1.f/60,true,false,true);
    }
    assert(std::abs(std::remainder(camera-(initial+3.14159265359f),6.28318530718f))<.002f);
    assert(reverse.basis(camera,0,false,true,.5f,.8f,false)==camera); // steering releases the special case
    reverse.basis(camera,0,false,true,0,1,false);
    assert(reverse.basis(1,0,false,true,0,0,false)==1); // release resets it
    assert(reverse.basis(2,0,false,true,0,1,true)==2); // manual orbit wins
    assert(reverse.basis(2,3,true,true,0,1,false)==3); // autorun wins
    assert(reverse.basis(2,0,false,false,0,1,false)==2); // old camera unchanged

}
