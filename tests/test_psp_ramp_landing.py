"""Run the production airborne integration against ramps, decks and loading holds."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'src/ui/psp/ui_world_screen.cpp').read_text()
start = source.index('        if (airborne_) {', source.index('// Integrate horizontal motion'))
body = source.index('{', start)
depth, end = 1, body+1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
integration = source[start:end]
cpp = r'''
#include "rendering/psp/psp_collision_math.hpp"
#include <cassert>
#include <cstdio>
namespace rendering = wowee::rendering;
#define PSP_LOGW(...) ((void)0)
struct Terrain { bool heightAt(float,float,float&) {return false;} };
struct World {
    float playerZ_=0,vz_=0,fallStartZ_=0;
    bool airborne_=true,wmoOnly_=false,wmoSafeValid_=false;
    glm::vec3 wmoSafePos_{};
    int wmoVoidFalls_=0;
    Terrain terrain_;
    float slope=0,deck=0;
    bool ready=true,missing=false;
    bool collisionReadyAt(float,float,float) {return ready;}
    float floorAt(float x,float,float probe) {
        float z=deck+slope*x;
        return !missing && z<=probe ? z : -1e30f;
    }
    bool step(float& nx,float& ny,float& sx,float& sy,float dt) {
        constexpr float kGravity=19.29f;
        bool collisionHeld=false;
        struct {float dt;} slices{dt};
        for(int i=0;i<1;++i) {
'''
tail = r'''
            nx=sx;ny=sy;
        }
        return collisionHeld;
    }
};
int main() {
    // Reproduce the old bug: at the endpoint the ramp is already above the old
    // vertical-ray origin, although the feet crossed it while moving forward.
    for(float origin:{0.f,60.45f,-42.42f})
    for(float slope:{.25f,.6f,1.0f,1.19f})
    for(int sign:{-1,1}) {
        World w;w.deck=origin;w.slope=slope*sign;
        w.playerZ_=origin+.005f;w.fallStartZ_=w.playerZ_;w.vz_=-1.f;
        float nx=0,ny=0,sx=.25f*sign,sy=0;
        const float ground=w.deck+w.slope*sx;
        assert(w.floorAt(sx,sy,w.playerZ_+.05f)<-1e29f);
        assert(!w.step(nx,ny,sx,sy,1.f/60));
        assert(!w.airborne_&&w.vz_==0&&std::fabs(w.playerZ_-ground)<.0001f);
    }
    // Low FPS/high downward speed still catches a thin deck (no tunnelling).
    World deck;deck.playerZ_=1.f;deck.fallStartZ_=1.f;deck.vz_=-60;
    auto slices=rendering::psp::movementSlices(.1f,62.f);
    float nx=0,ny=0,sx=0,sy=0;
    for(int i=0;i<slices.count&&deck.airborne_;++i)deck.step(nx,ny,sx,sy,slices.dt);
    assert(!deck.airborne_&&deck.playerZ_==0);
    // Moving uphill underneath a separate deck must not pull the player up to it.
    World under;under.deck=2;under.playerZ_=0;under.vz_=-1;
    sx=.25f;sy=0;nx=ny=0;
    under.step(nx,ny,sx,sy,1.f/60);
    assert(under.airborne_&&under.playerZ_<0);
    // Rising jumps are not converted to landings, even if a ramp is in range.
    World jumping;jumping.playerZ_=.02f;jumping.vz_=2;jumping.slope=1;
    sx=.25f;nx=ny=0;
    jumping.step(nx,ny,sx,sy,1.f/60);
    assert(jumping.airborne_&&jumping.vz_>0&&jumping.playerZ_>.02f);
    // A real ledge or a missing floor still permits normal falling.
    World ledge;ledge.deck=-3;ledge.playerZ_=1;ledge.vz_=-1;
    sx=0;nx=ny=0;
    ledge.step(nx,ny,sx,sy,1.f/60);
    assert(ledge.airborne_&&ledge.playerZ_<1&&ledge.playerZ_>0);
    World missing;missing.missing=true;missing.vz_=-1;
    missing.step(nx,ny,sx,sy,1.f/60);
    assert(missing.airborne_&&missing.playerZ_<0);
    // A pending collision read holds both position and velocity until ready.
    World pending;pending.ready=false;pending.playerZ_=3;pending.vz_=-1;
    assert(pending.step(nx,ny,sx,sy,1.f/60));
    assert(pending.playerZ_==3&&pending.vz_==-1);
    std::puts("Ramp landing passed: uphill descent, both directions, world heights, thin deck, ceilings, jumping, ledges, pending collision.");
}
'''
compiler = sys.argv[1] if len(sys.argv)>1 else 'c++'
glm = root.parent/'tooling/glm-1.0.1'
with tempfile.TemporaryDirectory(prefix='wowee-ramp-') as temp:
    folder=Path(temp); src=folder/'ramp.cpp'; exe=folder/'ramp.exe'
    src.write_text(cpp+integration+tail)
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/I'+str(root/'include'),'/I'+str(glm),str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'ramp.obj')]
    else:
        args=[compiler,'-std=c++20','-O2','-I'+str(root/'include'),'-I'+str(glm),str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True)
    subprocess.run([str(exe)],check=True)
