// Helm models of the NPC races in items.pak.
//
// The 3.3.5a client loads a helm as Item\ObjectComponents\Head\<LeftModel>_<ChrRaces.ClientPrefix><M|F>.m2 for
// any race - an NPC of a race outside the playable ones (CreatureDisplayInfoExtra) wears the file
// of its race. items.pak's ItemModels (psp_item_format.hpp) has columns for the 10 playable races and the base
// name ([20], where any other race goes); a helm with no base file had no model for those NPCs. psp_item_conv adds,
// for such helms only (no base model, a model of an NPC race exists), one "hr_<display>" record: the NPC races'
// variants (race, sex -> "a_<n>" model, "t_<n>" skin). Additive: the WITM records do not change, a pak without
// "hr_" records reads as before (no helm for those NPCs). ItemBank::npcRaceHelm reads it (only for a race outside
// the 10 playable ones).
#pragma once

#include <cstdint>

namespace wowee::rendering::psp {

constexpr char kNpcRaceHelmPrefix[] = "hr_";  // + the ItemDisplayInfo id

struct NpcRaceHelmHeader {     // "hr_<display>"
    char magic[4];             // "WHRV"
    uint16_t version;          // 1
    uint16_t count;            // NpcRaceHelmVariant records that follow
    uint32_t displayId;
};
static_assert(sizeof(NpcRaceHelmHeader) == 12, "hr_ header is 12 bytes");

struct NpcRaceHelmVariant {
    uint8_t race;              // ChrRaces id (not one of the 10 playable races)
    uint8_t sex;               // 0 male, 1 female
    uint16_t model;            // "a_<n>"
    uint16_t texture;          // "t_<n>", kNoItemLayer = none (the model's own textures)
    uint16_t reserved;
};
static_assert(sizeof(NpcRaceHelmVariant) == 8, "hr_ variant is 8 bytes");

constexpr uint32_t kNpcRaceHelmMaxVariants = 64;  // 2 sexes x ChrRaces ids (21 in 3.3.5a), with room

inline bool npcRaceHelmValid(const void* blob, uint32_t size) {
    if (size < sizeof(NpcRaceHelmHeader)) return false;
    const auto* h = static_cast<const NpcRaceHelmHeader*>(blob);
    return h->magic[0] == 'W' && h->magic[1] == 'H' && h->magic[2] == 'R' && h->magic[3] == 'V' && h->version == 1 &&
           h->count <= kNpcRaceHelmMaxVariants &&
           size == sizeof(NpcRaceHelmHeader) + h->count * sizeof(NpcRaceHelmVariant);
}

}  // namespace wowee::rendering::psp
