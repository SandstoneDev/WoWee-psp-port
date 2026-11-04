"""Exercise production tutorial opening/dismissal with host pad/session stubs."""
from pathlib import Path
import subprocess
import tempfile
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'src/ui/psp/ui_world_tutorial.cpp').read_text()
def function(signature):
    begin=source.index(signature);end=source.index('{',begin)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[begin:end]+'\n'
methods=''.join(function(s) for s in ['void WorldScreen::tutorialNew(',
    'void WorldScreen::tutorialHidden(', 'bool WorldScreen::tutorialPad('])
cpp=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
constexpr uint32_t PSP_CTRL_CIRCLE=1,PSP_CTRL_CROSS=2;
namespace pp {struct PadState {uint32_t pressed=0;};}
namespace game {enum class UF {UNIT_FIELD_LEVEL};int fieldIndex(UF){return 0;}}
struct WorldScreen {
    static constexpr uint8_t kWinTutorial=0xA0;
    struct TutorialUi {
        struct {bool find(uint32_t id){return id!=999;}} db;
        bool autoOpen=true,frameShown=false,alertShown=false;
        uint32_t lastId=0;std::vector<uint32_t> queue;
    };
    std::unique_ptr<TutorialUi> tut_=std::make_unique<TutorialUi>();
    struct {
        bool combat=false,logout=false;uint32_t flagged=0;
        bool isTutorialFlagged(uint32_t id){return id==flagged;}
        bool inCombat(){return combat;}bool logoutPending(){return logout;}
        uint32_t selfField(int){return 10;}
    } session_;
    struct {bool open=false;bool isOpen(){return open;}} settings_;
    bool uiMode_=false;std::vector<uint8_t> windows_;unsigned opened=0,sounds=0;
    void tutorialAlertClick(){++opened;tut_->queue.erase(tut_->queue.begin());tut_->frameShown=true;windows_.push_back(kWinTutorial);}
    void closeWindow(uint8_t id){windows_.erase(std::remove(windows_.begin(),windows_.end(),id),windows_.end());}
    void playSoundByName(const char*){++sounds;}
    void tutorialNew(uint32_t);void tutorialHidden(bool);bool tutorialPad(const pp::PadState&);
};
'''
main=r'''
int main() {
    WorldScreen game;game.tutorialNew(36);assert(game.opened==1&&game.tut_->frameShown&&!game.uiMode_);
    game.tut_->queue.push_back(37);
    assert(!game.tutorialPad({PSP_CTRL_CROSS}));
    assert(game.tutorialPad({PSP_CTRL_CIRCLE})); // no SELECT/cursor required
    assert(game.windows_.empty()&&!game.tut_->frameShown&&game.tut_->queue.size()==1);
    assert(game.opened==1&&game.sounds==1&&game.tut_->alertShown); // no instant replacement popup
    assert(!game.tutorialPad({PSP_CTRL_CIRCLE}));
    for(int mode=0;mode<5;++mode){
        WorldScreen w;
        if(mode==0)w.uiMode_=true;
        if(mode==1)w.windows_={3}; // menu/bag/another panel
        if(mode==2)w.settings_.open=true;
        if(mode==3)w.session_.logout=true;
        if(mode==4)w.session_.combat=true;
        w.tutorialNew(36);assert(!w.opened&&w.tut_->alertShown&&w.tut_->queue.size()==1);
    }
    WorldScreen covered;covered.tutorialNew(36);covered.windows_.push_back(3);
    assert(!covered.tutorialPad({PSP_CTRL_CIRCLE})&&covered.tut_->frameShown); // do not close through a foreground panel
    covered.windows_.pop_back();assert(covered.tutorialPad({PSP_CTRL_CIRCLE}));
    assert(!covered.tut_->alertShown); // empty queue at level >5
    WorldScreen explicitOpen;explicitOpen.tut_->autoOpen=false;explicitOpen.tutorialNew(36);
    assert(!explicitOpen.opened&&explicitOpen.tut_->alertShown);
    explicitOpen.tutorialNew(36);assert(explicitOpen.tut_->queue.size()==1);
    std::puts("Tutorial controls passed: direct Circle dismissal, deferred queue, foreground ownership, no menu/settings/combat interruption.");
}
'''
with tempfile.TemporaryDirectory(prefix='wowee-tutorial-') as temp:
    folder=Path(temp);src=folder/'test.cpp';exe=folder/'test.exe';src.write_text(cpp+methods+main)
    compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc',str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'test.obj')]
    else:args=[compiler,'-std=c++20',str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
