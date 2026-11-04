"""Exercise production object-fog selection and GE state restore without PSP hardware."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
def function(file,sig):
 s=(root/file).read_text();a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
methods=''.join(function('src/rendering/psp/psp_lighting_ge.cpp',sig) for sig in [
 'FogRange lightFogRange(', 'void lightObjectFog(', 'void lightBegin(', 'void lightEnd('])
methods+=function('src/rendering/psp/psp_models.cpp','float ModelRenderer::instanceFogStart(')
cpp=r'''
#include "rendering/psp/psp_fog_policy.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <cassert>
#include <cstdio>
using namespace wowee::rendering::psp;
struct TerrainCamera{float farZ=450;glm::vec3 pos{0},forward{0,1,0};};
enum {kTintOff,kTintGeAmbient,GU_FOG,GU_DITHER,GU_LIGHTING,GU_LIGHT0,GU_LIGHT1,GU_LIGHT2,GU_LIGHT3,GU_AMBIENT,GU_FRAGMENT_2X};
struct FrameLight{bool adaptiveFog=false;bool fogBands=false;float fogStart=250,fogEnd=440;uint32_t fogABGR=0xFF987654,ambientABGR=0xFFFFFFFF;int tint=0;bool twoX=false;}g_frame;
FogRange g_objectFog{};float g_objectFogStart=0;uint32_t g_objectFogColor=0;
bool g_dither3d=false,g_lit=false,g_unlitNow=false;int kWorldDither=0;
struct FogCall{float start,end;uint32_t color;};std::vector<FogCall> calls;
void sceGuFog(float a,float b,uint32_t c){calls.push_back({a,b,c});}
void sceGuEnable(int){}void sceGuDisable(int){}void sceGuSetDither(const int*){}
void sceGuModelColor(int,uint32_t,uint32_t,int){}void sceGuColorMaterial(int){}void sceGuAmbient(uint32_t){}
FogRange lightFogRange(const TerrainCamera&);void lightObjectFog(float);void lightBegin(const TerrainCamera&,uint32_t);void lightEnd();
constexpr uint32_t kModelGroundDetail=16;
struct ModelRenderer {
 struct Header{uint32_t flags=0;};struct Model{Header* h;uint32_t readyMs=0;};
 struct Inst{float x=0,y=160,z=0,r=0,reach=1;Model* m;};
 float viewDistance=160,performanceScale=1,groundClutterDistance=25;uint32_t drawNowMs_=1000;
 float instanceFogStart(const Inst&,const TerrainCamera&,float)const;
};
'''
main=r'''
int main(){
 for(float far : {100.f,177.f,450.f}) {
  const auto baseline=fitFogRange(100,170,far),later=adaptiveFogRange(baseline,true);
  assert(later.start>baseline.start && later.end==baseline.end);
  assert(later.end-later.start>=(baseline.end-baseline.start)*.799f);
  const auto normal=adaptiveFogRange(baseline,false);assert(normal.start==baseline.start && normal.end==baseline.end);
 }
 TerrainCamera cam;lightBegin(cam,0xFF123456);const FogCall original=calls.back();
 lightObjectFog(original.start);assert(calls.size()==1);
 lightObjectFog(-1000);assert(calls.size()==2 && calls.back().end==original.end && calls.back().color==original.color);
 lightObjectFog(-1000);assert(calls.size()==2); // cached common state, no duplicate commands
 lightEnd();lightBegin(cam,0xFF123456);assert(calls.back().start==original.start);
 g_frame.fogBands=true;g_frame.fogStart=100;g_frame.fogEnd=170;g_frame.fogABGR=0xFF557799;
 lightBegin(cam,0);lightObjectFog(-500);assert(calls.back().end==170 && calls.back().color==0xFF557799);
 lightEnd();lightBegin(cam,0);assert(std::abs(calls.back().start-100)<.001f); // next terrain/NPC pass gets zone fog
 g_frame.fogStart=250;g_frame.fogEnd=440;
 cam.farZ=100;lightBegin(cam,0);
 assert(calls.back().end<=98.01f && calls.back().start<75.f);
 lightObjectFog(-500);assert(calls.back().end<=98.01f);
 cam.farZ=450;
 ModelRenderer renderer;ModelRenderer::Header header;ModelRenderer::Model model{&header};
 ModelRenderer::Inst in;in.m=&model;
 // Neither the distance edge, adaptive scale, grass limit nor model age may
 // move an object's fog start away from the normal zone fog.
 for(float distance : {0.f,25.f,80.f,160.f}) {
  for(float scale : {.3f,.5f,1.f}) {
   renderer.performanceScale=scale;
   for(uint32_t flags : {0u,kModelGroundDetail}) {
    header.flags=flags;model.readyMs=renderer.drawNowMs_;
    assert(std::abs(renderer.instanceFogStart(in,cam,distance)-250)<.001f);
   }
  }
 }
 puts("Object fog passed: authored zone fog preserved for grass/fences, no streamed white reveal, GE state restoration.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-object-fog-') as temp:
 folder=Path(temp);src=folder/'fog.cpp';exe=folder/'fog.exe';src.write_text(cpp+methods+main)
 includes=[root/'include',root.parent/'tooling/glm-1.0.1']
 if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/O2']+['/I'+str(p) for p in includes]+[str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'fog.obj')]
 else:args=[compiler,'-std=c++20','-O2']+['-I'+str(p) for p in includes]+[str(src),'-o',str(exe)]
 subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
