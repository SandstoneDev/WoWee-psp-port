"""Run production spell texture I/O against a limited file budget."""
from pathlib import Path
import subprocess
import tempfile
import sys
root = Path(__file__).resolve().parents[1]
source = (root/'src/rendering/psp/psp_spellfx.cpp').read_text()
def function(signature):
    begin=source.index(signature); end=source.index('{',begin)+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[begin:end]+'\n'
methods=''.join(function(s) for s in ['bool SpellFxRenderer::readRaw(',
    'bool SpellFxRenderer::readTexture(', 'const pp::Texture* SpellFxRenderer::texture(', 'bool SpellFxRenderer::loadKits('])
cpp=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#define PSP_LOGE(...) ((void)0)
#define PSP_LOGW(...) ((void)0)
#define PSP_LOGI(...) ((void)0)
struct SpellFxKitMeta { uint32_t visual; };
struct SpellFxKitStarts { uint32_t visual; };
namespace pp {
unsigned pakOpens=0, kitReads=0, startReads=0;
bool failStarts=true, haveStarts=true;
void fsPath(char*, size_t, const char*) {}
void pakFree(void* p) { std::free(p); }
struct Entry { size_t size; bool starts; };
struct Pak {
    bool open(const char*) { ++pakOpens; return true; }
    const Entry* find(std::string name) const {
        static const Entry kit{sizeof(SpellFxKitMeta),false}, start{sizeof(SpellFxKitStarts),true};
        return name=="kits" ? &kit : haveStarts ? &start : nullptr;
    }
    void* load(const Entry* entry) const {
        if(entry->starts){++startReads;if(failStarts)return nullptr;}else ++kitReads;
        return std::calloc(1,entry->size);
    }
};
struct Texture { void* pixels=nullptr; };
unsigned now=100,opens=0,closes=0,reads=0;
bool tight=true,failOpen=false,failRead=false,badAsset=false;
int pakOpenRetry(const char*) { ++opens;return failOpen?-1:7; }
void pakNoteOpen(int,const char*) {}
void pakNoteClose(int) {}
bool pakRead(int fd,uint32_t,void*,uint32_t) { assert(fd==7);++reads;return !failRead; }
bool pakFdTight() { return tight; }
uint32_t logUptimeMs() { return now; }
bool textureFromBlob(void* p,uint32_t,Texture& t,bool) {
    if(badAsset)return false;t.pixels=p;return true;
}
}
int sceIoClose(int fd) { assert(fd==7);++pp::closes;return 0; }
void sceKernelDcacheWritebackRange(void*,uint32_t) {}
void* memalign(size_t,size_t n) { return std::malloc(n); }
class SpellFxRenderer {
public:
struct TexEntry {
    pp::Texture tex;uint32_t triedFrame=0,lastFrame=0,bytes=0,retryMs=0;
    bool broken=false,ok=false;
    ~TexEntry(){std::free(tex.pixels);}
};
int fd_=-1;std::string path_="spellfx.pak";
uint32_t lastReadMs_=0,frame_=1,cacheBytes_=0;
std::vector<uint32_t> texOff_{100},texSize_{64};
std::unordered_map<uint16_t,std::unique_ptr<TexEntry>> textures_;
std::vector<SpellFxKitMeta> kits_;
std::vector<SpellFxKitStarts> kitStarts_;
uint32_t kitsRetryMs_=0;
bool kitsUnavailable_=false,kitStartsUnavailable_=false;
bool loadKits();
bool makeRoom(uint32_t) { return true; }
bool readRaw(uint32_t,void*,uint32_t);
bool readTexture(uint16_t,TexEntry&);
const pp::Texture* texture(uint16_t);
};
'''
main=r'''
int main() {
    SpellFxRenderer fx;fx.textures_[0]=std::make_unique<SpellFxRenderer::TexEntry>();
    pp::failOpen=true;assert(!fx.texture(0));
    auto& tex=*fx.textures_[0];assert(!tex.broken&&tex.retryMs);
    pp::failOpen=false;fx.frame_++;unsigned tries=pp::opens;
    assert(!fx.texture(0)&&pp::opens==tries); // bounded retry, no frame-by-frame open storm
    pp::now=1200;pp::failRead=true;fx.frame_++;
    assert(!fx.texture(0)&&!tex.broken&&fx.fd_==-1&&pp::closes==1);
    pp::now=2300;pp::failRead=false;fx.frame_++;
    assert(fx.texture(0)&&tex.ok&&tex.retryMs==0&&fx.fd_==-1&&pp::closes==2);
    tries=pp::opens;fx.frame_++;assert(fx.texture(0)&&pp::opens==tries);
    SpellFxRenderer bad;SpellFxRenderer::TexEntry entry;pp::badAsset=true;
    assert(!bad.readTexture(0,entry)&&entry.broken); // genuine invalid assets still terminal
    pp::badAsset=false;pp::tight=false;
    SpellFxRenderer spare;SpellFxRenderer::TexEntry cached;
    assert(spare.readTexture(0,cached)&&spare.fd_==7); // retain normal fast path with spare slots
    sceIoClose(spare.fd_);
    SpellFxRenderer metadata;
    pp::now=3000;
    assert(metadata.loadKits() && metadata.kitStarts_.empty() && pp::kitReads==1 && pp::startReads==1);
    assert(metadata.loadKits() && pp::pakOpens==1);
    pp::now=5100;pp::failStarts=false;
    assert(metadata.loadKits() && metadata.kitStarts_.size()==1 && pp::kitReads==1 && pp::startReads==2);
    assert(metadata.loadKits() && pp::pakOpens==2);
    SpellFxRenderer legacy;
    pp::haveStarts=false;
    assert(legacy.loadKits() && legacy.kitStartsUnavailable_);
    pp::now=9000;
    assert(legacy.loadKits() && pp::pakOpens==3);
    std::puts("Spell file pressure passed: temporary failures recover, tight readers release files, cached textures remain visible.");
}
'''
with tempfile.TemporaryDirectory(prefix='wowee-spell-pressure-') as temp:
    folder=Path(temp);src=folder/'test.cpp';exe=folder/'test.exe';src.write_text(cpp+methods+main)
    compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc',str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'test.obj')]
    else:args=[compiler,'-std=c++20',str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
