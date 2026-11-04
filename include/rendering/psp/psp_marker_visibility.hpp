#pragma once
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
namespace wowee::rendering::psp {
// Fixed storage, no per-frame allocations. Invalidate on camera/marker movement
// as well as a short expiry so streamed walls cannot leave stale visibility.
class MarkerVisibilityCache {
    struct Entry {uint64_t id=0;glm::vec3 camera{},point{};uint32_t checked=0,used=0;bool valid=false,visible=false;};
    std::array<Entry,32> entries_{}; // 12 NPC names + up to 16 quest markers, sharing matching rays
    uint32_t clock_=0;
public:
    template<class Occluded>
    bool visible(uint64_t id,const glm::vec3& camera,const glm::vec3& point,uint32_t now,Occluded&& occluded){
        Entry* slot=nullptr;
        for(auto& e:entries_)if(e.valid && e.id==id){slot=&e;break;}
        if(!slot){slot=&entries_[0];for(auto& e:entries_)if(!e.valid || e.used<slot->used){slot=&e;if(!e.valid)break;}}
        const auto dc=camera-slot->camera,dp=point-slot->point;
        if(!slot->valid || slot->id!=id || now-slot->checked>=100u || glm::dot(dc,dc)>.0625f || glm::dot(dp,dp)>.01f){
            const auto delta=point-camera;const float length=glm::length(delta);
            slot->visible=length<=.2f || !occluded(camera,delta/length,length-.15f);
            slot->valid=true;slot->id=id;slot->camera=camera;slot->point=point;slot->checked=now;
        }
        slot->used=++clock_;return slot->visible;
    }
};
}
