// psp_item_conv - bakes equipment appearance for the PSP characters (M10c).
//
//   psp_item_conv --extracted <dir> --out <items.pak> [--start-outfits] [--tex-max 128]
//                 [--no-models] [--verify] [--dump] [--step 128]
//
// Which ItemDisplayInfo ids - every row of
// ItemDisplayInfo.dbc, from the game data alone (the list from the server
// DB, tools/psp/item_displays.py + --displays, is gone: community servers).
// --start-outfits bakes only the CharStartOutfit.dbc displays (the starter
// clothes of every race/class/sex) - a small set for checks. The pak is written
// sorted with the "~fence" record (conv_common.hpp SortedPakWriter, streamed
// through a temp file) for the PSP's PakIndex, plus the Item.dbc table
// "u_<block>" (item id -> display id + inventory type: other players' visible
// items, NPC virtual items; psp_item_format.hpp ItemTableHeader).
// Per id (include/rendering/psp/psp_item_format.hpp):
//   - GeosetGroup1/3 and hides-hair per sex (upstream helmHidesHair:
//     HelmetGeosetVis columns 13/14 -> any non-zero HelmetGeosetVisData mask);
//   - per sex and region the texture resolved like upstream
//     resolveItemRegionTexture (_F/_M, then _U, then plain), resampled to
//     half the upstream region rectangle and stored as a WLAY layer placed by
//     region index (not by file name);
//   - per sex the cape texture (upstream setOnlinePlayerEquipment "Cape
//     texture": female Right then Left, male Left then Right, each through
//     pipeline::capeTextureCandidates), as a WTEX "t_<n>" (<= --tex-max).
//     Baked for any display whose names resolve under the Cape folders; the
//     PSP uses it only for a worn back item (inventory type 16), as upstream.
//   - the item models of helmets, shoulders, weapons and shields
//     ("a_<n>" WMDL, Stand frame 0) with their skins ("t_<n>", the cape
//     bank), per race/sex where upstream picks a racial model
//     (psp_item_format.hpp ItemModels). --no-models skips them (M10c pak),
//     --dump prints the models per display.
// --verify: the written pak read back the way the PSP reads
// it - the TOC order and fence, every record found through pakIndexLookup
// (psp_pak_index.hpp, the PSP's own lookup), every layer / texture / model id a
// record names present and valid, every Item.dbc row in the table. Exit 3 on
// errors, 4 when the ids pass the format's u16 ranges.
// Weapons (weapon_fx_bake.hpp, psp_item_format.hpp's block): the Item.dbc SheatheType
// per item (the "u_" tail), per display the sheath hint and its own ItemVisual (record version 3), the weapon /
// shield models' attachments 0..4 (an "a_" footer) and the item visuals of the enchantments and displays
// ("ivis", effects "ifx_<n>", textures "ift_<n>"; --dump lists the effects).
//
// Region textures, the cloak texture, the body tail and "b_rules" follow the
// item_bake spec (clean-room implementation, item_conv_rules.hpp: B1-B9); every record is WITM version 4 with the
// body tail (GeosetGroup[1], Flags, HelmetGeosetVis ids), and "b_rules" holds ChrRaces Flags +
// HelmetGeosetVisData. --test-displays <id,...> adds displays to --start-outfits for a small test pak.

#include "core/helm_visual.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/item_textures.hpp"
#include "platform/psp/psp_pak_index.hpp"
#include "rendering/psp/psp_item_format.hpp"
#include "rendering/psp/psp_item_npc_race.hpp"  // NPC race helms
#include "rendering/psp/psp_model_format.hpp"

#include "conv_common.hpp"
#include "item_conv_rules.hpp"  // clean room: item_bake.md B1-B9
#include "model_bake.hpp"
#include "weapon_fx_bake.hpp"  // sheath types, weapon visual points, item visuals ("ivis")

#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_set>

#if defined(_WIN32)  // FileIndex lists the folders with FindFirstFileEx
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace wowee;
using namespace wowee::pspconv;
namespace pp = wowee::platform::psp;
namespace {

constexpr uint32_t kKindItem = 9;
constexpr uint32_t kKindLayer = 8;
constexpr uint32_t kKindItemTable = 3;  // raw ("u_<block>")

// The u16 id ranges of the format. Layers and models stop
// below their "none" value; textures below kTexSlot (0xFFF0), because a model
// batch's texture shares its u16 with kTexItemSkin and the slot values.
constexpr size_t kMaxLayerIds = fmt::kNoItemLayer;     // q_0 .. q_65534
constexpr size_t kMaxModelIds = fmt::kNoItemModel;     // a_0 .. a_65534
constexpr size_t kMaxTextureIds = fmt::kTexSlot;       // t_0 .. t_65519

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Every file under <root>/item in one set, read once - a full
// bake asks for ~2 million paths (21 race/sex variants per helm and shoulder,
// three folders per weapon); asking the file system each time took longer
// than the bake. On Windows the listing is FindFirstFileEx (std::filesystem's
// iterator stats every entry under MinGW: 25+ min for 48 000 files on a busy
// disk, against ~1.5 min).
class FileIndex {
public:
    // `fallback` (--mirror): root is a copy of <extracted>/item on a faster disk; a file only the real tree has
    // (missing from the copy, or outside item/: an item model's texture under Spells\ etc.) is copied into
    // the copy on first use.
    explicit FileIndex(std::string root, std::string fallback = {})
        : root_(std::move(root)), fallback_(std::move(fallback)) {}
    size_t scan(const char* sub) {
        const std::string prefix = lower(std::string(sub)) + "/";
#if defined(_WIN32)
        scanWin(root_ + "/" + sub, prefix, files_);
        if (!fallback_.empty()) {
            scanWin(fallback_ + "/" + sub, prefix, fallbackFiles_);
            for (const std::string& f : files_) fallbackFiles_.erase(f);
        }
#else
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::path(root_) / sub;
        std::string rootGeneric = std::filesystem::path(root_).generic_string();
        if (rootGeneric.empty() || rootGeneric.back() != '/') rootGeneric += '/';
        for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string p = it->path().generic_string();
            if (p.size() <= rootGeneric.size() || p.compare(0, rootGeneric.size(), rootGeneric) != 0) continue;
            files_.insert(lower(p.substr(rootGeneric.size())));
        }
#endif
        scanned_.push_back(prefix);
        return files_.size();
    }
    // Files under a scanned folder only the real tree has (--mirror incomplete).
    size_t fallbackOnly() const { return fallbackFiles_.size(); }
    bool exists(const std::string& gamePath) const {
        const std::string l = lower(gamePath);
        for (const std::string& s : scanned_) {
            if (l.compare(0, s.size(), s) != 0) continue;
            if (files_.count(l)) return true;
            if (!fallbackFiles_.count(l) || !copyIn(l)) return false;
            files_.insert(l);
            return true;
        }
        std::error_code ec;
        if (std::filesystem::exists(root_ + "/" + l, ec)) return true;
        return !fallback_.empty() && std::filesystem::exists(fallback_ + "/" + l, ec) && copyIn(l);
    }

private:
    bool copyIn(const std::string& l) const {
        std::error_code ec;
        const std::filesystem::path dst = std::filesystem::path(root_) / l;
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::filesystem::copy_file(std::filesystem::path(fallback_) / l, dst,
                                   std::filesystem::copy_options::skip_existing, ec);
        return !ec;
    }
#if defined(_WIN32)
    // Names in the client data are ASCII; `rel` is the lower-case path from the root with '/'.
    static void scanWin(const std::string& dir, const std::string& rel, std::unordered_set<std::string>& out) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileExA((dir + "\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            const std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scanWin(dir + "\\" + name, rel + lower(name) + "/", out);
            else out.insert(rel + lower(name));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif
    std::string root_, fallback_;
    mutable std::unordered_set<std::string> files_;  // grows when a file is copied in from the fallback
    std::unordered_set<std::string> fallbackFiles_;
    std::vector<std::string> scanned_;
};

class LayerBank {
public:
    LayerBank(std::string root, std::vector<PakItem>& out) : root_(std::move(root)), out_(out) {}

    uint16_t get(const std::string& key, int region) {
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second;
        uint16_t id = fmt::kNoItemLayer;
        std::vector<uint8_t> bytes;
        if (count_ >= kMaxLayerIds) {
            overflow_ = true;  // never wraps into kNoItemLayer
        } else if (readFile(root_ + "/" + key, bytes)) {
            const pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
            if (!img.data.empty()) {
                const auto* r = fmt::kItemRegionRect256[region];
                const int w = r[2] / 2, h = r[3] / 2;
                const std::vector<Rgb> px = resample(img, w, h);
                std::vector<uint8_t> idx;
                std::array<Rgb, 256> pal;
                quantize(px, idx, pal);
                fmt::LayerHeader lh{};
                std::memcpy(lh.magic, "WLAY", 4);
                lh.w = static_cast<uint16_t>(w);
                lh.h = static_cast<uint16_t>(h);
                lh.x = static_cast<uint16_t>(r[0] / 2);
                lh.y = static_cast<uint16_t>(r[1] / 2);
                std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&lh),
                                          reinterpret_cast<const uint8_t*>(&lh + 1));
                for (const Rgb& p : pal) put<uint32_t>(blob, to8888(p));
                blob.insert(blob.end(), idx.begin(), idx.end());
                id = static_cast<uint16_t>(count_++);
                bytes_ += blob.size();
                out_.push_back({"q_" + std::to_string(id), kKindLayer, std::move(blob)});
            }
        }
        ids_.emplace(key, id);
        return id;
    }
    size_t count() const { return count_; }
    size_t bytes() const { return bytes_; }
    bool overflow() const { return overflow_; }

private:
    std::string root_;
    std::vector<PakItem>& out_;
    std::map<std::string, uint16_t> ids_;
    size_t count_ = 0, bytes_ = 0;
    bool overflow_ = false;
};

