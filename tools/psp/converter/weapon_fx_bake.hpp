// What items.pak needs for weapons, shields and their glows
// (include/rendering/psp/psp_item_format.hpp, its weapons block).
// psp_item_conv's weapon hooks call this:
//  - sheathHints: per ItemDisplayInfo id the sheath hint (item_bake.md B8) - the character select screen and a
//    server's custom item have a display but no Item.dbc row;
//  - displaySlotKinds: per ItemDisplayInfo id the kind of slot its items go in (item_bake.md B7) - the models'
//    folders (psp_item_conv ItemNaming);
//  - itemVisualPointsFooter: a weapon / shield model's attachments 0..4 (upstream
//    CharacterRenderer::attachWeaponEffect: findAttachmentBone(weapon, visual slot)'s offset, the bone not
//    applied) as the ItemVisualPoints footer of its "a_" blob;
//  - ItemVisualBank: "ivis" (SpellItemEnchantment -> ItemVisual, ItemVisuals.dbc -> the effect in each slot) and
//    the effects "ifx_<n>" (particle_bake.hpp bakeFx - one bake for the spell effects, the world's emitters and
//    these; look class kPtcItem) with their textures "ift_<n>";
//  - verifyItemVisuals: --verify's part for the above.
// Upstream (pipeline/dbc_layout.cpp resolveEnchantItemVisuals / resolveItemVisualModels): SpellItemEnchantment's
// ItemVisual is field 31 in 3.3.5a (detectEnchantmentItemVisualField), ItemVisuals.dbc = ID + 5 effect ids (slot i
// at the weapon's attachment i: an empty slot stays empty), ItemVisualEffects.dbc = ID + the model path (.mdx in
// the DBC, .m2 on disk).
#pragma once

#include "conv_common.hpp"
#include "item_conv_rules.hpp"  // clean room: item_bake.md B7 / B8
#include "model_bake.hpp"
#include "particle_bake.hpp"

#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/psp/psp_item_format.hpp"
#include "rendering/psp/psp_spellfx_format.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::pspconv {

// Item.dbc rows -> per display the sheath hint of item_bake.md B8 (items::sheathHint over the SheatheType of every
// row showing the display). (Item.dbc 3.3.5a: ID 0, DisplayInfoID 5, InventoryType 6, SheatheType 7.)
inline std::unordered_map<uint32_t, uint8_t> sheathHints(const pipeline::DBCFile& item) {
    if (item.getFieldCount() < 8) return {};
    std::unordered_map<uint32_t, std::vector<uint32_t>> types;
    for (uint32_t r = 0; r < item.getRecordCount(); ++r) types[item.getUInt32(r, 5)].push_back(item.getUInt32(r, 7));
    std::unordered_map<uint32_t, uint8_t> out;
    for (const auto& [display, v] : types) {
        const uint8_t hint = items::sheathHint(v);
        if (display && hint != items::kSheathHintUnknown) out[display] = hint;
    }
    return out;
}

// Item.dbc rows -> per display the slot kind of its items (item_bake.md B7: items::displaySlotKind over the
// InventoryType of every row showing it); displays of unknown kind are left out (the full folder order).
inline std::unordered_map<uint32_t, items::SlotKind> displaySlotKinds(const pipeline::DBCFile& item) {
    if (item.getFieldCount() < 7) return {};
    std::unordered_map<uint32_t, std::vector<uint32_t>> types;
    for (uint32_t r = 0; r < item.getRecordCount(); ++r) types[item.getUInt32(r, 5)].push_back(item.getUInt32(r, 6));
    std::unordered_map<uint32_t, items::SlotKind> out;
    for (const auto& [display, v] : types) {
        const items::SlotKind kind = items::displaySlotKind(v);
        if (display && kind != items::SlotKind::kUnknown) out[display] = kind;
    }
    return out;
}

// upstream CharacterRenderer::findAttachmentBone: the lookup table first, then a scan by id (the key-bone fallback
// of ids 1 / 2 gives the offset 0 - the origin, as a slot the model lacks).
inline bool itemAttachmentOffset(const pipeline::M2Model& m, uint32_t id, glm::vec3& out) {
    out = glm::vec3(0.0f);
    if (id < m.attachmentLookup.size()) {
        const uint16_t k = m.attachmentLookup[id];
        if (k < m.attachments.size()) {
            out = m.attachments[k].position;
            return true;
        }
    }
    for (const auto& a : m.attachments)
        if (a.id == id) {
            out = a.position;
            return true;
        }
    return false;
}

