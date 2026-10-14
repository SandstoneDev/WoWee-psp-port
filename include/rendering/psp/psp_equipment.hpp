// Equipment on PSP characters (M10c): upstream's rules, data from
// data/items.pak (tools/psp/converter psp_item_conv, psp_item_format.hpp).
//  - geosets: EntitySpawner::setOnlinePlayerEquipment (entity_spawner_player
//    .cpp) on top of core::bareCharacterGeosets;
//  - body texture layers: CharacterRenderer::compositeWithRegions order -
//    slots 0..18, and within an item regions 0..7, over skin/face/underwear.
//  Geosets, body layers, the helm's hidden parts
//  and the cloak follow psp_body_rules.hpp (clean-room implementation of the
//  equipment specs), Show Helm / Show Cloak included (Equipment::hideHelm /
//  hideCloak); weapons, shields and the ranged item by their sheath
//  (equipmentAttachmentsBySheath); upstream's item model rules stay
//  (equipmentAttachments).
#pragma once

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_pak_index.hpp"  // the sorted items.pak
#include "rendering/psp/psp_body_rules.hpp"  // body rules
#include "rendering/psp/psp_item_format.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::rendering::psp {

// 19 visible slots in EquipSlot order (0 head .. 18 tabard).
constexpr int kEquipmentSlots = 19;

struct Equipment {
    uint32_t displayId[kEquipmentSlots] = {};
    uint8_t invType[kEquipmentSlots] = {};
    // ---- weapons, added ----
    // The weapon slots main hand / off hand / ranged (EquipSlot 15 / 16 / 17): sheath[k] = the Item.dbc
    // SheatheType of the item worn (kSheathUnknown: not known - the display's hint, else upstream's inventory-type
    // rule); enchantVisual[k] = the item visual its enchantments put on it (ItemVisuals.dbc id, 0 = none:
    // PLAYER_VISIBLE_ITEM_n_ENCHANTMENT through psp_item_format.hpp visibleEnchantVisual, or SMSG_CHAR_ENUM's).
    uint8_t sheath[3] = {kSheathUnknown, kSheathUnknown, kSheathUnknown};
    uint16_t enchantVisual[3] = {0, 0, 0};
    // ---- weapons (end) ----
    // ---- body rules, added ----
    // Show Helm / Show Cloak off: PLAYER_FLAGS_HIDE_HELM (0x400) / _HIDE_CLOAK (0x800) of the wearer (the own
    // player's and every other player's update fields; CHAR_ENUM's character flags carry the same bits). The item
    // stays worn; what is drawn is shownEquipment.
    bool hideHelm = false;
    bool hideCloak = false;
    // ---- guild tabard, added ----
    // The wearer's guild tabard data (SMSG_GUILD_QUERY_RESPONSE of PLAYER_GUILDID's guild): a worn guild tabard
    // (ItemDisplayInfo Flags & 0x1) shows it (psp_emblem_format.hpp); invalid = no guild or not known yet. A change is
    // a change of the equipment (the answer arriving re-dresses the torso sections).
    emblem::Emblem emblem;
    // ---- guild tabard (end) ----
    bool empty() const {
        for (uint32_t d : displayId)
            if (d) return false;
        return true;
    }
    bool operator==(const Equipment&) const = default;
};

// equipment_dress.md 3.1 (clean-room implementation): a copy of `eq` with slot 0 cleared when hideHelm is set and
// slot 14 cleared when hideCloak is set (display id and inventory type 0; the flags stay set). Everything drawn
// (geosets, body, cloak, item models) takes this; `eq` itself stays what is worn (a change of the flags is a change
// of the equipment: re-dress).
inline Equipment shownEquipment(const Equipment& eq) {
    Equipment shown = eq;
    if (shown.hideHelm) {
        shown.displayId[0] = 0;
        shown.invType[0] = 0;
    }
    if (shown.hideCloak) {
        shown.displayId[14] = 0;
        shown.invType[14] = 0;
    }
    return shown;
}

