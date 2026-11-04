"""Exercise tagged sustained sounds through real bank play, queue, cancel and eviction paths."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/platform/psp/psp_sound_bank.cpp').read_text(encoding='utf-8')
def fn(sig):
 a=s.index(sig);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]+'\n'
methods=''.join(fn(sig) for sig in ['bool SoundBank::start(', 'bool SoundBank::play(', 'void SoundBank::cancelTag(', 'void SoundBank::evict('])
cpp=r'''
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#define private public
#include "platform/psp/psp_sound_bank.hpp"
#undef private
#include "rendering/psp/psp_sound_format.hpp"
namespace wowee::platform::psp {
namespace fmt=wowee::rendering::psp;
constexpr uint32_t kBusyMarginMs=150;
bool playedLoop=false,usingPcm=true;uint32_t playedRate=0,playedTag=0,stoppedTag=0;
uint32_t logUptimeMs(){return 10000;}
int sfxPlay(const int16_t*,uint32_t,int,uint32_t rate,float,float,bool loop,uint32_t tag){playedLoop=loop;playedRate=rate;playedTag=tag;return 0;}
void sfxStopTag(uint32_t tag){stoppedTag=tag;}
bool sfxUsesPcm(const int16_t*){return usingPcm;}
SoundBank::~SoundBank(){}
bool SoundBank::request(int index){clips_[index].state=1;return true;}
''' + methods + r'''
}
using namespace wowee::platform::psp;
int main(){
 SoundBank bank;bank.open_=true;bank.clips_.resize(2);
 auto& clip=bank.clips_[0];clip.state=2;clip.size=sizeof(wowee::rendering::psp::SoundClipHeader)+200;
 clip.block=std::calloc(1,clip.size);auto* h=static_cast<wowee::rendering::psp::SoundClipHeader*>(clip.block);
 h->rate=22050;h->frames=100;h->channels=1;
 assert(bank.play(0,1,0,1,0x531,true));assert(playedLoop && playedTag==0x531 && playedRate==22050);
 assert(!bank.play(0,1,0,1,0,true)); // no unowned infinite voices
 assert(bank.play(0,1,0,1,0));assert(!playedLoop); // impact remains a one-shot
 assert(bank.play(1,1,0,1,0x532,true));assert(bank.late_[0].index==1 && bank.late_[0].loop);
 bank.cancelTag(0x532);assert(bank.late_[0].index==-1 && stoppedTag==0x532);
 bank.cancelTag(0x531);assert(stoppedTag==0x531);
 bank.budgetKb=0;bank.residentBytes_=clip.size;clip.busyUntilMs=0;
 bank.evict(20000);assert(clip.block); // playing PCM remains pinned past clip's original end
 usingPcm=false;bank.evict(20000);assert(!clip.block && bank.residentBytes_==0);
}
'''
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp);src=p/'sustain.cpp';exe=p/'sustain.exe';src.write_text(cpp)
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'c++','-std=c++20','-I'+str(root/'include'),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('Cast sustain passed: original rate, one-shots, deferred cancellation and active-PCM eviction protection.')
