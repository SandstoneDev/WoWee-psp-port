"""Production preset application, edit ownership, locking and legacy-config migration."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/psp/ui_settings_menu.cpp').read_text(encoding='utf-8')
h=(root/'include/ui/psp/ui_settings_menu.hpp').read_text(encoding='utf-8')
settings=h[h.index('struct GraphicsSettings {'):h.index('\nclass SettingsMenu')]
def fn(sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
cpp=r'''#include "ui/psp/ui_graphics_presets.hpp"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
using namespace wowee::ui::psp;
#define PSP_LOGI(...) ((void)0)
''' + settings + r'''
namespace pp{std::map<std::string,int> config;int configGetInt(const char* key,int fallback){auto i=config.find(key);return i==config.end()?fallback:i->second;}}
struct Row {struct Desc{const char* key;int defaultValue;}desc;int GraphicsSettings::*field;int lo,hi;};
const Row kRows[]={{{"graphicspreset",5},&GraphicsSettings::graphicspreset,0,5},{{"framecap",30},&GraphicsSettings::framecap,30,60},
 {{"m2view",160},&GraphicsSettings::m2view,40,240},{{"spellfx",1},&GraphicsSettings::spellfx,0,2},
 {{"weather",1},&GraphicsSettings::weather,0,1},{{"musicvol",30},&GraphicsSettings::musicvol,0,100},
 {{"brightness",100},&GraphicsSettings::brightness,50,150},{{"perfhud",0},&GraphicsSettings::perfhud,0,1}};
constexpr size_t kRowCount=sizeof(kRows)/sizeof(kRows[0]);
int clampToRow(const Row& row,int v){return std::clamp(v,row.lo,row.hi);}
const Row* findRow(const char* key){for(const auto& row:kRows)if(!std::strcmp(key,row.desc.key))return &row;return nullptr;}
template<class Getter>bool settingEnabled(const Row::Desc&,Getter){return true;}
std::string settingNumberText(int v){return std::to_string(v);}
struct SettingsMenu{GraphicsSettings values_;bool rowEnabled(size_t)const;bool commitValue(size_t,int);bool setValue(const char*,int);void load();int value(const char* key)const {const auto* row=findRow(key);return row?values_.*(row->field):0;}};
''' + ''.join(fn(sig) for sig in ['bool SettingsMenu::rowEnabled(', 'bool SettingsMenu::commitValue(', 'bool SettingsMenu::setValue(', 'void SettingsMenu::load(']) + r'''
int main(){
 GraphicsSettings g;g.framecap=60;g.musicvol=70;g.camfollow=0;
 int lastObjects=0,lastNpcs=0;
 for(int preset=0;preset<4;++preset){
  applyGraphicsPreset(g,preset);assert(g.graphicspreset==preset && !g.adaptive30);
  assert(g.framecap==60 && g.musicvol==70 && !g.camfollow);
  assert(g.m2view>lastObjects && g.npcmax>lastNpcs);lastObjects=g.m2view;lastNpcs=g.npcmax;
  assert(g.m2view<=240 && g.npcmax<=64 && g.playermax<=50);
 }
 SettingsMenu menu;applyGraphicsPreset(menu.values_,kGraphicsHigh);
 assert(menu.setValue("framecap",60));assert(menu.values_.graphicspreset==kGraphicsHigh);
 assert(menu.setValue("musicvol",50));assert(menu.values_.graphicspreset==kGraphicsHigh);
 assert(!menu.setValue("m2view",160));assert(menu.values_.graphicspreset==kGraphicsHigh);
 assert(menu.setValue("m2view",180));assert(menu.values_.graphicspreset==kGraphicsCustom && !menu.values_.adaptive30);
 assert(menu.setValue("graphicspreset",kGraphicsAdaptive));assert(menu.values_.adaptive30);
 assert(menu.rowEnabled(0)&&menu.rowEnabled(1));
 for(size_t i: {2,3,4,6})assert(!menu.rowEnabled(i));
 assert(!menu.setValue("m2view",240)&&!menu.setValue("spellfx",0)&&!menu.setValue("weather",0));
 assert(menu.values_.spellfx==2 && menu.values_.weather==1);
 assert(menu.rowEnabled(7) && menu.setValue("perfhud",1) && menu.values_.graphicspreset==kGraphicsAdaptive);
 applyGraphicsPreset(menu.values_,kGraphicsHigh);assert(menu.values_.perfhud==1);
 applyGraphicsPreset(menu.values_,kGraphicsAdaptive);
 assert(menu.setValue("framecap",30));assert(menu.values_.graphicspreset==kGraphicsAdaptive);
 assert(menu.setValue("graphicspreset",kGraphicsCustom));assert(menu.values_.m2view==160 && !menu.values_.adaptive30);
 assert(menu.setValue("spellfx",0));assert(menu.values_.spellfx==0);
 pp::config.clear();menu.load();assert(menu.values_.graphicspreset==kGraphicsAdaptive && menu.values_.adaptive30);
 pp::config={{"adaptive30",0},{"m2view",100}};menu.load();assert(menu.values_.graphicspreset==kGraphicsCustom && menu.values_.m2view==100 && !menu.values_.adaptive30);
 pp::config={{"graphicspreset",kGraphicsLow},{"framecap",60}};menu.load();assert(menu.values_.graphicspreset==kGraphicsLow && menu.values_.m2view==80 && menu.values_.framecap==60);
 pp::config={{"graphicspreset",kGraphicsCustom},{"m2view",200},{"adaptive30",1}};menu.load();assert(menu.values_.m2view==200 && !menu.values_.adaptive30);
 for(const char* key:{"m2view","playerview","playermax","npcview","npcmax","terrainsharp","sharpdist","classicfog","terraindetail","spellfx","spellfxreal","levelupfx","brightness","water","particles","weather"})assert(!graphicsPresetAllows(kGraphicsAdaptive,key));
 std::puts("Graphics presets passed: ordered tiers, Custom edits, FPS independence, Adaptive locking, defaults and legacy/custom config reloads.");
}
'''
with tempfile.TemporaryDirectory(prefix='wowee-presets-') as temp:
 p=Path(temp);src=p/'presets.cpp';exe=p/'presets.exe';src.write_text(cpp,encoding='utf-8')
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'c++','-std=c++20','-O2','-I'+str(root/'include'),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
