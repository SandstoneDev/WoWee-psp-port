"""Record production affordance draws to check hints follow controls/panel ownership."""
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
def function(file,signature):
    s=(root/file).read_text();a=s.index(signature);b=s.index('{',a)+1;depth=1
    while depth:
        depth+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[a:b]+'\n'
methods=function('src/ui/psp/ui_world_windows.cpp','void WorldScreen::drawWindowAffordances(')+function('src/ui/psp/ui_world_bags.cpp','void WorldScreen::drawBagControlHints(')
cpp=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
namespace pp {constexpr float kScreenW=480,kScreenH=272;}
struct Rect {float x,y,w,h;};
struct UiRenderer {std::vector<Rect> fills;void fill(Rect r,uint32_t){fills.push_back(r);}template<class... T>void textShadow(T...){}};
struct Font {int lineHeight()const{return 10;}};
constexpr int kFontSmall=0,kPadCross=1,kPadCircle=2,kPadSquare=3,kPadStick=4,kPadLeft=5,kPadRight=6;constexpr uint32_t kColGold=1;
#define OWN_STR(x) #x
#define CLIENT_STR(x) #x
struct WorldScreen {
    static constexpr uint8_t kWinBags=0x51,kWinBank=0xB0,kWinWorldMap=7;
    uint32_t switchCount=1;uint32_t windowSwitchCount()const{return switchCount;}
    bool uiMode_=true,psp=false,dragShoulder_=false;uint8_t top=3,dragWin_=0;
    bool pspLayoutActive()const{return psp;}
    struct {bool value=false;bool isOpen(){return value;}} settings_;
    struct {bool value=false;bool logoutPending(){return value;}} session_;
    struct {bool value=false;bool active(){return value;}} barAssign_;
    struct {bool active=false;} invHeld_;
    struct {
        Rect rect{100,100,40,20};uint32_t id=123;
        void setGroup(uint8_t){}bool focusedRect(Rect& r){r=rect;return true;}uint32_t focused(){return id;}
    } winFocus_;
    std::map<uint8_t,Rect> winRect_;
    struct Kit {
        Font f;struct Glyph {int id;Rect r;};struct Button {uint32_t id;Rect r;};
        std::vector<Glyph> glyphs;std::vector<Button> buttons;std::vector<std::string> hints;
        Font& font(int){return f;}
        void glyph(UiRenderer&,int id,Rect r){glyphs.push_back({id,r});}
        void button(UiRenderer&,decltype(WorldScreen::winFocus_)&,uint32_t id,Rect r,const char*){buttons.push_back({id,r});}
        void controlHint(UiRenderer&,const Font&,float,float,const char* s,uint32_t){hints.push_back(s);}
    } kit_;
    uint8_t activeWindow()const{return top;}
    bool deathPopupKeepsOnEscape(uint8_t w){return w==9;}bool isPopupWindow(uint8_t w){return w==8||w==9;}
    bool mapMini(){return false;}
    void drawWindowAffordances(UiRenderer&);void drawBagControlHints(UiRenderer&);
};
'''
main=r'''
int main(){
    WorldScreen w;UiRenderer ui;w.winRect_[3]={30,30,200,200};w.drawWindowAffordances(ui);
    assert(w.kit_.buttons.size()==2&&w.kit_.glyphs.back().id==kPadCross);
    auto cross=w.kit_.glyphs.back().r;assert(cross.x==135&&cross.y==115); // attached to focused control
    assert(w.kit_.hints.empty()); // no detached instruction panel
    auto oldButtons=w.kit_.buttons.size();w.switchCount=3;
    w.winFocus_.id=0x00FA0003u;w.drawWindowAffordances(ui);
    assert(w.kit_.buttons.size()==oldButtons+4 && w.kit_.hints.back()=="WIN_SWITCH_HINT");
    w.switchCount=1;
    w.winFocus_.id=0x5C80;w.drawWindowAffordances(ui);assert(w.kit_.glyphs.back().id==kPadSquare);
    w.top=WorldScreen::kWinBags;w.winRect_[w.top]={300,30,174,240};w.drawBagControlHints(ui);
    assert(w.kit_.hints.back()=="INV_PAD_EXTRA_HINT");
    w.invHeld_.active=true;w.drawBagControlHints(ui);assert(w.kit_.hints[w.kit_.hints.size()-2]=="INV_HELD_HINT");
    auto count=w.kit_.hints.size();w.top=8;w.winRect_[8]={100,80,200,100};w.drawBagControlHints(ui);
    assert(w.kit_.hints.size()==count); // hints belong to the active panel only
    w.psp=true;w.top=WorldScreen::kWinBags;w.drawBagControlHints(ui);assert(w.kit_.hints.size()==count);
    auto buttons=w.kit_.buttons.size();w.drawWindowAffordances(ui);assert(w.kit_.buttons.size()==buttons);
    w.top=WorldScreen::kWinBank;w.drawWindowAffordances(ui);assert(w.kit_.buttons.size()==buttons);
    w.psp=false;
    w.top=9;w.winRect_[9]={100,80,200,100};count=w.kit_.buttons.size();w.drawWindowAffordances(ui);
    assert(w.kit_.buttons.size()==count); // mandatory popup has no close/move action
    w.settings_.value=true;count=w.kit_.glyphs.size();w.drawWindowAffordances(ui);assert(w.kit_.glyphs.size()==count);
    std::puts("Window hints passed: attached action glyphs, bag-only strip, held-item context, modal ownership and protected dialogs.");
}
'''
with tempfile.TemporaryDirectory(prefix='wowee-window-hints-') as temp:
    folder=Path(temp);src=folder/'test.cpp';exe=folder/'test.exe';src.write_text(cpp+methods+main)
    compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
    if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc',str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'test.obj')]
    else:args=[compiler,'-std=c++20',str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
