"""Production ItemBank staging and cache-only commit guards, without a GPU."""
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/rendering/psp/psp_equipment.cpp').read_text()
def function(signature):
    start=source.index(signature);body=source.index('{',start);end=body+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'
functions=''.join(function(s) for s in ['uint32_t locationSlot(', 'uint32_t locationKey(',
    'void ItemBank::trimNpcWarm(', 'bool ItemBank::prepareNpcEntry(', 'const void* ItemBank::peekNpcEntry(',
    'void* ItemBank::takeNpcWarm(', 'void* ItemBank::loadNamed(', 'void* ItemBank::loadEntry(',
    'bool ItemBank::warmEquipmentLayer('])
cpp=r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <map>
unsigned ioCalls=0,allocs=0,frees=0;
void* allocate(unsigned n){++allocs;return std::malloc(n);}
namespace pp {
uint32_t clockMs=1000;uint32_t logUptimeMs(){return clockMs;}uint32_t heapFreeBytes(){return 8u<<20;}
void pakFree(void* p){if(p){++frees;std::free(p);}}
struct PakEntry {const char* name;uint32_t offset,size,kind;};
struct Index {bool isOpen(){return false;}bool find(const char*,PakEntry&){++ioCalls;return false;}void* load(PakEntry){++ioCalls;return nullptr;}};
struct Pak {bool isOpen(){return true;}const PakEntry* find(const char*){++ioCalls;return nullptr;}void* load(const PakEntry*){++ioCalls;return nullptr;}};
struct NpcAssetLoader {
    enum class Status {Pending,Ready,Missing,Retry};struct Result{uint32_t offset=0,size=0,kind=0;};
    Status status=Status::Pending;std::map<uint32_t,void*> buffers;
    ~NpcAssetLoader(){for(auto [key,p]:buffers)pakFree(p);}
    void trim(uint32_t){}uint32_t reservedBytes(){return 0;}
    bool mayQueue(uint32_t){return true;}
    Status request(uint32_t key,const char* name,uint32_t allowance,Result& r,uint32_t){
        if((key>>24)=='h')assert(std::strncmp(name,"hr_",3)==0);
        r={};if(status!=Status::Ready)return status;
        if(!buffers.count(key)&&allowance==0)return Status::Pending;
        if(!buffers.count(key)){assert(allowance>=16);buffers[key]=allocate(16);std::memset(buffers[key],42,16);}r.size=16;return status;
    }
    void* take(uint32_t key){auto p=buffers.at(key);buffers.erase(key);return p;}
};
}
class ItemBank {
public:
    static constexpr uint32_t kLocations=512;
    struct NpcWarm {uint32_t key=0,size=0,tick=0,lastMs=0;char name[32]{};void* blob=nullptr;};
    struct Location{uint32_t key=0,offset=0,size=0;};Location* locations_=nullptr;
    std::unique_ptr<pp::NpcAssetLoader> npcLoader_=std::make_unique<pp::NpcAssetLoader>();
    NpcWarm npcWarm_[32];uint32_t npcWarmTick_=0,locationHits_=0;bool npcNoIo_=false,npcDeferred_=false;
    pp::Index index_;pp::Pak pak_;std::map<uint32_t,int> npcRaceHelms_;
    ~ItemBank(){for(auto& w:npcWarm_)if(w.blob)pp::pakFree(w.blob);}
    bool locate(char,uint32_t){++ioCalls;return false;}
    void trimNpcWarm(uint32_t);bool prepareNpcEntry(char,uint32_t);
    const void* peekNpcEntry(char,uint32_t,uint32_t&)const;
    void* takeNpcWarm(const char*,uint32_t&,bool&);
    void* loadNamed(const char*,uint32_t&);void* loadEntry(char,uint32_t,uint32_t&);
    bool isOpen()const{return true;}bool npcStreaming()const{return npcLoader_!=nullptr;}
    bool equipmentLayerCached(uint16_t id)const{uint32_t n=0;return peekNpcEntry('q',id,n)!=nullptr;}
    bool warmEquipmentLayer(uint16_t);
    void* loadLayer(uint16_t,uint32_t&){++ioCalls;return nullptr;}
};
'''
main=r'''
int main(){
    {
        ItemBank b;uint32_t size=0;
        assert(!b.prepareNpcEntry('a',1));
        b.npcNoIo_=true;assert(!b.loadNamed("a_1",size)&&b.npcDeferred_&&ioCalls==0);b.npcNoIo_=b.npcDeferred_=false;
        b.npcLoader_->status=pp::NpcAssetLoader::Status::Missing;
        assert(b.prepareNpcEntry('a',1));b.npcNoIo_=true;
        assert(!b.loadNamed("a_1",size)&&!b.npcDeferred_&&ioCalls==0);b.npcNoIo_=false;
        b.npcLoader_->status=pp::NpcAssetLoader::Status::Ready;
        assert(b.prepareNpcEntry('a',2));assert(b.prepareNpcEntry('t',3));assert(b.prepareNpcEntry('h',4));
        assert(b.peekNpcEntry('a',2,size)&&size==16);
        b.npcNoIo_=true;void* model=b.loadNamed("a_2",size);assert(model&&size==16&&!b.npcDeferred_);pp::pakFree(model);
        void* texture=b.loadNamed("t_3",size);assert(texture&&!b.npcDeferred_);pp::pakFree(texture);
        void* record=b.loadEntry('h',4,size);assert(!record&&b.npcDeferred_); // hr_ is a named variant, not h_
        b.npcDeferred_=false;record=b.loadNamed("hr_4",size);assert(record&&!b.npcDeferred_);pp::pakFree(record);
        assert(ioCalls==0);
        // Remote-body layers stay in the existing staging slots until the
        // actual dress consumes them; preparation performs no second read/copy.
        assert(b.warmEquipmentLayer(77));assert(b.equipmentLayerCached(77));
        assert(b.warmEquipmentLayer(77));assert(ioCalls==0);
        void* layer=b.loadEntry('q',77,size);assert(layer&&size==16);pp::pakFree(layer);
        // An unexpected dependency during a staged replacement defers, never reads.
        assert(!b.loadNamed("a_999",size)&&b.npcDeferred_&&ioCalls==0);
        b.npcNoIo_=b.npcDeferred_=false;
        for(unsigned i=10;i<60;++i)assert(b.prepareNpcEntry('t',i)); // bounded LRU, correct ownership
        pp::clockMs=4001;b.trimNpcWarm(pp::clockMs);
        for(const auto& warm:b.npcWarm_)assert(warm.key==0);
        // Other callers keep their explicit synchronous behavior.
        assert(!b.loadNamed("outside_npc_scope",size)&&ioCalls>0);
    }
    assert(allocs==frees);
    std::puts("NPC equipment staging passed: pending vs absent, staged model/texture/helm reads, cache-only guards, LRU expiry and buffer ownership.");
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-npc-gear-') as temp:
    folder=Path(temp);src=folder/'gear.cpp';exe=folder/'gear.exe';src.write_text(cpp+functions+main)
    if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/DWOWEE_TEST_NPC_ASYNC=1',str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'gear.obj')]
    else:args=[compiler,'-std=c++20','-O2','-DWOWEE_TEST_NPC_ASYNC=1',str(src),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
