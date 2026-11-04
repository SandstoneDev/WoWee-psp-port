"""Exercise actual bobber draw routing, including a model with an inert Custom0 clip."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/psp/ui_world_gameobjects.cpp').read_text(encoding='utf-8')
def method(sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
code='''#include "rendering/psp/psp_fishing_visual.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <cassert>
namespace rp=wowee::rendering::psp;
namespace wowee::rendering::psp {struct TerrainCamera {float fovyDeg=60,nearZ=1,farZ=300;glm::vec3 forward{1,0,0},up{0,0,1},pos{0,0,0};};}
struct ScePspFMatrix4 {float x[16];};
namespace pp {uint32_t now=1000;uint32_t logUptimeMs(){return now;}int displayListFree(){return 99999;}void textureResetBinding(){}}
enum {GU_PROJECTION,GU_VIEW,GU_MODEL,GU_TEXTURE_2D,GU_LIGHTING,GU_ALPHA_TEST,GU_FOG,GU_CULL_FACE,GU_DEPTH_TEST,GU_BLEND,GU_TRUE,GU_FALSE,GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,GU_TRIANGLES,GU_COLOR_8888,GU_VERTEX_32BITF,GU_TRANSFORM_3D};
int draws=0,vertices=0;alignas(16) unsigned char arena[8192];
void* sceGuGetMemory(unsigned n){assert(n<=sizeof(arena));return arena;}
void sceGuSetMatrix(int,const void*){}void sceGuEnable(int){}void sceGuDisable(int){}void sceGuDepthMask(int){}void sceGuBlendFunc(int,int,int,unsigned,unsigned){}
void sceGuDrawArray(int,int,int count,const void*,const void*){assert(count>0 && count<=240);++draws;vertices=count;}
struct Session {struct Node{glm::vec3 pos{1,2,3};}node;uint64_t guid=1;uint32_t bite=0;uint64_t fishingBobber()const{return guid;}const Node* findGameObject(uint64_t g)const{return g?&node:nullptr;}uint32_t bobberBiteMs()const{return bite;}};
struct WorldScreen {
Session session_;uint64_t fishingVisualGuid_=1;uint32_t fishingSpawnMs_=0;
struct InertClip {bool hasAnimation(uint64_t,unsigned)const{return true;}}creatures_;
float fishingBobberVisualOffset()const;
void drawFishingSplash(const rp::TerrainCamera&);
};
'''+method('float WorldScreen::fishingBobberVisualOffset() const')+method('void WorldScreen::drawFishingSplash(')+'''
int main(){WorldScreen w;rp::TerrainCamera cam;
w.drawFishingSplash(cam);assert(draws==0);
w.fishingSpawnMs_=950;w.drawFishingSplash(cam);assert(draws==1 && vertices>0); // no pole/model/line needed
w.session_.bite=550;assert(w.fishingBobberVisualOffset()<-.34f); // clip availability MUST NOT suppress dip
w.drawFishingSplash(cam);assert(draws==2);
pp::now=3000;w.drawFishingSplash(cam);assert(draws==2 && w.fishingBobberVisualOffset()==0);
w.session_.guid=0;w.fishingSpawnMs_=2990;w.drawFishingSplash(cam);assert(draws==2);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code);exe=p/'test.exe';subprocess.run([sys.argv[1],'-std=c++20','-I'+str(root/'include'),'-I'+str(root.parent/'tooling/glm-1.0.1'),p/'test.cpp','-o',exe],check=True);subprocess.run([exe],check=True)
print('Bobber visuals: inert animated clips cannot suppress dip; landing/bite splash independent of rod/line, bounded geometry and expiration passed.')