bool loadDbc(const std::string& root, const char* name, pipeline::DBCFile& dbc) {
    std::vector<uint8_t> bytes;
    return readFile(root + "/dbfilesclient/" + name, bytes) && dbc.load(bytes);
}

// ---- item models ------------------------------------------------------------

constexpr uint32_t kKindItemModel = kKindModel;

std::string stripExtension(std::string name) {
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
}

// Item M2 -> "a_<n>" WMDL. Keyed by path and whether the display gives it a
// skin: upstream CharacterRenderer::attachWeapon puts the skin on the
// replaceable slots (types 2/3/4, else slot 0) and resolveBatchTexture then
// takes the first non-white texture of each batch - the same model resolves
// differently with and without a skin.
class ItemModelBank {
public:
    ItemModelBank(std::string root, const FileIndex& files, TextureBank& tex, std::vector<PakItem>& out)
        : root_(std::move(root)), files_(files), tex_(tex), out_(out) {}

    // -1: no such file (asked of the file index first - the
    // probes of absent race cuts are not remembered), or a model that does not
    // bake.
    int get(const std::string& gamePath, bool withSkin) {
        if (!files_.exists(gamePath)) return -1;
        const std::string key = lower(gamePath) + (withSkin ? "|skin" : "|plain");
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second;
        const int id = bake(gamePath, withSkin);
        ids_.emplace(key, id);
        if (id < 0) failed_.insert(lower(gamePath));
        return id;
    }
    size_t count() const { return count_; }
    size_t bytes() const { return bytes_; }
    size_t triangles() const { return tris_; }
    bool overflow() const { return overflow_; }

    // True when every batch that draws the display's skin on this model (the
    // replaceable slots 2/3/4, else slot 0 - bake()'s withSkin rule) is opaque (M2 blend mode 0: drawn without
    // alpha test) - the skin texture is then baked opaque (TextureBank::get's `opaque`).
    // False for a model without such a batch or that does not load. Cached per file.
    bool skinOpaque(const std::string& gamePath) {
        if (!files_.exists(gamePath)) return false;
        const std::string file = lower(gamePath);
        auto it = skinOpaque_.find(file);
        if (it != skinOpaque_.end()) return it->second;
        bool result = false;
        std::vector<uint8_t> bytes;
        pipeline::M2Model m;
        if (readFile(root_ + "/" + file, bytes) && loadM2(bytes, root_ + "/" + lower(pipeline::skinPathForM2(gamePath)), m)) {
            std::vector<uint8_t> skinSlot(m.textures.size(), 0), white(m.textures.size(), 1);
            bool replaceable = false;
            for (size_t k = 0; k < m.textures.size(); ++k) {
                const auto& t = m.textures[k];
                if (t.type == 2 || t.type == 3 || t.type == 4) {
                    skinSlot[k] = 1;
                    replaceable = true;
                }
                if (t.type == 0 && !std::string(t.filename.c_str()).empty() && files_.exists(t.filename.c_str()))
                    white[k] = 0;
            }
            if (!replaceable && !skinSlot.empty()) skinSlot[0] = 1;
            bool any = false, alpha = false;
            for (const auto& b : m.batches) {
                if (b.submeshLevel != 0 || b.indexCount < 3 || b.indexStart + b.indexCount > m.indices.size() ||
                    b.textureIndex == 0xFFFF)
                    continue;
                const uint16_t grp = b.submeshId / 100;
                if (grp == 15 || grp == 17 || grp == 18) continue;
                const uint32_t combos = std::min<uint32_t>(b.textureCount ? b.textureCount : 1u, 8u);
                for (uint32_t i = 0; i < combos; ++i) {
                    const uint32_t pos = static_cast<uint32_t>(b.textureIndex) + i;
                    if (pos >= m.textureLookup.size()) break;
                    const uint16_t slot = m.textureLookup[pos];
                    if (slot >= m.textures.size()) continue;
                    if (!skinSlot[slot] && white[slot]) continue;
                    if (skinSlot[slot]) {
                        any = true;
                        const uint16_t blendMode =
                            b.materialIndex < m.materials.size() ? m.materials[b.materialIndex].blendMode : 0;
                        if (mapBlend(blendMode) != fmt::kBlendOpaque) alpha = true;
                    }
                    break;
                }
            }
            result = any && !alpha;
        }
        skinOpaque_.emplace(file, result);
        return result;
    }
    const std::set<std::string>& failed() const { return failed_; }
    const std::string& pathOf(int id) const { return paths_[static_cast<size_t>(id)]; }

private:
    int bake(const std::string& gamePath, bool withSkin) {
        if (count_ >= kMaxModelIds) {
            overflow_ = true;
            return -1;
        }
        std::vector<uint8_t> bytes;
        pipeline::M2Model m;
        const std::string file = lower(gamePath);
        if (!readFile(root_ + "/" + file, bytes) ||
            !loadM2(bytes, root_ + "/" + lower(pipeline::skinPathForM2(gamePath)), m))
            return -1;
        // Items are static on the PSP: the Stand frame 0 (bind pose when the
        // model has no Stand).
        poseStand(root_, gamePath, bytes, m);
        // Texture per M2 slot (upstream loadModel: only type 0 names a file).
        // A slot's file is baked when a batch first picks it
        // (kPending) - a texture under the skin or in no batch's combo was baked
        // for nothing before (unreferenced "t_" in the pak). Same result: a
        // file that does not load is white and the combo goes on.
        constexpr int kWhite = -1, kSkin = -2, kPending = -3;
        std::vector<int> slotTex(m.textures.size(), kWhite);
        std::vector<std::string> slotName(m.textures.size());
        for (size_t s = 0; s < m.textures.size(); ++s) {
            const auto& t = m.textures[s];
            const std::string name = t.filename.c_str();
            if (t.type == 0 && !name.empty() && files_.exists(name)) {
                slotTex[s] = kPending;
                slotName[s] = name;
            }
        }
        if (withSkin) {
            bool replaceable = false;
            for (size_t s = 0; s < m.textures.size(); ++s)
                if (m.textures[s].type == 2 || m.textures[s].type == 3 || m.textures[s].type == 4) {
                    slotTex[s] = kSkin;
                    replaceable = true;
                }
            if (!replaceable && !slotTex.empty()) slotTex[0] = kSkin;
        }
        // A file only opaque batches draw (M2 blend mode 0 - the PSP draws it
        // without alpha test) is baked opaque (TextureBank::get's `opaque`, as psp_model_conv's
        // addBatches); a file also drawn with alpha keeps the plain texture for all its batches
        // (one texture in RAM). The batches' picks as below, a file that is there counting as not white.
        std::map<std::string, uint8_t> fileUse;  // lower file -> 1: an opaque batch draws it, 2: another one
        for (const auto& b : m.batches) {
            if (b.submeshLevel != 0 || b.indexCount < 3 || b.indexStart + b.indexCount > m.indices.size()) continue;
            const uint16_t grp = b.submeshId / 100;
            if (grp == 15 || grp == 17 || grp == 18 || b.textureIndex == 0xFFFF) continue;
            const uint16_t blendMode = b.materialIndex < m.materials.size() ? m.materials[b.materialIndex].blendMode : 0;
            const uint32_t combos = std::min<uint32_t>(b.textureCount ? b.textureCount : 1u, 8u);
            for (uint32_t i = 0; i < combos; ++i) {
                const uint32_t pos = static_cast<uint32_t>(b.textureIndex) + i;
                if (pos >= m.textureLookup.size()) break;
                const uint16_t slot = m.textureLookup[pos];
                if (slot >= slotTex.size() || slotTex[slot] == kWhite) continue;
                if (slotTex[slot] == kPending)
                    fileUse[lower(slotName[slot])] |= mapBlend(blendMode) == fmt::kBlendOpaque ? 1 : 2;
                break;
            }
        }
        WmdlBuilder o;
        for (const auto& b : m.batches) {
            if (b.submeshLevel != 0 || b.indexCount < 3 || b.indexStart + b.indexCount > m.indices.size()) continue;
            // upstream: without a geoset filter, groups 15/17/18 are not drawn.
            const uint16_t grp = b.submeshId / 100;
            if (grp == 15 || grp == 17 || grp == 18) continue;
            uint16_t blendMode = 0, matFlags = 0;
            if (b.materialIndex < m.materials.size()) {
                blendMode = m.materials[b.materialIndex].blendMode;
                matFlags = m.materials[b.materialIndex].flags;
            }
            // upstream resolveBatchTexture: hair (type 6) wins, else the first
            // non-white texture of the batch's combo, else white.
            fmt::ModelBatch ob{};
            ob.texture = fmt::kNoTexture;
            uint32_t texFlags = 0;
            if (b.textureIndex != 0xFFFF) {
                const uint32_t combos = std::min<uint32_t>(b.textureCount ? b.textureCount : 1u, 8u);
                bool picked = false;
                for (uint32_t i = 0; i < combos && !picked; ++i) {
                    const uint32_t pos = static_cast<uint32_t>(b.textureIndex) + i;
                    if (pos >= m.textureLookup.size()) break;
                    const uint16_t slot = m.textureLookup[pos];
                    if (slot >= slotTex.size()) continue;
                    if (slotTex[slot] == kPending) {
                        bool alpha = false;
                        const auto use = fileUse.find(lower(slotName[slot]));
                        const bool opaque = use != fileUse.end() && use->second == 1;  // the opaque rule
                        const int id = tex_.get(slotName[slot], alpha, opaque);
                        slotTex[slot] = id >= 0 ? id : kWhite;
                    }
                    const int pick = slotTex[slot];
                    if (pick == kWhite) continue;
                    ob.texture = pick == kSkin ? fmt::kTexItemSkin : static_cast<uint16_t>(pick);
                    texFlags = m.textures[slot].flags;
                    picked = true;
                }
            }
            ob.blend = mapBlend(blendMode);
            ob.flags = static_cast<uint8_t>(((texFlags & 1) ? fmt::kBatchWrapU : 0) |
                                            ((texFlags & 2) ? fmt::kBatchWrapV : 0) |
                                            ((matFlags & 1) ? fmt::kBatchUnlit : 0));
            if (!o.add(m.indices, b.indexStart, (b.indexCount / 3) * 3, 0, 0, ob)) break;
        }
        if (o.empty()) return -1;
        WmdlInfo info;
        std::vector<uint8_t> blob = o.build(m.vertices, 0, false, info);
        if (!fmt::itemModelValid(blob.data(), static_cast<uint32_t>(blob.size()))) {
            std::fprintf(stderr, "  item model %s: invalid WMDL\n", file.c_str());
            return -1;
        }
        // A weapon / shield carries where an item visual's effects sit on it
        // (its attachments 0..4) as a footer the WMDL readers do not see.
        if (itemModelTakesVisuals(gamePath)) {
            const std::vector<uint8_t> footer = itemVisualPointsFooter(m);
            blob.insert(blob.end(), footer.begin(), footer.end());
        }
        const int id = static_cast<int>(count_++);
        bytes_ += blob.size();
        tris_ += info.triangles;
        paths_.push_back(file + (withSkin ? " (skin)" : ""));
        out_.push_back({"a_" + std::to_string(id), kKindItemModel, std::move(blob)});
        return id;
    }

