#pragma once
#include <array>
#include <cstdint>

namespace wowee::platform::psp {
// Send one pickup at a time. A full bag stops the batch, rather than flooding
// the UI with one inventory error for every remaining item.
class LootRequests {
public:
    void reset() { *this=LootRequests{}; }
    void enqueue(uint8_t slot) {
        if(slot>=64 || count_>=64 || pending_==slot)return;
        for(unsigned i=0;i<count_;++i)if(queue_[(head_+i)%64]==slot)return;
        queue_[(head_+count_++)%64]=slot;
    }
    bool request(uint8_t slot,uint32_t now) {
        if(slot>=64 || pending_>=0 || (retryAt_ && static_cast<int32_t>(now-retryAt_)<0))return false;
        pending_=slot;sentAt_=now;return true;
    }
    void complete(uint8_t slot) { if(pending_==slot)pending_=-1; }
    void fail(uint32_t now) { pending_=-1;count_=head_=0;retryAt_=now+500;manual_=true; }
    int next(uint32_t now) {
        if(pending_>=0 && now-sentAt_>=3000)fail(now);
        if(pending_>=0 || !count_ || (retryAt_ && static_cast<int32_t>(now-retryAt_)<0))return -1;
        const int slot=queue_[head_];head_=(head_+1)%64;--count_;return slot;
    }
    bool pending()const{return pending_>=0;}
    bool manual()const{return manual_;}
private:
    std::array<uint8_t,64> queue_{};
    unsigned head_=0,count_=0;
    int pending_=-1;
    uint32_t retryAt_=0,sentAt_=0;
    bool manual_=false;
};
}
