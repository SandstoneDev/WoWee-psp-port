"""Exercise production NPC installation/ensure code across asynchronous states."""
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/rendering/psp/psp_creatures.cpp').read_text()
def function(signature):
    start=source.index(signature);body=source.index('{',start);end=body+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'
functions=''.join(function(s) for s in ['bool CreatureRenderer::loadSkin(', 'bool CreatureRenderer::loadTexture(', 'bool CreatureRenderer::ensure('])
cpp=r'''
#include "rendering/psp/psp_model_format.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
using namespace wowee::rendering::psp;
#define PROF_SCOPE(...) ((void)0)
#define PSP_LOGE(...) ((void)0)
#define PSP_LOGI(...) ((void)0)
#define PSP_LOGW(...) ((void)0)
struct SkinView {const SkinHeader* h=nullptr;const ModelBatch* batches=nullptr;
    bool init(void* blob,uint32_t size){if(size<sizeof(SkinHeader)+sizeof(ModelBatch))return false;h=static_cast<SkinHeader*>(blob);batches=reinterpret_cast<const ModelBatch*>(h+1);return true;}
};
namespace pp {
struct PakEntry {const char* name=nullptr;uint32_t offset=0,size=0,kind=0;};
struct Index {bool isOpen()const{return false;}bool lookupIoFailed()const{return false;}};
void pakFree(void* p){std::free(p);}uint32_t logUptimeMs(){return 0;}
bool textureFromBlob(void* b,uint32_t,void*& t,bool){t=b;return true;}
}
enum class Mode {Pending,ReadPending,Ready,Missing,Bad};
class CreatureRenderer {
public:
    struct InstallTimer {explicit InstallTimer(CreatureRenderer&) {}};
    struct Skin {void* blob=nullptr;SkinView view;uint32_t lod0Batches=0,bytes=0,lastUsed=0;bool failed=false;};
    struct Tex {void* t=nullptr;bool loaded=false,failed=false;uint32_t retryAtMs=0,bytes=0,lastUsed=0;};
    struct Model {void* blob=nullptr;const ModelHeader* h=nullptr;const ModelBatch* batches=nullptr;bool failed=false;};
    struct Display {uint32_t id=1,lastUsed=0;bool failed=false,loaded=true,texturesKnown=false;uint8_t skinMode=0;
        struct Header {uint16_t model=2,batchCount=1,slotTexture[8]{};}h;std::vector<uint8_t> active{1};std::vector<uint16_t> textures;};
    enum EntryKind {kEntrySkin,kEntryTexture};
    std::unordered_map<uint16_t,Skin> skins_;std::unordered_map<uint16_t,Tex> textures_;std::unordered_map<uint16_t,Model> models_;
    Mode skin=Mode::Pending,texture=Mode::Pending;bool entryPending_=false,animate_=true,asyncDrawing_=true;
    uint32_t frame_=7,heapHeldFrame_=999,memoryBytes_=0,skinLogs_=0;
    struct {unsigned loads=0;}stats_;pp::Index index_;
    EntryKind requested=kEntrySkin;
    std::vector<uint16_t> textureAsked;
    ModelHeader staticHeader{};ModelBatch staticBatch{};
    ~CreatureRenderer(){for(auto& [id,s]:skins_)std::free(s.blob);for(auto& [id,t]:textures_)std::free(t.t);}
    bool findEntry(EntryKind kind,uint16_t id,pp::PakEntry& e){
        if(kind==kEntryTexture)textureAsked.push_back(id);
        requested=kind;const auto mode=kind==kEntrySkin?skin:texture;entryPending_=mode==Mode::Pending;
        e.size=kind==kEntrySkin?sizeof(SkinHeader)+sizeof(ModelBatch):32;
        return mode!=Mode::Pending&&mode!=Mode::Missing;
    }
    void* loadEntry(const pp::PakEntry& e){
        const auto mode=requested==kEntrySkin?skin:texture;
        if(mode==Mode::ReadPending){entryPending_=true;return nullptr;}
        void* b=std::calloc(1,e.size);assert(b);
        if(requested==kEntrySkin){SkinHeader h{};h.batchCount=1;h.boneCount=2;h.subBatchCount=1;h.sequenceCount=mode==Mode::Bad?0:1;std::memcpy(b,&h,sizeof(h));
            auto* batch=reinterpret_cast<ModelBatch*>(static_cast<SkinHeader*>(b)+1);batch->texture=77;}
        return b;
    }
    bool roomFor(uint32_t){return true;}bool tocBudgetLeft()const{return true;}
    bool loadDisplay(Display&){return true;}
    bool loadModel(uint16_t id){staticHeader.batchCount[0]=1;staticBatch.texture=kNoTexture;models_[id]={reinterpret_cast<void*>(1),&staticHeader,&staticBatch,false};return true;}
    Skin* skinOf(uint16_t id){auto i=skins_.find(id);return i==skins_.end()?nullptr:&i->second;}
    bool loadSkin(uint16_t);bool loadTexture(uint16_t);bool ensure(Display&,uint32_t&);
};
'''
main=r'''
int main(){
    CreatureRenderer r;CreatureRenderer::Display d;uint32_t budget=8;
    assert(!r.ensure(d,budget));assert(d.skinMode==0&&!d.failed&&r.skins_.empty());
    r.skin=Mode::ReadPending;budget=8;assert(!r.ensure(d,budget));assert(d.skinMode==0&&!r.skins_[2].failed);
    r.skin=Mode::Ready;budget=8;assert(!r.ensure(d,budget));
    assert(d.skinMode==1&&r.skins_[2].blob&&!r.textures_[77].failed);
    d.texturesKnown=true;d.textures={77,78};r.textureAsked.clear();budget=8;
    assert(!r.ensure(d,budget));assert(r.textureAsked.size()==2); // pipeline both texture requests
    r.texture=Mode::ReadPending;budget=8;assert(!r.ensure(d,budget));assert(!r.textures_[77].failed);
    r.texture=Mode::Ready;budget=8;assert(r.ensure(d,budget));assert(r.textures_[77].loaded);
    budget=0;assert(r.ensure(d,budget)); // resident NPCs do not depend on streaming budget
    CreatureRenderer missing;CreatureRenderer::Display old;missing.skin=Mode::Missing;budget=8;
    assert(missing.ensure(old,budget)&&old.skinMode==2&&missing.skins_[2].failed);
    CreatureRenderer bad;CreatureRenderer::Display malformed;bad.skin=Mode::Bad;budget=8;
    assert(bad.ensure(malformed,budget)&&malformed.skinMode==2&&bad.skins_[2].failed);
    std::puts("NPC installation passed: pending reads retain animated path, textures wait, resident NPCs draw with zero budget, true missing/bad data retain fallback.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-npc-install-') as temp:
    folder=Path(temp);src=folder/'install.cpp';exe=folder/'install.exe';src.write_text(cpp+functions+main)
    if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/I'+str(root/'include'),str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'install.obj')]
    else:args=[compiler,'-std=c++20','-O2','-I'+str(root/'include'),str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
