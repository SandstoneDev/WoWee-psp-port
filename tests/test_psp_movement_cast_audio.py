"""Production movement/cast routing and server sound/damage packet regressions."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
def function(file,sig):
 s=(root/file).read_text();a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
methods=''.join(function('src/ui/psp/ui_world_sounds.cpp',sig) for sig in ['void WorldSounds::playLanding(', 'void WorldSounds::updateActivity(', 'bool WorldSounds::spatial('])
source=(root/'src/ui/psp/ui_world_spellfx.cpp').read_text(encoding='utf-8')
kit=source[source.index('    struct CastSound {'):source.index('    float castRemaining(')]
s=(root/'src/platform/psp/psp_world_session_spells.cpp').read_text();a=s.index('case LogicalOpcode::SMSG_ENVIRONMENTAL_DAMAGE_LOG:');b=s.index('case LogicalOpcode::SMSG_SPELLNONMELEEDAMAGELOG:',a)
env='bool damage(Packet& packet){switch(LogicalOpcode::SMSG_ENVIRONMENTAL_DAMAGE_LOG){'+s[a:b]+'default:return false;}}\n'
s=(root/'src/platform/psp/psp_world_session_chat.cpp').read_text();a=s.index('case LogicalOpcode::SMSG_PLAY_SOUND:');b=s.index('case LogicalOpcode::SMSG_MESSAGECHAT:',a)
server='bool server(LogicalOpcode op,Packet& packet){const bool inWorld=true;switch(op){'+s[a:b]+'default:return false;}}\n'
cpp=r'''
#include <glm/glm.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <deque>
#include <array>
#include <map>
#define PSP_LOGI(...) ((void)0)
namespace pp{uint32_t now=10000;uint32_t logUptimeMs(){return now;}}
using StepSurface=int;
struct WorldSounds {
 bool actInit_=false,prevGrounded_=true,prevFalling_=false,prevSwimming_=false;float sinceWaterExit_=.45f;
 uint32_t lastLandMs_=0,lastSplashMs_=0,lastStrokeMs_=0;
 std::vector<int> landing_[7]{{11}},splash_,swimStroke_,played;
 glm::vec3 listener_{0};float listenerYaw_=0;
 bool active(){return true;}bool elapsed(uint32_t&,float,uint32_t){return true;}
 int pick(const std::vector<int>& v){return v.empty()?-1:v[0];}float rnd(float a,float b){return (a+b)*.5f;}
 bool play2D(int clip,float,float=1){if(clip>=0)played.push_back(clip);return clip>=0;}
 void playLanding(StepSurface,bool);void updateActivity(float,bool,float,bool,bool,StepSurface);
 bool spatial(const glm::vec3&,float,float&,float&,float=1.f)const;
};
struct Meta{uint32_t precastSound=10,castSound=20,channelSound=30;};
struct Cast {
 struct {Meta meta;const Meta* kit(uint32_t){return &meta;}}fx;
 struct World {
  struct {uint64_t playerGuid(){return 1;}}session_;
  struct Sounds {std::vector<uint32_t> entries,tags,canceled;std::vector<bool> positional,sustained;
   void cancelSoundTag(uint32_t tag){canceled.push_back(tag);}
   bool playEntry(uint32_t id,const glm::vec3* at=nullptr,uint32_t tag=0,bool sustain=false){sustained.push_back(sustain);tags.push_back(tag);entries.push_back(id);positional.push_back(at!=nullptr);return true;}}sounds_;
  bool unitPosition(uint64_t,glm::vec3& at,float& h){at={50,50,50};h=2;return true;}
 }w;
 std::map<uint64_t,float> remaining;float castRemaining(uint64_t caster){auto it=remaining.find(caster);return it==remaining.end()?10.f:it->second;}
 uint32_t soundLogs=0;
'''+kit+r'''
};
struct Packet {
 std::vector<uint8_t> bytes;size_t pos=0;bool hasRemaining(size_t n){return bytes.size()-pos>=n;}
 uint8_t readUInt8(){return bytes[pos++];}uint32_t readUInt32(){uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(bytes[pos++])<<(i*8);return v;}
 uint64_t readUInt64(){uint64_t a=readUInt32();return a|(uint64_t(readUInt32())<<32);}
 void put(uint64_t v,int n){for(int i=0;i<n;++i)bytes.push_back(uint8_t(v>>(8*i)));}
};
enum class LogicalOpcode{SMSG_ENVIRONMENTAL_DAMAGE_LOG,SMSG_PLAY_SOUND,SMSG_PLAY_OBJECT_SOUND};
namespace game{struct CombatTextEntry{enum {ENVIRONMENTAL,ABSORB,RESIST};};}
struct Session {
 uint64_t playerGuid_=1;
 struct Text{int type,amount;uint8_t environment=255;};std::deque<Text> spellTexts_;
 struct ServerSound{uint32_t id=0;uint64_t source=0;};std::deque<ServerSound> serverSounds_;
 void pushSpellText(int type,int amount,int,bool,int,uint64_t){spellTexts_.push_back({type,amount});}
'''+env+server+r'''
};
'''
main=r'''
Packet environmental(uint64_t victim,uint8_t type,uint32_t damage,uint32_t absorb=0){Packet p;p.put(victim,8);p.put(type,1);p.put(damage,4);p.put(absorb,4);p.put(0,4);return p;}
int main(){
 WorldSounds sounds;sounds.updateActivity(.1f,true,0,false,false,0);
 for(int i=0;i<100;++i){pp::now+=1000;sounds.updateActivity(.1f,false,8,false,false,0);sounds.updateActivity(.1f,false,-8,false,false,0);sounds.updateActivity(.1f,true,0,false,false,0);}
 assert(sounds.played.size()==100);for(int clip:sounds.played)assert(clip==11); // only ground-contact steps, no voice
 float gain=0,pan=0;assert(sounds.spatial({10,0,0},50,gain,pan,8)&&std::abs(gain-.8f)<.001f);
 assert(sounds.spatial({1,0,0},50,gain,pan,8)&&gain==1);assert(!sounds.spatial({51,0,0},50,gain,pan,8));
 Cast cast;cast.kitSound(7,1,true);cast.kitSound(7,1,false);cast.kitSound(7,2,false,true);
 assert((cast.w.sounds_.sustained==std::vector<bool>{true,false,true}));
 assert((cast.w.sounds_.entries==std::vector<uint32_t>{10,20,30}));
 assert(!cast.w.sounds_.positional[0]&&!cast.w.sounds_.positional[1]&&cast.w.sounds_.positional[2]);
 cast.fx.meta.precastSound=0xFFFFFFFF;cast.kitSound(7,1,true);assert(cast.w.sounds_.entries.size()==3);
 Cast lifecycle;lifecycle.kitSound(7,1,true);const auto first=lifecycle.w.sounds_.tags.back();
 lifecycle.kitSound(7,2,true);const auto other=lifecycle.w.sounds_.tags.back();
 assert(first && other && first!=other);
 lifecycle.kitSound(7,1,false);
 assert(lifecycle.w.sounds_.canceled.size()==1 && lifecycle.w.sounds_.canceled[0]==first);
 assert(lifecycle.w.sounds_.tags.back()==0); // release is not tied to charge-up cancellation
 lifecycle.stopCastSound(2);assert(lifecycle.w.sounds_.canceled.back()==other);
 lifecycle.kitSound(7,3,false,true);const auto channel=lifecycle.w.sounds_.tags.back();
 lifecycle.remaining[3]=-1;lifecycle.updateCastSounds();assert(lifecycle.w.sounds_.canceled.back()==channel);
 const auto canceled=lifecycle.w.sounds_.canceled.size();lifecycle.updateCastSounds();assert(lifecycle.w.sounds_.canceled.size()==canceled);
 Session session;auto p=environmental(1,2,25);assert(session.damage(p));
 assert(session.spellTexts_.size()==1&&session.spellTexts_[0].environment==2&&session.spellTexts_[0].amount==25);
 session.spellTexts_.clear();p=environmental(1,2,0,100);session.damage(p);assert(session.spellTexts_.size()==1&&session.spellTexts_[0].type==game::CombatTextEntry::ABSORB);
 session.spellTexts_.clear();p=environmental(2,2,25);session.damage(p);assert(session.spellTexts_.empty());
 p=environmental(1,2,25);p.bytes.resize(20);session.damage(p);assert(session.spellTexts_.empty());
 Packet sound;sound.put(12570,4);session.server(LogicalOpcode::SMSG_PLAY_SOUND,sound);assert(session.serverSounds_.back().id==12570&&!session.serverSounds_.back().source);
 sound={};sound.put(11967,4);sound.put(99,8);session.server(LogicalOpcode::SMSG_PLAY_OBJECT_SOUND,sound);assert(session.serverSounds_.back().source==99);
 sound={};sound.put(1,3);const auto count=session.serverSounds_.size();session.server(LogicalOpcode::SMSG_PLAY_SOUND,sound);assert(session.serverSounds_.size()==count);
 for(int i=0;i<30;++i){sound={};sound.put(100+i,4);session.server(LogicalOpcode::SMSG_PLAY_SOUND,sound);}assert(session.serverSounds_.size()==16&&session.serverSounds_.back().id==129);
 puts("Audio passed: 100 silent-voice jumps, landing contacts, authored attenuation, self/other casts, fall-damage packets, event voices, truncation and bounded queues.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-cast-audio-') as temp:
 folder=Path(temp);src=folder/'audio.cpp';exe=folder/'audio.exe';src.write_text(cpp+methods+main)
 includes=[root.parent/'tooling/glm-1.0.1']
 if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/O2']+['/I'+str(p) for p in includes]+[str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'audio.obj')]
 else:args=[compiler,'-std=c++20','-O2']+['-I'+str(p) for p in includes]+[str(src),'-o',str(exe)]
 subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