// items.pak holds every ItemDisplayInfo row
// (~100 000 records) and is read on demand - the TOC stays on the card
// (platform::psp::PakIndex: its fence + two TOC blocks in RAM); an older,
// unsorted items.pak still opens through Pak (TOC in RAM) as before. What is
// read is kept within budgets, least recently used out first:
//   - records ("i_"): a fixed array of slots (default kRecordSlots). get() /
//     models() point into a slot; the pointer stays valid until that many
//     other displays were read - a dress asks for at most 19, so a caller may
//     hold the pointers while it dresses one character (as applyEquipment-
//     Geosets / equipmentAttachments do), not longer;
//   - layers ("q_", WLAY): kept up to kLayerCacheBytes; loadLayer hands out a
//     copy (the caller frees it with pakFree, as before);
//   - Item.dbc blocks ("u_"): kTableBlocks of them;
//   - models and textures are not cached here: their one cache for every
//     character is psp_attachments (wowee.cfg attachmax), capes included.
// The public API is source compatible (open/get/models/loadLayer/loadTexture/
// loadModel and the free functions below); the rest is added. Main thread only.
class ItemBank {
public:
    static constexpr uint32_t kRecordSlots = 128;          // x 236 B = 29.5 KB
    static constexpr uint32_t kLayerCacheBytes = 48 * 1024;
    static constexpr uint32_t kTableBlocks = 4;            // x 2 KB

    ItemBank() = default;
    ~ItemBank() { close(); }
    ItemBank(const ItemBank&) = delete;
    ItemBank& operator=(const ItemBank&) = delete;

    bool open(const char* path);
    bool isOpen() const { return index_.isOpen() || pak_.isOpen(); }
    // The baked record, or nullptr when the display id was not baked.
    const ItemDisplay* get(uint32_t displayId);
    // A layer blob ("q_<n>", WLAY) for the caller to blit; free with pakFree.
    void* loadLayer(uint16_t id, uint32_t& size);
    // A cape texture blob ("t_<n>", WTEX) for textureFromBlob.
    void* loadTexture(uint16_t id, uint32_t& size);
    // The display's item models (nullptr: none, or an items.pak
    // baked before them) and an "a_<n>" WMDL blob (free with pakFree).
    const ItemModels* models(uint32_t displayId);
    void* loadModel(uint16_t id, uint32_t& size);

    // ---- sorted items.pak, added ----
    void close();
    // True when the pak is the sorted one (PakIndex), false for an old pak (Pak).
    bool indexed() const { return index_.isOpen(); }
    // Item.dbc of the game data ("u_<block>"): the ItemDisplayInfo id
    // and InventoryType of an item id - another player's PLAYER_VISIBLE_ITEM
    // entries, an NPC's UNIT_VIRTUAL_ITEM_SLOT_ID (upstream
    // tryAttachCreatureVirtualWeapons: Item.dbc DisplayID). False when the
    // data's Item.dbc has no such item (a server's custom item: ask the
    // server, SMSG_ITEM_QUERY_SINGLE_RESPONSE) or the pak predates the table.
    bool itemDisplay(uint32_t itemId, uint32_t& displayId, uint8_t& inventoryType);
    // Budgets (before or after open; a smaller budget applies at the next read).
    void setBudget(uint32_t recordSlots, uint32_t layerCacheBytes);
    // RAM held now: the TOC index (or Pak's TOC) and every cache, bytes.
    uint32_t ramBytes() const;
    // "item_bank at=<at> ..." - RAM, cache use, reads (the card) and hits.
    void logStats(const char* at) const;
    // The pak's descriptor goes back after idleMs without a read (the card's few files).
    void releaseIfIdle(uint32_t nowMs, uint32_t idleMs) {
        index_.releaseIfIdle(nowMs, idleMs);
        pak_.releaseIfIdle(nowMs, idleMs);
        if (emblemsState_ > 0) emblems_.releaseIfIdle(nowMs, idleMs);  // data/emblems.pak
    }
    // items.pak and emblems.pak are rare paks (the main thread reads them when
    // dressing) - an open that finds no descriptor may ask for theirs (pakFdRelieve). Inline: the PC tools that build
    // ItemBank over their own Pak / PakIndex never call it.
    void setRareFds(bool on) {
        index_.setRare(on);
        pak_.setRare(on);
        emblems_.setRare(on);
    }