    std::string root_;
    const FileIndex& files_;
    TextureBank& tex_;
    std::vector<PakItem>& out_;
    std::map<std::string, int> ids_;
    std::map<std::string, bool> skinOpaque_;  // skins drawn opaque only
    std::set<std::string> failed_;
    std::vector<std::string> paths_;
    size_t count_ = 0, bytes_ = 0, tris_ = 0;
    bool overflow_ = false;
};

// Race/sex of each ItemModels column (psp_item_format.hpp itemVariant).
void variantRaceSex(int v, uint8_t& race, uint8_t& sex) {
    constexpr uint8_t kRaces[10] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    race = v < 20 ? kRaces[v / 2] : 0;
    sex = static_cast<uint8_t>(v < 20 ? v % 2 : 0);
}

// The models of one ItemDisplayInfo row (psp_item_format.hpp). Which folders are tried, in which order, and the
// resulting kind: item_bake.md B7 (item_conv_rules.hpp chooseItemModels, by the slot kind of the display's items -
// weapon_fx_bake.hpp displaySlotKinds). The naming inside one folder is upstream's (tryFolder below): helm /
// shoulder racial model first, then the base one, the texture racial if the file exists, else the base name
// (core::resolveHelmVisual, setOnlinePlayerEquipment shoulders); weapons and shields pipeline::readItemDisplayArt
// (the left pair, else the right), texture Weapon\<tex>.blp, else Shield\<tex>.blp. A texture that does not load
// leaves the model without a skin (upstream: loadTexture gives white, no override). A folder yields when a model of
// it bakes; its "a_" / "t_" ids are then those of yielded().
class ItemNaming : public items::ModelNaming {
public:
    ItemNaming(const FileIndex& files, ItemModelBank& models, TextureBank& tex)
        : files_(files), models_(models), tex_(tex) {}

    bool tryFolder(items::ModelFolder folder, const items::DisplayInfoRow& row, items::FolderModels& out) override {
        current_ = emptyModels();
        out = items::FolderModels{};
        bool any = false;
        switch (folder) {
            case items::ModelFolder::kHead:
                fileOpaque_.clear();
                planSkin("Item\\ObjectComponents\\Head\\", row.leftModel, row.leftModelTexture);  // the opaque rule
                any = racial("Item\\ObjectComponents\\Head\\", row.leftModel, row.leftModelTexture, 0, out);
                break;
            case items::ModelFolder::kShoulder: {
                fileOpaque_.clear();  // the opaque rule: the shoulders, both sides together
                planSkin("Item\\ObjectComponents\\Shoulder\\", row.leftModel, row.leftModelTexture);
                planSkin("Item\\ObjectComponents\\Shoulder\\", row.rightModel, row.rightModelTexture);
                const bool l = racial("Item\\ObjectComponents\\Shoulder\\", row.leftModel, row.leftModelTexture, 0, out);
                const bool rr = racial("Item\\ObjectComponents\\Shoulder\\", row.rightModel, row.rightModelTexture, 1, out);
                any = l || rr;
                break;
            }
            case items::ModelFolder::kWeapon:
            case items::ModelFolder::kShield:
                any = weaponOrShield(folder == items::ModelFolder::kWeapon, row, out);
                break;
        }
        if (any) yielded_ = current_;
        return any;
    }
    // The ids of the folder that yielded last (chooseItemModels stops at the first one that yields).
    const fmt::ItemModels& yielded() const { return yielded_; }

    // No folder yielded: whether the row names a model that is in none of the folders tried (statistics, --dump).
    bool unresolved(items::SlotKind kind, const items::DisplayInfoRow& row) const {
        if (kind == items::SlotKind::kHead) return !row.leftModel.empty();
        if (kind == items::SlotKind::kShoulder) return !row.leftModel.empty() || !row.rightModel.empty();
        const std::string modelFile = row.leftModel.empty() ? row.rightModel : row.leftModel;
        if (modelFile.empty()) return false;
        const std::string m = stripExtension(modelFile) + ".m2";
        return !files_.exists("Item\\ObjectComponents\\Weapon\\" + m) &&
               !files_.exists("Item\\ObjectComponents\\Shield\\" + m);
    }

    static fmt::ItemModels emptyModels() {
        fmt::ItemModels im{};
        im.kind = fmt::kItemModelNone;
        for (auto& side : im.model)
            for (auto& v : side) v = fmt::kNoItemModel;
        for (auto& side : im.texture)
            for (auto& v : side) v = fmt::kNoItemLayer;
        return im;
    }

private:
    // `opaque` - every batch drawing the skin on the models it goes with is opaque
    // (ItemModelBank::skinOpaque).
    int texId(const std::string& path, bool opaque) {
        if (path.empty() || !files_.exists(path)) return -1;
        bool alpha = false;
        return tex_.get(path, alpha, opaque);
    }
    // The texture is baked only once a model file is there (was: baked first - a texture of a
    // variant without a model stayed in the pak unused).
    // A texture file is baked opaque when every model it goes on in the display
    // (all the races, both shoulders; the model as racial() picks it) draws the skin opaque - one texture per file.
    void planSkin(const char* dir, const std::string& modelName, const std::string& texName) {
        const std::string name = stripExtension(modelName);
        if (name.empty() || texName.empty()) return;
        for (int v = 0; v < fmt::kItemVariants; ++v) {
            uint8_t race, sex;
            variantRaceSex(v, race, sex);
            const std::string suffix = v < 20 ? core::raceGenderSuffix(race, sex) : std::string();
            const std::string racialModel = suffix.empty() ? std::string() : std::string(dir) + name + suffix + ".m2";
            const std::string baseModel = std::string(dir) + name + ".m2";
            const bool racialThere = !racialModel.empty() && files_.exists(racialModel);
            if (!racialThere && !files_.exists(baseModel)) continue;
            std::string p = suffix.empty() ? std::string() : std::string(dir) + texName + suffix + ".blp";
            if (p.empty() || !files_.exists(p)) p = std::string(dir) + texName + ".blp";
            const bool opaque = models_.skinOpaque(racialThere ? racialModel : baseModel);
            auto it = fileOpaque_.emplace(lower(p), true).first;
            it->second = it->second && opaque;
        }
    }
    bool opaqueFor(const std::string& p) const {
        const auto it = fileOpaque_.find(lower(p));
        return it != fileOpaque_.end() && it->second;
    }
    bool racial(const char* dir, const std::string& modelName, const std::string& texName, int side,
                items::FolderModels& out) {
        bool any = false;
        const std::string name = stripExtension(modelName);
        if (name.empty()) return false;
        for (int v = 0; v < fmt::kItemVariants; ++v) {
            uint8_t race, sex;
            variantRaceSex(v, race, sex);
            const std::string suffix = v < 20 ? core::raceGenderSuffix(race, sex) : std::string();
            const std::string racialModel = suffix.empty() ? std::string() : std::string(dir) + name + suffix + ".m2";
            const std::string baseModel = std::string(dir) + name + ".m2";
            if ((racialModel.empty() || !files_.exists(racialModel)) && !files_.exists(baseModel)) continue;
            int t = -1;
            std::string texPath;
            if (!texName.empty()) {
                if (!suffix.empty()) {
                    const std::string p = std::string(dir) + texName + suffix + ".blp";
                    if (files_.exists(p)) {
                        t = texId(p, opaqueFor(p));
                        texPath = p;
                    }
                }
                if (t < 0) {
                    const std::string p = std::string(dir) + texName + ".blp";
                    t = texId(p, opaqueFor(p));
                    texPath = p;
                }
            }
            int m = -1;
            std::string modelPath;
            if (!racialModel.empty()) {
                m = models_.get(racialModel, t >= 0);
                modelPath = racialModel;
            }
            if (m < 0) {
                m = models_.get(baseModel, t >= 0);
                modelPath = baseModel;
            }
            if (m < 0) continue;
            current_.model[side][v] = static_cast<uint16_t>(m);
            current_.texture[side][v] = t >= 0 ? static_cast<uint16_t>(t) : fmt::kNoItemLayer;
            out.model[side][v] = modelPath;
            out.texture[side][v] = t >= 0 ? texPath : std::string();
            any = true;
        }
        return any;
    }
    // Weapons and shields: pipeline::readItemDisplayArt (item_textures.cpp, which needs the DBC layout; same
    // fields): the left pair, else the right; the texture by the weapon model when there is one, else the shield.
    bool weaponOrShield(bool weapon, const items::DisplayInfoRow& row, items::FolderModels& out) {
        std::string modelFile = row.leftModel, textureName = row.leftModelTexture;
        if (modelFile.empty()) {
            modelFile = row.rightModel;
            textureName = row.rightModelTexture;
        }
        if (modelFile.empty()) return false;
        modelFile = stripExtension(modelFile) + ".m2";
        const std::string weaponPath = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        const std::string shieldPath = "Item\\ObjectComponents\\Shield\\" + modelFile;
        const std::string& path = weapon ? weaponPath : shieldPath;
        if (!files_.exists(path)) return false;  // no texture baked for a model not there
        int t = -1;
        std::string texPath;
        if (!textureName.empty()) {
            // The opaque rule: the weapon model when there is one, else the shield.
            const bool opaque = models_.skinOpaque(files_.exists(weaponPath) ? weaponPath : shieldPath);
            texPath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            t = texId(texPath, opaque);
            if (t < 0) {
                texPath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
                t = texId(texPath, opaque);
            }
        }
        const int m = models_.get(path, t >= 0);
        if (m < 0) return false;
        for (int v = 0; v < fmt::kItemVariants; ++v) {
            current_.model[0][v] = static_cast<uint16_t>(m);
            current_.texture[0][v] = t >= 0 ? static_cast<uint16_t>(t) : fmt::kNoItemLayer;
            out.model[0][v] = path;
            out.texture[0][v] = t >= 0 ? texPath : std::string();
        }
        return true;
    }

