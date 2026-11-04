"""Exercise production category membership and settings input routing."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ui/psp/ui_settings_menu.cpp').read_text(encoding='utf-8')


def method(signature):
    start = source.index(signature)
    body = source.index('{', start)
    end, depth = body + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


keys = re.findall(r'\{\{"([a-z0-9]+)"', source)
page_keys = source[source.index('const char* kPageKeys'):source.index('const std::vector<size_t>& pageRows')]
cpp = r'''
#include <array>
#include <vector>
#include <string>
#include <cstring>
#include <string>
#include <algorithm>
#include <cassert>
#include <cstdint>
constexpr uint32_t PSP_CTRL_LEFT=1,PSP_CTRL_RIGHT=2,PSP_CTRL_UP=4,PSP_CTRL_DOWN=8,
 PSP_CTRL_RTRIGGER=16,PSP_CTRL_LTRIGGER=32,PSP_CTRL_SQUARE=64,PSP_CTRL_TRIANGLE=128,PSP_CTRL_CIRCLE=256;
namespace pp {struct PadState { uint32_t held=0,pressed=0; };}
constexpr int kPages=7, kLanguagePage=6;
constexpr uint32_t kIdClose=2,kIdReset=1,kIdRow0=100,kIdCategory0=20;
struct Row {struct Desc {const char* key;} desc;};
'''
cpp += 'const Row kRows[]={' + ','.join('{{"' + key + '"}}' for key in keys) + '};\n'
cpp += 'constexpr size_t kRowCount=sizeof(kRows)/sizeof(kRows[0]);\nconstexpr uint32_t kIdLangRow=kIdRow0+kRowCount;\n'
cpp += page_keys + method('const std::vector<size_t>& pageRows(')
cpp += r'''
struct Focus {
 uint32_t f=20, act=0, nextAct=0, navNext=0;
 pp::PadState last;
 uint32_t focused() const { return f; }
 uint32_t activated() const { return act; }
 void setFocus(uint32_t id) { f=id; }
 void update(const pp::PadState& p,float) { last=p;act=nextAct;nextAct=0;if(navNext){f=navNext;navNext=0;} }
};
struct Picker {bool visible=false;bool shown()const{return visible;}void show(Focus&){visible=true;}};
struct Repeat {uint32_t step(uint32_t,uint32_t pressed,float,float=.32f,float=.09f,float=.055f){return pressed;}};
struct SettingsMenu {
 bool open_=true,prompt_=false,language=true;
 float helpTime_=0; int page_=0, edits=0,dropdownRow_=-1;
 Focus focus_; Picker picker_; Repeat settingRepeat_;
 bool languageRow() const {return language;}
 bool updateDropdown(const pp::PadState&,float){return false;}
 bool openDropdown(size_t){return false;}
 bool updatePrompt(const pp::PadState&,float){return false;}
 bool updatePicker(const pp::PadState&,float){return false;}
 bool rowEnabled(size_t) const {return true;}
 bool step(size_t,int,bool){++edits;return true;}
 void stepLanguage(int){}
 void restoreDefaults(){++edits;}
 void close(){open_=false;}
 bool update(const pp::PadState&,float);
};
'''
cpp += method('bool SettingsMenu::update(')
cpp += r'''
int main(){
 std::vector<int> counts(kRowCount);
 for(int page=0;page<kPages;++page){
   const auto& rows=pageRows(page,true);
   assert(page!=0 || rows.size()==18);
   for(size_t row:rows)if(row<kRowCount)++counts[row];
 }
 for(int n:counts)assert(n==1);
 assert(kPages==7);
 for(size_t row:pageRows(0,true)) {
   const std::string key=kRows[row].desc.key;
   assert(key!="musicvol" && key!="autoloot" && key!="camfollow");
 } // no lost or duplicated settings
  auto contains=[](int page,const char* key){for(size_t row:pageRows(page,true))if(row<kRowCount && !std::strcmp(kRows[row].desc.key,key))return true;return false;};
 assert(contains(1,"musicvol") && contains(1,"weathervol"));
 assert(contains(2,"autoloot") && contains(2,"targetall"));
 assert(contains(3,"camfollow") && contains(3,"camsmartfollow"));
 assert(contains(4,"perfhud") && contains(4,"barassignment"));
 assert(contains(5,"remotepredict") && !contains(0,"perfhud"));
 assert(!std::strcmp(kRows[pageRows(0,true)[0]].desc.key,"framecap"));
 assert(pageRows(kLanguagePage,false).empty());
 assert(pageRows(kLanguagePage,true).size()==1 && pageRows(kLanguagePage,true)[0]==kRowCount);
 SettingsMenu s;
 auto press=[&](uint32_t p){return s.update(pp::PadState{p,p},.016f);};
 press(PSP_CTRL_RIGHT);
 assert(s.focus_.f==kIdRow0+pageRows(0,true)[0] && s.edits==0);
 assert(press(PSP_CTRL_RIGHT) && s.edits==1);
 assert(!(s.focus_.last.pressed&PSP_CTRL_RIGHT)); // editing must not move row focus
 // Scrolling row navigation explicitly reaches the entire merged Graphics list.
 for(size_t n=1;n<pageRows(0,true).size();++n){press(PSP_CTRL_DOWN);assert(s.focus_.f==kIdRow0+pageRows(0,true)[n]);}
 press(PSP_CTRL_DOWN);assert(s.focus_.f==kIdClose);
 s.focus_.setFocus(kIdRow0+pageRows(0,true)[0]);
 press(PSP_CTRL_UP);assert(s.focus_.f==kIdCategory0);
 s.focus_.setFocus(kIdRow0+pageRows(0,true)[0]);
 press(PSP_CTRL_SQUARE);
 assert(s.focus_.f==kIdCategory0 && s.edits==1);
 press(PSP_CTRL_RTRIGGER);
 assert(s.page_==1 && s.focus_.f==kIdCategory0+1);
 s.focus_.navNext=kIdCategory0+6;press(PSP_CTRL_DOWN);
 assert(s.page_==6); // moving in sidebar switches the right-hand panel
 s.focus_.nextAct=kIdCategory0+6;press(0);
 assert(s.focus_.f==kIdRow0+pageRows(6,true)[0] && s.edits==1);
 press(PSP_CTRL_LTRIGGER);
 assert(s.page_==5 && s.focus_.f==kIdRow0+pageRows(5,true)[0]);
 press(PSP_CTRL_CIRCLE);assert(!s.open_);
}
'''

if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / 'categories.cpp', Path(tmp) / 'categories.exe'
        src.write_text(cpp, encoding='utf-8')
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('Settings category coverage, language access and controller routing passed')
