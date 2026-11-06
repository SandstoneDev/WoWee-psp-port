#pragma once
#include "rendering/psp/psp_bigwmo_format.hpp"
#include "rendering/psp/psp_model_format.hpp"
#include <array>
#include <vector>
#include <algorithm>
#include <cstring>
namespace wowee::rendering::psp {
// Floating UVs preserve each source chunk's texture transform while positions
// and vertex colours stay byte-for-byte identical on the shared cell lattice.
struct WmoBatchVertex {float u,v;uint16_t color;int16_t x,y,z;};
static_assert(sizeof(WmoBatchVertex)==16);
class WmoBatchCache {
public:
    static constexpr uint32_t kMaxVertices=832, kSlots=4;
    struct Key {
        const void* blob;uint16_t batch;
        bool operator==(const Key& b)const{return blob==b.blob && batch==b.batch;}
    };
    struct Slot {std::vector<Key> keys;std::vector<WmoBatchVertex> vertices;std::vector<uint16_t> indices;uint32_t frame=0;};
    const Slot* find(const Key* keys,size_t count,uint32_t frame) {
        for(auto& s:slots_)if(s.keys.size()==count && std::equal(s.keys.begin(),s.keys.end(),keys)) {
            s.frame=frame;return &s;
        }
        return nullptr;
    }
    const Slot* build(const Key* keys,size_t count,uint32_t frame) {
        if(count<2 || count>32)return nullptr;
        size_t needed=0;
        for(size_t i=0;i<count;++i) {
            const auto* h=static_cast<const ChunkMeshHeader*>(keys[i].blob);
            const auto* b=reinterpret_cast<const ModelBatch*>(static_cast<const uint8_t*>(keys[i].blob)+h->batchOffset)+keys[i].batch;
            if(b->blend>=kBlendAlpha || b->indexCount<3 || h->vertexCount>4096)return nullptr;
            if(i)needed+=2+(needed&1u);
            needed+=b->indexCount;
            if(needed>kMaxVertices)return nullptr;
        }
        Slot* use=nullptr;
        for(auto& s:slots_)if(s.frame!=frame && (!use || s.frame<use->frame))use=&s;
        if(!use)return nullptr; // GE commands already emitted this frame retain their buffers
        use->keys.assign(keys,keys+count);
        use->vertices.clear();use->vertices.reserve(kMaxVertices);
        use->indices.clear();use->indices.reserve(kMaxVertices);
        use->keys.reserve(32);
        for(size_t i=0;i<count;++i) {
            const auto* raw=static_cast<const uint8_t*>(keys[i].blob);
            const auto* h=reinterpret_cast<const ChunkMeshHeader*>(raw);
            const auto& b=reinterpret_cast<const ModelBatch*>(raw+h->batchOffset)[keys[i].batch];
            const auto* vertices=reinterpret_cast<const ModelVertex*>(raw+h->vertexOffset);
            const auto* indices=reinterpret_cast<const uint16_t*>(raw+h->indexOffset);
            // Preserve indexed vertex reuse; the bounded temporary remap is
            // rebuilt once per source blob, never per frame on a cache hit.
            uint16_t remap[4096];std::fill(remap,remap+h->vertexCount,uint16_t{0xFFFF});
            auto vertex=[&](uint32_t j) {
                const uint16_t src=(h->flags&kChunkIndexed)?indices[b.indexStart+j]:static_cast<uint16_t>(b.indexStart+j);
                if(remap[src]!=0xFFFF)return remap[src];
                const auto& v=vertices[src];const auto out=static_cast<uint16_t>(use->vertices.size());
                use->vertices.push_back(WmoBatchVertex{v.u/32768.f*h->uvScale[0]+h->uvMin[0],
                    v.v/32768.f*h->uvScale[1]+h->uvMin[1],v.color,v.x,v.y,v.z});
                remap[src]=out;return out;
            };
            if(!use->indices.empty()) {
                const bool odd=use->indices.size()&1u;
                const uint16_t first=vertex(0),last=use->indices.back();
                use->indices.push_back(last);use->indices.push_back(first);
                if(odd)use->indices.push_back(first);
            }
            for(uint32_t j=0;j<b.indexCount;++j)use->indices.push_back(vertex(j));
        }
        use->frame=frame;return use;
    }
    size_t bytes()const {
        size_t n=0;for(const auto& s:slots_)n+=s.vertices.capacity()*sizeof(WmoBatchVertex)+s.keys.capacity()*sizeof(Key)+s.indices.capacity()*sizeof(uint16_t);return n;
    }
    void clear(){for(auto& s:slots_){Slot empty;s=std::move(empty);}}
private:
    std::array<Slot,kSlots> slots_{}; // <=64 KiB payload; no startup allocation
};
}
