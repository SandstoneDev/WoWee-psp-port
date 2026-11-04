"""Exercise production settings dropdown opening, selection and cancellation."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1];source=(root/'src/ui/psp/ui_settings_menu.cpp').read_text(encoding='utf-8')
def function(sig):
 a=source.index(sig);b=source.index('{',a)+1;depth=1
 while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]+'\n'
methods=''.join(function(sig) for sig in ['bool SettingsMenu::openDropdown(', 'bool SettingsMenu::updateDropdown('])
cpp=r'''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
constexpr uint32_t PSP_CTRL_UP=1,PSP_CTRL_DOWN=2,PSP_CTRL_LEFT=4,PSP_CTRL_RIGHT=8,PSP_CTRL_CIRCLE=16,PSP_CTRL_TRIANGLE=32;
constexpr uint32_t kIdRow0=100,kIdChoice0=300;
namespace pp{struct PadState{uint32_t held=0,pressed=0;};const char* ps(int){return "Off|Self|All";}}
enum class SettingKind{Enum,Int};struct Values{int mode=1,number=20,graphicspreset=5;};using GraphicsSettings=Values;
struct Row{struct Desc{SettingKind kind;}desc;int Values::*field;int choices;};
const Row kRows[]={{{SettingKind::Enum},&Values::mode,0},{{SettingKind::Int},&Values::number,0}};
constexpr size_t kRowCount=2;int choiceCount(const char*){return 3;}
struct SettingsMenu{Values values_;int dropdownRow_=-1,dropdownSelection_=0;
 struct Focus{uint32_t id=0,act=0;void cancelPresses(){act=0;}void setFocus(uint32_t v){id=v;}uint32_t activated(){return act;}void update(const pp::PadState&,float){}}focus_;
 struct Repeat{uint32_t step(uint32_t,uint32_t p,float,float=.32f,float=.09f,float=.055f){return p;}}settingRepeat_;
 bool rowEnabled(size_t)const{return true;}
 bool commitValue(size_t row,int v){bool changed=values_.*(kRows[row].field)!=v;values_.*(kRows[row].field)=v;return changed;}
 bool openDropdown(size_t);bool updateDropdown(const pp::PadState&,float);
};
'''
main=r'''int main(){SettingsMenu s;
 assert(!s.openDropdown(1) && !s.openDropdown(20));
 assert(s.openDropdown(0) && s.dropdownSelection_==1 && s.focus_.id==kIdChoice0+1);
 s.updateDropdown({PSP_CTRL_DOWN,PSP_CTRL_DOWN},.016f);assert(s.dropdownSelection_==2 && s.values_.mode==1);
 s.updateDropdown({PSP_CTRL_CIRCLE,PSP_CTRL_CIRCLE},.016f);assert(s.dropdownRow_==-1 && s.values_.mode==1 && s.focus_.id==kIdRow0);
 s.openDropdown(0);s.focus_.act=kIdChoice0+2;assert(s.updateDropdown({},.016f));assert(s.values_.mode==2 && s.dropdownRow_==-1 && s.focus_.id==kIdRow0);
 s.openDropdown(0);s.focus_.act=kIdChoice0+2;assert(!s.updateDropdown({},.016f));assert(s.dropdownRow_==-1);
 std::puts("Settings dropdown: open, navigate without applying, cancel, select and restore parent focus passed.");}
'''
with tempfile.TemporaryDirectory(prefix='wowee-settings-dropdown-') as temp:
 src=Path(temp)/'test.cpp';exe=Path(temp)/'test.exe';src.write_text(cpp+methods+main,encoding='utf-8')
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'c++','-std=c++20','-O2',src,'-o',exe],check=True);subprocess.run([exe],check=True)
