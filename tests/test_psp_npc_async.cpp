#ifdef NDEBUG
#undef NDEBUG
#endif
#include "platform/psp/psp_npc_asset_loader.hpp"
#include "pspkernel.h"
#include "pspiofilemgr.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace wowee::platform::psp;
namespace {
std::thread::id mainId=std::this_thread::get_id();
std::mutex mutex,allocationMutex,kernelMutex;
std::condition_variable readCv;
bool blocked=false,failRead=false,failLookup=false,legacyMode=false,failThread=false,failOpen=false;
unsigned activeReads=0,bulkReads=0,lookups=0;
std::atomic<unsigned> nextId{10};
std::set<int> readingFds;
std::set<void*> allocations;
std::set<int> descriptors;
struct Semaphore {std::mutex mutex;std::condition_variable cv;int count=0;};
struct Thread {int(*entry)(unsigned int,void*);std::thread thread;};
std::map<int,std::shared_ptr<Semaphore>> semaphores;
std::map<int,std::shared_ptr<Thread>> threads;
std::map<std::string,PakEntry> entries={{"d_1",{"d_1",64,64,3}},{"n_2",{"n_2",128,4096,3}},{"t_3",{"t_3",8192,1024,1}},{"empty",{"empty",9216,0,3}}};
template<class F> void eventually(F condition) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!condition()) {assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
void gate(bool value){std::lock_guard lock(mutex);blocked=value;if(!value)readCv.notify_all();}
NpcAssetLoader::Status poll(NpcAssetLoader& loader,uint32_t key,const char* name,uint32_t allowance,NpcAssetLoader::Result& result,uint32_t now=1000) {
    NpcAssetLoader::Status status;
    eventually([&]{status=loader.request(key,name,allowance,result,now);return status!=NpcAssetLoader::Status::Pending;});
    return status;
}
}
namespace wowee::platform::psp {
int pakOpenRetry(const char*) {assert(std::this_thread::get_id()==mainId);std::lock_guard lock(mutex);int fd=++nextId;descriptors.insert(fd);return fd;}
void pakNoteOpen(int,const char*){}
void pakNoteClose(int){}
bool PakIndex::open(const char* path){assert(std::this_thread::get_id()==mainId);if(legacyMode)return false;fd_=pakOpenRetry(path);open_=true;return true;}
void PakIndex::releaseFd(){if(fd_>=0){sceIoClose(fd_);fd_=-1;}}
void PakIndex::close(){releaseFd();open_=false;}
int pakFdRelieve(int){return 0;}
bool PakIndex::findOnFd(const char* name,PakEntry& out,int fd){assert(fd>=0);return find(name,out);}
bool PakIndex::find(const char* name,PakEntry& result){
    assert(std::this_thread::get_id()!=mainId);std::lock_guard lock(mutex);++lookups;
    ioFailed_=failLookup;if(failLookup)return false;
    auto it=entries.find(name);if(it==entries.end())return false;result=it->second;return true;
}
const PakEntry* Pak::find(const char* name)const {
    assert(std::this_thread::get_id()!=mainId);std::lock_guard lock(mutex);++lookups;
    auto it=entries.find(name);return it==entries.end()?nullptr:&it->second;
}
bool pakRead(int fd,uint32_t offset,void* dst,uint32_t size){
    assert(std::this_thread::get_id()!=mainId);std::unique_lock lock(mutex);
    assert(descriptors.count(fd));assert(readingFds.insert(fd).second);++activeReads;++bulkReads;readCv.notify_all();
    readCv.wait(lock,[]{return !blocked;});assert(descriptors.count(fd));
    const bool ok=!failRead;if(ok)std::memset(dst,static_cast<int>(offset&255),size);
    --activeReads;readingFds.erase(fd);readCv.notify_all();return ok;
}
}
int sceIoOpen(const char*,int,int){assert(std::this_thread::get_id()!=mainId);std::lock_guard lock(mutex);if(failOpen)return static_cast<int>(0x80020320u);int fd=++nextId;descriptors.insert(fd);return fd;}
int sceIoClose(int fd){std::lock_guard lock(mutex);assert(!readingFds.count(fd));assert(descriptors.erase(fd)==1);return 0;}
int sceKernelCreateSema(const char*,int,int,int,void*){std::lock_guard lock(kernelMutex);int id=++nextId;semaphores[id]=std::make_shared<Semaphore>();return id;}
int sceKernelSignalSema(int id,int n){std::shared_ptr<Semaphore> s;{std::lock_guard lock(kernelMutex);s=semaphores.at(id);}{std::lock_guard lock(s->mutex);s->count+=n;}s->cv.notify_one();return 0;}
int sceKernelWaitSema(int id,int n,void*){std::shared_ptr<Semaphore> s;{std::lock_guard lock(kernelMutex);s=semaphores.at(id);}std::unique_lock lock(s->mutex);s->cv.wait(lock,[&]{return s->count>=n;});s->count-=n;return 0;}
int sceKernelDeleteSema(int id){std::lock_guard lock(kernelMutex);semaphores.erase(id);return 0;}
int sceKernelCreateThread(const char*,int(*entry)(unsigned int,void*),int priority,unsigned int,int,void*){assert(priority>49);if(failThread)return -1;int id=++nextId;auto t=std::make_shared<Thread>();t->entry=entry;threads[id]=t;return id;}
int sceKernelStartThread(int id,unsigned int n,void* args){
    auto t=threads.at(id);std::vector<uint8_t> copy(static_cast<uint8_t*>(args),static_cast<uint8_t*>(args)+n);
    t->thread=std::thread([t,n,copy=std::move(copy)]()mutable{t->entry(n,copy.data());});return 0;
}
int sceKernelWaitThreadEnd(int id,SceUInt*){auto t=threads.at(id);if(t->thread.joinable())t->thread.join();return 0;}
int sceKernelDeleteThread(int id){threads.erase(id);return 0;}
void sceKernelDcacheWritebackRange(void*,unsigned int){}
void* npcTestAlloc(std::size_t,std::size_t bytes){void* p=std::malloc(bytes);assert(p);std::lock_guard lock(allocationMutex);assert(allocations.insert(p).second);return p;}
void npcTestFree(void* p){if(!p)return;{std::lock_guard lock(allocationMutex);assert(allocations.erase(p)==1);}std::free(p);}
int main(){
    NpcAssetLoader loader;assert(loader.start("creatures.pak"));NpcAssetLoader::Result result;
    assert(descriptors.empty()); // idle workers leave the audio file budget intact
    gate(true);
    assert(loader.request(1,"d_1",4096,result,1000)==NpcAssetLoader::Status::Pending);
    eventually([&]{std::lock_guard lock(mutex);return activeReads==1;});
    assert(!loader.mayQueue(1)&&loader.mayQueue(2));
    // Rendering-side request/poll performs neither metadata nor data I/O.
    const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<10000;++i)assert(loader.request(1,"d_1",0,result,1000)==NpcAssetLoader::Status::Pending);
    assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100));
    assert(loader.reservedBytes()==4096);
    gate(false);assert(poll(loader,1,"d_1",4096,result)==NpcAssetLoader::Status::Ready);
    assert(result.size==64&&result.offset==64&&loader.ready(1));
    auto* bytes=static_cast<uint8_t*>(loader.take(1));assert(bytes&&bytes[0]==64&&!loader.take(1));npcTestFree(bytes);
    {std::lock_guard lock(mutex);assert(descriptors.empty());}
    failOpen=true;assert(poll(loader,77,"d_1",4096,result,1000)==NpcAssetLoader::Status::Retry);
    failOpen=false;assert(poll(loader,77,"d_1",4096,result,1300)==NpcAssetLoader::Status::Ready);npcTestFree(loader.take(77));
    assert(poll(loader,9,"missing",1024,result)==NpcAssetLoader::Status::Missing);
    assert(poll(loader,99,"empty",1024,result)==NpcAssetLoader::Status::Missing);
    failRead=true;assert(poll(loader,2,"n_2",8192,result)==NpcAssetLoader::Status::Retry);assert(!loader.ready(2));
    failRead=false;assert(poll(loader,2,"n_2",8192,result,1300)==NpcAssetLoader::Status::Ready);npcTestFree(loader.take(2));
    // Budget failure is retryable, never a missing-model answer.
    assert(poll(loader,3,"t_3",128,result,1500)==NpcAssetLoader::Status::Retry);
    assert(result.size==1024);
    assert(poll(loader,3,"t_3",2048,result,1800)==NpcAssetLoader::Status::Ready);npcTestFree(loader.take(3));
    failLookup=true;assert(poll(loader,8,"d_1",4096,result,2000)==NpcAssetLoader::Status::Retry);
    failLookup=false;assert(poll(loader,8,"d_1",4096,result,2300)==NpcAssetLoader::Status::Ready);npcTestFree(loader.take(8));
    // Cancel a running read and a queued read. Neither belongs to the new generation.
    gate(true);loader.request(10,"n_2",8192,result,2500);
    eventually([&]{std::lock_guard lock(mutex);return activeReads==1;});
    loader.request(11,"t_3",2048,result,2500);loader.cancel();gate(false);
    eventually([&]{return loader.reservedBytes()==0;});loader.trim(2501);
    assert(!loader.take(10)&&!loader.take(11));
    assert(poll(loader,10,"d_1",4096,result,2600)==NpcAssetLoader::Status::Ready);npcTestFree(loader.take(10));
    assert(poll(loader,12,"t_3",2048,result,2700)==NpcAssetLoader::Status::Ready);
    loader.trim(5001);assert(!loader.take(12));
    // Two workers share reservations rather than both spending the same headroom.
    NpcAssetLoader equipment;assert(equipment.start("items.pak"));gate(true);
    loader.request(20,"n_2",8192,result,6000);equipment.request(21,"t_3",2048,result,6000);
    eventually([&]{std::lock_guard lock(mutex);return activeReads==2;});assert(loader.reservedBytes()==10240);
    gate(false);assert(poll(loader,20,"n_2",8192,result,6000)==NpcAssetLoader::Status::Ready);npcTestFree(loader.take(20));
    assert(poll(equipment,21,"t_3",2048,result,6000)==NpcAssetLoader::Status::Ready);npcTestFree(equipment.take(21));equipment.stop();
    // Shutdown joins the reader before freeing its buffer or closing its descriptor.
    gate(true);loader.request(30,"n_2",8192,result,7000);eventually([&]{std::lock_guard lock(mutex);return activeReads==1;});
    std::thread release([]{std::this_thread::sleep_for(std::chrono::milliseconds(20));gate(false);});loader.stop();release.join();
    assert(loader.reservedBytes()==0&&threads.empty()&&semaphores.empty()&&descriptors.empty()&&allocations.empty());
    legacyMode=true;Pak legacy;assert(loader.start("old.pak",&legacy));
    assert(poll(loader,40,"t_3",2048,result,8000)==NpcAssetLoader::Status::Ready);npcTestFree(loader.take(40));loader.stop();legacyMode=false;
    failThread=true;assert(!loader.start("creatures.pak"));loader.stop();
    assert(loader.reservedBytes()==0&&descriptors.empty()&&allocations.empty());
    std::printf("NPC async worker passed: background-only I/O, nonblocking requests, retries, budgets, cancellation, shared reservations, legacy pak, joined shutdown.\n");
}
