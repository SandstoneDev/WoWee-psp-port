#include "ui/psp/ui_focus.hpp"
#include <pspctrl.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
using namespace wowee::ui::psp;
using wowee::platform::psp::PadState;
static PadState pad(uint32_t held=0,uint32_t pressed=0,uint32_t released=0) { PadState p; p.held=held;p.pressed=pressed;p.released=released;return p; }
static void grid(FocusRing& f) {
    f.setGroup(0); f.addStop(1,{10,10,20,20});f.addStop(2,{40,10,20,20});f.addStop(3,{70,10,20,20});
}
static void panels(FocusRing& f) {
    f.setGroup(1);f.addStop(10,{10,10,20,20});f.addStop(11,{40,10,20,20});
    f.setGroup(2);f.addStop(20,{100,10,20,20});f.setGroup(0);
}
int main() {
    UiDpadRepeat repeat;
    assert(repeat.step(1,1,.016f)==1);
    assert(repeat.step(1,0,.1f)==0);assert(repeat.step(1,0,.1f)==0);assert(repeat.step(1,0,.1f)==0);
    assert(repeat.step(1,0,.03f)==1);
    assert(repeat.step(0,0,.1f)==0);assert(repeat.step(2,2,.01f)==2);
    // Settings emits one pulse per tap, then waits half a second. Feeding
    // raw held input into FocusRing here would cause a second .32s repeat.
    FocusRing settings;UiDpadRepeat settingsRepeat;
    auto rows=[&](){for(int i=0;i<12;++i)settings.addStop(500+i,{10.f,10.f+i*20,40,18});};
    rows();settings.setFocus(500);settings.update(pad(),.016f);rows();
    auto settingsFrame=[&](bool held,bool pressed,float dt){
        const auto pulse=settingsRepeat.step(held?PSP_CTRL_DOWN:0,pressed?PSP_CTRL_DOWN:0,dt,.5f,.14f,.14f);
        settings.update(pad(pulse,pulse),dt);rows();
    };
    settingsFrame(true,true,.016f);assert(settings.focused()==501);
    for(int i=0;i<4;++i)settingsFrame(true,false,.1f);
    assert(settings.focused()==501);
    settingsFrame(false,false,.016f);settingsFrame(true,true,.016f);
    assert(settings.focused()==502); // a second tap is exactly one step
    for(int i=0;i<4;++i)settingsFrame(true,false,.1f);
    assert(settings.focused()==502);
    settingsFrame(true,false,.1f);assert(settings.focused()==503);
    settingsFrame(true,false,.1f);assert(settings.focused()==503);
    settingsFrame(true,false,.05f);assert(settings.focused()==504);
    FocusRing f;
    grid(f);f.setFocus(1);f.update(pad(),.016f);grid(f);
    f.update(pad(PSP_CTRL_RIGHT,PSP_CTRL_RIGHT),.016f);assert(f.focused()==2);grid(f);
    for(int i=0;i<4;++i) { f.update(pad(PSP_CTRL_RIGHT),.1f);grid(f); }
    assert(f.focused()==3); // holding repeats, without repeated pressed edges
    f.setFocus(1);f.update(pad(),.016f);grid(f);
    f.update(pad(PSP_CTRL_CROSS,PSP_CTRL_CROSS),.016f);assert(!f.activated());grid(f);
    f.update(pad(PSP_CTRL_CROSS|PSP_CTRL_RIGHT,PSP_CTRL_RIGHT),.016f);grid(f);
    f.update(pad(0,0,PSP_CTRL_CROSS),.016f);assert(!f.activated());grid(f); // no release on another widget
    f.update(pad(PSP_CTRL_CROSS,PSP_CTRL_CROSS),.016f);grid(f);
    f.update(pad(0,0,PSP_CTRL_CROSS),.016f);assert(f.activated()==2);grid(f);
    f.update(pad(0,0,PSP_CTRL_CROSS),.016f);assert(!f.activated());grid(f); // no orphan release
    FocusRing g;g.setPanelNavigation(true);panels(g);g.focusGroup(1);g.setFocus(10);
    g.update(pad(),.016f);panels(g);
    g.update(pad(PSP_CTRL_RIGHT,PSP_CTRL_RIGHT),.016f);assert(g.focused()==11);panels(g);
    g.update(pad(),.016f);panels(g);
    g.update(pad(PSP_CTRL_RIGHT,PSP_CTRL_RIGHT),.016f);assert(g.focused()==11);panels(g); // stay in panel
    g.update(pad(PSP_CTRL_LTRIGGER|PSP_CTRL_RIGHT,PSP_CTRL_RIGHT),.016f);assert(g.focused()==20);panels(g);
    g.update(pad(PSP_CTRL_LTRIGGER|PSP_CTRL_LEFT,PSP_CTRL_LEFT),.016f);assert(g.focused()==11);panels(g); // remember selection
    g.setModalGroup(2);g.update(pad(),.016f);assert(g.focused()==20);panels(g);
    g.update(pad(PSP_CTRL_LTRIGGER|PSP_CTRL_LEFT,PSP_CTRL_LEFT),.016f);assert(g.focused()==20);panels(g); // modal capture
    g.setModalGroup(0);
    FocusRing overlap;overlap.addStop(1,{0,0,100,20});overlap.addStop(2,{80,0,10,20});overlap.setFocus(1);
    overlap.update(pad(),.016f);overlap.addStop(1,{0,0,100,20});overlap.addStop(2,{80,0,10,20});
    overlap.update(pad(PSP_CTRL_RIGHT,PSP_CTRL_RIGHT),.016f);assert(overlap.focused()==2);
    FocusRing basic;basic.setEnhancedNavigation(false);grid(basic);basic.setFocus(1);
    basic.update(pad(),.016f);grid(basic);
    basic.update(pad(PSP_CTRL_RIGHT,PSP_CTRL_RIGHT),.016f);grid(basic);
    assert(basic.focused()==2);
    for (int i=0;i<10;++i) { basic.update(pad(PSP_CTRL_RIGHT),.1f);grid(basic); }
    assert(basic.focused()==2); // Basic navigation keeps single-step input
    basic.setEnhancedNavigation(true);
    for (int i=0;i<5;++i) { basic.update(pad(PSP_CTRL_RIGHT),.1f);grid(basic); }
    assert(basic.focused()==3); // Enhanced can be enabled live
    // Character screen regression: its row handler consumes the held direction,
    // preventing the shared FocusRing from skipping the second of three rows.
    FocusRing chars; UiDpadRepeat owner;
    auto declareChars=[&]() { for(int i=0;i<3;++i) chars.addStop(100+i,{300.0f,30.0f+i*30,120,29}); };
    declareChars();chars.setFocus(100);chars.update(pad(),.016f);declareChars();
    auto charFrame=[&](PadState input) {
        const auto key=owner.step(input.held & (PSP_CTRL_UP|PSP_CTRL_DOWN),input.pressed,.016f);
        int index=static_cast<int>(chars.focused()-100);
        if ((key & PSP_CTRL_DOWN) && index<2) chars.setFocus(100+index+1);
        else if ((key & PSP_CTRL_UP) && index>0) chars.setFocus(100+index-1);
        input.held &= ~(PSP_CTRL_UP|PSP_CTRL_DOWN);
        input.pressed &= ~(PSP_CTRL_UP|PSP_CTRL_DOWN);
        input.released &= ~(PSP_CTRL_UP|PSP_CTRL_DOWN);
        chars.update(input,.016f);declareChars();
    };
    charFrame(pad(PSP_CTRL_DOWN,PSP_CTRL_DOWN));assert(chars.focused()==101);
    for(int i=0;i<10;++i) charFrame(pad(PSP_CTRL_DOWN));
    assert(chars.focused()==101); // no second movement before repeat delay
    for(int i=0;i<12;++i) charFrame(pad(PSP_CTRL_DOWN));
    assert(chars.focused()==102);
    charFrame(pad());charFrame(pad(PSP_CTRL_UP,PSP_CTRL_UP));assert(chars.focused()==101);
    // A free cursor over empty modal space must stay free after the stick stops.
    FocusRing mouse; mouse.setPanelNavigation(true); mouse.setGroup(2);
    mouse.addStop(20,{100,100,20,20}); mouse.focusGroup(2); mouse.setModalGroup(2);
    mouse.update(pad(),.016f); mouse.setGroup(2); mouse.addStop(20,{100,100,20,20});
    PadState free=pad(); free.ax=1;
    mouse.update(free,.1f); mouse.setGroup(2); mouse.addStop(20,{100,100,20,20});
    assert(mouse.cursorVisible() && mouse.focused()==0);
    const float freeX=mouse.cursorX();
    mouse.update(pad(),.016f); mouse.setGroup(2); mouse.addStop(20,{100,100,20,20});
    assert(mouse.cursorVisible() && mouse.focused()==0 && mouse.cursorX()==freeX);
    // A newly opened group starts at its first control, independent of cursor distance.
    FocusRing initial;initial.setPanelNavigation(true);initial.setGroup(4);
    initial.addStop(40,{0,0,20,20});initial.addStop(41,{230,130,20,20});initial.focusGroup(4);
    initial.update(pad(),.016f);assert(initial.focused()==40);
    // Focus cannot activate a control in an inactive/covered panel.
    FocusRing guarded;guarded.setPanelNavigation(true);panels(guarded);guarded.focusGroup(1);guarded.setFocus(20);
    guarded.update(pad(PSP_CTRL_CROSS,PSP_CTRL_CROSS),.016f);panels(guarded);
    guarded.update(pad(0,0,PSP_CTRL_CROSS),.016f);assert(guarded.activated()!=20);
    // Reserve the management controls even when a large panel fills the stop list.
    FocusRing crowded;crowded.setPanelNavigation(true);crowded.setGroup(5);
    for(unsigned i=0;i<FocusRing::kMaxStops;++i)crowded.addStop(1000+i,{0,0,10,10});
    crowded.addStop(0x00FE0005,{450,0,20,20});crowded.addStop(0x00FD0005,{0,0,20,20});
    crowded.focusGroup(5);crowded.setFocus(0x00FE0005);crowded.update(pad(),.016f);
    assert(crowded.focused()==0x00FE0005);
    std::puts("PSP UI navigation passed: repeat, remembered panels, deterministic focus, inactive activation rejection, reserved close/move controls.");
}