    // ---- weapons, added ----
    // itemDisplay and the item's Item.dbc SheatheType (kSheathUnknown when the pak's table predates it).
    bool itemDisplay(uint32_t itemId, uint32_t& displayId, uint8_t& inventoryType, uint8_t& sheath);
    // A record by name ("ivis", "ifx_<n>", "ift_<n>"): 64-byte aligned, free with pakFree; nullptr when absent.
    void* loadRecord(const char* name, uint32_t& size) { return isOpen() ? loadNamed(name, size) : nullptr; }

    // ---- body rules, added ----
    // The record's body tail (WITM version 4: GeosetGroup[1], Flags, HelmetGeosetVis ids); nullptr for a display
    // not baked or a record of an older items.pak. Valid like get().
    const ItemBodyInfo* body(uint32_t displayId);
    // "b_rules" (read at open): ChrRaces Flags by race id (0 when the pak predates it) and a HelmetGeosetVisData
    // row by id (nullptr: none).
    bool hasBodyRules() const { return bodyRules_; }
    uint32_t raceFlags(uint8_t race) const { return race < kBodyRuleRaces ? raceFlags_[race] : 0u; }
    const HelmetVisRow* helmetVis(uint32_t id) const;
    // Re-dress without a hitch: the lookups of a dress in one pass, in TOC (name) order - names that share a TOC
    // block of 8 KB then cost one block read instead of one each - and remembered (an entry's place on the card,
    // kLocations of them), so a record or layer read again later (evicted, or another character in the same
    // item) is one read without the TOC. Records go into their slots, layers only get located (they are read
    // in the order they are blitted: no extra RAM). Main thread only, like the rest.
    void prefetchRecords(const uint32_t* displayIds, uint32_t count);
    void prefetchLayers(const uint16_t* layerIds, uint32_t count);
    static constexpr uint32_t kLocations = 512;            // x 12 B = 6 KB

    // ---- NPC race helms, added ----
    // The helm model of an NPC race (psp_item_npc_race.hpp, "hr_<display>"): a race outside the 10 playable ones
    // wearing a helm whose base model the game data lacks. False when there is none (or the pak predates them).
    // Results cached per display (psp_item_npc_race.cpp).
    bool npcRaceHelm(uint32_t displayId, uint8_t race, uint8_t sex, uint16_t& model, uint16_t& texture);

    // ---- guild tabard, added ----
    // A guild emblem part (psp_emblem_format.hpp: emblem::kBackground / kBorder / kEmblem by its code) for a torso
    // section - a WLAY like loadLayer's, from data/emblems.pak next to items.pak (psp_emblem_conv; a sorted pak read
    // through PakIndex, opened on first use, its descriptor given back with items.pak's in releaseIfIdle). nullptr
    // when the pak or the entry is missing (the tabard then shows its own texture: an emblem part never replaces it
    // half). Free with pakFree. Main thread only.
    void* loadEmblemLayer(uint8_t part, int section, uint16_t code, uint32_t& size);
    bool emblemsAvailable();  // data/emblems.pak opens (tried once per open())

private:
    struct Slot {
        uint32_t displayId = 0;
        uint32_t lastUse = 0;
        ItemDisplay display;
        ItemModels models;
        bool hasModels = false;
        bool hasBody = false;    // the body tail
        ItemBodyInfo body{};     // the body tail
    };
    struct Layer {
        void* blob = nullptr;
        uint32_t size = 0;
        uint32_t lastUse = 0;
    };
    struct TableBlock {
        void* blob = nullptr;   // ItemTableHeader + entries
        uint32_t size = 0;
        uint32_t block = 0xFFFFFFFFu;
        uint32_t lastUse = 0;
    };
    void* loadNamed(const char* name, uint32_t& size);
    Slot* slotFor(uint32_t displayId);
    void freeCaches();
    // An "i_" / "q_" / "u_" entry by prefix and id - its remembered place on the card, else the TOC.
    void* loadEntry(char prefix, uint32_t id, uint32_t& size);
    bool locate(char prefix, uint32_t id);
    void loadBodyRules();

