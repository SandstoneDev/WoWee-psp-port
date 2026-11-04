#include "platform/psp/psp_npc_asset_loader.hpp"
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <malloc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace wowee::platform::psp {
namespace {std::atomic<uint32_t> g_reservedBytes{0};}
bool NpcAssetLoader::start(const char* path,const Pak* legacy) {
    stop();
    if(!path || std::strlen(path)>=sizeof(path_)) return false;
    std::snprintf(path_,sizeof(path_),"%s",path);
    if(index_.open(path)) index_.releaseFd(); // keep lookup data, not a pinned file
    else if(legacy && legacy->isOpen()) legacy_=legacy;
    else {stop();return false;}
    sema_=sceKernelCreateSema("wowee_npc_io",0,0,4,nullptr);
    if(sema_<0) {stop();return false;}
    quit_.store(false,std::memory_order_release);
    // PSP priorities: lower number runs first. Music decoder=24, effects loader=49.
    // Cosmetic NPC reads must not starve either audio producer.
    thread_=sceKernelCreateThread("wowee_npc_io",threadMain,50,16*1024,PSP_THREAD_ATTR_USER,nullptr);
    if(thread_<0) {stop();return false;}
    NpcAssetLoader* self=this;
    if(sceKernelStartThread(thread_,sizeof(self),&self)<0) {
        sceKernelDeleteThread(thread_);thread_=-1;stop();return false;
    }
    return true;
}
void NpcAssetLoader::discard(Slot& s) {
    if(s.state.load(std::memory_order_acquire)==Queued) g_reservedBytes.fetch_sub(s.allowance,std::memory_order_acq_rel);
    if(s.blob) std::free(s.blob);
    s.blob=nullptr;s.result={};s.retryAt=0;
    s.state.store(Empty,std::memory_order_release);
}
void NpcAssetLoader::stop() {
    if(thread_>=0) {
        quit_.store(true,std::memory_order_release);
        sceKernelSignalSema(sema_,1);
        // One in-flight read completes. Never close its FD or kill a thread
        // inside the allocator, which could strand a lock during Vita exit.
        sceKernelWaitThreadEnd(thread_,nullptr);
        sceKernelDeleteThread(thread_);thread_=-1;
    }
    if(sema_>=0) {sceKernelDeleteSema(sema_);sema_=-1;}
    for(auto& s:slots_) discard(s);
    index_.close();legacy_=nullptr;path_[0]=0;
}
void NpcAssetLoader::cancel() {
    generation_.fetch_add(1,std::memory_order_acq_rel);
    for(auto& s:slots_) {
        uint32_t state=s.state.load(std::memory_order_acquire);
        if(state==Queued) {
            if(s.state.compare_exchange_strong(state,Empty,std::memory_order_acq_rel)) {
                g_reservedBytes.fetch_sub(s.allowance,std::memory_order_acq_rel);continue;
            }
        }
        if(state!=Running && state!=Queued && state!=Empty) discard(s);
    }
}
void NpcAssetLoader::trim(uint32_t now) {
    const uint32_t generation=generation_.load(std::memory_order_acquire);
    for(auto& s:slots_) {
        const uint32_t state=s.state.load(std::memory_order_acquire);
        if(state==Empty || state==Queued || state==Running) continue;
        if(s.generation!=generation || static_cast<uint32_t>(now-s.touched)>2000u) discard(s);
    }
}
NpcAssetLoader::Status NpcAssetLoader::request(uint32_t key,const char* name,uint32_t allowance,Result& out,uint32_t now) {
    out={};
    const uint32_t generation=generation_.load(std::memory_order_acquire);
    for(auto& s:slots_) {
        uint32_t state=s.state.load(std::memory_order_acquire);
        if(state==Empty || s.key!=key || s.generation!=generation) continue;
        s.touched=now;
        if(state==Complete) {out=s.result;return Status::Ready;}
        if(state==Absent) {discard(s);return Status::Missing;}
        if(state==Failed) {
            out=s.result;
            if(!s.retryAt) s.retryAt=now+250u;
            if(static_cast<int32_t>(now-s.retryAt)<0) return Status::Retry;
            discard(s);break;
        }
        return Status::Pending;
    }
    if(thread_<0 || !allowance) return Status::Pending;
    for(auto& s:slots_) {
        if(s.state.load(std::memory_order_acquire)!=Empty) continue;
        s.key=key;s.generation=generation;s.allowance=allowance;
        s.result={};s.blob=nullptr;s.touched=now;s.retryAt=0;
        std::snprintf(s.name,sizeof(s.name),"%s",name);
        g_reservedBytes.fetch_add(allowance,std::memory_order_acq_rel);
        s.state.store(Queued,std::memory_order_release);
        sceKernelSignalSema(sema_,1);
        break;
    }
    return Status::Pending;
}
bool NpcAssetLoader::ready(uint32_t key) const {
    for(const auto& s:slots_)
        if(s.state.load(std::memory_order_acquire)==Complete && s.key==key && s.generation==generation_.load(std::memory_order_acquire)) return true;
    return false;
}
bool NpcAssetLoader::mayQueue(uint32_t key) const {
    bool empty=false;
    const uint32_t generation=generation_.load(std::memory_order_acquire);
    for(const auto& s:slots_) {
        const uint32_t state=s.state.load(std::memory_order_acquire);
        if(state==Empty) empty=true;
        else if(s.key==key && s.generation==generation) return false;
    }
    return empty;
}
void* NpcAssetLoader::take(uint32_t key) {
    for(auto& s:slots_) {
        if(s.state.load(std::memory_order_acquire)!=Complete || s.key!=key || s.generation!=generation_.load(std::memory_order_acquire)) continue;
        void* blob=s.blob;s.blob=nullptr;s.state.store(Empty,std::memory_order_release);return blob;
    }
    return nullptr;
}
uint32_t NpcAssetLoader::reservedBytes() const {
    return g_reservedBytes.load(std::memory_order_acquire);
}
int NpcAssetLoader::threadMain(unsigned int,void* arg) {(*static_cast<NpcAssetLoader**>(arg))->run();return 0;}
void NpcAssetLoader::run() {
    uint32_t next=0;
    int fd=-1;
    auto closeFile=[&] {if(fd>=0){pakNoteClose(fd);sceIoClose(fd);fd=-1;}};
    for(;;) {
        sceKernelWaitSema(sema_,1,nullptr);
        if(quit_.load(std::memory_order_acquire)) {closeFile();return;}
        Slot* job=nullptr;
        for(uint32_t i=0;i<4;++i) {
            Slot& s=slots_[(next+i)%4];uint32_t expected=Queued;
            if(s.state.compare_exchange_strong(expected,Running,std::memory_order_acq_rel)) {
                job=&s;next=(next+i+1u)%4u;break;
            }
        }
        if(!job) {closeFile();continue;}
        PakEntry entry{};
        bool found=false,ioFailed=false;
        if(fd<0) {
            fd=sceIoOpen(path_,PSP_O_RDONLY,0);
            if(fd>=0) pakNoteOpen(fd,path_);
            else {if(static_cast<unsigned>(fd)==0x80020320u) pakFdRelieve(1);fd=-1;}
        }
        if(fd<0) ioFailed=true; // descriptor pressure is transient, not absent data
        else if(index_.isOpen()) {found=index_.findOnFd(job->name,entry,fd);ioFailed=index_.lookupIoFailed();}
        else if(const PakEntry* e=legacy_?legacy_->find(job->name):nullptr) {entry=*e;found=true;}
        State result=found ? Failed : ioFailed ? Failed : Absent;
        if(found) {
            job->result={entry.offset,entry.size,entry.kind};
            if(!entry.size) result=Absent; // empty placeholders are not transient read failures
            if(entry.size && entry.size<=job->allowance && entry.size<=0x7fffffc0u) {
                const uint32_t bytes=(entry.size+63u)&~63u;
                job->blob=memalign(64,bytes);
                if(job->blob && pakRead(fd,entry.offset,job->blob,entry.size)) {
                    sceKernelDcacheWritebackRange(job->blob,bytes);
                    reads_.fetch_add(1,std::memory_order_relaxed);result=Complete;
                }
            }
        }
        if(result==Failed) failures_.fetch_add(1,std::memory_order_relaxed);
        if(result!=Complete && job->blob) {std::free(job->blob);job->blob=nullptr;}
        bool queued=false;
        for(const auto& s:slots_) if(s.state.load(std::memory_order_acquire)==Queued) {queued=true;break;}
        if(!queued) closeFile(); // return handles for music/SFX whenever this batch drains
        g_reservedBytes.fetch_sub(job->allowance,std::memory_order_acq_rel);
        if(job->generation!=generation_.load(std::memory_order_acquire) || quit_.load(std::memory_order_acquire)) {
            if(job->blob) std::free(job->blob);
            job->blob=nullptr;job->state.store(Empty,std::memory_order_release);
        } else job->state.store(result,std::memory_order_release);
    }
}
}
