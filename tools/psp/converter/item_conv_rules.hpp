// Item converter rules (PC converter, psp_item_conv): clean-room implementation (cleanroom/clean/equip) of the spec
// item_bake.md rules B1-B9 and the record formats of its section 2 (psp_item_format.hpp).
//
// Inputs come through our own interfaces so that tests can feed the vector inputs:
//   * table rows as plain structs (the DBC loading stays with the existing converter),
//   * FileQuery   - "does this data file exist" (case-insensitive), returning the stored spelling,
//   * ModelNaming - the existing per-folder naming rules of the converter (racial helm / shoulder files, LeftModel
//                   else RightModel for weapons, textures likewise). Those rules are outside this rewrite (B7 note);
//                   B7 here only decides which folders are tried, in which order, and the resulting kind.
// Everything else of the converter (file index, BLP resampling and palettes, WMDL baking, cape / skin texture baking,
// sorted pak writing, --verify) stays outside. No exceptions are thrown by this code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rendering/psp/psp_item_format.hpp"

namespace wowee::pspconv::items {

namespace fmt = ::wowee::rendering::psp;

// ---------------------------------------------------------------------------------------------------------------
// Input rows (public 3.3.5a layouts; only the columns the rules read).

struct DisplayInfoRow {                     // ItemDisplayInfo.dbc
    uint32_t id = 0;                        // 0
    std::string leftModel;                  // 1
    std::string rightModel;                 // 2
    std::string leftModelTexture;           // 3
    std::string rightModelTexture;          // 4
    uint32_t geosetGroup[3] = {0, 0, 0};    // 7..9
    uint32_t flags = 0;                     // 10
    uint32_t helmetGeosetVis[2] = {0, 0};   // 13 male, 14 female
    std::string texture[8];                 // 15..22, section order ArmUpper..Foot
    bool hasItemVisualColumn = false;       // the table has more than 23 columns
    uint32_t itemVisual = 0;                // 23
};

struct HelmetVisDataRow {                   // HelmetGeosetVisData.dbc
    uint32_t id = 0;                        // 0
    uint32_t hide[7] = {0, 0, 0, 0, 0, 0, 0};  // 1..7
};

struct ItemRow {                            // Item.dbc
    uint32_t id = 0;                        // 0
    uint32_t itemClass = 0;                 // 1
    uint32_t subClass = 0;                  // 2
    uint32_t displayInfoId = 0;             // 5
    uint32_t inventoryType = 0;             // 6
    uint32_t sheatheType = 0;               // 7 (ignored when the table has fewer than 8 columns)
};

struct RaceRow {                            // ChrRaces.dbc
    uint32_t id = 0;                        // 0
    uint32_t flags = 0;                     // 1
};

// ---------------------------------------------------------------------------------------------------------------
// File queries.

std::string toLowerAscii(const std::string& s);
bool endsWithNoCase(const std::string& s, const char* suffix);

class FileQuery {
public:
    virtual ~FileQuery() = default;
    // Case-insensitive lookup of a data path ("Item\\...\\x.blp"). On success, `actualPath` (when not null) receives
    // the stored spelling of the path.
    virtual bool find(const std::string& path, std::string* actualPath) const = 0;
};

// A simple case-insensitive set of data paths ('/' and '\\' are treated alike).
class FileSet : public FileQuery {
public:
    void add(const std::string& path);
    bool find(const std::string& path, std::string* actualPath) const override;
    static std::string key(const std::string& path);

private:
    std::unordered_map<std::string, std::string> paths_;
};

// ---------------------------------------------------------------------------------------------------------------
// B1. Body region texture file of a display, section s, wearer sex (0 male / 1 female). "" = no layer.
extern const char* const kRegionTextureDir[8];
std::string bodyLayerSource(const std::string& textureName, int section, int sex, const FileQuery& files);

// B2. Cloak texture file of a display (both sexes). "" = none.
std::string capeTextureSource(const std::string& leftModelTexture, const FileQuery& files);

// B3. HelmetGeosetVisData by id.
class HelmetVisIndex {
public:
    explicit HelmetVisIndex(const std::vector<HelmetVisDataRow>& rows);
    const HelmetVisDataRow* find(uint32_t id) const;

private:
    std::vector<HelmetVisDataRow> rows_;
    std::unordered_map<uint32_t, size_t> index_;
};

uint8_t hidesHair(const DisplayInfoRow& row, int sex, const HelmetVisIndex& vis);

// B3 + B4 (version). `layer` / `cape` are the baked ids ("q_<n>" / "t_<n>", kNoItemLayer for none).
constexpr uint16_t kItemRecordVersion = 4;
fmt::ItemDisplay makeItemDisplay(const DisplayInfoRow& row, const HelmetVisIndex& vis, const uint16_t layer[2][8],
                                 const uint16_t cape[2]);

// B4. The body tail.
fmt::ItemBodyInfo makeBodyInfo(const DisplayInfoRow& row);

// B5. The "b_rules" record. races == null: no ChrRaces.dbc (all flags 0, `warning` set). helmetVisColumns: the
// column count of HelmetGeosetVisData.dbc (hide values of missing columns are 0).
std::vector<uint8_t> buildBodyRules(const std::vector<RaceRow>* races, const std::vector<HelmetVisDataRow>& helmetVis,
                                    int helmetVisColumns, std::string* warning);

// B6. The "u_<block>" item table.
constexpr uint32_t kItemsPerBlock = 512;
constexpr int kExitCodeBadItemRow = 4;

struct ItemTableBlock {
    uint32_t block = 0;
    std::vector<uint8_t> bytes;
    std::string name() const { return "u_" + std::to_string(block); }
};

enum class ItemTableStatus { kOk, kDisplayIdTooLarge, kInventoryTypeTooLarge };

// Blocks come out in ascending block order. On an aborting row, `out` is cleared and `badItemId` (when not null)
// receives the row's ID.
ItemTableStatus buildItemTable(const std::vector<ItemRow>& rows, bool hasSheatheColumn,
                               std::vector<ItemTableBlock>& out, uint32_t* badItemId);

// B7. Model folder by the slot kind of the display's items.
enum class SlotKind : uint8_t { kUnknown = 0, kHead, kShoulder, kHand };
enum class ModelFolder : uint8_t { kHead = 0, kShoulder, kWeapon, kShield };

// false when the inventory type does not count.
bool slotKindOfInventoryType(uint32_t inventoryType, SlotKind& kind);
SlotKind displaySlotKind(const std::vector<uint32_t>& inventoryTypes);
const char* modelFolderName(ModelFolder folder);       // "Head", "Shoulder", "Weapon", "Shield"
int modelFolderOrder(SlotKind kind, ModelFolder out[4]);  // number of folders written
uint8_t modelKindOf(ModelFolder folder);               // 1 helm, 2 shoulder, 3 weapon, 4 shield

// Source files of the item models found in one folder, per side and variant ("" = none).
struct FolderModels {
    std::string model[2][fmt::kItemVariants];
    std::string texture[2][fmt::kItemVariants];
    bool anyModel() const;
};

class ModelNaming {
public:
    virtual ~ModelNaming() = default;
    // The existing naming rules inside `folder` (outside this rewrite). Returns true and fills `out` when the
    // folder yields a model for the display.
    virtual bool tryFolder(ModelFolder folder, const DisplayInfoRow& row, FolderModels& out) = 0;
};

// The first folder that yields a model wins; returns the resulting kind (0 = no models).
uint8_t chooseItemModels(SlotKind kind, const DisplayInfoRow& row, ModelNaming& naming, FolderModels& out);

// B8. Sheath hint of a display from the SheatheType values of all its Item.dbc rows. 255 = unknown.
constexpr uint8_t kSheathHintUnknown = 255;
uint8_t sheathHint(const std::vector<uint32_t>& sheatheTypes);

// B9. Own item visual of a display.
uint16_t ownItemVisual(const DisplayInfoRow& row, const std::unordered_set<uint32_t>& itemVisualIds);

// ---------------------------------------------------------------------------------------------------------------
// Record writers / readers (little-endian, independent of the host byte order).
std::vector<uint8_t> writeItemRecord(const fmt::ItemDisplay& display, const fmt::ItemModels* models,
                                     const fmt::ItemBodyInfo& body);
bool readItemRecord(const std::vector<uint8_t>& bytes, fmt::ItemDisplay& display, bool& hasModels,
                    fmt::ItemModels& models, fmt::ItemBodyInfo& body);
std::vector<uint8_t> writeItemTableBlock(uint32_t firstItem, uint32_t count, bool withSheath, const uint32_t* entries,
                                         const uint8_t* sheath);
std::vector<uint8_t> writeBodyRules(const uint32_t raceFlags[32], const std::vector<fmt::HelmetVisRow>& rows);

// ---------------------------------------------------------------------------------------------------------------
// Whole decision pass over the tables (what the converter writes, apart from the baked images / models).

// Case-insensitive source path -> sequential id (q_<n>, t_<n>, a_<n>).
class SourceIds {
public:
    uint16_t idOf(const std::string& path);  // kNoItemLayer when the id space is exhausted
    const std::vector<std::string>& paths() const { return paths_; }

private:
    std::unordered_map<std::string, uint16_t> ids_;
    std::vector<std::string> paths_;
};

struct ItemBakeInput {
    std::vector<DisplayInfoRow> displays;
    std::vector<HelmetVisDataRow> helmetVis;
    int helmetVisColumns = 8;
    std::vector<ItemRow> items;
    bool itemHasSheatheColumn = true;
    bool hasChrRaces = true;
    std::vector<RaceRow> races;
    std::unordered_set<uint32_t> itemVisualIds;
    bool withModels = true;  // false = --no-models
};

struct DisplayBake {
    uint32_t displayId = 0;
    std::string layerSource[2][8];  // [sex][section], "" = none
    std::string capeSource;         // "" = none (same for both sexes)
    SlotKind slotKind = SlotKind::kUnknown;
    FolderModels modelFiles;
    fmt::ItemDisplay header{};
    bool hasModels = false;
    fmt::ItemModels models{};
    fmt::ItemBodyInfo body{};
    std::vector<uint8_t> record;  // "i_<displayId>"
};

struct ItemBakePlan {
    std::vector<DisplayBake> displays;
    SourceIds layers;    // q_<n>
    SourceIds textures;  // t_<n>
    SourceIds models;    // a_<n>
    std::vector<ItemTableBlock> itemTable;
    ItemTableStatus itemTableStatus = ItemTableStatus::kOk;
    uint32_t badItemId = 0;
    std::vector<uint8_t> bodyRules;
    std::string warning;

    int exitCode() const { return itemTableStatus == ItemTableStatus::kOk ? 0 : kExitCodeBadItemRow; }
    // Sorted entry names this pass decides (the pak writer's own entries, e.g. its end marker, are not included).
    std::vector<std::string> entryNames() const;
};

ItemBakePlan planItemBake(const ItemBakeInput& in, const FileQuery& files, ModelNaming* naming);

}  // namespace wowee::pspconv::items