    platform::psp::PakIndex index_;  // the sorted pak
    platform::psp::Pak pak_;         // an older pak
    std::vector<Slot> slots_;        // capacity reserved once: addresses stay put
    std::unordered_map<uint32_t, uint16_t> slotOf_;
    std::unordered_set<uint32_t> missing_;
    std::unordered_map<uint16_t, Layer> layers_;
    uint32_t layerBytes_ = 0;
    TableBlock table_[kTableBlocks];
    std::unordered_set<uint32_t> tableMissing_;  // blocks the pak has not
    uint32_t recordSlots_ = kRecordSlots, layerBudget_ = kLayerCacheBytes;
    uint32_t tick_ = 0;
    uint32_t recordReads_ = 0, recordHits_ = 0, layerReads_ = 0, layerHits_ = 0, tableReads_ = 0, tableHits_ = 0;
    // ---- body rules ----
    struct Location {
        uint32_t key = 0;   // prefix << 24 | id; 0 = free
        uint32_t offset = 0, size = 0;
    };
    Location* locations_ = nullptr;   // kLocations, direct mapped (allocated with the sorted pak)
    uint32_t locationHits_ = 0;
    bool bodyRules_ = false;
    uint32_t raceFlags_[kBodyRuleRaces] = {};
    std::vector<HelmetVisRow> helmetVis_;
    // Display -> its "hr_" variants packed (race << 40 | sex << 32 | model << 16 | texture),
    // empty when the pak has none for it.
    std::unordered_map<uint32_t, std::vector<uint64_t>> npcRaceHelms_;
    // data/emblems.pak (0 not tried, 1 open, -1 missing), its path (items.pak's folder)
    platform::psp::PakIndex emblems_;
    int emblemsState_ = 0;
    char emblemsPath_[128] = {};
    uint32_t emblemReads_ = 0, emblemMisses_ = 0;
};

// Dressing a character from its equipment - equipment_dress.md 3.3-3.6, clean-room
// implementation in psp_equipment_dress.cpp over the body rules (psp_body_rules.hpp), for what is shown
// (shownEquipment).

// 3.3: the outfit of the shown slots by kind (each record's GeosetGroups / Flags / layer per section for the sex),
// the race's no-feet flag ("b_rules"), the guild emblem, and the shown head item's hidden parts (kHelmetHideKinds
// race masks; with race 0 or without "b_rules": the record's hidesHair hides the hair of every race). The records
// are read in one pass (ItemBank::prefetchRecords).
void outfitOf(const Equipment& eq, uint8_t race, uint8_t sex, ItemBank& items, body::Outfit& out,
              uint32_t helmHide[kHelmetHideKinds]);

// 3.4: `geosets` = the character's bare set (core::bareCharacterGeosets); the helm hides what its row says for the
// race (body::helmetGeosets), then the equipment groups (body::outfitGeosets).
void applyEquipmentGeosets(std::unordered_set<uint16_t>& geosets, const Equipment& eq, uint8_t race, uint8_t sex,
                           ItemBank& items);
// 3.4 legacy overload (psp_equip_preview): ignores hairScalp, hasHairStyle and modelGeosets; behaves as race 0.
void applyEquipmentGeosets(std::unordered_set<uint16_t>& geosets, uint16_t hairScalp, bool hasHairStyle,
                           const Equipment& eq, uint8_t sex, ItemBank& items,
                           const std::unordered_set<uint16_t>& modelGeosets);

// 3.5: the body layers to composite over the skin, face and underwear: section by section, each section's layer
// slots (body::pasteOf). Whether a section's underwear shows is body::Paste::underwear.
std::vector<uint16_t> equipmentLayers(const Equipment& eq, uint8_t race, uint8_t sex, ItemBank& items);
std::vector<uint16_t> equipmentLayers(const Equipment& eq, uint8_t sex, ItemBank& items);  // 3.5 legacy = race 0

// 3.6: the cape texture id ("t_<n>") of the shown back slot's display, kNoItemLayer when none.
uint16_t capeTexture(const Equipment& eq, uint8_t sex, ItemBank& items);

