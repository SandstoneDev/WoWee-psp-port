#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace wowee::rendering::psp {
// Selection only: matrices, bone uploads, textures and draws remain live.
class NpcBatchPlans {
public:
    struct Pick {uint32_t index;uint8_t active;};
    template<class Build> const std::vector<Pick>& get(const void* geometry,uint32_t count,uint32_t split,
                                                       bool lod,bool hair,bool blend,Build&& build) {
        if(geometry_!=geometry || count_!=count || split_!=split) {
            geometry_=geometry;count_=count;split_=split;ready_=0;
        }
        const unsigned key=(lod?4u:0u)|(hair?2u:0u)|(blend?1u:0u);
        auto& plan=plans_[key];
        if(!(ready_&(1u<<key))) {plan.clear();build(plan);ready_|=1u<<key;++builds;}
        else ++hits;
        return plan;
    }
    uint32_t hits=0,builds=0;
private:
    const void* geometry_=nullptr;uint32_t count_=0,split_=0;unsigned ready_=0;
    std::array<std::vector<Pick>,8> plans_;
};
}
