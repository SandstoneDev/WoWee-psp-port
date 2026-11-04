"""Run the production stationary-stream retry branch after a failed initial building load."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/ui/psp/ui_world_streaming.cpp').read_text(encoding='utf-8')
a=s.index('} else if (queue_.empty() && !job_.active && static_cast<int32_t>(pp::logUptimeMs() - retryScanMs_) >= 0) {')+2
b=s.index('    const uint32_t budgetUs',a)
branch=s[a:b]
cpp=r'''#include <cassert>
#include <cstdint>
#include <vector>
#include "ui/psp/ui_teleport_hold.hpp"
#include "rendering/psp/psp_tile_budget.hpp"
namespace pp {uint32_t now=1000;uint32_t logUptimeMs(){return now;}}
struct TileRec{int x=31,y=27;bool terrain=true,doodads=false;uint32_t retryMs=0;bool doodadRetryDue(uint32_t now) const{return now>=retryMs;}};
struct Stream {
 std::vector<int> queue_;struct{bool active=false;}job_;uint32_t retryScanMs_=0;
 std::vector<TileRec> tiles_{{}};int scans=0;
 bool inWindow(int,int){return true;}
 bool needsTerrain(const TileRec& t){return !t.terrain;}
 bool needsDoodads(const TileRec& t,float,float){return !t.doodads;}
 void computeQueue(float,float){++scans;queue_.push_back(1);}
 void update(){struct{float x=2268.43f,y=243.88f;}env;
 if(false){} ''' + branch + r'''
 }
};
int main(){
 Stream s;s.update();assert(s.scans==1); // failed building job, no movement, queue empty
 s.queue_.clear();pp::now+=100;s.update();assert(s.scans==1); // bounded retry cadence
 pp::now+=1000;s.update();assert(s.scans==2);
 s.queue_.clear();s.tiles_[0].doodads=true;pp::now+=1000;s.update();assert(s.scans==2); // ready tile stops retrying
 s.tiles_[0].terrain=false;pp::now+=1000;s.update();assert(s.scans==3); // terrain retry preserved
 s.queue_.clear();s.tiles_[0].terrain=true;s.tiles_[0].doodads=false;
 s.tiles_[0].retryMs=pp::now+5000;pp::now+=1000;s.update();assert(s.scans==3);
 pp::now+=5000;s.update();assert(s.scans==4);
 using wowee::rendering::psp::tilePlacementHeadroom;
 constexpr uint32_t kib=1024;
 assert(tilePlacementHeadroom(1536,100*kib,true)==3122*kib);
 assert(tilePlacementHeadroom(1536,100*kib,false)==3222*kib);
 using namespace wowee::ui::psp;TeleportHoldState hold;
 hold.terrainLoaded=true;hold.floorNear=true;
 assert(teleportHoldDecide(hold,87,8000)==TeleportHoldWhy::kHold); // hearth inn not installed yet
 hold.doodadsLoaded=true;assert(teleportHoldDecide(hold,1000,8000)==TeleportHoldWhy::kFloor);
 hold.doodadsLoaded=false;hold.insideBigWmo=true;hold.wmoReady=true;
 assert(teleportHoldDecide(hold,1000,8000)==TeleportHoldWhy::kFloor);
 hold.objectCollisionReady=false;
 assert(teleportHoldDecide(hold,1000,8000)==TeleportHoldWhy::kHold);
 hold.objectCollisionReady=true;
 assert(teleportLeavesWindow(-8800.f, 900.f, -8950.f, -132.5f, 1));
 assert(!teleportLeavesWindow(-8800.f, 300.f, -8950.f, -132.5f, 1));
 hold.wmoReady=false;assert(teleportHoldDecide(hold,1000,8000)==TeleportHoldWhy::kHold);
 assert(teleportHoldDecide(hold,8000,8000)==TeleportHoldWhy::kTimeout);
}
'''
with tempfile.TemporaryDirectory(prefix='wowee-stream-retry-') as temp:
 p=Path(temp);src=p/'retry.cpp';exe=p/'retry.exe';src.write_text(cpp,encoding='utf-8')
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'c++','-std=c++20','-I'+str(root/'include'),str(src),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('Stationary failed-building retries and safe hearthstone landing holds passed.')
