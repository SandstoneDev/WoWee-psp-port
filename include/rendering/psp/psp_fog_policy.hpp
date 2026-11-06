#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace wowee::rendering::psp {
inline float worldFarClip(bool classicLow) { return classicLow ? 177.f : 450.f; }
struct FogRange { float start,end; };
inline FogRange fitFogRange(float start,float end,float farClip) {
    const float limit=farClip*.98f;
    if (!std::isfinite(end) || end<=1.f) end=limit;
    if (!std::isfinite(start)) start=end*.55f;
    // A narrow authored band becomes a visible wall at PSP draw distances.
    const float share=std::clamp(start/end,0.f,.72f);
    end=std::min(end,limit);
    return {end*share,end};
}
inline FogRange adaptiveFogRange(FogRange fog,bool enabled) {
    // Delay the first haze by a fifth of the band, without extending the far
    // plane, streaming radius, or fully hidden edge. No extra geometry work.
    if(enabled)fog.start+=(fog.end-fog.start)*.2f;
    return fog;
}
inline float smoothVisibility(float t) {
    t=std::clamp(t,0.f,1.f);return t*t*(3.f-2.f*t);
}
inline float objectDistanceVisibility(float distance,float limit) {
    // Stay within the existing draw budget. The outer shell is submitted
    // while almost completely fogged, then reveals smoothly on approach.
    const float width=std::min(limit*.4f,std::clamp(limit*.25f,12.f,40.f));
    return smoothVisibility((limit-distance)/std::max(1.f,width));
}
inline float objectResidentVisibility(uint32_t ageMs) {
    return smoothVisibility(static_cast<float>(ageMs)/400.f);
}
inline float objectFogStart(FogRange fog,float nearestDepth,float visibility) {
    if(visibility>=1.f || nearestDepth>=fog.end)return fog.start;
    // Move only this object's fog start. End/colour and normal world visibility
    // remain unchanged. This works with opaque/cutout materials in one pass.
    const float span=fog.end-std::min(fog.start,std::max(0.f,nearestDepth));
    return fog.start-span*(1.f-visibility)/std::max(.0001f,visibility);
}
}
