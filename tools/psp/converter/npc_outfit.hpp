// NPC outfit builder of the creature baker (psp_creature_conv): clean-room implementation (cleanroom/clean/equip) of
// the spec outfit_geosets.md section 3.3.
//
// Builds a body::Outfit from a CreatureDisplayInfoExtra row's 11 item-display columns (NPCItemDisplay, column k =
// Kind k), an ItemDisplayInfo lookup and the result of the converter's existing cape-texture lookup. The NPC's
// equipment groups are then body::outfitGeosets(vis, outfit, dress) with dress = body::buildDress(outfit).
// The rest of the NPC geoset rules (hair, facial hair, race 4 eye glow, removing ids the model lacks, the "no cape"
// panel) stay in the existing converter (integration boundary of section 3.3).
#pragma once

#include <cstdint>
#include <unordered_set>

#include "rendering/psp/psp_body_rules.hpp"

namespace wowee::pspconv {

// The ItemDisplayInfo columns the builder reads (public 3.3.5a layout).
struct NpcItemDisplayRow {
    uint32_t geosetGroup[3] = {0, 0, 0};  // columns 7..9
    uint32_t flags = 0;                   // column 10
    bool textureNonEmpty[8] = {};         // columns 15..22 hold a non-empty string
};

class ItemDisplayLookup {
public:
    virtual ~ItemDisplayLookup() = default;
    // false when ItemDisplayInfo has no row for the id
    virtual bool find(uint32_t displayId, NpcItemDisplayRow& out) const = 0;
};

// The value stored in Item::texture[s] for a painted section ("any value other than kNone"; only painted or not
// matters). Our choice: 0.
constexpr uint16_t kNpcPaintedTexture = 0;

// displayIds: the 11 NPCItemDisplay columns in Kind order. capeTextureFound: the converter's existing cape-texture
// lookup found a file for the cape column's display.
void buildNpcOutfit(const uint32_t displayIds[11], const ItemDisplayLookup& itemDisplayInfo, bool capeTextureFound,
                    ::wowee::rendering::psp::body::Outfit& out);

// Convenience: dress from the outfit, then body::outfitGeosets (equipment groups of 3.1 / 3.2).
void applyNpcOutfitGeosets(std::unordered_set<uint16_t>& vis, const ::wowee::rendering::psp::body::Outfit& outfit);

}  // namespace wowee::pspconv