// Helmets, shoulders, weapons and shields as item models on
// attachment points. Two upstream paths decide where they go:
//  - Online: EntitySpawner::setOnlinePlayerEquipment (other players) - helm
//    at 11, shoulders Left 5 / Right 6, main hand 1, off hand 2, always held;
//  - Local: AppearanceComposer (the own player) - the same helm
//    (loadEquippedHelm), weapons by weaponAttachment(sheathed, slot,
//    inventoryType): held = shield
//    0, off hand 2, else 1; sheathed = two-hand 12 (back, weaponLocalTransform),
//    shield 28, one-hand/main-hand 32 (off hand 33), others hidden; the ranged
//    weapon in the right hand (sheathed: on the back) when the main hand is
//    empty. Upstream starts unsheathed (weaponsSheathed_ = false).
//    PSP: upstream's local path attaches no shoulders (only the online one
//    does); the game shows them on the own character too, so the local
//    rules take the online shoulders.
enum class AttachRules : uint8_t { Online, Local };

struct AttachPick {
    uint16_t point = 0;       // M2 attachment id
    uint16_t model = kNoItemModel;    // "a_<n>"
    uint16_t texture = kNoItemLayer;  // "t_<n>": the display's skin (kTexItemSkin batches)
    uint8_t slot = 0;         // equipment slot the item is worn in
    uint8_t side = 0;         // 1 = right shoulder model
    uint32_t displayId = 0;
    bool hasLocal = false;    // `local` applies (upstream attachWeapon localTransform)
    float local[12] = {};     // BoneMatrix layout (psp_skeleton.hpp)
    uint16_t itemVisual = 0;  // the glow on this weapon (ItemVisuals.dbc id), 0 = none
};

std::vector<AttachPick> equipmentAttachments(const Equipment& eq, uint8_t race, uint8_t sex, AttachRules rules,
                                             bool sheathed, ItemBank& items);

// ---- weapons, added ----
// The unit's sheath state, UNIT_FIELD_BYTES_2 byte 0 (SHEATH_STATE_*).
enum SheathState : uint8_t { kSheathStateUnarmed = 0, kSheathStateMelee = 1, kSheathStateRanged = 2 };

// weapon_placement.md 3.2-3.4 + weapon_glow.md 3 (clean-room implementation, psp_weapon_placement.cpp): every
// character's items by its sheath state - the helm at 11 and the shoulders at 5 / 6; the main and off hand in the
// hand when `sheathState` is melee (a shield on 0), else at sheathPointOf(hand, SheatheType) (Equipment::sheath, else
// the display's hint; neither known: legacyPutAwayPoint); the ranged item in hand only in the ranged state. A
// weapon's glow: the display's own ItemVisual, else Equipment::enchantVisual (AttachPick::itemVisual). Applies
// shownEquipment itself. equipmentAttachments / AttachRules above stay as they were.
struct SheathAttachOptions {
    // PSP option (wowee.cfg rangedback): with the main hand empty, the ranged item in the right hand when melee is
    // drawn and on the back (12, the two-hand transform) when put away.
    bool rangedOnBack = false;
    // A creature (NPC virtual items, UNIT_VIRTUAL_ITEM_SLOT_ID): its off-hand item is not shown while the main hand
    // holds a two-hand item (inventory type 17); players skip that test.
    bool creature = false;
};

// 3.3: the local transform translate(-0.03, -0.10, 0) x rotX(33 deg) x rotY(90 deg) x rotX(90 deg), first 3 rows of
// its 4 columns, column-major (12 floats).
void twoHandBackTransform(float out[12]);

// 3.3: the legacy put-away point of a hand slot (15 / 16) by inventory type; kNoSheathPoint when not shown.
// `hasLocal` tells whether the two-hand transform applies.
uint16_t legacyPutAwayPoint(int slot, uint8_t invType, bool& hasLocal);

// 3.2
std::vector<AttachPick> equipmentAttachmentsBySheath(const Equipment& eq, uint8_t race, uint8_t sex,
                                                     uint8_t sheathState, ItemBank& items,
                                                     const SheathAttachOptions& options = {});
// ---- weapons (end) ----

} // namespace wowee::rendering::psp
