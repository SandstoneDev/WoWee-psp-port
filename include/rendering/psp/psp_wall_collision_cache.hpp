#pragma once
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_collision_math.hpp"
#include <array>
#include <cstddef>

namespace wowee::rendering::psp {
// Prepared geometry only: never cache a contact or the player's resolved position.
// Keep cell/list order, including duplicate references, for identical corner pushes.
class WallCollisionCache {
public:
    struct Triangle { glm::vec3 a,b,c,n,lo,hi; };
    struct Set {
        std::array<Triangle,128> triangles{};
        size_t count=0;
        const CollisionHeader* header=nullptr;
        uint32_t generation=0,instance=0;
        int x0=0,x1=0,y0=0,y1=0;
        glm::mat3 axes{1.f}; glm::vec3 pos{0.f};
        bool valid=false,overflow=false;
        uint32_t lastUse=0;
    };
    const Set* get(const CollisionHeader* h,uint32_t generation,uint32_t instance,
                   const glm::mat3& axes,const glm::vec3& pos,glm::vec2 lo,glm::vec2 hi) {
        if (!h || !std::isfinite(lo.x)||!std::isfinite(lo.y)||!std::isfinite(hi.x)||!std::isfinite(hi.y))return nullptr;
        // Match the original forCells range exactly, including edge rounding.
        const int x0=std::max(0,static_cast<int>((lo.x-h->bmin[0])/h->cell));
        const int x1=std::min(h->nx-1,static_cast<int>((hi.x-h->bmin[0])/h->cell));
        const int y0=std::max(0,static_cast<int>((lo.y-h->bmin[1])/h->cell));
        const int y1=std::min(h->ny-1,static_cast<int>((hi.y-h->bmin[1])/h->cell));
        ++tick_;
        for(auto& s:sets_)if(s.valid && s.header==h && s.generation==generation && s.instance==instance &&
            s.x0==x0 && s.x1==x1 && s.y0==y0 && s.y1==y1 && s.axes==axes && s.pos==pos) {
            s.lastUse=tick_;++hits;return s.overflow?nullptr:&s;
        }
        unsigned reason=0;
        for(const auto& old:sets_)if(old.valid && old.header==h && old.instance==instance) {
            const unsigned candidate=old.generation!=generation?1u:(old.axes!=axes || old.pos!=pos)?2u:3u;
            reason=std::max(reason,candidate);
        }
        if(reason==1)++generationMisses;else if(reason==2)++transformMisses;else if(reason==3)++cellMisses;
        Set* victim=&sets_[0];
        for(auto& old:sets_) {
            if(!old.valid){victim=&old;break;}
            if(static_cast<uint32_t>(tick_-old.lastUse)>static_cast<uint32_t>(tick_-victim->lastUse))victim=&old;
        }
        if(!reason){if(victim->valid)++capacityMisses;else ++coldMisses;}
        if(victim->valid)++evictions;
        auto& s=*victim;s.count=0;s.overflow=false;s.lastUse=tick_;
        s.header=h;s.generation=generation;s.instance=instance;s.axes=axes;s.pos=pos;
        s.x0=x0;s.x1=x1;s.y0=y0;s.y1=y1;s.valid=true;++builds;
        const auto* bytes=reinterpret_cast<const uint8_t*>(h);
        const auto* vertices=reinterpret_cast<const float*>(bytes+h->vertexOffset);
        const auto* triangles=reinterpret_cast<const CollisionTri*>(bytes+h->triangleOffset);
        const auto* cells=reinterpret_cast<const uint32_t*>(bytes+h->cellOffset);
        const auto* list=reinterpret_cast<const uint32_t*>(bytes+h->listOffset);
        auto vertex=[&](uint16_t i){return axes*glm::vec3(vertices[i*3],vertices[i*3+1],vertices[i*3+2])+pos;};
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
            const auto* cell=cells+(static_cast<uint32_t>(y)*h->nx+x)*2u;
            for(uint32_t i=0;i<cell[1];++i) {
                const auto& t=triangles[list[cell[0]+i]];
                if(!(t.flags&kColBlocksWalls))continue;
                Triangle v;v.a=vertex(t.a);v.b=vertex(t.b);v.c=vertex(t.c);++prepared;
                v.n=glm::cross(v.b-v.a,v.c-v.a);
                if(glm::dot(v.n,v.n)<1e-18f || collisionWalkable(v.n))continue;
                if(s.count==s.triangles.size()){s.overflow=true;++overflows;return nullptr;}
                v.lo=glm::min(v.a,glm::min(v.b,v.c));v.hi=glm::max(v.a,glm::max(v.b,v.c));
                s.triangles[s.count++]=v;
            }
        }
        return &s;
    }
    uint32_t hits=0,builds=0,prepared=0,overflows=0;
    uint32_t generationMisses=0,transformMisses=0,cellMisses=0,capacityMisses=0,coldMisses=0,evictions=0;
private:
    std::array<Set,2> sets_{};
    uint32_t tick_=0;
};
static_assert(sizeof(WallCollisionCache)<20*1024,"Wall geometry cache must stay small on PSP");
}
