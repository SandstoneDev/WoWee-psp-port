"""Exercise real OSK lifecycle plus lightweight world draw and temporary dialog pacing."""
from pathlib import Path
import re,subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/platform/psp/psp_osk.cpp').read_text();a=s.index('void oskAfterGeSync()');b=s.index('\nbool oskFinished',a);method=s[a:b]
s=(root/'src/ui/psp/ui_world_screen.cpp').read_text();a=s.index('void WorldScreen::draw(UiRenderer& ui, const FrameStats& stats) {');b=s.index('    if (firstDraws_',a);draw=s[a:b]+'    ++worldDraws;\n}\n'
s=(root/'src/platform/psp/psp_display.cpp').read_text();expr=re.search(r'const int interval=(.*?);',s).group(1)
cpp=r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <cstdio>
#define PSP_LOGI(...) ((void)0)
#define PSP_LOGW(...) ((void)0)
constexpr int PSP_UTILITY_OSK_RESULT_CANCELLED=1;
namespace pp {
 void logFlush(){}
 bool g_active=false,g_shutdownSent=false,g_finished=false,g_accepted=false;
 uint64_t g_startedUs=0;int g_lastStatus=-1;
 struct{struct{int result=0;}base;}g_params;
 struct{int result=0;}g_data;
 uint16_t g_out[512]={'h','e','l','l','o',0};std::string g_result;
 int status=0,updates=0,shutdowns=0;
 bool oskActive(){return g_active;}
 int sceUtilityOskGetStatus(){return status;}void sceUtilityOskUpdate(int n){assert(n==1);++updates;}
 void sceUtilityOskShutdownStart(){++shutdowns;}
 uint64_t sceKernelGetSystemTimeWide(){return 1000000;}
 std::string toUtf8(const uint16_t* s){std::string r;while(*s)r+=char(*s++);return r;}
 METHOD
}
struct Rect{float x,y,w,h;};
struct UiRenderer{int begins=0,ends=0;void fill(Rect,uint32_t){}void beginFrame(){++begins;}void endFrame(){++ends;}};
struct FrameStats{};
struct WorldScreen{struct{int draws=0;void background(UiRenderer&,bool dim){assert(dim);++draws;}}kit_;
 int worldDraws=0,inventoryDraws=0;bool psp=false,uiMode_=true,bagOpen=false;
 static constexpr uint8_t kWinBags=1,kWinBank=2;
 bool pspLayoutActive()const{return psp;}bool windowOpen(uint8_t)const{return bagOpen;}
 void drawHudLayers(UiRenderer&,const FrameStats&){++inventoryDraws;}
 void draw(UiRenderer&,const FrameStats&);};
DRAW
int g_frameInterval=2;bool oskActive(){return pp::oskActive();}int interval(){return EXPRESSION;}
int main(){
 WorldScreen w;UiRenderer ui;FrameStats stats;
 assert(interval()==2);pp::g_active=true;assert(interval()==1&&g_frameInterval==2);
 for(int frame=0;frame<300;++frame)w.draw(ui,stats);
 assert(w.worldDraws==0&&w.kit_.draws==300&&ui.begins==300&&ui.ends==300);
 pp::status=1;pp::oskAfterGeSync();assert(pp::updates==0&&pp::shutdowns==0);
 pp::status=2;for(int i=0;i<5;++i)pp::oskAfterGeSync();assert(pp::updates==5);
 pp::status=3;for(int i=0;i<5;++i)pp::oskAfterGeSync();assert(pp::shutdowns==1&&!pp::g_finished);
 pp::status=4;pp::oskAfterGeSync();assert(pp::g_active&&!pp::g_finished);
 pp::status=0;pp::oskAfterGeSync();assert(!pp::g_active&&pp::g_finished&&pp::g_accepted&&pp::g_result=="hello");
 assert(interval()==2&&g_frameInterval==2);w.draw(ui,stats);assert(w.worldDraws==1);
 w.psp=true;w.bagOpen=true;for(int i=0;i<100;++i)w.draw(ui,stats);assert(w.inventoryDraws==100&&w.worldDraws==1);
 w.uiMode_=false;w.draw(ui,stats);assert(w.worldDraws==2);
 pp::g_active=true;pp::g_shutdownSent=true;pp::g_finished=false;pp::g_params.base.result=1;pp::g_data.result=1;pp::oskAfterGeSync();
 assert(!pp::g_active&&pp::g_finished&&!pp::g_accepted&&interval()==2);
 pp::oskAfterGeSync();assert(pp::shutdowns==1); // no lifecycle work after ownership ends
 std::puts("OSK responsiveness passed: 300 lightweight frames, 60 Hz dialog pacing, stored cap restoration, accepted/cancelled lifecycle, one shutdown.");
}
'''.replace('METHOD',method).replace('DRAW',draw).replace('EXPRESSION',expr)
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-osk-fast-') as temp:
 folder=Path(temp);src=folder/'osk.cpp';exe=folder/'osk.exe';src.write_text(cpp)
 if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc',str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'osk.obj')]
 else:args=[compiler,'-std=c++20',str(src),'-o',str(exe)]
 subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
