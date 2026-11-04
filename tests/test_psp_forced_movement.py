"""Exercise the production self-spline handler and endpoint delivery with real Entity interpolation."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/platform/psp/psp_world_session_move.cpp').read_text(encoding='utf-8')
def function(sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
methods=''.join(function(sig) for sig in ['void WorldSession::handleMonsterMove(', 'bool WorldSession::takeForcedMovement('])
cpp=r'''#include "game/world_packets.hpp"
#include "core/coordinates.hpp"
#include <cassert>
#include <cstdio>
using namespace wowee;
#define PSP_LOGI(...) do{}while(0)
#define PSP_LOGW(...) do{}while(0)
constexpr float kPi=3.14159265358979323846f;
game::MonsterMoveData input;
bool game::MonsterMoveParser::parse(network::Packet&,game::MonsterMoveData& out){out=input;return true;}
struct WorldSession{
 std::shared_ptr<game::Entity> ownForcedEntity_;uint64_t playerGuid_=1;bool ownForcedMove_=false,onTaxiFlight_=false,taxiActivatePending_=false,taxiClientActive_=false,teleportPending_=false,vehicle=false;
 uint32_t monsterMoveFails_=0,monsterMoves_=0,monsterMoveLogs_=0;int taxiCalls=0;
 struct Entities{std::map<uint64_t,std::shared_ptr<game::Entity>> entries;
 const auto& getEntities()const{return entries;}
 auto getEntity(uint64_t guid)const{auto i=entries.find(guid);return i==entries.end()?std::shared_ptr<game::Entity>{}:i->second;}}entities_;
 void taxiOwnMonsterMove(const game::MonsterMoveData&){++taxiCalls;}
 void noteSplineGait(const game::MonsterMoveData&){}
 bool inVehicle()const{return vehicle;}
 void updateEntities(float);
 void handleMonsterMove(network::Packet&);
 bool takeForcedMovement(glm::vec3&,float&,bool&);
};
'''

source=(root/'src/platform/psp/psp_world_session.cpp').read_text(encoding='utf-8')
a=source.index('void WorldSession::updateEntities(float dtSec) {');b=source.index('\n}',a)+2
methods+=source[a:b]+'\n'
main=r'''int main(){
 WorldSession s;auto me=std::make_shared<game::Entity>(1);/* local player deliberately absent from remote entity list */
 auto npc=std::make_shared<game::Entity>(2);s.entities_.entries[2]=npc;
 network::Packet packet;input.guid=1;input.x=10;input.y=20;input.z=3;input.destX=20;input.destY=20;input.destZ=3;input.duration=500;input.hasDest=true;
 me->setPosition(-100,-100,-100,0);s.handleMonsterMove(packet);assert(s.ownForcedMove_ && s.taxiCalls==0 && !s.entities_.getEntity(1));
 s.updateEntities(.25f);glm::vec3 pos;float facing=0;bool moving=false;
 assert(s.takeForcedMovement(pos,facing,moving) && moving);assert(std::abs(pos.x-15)<.001f && pos.y==20 && pos.z==3);
 s.updateEntities(.30f);assert(s.takeForcedMovement(pos,facing,moving) && !moving);assert(pos.x==20 && pos.y==20);
 assert(!s.takeForcedMovement(pos,facing,moving)); // exact endpoint delivered once; normal input resumes
 input.waypoints={{13,21,3},{17,21,3}};s.handleMonsterMove(packet);s.updateEntities(.2f);
 assert(s.takeForcedMovement(pos,facing,moving) && moving && pos.x>10 && pos.x<20);
 input.moveType=1;input.hasDest=false;input.x=14;input.y=21;s.handleMonsterMove(packet);
 assert(s.takeForcedMovement(pos,facing,moving) && !moving && pos.x==14 && pos.y==21);
 input.moveType=0;input.hasDest=true;input.waypoints.clear();s.onTaxiFlight_=true;s.handleMonsterMove(packet);assert(s.taxiCalls==1 && !s.ownForcedMove_);
 s.onTaxiFlight_=false;input.guid=2;s.handleMonsterMove(packet);assert(!s.ownForcedMove_ && npc->isActivelyMoving());
 input.guid=1;s.handleMonsterMove(packet);s.teleportPending_=true;assert(!s.takeForcedMovement(pos,facing,moving) && !s.ownForcedMove_);
 std::puts("Forced movement passed: local Charge interpolation, waypoints, exact endpoint, explicit stop, taxi separation, NPC movement and teleport cancellation.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-forced-move-') as temp:
 src=Path(temp)/'test.cpp';exe=Path(temp)/'test.exe';src.write_text(cpp+methods+main,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O2','-I'+str(root/'include'),'-I'+str(root/'extern'),src,root/'src/math/spline.cpp',root/'src/network/packet.cpp',root/'src/core/logger.cpp','-o',exe],check=True)
 subprocess.run([exe],check=True)
