"""Exercise production achievement load/evict/reopen and failed-read retry semantics."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/platform/psp/psp_world_session_achievements.cpp').read_text()
def method(sig):
 a=source.index(sig);b=source.index('{',a)+1;depth=1
 while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]+'\n'
cpp=r'''
#include "platform/psp/psp_wotlk_db.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#define PSP_LOGI(...) ((void)0)
#define PSP_LOGW(...) ((void)0)
namespace wowee::platform::psp {
class Pak{public:void close(){}};
unsigned reads=0,freed=0;bool valid=true,available=true;
bool wotlkPakOpen(Pak&){return available;}
void pakFree(void* p){++freed;std::free(p);}
void* wotlkPakRaw(Pak&,const char*,uint32_t& size){
 ++reads;size=sizeof(AchPakHeader)+sizeof(AchCatRec)+sizeof(AchIdxRec)+1;
 auto* data=static_cast<unsigned char*>(std::calloc(1,size));auto* h=reinterpret_cast<AchPakHeader*>(data);
 std::memcpy(h->magic,"WACH",4);h->version=valid?1:99;h->categoryCount=1;h->achievementCount=1;
 h->catOff=sizeof(*h);h->achOff=h->catOff+sizeof(AchCatRec);h->strOff=h->achOff+sizeof(AchIdxRec);h->strSize=1;
 auto* r=reinterpret_cast<AchIdxRec*>(data+h->achOff);r->id=6;r->category=92;r->points=10;
 return data;
}
METHODS
}
using namespace wowee::platform::psp;
int main(){
 AchIndex index;
 for(int open=0;open<20;++open){
  if(!index.loaded()&&!index.tried())index.load(); // real frame guard
  assert(index.loaded()&&index.tried()&&index.count()==1&&index.at(0).id==6);
  index.release();assert(!index.loaded()&&!index.tried());
 }
 assert(reads==20&&freed==20);
 valid=false;assert(!index.load()&&index.tried());unsigned attempts=reads;
 for(int frame=0;frame<300;++frame)if(!index.loaded()&&!index.tried())index.load();
 assert(reads==attempts); // no repeated failed reads each frame
 index.release();valid=true;assert(index.load());index.release();
 available=false;assert(!index.load()&&index.tried());index.release();available=true;assert(index.load());index.release();
 assert(freed==reads);
 std::puts("Achievement reload passed: 20 open/close cycles, invalid/missing data and explicit retry, no repeated failure I/O.");
}
'''.replace('METHODS',method('bool AchIndex::load()')+method('void AchIndex::release()'))
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-ach-') as temp:
 folder=Path(temp);src=folder/'ach.cpp';exe=folder/'ach.exe';src.write_text(cpp)
 if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/I'+str(root/'include'),str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'ach.obj')]
 else:args=[compiler,'-std=c++20','-I'+str(root/'include'),str(src),'-o',str(exe)]
 subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
