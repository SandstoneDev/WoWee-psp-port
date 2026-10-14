// Equipment appearance baked for the PSP (M10c): "data/items.pak" written by
// tools/psp/converter psp_item_conv, read by src/rendering/psp/psp_equipment.
// Every ItemDisplayInfo row + the Item.dbc
// table "u_<block>", the pak sorted for PakIndex (see the block further down).
//
// Entries:
//   "i_<displayId>"  ItemDisplay: one ItemDisplayInfo.dbc row reduced to what
//                    the character needs (upstream CharacterRenderer
//                    compositeWithRegions + EntitySpawner geoset rules);
//                    followed by ItemModels since version 2
//   "q_<n>"          WLAY layer (include/rendering/psp/psp_model_format.hpp),
//                    placed by *region index* at half the 256 atlas coords
//   "t_<n>"          WTEX cape texture (upstream setOnlinePlayerEquipment
//                    "Cape texture": Left/RightModelTexture through
//                    pipeline::capeTextureCandidates), drawn on geoset group 15;
//                    since version 2 also the skins of the item models
//   "a_<n>"          WMDL item model: a helmet, shoulder, weapon or
//                    shield M2 (Item\ObjectComponents\...) in its Stand frame 0,
//                    static, drawn on a character attachment point
//                    (psp_model_format.hpp AttachPoint, "p_<race>_<sex>")
//
// Texture paths are resolved on the PC like upstream resolveItemRegionTexture:
// Item\TextureComponents\<RegionDir>\<name>_F|_M.blp, then _U, then plain.
#pragma once

#include "rendering/psp/psp_model_format.hpp"

#include <cstdint>
#include <cstring>

