#pragma once
#include <cstdint>
namespace wowee::platform::psp {
// WotLK rest-trigger identities: AzerothCore areatrigger_tavern, checked 2026-11-03.
// https://github.com/azerothcore/azerothcore-wotlk/blob/master/data/sql/base/db_world/areatrigger_tavern.sql
// These are notifications, never client-side grants of rested XP or instant logout.
inline bool isRestAreaTrigger(uint32_t id) {
    switch(id) {
        case 71: case 98: case 178: case 562: case 682: case 707: case 708: case 709:
        case 712: case 713: case 715: case 716: case 717: case 719: case 720: case 721:
        case 722: case 742: case 743: case 843: case 844: case 862: case 982: case 1022:
        case 1023: case 1024: case 1025: case 1042: case 1606: case 1646: case 2266: case 2267:
        case 2286: case 2287: case 2610: case 2786: case 3547: case 3690: case 3886: case 3985:
        case 4058: case 4090: case 4108: case 4109: case 4240: case 4241: case 4265: case 4300:
        case 4336: case 4337: case 4373: case 4374: case 4375: case 4376: case 4377: case 4378:
        case 4380: case 4381: case 4382: case 4383: case 4486: case 4494: case 4498: case 4499:
        case 4521: case 4526: case 4528: case 4555: case 4558: case 4577: case 4595: case 4607:
        case 4608: case 4640: case 4714: case 4753: case 4755: case 4756: case 4769: case 4775:
        case 4847: case 4861: case 4867: case 4868: case 4910: case 4961: case 4964: case 4965:
        case 4966: case 4967: case 4970: case 4975: case 4976: case 4977: case 4979: case 4993:
        case 5030: case 5045: case 5062: case 5164: case 5182: case 5183: case 5200: case 5204:
        case 5217: case 5227: case 5314: case 5315: case 5316: case 5317: case 5323: case 5327:
        case 5360:
            return true;
        default:return false;
    }
}
inline bool suppressAreaTrigger(uint32_t id,bool first,bool cooldown,bool returnPortal) {
    return !isRestAreaTrigger(id) && (first || cooldown || returnPortal);
}
// Controller combat mode alone is not actual combat and must not hide resting.
inline bool showRestStatus(bool resting,bool combat,bool attacking) {
    return resting && !combat && !attacking;
}
}
