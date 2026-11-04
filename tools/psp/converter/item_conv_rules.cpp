// Clean-room implementation (cleanroom/clean/equip) of the spec item_bake.md rules B1-B9 and the formats of its
// section 2.
#include "item_conv_rules.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace wowee::pspconv::items {

// ---------------------------------------------------------------------------------------------------------------
// Helpers

std::string toLowerAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

bool endsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    return toLowerAscii(s.substr(s.size() - n)) == toLowerAscii(suffix);
}

namespace {

inline uint16_t low16(uint32_t v) { return static_cast<uint16_t>(v & 0xFFFFu); }

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) {
        b.push_back(static_cast<uint8_t>(v & 0xFF));
        b.push_back(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void tag(const char* t) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(t[i]));
    }
};

struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    uint8_t u8() { return b[pos++]; }
    uint16_t u16() {
        const uint16_t v = static_cast<uint16_t>(b[pos] | (b[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(b[pos + i]) << (8 * i);
        pos += 4;
        return v;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// FileSet

std::string FileSet::key(const std::string& path) {
    std::string k = toLowerAscii(path);
    for (char& c : k) {
        if (c == '/') c = '\\';
    }
    return k;
}

void FileSet::add(const std::string& path) { paths_[key(path)] = path; }

bool FileSet::find(const std::string& path, std::string* actualPath) const {
    auto it = paths_.find(key(path));
    if (it == paths_.end()) return false;
    if (actualPath) *actualPath = it->second;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// B1

const char* const kRegionTextureDir[8] = {
    "ArmUpperTexture", "ArmLowerTexture", "HandTexture",    "TorsoUpperTexture",
    "TorsoLowerTexture", "LegUpperTexture", "LegLowerTexture", "FootTexture",
};

std::string bodyLayerSource(const std::string& textureName, int section, int sex, const FileQuery& files) {
    if (textureName.empty() || section < 0 || section >= 8) return std::string();
    const std::string dir = std::string("Item\\TextureComponents\\") + kRegionTextureDir[section] + "\\";
    const char* candidates[2] = {"_U.blp", sex != 0 ? "_F.blp" : "_M.blp"};
    for (const char* suffix : candidates) {
        std::string actual;
        if (files.find(dir + textureName + suffix, &actual)) return actual;
    }
    return std::string();
}

// ---------------------------------------------------------------------------------------------------------------
// B2

std::string capeTextureSource(const std::string& leftModelTexture, const FileQuery& files) {
    if (leftModelTexture.empty()) return std::string();
    std::string actual;
    if (files.find("Item\\ObjectComponents\\Cape\\" + leftModelTexture + ".blp", &actual)) return actual;
    return std::string();
}

// ---------------------------------------------------------------------------------------------------------------
// B3 / B4

HelmetVisIndex::HelmetVisIndex(const std::vector<HelmetVisDataRow>& rows) : rows_(rows) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        index_.emplace(rows_[i].id, i);  // first row of an id wins
    }
}

const HelmetVisDataRow* HelmetVisIndex::find(uint32_t id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &rows_[it->second];
}

uint8_t hidesHair(const DisplayInfoRow& row, int sex, const HelmetVisIndex& vis) {
    const uint32_t visId = row.helmetGeosetVis[sex != 0 ? 1 : 0];
    if (visId == 0) return 0;
    const HelmetVisDataRow* r = vis.find(visId);
    if (r == nullptr) return 0;
    for (uint32_t h : r->hide) {
        if (h != 0) return 1;
    }
    return 0;
}

fmt::ItemDisplay makeItemDisplay(const DisplayInfoRow& row, const HelmetVisIndex& vis, const uint16_t layer[2][8],
                                 const uint16_t cape[2]) {
    fmt::ItemDisplay d{};
    std::memcpy(d.magic, "WITM", 4);
    d.displayId = row.id;
    d.geosetGroup1 = low16(row.geosetGroup[0]);
    d.geosetGroup3 = low16(row.geosetGroup[2]);
    d.hidesHair[0] = hidesHair(row, 0, vis);
    d.hidesHair[1] = hidesHair(row, 1, vis);
    d.version = kItemRecordVersion;
    for (int sx = 0; sx < 2; ++sx) {
        for (int s = 0; s < 8; ++s) d.layer[sx][s] = layer[sx][s];
        d.capeTexture[sx] = cape[sx];
    }
    return d;
}

fmt::ItemBodyInfo makeBodyInfo(const DisplayInfoRow& row) {
    fmt::ItemBodyInfo b{};
    b.geosetGroup2 = low16(row.geosetGroup[1]);
    b.flags = low16(row.flags);
    const bool hasLeftModel = !row.leftModel.empty();
    b.helmetVis[0] = hasLeftModel ? low16(row.helmetGeosetVis[0]) : 0;
    b.helmetVis[1] = hasLeftModel ? low16(row.helmetGeosetVis[1]) : 0;
    return b;
}

// ---------------------------------------------------------------------------------------------------------------
// B5

std::vector<uint8_t> buildBodyRules(const std::vector<RaceRow>* races, const std::vector<HelmetVisDataRow>& helmetVis,
                                    int helmetVisColumns, std::string* warning) {
    uint32_t raceFlags[32] = {};
    if (races == nullptr) {
        if (warning) *warning = "ChrRaces.dbc not found: all race flags are 0";
    } else {
        bool seen[32] = {};
        for (const RaceRow& r : *races) {
            if (r.id >= 32 || seen[r.id]) continue;  // ids >= 32 ignored; first row of an id wins
            raceFlags[r.id] = r.flags;
            seen[r.id] = true;
        }
    }
    const int present = helmetVisColumns - 1;  // hide columns that exist (column 0 is the id)
    std::vector<fmt::HelmetVisRow> rows;
    rows.reserve(helmetVis.size());
    for (const HelmetVisDataRow& in : helmetVis) {
        fmt::HelmetVisRow r{};
        r.id = in.id;
        for (int k = 0; k < 7; ++k) r.hide[k] = k < present ? in.hide[k] : 0;
        rows.push_back(r);
    }
    return writeBodyRules(raceFlags, rows);
}

// ---------------------------------------------------------------------------------------------------------------
// B6

ItemTableStatus buildItemTable(const std::vector<ItemRow>& rows, bool hasSheatheColumn,
                               std::vector<ItemTableBlock>& out, uint32_t* badItemId) {
    out.clear();
    struct Block {
        uint32_t count = 0;
        std::vector<uint32_t> entry = std::vector<uint32_t>(kItemsPerBlock, 0);
        std::vector<uint8_t> sheath = std::vector<uint8_t>(kItemsPerBlock, 0);
    };
    std::map<uint32_t, Block> blocks;
    for (const ItemRow& r : rows) {
        if (r.id == 0 || r.displayInfoId == 0) continue;
        if (r.displayInfoId > 0xFFFFFFu) {
            if (badItemId) *badItemId = r.id;
            return ItemTableStatus::kDisplayIdTooLarge;
        }
        if (r.inventoryType > 255u) {
            if (badItemId) *badItemId = r.id;
            return ItemTableStatus::kInventoryTypeTooLarge;
        }
        const uint32_t block = r.id / kItemsPerBlock;
        const uint32_t k = r.id % kItemsPerBlock;
        Block& b = blocks[block];
        b.entry[k] = r.displayInfoId | (r.inventoryType << 24);
        b.sheath[k] = hasSheatheColumn ? static_cast<uint8_t>(std::min<uint32_t>(r.sheatheType, 254u)) : 0;
        b.count = std::max(b.count, k + 1);
    }
    for (const auto& kv : blocks) {
        ItemTableBlock tb;
        tb.block = kv.first;
        tb.bytes = writeItemTableBlock(kv.first * kItemsPerBlock, kv.second.count, true, kv.second.entry.data(),
                                       kv.second.sheath.data());
        out.push_back(std::move(tb));
    }
    return ItemTableStatus::kOk;
}

// ---------------------------------------------------------------------------------------------------------------
// B7

bool slotKindOfInventoryType(uint32_t inventoryType, SlotKind& kind) {
    switch (inventoryType) {
        case 1: kind = SlotKind::kHead; return true;
        case 3: kind = SlotKind::kShoulder; return true;
        case 13: case 14: case 15: case 17: case 21: case 22: case 23: case 25: case 26:
            kind = SlotKind::kHand;
            return true;
        default: return false;
    }
}

SlotKind displaySlotKind(const std::vector<uint32_t>& inventoryTypes) {
    bool any = false;
    SlotKind agreed = SlotKind::kUnknown;
    for (uint32_t t : inventoryTypes) {
        SlotKind k;
        if (!slotKindOfInventoryType(t, k)) continue;
        if (!any) {
            agreed = k;
            any = true;
        } else if (k != agreed) {
            return SlotKind::kUnknown;
        }
    }
    return any ? agreed : SlotKind::kUnknown;
}

const char* modelFolderName(ModelFolder folder) {
    switch (folder) {
        case ModelFolder::kHead: return "Head";
        case ModelFolder::kShoulder: return "Shoulder";
        case ModelFolder::kWeapon: return "Weapon";
        case ModelFolder::kShield: return "Shield";
    }
    return "";
}

int modelFolderOrder(SlotKind kind, ModelFolder out[4]) {
    switch (kind) {
        case SlotKind::kHead: out[0] = ModelFolder::kHead; return 1;
        case SlotKind::kShoulder: out[0] = ModelFolder::kShoulder; return 1;
        case SlotKind::kHand:
            out[0] = ModelFolder::kWeapon;
            out[1] = ModelFolder::kShield;
            return 2;
        case SlotKind::kUnknown:
        default:
            out[0] = ModelFolder::kHead;
            out[1] = ModelFolder::kShoulder;
            out[2] = ModelFolder::kWeapon;
            out[3] = ModelFolder::kShield;
            return 4;
    }
}

uint8_t modelKindOf(ModelFolder folder) {
    switch (folder) {
        case ModelFolder::kHead: return fmt::kItemModelHelm;
        case ModelFolder::kShoulder: return fmt::kItemModelShoulder;
        case ModelFolder::kWeapon: return fmt::kItemModelWeapon;
        case ModelFolder::kShield: return fmt::kItemModelShield;
    }
    return fmt::kItemModelNone;
}

bool FolderModels::anyModel() const {
    for (int side = 0; side < 2; ++side) {
        for (int v = 0; v < fmt::kItemVariants; ++v) {
            if (!model[side][v].empty()) return true;
        }
    }
    return false;
}

uint8_t chooseItemModels(SlotKind kind, const DisplayInfoRow& row, ModelNaming& naming, FolderModels& out) {
    ModelFolder order[4];
    const int n = modelFolderOrder(kind, order);
    for (int i = 0; i < n; ++i) {
        FolderModels found;
        if (naming.tryFolder(order[i], row, found) && found.anyModel()) {
            out = found;
            return modelKindOf(order[i]);
        }
    }
    out = FolderModels{};
    return fmt::kItemModelNone;
}

// ---------------------------------------------------------------------------------------------------------------
// B8 / B9

uint8_t sheathHint(const std::vector<uint32_t>& sheatheTypes) {
    uint32_t votes[255] = {};
    bool any = false;
    for (uint32_t t : sheatheTypes) {
        if (t > 254u) continue;
        ++votes[t];
        any = true;
    }
    if (!any) return kSheathHintUnknown;
    uint32_t best = 0;
    for (uint32_t t = 1; t < 255; ++t) {
        if (votes[t] > votes[best]) best = t;  // strict: ties keep the smaller value
    }
    return static_cast<uint8_t>(best);
}

uint16_t ownItemVisual(const DisplayInfoRow& row, const std::unordered_set<uint32_t>& itemVisualIds) {
    if (!row.hasItemVisualColumn) return 0;
    const uint32_t v = row.itemVisual;
    if (v == 0 || v > 65535u) return 0;
    return itemVisualIds.count(v) != 0 ? static_cast<uint16_t>(v) : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Writers / readers

std::vector<uint8_t> writeItemRecord(const fmt::ItemDisplay& display, const fmt::ItemModels* models,
                                     const fmt::ItemBodyInfo& body) {
    Writer w;
    w.b.reserve(models ? fmt::kItemRecordSizeModels : fmt::kItemRecordSizeNoModels);
    w.tag("WITM");
    w.u32(display.displayId);
    w.u16(display.geosetGroup1);
    w.u16(display.geosetGroup3);
    w.u8(display.hidesHair[0]);
    w.u8(display.hidesHair[1]);
    w.u16(display.version);
    for (int sx = 0; sx < 2; ++sx) {
        for (int s = 0; s < 8; ++s) w.u16(display.layer[sx][s]);
    }
    w.u16(display.capeTexture[0]);
    w.u16(display.capeTexture[1]);
    if (models != nullptr) {
        w.u8(models->kind);
        w.u8(models->sheathHint);
        w.u16(models->itemVisual);
        for (int side = 0; side < 2; ++side) {
            for (int v = 0; v < fmt::kItemVariants; ++v) w.u16(models->model[side][v]);
        }
        for (int side = 0; side < 2; ++side) {
            for (int v = 0; v < fmt::kItemVariants; ++v) w.u16(models->texture[side][v]);
        }
    }
    w.u16(body.geosetGroup2);
    w.u16(body.flags);
    w.u16(body.helmetVis[0]);
    w.u16(body.helmetVis[1]);
    return w.b;
}

bool readItemRecord(const std::vector<uint8_t>& bytes, fmt::ItemDisplay& display, bool& hasModels,
                    fmt::ItemModels& models, fmt::ItemBodyInfo& body) {
    if (bytes.size() != fmt::kItemRecordSizeNoModels && bytes.size() != fmt::kItemRecordSizeModels) return false;
    if (std::memcmp(bytes.data(), "WITM", 4) != 0) return false;
    Reader r{bytes};
    display = fmt::ItemDisplay{};
    for (int i = 0; i < 4; ++i) display.magic[i] = static_cast<char>(r.u8());
    display.displayId = r.u32();
    display.geosetGroup1 = r.u16();
    display.geosetGroup3 = r.u16();
    display.hidesHair[0] = r.u8();
    display.hidesHair[1] = r.u8();
    display.version = r.u16();
    for (int sx = 0; sx < 2; ++sx) {
        for (int s = 0; s < 8; ++s) display.layer[sx][s] = r.u16();
    }
    display.capeTexture[0] = r.u16();
    display.capeTexture[1] = r.u16();
    hasModels = bytes.size() == fmt::kItemRecordSizeModels;
    models = fmt::ItemModels{};
    if (hasModels) {
        models.kind = r.u8();
        models.sheathHint = r.u8();
        models.itemVisual = r.u16();
        for (int side = 0; side < 2; ++side) {
            for (int v = 0; v < fmt::kItemVariants; ++v) models.model[side][v] = r.u16();
        }
        for (int side = 0; side < 2; ++side) {
            for (int v = 0; v < fmt::kItemVariants; ++v) models.texture[side][v] = r.u16();
        }
    }
    body.geosetGroup2 = r.u16();
    body.flags = r.u16();
    body.helmetVis[0] = r.u16();
    body.helmetVis[1] = r.u16();
    return true;
}

std::vector<uint8_t> writeItemTableBlock(uint32_t firstItem, uint32_t count, bool withSheath, const uint32_t* entries,
                                         const uint8_t* sheath) {
    Writer w;
    w.b.reserve(16 + size_t(count) * (withSheath ? 5 : 4));
    w.tag("WITB");
    w.u32(firstItem);
    w.u32(count);
    w.u32(withSheath ? 1u : 0u);
    for (uint32_t i = 0; i < count; ++i) w.u32(entries[i]);
    if (withSheath) {
        for (uint32_t i = 0; i < count; ++i) w.u8(sheath[i]);
    }
    return w.b;
}

std::vector<uint8_t> writeBodyRules(const uint32_t raceFlags[32], const std::vector<fmt::HelmetVisRow>& rows) {
    Writer w;
    w.b.reserve(sizeof(fmt::BodyRulesHeader) + rows.size() * sizeof(fmt::HelmetVisRow));
    w.tag("WBRL");
    w.u16(1);
    w.u16(static_cast<uint16_t>(rows.size()));
    for (int r = 0; r < 32; ++r) w.u32(raceFlags[r]);
    for (const fmt::HelmetVisRow& row : rows) {
        w.u32(row.id);
        for (int k = 0; k < 7; ++k) w.u32(row.hide[k]);
    }
    return w.b;
}

// ---------------------------------------------------------------------------------------------------------------
// Whole pass

uint16_t SourceIds::idOf(const std::string& path) {
    const std::string k = FileSet::key(path);
    auto it = ids_.find(k);
    if (it != ids_.end()) return it->second;
    if (paths_.size() >= fmt::kNoItemLayer) return fmt::kNoItemLayer;
    const uint16_t id = static_cast<uint16_t>(paths_.size());
    ids_.emplace(k, id);
    paths_.push_back(path);
    return id;
}

std::vector<std::string> ItemBakePlan::entryNames() const {
    std::vector<std::string> names;
    names.push_back("b_rules");
    for (const DisplayBake& d : displays) names.push_back("i_" + std::to_string(d.displayId));
    for (size_t i = 0; i < layers.paths().size(); ++i) names.push_back("q_" + std::to_string(i));
    for (size_t i = 0; i < textures.paths().size(); ++i) names.push_back("t_" + std::to_string(i));
    for (size_t i = 0; i < models.paths().size(); ++i) names.push_back("a_" + std::to_string(i));
    for (const ItemTableBlock& b : itemTable) names.push_back(b.name());
    std::sort(names.begin(), names.end());
    return names;
}

ItemBakePlan planItemBake(const ItemBakeInput& in, const FileQuery& files, ModelNaming* naming) {
    ItemBakePlan plan;
    const HelmetVisIndex vis(in.helmetVis);

    // Item.dbc rows by display (B7, B8: every row of the display).
    std::unordered_map<uint32_t, std::vector<const ItemRow*>> rowsOfDisplay;
    for (const ItemRow& r : in.items) rowsOfDisplay[r.displayInfoId].push_back(&r);

    for (const DisplayInfoRow& row : in.displays) {
        DisplayBake d;
        d.displayId = row.id;

        // B1
        uint16_t layer[2][8];
        for (int sx = 0; sx < 2; ++sx) {
            for (int s = 0; s < 8; ++s) {
                d.layerSource[sx][s] = bodyLayerSource(row.texture[s], s, sx, files);
                layer[sx][s] = d.layerSource[sx][s].empty() ? fmt::kNoItemLayer : plan.layers.idOf(d.layerSource[sx][s]);
            }
        }
        // B2
        d.capeSource = capeTextureSource(row.leftModelTexture, files);
        const uint16_t capeId = d.capeSource.empty() ? fmt::kNoItemLayer : plan.textures.idOf(d.capeSource);
        const uint16_t cape[2] = {capeId, capeId};
        // B3, B4
        d.header = makeItemDisplay(row, vis, layer, cape);
        d.body = makeBodyInfo(row);

        // B7-B9 (records with models only)
        if (in.withModels && naming != nullptr) {
            std::vector<uint32_t> invTypes;
            std::vector<uint32_t> sheathTypes;
            auto it = rowsOfDisplay.find(row.id);
            if (it != rowsOfDisplay.end()) {
                for (const ItemRow* r : it->second) {
                    invTypes.push_back(r->inventoryType);
                    if (in.itemHasSheatheColumn) sheathTypes.push_back(r->sheatheType);
                }
            }
            d.slotKind = displaySlotKind(invTypes);
            const uint8_t kind = chooseItemModels(d.slotKind, row, *naming, d.modelFiles);
            if (kind != fmt::kItemModelNone) {
                d.hasModels = true;
                d.models.kind = kind;
                d.models.sheathHint = sheathHint(sheathTypes);
                d.models.itemVisual = ownItemVisual(row, in.itemVisualIds);
                for (int side = 0; side < 2; ++side) {
                    for (int v = 0; v < fmt::kItemVariants; ++v) {
                        const std::string& m = d.modelFiles.model[side][v];
                        const std::string& t = d.modelFiles.texture[side][v];
                        d.models.model[side][v] = m.empty() ? fmt::kNoItemLayer : plan.models.idOf(m);
                        d.models.texture[side][v] = t.empty() ? fmt::kNoItemLayer : plan.textures.idOf(t);
                    }
                }
            }
        }
        d.record = writeItemRecord(d.header, d.hasModels ? &d.models : nullptr, d.body);
        plan.displays.push_back(std::move(d));
    }

    // B6
    plan.itemTableStatus = buildItemTable(in.items, in.itemHasSheatheColumn, plan.itemTable, &plan.badItemId);
    // B5
    plan.bodyRules = buildBodyRules(in.hasChrRaces ? &in.races : nullptr, in.helmetVis, in.helmetVisColumns,
                                    &plan.warning);
    return plan;
}

}  // namespace wowee::pspconv::items
