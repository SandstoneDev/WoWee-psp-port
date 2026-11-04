#pragma once
#include "rendering/psp/psp_model_format.hpp"
#include <cmath>
#include <cstdint>
#include <functional>
namespace wowee::rendering::psp {
inline uint32_t wmoDepthBucket(float distance) {
    if(!std::isfinite(distance) || distance>=1048576.f) return 32768u;
    return distance<=0.f ? 0u : static_cast<uint32_t>(distance/32.f);
}
template<class Item> bool wmoDrawBefore(const Item& a,const Item& b,bool depthOrder) {
    const bool ab=a.blend>=kBlendAlpha, bb=b.blend>=kBlendAlpha;
    if(ab!=bb) return bb; // depth-writing opaque/alpha-key geometry first
    if(ab) return a.dist>b.dist; // keep transparent geometry back-to-front
    if(depthOrder) {
        if(a.depthBand!=b.depthBand) return a.depthBand<b.depthBand;
    }
    // Coarse depth bands retain texture batching within each band.
    if(a.texture!=b.texture) return a.texture<b.texture;
    if(a.cell!=b.cell) return a.cell<b.cell;
    return std::less<const void*>{}(a.blob,b.blob);
}
}
