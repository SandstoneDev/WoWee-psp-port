"""Production fishing bobber resolution when channel/object updates arrive separately."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/platform/psp/psp_world_session_gouse.cpp').read_text(encoding='utf-8');a=s.index('uint64_t WorldSession::fishingBobber() const {');b=s.index('\n}\n',a)+2
method=s[a:b]
code='''#include "game/spell_classification.hpp"
#include <vector>
#include <cstdint>
#include <cassert>
namespace game=wowee::game;
namespace rp {constexpr uint8_t kGoFishingNode=17;}
constexpr uint16_t kUnitChannelObject=10;
struct GameObjectInfo {uint64_t guid=0,createdBy=0;uint8_t type=0;uint32_t seenMs=0;};
struct WorldSession {
std::vector<GameObjectInfo> gameObjects_;uint64_t playerGuid_=1,ch=0;uint32_t spell=7620;bool channel=true;
uint32_t selfField(uint16_t id) const {return id==kUnitChannelObject?uint32_t(ch):uint32_t(ch>>32);}
const GameObjectInfo* findGameObject(uint64_t g)const{for(const auto& n:gameObjects_)if(n.guid==g)return &n;return nullptr;}
bool isChanneling()const{return channel;}
uint32_t currentCastSpellId()const{return spell;}
uint64_t fishingBobber()const;
};
'''+method+'''
int main(){
WorldSession s;s.gameObjects_={{10,2,17,100},{11,1,17,100},{12,1,17,200}};
assert(s.fishingBobber()==12); // own newest node, never another player's bobber
s.ch=11;assert(s.fishingBobber()==11); // authoritative channel object wins
s.gameObjects_[1].type=0;assert(s.fishingBobber()==11); // template/type update not received yet
s.channel=false;assert(s.fishingBobber()==0); // no fishing fallback after channel cancellation
s.gameObjects_[1].type=17;assert(s.fishingBobber()==11); // known channel-object field remains authoritative
s.ch=0;s.channel=true;s.spell=133;assert(s.fishingBobber()==0); // no fallback on unrelated spell
s.spell=7620;s.gameObjects_.clear();assert(s.fishingBobber()==0);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code);exe=p/'test.exe'
 subprocess.run([sys.argv[1],'-std=c++20','-I'+str(root/'include'),p/'test.cpp','-o',exe],check=True);subprocess.run([exe],check=True)
print('Fishing owner resolution: authoritative channel object, delayed type/field updates, newest own node and cancellation passed.')
