#pragma once
#include "rendering/psp/psp_collision_bounds_index.hpp"
#include <array>
#include <cmath>

namespace wowee::rendering::psp {
// Cache broad-phase candidates only, never heights or contact answers. Columns
// include every Z, letting floors and walls share a set without dropping floors
// below the player. Fixed storage: overflow falls back to the full stable scan.
class CollisionCandidates {
public:
    struct Set {
        std::array<uint32_t,128> ids{};
        size_t count=0;
        float x0=0,y0=0,x1=0,y1=0;
        uint32_t generation=0;
        bool valid=false,overflow=false;
    };
    const Set* get(const CollisionBoundsIndex& index,uint32_t generation,
                   float x0,float y0,float x1,float y1) const {
        if (!std::isfinite(x0)||!std::isfinite(y0)||!std::isfinite(x1)||!std::isfinite(y1)) return nullptr;
        for (const auto& s:sets_)
            if (s.valid && s.generation==generation && x0>=s.x0 && y0>=s.y0 && x1<=s.x1 && y1<=s.y1) {
                ++hits;return s.overflow?nullptr:&s;
            }
        Set& s=sets_[next_++%sets_.size()];s.count=0;s.overflow=false;
        s.generation=generation;s.valid=true;
        s.x0=x0-4.f;s.y0=y0-4.f;s.x1=x1+4.f;s.y1=y1+4.f;
        index.query({s.x0,s.y0,-1e30f},{s.x1,s.y1,1e30f},[&](const CollisionBoundsIndex::Entry& e){
            if(s.count<s.ids.size())s.ids[s.count++]=e.id;else s.overflow=true;
        });
        std::sort(s.ids.begin(),s.ids.begin()+s.count);++builds;
        return s.overflow?nullptr:&s;
    }
    mutable uint32_t hits=0,builds=0;
private:
    mutable std::array<Set,2> sets_{};
    mutable size_t next_=0;
};
}