// The footer appended to a weapon / shield "a_" blob (psp_item_format.hpp ItemVisualPoints).
inline std::vector<uint8_t> itemVisualPointsFooter(const pipeline::M2Model& m) {
    fmt::ItemVisualPoints p{};
    for (int i = 0; i < fmt::kItemVisualSlots; ++i) {
        glm::vec3 o;
        if (itemAttachmentOffset(m, static_cast<uint32_t>(i), o) && fmt::itemFloatSane(o.x) && fmt::itemFloatSane(o.y) &&
            fmt::itemFloatSane(o.z)) {
            p.found = static_cast<uint8_t>(p.found | (1u << i));
            p.position[i][0] = o.x;
            p.position[i][1] = o.y;
            p.position[i][2] = o.z;
        }
    }
    std::memcpy(p.magic, "WIVP", 4);
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&p), reinterpret_cast<const uint8_t*>(&p + 1));
}

// The weapon / shield folders (bakeItemModels): only their models carry item visuals.
inline bool itemModelTakesVisuals(const std::string& gamePath) {
    const std::string l = lower(gamePath);
    return l.find("item/objectcomponents/weapon/") == 0 || l.find("item/objectcomponents/shield/") == 0;
}

struct ItemVisualBakeStats {
    size_t visuals = 0, visualsUsed = 0, enchants = 0, effects = 0, effectPaths = 0, effectBytes = 0, textures = 0,
           textureBytes = 0, emitters = 0, ribbons = 0, groups = 0, displayVisuals = 0;
    std::map<std::string, int> failures;  // why -> count
    std::vector<std::string> lines;       // per effect (--dump)
};

// ItemVisuals + ItemVisualEffects + SpellItemEnchantment -> "ivis", "ifx_<n>", "ift_<n>" (into `out`).
// `displayVisuals`: the ItemVisual ids the displays name (ItemDisplayInfo field 23) - baked as well. `root`: the
// extracted tree (the effects live under Spells\, their textures anywhere). texMax: the textures' size limit.
class ItemVisualBank {
public:
    ItemVisualBank(std::string root, int texMax, std::vector<PakItem>& out)
        : root_(std::move(root)), out_(out), texItems_(), tex_(root_, texMax, texItems_) {}

