"""Check actual audio-open retry and borrowed-index lookup routines on the host."""
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
def function(text,signature):
    begin=text.index(signature);body=text.index('{',begin);end=body+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]+'\n'
methods=function((root/'include/platform/psp/psp_audio_io.hpp').read_text(),'inline int audioOpenRead(')
methods+=function((root/'src/platform/psp/psp_pak_index.cpp').read_text(),'bool PakIndex::findOnFd(')
methods+=function((root/'src/rendering/psp/psp_spellfx.cpp').read_text(),'bool SpellFxRenderer::loadKits(')
cpp=r'''
#include <cassert>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include "rendering/psp/psp_spellfx_format.hpp"
#define PSP_LOGW(...) ((void)0)
#define PSP_LOGI(...) ((void)0)
using SpellFxKitMeta=wowee::rendering::psp::SpellFxKitMeta;
namespace pp {
uint32_t now=100;unsigned kitOpens=0;bool failKitOpen=true,failKitLoad=false,noKits=false;
uint32_t logUptimeMs(){return now;}
void fsPath(char*,size_t,const char*){}
void pakFree(void* p){std::free(p);}
struct Entry {uint32_t size=64;};
struct Pak {
    Entry entry;
    bool open(const char*){++kitOpens;return !failKitOpen;}
    const Entry* find(const char*){return noKits?nullptr:&entry;}
    void* load(const Entry*){if(failKitLoad)return nullptr;auto* rows=static_cast<SpellFxKitMeta*>(std::calloc(2,sizeof(SpellFxKitMeta)));rows[0].visual=99;rows[1].visual=7;return rows;}
};
}
struct SpellFxRenderer {
    std::vector<SpellFxKitMeta> kits_;uint32_t kitsRetryMs_=0;bool kitsUnavailable_=false;
    bool loadKits();
};
constexpr int PSP_O_RDONLY=1;
constexpr int full=static_cast<int>(0x80020320u),missing=static_cast<int>(0x80010002u);
std::vector<int> results;unsigned opens=0,delays=0,reliefs=0;
int sceIoOpen(const char*,int flags,int){assert(flags==PSP_O_RDONLY);return results.at(opens++);}
void sceKernelDelayThread(unsigned us){assert(us==10000);++delays;}
int pakFdRelieve(int n){assert(n==1);++reliefs;return 0;}
struct PakEntry{};
struct PakIndex {
    int fd_=-1;bool lookupIoFailed_=false,succeed=true;
    bool find(const char*,PakEntry&){assert(fd_==42);return succeed;}
    bool findOnFd(const char*,PakEntry&,int);
};
'''
main=r'''
void reset(std::vector<int> next){results=next;opens=delays=reliefs=0;}
int main(){
    reset({17});assert(audioOpenRead("music.mp3")==17&&opens==1&&delays==0);
    reset({full,full,42});assert(audioOpenRead("spell.pak")==42&&opens==3&&delays==2&&reliefs==2);
    reset({full,full,full,full});assert(audioOpenRead("music.mp3")==full&&opens==4&&delays==3);
    reset({missing});assert(audioOpenRead("missing.mp3")==missing&&opens==1&&delays==0);
    PakIndex index;PakEntry entry;
    assert(index.findOnFd("record",entry,42)&&index.fd_==-1);
    index.fd_=7;index.succeed=false;
    assert(!index.findOnFd("missing",entry,42)&&index.fd_==7);
    assert(!index.findOnFd("record",entry,-1)&&index.lookupIoFailed_&&index.fd_==7);
    // One failed metadata open must not mute all later spell casts this login.
    SpellFxRenderer fx;assert(!fx.loadKits()&&pp::kitOpens==1);
    pp::failKitOpen=false;assert(!fx.loadKits()&&pp::kitOpens==1);
    pp::now=2101;pp::failKitLoad=true;assert(!fx.loadKits()&&pp::kitOpens==2);
    pp::now=4102;pp::failKitLoad=false;assert(fx.loadKits()&&fx.kits_[0].visual==7&&pp::kitOpens==3);
    assert(fx.loadKits()&&pp::kitOpens==3); // cached without opening a file again
    SpellFxRenderer oldPack;pp::noKits=true;assert(!oldPack.loadKits());
    pp::now=9000;unsigned calls=pp::kitOpens;assert(!oldPack.loadKits()&&pp::kitOpens==calls);
    std::puts("Audio file budget passed: bounded pressure recovery, no retry on missing assets, borrowed handles restored.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-audio-fds-') as temp:
    folder=Path(temp);src=folder/'audiofds.cpp';exe=folder/'audiofds.exe';src.write_text(cpp+methods+main)
    if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/I'+str(root/'include'),str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'audiofds.obj')]
    else:args=[compiler,'-std=c++20','-O2','-I'+str(root/'include'),str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
