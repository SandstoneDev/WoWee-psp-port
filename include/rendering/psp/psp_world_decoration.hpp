#pragma once
#include "rendering/psp/psp_model_format.hpp"
namespace wowee::rendering::psp {
// Legacy packs may still contain generated ground decoration. Consume it once,
// without textures, render instances or deferred top-up work.
inline bool excludedGroundDecoration(uint16_t flags){return (flags&kModelGroundDetail)!=0;}
inline bool pollStreamReadiness(uint32_t now,uint32_t& next){
    if(static_cast<int32_t>(now-next)<0)return false;
    next=now+250u;return true;
}
}
