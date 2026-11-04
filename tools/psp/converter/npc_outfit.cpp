// Clean-room implementation (cleanroom/clean/equip) of the spec outfit_geosets.md section 3.3 (the outfit builder).
#include "npc_outfit.hpp"

namespace wowee::pspconv {

namespace body = ::wowee::rendering::psp::body;

void buildNpcOutfit(const uint32_t displayIds[11], const ItemDisplayLookup& itemDisplayInfo, bool capeTextureFound,
                    body::Outfit& out) {
    out = body::Outfit{};  // noFeet false, emblem invalid
    for (int kind = 0; kind < body::kKinds; ++kind) {
        const uint32_t displayId = displayIds[kind];
        if (displayId == 0) continue;
        NpcItemDisplayRow row;
        if (!itemDisplayInfo.find(displayId, row)) continue;
        if (kind == body::kCape && !capeTextureFound) continue;
        body::Item& it = out.item[kind];
        it.present = true;
        for (int g = 0; g < 3; ++g) it.geosetGroup[g] = static_cast<uint16_t>(row.geosetGroup[g] & 0xFFFFu);
        it.flags = static_cast<uint16_t>(row.flags & 0xFFFFu);
        for (int s = 0; s < body::kSections; ++s) {
            it.texture[s] = row.textureNonEmpty[s] ? kNpcPaintedTexture : body::kNone;
        }
    }
}

void applyNpcOutfitGeosets(std::unordered_set<uint16_t>& vis, const body::Outfit& outfit) {
    body::Dress dress;
    body::buildDress(outfit, dress);
    body::outfitGeosets(vis, outfit, dress);
}

}  // namespace wowee::pspconv
