#pragma once
#include <glm/glm.hpp>
#include <cstdint>
namespace wowee::rendering::psp {
// Exact local coordinates only: never delay a room/portal crossing. The owner
// invalidates whenever collision used to identify rooms is installed or freed.
class WmoGroupCache {
public:
    void invalidate() { for(auto& e:entries_) e.valid=false; next_=0; }
    template<class F> int resolve(const glm::vec3& p,int mode,F&& compute) const {
        for(const auto& e:entries_)
            if(e.valid && e.mode==mode && e.point==p) return e.group;
        const int group=compute();
        entries_[next_]={p,mode,group,true};
        next_=(next_+1u)%4u;
        return group;
    }
private:
    struct Entry {glm::vec3 point{};int mode=0,group=-1;bool valid=false;};
    mutable Entry entries_[4];
    mutable uint32_t next_=0;
};
}
