"""Reproduce a movement/loading deadlock at a vertical neighborhood boundary."""
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/rendering/psp/psp_bigwmo.cpp').read_text()
start=source.index('void BigWmo::setCollisionProbe(');body=source.index('{',start);end=body+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
method=source[start:end]
cpp=r'''
#include "rendering/psp/psp_collision_neighborhood.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cstdio>
using namespace wowee::rendering::psp;
class BigWmo {
public:
    glm::mat4 M_{1};glm::mat3 Ai_{1};glm::vec3 collisionProbeLocal_{};bool collisionProbeActive_=false,collisionDirty_=false;
    void setCollisionProbe(const glm::vec3*);
};
'''
main=r'''
int main(){
    for(float scale:{.5f,1.f,2.f})for(int direction:{-1,1}) {
        const glm::vec3 held(1626.62f,240.73f,-42.42f);
        const glm::vec3 blocked=held+glm::vec3(0,0,.03f*direction/scale);
        std::vector<CollisionBoundsIndex::Entry> boxes;
        for(uint32_t i=0;i<9;++i)boxes.push_back({held-glm::vec3(1,1,1),held+glm::vec3(1,1,1),i});
        glm::vec3 lo=held-glm::vec3(1,1,0),hi=held+glm::vec3(1,1,0);
        if(direction>0){lo.z=held.z+3.015f/scale;hi.z=held.z+4.f/scale;}
        else{lo.z=held.z-9.f/scale;hi.z=held.z-8.015f/scale;}
        boxes.push_back({lo,hi,9});CollisionBoundsIndex index;index.build(boxes);
        bool wanted[10]={},resident[10]={};float priorities[10]={};
        auto prepare=[&]{collisionNeighborhood(index,held,scale,10.f,[&](uint32_t id,float d2){
            wanted[id]=true;priorities[id]=collisionStreamPriority(std::sqrt(d2)*scale);
        });};
        prepare();for(unsigned i=0;i<9;++i)resident[i]=true;
        assert(!wanted[9]); // old loader never requests the last chunk
        unsigned need=0,have=0;
        collisionNeighborhood(index,blocked,scale,kCollisionLandingRadius,[&](uint32_t id,float){++need;have+=resident[id];});
        assert(need==10&&have==9);
        // The repaired plan includes BOTH held and blocked neighborhoods.
        collisionNeighborhood(index,blocked,scale,kCollisionLandingRadius,[&](uint32_t id,float d2){
            const float priority=-20000.f+std::sqrt(d2)*scale;
            priorities[id]=wanted[id]?std::min(priorities[id],priority):priority;wanted[id]=true;
        });
        assert(wanted[9]&&priorities[9]<0);
        assert(collisionReadAllowed(true,true,priorities[9]));
        assert(collisionPlanFits(4096,1024,4096,true)); // essential even under a full cosmetic budget
        resident[9]=true;need=have=0;
        collisionNeighborhood(index,blocked,scale,kCollisionLandingRadius,[&](uint32_t id,float){++need;have+=resident[id];});
        assert(need==have); // movement can resume without disabling collision
    }
    BigWmo w;glm::vec3 p(1,2,3);
    w.setCollisionProbe(nullptr);assert(!w.collisionDirty_);
    w.setCollisionProbe(&p);assert(w.collisionDirty_&&w.collisionProbeActive_);
    w.collisionDirty_=false;w.setCollisionProbe(&p);assert(!w.collisionDirty_);
    p.z+=.03f;w.setCollisionProbe(&p);assert(w.collisionDirty_); // no 1-yard/20-frame plan delay
    w.collisionDirty_=false;w.setCollisionProbe(nullptr);assert(w.collisionDirty_&&!w.collisionProbeActive_);
    w.M_=glm::translate(glm::mat4(1),glm::vec3(10,20,30));
    p={11,22,33};w.setCollisionProbe(&p);assert(w.collisionProbeLocal_==glm::vec3(1,2,3));
    std::puts("Bridge loading passed: 9/10 deadlock, ascent/descent, scales, low-memory priority, immediate replanning, transforms.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++';glm=root.parent/'tooling/glm-1.0.1'
with tempfile.TemporaryDirectory(prefix='wowee-bridge-') as temp:
    folder=Path(temp);src=folder/'bridge.cpp';exe=folder/'bridge.exe';src.write_text(cpp+method+main)
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/I'+str(root/'include'),'/I'+str(glm),str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'bridge.obj')]
    else:args=[compiler,'-std=c++20','-O2','-I'+str(root/'include'),'-I'+str(glm),str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
