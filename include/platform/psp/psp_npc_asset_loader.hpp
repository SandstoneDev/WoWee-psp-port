#pragma once
#include "platform/psp/psp_pak_index.hpp"
#include <atomic>
#include <cstdint>
namespace wowee::platform::psp {
// One file, one worker-owned lookup index and four bounded result slots. Main
// thread owns requests/take/cancel/stop; the worker only reads and publishes.
class NpcAssetLoader {
public:
    enum class Status { Pending, Ready, Missing, Retry };
    struct Result {uint32_t offset=0,size=0,kind=0;};
    ~NpcAssetLoader(){stop();}
    bool start(const char* path,const Pak* legacy=nullptr);
    void stop(); // joins before descriptors, slots or caller-owned legacy TOC go away
    void cancel(); // invalidates old character/map results without blocking a frame
    void trim(uint32_t nowMs);
    Status request(uint32_t key,const char* name,uint32_t allowance,Result& result,uint32_t nowMs);
    void* take(uint32_t key);
    bool ready(uint32_t key) const;
    bool mayQueue(uint32_t key) const;
    uint32_t reservedBytes() const;
    uint32_t reads() const{return reads_.load(std::memory_order_relaxed);}
    uint32_t failures() const{return failures_.load(std::memory_order_relaxed);}
private:
    enum State:uint32_t {Empty,Queued,Running,Complete,Absent,Failed};
    struct Slot {
        std::atomic<uint32_t> state{Empty};
        uint32_t key=0,generation=0,allowance=0,touched=0,retryAt=0;
        char name[32]{};
        Result result;
        void* blob=nullptr;
    };
    Slot slots_[4];
    PakIndex index_; // opened/closed on main; lookup cache used exclusively by worker
    const Pak* legacy_=nullptr; // read-only legacy TOC; stop precedes its destruction
    char path_[128]{};
    int thread_=-1,sema_=-1;
    std::atomic<bool> quit_{false};
    std::atomic<uint32_t> generation_{1},reads_{0},failures_{0};
    static int threadMain(unsigned int,void*);
    void run();
    void discard(Slot& slot);
};
#if !defined(WOWEE_PLATFORM_PSP) && !defined(WOWEE_TEST_NPC_ASYNC)
// Host converter checks have no kernel worker; retain their synchronous bank.
inline bool NpcAssetLoader::start(const char*,const Pak*) {return false;}
inline void NpcAssetLoader::stop() {}
inline void NpcAssetLoader::cancel() {}
inline void NpcAssetLoader::trim(uint32_t) {}
inline NpcAssetLoader::Status NpcAssetLoader::request(uint32_t,const char*,uint32_t,Result&,uint32_t) {return Status::Pending;}
inline void* NpcAssetLoader::take(uint32_t) {return nullptr;}
inline bool NpcAssetLoader::ready(uint32_t)const{return false;}
inline bool NpcAssetLoader::mayQueue(uint32_t)const{return false;}
inline uint32_t NpcAssetLoader::reservedBytes()const{return 0;}
#endif
}