namespace wowee::rendering::psp {

constexpr int kItemRegions = 8;  // ArmUpper, ArmLower, Hand, TorsoUpper, TorsoLower, LegUpper, LegLower, Foot
constexpr uint16_t kNoItemLayer = 0xFFFF;

struct ItemDisplay {
    char magic[4];                  // "WITM"
    uint32_t displayId;
    uint16_t geosetGroup1;          // ItemDisplayInfo GeosetGroup1 (field 7)
    uint16_t geosetGroup3;          // GeosetGroup3 (field 9) - robe kilt
    uint8_t hidesHair[2];           // male, female (HelmetGeosetVisData, upstream helmHidesHair)
    uint16_t version;               // 0 = M10c (52 bytes, was "reserved"); kItemFormatAttach: + ItemModels;
                                    // kItemFormatBody (4): [+ ItemModels] + ItemBodyInfo last
    uint16_t layer[2][kItemRegions];  // [sex][region] -> "q_<n>", kNoItemLayer = none
    uint16_t capeTexture[2];          // [sex] -> "t_<n>", kNoItemLayer = none
};
static_assert(sizeof(ItemDisplay) == 52, "WITM record is 52 bytes");

// Upstream 256 atlas regions (CharacterRenderer regionCoords256 /
// regionSizes256): x, y, w, h. Layers are baked at half size for the 128
// body canvas.
constexpr uint16_t kItemRegionRect256[kItemRegions][4] = {
    {0, 0, 128, 64},    // ArmUpper
    {0, 64, 128, 64},   // ArmLower
    {0, 128, 128, 32},  // Hand
    {128, 0, 128, 64},  // TorsoUpper
    {128, 64, 128, 32}, // TorsoLower
    {128, 96, 128, 64}, // LegUpper
    {128, 160, 128, 64},// LegLower
    {128, 224, 128, 32},// Foot
};

// Item models on attachment points (upstream EntitySpawner::
// setOnlinePlayerEquipment / AppearanceComposer::loadEquippedWeapons +
// CharacterRenderer::attachWeapon). A WITM record with version >=
// kItemFormatAttach is followed by ItemModels; older items.pak stay readable
// (no models). Which models a display has is decided on the PC by the folder
// its ItemDisplayInfo model name resolves in - the folder upstream loads from
// for the slot the item is worn in:
//   Head     helm: LeftModel + race/sex suffix (core::raceGenderSuffix), else
//            the base name; LeftModelTexture likewise (core::resolveHelmVisual)
//   Shoulder LeftModel -> side 0 (attachment 5), RightModel -> side 1 (6), same
//            suffix rule, textures Left/RightModelTexture
//   Weapon, then Shield: pipeline::readItemDisplayArt (Left pair, else Right),
//            texture Weapon\<tex>.blp, else Shield\<tex>.blp
constexpr uint16_t kItemFormatAttach = 2;
constexpr int kItemVariants = 21;   // 10 races x 2 sexes + [20] = no race suffix
constexpr uint16_t kNoItemModel = 0xFFFF;
// Batch texture of an item model that takes the display's skin (upstream
// attachWeapon: setModelTexture on the replaceable slots 2/3/4, else slot 0),
// filled at run time from ItemModels::texture.
constexpr uint16_t kTexItemSkin = 0xFFF2;

enum ItemModelKind : uint8_t {
    kItemModelNone = 0,
    kItemModelHelm = 1,
    kItemModelShoulder = 2,
    kItemModelWeapon = 3,   // Item\ObjectComponents\Weapon
    kItemModelShield = 4,   // Item\ObjectComponents\Shield
};

struct ItemModels {
    uint8_t kind;                           // ItemModelKind
    // Were reserved[3] (0 in older bakes); valid in records of version
    // kItemFormatWeapons and later (see the weapons block at the end of this file).
    uint8_t sheathHint;                     // the Item.dbc SheatheType most items of this display have (kSheathUnknown)
    uint16_t itemVisual;                    // ItemDisplayInfo.ItemVisual: the display's own glow (ItemVisuals.dbc), 0 none
    uint16_t model[2][kItemVariants];       // [side][race/sex] -> "a_<n>", kNoItemModel = none
    uint16_t texture[2][kItemVariants];     // [side][race/sex] -> "t_<n>", kNoItemLayer = none
};
static_assert(sizeof(ItemModels) == 172, "WITM model tail is 172 bytes");

// Race/sex column of ItemModels (the races core::raceGenderSuffix knows);
// any other race takes column 20 (base names, as an empty suffix upstream).
inline int itemVariant(uint8_t race, uint8_t sex) {
    constexpr uint8_t kRaces[10] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    for (int i = 0; i < 10; ++i)
        if (kRaces[i] == race) return i * 2 + (sex ? 1 : 0);
    return kItemVariants - 1;
}

// ---------------------------------------------------------------------------
// items.pak from the game data alone - every
// ItemDisplayInfo row (no list from the server DB any more), written sorted
// with the "~fence" record (tools/psp/converter conv_common.hpp
// SortedPakWriter) and read on the PSP through platform::psp::PakIndex: the
// TOC (~100 000 records) stays on the card. "i_", "q_", "t_", "a_" keep the
// formats above (psp_item_conv refuses a bake whose ids pass the u16 ranges).
// One new entry kind:
//   "u_<block>"      WITB: Item.dbc -> ItemDisplayInfo id + InventoryType of
//                    the item ids block * kItemTableBlock .. + kItemTableBlock
//                    - 1 (the data's own item table: other players' visible
//                    items, NPC virtual items - upstream
//                    tryAttachCreatureVirtualWeapons reads Item.dbc
//                    DisplayID, field 5). Blocks without a row are not written.
constexpr uint32_t kItemTableBlock = 512;

struct ItemTableHeader {
    char magic[4];          // "WITB"
    uint32_t firstItem;     // block * kItemTableBlock
    uint32_t count;         // u32 entries that follow (<= kItemTableBlock)
    uint32_t reserved;
    // uint32_t entry[count]: ItemDisplayInfo id (bits 0-23) | InventoryType << 24; 0 = no Item.dbc row
};
static_assert(sizeof(ItemTableHeader) == 16, "WITB header is 16 bytes");

inline uint32_t itemTablePack(uint32_t displayId, uint32_t inventoryType) {
    return (displayId & 0xFFFFFFu) | ((inventoryType & 0xFFu) << 24);
}

// Checked view of an "a_<n>" WMDL item model: the PSP renderer, the
// converter's --verify and psp_equip_preview accept exactly the same blobs.
inline bool itemModelValid(const void* blob, uint32_t size) {
    if (!blob || size < sizeof(ModelHeader)) return false;
    ModelHeader h;
    std::memcpy(&h, blob, sizeof(h));
    if (std::memcmp(h.magic, "WMDL", 4) != 0 || h.version != kModelFormatVersion || (h.vertexOffset & 15u))
        return false;
    const uint32_t batches = static_cast<uint32_t>(h.batchCount[0]) + h.batchCount[1];
    if (h.batchCount[0] == 0 || h.vertexCount == 0) return false;
    if (uint64_t{h.vertexOffset} + uint64_t{h.vertexCount} * sizeof(ModelVertex) > size) return false;
    if (uint64_t{h.indexOffset} + uint64_t{h.indexCount} * 2u > size) return false;
    if (uint64_t{h.batchOffset} + uint64_t{batches} * sizeof(ModelBatch) > size) return false;
    const bool strips = (h.flags & kModelStrips) != 0;
    const uint32_t limit = strips ? h.vertexCount : h.indexCount;
    const auto* b = static_cast<const uint8_t*>(blob) + h.batchOffset;
    for (uint32_t i = 0; i < batches; ++i) {
        ModelBatch mb;
        std::memcpy(&mb, b + i * sizeof(ModelBatch), sizeof(mb));
        if (mb.indexStart > limit || mb.indexCount > limit - mb.indexStart) return false;
        if (mb.indexCount < 3) return false;
    }
    if (!strips) {
        const auto* idx = static_cast<const uint8_t*>(blob) + h.indexOffset;
        for (uint32_t i = 0; i < h.indexCount; ++i) {
            uint16_t v;
            std::memcpy(&v, idx + i * 2u, 2);
            if (v >= h.vertexCount) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Weapons, shields and their glows. All of it is
// added so that older paks and older EBOOTs keep reading each other:
//  - "u_<block>": with ItemTableHeader::reserved & kItemTableSheath, u8 sheath[count] follow the u32 entries:
//    the Item.dbc SheatheType (field 7) of each item;
//  - "i_<n>": record version kItemFormatWeapons - ItemModels::sheathHint / itemVisual (above) are set;
//  - "a_<n>" of a weapon or shield model: an ItemVisualPoints footer - the model's attachments 0..4, where the
//    effects of an item visual sit on it;
//  - "ivis": ItemVisualTable - SpellItemEnchantment -> ItemVisual, ItemVisuals.dbc -> its effect per slot;
//  - "ifx_<n>": WSFX effect n (psp_spellfx_format.hpp, tools/psp/converter/particle_bake.hpp bakeFx) of an
//    ItemVisualEffects.dbc model, look class kPtcItem;
//  - "ift_<n>": WTEX texture n of those effects.
// Where the items are placed and which glow they show: the specs weapon_placement.md / weapon_glow.md, implemented
// by sheathPointOf / visibleEnchantVisual below and psp_equipment.hpp equipmentAttachmentsBySheath (clean-room
// implementation).
constexpr uint16_t kItemFormatWeapons = 3;   // "i_" record version: ItemModels::sheathHint / itemVisual valid
constexpr uint32_t kItemTableSheath = 1u;    // ItemTableHeader::reserved bit: the u8 sheath[count] tail
constexpr uint8_t kSheathUnknown = 0xFF;     // no Item.dbc SheatheType known (older pak, a server's custom item)

enum ItemSheathType : uint8_t {  // Item.dbc SheatheType
    kSheathTypeNone = 0,
    kSheathTypeTwoHand = 1,      // two-handed axes, maces, swords (and polearms - the data says so)
    kSheathTypeStaff = 2,        // staves, most polearms
    kSheathTypeOneHand = 3,      // one-handers, daggers
    kSheathTypeShield = 4,
};

constexpr uint16_t kNoSheathPoint = 0xFFFF;  // not shown put away

// weapon_placement.md 3.1 (clean-room implementation): the M2 attachment an item sits on put away, by the hand
// (0 = main hand, 1 = off hand; the ranged slot takes this row too) and its Item.dbc SheatheType - 26 / 27
// SheathMainHand / SheathOffHand, 28 SheathShield, 30 / 31 LargeWeaponLeft / Right, 32 / 33 HipWeaponLeft / Right;
// kNoSheathPoint for the other types (0, 5 and above, kSheathUnknown) and for any other hand value.
inline uint16_t sheathPointOf(int hand, uint8_t type) {
    if (hand != 0 && hand != 1) return kNoSheathPoint;
    switch (type) {
        case kSheathTypeTwoHand: return hand == 0 ? 26 : 27;
        case kSheathTypeStaff: return hand == 0 ? 30 : 31;
        case kSheathTypeOneHand: return hand == 0 ? 32 : 33;
        case kSheathTypeShield: return 28;
        default: return kNoSheathPoint;  // 0 and 5 and above (incl. 0xFF)
    }
}

constexpr int kItemVisualSlots = 5;  // ItemVisuals.dbc: an effect per weapon attachment 0..4

// The last 72 bytes of a weapon / shield "a_<n>" blob (after the WMDL; older bakes have none): the item M2's
// attachments 0..4 - upstream findAttachmentBone(weapon, slot)'s offset, the bone not applied (as upstream
// attachWeaponEffect); a slot the model lacks sits at the origin (upstream).
struct ItemVisualPoints {
    float position[kItemVisualSlots][3];  // model space
    uint8_t found;                        // bit i: the model has attachment i
    uint8_t reserved[3];
    uint32_t reserved2;
    char magic[4];                        // "WIVP": the blob's last 4 bytes
};
static_assert(sizeof(ItemVisualPoints) == 72, "WIVP footer is 72 bytes");

// A float the PSP may compare (no NaN / inf: a compare with one traps on the Allegrex FPU), within 100 yd.
inline bool itemFloatSane(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    if ((u & 0x7F800000u) == 0x7F800000u) return false;
    return f > -100.0f && f < 100.0f;
}

inline bool itemVisualPoints(const void* blob, uint32_t size, ItemVisualPoints& out) {
    if (!blob || size < sizeof(ModelHeader) + sizeof(ItemVisualPoints)) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(blob) + size - sizeof(out), sizeof(out));
    if (std::memcmp(out.magic, "WIVP", 4) != 0) return false;
    for (const auto& p : out.position)
        for (float f : p)
            if (!itemFloatSane(f)) return false;
    return true;
}

constexpr uint16_t kItemVisualTableVersion = 1;

struct ItemVisualTableHeader {
    char magic[4];          // "WIVT"
    uint16_t version;       // kItemVisualTableVersion
    uint16_t reserved;
    uint32_t visualCount;   // ItemVisualRec[visualCount], by visual id
    uint32_t enchantCount;  // EnchantVisualRec[enchantCount] after them, by enchantment id (only those with a visual)
    uint32_t effectCount;   // "ifx_<n>", n < effectCount
    uint32_t textureCount;  // "ift_<n>", n < textureCount
};
static_assert(sizeof(ItemVisualTableHeader) == 24, "WIVT header is 24 bytes");

struct ItemVisualRec {      // an ItemVisuals.dbc row
    uint16_t visual;
    uint16_t effect[kItemVisualSlots];  // "ifx_<n>", 0xFFFF none (empty slot, no file, did not bake)
};
static_assert(sizeof(ItemVisualRec) == 12, "WIVT visual is 12 bytes");

struct EnchantVisualRec {   // SpellItemEnchantment.dbc ID -> its ItemVisual (field 31 in 3.3.5a)
    uint16_t enchant;
    uint16_t visual;
};
static_assert(sizeof(EnchantVisualRec) == 4, "WIVT enchantment is 4 bytes");

inline bool itemVisualTableValid(const void* blob, uint32_t size) {
    if (!blob || size < sizeof(ItemVisualTableHeader)) return false;
    ItemVisualTableHeader h;
    std::memcpy(&h, blob, sizeof(h));
    if (std::memcmp(h.magic, "WIVT", 4) != 0 || h.version != kItemVisualTableVersion) return false;
    if (h.visualCount > 4096 || h.enchantCount > 65536 || h.effectCount > 0xFFFF || h.textureCount > 0xFFFF)
        return false;
    if (sizeof(h) + uint64_t{h.visualCount} * sizeof(ItemVisualRec) + uint64_t{h.enchantCount} * sizeof(EnchantVisualRec) >
        size)
        return false;
    const auto* v = reinterpret_cast<const ItemVisualRec*>(static_cast<const uint8_t*>(blob) + sizeof(h));
    for (uint32_t i = 0; i < h.visualCount; ++i) {
        if (i && v[i].visual <= v[i - 1].visual) return false;
        for (uint16_t e : v[i].effect)
            if (e != 0xFFFF && e >= h.effectCount) return false;
    }
    const auto* e = reinterpret_cast<const EnchantVisualRec*>(v + h.visualCount);
    for (uint32_t i = 1; i < h.enchantCount; ++i)
        if (e[i].enchant <= e[i - 1].enchant) return false;
    return true;
}

// "ivis" table lookups (weapon_glow.md 3, clean-room implementation). The table is the pak record: header,
// ItemVisualRec[visualCount] sorted by visual, EnchantVisualRec[enchantCount] sorted by enchant. A null table, or
// one whose magic / version is not "WIVT" / 1, finds nothing (the size-checking validity test is
// itemVisualTableValid, used by the loader).
namespace ivis_detail {

inline bool header(const void* table, ItemVisualTableHeader& h) {
    if (table == nullptr) return false;
    std::memcpy(&h, table, sizeof h);
    return std::memcmp(h.magic, "WIVT", 4) == 0 && h.version == kItemVisualTableVersion;
}

inline const ItemVisualRec* visuals(const void* table) {
    return reinterpret_cast<const ItemVisualRec*>(static_cast<const uint8_t*>(table) + sizeof(ItemVisualTableHeader));
}

inline const EnchantVisualRec* enchants(const void* table, uint32_t visualCount) {
    return reinterpret_cast<const EnchantVisualRec*>(static_cast<const uint8_t*>(table) +
                                                     sizeof(ItemVisualTableHeader) +
                                                     size_t(visualCount) * sizeof(ItemVisualRec));
}

}  // namespace ivis_detail

// visual 0 -> null; otherwise the record whose `visual` equals it, or null.
inline const ItemVisualRec* itemVisualFind(const void* table, uint16_t visual) {
    if (visual == 0) return nullptr;
    ItemVisualTableHeader h;
    if (!ivis_detail::header(table, h)) return nullptr;
    const ItemVisualRec* recs = ivis_detail::visuals(table);
    uint32_t lo = 0;
    uint32_t hi = h.visualCount;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (recs[mid].visual < visual) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo < h.visualCount && recs[lo].visual == visual) ? &recs[lo] : nullptr;
}

// enchant 0 or > 65535 -> 0; otherwise the visual of the record whose `enchant` equals it (may be 0), or 0.
inline uint16_t enchantVisualFind(const void* table, uint32_t enchant) {
    if (enchant == 0 || enchant > 65535u) return 0;
    ItemVisualTableHeader h;
    if (!ivis_detail::header(table, h)) return 0;
    const EnchantVisualRec* recs = ivis_detail::enchants(table, h.visualCount);
    uint32_t lo = 0;
    uint32_t hi = h.enchantCount;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (recs[mid].enchant < enchant) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo < h.enchantCount && recs[lo].enchant == enchant) ? recs[lo].visual : 0;
}

// PLAYER_VISIBLE_ITEM_n_ENCHANTMENT: P = low 16 bits (permanent), T = high 16 bits (temporary). Which one shows:
// upstream AppearanceComposer::applyEnchantVisuals - a temporary enchantment (a sharpening stone, an oil) masks the
// permanent one, so with T set its visual is the glow (none when T has no visual), else P's.
inline uint16_t visibleEnchantVisual(const void* table, uint32_t enchantField) {
    const uint32_t temporary = enchantField >> 16;
    if (temporary != 0) return enchantVisualFind(table, temporary);
    return enchantVisualFind(table, enchantField & 0xFFFFu);
}

// ---------------------------------------------------------------------------
// WITM version kItemFormatBody (4) = version 3 + the body tail (ItemBodyInfo) - what
// the body rules (psp_body_rules.hpp) read of an ItemDisplayInfo row beyond the WITM header (spec item_bake.md B4) -
// and "b_rules", the two tables they read beside it (B5).
//
// Layout: the 52-byte header, then ItemModels when the display has models, then ItemBodyInfo as the record's LAST
// 8 bytes, on every record. An EBOOT from before version 4 still reads it (it takes the models only when the record holds
// 52 + 172 bytes, and a record without models is 60 bytes); this EBOOT reads a version 0/2/3 record without the
// tail as "no GeosetGroup[1], no flags, helm hides by hidesHair".
constexpr uint16_t kItemFormatBody = 4;

struct ItemBodyInfo {
    uint16_t geosetGroup2;   // ItemDisplayInfo GeosetGroup[1] (field 8): trousers -> 9xx kneepads, shirt -> 10xx
    uint16_t flags;          // ItemDisplayInfo Flags (field 10); the bits the body rules read: psp_body_rules.hpp
    uint16_t helmetVis[2];   // HelmetGeosetVisData id (field 13 male, 14 female); 0 = none, or no LeftModel
                             // (item_bake.md B4)
};
static_assert(sizeof(ItemBodyInfo) == 8, "WITM body tail is 8 bytes");

// Record sizes of version 4: without / with ItemModels.
constexpr uint32_t kItemRecordSizeNoModels = sizeof(ItemDisplay) + sizeof(ItemBodyInfo);                     // 60
constexpr uint32_t kItemRecordSizeModels = sizeof(ItemDisplay) + sizeof(ItemModels) + sizeof(ItemBodyInfo);  // 232

// "b_rules" (one record, ~0.8 KB, read when the bank opens; item_bake.md B5): ChrRaces.dbc Flags per race id and
// every HelmetGeosetVisData.dbc row (an id and kHelmetHideKinds race masks, bit 1 << race id).
constexpr char kBodyRulesName[] = "b_rules";
constexpr int kBodyRuleRaces = 32;
constexpr uint32_t kRaceFlagNoFeet = 0x2;  // the ChrRaces Flags bit the body rules read (body_layers.md R5)
constexpr int kHelmetHideKinds = 7;        // HelmetGeosetVisData columns 1..7 (helmet_visibility.md)

struct BodyRulesHeader {
    char magic[4];           // "WBRL"
    uint16_t version;        // 1
    uint16_t helmetVisRows;
    uint32_t raceFlags[kBodyRuleRaces];
    // HelmetVisRow rows[helmetVisRows]
};
static_assert(sizeof(BodyRulesHeader) == 136, "WBRL header is 136 bytes");

struct HelmetVisRow {
    uint32_t id;
    uint32_t hide[kHelmetHideKinds];
};
static_assert(sizeof(HelmetVisRow) == 32, "WBRL helmet row is 32 bytes");

} // namespace wowee::rendering::psp
