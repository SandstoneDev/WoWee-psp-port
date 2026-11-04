"""Exercise production window management and navigation with host UI substitutes."""
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/ui/psp/ui_world_windows.cpp').read_text()
def function(signature):
    begin=source.index(signature);end=source.index('{',begin)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[begin:end]+'\n'
methods=''.join(function(s) for s in ['bool WorldScreen::windowOpen(',
    'void WorldScreen::openWindow(', 'void WorldScreen::closeWindow(',
    'void WorldScreen::setUiMode(', 'void WorldScreen::toggleGameMenu(',
    'uint8_t WorldScreen::activeWindow(', 'void WorldScreen::raiseWindow(',
    'uint32_t WorldScreen::windowSwitchCount(', 'bool WorldScreen::windowSwitchPad(', 'void WorldScreen::cycleWindow(', 'bool WorldScreen::windowBack(',
    'bool WorldScreen::windowManagementAction(', 'void WorldScreen::placeBeside(', 'void WorldScreen::dragWindows('])
map_source=(root/'src/ui/psp/ui_world_map.cpp').read_text()
map_begin=map_source.index('void WorldScreen::openWorldMap()')
map_end=map_source.index('    if (w.open) return;',map_begin)+len('    if (w.open) return;')
# Exercise the production transition; the remaining tail loads map assets.
methods+=map_source[map_begin:map_end]+'\n    w.open=true;\n}\n'
cpp=r"""
#include "ui/psp/ui_focus.hpp"
#include "platform/psp/psp_display.hpp"
#include <pspctrl.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <map>
#include <string>
#include <vector>
using wowee::ui::psp::FocusRing;using wowee::ui::psp::Rect;
namespace pp=wowee::platform::psp;
namespace wowee::platform::psp {
int configGetInt(const char*,int fallback){return fallback;}
std::string saved;void configSet(const char*,const char* v){saved=v;}void configSave(){}
}
#define PSP_LOGI(...) ((void)0)
void windowSound(uint8_t,bool){}void uiArtNoteWindowOpen(){}
struct WorldChat {static constexpr uint32_t kBarButtonId=100;};
constexpr uint32_t kIdMenuGraphics=0x5711,kIdMapArea=0x6810;
struct MapUi {int mini=-1;struct {bool open=false;} world;};
struct WorldScreen {
    static constexpr uint8_t kWinMenu=1,kWinDebug=2,kWinWorldMap=7,kWinPullouts=250,kWinReadyCheck=251,kWinBank=0xB0;
    static constexpr uint8_t kWinCharacter=0x50,kWinBags=0x51,kWinQuestLog=0x5D,kWinDeleteItem=0x61,kWinSpellbook=0x43;
    static constexpr uint32_t kIdBagSlot0=0x6C00;
    struct {bool pending=false;bool logoutPending(){return pending;}void cancelLogout(){pending=false;}} session_;
    struct {bool shown=false;bool isOpen(){return shown;}void close(){shown=false;}void open(int){shown=true;}} settings_;
    struct {bool on=false;bool active(){return on;}} barAssign_;
    struct InvHeld {bool active=false;};InvHeld invHeld_;
    uint32_t bookPick_=0;bool bookPickItem_=false,uiMode_=false,menuReturnSettings_=false;
    bool bagOpen_[5]={};int bagCloses=0;bool closeFocusedBag(){++bagCloses;return true;}
    int invBag_=0;uint8_t menuReturnWindow_=0;
    FocusRing winFocus_;
    MapUi mapStorage_;MapUi* map_=nullptr;
    void initMap(){map_=&mapStorage_;}void openWorldMap();
    struct WinPos {float dx=0,dy=0;};
    std::map<uint8_t,WinPos> winPos_,autoPos_;std::map<uint8_t,Rect> winRect_;
    std::vector<uint8_t> windows_,winDrawn_,placeNext_;
    bool dragShoulder_=false;uint8_t dragWin_=0;WinPos dragOrigin_;uint32_t dragReturnFocus_=0;float lastDt_=1.f/30;
    bool pspLayout_=false;bool pspLayoutActive()const{return pspLayout_;}
    bool fxOpenWindow(uint8_t){return false;}
    bool isPopupWindow(uint8_t w)const{return w==kWinMenu||w==kWinDebug||w==8||w==9||w==kWinDeleteItem;}
    bool deathPopupKeepsOnEscape(uint8_t w){return w==9;}
    bool mapMini(){return false;}int liveSettings(){return 0;}
    void cancelSpellAssignment(){barAssign_.on=false;}
    bool bankIdToWire(uint32_t id,uint8_t&,uint8_t&)const{return id>=0x7400&&id<0x741C;}
    bool inventoryIdToWire(uint32_t id,uint8_t&,uint8_t&){return id==kIdBagSlot0||id==0x6010;}
    bool windowOpen(uint8_t)const;void openWindow(uint8_t);void closeWindow(uint8_t);
    void setUiMode(bool);void toggleGameMenu();uint8_t activeWindow()const;
    void raiseWindow(uint8_t);uint32_t windowSwitchCount()const;bool windowSwitchPad(const pp::PadState&);void cycleWindow(int);bool windowBack(const pp::PadState&);
    void placeBeside(uint8_t,const Rect&);
    bool windowManagementAction(uint32_t);void dragWindows(pp::PadState&,const pp::PadState&);
};
pp::PadState pad(uint32_t pressed=0,uint32_t held=0,float ax=0,float ay=0){pp::PadState p;p.pressed=pressed;p.held=held;p.ax=ax;p.ay=ay;return p;}
"""
main=r"""
int main(){
    WorldScreen w;w.setUiMode(true);w.openWindow(3);w.openWindow(4);w.openWindow(5);
    w.cycleWindow(1);assert(w.activeWindow()==3);w.cycleWindow(-1);assert(w.activeWindow()==5);
    // Backward cycling traverses every panel; front-to-back reordering cannot
    // trap it alternating between two. Forward is the inverse.
    w.cycleWindow(-1);assert(w.activeWindow()==4);
    w.cycleWindow(-1);assert(w.activeWindow()==3);
    w.cycleWindow(-1);assert(w.activeWindow()==5);
    w.invHeld_.active=true;w.bookPick_=99;
    assert(w.windowSwitchPad(pad(PSP_CTRL_RTRIGGER,PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER)));
    assert(w.activeWindow()==3 && w.invHeld_.active && w.bookPick_==99);
    assert(w.windowManagementAction(0x00FB0003));assert(w.activeWindow()==5);
    w.invHeld_.active=false;w.bookPick_=0;
    w.openWindow(WorldScreen::kWinPullouts);w.openWindow(WorldScreen::kWinReadyCheck);
    assert(w.windowSwitchCount()==3);w.raiseWindow(5);w.cycleWindow(1);assert(w.activeWindow()==3);
    w.closeWindow(WorldScreen::kWinPullouts);w.closeWindow(WorldScreen::kWinReadyCheck);w.raiseWindow(5);
    w.settings_.shown=true;assert(!w.windowSwitchPad(pad(PSP_CTRL_RTRIGGER,PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER)));
    assert(w.activeWindow()==5);w.settings_.shown=false;
    // Chrome buttons must not overwrite the remembered inventory/character slot.
    w.winFocus_.focusGroup(5);w.winFocus_.setGroup(5);
    w.winFocus_.addStop(500,{10,10,20,20});w.winFocus_.setFocus(500);w.winFocus_.update(pad(),.03f);
    w.winFocus_.addStop(500,{10,10,20,20});w.winFocus_.addStop(0x00FA0005,{40,10,16,16});w.winFocus_.update(pad(),.03f);
    w.winFocus_.setFocus(0x00FA0005);w.winFocus_.focusGroup(3);
    assert(w.winFocus_.rememberedFocus(5)==500);
    w.session_.pending=true;w.barAssign_.on=true;w.toggleGameMenu();
    assert(w.activeWindow()==1&&w.uiMode_&&!w.session_.pending&&!w.barAssign_.on);
    w.toggleGameMenu();assert(w.activeWindow()==5&&w.uiMode_);
    w.settings_.shown=true;w.toggleGameMenu();assert(w.activeWindow()==1&&!w.settings_.shown);
    w.toggleGameMenu();assert(w.settings_.shown&&w.activeWindow()==5);
    // START while settings covers an already-open menu reveals the menu first.
    w.settings_.shown=false;w.toggleGameMenu();w.settings_.shown=true;w.toggleGameMenu();assert(w.activeWindow()==1&&!w.settings_.shown);
    w.toggleGameMenu();assert(w.settings_.shown);w.settings_.shown=false;
    // Holding inventory modifiers never moves windows, even over their title.
    w.winRect_[5]={350,100,110,160};auto r=pad(PSP_CTRL_RTRIGGER,PSP_CTRL_RTRIGGER);auto p=r;
    w.dragWindows(p,r);assert(!w.dragWin_&&w.winPos_.empty());
    assert(w.windowManagementAction(0x00FD0005)&&w.dragWin_==5&&w.winPos_[5].dx==350);
    r=pad(0,0,-1);w.dragWindows(p,r);assert(w.winPos_[5].dx==346);
    r=pad(PSP_CTRL_CROSS);w.dragWindows(p,r);assert(!w.dragWin_&&pp::saved=="346,100");
    w.windowManagementAction(0x00FD0005);r=pad(0,0,-1);w.dragWindows(p,r);
    r=pad(PSP_CTRL_CIRCLE);w.dragWindows(p,r);assert(!w.dragWin_&&w.winPos_[5].dx==350); // original rendered position
    w.placeBeside(WorldScreen::kWinBags,{370,100,100,140});
    assert(!w.autoPos_.count(WorldScreen::kWinBags)); // bag anchors stay bottom right
    // L+stick drags the active window; releasing L commits without a click.
    r=pad(0,PSP_CTRL_LTRIGGER,.1f);w.dragWindows(p,r);assert(!w.dragWin_); // dead zone
    r=pad(0,PSP_CTRL_LTRIGGER,-1);w.dragWindows(p,r);
    assert(w.dragWin_==5&&w.dragShoulder_&&w.winPos_[5].dx==346);
    r=pad();w.dragWindows(p,r);assert(!w.dragWin_&&pp::saved=="346,100");
    r=pad(0,PSP_CTRL_LTRIGGER,-1);w.dragWindows(p,r);
    r=pad(PSP_CTRL_CIRCLE,PSP_CTRL_LTRIGGER);w.dragWindows(p,r);
    assert(!w.dragWin_&&w.winPos_[5].dx==350);
    // Modal windows cannot be moved or switched past.
    w.openWindow(8);w.cycleWindow(1);assert(w.activeWindow()==8);
    w.windowManagementAction(0x00FD0008);assert(!w.dragWin_);
    r=pad(0,PSP_CTRL_LTRIGGER,-1);w.dragWindows(p,r);assert(!w.dragWin_);
    assert(w.windowBack(pad(PSP_CTRL_CIRCLE))&&w.activeWindow()==5);
    w.invHeld_.active=true;assert(w.windowBack(pad(PSP_CTRL_CIRCLE))&&!w.invHeld_.active&&w.activeWindow()==5);
    w.bookPick_=123;assert(w.windowBack(pad(PSP_CTRL_CIRCLE))&&!w.bookPick_&&w.activeWindow()==5);
    w.openWindow(WorldScreen::kWinBags);w.winFocus_.setFocus(WorldScreen::kIdBagSlot0);
    assert(w.windowBack(pad(PSP_CTRL_CIRCLE))&&w.bagCloses==1&&w.windowOpen(WorldScreen::kWinBags));
    assert(!w.windowBack(pad(PSP_CTRL_CIRCLE,PSP_CTRL_LTRIGGER))); // L+Circle retains item use
    w.windowManagementAction(0x00FE0000|WorldScreen::kWinBags);assert(w.activeWindow()==5);
    w.openWindow(WorldScreen::kWinBank);w.winFocus_.setFocus(0x7400);
    assert(!w.windowBack(pad(PSP_CTRL_CIRCLE))&&w.windowOpen(WorldScreen::kWinBank));
    w.closeWindow(WorldScreen::kWinBank);
    w.invHeld_.active=true;w.openWindow(WorldScreen::kWinDeleteItem);
    assert(w.windowBack(pad(PSP_CTRL_CIRCLE))&&!w.invHeld_.active&&!w.windowOpen(WorldScreen::kWinDeleteItem));
    w.openWindow(9);assert(w.windowBack(pad(PSP_CTRL_CIRCLE))&&w.windowOpen(9)); // mandatory death popup protected
    w.closeWindow(9);w.closeWindow(3);w.closeWindow(4);w.closeWindow(5);assert(w.windows_.empty()&&!w.uiMode_);
    // Menu Map closes the last window before opening the map, even on first use.
    w.setUiMode(true);w.openWindow(WorldScreen::kWinMenu);
    w.closeWindow(WorldScreen::kWinMenu);assert(!w.uiMode_);
    w.openWorldMap();assert(w.uiMode_&&w.activeWindow()==WorldScreen::kWinWorldMap);
    assert(w.winFocus_.focused()==kIdMapArea);
    auto declareMap=[&](){w.winFocus_.setGroup(WorldScreen::kWinWorldMap);
        w.winFocus_.addStop(kIdMapArea,{10,10,30,30});
        w.winFocus_.addStop(kIdMapArea+1,{60,10,30,30});};
    declareMap();w.winFocus_.update(pad(),1.f/30);declareMap();
    w.winFocus_.update(pad(PSP_CTRL_RIGHT,PSP_CTRL_RIGHT),1.f/30);
    assert(w.winFocus_.focused()==kIdMapArea+1);declareMap();
    w.winFocus_.update(pad(PSP_CTRL_CROSS,PSP_CTRL_CROSS),1.f/30);declareMap();
    auto release=pad();release.released=PSP_CTRL_CROSS;
    w.winFocus_.update(release,1.f/30);assert(w.winFocus_.activated()==kIdMapArea+1);declareMap();
    w.closeWindow(WorldScreen::kWinWorldMap);w.openWorldMap();
    assert(w.uiMode_&&w.winFocus_.focused()==kIdMapArea+1);
    w.closeWindow(WorldScreen::kWinWorldMap);w.pspLayout_=true;w.setUiMode(true);
    assert(w.winFocus_.focused()==0x5C10); // SELECT starts on the first spell slot
    w.setUiMode(false);w.pspLayout_=false;w.setUiMode(true);assert(w.winFocus_.focused()==WorldChat::kBarButtonId);
    std::puts("Window management passed: stack cycling, START restoration, modal capture, explicit move/commit/cancel, held items, bag bindings and last-window exit.");
}
"""
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-menu-') as temp:
    folder=Path(temp);src=folder/'menu.cpp';exe=folder/'menu.exe';src.write_text(cpp+methods+main)
    includes=[root/'tests/psp_ui_stubs',root/'include',root]
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/FI'+str(root/'tests/psp_npc_async_stubs/host_compat.hpp')]+['/I'+str(p) for p in includes]+[str(src),str(root/'src/ui/psp/ui_focus.cpp'),'/Fe:'+str(exe),'/Fo:'+str(folder)+'\\']
    else:args=[compiler,'-std=c++20','-O2']+['-I'+str(p) for p in includes]+[str(src),str(root/'src/ui/psp/ui_focus.cpp'),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