    const FileIndex& files_;
    ItemModelBank& models_;
    TextureBank& tex_;
    std::map<std::string, bool> fileOpaque_;
    fmt::ItemModels current_ = emptyModels();
    fmt::ItemModels yielded_ = emptyModels();
};

const char* kindName(uint8_t k) {
    switch (k) {
        case fmt::kItemModelHelm: return "helm";
        case fmt::kItemModelShoulder: return "shoulder";
        case fmt::kItemModelWeapon: return "weapon";
        case fmt::kItemModelShield: return "shield";
        default: return "none";
    }
}

// The file index as item_conv_rules.hpp asks it (B1 region textures, B2 cloak textures): case-insensitive, the
// path handed back lower-cased (the banks key their files so).
class IndexQuery : public items::FileQuery {
public:
    explicit IndexQuery(const FileIndex& files) : files_(files) {}
    bool find(const std::string& path, std::string* actualPath) const override {
        if (!files_.exists(path)) return false;
        if (actualPath) *actualPath = lower(path);
        return true;
    }

private:
    const FileIndex& files_;
};

// An ItemDisplayInfo row as item_conv_rules.hpp takes it (texture columns by upstream's layout: texFields).
items::DisplayInfoRow displayInfoRow(const pipeline::DBCFile& display, uint32_t r, const uint32_t texFields[8]) {
    items::DisplayInfoRow d;
    d.id = display.getUInt32(r, 0);
    d.leftModel = display.getString(r, 1);
    d.rightModel = display.getString(r, 2);
    d.leftModelTexture = display.getString(r, 3);
    d.rightModelTexture = display.getString(r, 4);
    for (int g = 0; g < 3; ++g) d.geosetGroup[g] = display.getUInt32(r, 7u + static_cast<uint32_t>(g));
    d.flags = display.getUInt32(r, 10);
    d.helmetGeosetVis[0] = display.getUInt32(r, 13);
    d.helmetGeosetVis[1] = display.getUInt32(r, 14);
    for (int s = 0; s < 8; ++s) d.texture[s] = display.getString(r, texFields[s]);
    d.hasItemVisualColumn = display.getFieldCount() > 23;
    if (d.hasItemVisualColumn) d.itemVisual = display.getUInt32(r, 23);
    return d;
}

// ---- --verify reads the written pak like the PSP ---------------------------