    // False when the DBCs are not there (the pak then has no "ivis": no glows, as an older pak).
    bool bake(const std::set<uint32_t>& displayVisuals, ItemVisualBakeStats& st) {
        pipeline::DBCFile visuals, effects, enchants;
        auto load = [&](const char* name, pipeline::DBCFile& d) {
            std::vector<uint8_t> b;
            return readFile(root_ + "/dbfilesclient/" + name, b) && d.load(b);
        };
        if (!load("itemvisuals.dbc", visuals) || !load("itemvisualeffects.dbc", effects) ||
            !load("spellitemenchantment.dbc", enchants)) {
            std::fprintf(stderr, "item visuals: ItemVisuals / ItemVisualEffects / SpellItemEnchantment.dbc missing - "
                                 "no \"ivis\" (no glows)\n");
            return false;
        }
        // upstream detectEnchantmentItemVisualField: 31 in the 38-field 3.3.5a file (30 in 2.4.3, 19 in 1.12).
        const uint32_t fc = enchants.getFieldCount();
        const uint32_t visualField = fc >= 38 ? 31u : fc >= 34 ? 30u : 19u;
        std::unordered_map<uint32_t, uint32_t> visualRow;
        for (uint32_t r = 0; r < visuals.getRecordCount(); ++r) visualRow[visuals.getUInt32(r, 0)] = r;
        st.visuals = visualRow.size();
        std::vector<fmt::EnchantVisualRec> enchantRecs;
        std::set<uint32_t> used(displayVisuals.begin(), displayVisuals.end());
        for (uint32_t r = 0; r < enchants.getRecordCount() && visualField < fc; ++r) {
            const uint32_t id = enchants.getUInt32(r, 0), v = enchants.getUInt32(r, visualField);
            if (!id || !v || !visualRow.count(v)) continue;
            if (id > 0xFFFFu || v > 0xFFFFu) {
                std::fprintf(stderr, "item visuals: enchantment %u / visual %u past u16 - skipped\n", id, v);
                continue;
            }
            enchantRecs.push_back({static_cast<uint16_t>(id), static_cast<uint16_t>(v)});
            used.insert(v);
        }
        std::sort(enchantRecs.begin(), enchantRecs.end(),
                  [](const fmt::EnchantVisualRec& a, const fmt::EnchantVisualRec& b) { return a.enchant < b.enchant; });
        st.enchants = enchantRecs.size();
        std::unordered_map<uint32_t, std::string> effectPath;
        for (uint32_t r = 0; r < effects.getRecordCount(); ++r) {
            const std::string p = effects.getString(r, 1);
            if (!p.empty()) effectPath[effects.getUInt32(r, 0)] = pipeline::modelPathToM2(p);
        }
        std::vector<fmt::ItemVisualRec> visualRecs;
        for (uint32_t v : used) {
            auto row = visualRow.find(v);
            if (row == visualRow.end() || v > 0xFFFFu) continue;
            fmt::ItemVisualRec rec{};
            rec.visual = static_cast<uint16_t>(v);
            bool any = false;
            for (int s = 0; s < fmt::kItemVisualSlots; ++s) {
                rec.effect[s] = sfx::kFxNone;
                const uint32_t field = static_cast<uint32_t>(s) + 1u;
                if (field >= visuals.getFieldCount()) continue;
                auto p = effectPath.find(visuals.getUInt32(row->second, field));
                if (p == effectPath.end()) continue;  // upstream: an effect id with no row - the slot stays empty
                const int e = effectFor(p->second, st);
                if (e >= 0) {
                    rec.effect[s] = static_cast<uint16_t>(e);
                    any = true;
                }
            }
            if (any) visualRecs.push_back(rec);
        }
        st.visualsUsed = visualRecs.size();
        st.displayVisuals = displayVisuals.size();
        // The table, then the effects and textures (textures renamed "x_" -> "ift_").
        fmt::ItemVisualTableHeader h{};
        std::memcpy(h.magic, "WIVT", 4);
        h.version = fmt::kItemVisualTableVersion;
        h.visualCount = static_cast<uint32_t>(visualRecs.size());
        h.enchantCount = static_cast<uint32_t>(enchantRecs.size());
        h.effectCount = static_cast<uint32_t>(effects_.size());
        h.textureCount = static_cast<uint32_t>(tex_.count());
        std::vector<uint8_t> table(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
        table.insert(table.end(), reinterpret_cast<const uint8_t*>(visualRecs.data()),
                     reinterpret_cast<const uint8_t*>(visualRecs.data() + visualRecs.size()));
        table.insert(table.end(), reinterpret_cast<const uint8_t*>(enchantRecs.data()),
                     reinterpret_cast<const uint8_t*>(enchantRecs.data() + enchantRecs.size()));
        if (!fmt::itemVisualTableValid(table.data(), static_cast<uint32_t>(table.size()))) {
            std::fprintf(stderr, "item visuals: table self-check failed\n");
            return false;
        }
        out_.push_back({"ivis", 3, std::move(table)});
        for (size_t i = 0; i < effects_.size(); ++i) {
            st.effectBytes += effects_[i].size();
            out_.push_back({"ifx_" + std::to_string(i), 3, std::move(effects_[i])});
        }
        for (PakItem& t : texItems_) {
            st.textureBytes += t.blob.size();
            t.name = "ift_" + t.name.substr(2);
            out_.push_back(std::move(t));
        }
        st.effects = effects_.size();
        st.textures = tex_.count();
        texItems_.clear();
        effects_.clear();
        return true;
    }

private:
    // One effect model -> "ifx_<n>" (bakeFx, the look class kPtcItem), -1 when it does not bake.
    int effectFor(const std::string& m2Path, ItemVisualBakeStats& st) {
        const std::string key = lower(m2Path);
        auto it = effectOf_.find(key);
        if (it != effectOf_.end()) return it->second;
        ++st.effectPaths;
        int id = -1;
        std::vector<uint8_t> m2Bytes;
        if (readFile(root_ + "/" + key, m2Bytes)) {
            pipeline::M2Model m = pipeline::M2Loader::load(m2Bytes);
            if (m.version >= 264) {
                std::vector<uint8_t> skin;
                if (readFile(root_ + "/" + lower(pipeline::skinPathForM2(m2Path)), skin)) pipeline::M2Loader::loadSkin(skin, m);
            }
            FxBakeOptions o;
            o.cls = sfx::kPtcItem;  // the glow's own scale and colour, every batch of it additive (upstream)
            o.mesh = true;
            o.ribbons = true;
            o.forceV2 = true;
            o.maxEmitters = sfx::kFxSpellEmitters;
            std::vector<uint8_t> blob;
            FxBakeStats fs;
            std::set<std::string> missing;
            const FxReadFn read = [this](const std::string& p, std::vector<uint8_t>& out) {
                return readFile(root_ + "/" + lower(p), out);
            };
            if (bakeFx(m2Path, m2Bytes, m, o, tex_, read, missing, blob, fs)) {
                id = static_cast<int>(effects_.size());
                char line[320];
                std::snprintf(line, sizeof(line), "  ifx_%d %s: %zu B, emitters %zu/%zu, ribbons %zu, groups %zu, tex %zu, "
                              "reach %.2f", id, key.c_str(), blob.size(), fs.emitters, fs.sourceEmitters, fs.ribbonsKept,
                              fs.groups, fs.textures, static_cast<double>(fs.reach));
                st.lines.push_back(line);
                effects_.push_back(std::move(blob));
                st.emitters += fs.emitters;
                st.ribbons += fs.ribbonsKept;
                st.groups += fs.groups;
            } else {
                ++st.failures[fs.why.empty() ? "unbakeable" : fs.why];
                st.lines.push_back("  (not baked) " + key + ": " + fs.why);
            }
        } else {
            ++st.failures["missing file"];
            st.lines.push_back("  (missing) " + key);
        }
        effectOf_.emplace(key, id);
        return id;
    }

    std::string root_;
    std::vector<PakItem>& out_;
    std::vector<PakItem> texItems_;
    TextureBank tex_;
    std::map<std::string, int> effectOf_;
    std::vector<std::vector<uint8_t>> effects_;
};

// --verify: "ivis", every "ifx_" (spellFxValid, v2, its textures "ift_" are WTEX). `load(name, out)` reads a record.
// Returns the errors; `summary` gets one line.
inline int verifyItemVisuals(const std::function<bool(const std::string&, std::vector<uint8_t>&)>& load,
                             std::string& summary) {
    std::vector<uint8_t> table, b;
    if (!load("ivis", table)) {
        summary = "no ivis (an older bake: no glows)";
        return 0;
    }
    int errors = 0;
    if (!fmt::itemVisualTableValid(table.data(), static_cast<uint32_t>(table.size()))) {
        summary = "ivis invalid";
        return 1;
    }
    fmt::ItemVisualTableHeader h;
    std::memcpy(&h, table.data(), sizeof(h));
    std::set<uint32_t> textures;
    size_t effectBytes = 0;
    for (uint32_t i = 0; i < h.effectCount; ++i) {
        if (!load("ifx_" + std::to_string(i), b) || !sfx::spellFxValid(b.data(), static_cast<uint32_t>(b.size())) ||
            !sfx::spellFxExt(b.data())) {
            std::fprintf(stderr, "verify: ifx_%u missing or invalid\n", i);
            ++errors;
            continue;
        }
        effectBytes += b.size();
        sfx::SpellFxHeader fh;
        std::memcpy(&fh, b.data(), sizeof(fh));
        for (uint32_t t = 0; t < fh.textureCount; ++t) {
            uint16_t id;
            std::memcpy(&id, b.data() + fh.textureOffset + t * 2u, 2);
            if (id >= h.textureCount) {
                std::fprintf(stderr, "verify: ifx_%u texture %u past the table\n", i, id);
                ++errors;
            }
            textures.insert(id);
        }
    }
    size_t textureBytes = 0;
    for (uint32_t t = 0; t < h.textureCount; ++t) {
        if (!load("ift_" + std::to_string(t), b) || b.size() < 32 || std::memcmp(b.data(), "WTEX", 4) != 0) {
            std::fprintf(stderr, "verify: ift_%u missing or not WTEX\n", t);
            ++errors;
            continue;
        }
        textureBytes += b.size();
    }
    char line[256];
    std::snprintf(line, sizeof(line), "ivis: %u visuals, %u enchantments, %u effects (%zu KB), %u textures (%zu KB), %d errors",
                  h.visualCount, h.enchantCount, h.effectCount, effectBytes / 1024, h.textureCount, textureBytes / 1024,
                  errors);
    summary = line;
    return errors;
}

} // namespace wowee::pspconv
