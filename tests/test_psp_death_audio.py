"""Exercise production death/ghost music transitions without PSP audio hardware."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/psp/ui_world_audio.cpp').read_text(encoding='utf-8');a=s.index('    const uint8_t mode=',s.index('void WorldScreen::updateAudio'));b=s.index('    if (!musicOk_) return;',a)
body=s[a:b]
s=(root/'src/ui/psp/ui_world_sounds.cpp').read_text(encoding='utf-8');a=s.index('void WorldSounds::playDeath()');b=s.index('\n}',a)+2;voice=s[a:b]
code='''#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cassert>
#include <cstdio>
#define PSP_LOGI(...) ((void)0)
#define PSP_LOGW(...) ((void)0)
namespace pp {
struct WorldSession {enum class Life{Alive,Dead,Ghost};Life value=Life::Alive;Life life()const{return value;}};
int starts=0,stops=0;bool active=true,exists=true,loop=false;float volume=0;
void musicStop(int){++stops;active=false;}
void musicSetVolume(float v){volume=v;}
bool musicActive(){return active;}
bool musicFilePath(char* p,size_t,const char* name){assert(std::string(name)=="ambience/ghoststate.mp3");std::strcpy(p,name);return exists;}
void musicPlay(const char*,bool l,int,int){++starts;loop=l;active=true;}
}
struct Settings{struct Values{int ambientvol=35,musicvol=30;}v;const Values& values()const{return v;}};
struct WorldScreen {
pp::WorldSession session_;Settings settings_;unsigned char deathAudioMode_=255;float deathAudioRetry_=0;
char pendingTrack_[256]="old_zone.mp3";float musicCooldown_=9;unsigned musicKey_=1;bool musicWasPlaying_=true;
void updateAudio(float dtSec){'''+body+'''}
};
struct WorldSounds {std::vector<int> deathVoice_{7};int plays=0;bool on=true;bool active()const{return on;}int pick(const std::vector<int>& v){return v[0];}void play2D(int clip,float gain,float pitch){assert(clip==7 && gain==1 && pitch==1);++plays;}void playDeath();};
'''+voice+'''
int main(){
WorldScreen w;w.updateAudio(.1f);assert(pp::stops==0 && pp::starts==0);
w.session_.value=pp::WorldSession::Life::Dead;w.updateAudio(.1f);assert(pp::stops==1 && pp::starts==1 && pp::loop && pp::volume==.35f && !w.pendingTrack_[0]);
w.updateAudio(.1f);assert(pp::starts==1);
w.session_.value=pp::WorldSession::Life::Ghost;w.updateAudio(.1f);assert(pp::starts==1 && pp::stops==1);
w.session_.value=pp::WorldSession::Life::Alive;w.updateAudio(.1f);assert(pp::stops==2 && pp::volume==.30f && w.musicKey_==0xffffffffu);
pp::exists=false;w.session_.value=pp::WorldSession::Life::Dead;w.updateAudio(.1f);assert(pp::starts==1 && w.deathAudioRetry_==5.f);
w.updateAudio(1.f);assert(w.deathAudioRetry_==4.f);
WorldSounds sounds;sounds.playDeath();assert(sounds.plays==1);sounds.on=false;sounds.playDeath();assert(sounds.plays==1);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code);exe=p/'test.exe';subprocess.run([sys.argv[1],'-std=c++20',p/'test.cpp','-o',exe],check=True);subprocess.run([exe],check=True)
print('Death audio: alive/dead/ghost/resurrection transitions, pending-zone cancellation, ambient volume, missing-file backoff and death voice passed.')