class PakReader {
public:
    ~PakReader() {
        if (f_) std::fclose(f_);
    }
    bool open(const std::string& path, std::string& why) {
        f_ = std::fopen(path.c_str(), "rb");
        if (!f_) return (why = "cannot open"), false;
        uint32_t head[4];
        if (!readAt(0, head, sizeof(head)) || std::memcmp(head, "WPAK", 4) != 0 || head[1] != 1 || head[2] == 0)
            return (why = "not WPAK v1"), false;
        count_ = head[2];
        tocOffset_ = head[3];
        toc_.resize(count_);
        if (!readAt(tocOffset_, toc_.data(), count_ * sizeof(pp::PakDiskEntry))) return (why = "TOC short"), false;
        for (auto& e : toc_) e.name[sizeof(e.name) - 1] = '\0';
        const pp::PakDiskEntry& last = toc_.back();
        if (std::strcmp(last.name, pp::kPakFenceName) != 0) return (why = "no ~fence record last"), false;
        fence_.resize(last.size);
        if (last.size < sizeof(pp::PakFenceHeader) || !readAt(last.offset, fence_.data(), last.size))
            return (why = "fence unreadable"), false;
        pp::PakFenceHeader fh{};
        std::memcpy(&fh, fence_.data(), sizeof(fh));
        if (std::memcmp(fh.magic, "WFNC", 4) != 0 || fh.version != 1 || fh.records != count_ || fh.step == 0 ||
            fh.fences != (count_ + fh.step - 1) / fh.step || sizeof(fh) + fh.fences * 4u > last.size)
            return (why = "fence header"), false;
        step_ = fh.step;
        fences_ = fh.fences;
        scratch_.resize(step_);
        return true;
    }
    uint32_t count() const { return count_; }
    uint32_t step() const { return step_; }
    uint32_t fences() const { return fences_; }
    const std::vector<pp::PakDiskEntry>& toc() const { return toc_; }
    const char* fenceName(uint32_t i) const {
        uint32_t o;
        std::memcpy(&o, fence_.data() + sizeof(pp::PakFenceHeader) + i * 4u, 4);
        return o < fence_.size() ? reinterpret_cast<const char*>(fence_.data() + o) : "";
    }
    // The PSP's lookup (pakIndexLookup) over disk blocks of the TOC.
    bool find(const std::string& name, pp::PakDiskEntry& out) {
        ++lookups_;
        return pp::pakIndexLookup(
            name.c_str(), count_, step_, fences_, [this](uint32_t i) { return fenceName(i); },
            [this](uint32_t first, uint32_t n, pp::PakDiskEntry* dst) {
                ++blockReads_;
                return readAt(tocOffset_ + uint64_t{first} * sizeof(pp::PakDiskEntry), dst,
                              n * sizeof(pp::PakDiskEntry));
            },
            scratch_.data(), out);
    }
    bool read(const pp::PakDiskEntry& e, std::vector<uint8_t>& out) {
        out.resize(e.size);
        return e.size == 0 || readAt(e.offset, out.data(), e.size);
    }
    bool load(const std::string& name, std::vector<uint8_t>& out) {
        pp::PakDiskEntry e{};
        return find(name, e) && read(e, out);
    }
    uint64_t lookups() const { return lookups_; }
    uint64_t blockReads() const { return blockReads_; }
    uint64_t fileSize() {
#if defined(_WIN32)
        _fseeki64(f_, 0, SEEK_END);
        return static_cast<uint64_t>(_ftelli64(f_));
#else
        fseeko(f_, 0, SEEK_END);
        return static_cast<uint64_t>(ftello(f_));
#endif
    }

private:
    bool readAt(uint64_t offset, void* dst, size_t size) {
#if defined(_WIN32)
        if (_fseeki64(f_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
        if (fseeko(f_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
        return std::fread(dst, 1, size, f_) == size;
    }
    std::FILE* f_ = nullptr;
    uint32_t count_ = 0, tocOffset_ = 0, step_ = 0, fences_ = 0;
    std::vector<pp::PakDiskEntry> toc_;
    std::vector<uint8_t> fence_;
    std::vector<pp::PakDiskEntry> scratch_;
    uint64_t lookups_ = 0, blockReads_ = 0;
};

bool wtexValid(const std::vector<uint8_t>& b) {
    if (b.size() < 32 + 1024 || std::memcmp(b.data(), "WTEX", 4) != 0) return false;
    uint16_t w, h;
    std::memcpy(&w, b.data() + 8, 2);
    std::memcpy(&h, b.data() + 10, 2);
    return w >= 16 && h >= 8 && w <= 512 && h <= 512 && b.size() >= 32u + 1024u + size_t{w} * h;
}

bool wlayValid(const std::vector<uint8_t>& b) {
    if (b.size() < sizeof(fmt::LayerHeader) + 1024 || std::memcmp(b.data(), "WLAY", 4) != 0) return false;
    fmt::LayerHeader lh;
    std::memcpy(&lh, b.data(), sizeof(lh));
    return lh.w && lh.h && lh.x + lh.w <= fmt::kBodyTexSize && lh.y + lh.h <= fmt::kBodyTexSize &&
           b.size() == sizeof(lh) + 1024u + size_t{lh.w} * lh.h;
}

struct ItemRow {
    uint32_t item, display, invType;
    uint32_t sheath = 0;  // Item.dbc SheatheType (field 7)
};

int verifyPak(const std::string& path, const std::vector<uint32_t>& displays, const std::vector<ItemRow>& itemRows,
              bool withModels) {
    int errors = 0;
    auto err = [&](const std::string& what) {
        if (++errors <= 40) std::fprintf(stderr, "verify: %s\n", what.c_str());
    };
    PakReader pak;
    std::string why;
    if (!pak.open(path, why)) {
        std::fprintf(stderr, "verify: %s: %s\n", path.c_str(), why.c_str());
        return 1;
    }
    // TOC: sorted (strcmp, the PSP's order), blobs 64-aligned inside the file, the fence = every step-th name.
    const uint64_t fileSize = pak.fileSize();
    const auto& toc = pak.toc();
    for (uint32_t i = 0; i < pak.count(); ++i) {
        if (i && std::strcmp(toc[i - 1].name, toc[i].name) > 0) err(std::string("TOC out of order at ") + toc[i].name);
        if ((toc[i].offset & 63u) || uint64_t{toc[i].offset} + toc[i].size > fileSize)
            err(std::string("blob outside the file: ") + toc[i].name);
    }
    for (uint32_t f = 0; f < pak.fences(); ++f)
        if (std::strcmp(pak.fenceName(f), toc[f * pak.step()].name) != 0) err("fence " + std::to_string(f));
    // Every record reachable through the PSP lookup (the first of its name).
    size_t found = 0;
    for (uint32_t i = 0; i < pak.count(); ++i) {
        if (i && std::strcmp(toc[i - 1].name, toc[i].name) == 0) continue;
        pp::PakDiskEntry e{};
        if (!pak.find(toc[i].name, e) || e.offset != toc[i].offset) err(std::string("lookup failed: ") + toc[i].name);
        else ++found;
    }
    const uint64_t tocLookups = pak.lookups(), tocBlocks = pak.blockReads();
    // Records and what they name.
    std::set<uint16_t> goodLayers, goodTextures, goodModels;
    size_t records = 0, withTail = 0;
    size_t visualPoints = 0, weaponRecords = 0, sheathRows = 0;  // weapons
    std::vector<uint8_t> blob, sub;
    auto layerOk = [&](uint16_t q) {
        if (goodLayers.count(q)) return true;
        if (!pak.load("q_" + std::to_string(q), sub) || !wlayValid(sub)) return false;
        goodLayers.insert(q);
        return true;
    };
    auto textureOk = [&](uint16_t t) {
        if (goodTextures.count(t)) return true;
        if (t >= kMaxTextureIds || !pak.load("t_" + std::to_string(t), sub) || !wtexValid(sub)) return false;
        goodTextures.insert(t);
        return true;
    };
    auto modelOk = [&](uint16_t a) {
        if (goodModels.count(a)) return true;
        if (!pak.load("a_" + std::to_string(a), sub) ||
            !fmt::itemModelValid(sub.data(), static_cast<uint32_t>(sub.size())))
            return false;
        // A weapon / shield's ItemVisualPoints footer, when there is one, reads back.
        if (sub.size() >= sizeof(fmt::ItemVisualPoints) && std::memcmp(sub.data() + sub.size() - 4, "WIVP", 4) == 0) {
            fmt::ItemVisualPoints vp;
            if (!fmt::itemVisualPoints(sub.data(), static_cast<uint32_t>(sub.size()), vp)) return false;
            ++visualPoints;
        }
        fmt::ModelHeader h;
        std::memcpy(&h, sub.data(), sizeof(h));
        std::vector<uint16_t> texIds;
        for (uint32_t i = 0; i < h.batchCount[0]; ++i) {
            fmt::ModelBatch b;
            std::memcpy(&b, sub.data() + h.batchOffset + i * sizeof(b), sizeof(b));
            if (b.texture != fmt::kNoTexture && b.texture != fmt::kTexItemSkin) texIds.push_back(b.texture);
        }
        for (uint16_t t : texIds)
            if (!textureOk(t)) return false;
        goodModels.insert(a);
        return true;
    };
    // "b_rules" - present, whole, and every HelmetGeosetVis id a record names is a row of it.
    std::set<uint32_t> visRows;
    size_t bodyTails = 0;
    {
        fmt::BodyRulesHeader bh{};
        if (!pak.load(fmt::kBodyRulesName, sub) || sub.size() < sizeof(bh)) {
            err("b_rules: missing");
        } else {
            std::memcpy(&bh, sub.data(), sizeof(bh));
            if (std::memcmp(bh.magic, "WBRL", 4) != 0 || bh.version != 1 ||
                sub.size() != sizeof(bh) + size_t{bh.helmetVisRows} * sizeof(fmt::HelmetVisRow))
                err("b_rules: bad header / size");
            else
                for (uint32_t i = 0; i < bh.helmetVisRows; ++i) {
                    fmt::HelmetVisRow row;
                    std::memcpy(&row, sub.data() + sizeof(bh) + i * sizeof(row), sizeof(row));
                    visRows.insert(row.id);
                }
        }
    }
    for (uint32_t id : displays) {
        const std::string name = "i_" + std::to_string(id);
        if (!pak.load(name, blob) || blob.size() < sizeof(fmt::ItemDisplay) || std::memcmp(blob.data(), "WITM", 4) != 0) {
            err(name + ": missing or not WITM");
            continue;
        }
        ++records;
        fmt::ItemDisplay d;
        std::memcpy(&d, blob.data(), sizeof(d));
        if (d.displayId != id) err(name + ": display id " + std::to_string(d.displayId));
        for (const auto& sexLayers : d.layer)
            for (uint16_t q : sexLayers)
                if (q != fmt::kNoItemLayer && !layerOk(q)) err(name + ": layer q_" + std::to_string(q));
        for (uint16_t c : d.capeTexture)
            if (c != fmt::kNoItemLayer && !textureOk(c)) err(name + ": cape t_" + std::to_string(c));
        // Version 4 - the body tail is the record's last 8 bytes, models (if any) before it.
        const size_t tail = d.version >= fmt::kItemFormatBody ? sizeof(fmt::ItemBodyInfo) : 0u;
        if (tail) {
            if (blob.size() < sizeof(d) + tail) {
                err(name + ": body tail");
                continue;
            }
            fmt::ItemBodyInfo bi;
            std::memcpy(&bi, blob.data() + blob.size() - tail, sizeof(bi));
            for (uint16_t v : bi.helmetVis)
                if (v && !visRows.count(v)) err(name + ": HelmetGeosetVis " + std::to_string(v) + " not in b_rules");
            ++bodyTails;
        }
        if (d.version < fmt::kItemFormatAttach || blob.size() == sizeof(d) + tail) {
            if (blob.size() != sizeof(d) + tail) err(name + ": size");
            continue;
        }
        if (!withModels || blob.size() != sizeof(d) + sizeof(fmt::ItemModels) + tail) {
            err(name + ": model tail");
            continue;
        }
        ++withTail;
        fmt::ItemModels im;
        std::memcpy(&im, blob.data() + sizeof(d), sizeof(im));
        if (im.kind == fmt::kItemModelNone || im.kind > fmt::kItemModelShield) err(name + ": kind");
        // The sheath hint is an Item.dbc SheatheType (0..7) or unknown.
        if (d.version >= fmt::kItemFormatWeapons) {
            ++weaponRecords;
            if (im.sheathHint > 7 && im.sheathHint != fmt::kSheathUnknown) err(name + ": sheath hint");
        }
        for (int side = 0; side < 2; ++side)
            for (int v = 0; v < fmt::kItemVariants; ++v) {
                if (im.model[side][v] != fmt::kNoItemModel && !modelOk(im.model[side][v]))
                    err(name + ": model a_" + std::to_string(im.model[side][v]));
                if (im.texture[side][v] != fmt::kNoItemLayer && !textureOk(im.texture[side][v]))
                    err(name + ": skin t_" + std::to_string(im.texture[side][v]));
            }
    }
    // The Item.dbc table, row by row.
    size_t tableRows = 0;
    std::map<uint32_t, std::vector<uint8_t>> blocks;
    for (const ItemRow& row : itemRows) {
        const uint32_t block = row.item / fmt::kItemTableBlock;
        auto it = blocks.find(block);
        if (it == blocks.end()) {
            std::vector<uint8_t> b;
            if (!pak.load("u_" + std::to_string(block), b)) b.clear();
            it = blocks.emplace(block, std::move(b)).first;
        }
        const std::vector<uint8_t>& b = it->second;
        fmt::ItemTableHeader th{};
        if (b.size() >= sizeof(th)) std::memcpy(&th, b.data(), sizeof(th));
        const uint32_t k = row.item - block * fmt::kItemTableBlock;
        uint32_t v = 0;
        // + u8 sheath[count] when the header says so.
        const bool sheathTail = b.size() >= sizeof(th) && (th.reserved & fmt::kItemTableSheath) != 0;
        if (b.size() < sizeof(th) || std::memcmp(th.magic, "WITB", 4) != 0 || th.firstItem != block * fmt::kItemTableBlock ||
            th.count > fmt::kItemTableBlock || b.size() != sizeof(th) + th.count * (sheathTail ? 5u : 4u)) {
            err("u_" + std::to_string(block) + ": bad block");
            continue;
        }
        if (k < th.count) std::memcpy(&v, b.data() + sizeof(th) + k * 4u, 4);
        if (v != fmt::itemTablePack(row.display, row.invType)) err("item " + std::to_string(row.item) + ": table value");
        else ++tableRows;
        if (sheathTail && k < th.count) {  // the sheath tail
            if (b[sizeof(th) + th.count * 4u + k] != static_cast<uint8_t>(row.sheath))
                err("item " + std::to_string(row.item) + ": sheath type");
            else
                ++sheathRows;
        }
    }
    // The item visuals ("ivis", "ifx_", "ift_").
    std::string visualSummary;
    errors += verifyItemVisuals([&](const std::string& n, std::vector<uint8_t>& out) { return pak.load(n, out); },
                                visualSummary);
    std::printf("verify: %u records (sorted, fence ok), %zu found by the PSP lookup (%.2f TOC block reads each); "
                "%zu displays (%zu with models, %zu with the body tail; b_rules %zu helm rows): %zu layers, %zu models, "
                "%zu textures checked; %zu Item.dbc rows; %d errors\n",
                pak.count(), found, tocLookups ? static_cast<double>(tocBlocks) / static_cast<double>(tocLookups) : 0.0,
                records, withTail, bodyTails, visRows.size(), goodLayers.size(), goodModels.size(),
                goodTextures.size(), tableRows, errors);
    std::printf("verify (weapons): %zu records v%u+ (sheath hint, item visual), %zu models with visual "
                "points, %zu Item.dbc sheath types; %s\n",
                weaponRecords, static_cast<unsigned>(fmt::kItemFormatWeapons), visualPoints, sheathRows,
                visualSummary.c_str());
    return errors;
}

// ---- The NPC races' helm models ---------------------------------------------
// Helm models exist per race as Item\ObjectComponents\Head\<LeftModel>_<ChrRaces.ClientPrefix><M|F>.m2 for any
// race. ItemModels (above) holds the 10 playable races and the base name - a goblin, vrykul, tuskarr, forest / ice
// troll or skeleton NPC (CreatureDisplayInfoExtra) takes the base column, and a helm without a base file had no
// model for it. For such helms only (no base model, a model of a ChrRaces race outside the 10 exists): one
// "hr_<display>" record (include/rendering/psp/psp_item_npc_race.hpp) with those races' variants - the model, its
// skin as ItemNaming's racial() picks it (LeftModelTexture + the suffix if the file exists, else plain).
// Additive: the WITM records stay as they were; the PSP asks for "hr_" only for a race outside the 10.

constexpr uint32_t kKindItemNpcRaceHelm = kKindItem;

struct NpcRace {
    uint8_t race;
    std::string prefix;  // ChrRaces.ClientPrefix ("Go", "Vr", ...)
};

// ChrRaces (3.3.5a: ID 0, ClientPrefix 6), the races items.pak has no column for.
std::vector<NpcRace> npcRacesOf(const std::string& root) {
    std::vector<NpcRace> out;
    pipeline::DBCFile races;
    if (!loadDbc(root, "chrraces.dbc", races)) return out;
    for (uint32_t r = 0; r < races.getRecordCount(); ++r) {
        const uint32_t id = races.getUInt32(r, 0);
        if (id == 0 || id > 255 || fmt::itemVariant(static_cast<uint8_t>(id), 0) != fmt::kItemVariants - 1) continue;
        const std::string prefix = races.getString(r, 6);
        if (!prefix.empty()) out.push_back({static_cast<uint8_t>(id), prefix});
    }
    return out;
}

struct NpcRaceHelmStats {
    size_t records = 0, variants = 0, bytes = 0;
};

std::vector<uint8_t> bakeNpcRaceHelms(const FileIndex& files, const pipeline::DBCFile& display, uint32_t r, uint32_t id,
                                      const std::vector<NpcRace>& races, ItemModelBank& models, TextureBank& tex,
                                      NpcRaceHelmStats& stats) {
    const std::string name = stripExtension(display.getString(r, 1));  // LeftModel
    if (name.empty() || races.empty()) return {};
    const std::string dir = "Item\\ObjectComponents\\Head\\";
    if (files.exists(dir + name + ".m2")) return {};  // the base model: the base column has it
    const std::string texName = display.getString(r, 3);  // LeftModelTexture
    std::vector<fmt::NpcRaceHelmVariant> vars;
    for (const NpcRace& race : races)
        for (uint8_t sex = 0; sex < 2; ++sex) {
            const std::string suffix = "_" + race.prefix + (sex ? "F" : "M");
            const std::string modelPath = dir + name + suffix + ".m2";
            if (!files.exists(modelPath)) continue;
            const bool opaque = models.skinOpaque(modelPath);  // the opaque rule for this model
            int t = -1;
            if (!texName.empty()) {
                const std::string racialTex = dir + texName + suffix + ".blp";
                bool alpha = false;
                if (files.exists(racialTex)) t = tex.get(racialTex, alpha, opaque);
                const std::string baseTex = dir + texName + ".blp";
                if (t < 0 && files.exists(baseTex)) t = tex.get(baseTex, alpha, opaque);
            }
            const int m = models.get(modelPath, t >= 0);
            if (m < 0 || vars.size() >= fmt::kNpcRaceHelmMaxVariants) continue;
            fmt::NpcRaceHelmVariant v{};
            v.race = race.race;
            v.sex = sex;
            v.model = static_cast<uint16_t>(m);
            v.texture = t >= 0 ? static_cast<uint16_t>(t) : fmt::kNoItemLayer;
            vars.push_back(v);
        }
    if (vars.empty()) return {};
    fmt::NpcRaceHelmHeader h{};
    std::memcpy(h.magic, "WHRV", 4);
    h.version = 1;
    h.count = static_cast<uint16_t>(vars.size());
    h.displayId = id;
    std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(vars.data()),
                reinterpret_cast<const uint8_t*>(vars.data() + vars.size()));
    ++stats.records;
    stats.variants += vars.size();
    stats.bytes += blob.size();
    return blob;
}

// --verify: every "hr_" record found by the PSP lookup, valid, its models and skins present and valid.
int verifyNpcRaceHelms(const std::string& path, const std::vector<uint32_t>& ids) {
    int errors = 0;
    auto err = [&](const std::string& what) {
        if (++errors <= 40) std::fprintf(stderr, "verify: %s\n", what.c_str());
    };
    PakReader pak;
    std::string why;
    if (!pak.open(path, why)) {
        std::fprintf(stderr, "verify: %s: %s\n", path.c_str(), why.c_str());
        return 1;
    }
    std::vector<uint8_t> blob, sub;
    size_t variants = 0;
    for (uint32_t id : ids) {
        const std::string name = std::string(fmt::kNpcRaceHelmPrefix) + std::to_string(id);
        if (!pak.load(name, blob) || !fmt::npcRaceHelmValid(blob.data(), static_cast<uint32_t>(blob.size()))) {
            err(name + ": missing or not WHRV");
            continue;
        }
        fmt::NpcRaceHelmHeader h;
        std::memcpy(&h, blob.data(), sizeof(h));
        if (h.displayId != id) err(name + ": display id");
        for (uint32_t k = 0; k < h.count; ++k) {
            fmt::NpcRaceHelmVariant v;
            std::memcpy(&v, blob.data() + sizeof(h) + k * sizeof(v), sizeof(v));
            ++variants;
            if (fmt::itemVariant(v.race, v.sex) != fmt::kItemVariants - 1 || v.sex > 1) err(name + ": race / sex");
            if (!pak.load("a_" + std::to_string(v.model), sub) ||
                !fmt::itemModelValid(sub.data(), static_cast<uint32_t>(sub.size())))
                err(name + ": model a_" + std::to_string(v.model));
            if (v.texture != fmt::kNoItemLayer && (!pak.load("t_" + std::to_string(v.texture), sub) || !wtexValid(sub)))
                err(name + ": skin t_" + std::to_string(v.texture));
        }
    }
    std::printf("verify (others): %zu hr_ records, %zu NPC race helm variants checked; %d errors\n",
                ids.size(), variants, errors);
    return errors;
}

} // namespace

int main(int argc, char** argv) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string root, out, mirror;
    bool startOutfits = false;
    bool withModels = true, verify = false, dump = false;  // --no-models clears withModels
    int texMax = 128;
    uint32_t step = 128;  // fence step (PakIndex reads step x 64 B per TOC block)
    std::set<uint32_t> testDisplays;  // --test-displays (PC checks only)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) root = argv[++i];
        else if (a == "--mirror" && i + 1 < argc) mirror = argv[++i];  // a faster copy of <extracted>/item
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--start-outfits") startOutfits = true;
        else if (a == "--test-displays" && i + 1 < argc) {
            // A few ItemDisplayInfo ids (comma separated, taken from the DBCs) added to
            // --start-outfits for a small test pak - the full bake takes every row.
            for (const char* p = argv[++i]; *p;) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 10);
                if (end == p) break;
                if (v) testDisplays.insert(static_cast<uint32_t>(v));
                p = *end == ',' ? end + 1 : end;
            }
        }
        else if (a == "--tex-max" && i + 1 < argc) texMax = std::atoi(argv[++i]);
        else if (a == "--step" && i + 1 < argc) step = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--no-models") withModels = false;
        else if (a == "--verify") verify = true;
        else if (a == "--dump") dump = true;
        else if (a == "--displays") {
            std::fprintf(stderr, "--displays is gone: every ItemDisplayInfo row is baked; "
                                 "--start-outfits for a small set\n");
            return 2;
        }
    }
    if (root.empty() || out.empty()) {
        std::fprintf(stderr, "usage: psp_item_conv --extracted <dir> --out <pak> [--start-outfits] [--tex-max 128]\n"
                             "       [--no-models] [--verify] [--dump] [--step 128] [--mirror <copy of extracted/item>]\n"
                             "       [--test-displays <id,id,...> (with --start-outfits: a small test pak)]\n");
        return 2;
    }
    if (!testDisplays.empty() && !startOutfits) {
        std::fprintf(stderr, "--test-displays goes with --start-outfits (a full bake takes every ItemDisplayInfo row)\n");
        return 2;
    }
    pipeline::DBCFile display, vis, outfits, itemDbc;
    if (!loadDbc(root, "itemdisplayinfo.dbc", display) || !loadDbc(root, "helmetgeosetvisdata.dbc", vis)) {
        std::fprintf(stderr, "missing ItemDisplayInfo / HelmetGeosetVisData under %s/dbfilesclient\n", root.c_str());
        return 1;
    }
    // HelmetGeosetVisData rows (ID, 7 hide masks; a missing column reads 0) for B3 / B5.
    std::vector<items::HelmetVisDataRow> visRows;
    for (uint32_t i = 0; i < vis.getRecordCount(); ++i) {
        items::HelmetVisDataRow row;
        row.id = vis.getUInt32(i, 0);
        for (uint32_t k = 0; k < 7; ++k) row.hide[k] = 1u + k < vis.getFieldCount() ? vis.getUInt32(i, 1u + k) : 0u;
        visRows.push_back(row);
    }
    const items::HelmetVisIndex helmetVis(visRows);
    // Which displays: every ItemDisplayInfo row, or the starter outfits.
    std::set<uint32_t> ids;
    if (startOutfits) {
        if (!loadDbc(root, "charstartoutfit.dbc", outfits)) {
            std::fprintf(stderr, "missing CharStartOutfit.dbc\n");
            return 1;
        }
        // 3.3.5a: ID, packed race/class/sex/outfit, 24 item ids, 24 display ids, 24 inventory types.
        for (uint32_t r = 0; r < outfits.getRecordCount(); ++r)
            for (uint32_t k = 0; k < 24; ++k)
                if (const uint32_t d = outfits.getUInt32(r, 26 + k); d != 0 && d != 0xFFFFFFFFu) ids.insert(d);
        ids.insert(testDisplays.begin(), testDisplays.end());  // --test-displays
    } else {
        for (uint32_t r = 0; r < display.getRecordCount(); ++r)
            if (const uint32_t d = display.getUInt32(r, 0)) ids.insert(d);
    }
    // Item.dbc (3.3.5a: ID 0, Class 1, SubClass 2, DisplayInfoID 5, InventoryType 6, SheatheType 7 - upstream's
    // "DisplayID" default 5): the "u_" table (item_bake.md B6, built here: a row past the table's ranges stops the
    // bake before anything is baked) and the rows --verify checks.
    std::vector<ItemRow> itemRows;
    std::vector<items::ItemTableBlock> itemTable;
    if (loadDbc(root, "item.dbc", itemDbc) && itemDbc.getFieldCount() >= 7) {
        const bool hasSheathe = itemDbc.getFieldCount() >= 8;  // SheatheType (field 7)
        std::vector<items::ItemRow> rows;
        rows.reserve(itemDbc.getRecordCount());
        for (uint32_t r = 0; r < itemDbc.getRecordCount(); ++r) {
            items::ItemRow row;
            row.id = itemDbc.getUInt32(r, 0);
            row.itemClass = itemDbc.getUInt32(r, 1);
            row.subClass = itemDbc.getUInt32(r, 2);
            row.displayInfoId = itemDbc.getUInt32(r, 5);
            row.inventoryType = itemDbc.getUInt32(r, 6);
            row.sheatheType = hasSheathe ? itemDbc.getUInt32(r, 7) : 0u;
            rows.push_back(row);
        }
        uint32_t badItem = 0;
        if (items::buildItemTable(rows, hasSheathe, itemTable, &badItem) != items::ItemTableStatus::kOk) {
            std::fprintf(stderr, "item %u: display / inventory type past the table's 24/8 bits\n", badItem);
            return items::kExitCodeBadItemRow;
        }
        for (const items::ItemRow& row : rows) {
            if (!row.id || !row.displayInfoId) continue;
            ItemRow v{row.id, row.displayInfoId, row.inventoryType};
            v.sheath = hasSheathe ? std::min<uint32_t>(row.sheatheType, 0xFEu) : 0u;
            itemRows.push_back(v);
        }
    } else {
        std::fprintf(stderr, "warning: no Item.dbc - the pak has no item table (other players fall back to item "
                             "queries)\n");
    }
    std::printf("DBCs read (%.1f s), listing %s/item ...\n", secondsSince(t0),
                mirror.empty() ? root.c_str() : mirror.c_str());
    std::fflush(stdout);
    // --mirror <dir> = a copy of <extracted>/item on a faster disk (the bake reads ~45 000
    // files; on a busy hard disk that is hours) - item files are read from it, everything else from --extracted.
    const std::string itemRoot = mirror.empty() ? root : mirror;
    FileIndex files(itemRoot, mirror.empty() ? std::string() : root);
    const size_t indexed = files.scan("item");
    std::printf("item displays to bake: %zu (%s); Item.dbc rows %zu; files under item/: %zu", ids.size(),
                startOutfits ? "CharStartOutfit" : "every ItemDisplayInfo row", itemRows.size(), indexed);
    if (!mirror.empty()) std::printf(" (+%zu only in --extracted: copied when used)", files.fallbackOnly());
    std::printf(" (%.1f s)\n", secondsSince(t0));
    std::fflush(stdout);

    // The races without an ItemModels column (ChrRaces.ClientPrefix) for the "hr_" records.
    const std::vector<NpcRace> npcRaces = withModels ? npcRacesOf(root) : std::vector<NpcRace>{};
    NpcRaceHelmStats npcHelmStats;
    std::vector<uint32_t> npcHelmIds;
    size_t npcHelmModels = 0, npcHelmModelBytes = 0, npcHelmTextures = 0, npcHelmTextureBytes = 0;
    SortedPakWriter writer(out, step);
    if (!writer.ok()) {
        std::fprintf(stderr, "cannot write %s.tmp\n", out.c_str());
        return 1;
    }
    // Per display the sheath hint (item_bake.md B8), the visuals the displays
    // name themselves (ItemDisplayInfo field 23), and the item visuals: "ivis" + "ifx_" + "ift_" (the effects read
    // from --extracted: they live under Spells\ and elsewhere, not in item/).
    const std::unordered_map<uint32_t, uint8_t> hints =
        itemDbc.getRecordCount() ? sheathHints(itemDbc) : std::unordered_map<uint32_t, uint8_t>{};
    // The slot kind of the display's items - the models' folders (item_bake.md B7, ItemNaming).
    const std::unordered_map<uint32_t, items::SlotKind> slotKinds =
        itemDbc.getRecordCount() ? displaySlotKinds(itemDbc) : std::unordered_map<uint32_t, items::SlotKind>{};
    std::set<uint32_t> displayVisuals;
    if (display.getFieldCount() > 23)
        for (uint32_t id : ids)
            if (const int32_t row = display.findRecordById(id); row >= 0) {
                const uint32_t v = display.getUInt32(static_cast<uint32_t>(row), 23);
                if (v && v != 0xFFFFFFFFu) displayVisuals.insert(v);
            }
    std::unordered_set<uint32_t> visualRows;  // ItemVisuals.dbc ids (item_bake.md B9: a display's own visual)
    {
        pipeline::DBCFile iv;
        if (loadDbc(root, "itemvisuals.dbc", iv))
            for (uint32_t r = 0; r < iv.getRecordCount(); ++r) visualRows.insert(iv.getUInt32(r, 0));
    }
    {
        const auto tFx = std::chrono::steady_clock::now();
        std::vector<PakItem> fxItems;
        ItemVisualBank fxBank(root, 64, fxItems);
        ItemVisualBakeStats fxStats;
        if (fxBank.bake(displayVisuals, fxStats)) {
            for (PakItem& it : fxItems) writer.add(it);
            std::printf("item visuals: %zu enchantments with a visual, %zu visuals named by the displays; %zu visuals "
                        "with an effect of %zu in ItemVisuals.dbc; effects %zu of %zu models (%zu KB; %zu emitters, %zu "
                        "ribbons, %zu mesh groups), textures %zu (%zu KB) (%.1f s)\n",
                        fxStats.enchants, fxStats.displayVisuals, fxStats.visualsUsed, fxStats.visuals, fxStats.effects,
                        fxStats.effectPaths, fxStats.effectBytes / 1024, fxStats.emitters, fxStats.ribbons,
                        fxStats.groups, fxStats.textures, fxStats.textureBytes / 1024, secondsSince(tFx));
            for (const auto& [why, n] : fxStats.failures) std::printf("  effects not baked: %d %s\n", n, why.c_str());
            if (dump)
                for (const std::string& l : fxStats.lines) std::printf("%s\n", l.c_str());
            std::fflush(stdout);
        }
    }
    uint32_t texFields[8];
    pipeline::getItemDisplayInfoTextureFields(display, nullptr, texFields);
    const IndexQuery query(files);
    std::vector<PakItem> layerItems, capeItems, modelItems;
    LayerBank layers(itemRoot, layerItems);
    TextureBank capes(itemRoot, texMax, capeItems);
    // Item models; their skins share the cape bank ("t_<n>").
    ItemModelBank itemModels(itemRoot, files, capes, modelItems);
    // The banks' blobs go to the writer's temp file as they come.
    auto drain = [&](std::vector<PakItem>& v, char renameTo) {
        for (PakItem& it : v) {
            if (renameTo) it.name[0] = renameTo;  // TextureBank names "x_<n>"; in items.pak they are "t_<n>"
            writer.add(it);
        }
        v.clear();
    };
    size_t modelDisplays[5] = {};
    size_t capeDisplays = 0, layerDisplays = 0, emptyDisplays = 0, unresolvedModels = 0;
    size_t missing = 0, done = 0;
    std::vector<uint32_t> written;
    written.reserve(ids.size());
    const auto tBake = std::chrono::steady_clock::now();
    for (uint32_t id : ids) {
        const int32_t row = display.findRecordById(id);
        if (row < 0) {
            ++missing;
            continue;
        }
        const uint32_t r = static_cast<uint32_t>(row);
        const items::DisplayInfoRow info = displayInfoRow(display, r, texFields);
        // item_bake.md B1: per sex and region the body layer ("q_<n>"), resampled to half the upstream region
        // rectangle, placed by region index.
        uint16_t layer[2][fmt::kItemRegions];
        bool anyLayer = false;
        for (int sex = 0; sex < 2; ++sex) {
            for (int reg = 0; reg < fmt::kItemRegions; ++reg) {
                layer[sex][reg] = fmt::kNoItemLayer;
                const std::string path = items::bodyLayerSource(info.texture[reg], reg, sex, query);
                if (!path.empty()) layer[sex][reg] = layers.get(path, reg);
                anyLayer = anyLayer || layer[sex][reg] != fmt::kNoItemLayer;
            }
        }
        layerDisplays += anyLayer ? 1 : 0;
        // item_bake.md B2: the cloak texture ("t_<n>"), the same for both sexes.
        uint16_t cape[2] = {fmt::kNoItemLayer, fmt::kNoItemLayer};
        const std::string capePath = items::capeTextureSource(info.leftModelTexture, query);
        if (!capePath.empty()) {
            bool alpha = false;
            const int tid = capes.get(capePath, alpha);
            if (tid >= 0) cape[0] = cape[1] = static_cast<uint16_t>(tid);
        }
        const bool anyCape = cape[0] != fmt::kNoItemLayer;
        capeDisplays += anyCape ? 1 : 0;
        // Helm / shoulder / weapon / shield models (the ItemModels tail); item_bake.md B7 picks the folder.
        fmt::ItemModels im = ItemNaming::emptyModels();
        bool unresolved = false;
        if (withModels) {
            const auto slot = slotKinds.find(id);  // the item models' folder kind
            const items::SlotKind kind = slot != slotKinds.end() ? slot->second : items::SlotKind::kUnknown;
            ItemNaming naming(files, itemModels, capes);
            items::FolderModels found;
            const uint8_t modelKind = items::chooseItemModels(kind, info, naming, found);
            if (modelKind != fmt::kItemModelNone) {
                im = naming.yielded();
                im.kind = modelKind;
            } else {
                unresolved = naming.unresolved(kind, info);
            }
        }
        unresolvedModels += (unresolved && !anyCape) ? 1 : 0;
        if (dump && unresolved && !anyCape && unresolvedModels <= 40) {
            const std::string l = display.getString(r, 1);
            std::printf("  no folder: i_%u %s\n", id, (l.empty() ? display.getString(r, 2) : l).c_str());
        }
        const bool hasModels = withModels && im.kind != fmt::kItemModelNone;
        if (hasModels) {
            // The display's sheath hint (B8) and its own visual (B9).
            const auto hint = hints.find(id);
            im.sheathHint = hint != hints.end() ? hint->second : items::kSheathHintUnknown;
            im.itemVisual = items::ownItemVisual(info, visualRows);
        }
        // The header (B3, version 4 on every record) and the body tail (B4).
        const fmt::ItemDisplay d = items::makeItemDisplay(info, helmetVis, layer, cape);
        const fmt::ItemBodyInfo bodyInfo = items::makeBodyInfo(info);
        ++modelDisplays[im.kind < 5 ? im.kind : 0];
        if (!anyLayer && !anyCape && im.kind == fmt::kItemModelNone && !d.geosetGroup1 && !d.geosetGroup3 &&
            !d.hidesHair[0] && !d.hidesHair[1])
            ++emptyDisplays;  // icons only (potions, quest items): written all the same - every row is in the pak
        const std::vector<uint8_t> blob = items::writeItemRecord(d, hasModels ? &im : nullptr, bodyInfo);
        if (dump && im.kind != fmt::kItemModelNone) {
            std::printf("  i_%u %s:", id, kindName(im.kind));
            for (int side = 0; side < 2; ++side) {
                std::set<std::pair<uint16_t, uint16_t>> seen;
                for (int v = 0; v < fmt::kItemVariants; ++v) {
                    if (im.model[side][v] == fmt::kNoItemModel || !seen.insert({im.model[side][v], im.texture[side][v]}).second)
                        continue;
                    std::printf(" [%d v%d] a_%u=%s", side, v, im.model[side][v],
                                itemModels.pathOf(im.model[side][v]).c_str());
                    if (im.texture[side][v] != fmt::kNoItemLayer) std::printf(" t_%u", im.texture[side][v]);
                }
            }
            std::printf("\n");
        }
        writer.add("i_" + std::to_string(id), kKindItem, blob.data(), blob.size());
        written.push_back(id);
        if (withModels && !npcRaces.empty()) {  // the NPC races' helm models ("hr_<display>")
            const size_t m0 = itemModels.count(), mb0 = itemModels.bytes(), t0n = capes.count(), tb0 = capes.bytes();
            const std::vector<uint8_t> hr =
                bakeNpcRaceHelms(files, display, r, id, npcRaces, itemModels, capes, npcHelmStats);
            npcHelmModels += itemModels.count() - m0;
            npcHelmModelBytes += itemModels.bytes() - mb0;
            npcHelmTextures += capes.count() - t0n;
            npcHelmTextureBytes += capes.bytes() - tb0;
            if (!hr.empty()) {
                writer.add(std::string(fmt::kNpcRaceHelmPrefix) + std::to_string(id), kKindItemNpcRaceHelm, hr.data(),
                           hr.size());
                npcHelmIds.push_back(id);
            }
        }
        drain(layerItems, 0);
        drain(capeItems, 't');
        drain(modelItems, 0);
        if (layers.overflow() || itemModels.overflow() || capes.count() > kMaxTextureIds) {
            std::fprintf(stderr, "ids past the u16 ranges of the format (layers %zu, models %zu, textures %zu): "
                                 "a new WITM version is needed\n",
                         layers.count(), itemModels.count(), capes.count());
            return 4;
        }
        if (++done % 2000 == 0) {
            std::printf("  [%zu/%zu] layers %zu, models %zu, textures %zu, %.0f MB, %.0f s\n", done, ids.size(),
                        layers.count(), itemModels.count(), capes.count(), static_cast<double>(writer.bytes()) / 1048576.0,
                        secondsSince(tBake));
            std::fflush(stdout);
        }
    }
    const double bakeSeconds = secondsSince(tBake);
    // The Item.dbc table, one "u_<block>" per kItemTableBlock item ids that have a row, each with
    // its u8 sheath[count] tail (item_bake.md B6, built above).
    size_t tableBytes = 0;
    for (const items::ItemTableBlock& b : itemTable) {
        tableBytes += b.bytes.size();
        writer.add(b.name(), kKindItemTable, b.bytes.data(), b.bytes.size());
    }
    // "b_rules" - ChrRaces Flags (ID 0, Flags 1) and every HelmetGeosetVisData row
    // (item_bake.md B5; psp_item_format.hpp BodyRulesHeader).
    {
        pipeline::DBCFile races;
        std::vector<items::RaceRow> raceRows;
        const bool haveRaces = loadDbc(root, "chrraces.dbc", races) && races.getFieldCount() >= 2;
        if (haveRaces)
            for (uint32_t i = 0; i < races.getRecordCount(); ++i) {
                items::RaceRow rr;
                rr.id = races.getUInt32(i, 0);
                rr.flags = races.getUInt32(i, 1);
                raceRows.push_back(rr);
            }
        std::string warning;
        const std::vector<uint8_t> blob = items::buildBodyRules(haveRaces ? &raceRows : nullptr, visRows,
                                                                static_cast<int>(vis.getFieldCount()), &warning);
        if (!warning.empty()) std::fprintf(stderr, "warning: %s (feet painted on every race)\n", warning.c_str());
        writer.add(fmt::kBodyRulesName, kKindItemTable, blob.data(), blob.size());
        fmt::BodyRulesHeader bh{};
        if (blob.size() >= sizeof(bh)) std::memcpy(&bh, blob.data(), sizeof(bh));
        uint32_t noFeet = 0;
        for (uint32_t f : bh.raceFlags) noFeet += (f & fmt::kRaceFlagNoFeet) ? 1u : 0u;
        std::printf("b_rules: %zu HelmetGeosetVisData rows, %u races without foot textures\n", visRows.size(), noFeet);
    }
    std::printf("item displays %zu (%zu not in ItemDisplayInfo, %zu with body layers, %zu icon-only), layers %zu "
                "(%zu KB), capes %zu displays, %zu textures (%zu KB)\n",
                written.size(), missing, layerDisplays, emptyDisplays, layers.count(), layers.bytes() / 1024,
                capeDisplays, capes.count(), capes.bytes() / 1024);
    if (withModels) {
        std::printf("item models: helm %zu, shoulder %zu, weapon %zu, shield %zu displays; %zu models (%zu KB, %zu "
                    "tris); %zu displays name a model in no folder, %zu model files did not bake\n",
                    modelDisplays[fmt::kItemModelHelm], modelDisplays[fmt::kItemModelShoulder],
                    modelDisplays[fmt::kItemModelWeapon], modelDisplays[fmt::kItemModelShield], itemModels.count(),
                    itemModels.bytes() / 1024, itemModels.triangles(), unresolvedModels, itemModels.failed().size());
        // What the "hr_" records add (models / skins only they use counted where first baked).
        std::printf("npc race helms: %zu ChrRaces without a column, %zu hr_ records (%zu "
                    "variants, %zu B), %zu more models (%zu KB), %zu more textures (%zu KB)\n",
                    npcRaces.size(), npcHelmStats.records, npcHelmStats.variants, npcHelmStats.bytes, npcHelmModels,
                    npcHelmModelBytes / 1024, npcHelmTextures, npcHelmTextureBytes / 1024);
        size_t shown = 0;
        for (const std::string& p : itemModels.failed())
            if (dump && ++shown <= 200) std::printf("  model did not bake: %s\n", p.c_str());
    }
    std::printf("item table: %zu Item.dbc rows in %zu blocks (%zu KB)\n", itemRows.size(), itemTable.size(),
                tableBytes / 1024);
    if (written.empty()) return 1;
    const auto tWrite = std::chrono::steady_clock::now();
    if (!writer.finish()) return 1;
    std::printf("time: bake %.0f s, write %.0f s, total %.0f s\n", bakeSeconds, secondsSince(tWrite), secondsSince(t0));
    std::fflush(stdout);
    int verifyErrors = 0;
    if (verify) {
        const auto tVerify = std::chrono::steady_clock::now();
        verifyErrors = verifyPak(out, written, itemRows, withModels);
        if (!npcHelmIds.empty()) verifyErrors += verifyNpcRaceHelms(out, npcHelmIds);  // the NPC races' helms
        std::printf("verify time %.0f s\n", secondsSince(tVerify));
    }
    return verifyErrors ? 3 : 0;
}
