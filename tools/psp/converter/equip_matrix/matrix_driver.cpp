// psp_equip_preview --matrix.
//
// Every ItemDisplayInfo that Item.dbc puts in a visible slot, on all 20
// race/sex models, dressed twice:
//   PSP      - the PSP's own code (psp_equipment.cpp: applyEquipmentGeosets,
//              equipmentLayers, capeTexture, equipmentAttachments) over the
//              items.pak / characters.pak the PSP reads, the way
//              CharacterModel::dress / attachEquipment use them;
//   upstream - upstream WoWee's own equipment code run on the PC
//              (upstream_ref.hpp: EntitySpawner for other players,
//              AppearanceComposer + GameScreen rules for the own player).
// Compared automatically: the geosets drawn (each side against its own
// character model), the body regions painted and with what (the region
// order, and the art itself: upstream's BLP resampled the way the bake does
// against the PSP layer), the cape texture, the models on attachment points
// (point, model geometry, skin, the sheath transform). Every difference is
// one case of a class; classes are counted per item display, race/sex and
// inventory type, with examples. A few cases per class are drawn for the eye
// (body texture, upstream | PSP).
// Beyond PSP vs upstream (where both can be wrong the same way): the rules
// against the race/sex model itself (a geoset switched on that the model
// lacks, an item GeosetGroup not drawn), helms against HelmetGeosetVisData,
// a reference placement (placement_oracle.hpp, weapon_placement spec 3.5) as a third column,
// GeosetGroup columns nobody reads, the layer order of ItemSets, and a sample
// of humanoid NPCs (creatures.pak against EntitySpawner::spawnOnlineCreature).
#include "placement_oracle.hpp"  // clean room: weapon_placement.md 3.5
#include "upstream_ref.hpp"

#include "core/geoset_rules.hpp"
#include "platform/psp/psp_pak.hpp"
#include "rendering/psp/psp_body_rules.hpp"
#include "rendering/psp/psp_equipment.hpp"
#include "rendering/psp/psp_item_format.hpp"
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_skeleton.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef WOWEE_MATRIX_REPO
#define WOWEE_MATRIX_REPO "."
#endif

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;
namespace core = wowee::core;

namespace eqm {

namespace {

constexpr uint8_t kRaces[10] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
const char* raceName(int r) {
    switch (r) {
        case 1: return "Human";
        case 2: return "Orc";
        case 3: return "Dwarf";
        case 4: return "NightElf";
        case 5: return "Undead";
        case 6: return "Tauren";
        case 7: return "Gnome";
        case 8: return "Troll";
        case 10: return "BloodElf";
        case 11: return "Draenei";
        default: return "?";
    }
}
std::string rsName(int race, int sex) { return std::string(raceName(race)) + (sex ? "F" : "M"); }

const char* invName(int t) {
    switch (t) {
        case 1: return "head";
        case 3: return "shoulders";
        case 4: return "shirt";
        case 5: return "chest";
        case 6: return "waist";
        case 7: return "legs";
        case 8: return "feet";
        case 9: return "wrists";
        case 10: return "hands";
        case 13: return "one-hand";
        case 14: return "shield";
        case 15: return "bow";
        case 16: return "cloak";
        case 17: return "two-hand";
        case 19: return "tabard";
        case 20: return "robe";
        case 21: return "main-hand";
        case 22: return "off-hand";
        case 23: return "held-in-off-hand";
        case 25: return "thrown";
        case 26: return "gun/crossbow/wand";
        case 28: return "relic";
        default: return "?";
    }
}

const char* groupName(int g) {
    switch (g) {
        case 0: return "hair/scalp";
        case 1: return "facial 1";
        case 2: return "facial 2";
        case 3: return "facial 3";
        case 4: return "gloves";
        case 5: return "boots";
        case 6: return "tail";
        case 7: return "ears";
        case 8: return "sleeves";
        case 9: return "kneepads";
        case 10: return "chest";
        case 11: return "pants";
        case 12: return "tabard";
        case 13: return "trousers/robe";
        case 14: return "loincloth";
        case 15: return "cape";
        case 16: return "facial jewelry";
        case 17: return "eye glow";
        case 18: return "belt";
        case 19: return "tail 2";
        case 20: return "feet";
        default: return "other";
    }
}

const char* armorType(uint8_t cls, uint8_t sub) {
    if (cls == 4) {
        switch (sub) {
            case 0: return "misc";
            case 1: return "cloth";
            case 2: return "leather";
            case 3: return "mail";
            case 4: return "plate";
            case 6: return "shield";
            default: return "armor";
        }
    }
    if (cls == 2) {
        switch (sub) {
            case 0: return "axe1h";
            case 1: return "axe2h";
            case 2: return "bow";
            case 3: return "gun";
            case 4: return "mace1h";
            case 5: return "mace2h";
            case 6: return "polearm";
            case 7: return "sword1h";
            case 8: return "sword2h";
            case 10: return "staff";
            case 13: return "fist";
            case 14: return "misc";
            case 15: return "dagger";
            case 16: return "thrown";
            case 18: return "crossbow";
            case 19: return "wand";
            case 20: return "fishing pole";
            default: return "weapon";
        }
    }
    return "other";
}

const char* slotName(int s) {
    static const char* const k[19] = {"head", "neck", "shoulders", "shirt", "chest", "waist", "legs", "feet", "wrists",
                                      "hands", "finger1", "finger2", "trinket1", "trinket2", "back", "main hand",
                                      "off hand", "ranged", "tabard"};
    return s >= 0 && s < 19 ? k[s] : "?";
}

const char* regionName(int r) {
    static const char* const k[8] = {"ArmUpper", "ArmLower", "Hand", "TorsoUpper", "TorsoLower", "LegUpper", "LegLower", "Foot"};
    return r >= 0 && r < 8 ? k[r] : "?";
}

const char* sheatheName(int t) {
    switch (t) {
        case 0: return "0 none";
        case 1: return "1 two-hand";
        case 2: return "2 staff";
        case 3: return "3 one-hand";
        case 4: return "4 shield";
        case 5: return "5 rod";
        case 6: return "6 off-hand";
        case 7: return "7";
        default: return "?";
    }
}

// The reference placement of a single worn weapon (placement_oracle.hpp expectedSheathPoint): -1 = not shown.
int referencePoint(int slot, uint8_t inv, uint8_t sheathe, uint8_t cls, uint8_t sub, int state) {
    const uint16_t p = wowee::pspconv::expectedSheathPoint(slot, inv, sheathe, cls, sub, static_cast<uint8_t>(state));
    return p == rp::kNoSheathPoint ? -1 : static_cast<int>(p);
}

// The tabard's geoset by the outfit rules (psp_body_rules.hpp outfitGeosets): an outfit of the tabard alone.
uint16_t tabardGeosetOf(uint32_t geosetGroup1) {
    namespace body = rp::body;
    body::Outfit o;
    body::Item& t = o.item[body::kTabard];
    t.present = true;
    t.geosetGroup[0] = static_cast<uint16_t>(geosetGroup1);
    body::Dress d;
    body::buildDress(o, d);
    std::unordered_set<uint16_t> vis;
    body::outfitGeosets(vis, o, d);
    uint16_t want = 1201;
    for (uint16_t g : vis)
        if (g / 100 == 12 && g != 1201) want = g;
    return want;
}

std::string pointName(int p) { return p < 0 ? std::string("not shown") : std::to_string(p); }

// Equipment slot of an inventory type (psp_equip_preview slotFor; 0..18 EquipSlot order).
int slotFor(uint8_t inv) {
    switch (inv) {
        case 1: return 0;
        case 2: return 1;
        case 3: return 2;
        case 4: return 3;
        case 5: case 20: return 4;
        case 6: return 5;
        case 7: return 6;
        case 8: return 7;
        case 9: return 8;
        case 10: return 9;
        case 16: return 14;
        case 19: return 18;
        case 13: case 17: case 21: return 15;
        case 14: case 22: case 23: return 16;
        case 15: case 25: case 26: case 28: return 17;
        default: return -1;
    }
}

// --- a WDBC table -----------------------------------------------------------
struct Dbc {
    std::vector<uint8_t> b;
    uint32_t rows = 0, fields = 0, recSize = 0, strSize = 0;
    bool load(std::vector<uint8_t> bytes) {
        b = std::move(bytes);
        if (b.size() < 20 || std::memcmp(b.data(), "WDBC", 4) != 0) return false;
        std::memcpy(&rows, b.data() + 4, 4);
        std::memcpy(&fields, b.data() + 8, 4);
        std::memcpy(&recSize, b.data() + 12, 4);
        std::memcpy(&strSize, b.data() + 16, 4);
        return b.size() >= 20u + size_t{rows} * recSize + strSize;
    }
    uint32_t u32(uint32_t r, uint32_t f) const {
        uint32_t v = 0;
        if (r < rows && f < fields) std::memcpy(&v, b.data() + 20 + size_t{r} * recSize + f * 4u, 4);
        return v;
    }
    std::string str(uint32_t r, uint32_t f) const {
        const uint32_t o = u32(r, f);
        const size_t base = 20 + size_t{rows} * recSize;
        if (o >= strSize) return {};
        return std::string(reinterpret_cast<const char*>(b.data() + base + o));
    }
};

// --- findings ---------------------------------------------------------------
struct Finding {
    std::string task;
    std::set<uint64_t> pairs;  // display << 8 | inventory type
    std::set<int> raceSex;     // race * 2 + sex
    std::map<int, size_t> byInv;
    size_t cases = 0;
    std::vector<std::string> examples;
    struct Sample {
        uint32_t display = 0;
        uint8_t inv = 0, slot = 0, race = 0, sex = 0;
        bool local = false;
        std::string set;  // ItemSet / combo name
        std::array<uint32_t, 19> disp{};  // an outfit (set): every slot
        std::array<uint8_t, 19> invs{};
    };
    std::vector<Sample> samples;
};

class Report {
public:
    void add(const std::string& cls, const char* task, uint32_t display, uint8_t inv, uint8_t race, uint8_t sex,
             const std::string& detail, const Finding::Sample& s) {
        Finding& f = f_[cls];
        if (f.task.empty()) f.task = task;
        f.pairs.insert((uint64_t{display} << 8) | inv);
        f.raceSex.insert(race * 2 + sex);
        ++f.byInv[inv];
        ++f.cases;
        if (f.examples.size() < 12) {
            std::string e = "display " + std::to_string(display) + " (" + invName(inv) + ") " + rsName(race, sex);
            if (!s.set.empty()) e += " [" + s.set + "]";
            if (!detail.empty()) e += ": " + detail;
            f.examples.push_back(e);
        }
        if (f.samples.size() < 6) f.samples.push_back(s);
        if (tsv_) {
            std::fprintf(tsv_, "%s\t%s\t%u\t%u\t%s\t%s\t%s\t%s\n", task, cls.c_str(), display, inv, invName(inv),
                         rsName(race, sex).c_str(), s.set.c_str(), detail.c_str());
        }
    }
    void openTsv(const std::string& path) {
        tsv_ = std::fopen(path.c_str(), "wb");
        if (tsv_) std::fprintf(tsv_, "task\tclass\tdisplay\tinv\tinv_name\trace_sex\tset\tdetail\n");
    }
    void closeTsv() {
        if (tsv_) std::fclose(tsv_);
        tsv_ = nullptr;
    }
    const std::map<std::string, Finding>& all() const { return f_; }

private:
    std::map<std::string, Finding> f_;
    std::FILE* tsv_ = nullptr;
};

// --- PSP side ---------------------------------------------------------------
struct PspChar {
    bool ok = false;
    std::unordered_set<uint16_t> geosetsN, geosetsC;  // skinned "n_" / static "c_" batch geosets
    std::vector<uint16_t> scalp;                     // per hair style
    std::vector<std::array<uint16_t, 3>> facial;     // per facial style
    std::vector<rp::AttachPoint> points;
    std::array<uint16_t, rp::kBodyLayers> body{};    // skin 0 face 0 layers ("l_")
    const std::unordered_set<uint16_t>& geosets() const { return geosetsN.empty() ? geosetsC : geosetsN; }
};

template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

PspChar loadPspChar(pp::Pak& chars, int race, int sex) {
    PspChar c;
    char name[32];
    std::snprintf(name, sizeof(name), "n_%d_%d", race, sex);
    if (const pp::PakEntry* e = chars.find(name)) {
        auto* b = static_cast<uint8_t*>(chars.load(e));
        rp::SkinView v;
        if (b && v.init(b, e->size))
            for (uint32_t i = 0; i < v.h->batchCount; ++i) c.geosetsN.insert(v.geosets[i]);
        if (b) pp::pakFree(b);
    }
    std::snprintf(name, sizeof(name), "c_%d_%d", race, sex);
    if (const pp::PakEntry* e = chars.find(name)) {
        auto* b = static_cast<uint8_t*>(chars.load(e));
        if (b && e->size >= sizeof(rp::ModelHeader)) {
            rp::ModelHeader h;
            std::memcpy(&h, b, sizeof(h));
            if (h.geosetOffset && h.geosetOffset + h.batchCount[0] * 2u <= e->size)
                for (uint32_t i = 0; i < h.batchCount[0]; ++i) c.geosetsC.insert(rd<uint16_t>(b + h.geosetOffset + i * 2));
        }
        if (b) pp::pakFree(b);
    }
    std::snprintf(name, sizeof(name), "a_%d_%d", race, sex);
    if (const pp::PakEntry* e = chars.find(name)) {
        auto* b = static_cast<uint8_t*>(chars.load(e));
        if (b && e->size >= sizeof(rp::AppearanceHeader) && std::memcmp(b, "WAPP", 4) == 0) {
            rp::AppearanceHeader ah;
            std::memcpy(&ah, b, sizeof(ah));
            for (uint32_t s = 0; s < ah.hairStyles; ++s) {
                const uint32_t at = ah.hairGeosetOffset + s * 2u;
                uint16_t g = at + 2 <= e->size ? rd<uint16_t>(b + at) : 0;
                c.scalp.push_back(g ? g : 1);  // CharacterModel::load: 0 = default 1
            }
            for (uint32_t f = 0; f < ah.facials; ++f) {
                const uint32_t at = ah.facialOffset + f * 6u;
                std::array<uint16_t, 3> v{1, 1, 1};
                if (at + 6 <= e->size)
                    for (int k = 0; k < 3; ++k) v[k] = rd<uint16_t>(b + at + k * 2);
                c.facial.push_back(v);
            }
            const uint32_t bodyAt = ah.bodyOffset;  // skin 0, face 0
            for (int i = 0; i < rp::kBodyLayers; ++i)
                c.body[i] = bodyAt + (i + 1) * 2u <= e->size ? rd<uint16_t>(b + bodyAt + i * 2) : rp::kNoLayer;
        }
        if (b) pp::pakFree(b);
    }
    std::snprintf(name, sizeof(name), "p_%d_%d", race, sex);
    if (const pp::PakEntry* e = chars.find(name)) {
        auto* b = static_cast<uint8_t*>(chars.load(e));
        if (b && e->size >= sizeof(rp::AttachTable) && std::memcmp(b, "WATP", 4) == 0) {
            rp::AttachTable t;
            std::memcpy(&t, b, sizeof(t));
            if (e->size >= sizeof(t) + t.count * sizeof(rp::AttachPoint)) {
                c.points.resize(t.count);
                std::memcpy(c.points.data(), b + sizeof(t), t.count * sizeof(rp::AttachPoint));
            }
        }
        if (b) pp::pakFree(b);
    }
    c.ok = !c.geosets().empty() && !c.scalp.empty();
    return c;
}

struct PspDress {
    std::set<uint16_t> requested;       // the whole set the PSP builds (what its rules switch on)
    std::set<uint16_t> geosets;         // drawn: the set the PSP builds, on its model's geosets
    std::vector<uint16_t> layers;       // "q_" in order
    uint16_t cape = rp::kNoItemLayer;   // "t_"
    std::vector<rp::AttachPick> picks;  // equipmentAttachments
};

// CharacterModel::dress / attachEquipment (psp_character.cpp) for one look.
PspDress pspDress(const PspChar& c, rp::ItemBank& bank, const rp::Equipment& eq, uint8_t race, uint8_t sex,
                  uint8_t hair, uint8_t facial, rp::AttachRules rules, bool sheathed) {
    PspDress d;
    const uint16_t scalp = c.scalp[std::min<size_t>(hair, c.scalp.size() - 1)];
    std::array<uint16_t, 3> f{1, 1, 1};
    if (!c.facial.empty()) f = c.facial[std::min<size_t>(facial, c.facial.size() - 1)];
    auto g = core::bareCharacterGeosets(scalp, f[0], f[1], f[2], race);
    g.insert(core::kGeosetNoCape);
    const bool dressed = !eq.empty();
    if (dressed) rp::applyEquipmentGeosets(g, scalp, hair > 0, eq, sex, bank, c.geosets());
    d.requested.insert(g.begin(), g.end());
    for (uint16_t v : g)
        if (c.geosets().count(v)) d.geosets.insert(v);
    if (dressed) {
        d.layers = rp::equipmentLayers(eq, sex, bank);
        d.cape = rp::capeTexture(eq, sex, bank);
        d.picks = rp::equipmentAttachments(eq, race, sex, rules, sheathed, bank);
    }
    return d;
}

// --- images -----------------------------------------------------------------
struct Rgba {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // RGBA8
};

// tools/psp/converter/model_bake.hpp resample: area average, colour alpha-weighted.
Rgba resampleLikeBake(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh) {
    Rgba out;
    out.w = dw;
    out.h = dh;
    out.px.assign(static_cast<size_t>(dw) * dh * 4, 0);
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            float sr = 0, sg = 0, sb = 0, sa = 0, ur = 0, ug = 0, ub = 0;
            int n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = &src[(static_cast<size_t>(yy) * sw + xx) * 4];
                    const float a = p[3] / 255.0f;
                    sr += p[0] * a;
                    sg += p[1] * a;
                    sb += p[2] * a;
                    sa += a;
                    ur += p[0];
                    ug += p[1];
                    ub += p[2];
                    ++n;
                }
            uint8_t* o = &out.px[(static_cast<size_t>(y) * dw + x) * 4];
            if (sa > 1e-4f) {
                o[0] = static_cast<uint8_t>(std::lround(std::clamp(sr / sa, 0.0f, 255.0f)));
                o[1] = static_cast<uint8_t>(std::lround(std::clamp(sg / sa, 0.0f, 255.0f)));
                o[2] = static_cast<uint8_t>(std::lround(std::clamp(sb / sa, 0.0f, 255.0f)));
                o[3] = static_cast<uint8_t>(std::lround(std::clamp(sa / n * 255.0f, 0.0f, 255.0f)));
            } else {
                o[0] = static_cast<uint8_t>(ur / n);
                o[1] = static_cast<uint8_t>(ug / n);
                o[2] = static_cast<uint8_t>(ub / n);
                o[3] = 0;
            }
        }
    }
    return out;
}

// Mean difference of two equal-size images, 0..255: colour weighted by the larger alpha, plus alpha.
double imageDiff(const Rgba& a, const Rgba& b) {
    if (a.w != b.w || a.h != b.h || a.px.empty()) return 999.0;
    double sum = 0;
    const size_t n = static_cast<size_t>(a.w) * a.h;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &a.px[i * 4];
        const uint8_t* q = &b.px[i * 4];
        const double w = std::max(p[3], q[3]) / 255.0;
        const double dc = (std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2])) / 3.0;
        sum += dc * w + std::abs(p[3] - q[3]);
    }
    return sum / static_cast<double>(n);
}

struct PspLayer {
    bool ok = false;
    int x = 0, y = 0;
    Rgba img;
};

// A WTEX blob's level 0: T8 swizzled (16-byte x 8-row blocks) + CLUT (model_bake.hpp TextureBank).
bool decodeWtex(const uint8_t* b, uint32_t size, Rgba& out) {
    if (!b || size < 32 + 1024 || std::memcmp(b, "WTEX", 4) != 0 || b[4] != 5) return false;
    uint16_t w, h;
    uint32_t clutAt, pixAt;
    std::memcpy(&w, b + 8, 2);
    std::memcpy(&h, b + 10, 2);
    std::memcpy(&clutAt, b + 24, 4);
    std::memcpy(&pixAt, b + 28, 4);
    const bool swz = b[5] != 0;
    if (pixAt + uint32_t{w} * h > size || clutAt + 1024 > size || w < 16 || h < 8) return false;
    out.w = w;
    out.h = h;
    out.px.assign(size_t{w} * h * 4, 0);
    const uint8_t* src = b + pixAt;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t o = static_cast<size_t>(y) * w + x;
            if (swz) o = (static_cast<size_t>(y / 8) * (w / 16) + x / 16) * 128 + (y % 8) * 16 + x % 16;
            std::memcpy(&out.px[(static_cast<size_t>(y) * w + x) * 4], b + clutAt + src[o] * 4u, 4);
        }
    return true;
}

// --- the run ----------------------------------------------------------------
struct Options {
    std::string items, chars, extracted, repo = WOWEE_MATRIX_REPO, out = "equip_matrix_out";
    std::string creatures;            // creatures.pak: the NPC pass (skipped without it)
    std::vector<int> npcDisplays;     // CreatureDisplayInfo ids to check (default: a spread sample of humanoids)
    size_t npcEvery = 50;
    uint32_t npcWeapon = 0;           // an Item.dbc entry every sampled NPC holds (UNIT_VIRTUAL_ITEM_SLOT_ID), 0 none
    std::vector<std::string> mirrors;
    std::vector<int> races, sexes{0, 1}, invs;
    std::set<uint32_t> displays;
    size_t limit = 0, every = 1;
    int hair = 1, samples = 3;
    bool local = true, sets = true, combos = true, npc = true, verbose = false;
    // Mean difference (0..255) above which two pictures are different art: body layers (WLAY, 256 colours over
    // <= 64x32) stay below ~6 when the file is the same, item / cape textures (WTEX <= 128 px) below ~15.
    double layerTol = 10.0, texTol = 20.0;
};

struct Case {
    uint32_t display = 0;
    uint8_t inv = 0, slot = 0, cls = 0, sub = 0, sheathe = 0;  // Item.dbc of the first item with this display
    uint32_t item = 0, items = 0;
};

class Matrix {
public:
    Matrix(const Options& o, Upstream& up, rp::ItemBank& bank, pp::Pak& chars) : o_(o), up_(up), bank_(bank), chars_(chars) {}
    int run();

private:
    bool loadTables();
    void buildCases();
    void checkCharacters();
    void runCase(const Case& c, uint8_t race, uint8_t sex);
    void runOutfit(const std::string& name, const uint32_t disp[19], const uint8_t inv[19], uint8_t race, uint8_t sex);
    // comparisons: `who` is "other" (online rules) or "own" (local rules)
    // `other` (own-player passes): upstream's other-player result for the same look - a difference that is
    // only against upstream's own-player rule, while the PSP matches its other-player rule, is upstream
    // disagreeing with itself (the PSP uses one rule for both) and gets a class of its own.
    void compareGeosets(const char* who, const PspChar& pc, const PspDress& p, const UpResult& u,
                        const std::set<uint16_t>& upModel, const Finding::Sample& s, uint32_t display, uint8_t inv,
                        const UpResult* other = nullptr);
    void compareBody(const char* who, const PspDress& p, const UpResult& u, const Finding::Sample& s,
                     uint32_t display, uint8_t inv, uint8_t sex, bool capeOnly, const UpResult* other = nullptr);
    // `cs` + `refState` (weapon cases): the reference placement (referencePoint) as the third column -
    // state 1 drawn, 0 sheathed, -1 none.
    void compareAttach(const char* who, const PspChar& pc, const std::vector<rp::AttachPick>& picks,
                       const UpResult& u, const Finding::Sample& s, uint32_t display, uint8_t inv, bool transforms,
                       const Case* cs = nullptr, int refState = -1);
    // Against the character model itself (both sides can be wrong the same way): every geoset a rule switches on
    // for the item must be one the race/sex model has, and an item GeosetGroup that names a variant the model
    // has must be the one drawn.
    void checkModelRules(const char* who, const Case& c, const PspChar& pc, const std::set<uint16_t>& upModel,
                         const PspDress& p, const UpResult& u, const std::set<uint16_t>& pspNaked,
                         const std::set<uint16_t>& upNaked, const Finding::Sample& s);
    void geosetGroupCheck(const char* who, uint32_t display, uint8_t inv, const std::set<uint16_t>& model,
                          const std::set<uint16_t>& pspDrawn, const std::set<uint16_t>& upDrawn, const Finding::Sample& s);
    void helmVisCheck(const char* who, const Case& c, const PspChar& pc, const PspDress& p, const std::set<uint16_t>& ud,
                      const Finding::Sample& s);
    void dataPass();       // GeosetGroup columns neither side reads
    void layerOrder(const std::string& name, const uint32_t disp[19], const uint8_t inv[19], uint8_t sex);
    void writeModelGeosets(std::FILE* f);
    uint32_t idiField(uint32_t display, uint32_t field) const {
        auto it = idiRow_.find(display);
        return it == idiRow_.end() ? 0u : idi_.u32(it->second, field);
    }
    const PspLayer& layer(uint16_t q);
    double layerVsPath(uint16_t q, const std::string& path);
    double textureVsPath(uint16_t t, const std::string& path, int& tw, int& th);
    bool pspTexture(uint16_t t, Rgba& out);
    double rgbaVsPath(const Rgba& img, const std::string& path);
    struct ModelSig {
        bool ok = false;
        float bmin[3] = {}, bmax[3] = {};
        uint32_t tris = 0, verts = 0;
    };
    const ModelSig& pspModel(uint16_t a);
    const Upstream::M2Info& upModel(const std::string& path);
    std::string bestRegionMatch(uint16_t q, int region, const std::string& upPath, double& score);
    void add(const std::string& cls, const char* task, const Finding::Sample& s, uint32_t display, uint8_t inv,
             const std::string& detail) {
        report_.add(cls, task, display, inv, s.race, s.sex, detail, s);
    }
    void writeSummary(double seconds);
    void writeSamples();
    bool sampleImages(const Finding::Sample& s, Rgba& upImg, Rgba& pspImg, std::string& text);
    void npcPass();
    void dump(const char* who, const Case& c, uint8_t race, uint8_t sex, const PspDress& p, const UpResult& u,
              const std::set<uint16_t>& upModel);

    const Options& o_;
    Upstream& up_;
    rp::ItemBank& bank_;
    pp::Pak& chars_;
    Report report_;
    Dbc item_, itemSet_, cdi_, cdie_, idi_, hvis_;
    std::unordered_map<uint32_t, uint32_t> hvisRow_;  // HelmetGeosetVisData id -> row
    std::unordered_map<uint32_t, uint32_t> idiRow_;   // ItemDisplayInfo id -> row
    std::map<int, std::set<uint16_t>> pspNaked_, upNakedOther_;  // race * 2 + sex: what the naked character asks for
    std::set<std::string> layerOrderSeen_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint8_t>> itemRow_;  // item -> display, inv
    std::vector<Case> cases_;
    std::map<int, PspChar> psp_;                       // race * 2 + sex
    std::map<int, std::set<uint16_t>> upModel_;        // race * 2 + sex
    std::unordered_map<uint16_t, PspLayer> layers_;
    std::map<std::pair<uint16_t, std::string>, double> layerScore_, texScore_;
    std::unordered_map<uint16_t, ModelSig> models_;
    std::unordered_map<std::string, Upstream::M2Info> m2_;
    size_t runs_ = 0, outfits_ = 0;
};

bool Matrix::loadTables() {
    auto load = [&](Dbc& d, const char* name) { return d.load(up_.readFile(std::string("DBFilesClient\\") + name)); };
    if (!load(item_, "Item.dbc")) {
        std::fprintf(stderr, "matrix: no Item.dbc\n");
        return false;
    }
    load(itemSet_, "ItemSet.dbc");
    load(cdi_, "CreatureDisplayInfo.dbc");
    load(cdie_, "CreatureDisplayInfoExtra.dbc");
    load(idi_, "ItemDisplayInfo.dbc");
    load(hvis_, "HelmetGeosetVisData.dbc");
    for (uint32_t r = 0; r < hvis_.rows; ++r) hvisRow_[hvis_.u32(r, 0)] = r;
    for (uint32_t r = 0; r < idi_.rows; ++r) idiRow_[idi_.u32(r, 0)] = r;
    for (uint32_t r = 0; r < item_.rows; ++r)
        itemRow_[item_.u32(r, 0)] = {item_.u32(r, 5), static_cast<uint8_t>(item_.u32(r, 6))};
    return true;
}

void Matrix::buildCases() {
    std::map<std::pair<uint32_t, uint8_t>, Case> pairs;
    for (uint32_t r = 0; r < item_.rows; ++r) {
        const uint32_t display = item_.u32(r, 5);
        const uint8_t inv = static_cast<uint8_t>(item_.u32(r, 6));
        if (!display || slotFor(inv) < 0 || inv == 2 || inv == 11 || inv == 12) continue;
        if (!o_.invs.empty() && std::find(o_.invs.begin(), o_.invs.end(), inv) == o_.invs.end()) continue;
        if (!o_.displays.empty() && !o_.displays.count(display)) continue;
        Case& c = pairs[{display, inv}];
        if (!c.display) {
            c.display = display;
            c.inv = inv;
            c.slot = static_cast<uint8_t>(slotFor(inv));
            c.item = item_.u32(r, 0);
            c.cls = static_cast<uint8_t>(item_.u32(r, 1));
            c.sub = static_cast<uint8_t>(item_.u32(r, 2));
            c.sheathe = static_cast<uint8_t>(item_.u32(r, 7));
        }
        ++c.items;
    }
    for (auto& [k, c] : pairs) {
        cases_.push_back(c);
        // A one-hand weapon (13) or an off-hand one (22) is also worn in the off hand.
        if (c.inv == 13) {
            Case off = c;
            off.slot = 16;
            cases_.push_back(off);
        }
    }
    if (o_.every > 1) {  // a spread sample: every n-th case
        std::vector<Case> kept;
        for (size_t i = 0; i < cases_.size(); i += o_.every) kept.push_back(cases_[i]);
        cases_.swap(kept);
    }
    if (o_.limit && cases_.size() > o_.limit) cases_.resize(o_.limit);
}

// Per race/sex: the character model's geosets, hair and facial geosets, the attachment points.
void Matrix::checkCharacters() {
    for (int race : o_.races)
        for (int sex : o_.sexes) {
            const int k = race * 2 + sex;
            PspChar pc = loadPspChar(chars_, race, sex);
            std::set<uint16_t> um = up_.modelGeosets(static_cast<uint8_t>(race), static_cast<uint8_t>(sex));
            upModel_[k] = um;
            Finding::Sample s;
            s.race = static_cast<uint8_t>(race);
            s.sex = static_cast<uint8_t>(sex);
            if (!pc.ok || um.empty()) {
                add("character: model missing", "chars", s, 0, 0, pc.ok ? "upstream model did not load" : "no n_/c_/a_ in characters.pak");
                psp_[k] = std::move(pc);
                continue;
            }
            // Geosets of the model the PSP draws (n_, else c_) against upstream's M2 (skin 00 submeshes).
            std::vector<uint16_t> onlyPsp, onlyUp;
            for (uint16_t g : pc.geosets())
                if (!um.count(g)) onlyPsp.push_back(g);
            for (uint16_t g : um)
                if (!pc.geosets().count(g)) onlyUp.push_back(g);
            std::sort(onlyPsp.begin(), onlyPsp.end());
            if (!onlyPsp.empty() || !onlyUp.empty()) {
                std::string d = "PSP model lacks";
                for (uint16_t g : onlyUp) d += " " + std::to_string(g);
                d += "; PSP only";
                for (uint16_t g : onlyPsp) d += " " + std::to_string(g);
                add("character: model geosets differ (n_ vs M2)", "chars", s, 0, 0, d);
            }
            if (!pc.geosetsN.empty() && !pc.geosetsC.empty() && pc.geosetsN != pc.geosetsC)
                add("character: n_ and c_ geosets differ", "chars", s, 0, 0, "");
            // Hair scalp per style, facial 0.
            for (size_t h = 0; h < pc.scalp.size(); ++h) {
                const uint16_t u = up_.hairScalp(static_cast<uint8_t>(race), static_cast<uint8_t>(sex), static_cast<uint8_t>(h));
                if (u != pc.scalp[h])
                    add("character: hair scalp geoset differs", "chars", s, 0, 0,
                        "style " + std::to_string(h) + ": PSP " + std::to_string(pc.scalp[h]) + " upstream " + std::to_string(u));
            }
            bool found = false;
            const auto uf = up_.facialGeosets(static_cast<uint8_t>(race), static_cast<uint8_t>(sex), 0, found);
            if (!pc.facial.empty() && (pc.facial[0][0] != uf[0] || pc.facial[0][1] != uf[1] || pc.facial[0][2] != uf[2]))
                add("character: facial geosets differ", "chars", s, 0, 0,
                    "PSP " + std::to_string(pc.facial[0][0]) + "/" + std::to_string(pc.facial[0][1]) + "/" +
                        std::to_string(pc.facial[0][2]) + " upstream " + std::to_string(uf[0]) + "/" +
                        std::to_string(uf[1]) + "/" + std::to_string(uf[2]));
            // Attachment points: what upstream's findAttachmentBone resolves vs the PSP table.
            for (uint32_t point : {0u, 1u, 2u, 5u, 6u, 11u, 12u, 26u, 27u, 28u, 32u, 33u}) {
                const bool u = up_.hasAttachment(static_cast<uint8_t>(race), static_cast<uint8_t>(sex), point);
                bool p = false;
                for (const auto& q : pc.points) p = p || q.id == point;
                if (u != p)
                    add("character: attachment point differs", "chars", s, 0, 0,
                        "point " + std::to_string(point) + ": upstream " + (u ? "yes" : "no") + ", PSP " + (p ? "yes" : "no"));
            }
            // Naked: the PSP draws the bare set (no equipment pass); upstream's equipment update with every slot
            // empty still runs setOnlinePlayerEquipment (other players) / GameScreen (the own player).
            uint32_t disp[19] = {};
            uint8_t inv[19] = {};
            rp::Equipment none;
            const PspDress p = pspDress(pc, bank_, none, static_cast<uint8_t>(race), static_cast<uint8_t>(sex),
                                        static_cast<uint8_t>(o_.hair), 0, rp::AttachRules::Online, false);
            UpResult u, uo;
            const bool haveOther =
                up_.online(static_cast<uint8_t>(race), static_cast<uint8_t>(sex), static_cast<uint8_t>(o_.hair), 0, disp, inv, uo);
            pspNaked_[k] = p.requested;
            if (haveOther) {
                upNakedOther_[k] = uo.geosets;
                compareGeosets("naked other", pc, p, uo, um, s, 0, 0);
            }
            if (o_.local && up_.local(static_cast<uint8_t>(race), static_cast<uint8_t>(sex), static_cast<uint8_t>(o_.hair), 0,
                                      disp, inv, false, true, true, u))
                compareGeosets("naked own", pc, p, u, um, s, 0, 0, haveOther ? &uo : nullptr);
            psp_[k] = std::move(pc);
        }
}

void Matrix::compareGeosets(const char* who, const PspChar& pc, const PspDress& p, const UpResult& u,
                            const std::set<uint16_t>& upModel, const Finding::Sample& s, uint32_t display, uint8_t inv,
                            const UpResult* other) {
    std::set<uint16_t> ud, od;
    for (uint16_t g : u.geosets)
        if (upModel.count(g)) ud.insert(g);
    if (ud == p.geosets) return;
    if (other)
        for (uint16_t g : other->geosets)
            if (upModel.count(g)) od.insert(g);
    auto groupOf = [](const std::set<uint16_t>& set, int grp) {
        std::set<uint16_t> out;
        for (uint16_t g : set)
            if (g / 100 == grp) out.insert(g);
        return out;
    };
    std::map<int, std::pair<std::string, std::string>> groups;
    for (uint16_t g : ud)
        if (!p.geosets.count(g)) groups[g / 100].second += " " + std::to_string(g);
    for (uint16_t g : p.geosets)
        if (!ud.count(g)) groups[g / 100].first += " " + std::to_string(g);
    for (const auto& [grp, d] : groups) {
        std::string psp = d.first.empty() ? std::string(" -") : d.first;
        std::string ups = d.second.empty() ? std::string(" -") : d.second;
        // An upstream geoset the PSP's own model does not carry: the conversion, not the rule.
        bool missingFromModel = false;
        std::istringstream is(d.second);
        uint16_t g = 0;
        while (is >> g) missingFromModel = missingFromModel || !pc.geosets().count(g);
        std::string cls = std::string(who) + ": geoset group " + std::to_string(grp) + " (" + groupName(grp) + ")";
        if (missingFromModel) cls += " - upstream geoset not in the PSP model";
        const char* task = "equip-body";
        if (other && groupOf(od, grp) == groupOf(p.geosets, grp)) {
            cls = std::string(who) + " [upstream own-player rule != its other-player rule; PSP = other-player]: geoset group " +
                  std::to_string(grp) + " (" + groupName(grp) + ")";
            task = "upstream";
        }
        add(cls, task, s, display, inv, "PSP" + psp + " | upstream" + ups);
    }
}

const PspLayer& Matrix::layer(uint16_t q) {
    auto it = layers_.find(q);
    if (it != layers_.end()) return it->second;
    PspLayer l;
    uint32_t size = 0;
    auto* b = static_cast<uint8_t*>(bank_.loadLayer(q, size));
    if (b && size >= sizeof(rp::LayerHeader) + 1024 && std::memcmp(b, "WLAY", 4) == 0) {
        rp::LayerHeader h;
        std::memcpy(&h, b, sizeof(h));
        if (size >= sizeof(h) + 1024u + uint32_t{h.w} * h.h) {
            l.ok = true;
            l.x = h.x;
            l.y = h.y;
            l.img.w = h.w;
            l.img.h = h.h;
            l.img.px.resize(size_t{h.w} * h.h * 4);
            const uint8_t* clut = b + sizeof(h);
            const uint8_t* px = clut + 1024;
            for (size_t i = 0; i < size_t{h.w} * h.h; ++i) std::memcpy(&l.img.px[i * 4], clut + px[i] * 4u, 4);
        }
    }
    if (b) pp::pakFree(b);
    return layers_.emplace(q, std::move(l)).first->second;
}

double Matrix::layerVsPath(uint16_t q, const std::string& path) {
    const auto key = std::make_pair(q, path);
    auto it = layerScore_.find(key);
    if (it != layerScore_.end()) return it->second;
    double score = 999.0;
    const PspLayer& l = layer(q);
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (l.ok && up_.loadBlp(path, rgba, w, h)) score = imageDiff(resampleLikeBake(rgba, w, h, l.img.w, l.img.h), l.img);
    layerScore_.emplace(key, score);
    return score;
}

// "t_<n>" of items.pak, level 0.
bool Matrix::pspTexture(uint16_t t, Rgba& out) {
    uint32_t size = 0;
    auto* b = static_cast<uint8_t*>(bank_.loadTexture(t, size));
    const bool ok = decodeWtex(b, size, out);
    if (b) pp::pakFree(b);
    return ok;
}

// A decoded PSP texture against upstream's BLP resampled the way the bake does.
double Matrix::rgbaVsPath(const Rgba& img, const std::string& path) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (img.px.empty() || !up_.loadBlp(path, rgba, w, h)) return 999.0;
    // An opaque-baked texture (TextureBank::get(opaque): alpha 1 before the halving) -
    // the same here.
    bool allOpaque = true;
    for (size_t i = 3; i < img.px.size(); i += 4) allOpaque = allOpaque && img.px[i] == 255;
    if (allOpaque)
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return imageDiff(resampleLikeBake(rgba, w, h, img.w, img.h), img);
}

double Matrix::textureVsPath(uint16_t t, const std::string& path, int& tw, int& th) {
    const auto key = std::make_pair(t, path);
    auto it = texScore_.find(key);
    if (it != texScore_.end()) return it->second;
    double score = 999.0;
    Rgba img;
    if (pspTexture(t, img)) {
        tw = img.w;
        th = img.h;
        score = rgbaVsPath(img, path);
    }
    texScore_.emplace(key, score);
    return score;
}

const Matrix::ModelSig& Matrix::pspModel(uint16_t a) {
    auto it = models_.find(a);
    if (it != models_.end()) return it->second;
    ModelSig sig;
    uint32_t size = 0;
    void* b = bank_.loadModel(a, size);
    if (b && rp::itemModelValid(b, size)) {
        rp::ModelHeader h;
        std::memcpy(&h, b, sizeof(h));
        sig.ok = true;
        sig.verts = h.vertexCount;
        for (int k = 0; k < 3; ++k) {
            sig.bmin[k] = h.center[k] - h.half[k];
            sig.bmax[k] = h.center[k] + h.half[k];
        }
        for (uint32_t i = 0; i < h.batchCount[0]; ++i) {
            rp::ModelBatch mb;
            std::memcpy(&mb, static_cast<const uint8_t*>(b) + h.batchOffset + i * sizeof(mb), sizeof(mb));
            sig.tris += (h.flags & rp::kModelStrips) ? mb.indexCount - 2 : mb.indexCount / 3;
        }
    }
    if (b) pp::pakFree(b);
    return models_.emplace(a, sig).first->second;
}

const Upstream::M2Info& Matrix::upModel(const std::string& path) {
    auto it = m2_.find(path);
    if (it != m2_.end()) return it->second;
    return m2_.emplace(path, up_.m2Info(path)).first->second;
}

// Which spelling of the region's art the PSP layer is closest to (diagnosis of a content mismatch).
std::string Matrix::bestRegionMatch(uint16_t q, int region, const std::string& upPath, double& score) {
    static const char* kDirs[8] = {"armuppertexture", "armlowertexture", "handtexture", "torsouppertexture",
                                   "torsolowertexture", "leguppertexture", "leglowertexture", "foottexture"};
    // the name without folder and suffix
    std::string base = upPath;
    const size_t slash = base.rfind('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.size() > 4) base.resize(base.size() - 4);
    for (const char* sfx : {"_m", "_f", "_u"})
        if (base.size() > 2 && base.compare(base.size() - 2, 2, sfx) == 0) base.resize(base.size() - 2);
    std::string best;
    score = 999.0;
    for (int r = 0; r < 8; ++r)
        for (const char* sfx : {"_m.blp", "_f.blp", "_u.blp", ".blp"}) {
            const std::string p = std::string("item/texturecomponents/") + kDirs[r] + "/" + base + sfx;
            if (!up_.fileExists(p)) continue;
            const double sc = layerVsPath(q, p);
            if (sc < score) {
                score = sc;
                best = p + (r == region ? "" : " (other region)");
            }
        }
    return best;
}

void Matrix::compareBody(const char* who, const PspDress& p, const UpResult& u, const Finding::Sample& s,
                         uint32_t display, uint8_t inv, uint8_t sex, bool capeOnly, const UpResult* other) {
    const std::string selfRule =
        std::string(who) + " [upstream own-player rule != its other-player rule; PSP = other-player]";
    if (!capeOnly) {
        // Region layers: upstream's (region, file) list in order against the PSP's "q_" list in order.
        std::vector<int> pr;
        for (uint16_t q : p.layers) {
            const PspLayer& l = layer(q);
            int region = -1;
            for (int r = 0; r < rp::kItemRegions; ++r)
                if (l.ok && l.x == rp::kItemRegionRect256[r][0] / 2 && l.y == rp::kItemRegionRect256[r][1] / 2 &&
                    l.img.w == rp::kItemRegionRect256[r][2] / 2 && l.img.h == rp::kItemRegionRect256[r][3] / 2)
                    region = r;
            if (region < 0) add(std::string(who) + ": body layer rect is no region", "equip-body", s, display, inv, "q_" + std::to_string(q));
            pr.push_back(region);
        }
        std::vector<int> ur, orr;
        for (const auto& [r, path] : u.regions) ur.push_back(r);
        if (other)
            for (const auto& [r, path] : other->regions) orr.push_back(r);
        if (pr != ur && other && pr == orr) {
            add(selfRule + ": body regions differ", "upstream", s, display, inv, "");
        } else if (pr != ur) {
            auto list = [](const std::vector<int>& v) {
                std::string out;
                for (int r : v) out += (out.empty() ? "" : ",") + std::to_string(r);
                return out.empty() ? std::string("-") : out;
            };
            std::string cls = std::string(who) + ": body regions differ";
            if (pr.size() < ur.size()) cls += " (PSP paints fewer)";
            else if (pr.size() > ur.size()) cls += " (PSP paints more)";
            else cls += " (same count, other regions/order)";
            add(cls, "equip-body", s, display, inv, "PSP [" + list(pr) + "] upstream [" + list(ur) + "]");
        } else {
            for (size_t i = 0; i < ur.size(); ++i) {
                const double sc = layerVsPath(p.layers[i], u.regions[i].second);
                if (sc <= o_.layerTol) continue;
                double best = 0;
                const std::string match = bestRegionMatch(p.layers[i], u.regions[i].first, u.regions[i].second, best);
                char d[512];
                std::snprintf(d, sizeof(d), "region %d: upstream %s, PSP q_%u differs %.1f; PSP is closest to %s (%.1f)",
                              u.regions[i].first, u.regions[i].second.c_str(), p.layers[i], sc,
                              match.empty() ? "none" : match.c_str(), best);
                add(std::string(who) + ": body layer art differs", "equip-body", s, display, inv, d);
            }
        }
    }
    // The cape texture (group 15 / the cloak slot).
    const bool upCape = !u.capeTexture.empty() && u.capeTexture[0] != '#';
    const bool pspCape = p.cape != rp::kNoItemLayer;
    // Whether the PSP cape is what upstream's other-player rule gives (own-player passes).
    auto otherAgrees = [&]() {
        if (!other) return false;
        const bool oc = !other->capeTexture.empty() && other->capeTexture[0] != '#';
        if (oc != pspCape) return false;
        int tw = 0, th = 0;
        return !oc || textureVsPath(p.cape, other->capeTexture, tw, th) <= o_.texTol;
    };
    if (upCape != pspCape) {
        const std::string what = upCape ? ": cape texture missing on PSP" : ": cape texture only on PSP";
        if (otherAgrees())
            add(selfRule + what, "upstream", s, display, inv, upCape ? u.capeTexture : "t_" + std::to_string(p.cape));
        else
            add(std::string(who) + what, "equip-body", s, display, inv, upCape ? u.capeTexture : "t_" + std::to_string(p.cape));
    } else if (upCape) {
        int tw = 0, th = 0;
        const double sc = textureVsPath(p.cape, u.capeTexture, tw, th);
        if (sc > o_.texTol) {
            char d[400];
            std::snprintf(d, sizeof(d), "upstream %s vs PSP t_%u (%dx%d) differs %.1f", u.capeTexture.c_str(), p.cape, tw, th, sc);
            if (otherAgrees()) add(selfRule + ": cape texture differs", "upstream", s, display, inv, d);
            else add(std::string(who) + ": cape texture differs", "equip-body", s, display, inv, d);
        }
    }
    (void)sex;
}

void Matrix::compareAttach(const char* who, const PspChar& pc, const std::vector<rp::AttachPick>& picks,
                           const UpResult& u, const Finding::Sample& s, uint32_t display, uint8_t inv, bool transforms,
                           const Case* cs, int refState) {
    std::map<uint32_t, const rp::AttachPick*> drawn;
    for (const auto& pk : picks) {
        bool point = false;
        for (const auto& q : pc.points) point = point || q.id == pk.point;
        if (!point) {
            add(std::string(who) + ": PSP pick on a point the PSP model lacks (not drawn)", "equip-weapons", s, display, inv,
                "point " + std::to_string(pk.point));
            continue;
        }
        if (pk.model == rp::kNoItemModel || !pspModel(pk.model).ok) {
            add(std::string(who) + ": PSP pick without a valid model", "equip-weapons", s, display, inv,
                "point " + std::to_string(pk.point));
            continue;
        }
        drawn[pk.point] = &pk;
    }
    std::set<uint32_t> points;
    for (const auto& [pt, a] : u.attach) points.insert(pt);
    for (const auto& [pt, pk] : drawn) points.insert(pt);
    for (uint32_t pt : points) {
        const auto ua = u.attach.find(pt);
        const auto pa = drawn.find(pt);
        if (ua == u.attach.end()) {
            std::string why = "PSP draws point " + std::to_string(pt) + " (slot " + std::to_string(pa->second->slot) + ")";
            if (transforms && pa->second->slot == 2 && (pt == 5 || pt == 6))
                add(std::string(who) + " [PSP deviation, psp_equipment.hpp: shown in game]: shoulders only on PSP "
                    "(upstream's own-player path attaches none)", "upstream", s, display, inv, why);
            else
                add(std::string(who) + ": model attached only on PSP", "equip-weapons", s, display, inv, why);
            continue;
        }
        if (pa == drawn.end()) {
            std::string cls = std::string(who) + ": model attached only upstream";
            // Why the PSP has none: the display has no baked model, or the rule put it elsewhere / nowhere.
            bool elsewhere = !drawn.empty();
            add(cls + (elsewhere ? " (PSP uses another point)" : ""), "equip-weapons", s, display, inv,
                "point " + std::to_string(pt) + " " + ua->second.model);
            continue;
        }
        const rp::AttachPick& pk = *pa->second;
        const UpAttach& a = ua->second;
        // Geometry: the a_ bounds against the M2 upstream loaded (bind pose; the bake poses Stand frame 0).
        const ModelSig& ps = pspModel(pk.model);
        const Upstream::M2Info& um = upModel(a.model);
        if (um.ok && ps.ok) {
            // The bake keeps a half extent of at least 1 cm per axis (model_bake.hpp WmdlBuilder) and stores s16.
            float worst = 0;
            for (int k = 0; k < 3; ++k) {
                const float c = (um.bmin[k] + um.bmax[k]) * 0.5f, h = std::max((um.bmax[k] - um.bmin[k]) * 0.5f, 0.01f);
                const float ext = std::max(0.02f, 2.0f * h);
                worst = std::max(worst, std::abs((c - h) - ps.bmin[k]) / ext);
                worst = std::max(worst, std::abs((c + h) - ps.bmax[k]) / ext);
            }
            if (worst > 0.03f) {
                char d[400];
                std::snprintf(d, sizeof(d), "point %u: upstream %s (%u tris%s) vs PSP a_%u, bounds off by %.0f%%", pt,
                              a.model.c_str(), um.triangles, um.posed ? ", Stand" : "", pk.model, worst * 100.0f);
                add(std::string(who) + ": attached model geometry differs", "equip-weapons", s, display, inv, d);
            }
        } else if (!um.ok) {
            add(std::string(who) + ": upstream model unreadable for the check", "matrix", s, display, inv, a.model);
        }
        // Skin.
        const bool upSkin = a.textureApplied;
        const bool pspSkin = pk.texture != rp::kNoItemLayer;
        if (upSkin != pspSkin) {
            add(std::string(who) + (upSkin ? ": item skin missing on PSP" : ": item skin only on PSP"), "equip-weapons", s,
                display, inv, "point " + std::to_string(pt) + " " + (upSkin ? a.texture : "t_" + std::to_string(pk.texture)));
        } else if (upSkin) {
            int tw = 0, th = 0;
            const double sc = textureVsPath(pk.texture, a.texture, tw, th);
            if (sc > o_.texTol) {
                char d[400];
                std::snprintf(d, sizeof(d), "point %u: upstream %s vs PSP t_%u differs %.1f", pt, a.texture.c_str(), pk.texture, sc);
                add(std::string(who) + ": item skin differs", "equip-weapons", s, display, inv, d);
            }
        }
        // The sheath transform (own player): upstream weaponLocalTransform vs the PSP pick.
        if (transforms) {
            float worst = 0;
            if (a.hasLocal != pk.hasLocal) {
                worst = 1;
            } else if (a.hasLocal) {
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 3; ++r) worst = std::max(worst, std::abs(a.local[c * 4 + r] - pk.local[c * 3 + r]));
            }
            if (worst > 1e-4f)
                add(std::string(who) + ": sheath/local transform differs", "equip-weapons", s, display, inv,
                    "point " + std::to_string(pt) + (a.hasLocal ? " upstream has one" : " upstream none") +
                        (pk.hasLocal ? ", PSP has one" : ", PSP none"));
        }
    }
    // The reference placement (a single worn weapon: any point but the helm's and the shoulders' is its).
    if (cs && cs->slot >= 15 && cs->slot <= 17) {
        int upPt = -1, pspPt = -1;
        for (const auto& [pt, a] : u.attach)
            if (pt != 11 && pt != 5 && pt != 6) upPt = static_cast<int>(pt);
        for (const auto& [pt, pk] : drawn)
            if (pk->slot == cs->slot) pspPt = static_cast<int>(pt);
        const int cp = referencePoint(cs->slot, cs->inv, cs->sheathe, cs->cls, cs->sub, refState);
        // A display with no model anywhere (quivers, ammo) is not a placement question.
        const bool anyModel = upPt >= 0 || pspPt >= 0 || bank_.models(cs->display);
        if (anyModel && (pspPt != cp || upPt != cp)) {
            const std::string kind = std::string(slotName(cs->slot)) + ", SheatheType " + sheatheName(cs->sheathe);
            const std::string d = std::string(invName(cs->inv)) + ": reference " + pointName(cp) + ", upstream " +
                                  pointName(upPt) + ", PSP " + pointName(pspPt);
            if (pspPt == upPt)  // upstream's placement, which the PSP follows, is not the reference's
                add(std::string(who) + ": attachment point vs reference placement [PSP = upstream]: reference " + pointName(cp) +
                        ", upstream/PSP " + pointName(upPt) + " (" + kind + ")",
                    "reference", s, display, inv, d);
            else if (pspPt != cp)
                add(std::string(who) + ": attachment point vs reference placement [PSP != upstream]: reference " + pointName(cp) +
                        ", upstream " + pointName(upPt) + ", PSP " + pointName(pspPt) + " (" + kind + ")",
                    "equip-weapons", s, display, inv, d);
        }
    }
    for (const auto& [pt, why] : u.attachFailed)
        if (!u.attach.count(pt))
            add(std::string(who) + (why == 1 ? ": upstream attach failed (no bone on the character)"
                                             : ": upstream attach failed (model did not load)"),
                "equip-weapons", s, display, inv, "point " + std::to_string(pt));
}

void Matrix::runCase(const Case& c, uint8_t race, uint8_t sex) {
    const int k = race * 2 + sex;
    const PspChar& pc = psp_[k];
    if (!pc.ok) return;
    const std::set<uint16_t>& um = upModel_[k];
    uint32_t disp[19] = {};
    uint8_t inv[19] = {};
    disp[c.slot] = c.display;
    inv[c.slot] = c.inv;
    rp::Equipment eq;
    eq.displayId[c.slot] = c.display;
    eq.invType[c.slot] = c.inv;
    Finding::Sample s;
    s.display = c.display;
    s.inv = c.inv;
    s.slot = c.slot;
    s.race = race;
    s.sex = sex;
    const uint8_t hair = static_cast<uint8_t>(o_.hair);
    ++runs_;
    // Other players: setOnlinePlayerEquipment vs the PSP's online rules (always held, as upstream).
    UpResult u, uo;
    const bool haveOther = up_.online(race, sex, hair, 0, disp, inv, u);
    if (haveOther) {
        uo = u;
        const PspDress p = pspDress(pc, bank_, eq, race, sex, hair, 0, rp::AttachRules::Online, false);
        if (o_.verbose) dump("other", c, race, sex, p, u, um);
        compareGeosets("other", pc, p, u, um, s, c.display, c.inv);
        compareBody("other", p, u, s, c.display, c.inv, sex, false);
        compareAttach("other", pc, p.picks, u, s, c.display, c.inv, false, &c, 1);
        checkModelRules("other", c, pc, um, p, u, pspNaked_[k], upNakedOther_[k], s);
    } else {
        add("other: upstream did not dress", "matrix", s, c.display, c.inv, "");
    }
    if (!o_.local) return;
    // The own player: GameScreen rules + AppearanceComposer (held; weapons also sheathed).
    s.local = true;
    if (up_.local(race, sex, hair, 0, disp, inv, false, true, true, u)) {
        const PspDress p = pspDress(pc, bank_, eq, race, sex, hair, 0, rp::AttachRules::Local, false);
        if (o_.verbose) dump("own", c, race, sex, p, u, um);
        compareGeosets("own", pc, p, u, um, s, c.display, c.inv, haveOther ? &uo : nullptr);
        compareBody("own", p, u, s, c.display, c.inv, sex, false, haveOther ? &uo : nullptr);
        compareAttach("own held", pc, p.picks, u, s, c.display, c.inv, true, &c, 1);
    }
    if (c.slot >= 15 && c.slot <= 17 && up_.local(race, sex, hair, 0, disp, inv, true, true, true, u)) {
        const PspDress p = pspDress(pc, bank_, eq, race, sex, hair, 0, rp::AttachRules::Local, true);
        compareAttach("own sheathed", pc, p.picks, u, s, c.display, c.inv, true, &c, 0);
    }
}

void Matrix::runOutfit(const std::string& name, const uint32_t disp[19], const uint8_t inv[19], uint8_t race,
                       uint8_t sex) {
    const int k = race * 2 + sex;
    const PspChar& pc = psp_[k];
    if (!pc.ok) return;
    rp::Equipment eq;
    uint32_t first = 0;
    uint8_t firstInv = 0;
    for (int i = 0; i < 19; ++i) {
        eq.displayId[i] = disp[i];
        eq.invType[i] = inv[i];
        if (disp[i] && !first) {
            first = disp[i];
            firstInv = inv[i];
        }
    }
    Finding::Sample s;
    s.display = first;
    s.inv = firstInv;
    s.race = race;
    s.sex = sex;
    s.set = name;
    for (int i = 0; i < 19; ++i) {
        s.disp[static_cast<size_t>(i)] = disp[i];
        s.invs[static_cast<size_t>(i)] = inv[i];
    }
    const uint8_t hair = static_cast<uint8_t>(o_.hair);
    ++outfits_;
    if (race == o_.races.front()) layerOrder(name + (sex ? " F" : " M"), disp, inv, sex);
    UpResult u, uo;
    const bool haveOther = up_.online(race, sex, hair, 0, disp, inv, u);
    if (haveOther) {
        uo = u;
        const PspDress p = pspDress(pc, bank_, eq, race, sex, hair, 0, rp::AttachRules::Online, false);
        compareGeosets("outfit other", pc, p, u, upModel_[k], s, first, firstInv);
        compareBody("outfit other", p, u, s, first, firstInv, sex, false);
    }
    if (o_.local && up_.local(race, sex, hair, 0, disp, inv, false, true, true, u)) {
        const PspDress p = pspDress(pc, bank_, eq, race, sex, hair, 0, rp::AttachRules::Local, false);
        compareGeosets("outfit own", pc, p, u, upModel_[k], s, first, firstInv, haveOther ? &uo : nullptr);
        compareBody("outfit own", p, u, s, first, firstInv, sex, false, haveOther ? &uo : nullptr);
    }
}

int Matrix::run() {
    const auto t0 = std::chrono::steady_clock::now();
    if (!loadTables()) return 1;
    buildCases();
    std::filesystem::create_directories(o_.out);
    report_.openTsv(o_.out + "/matrix_cases.tsv");
    std::printf("matrix: %zu item cases (Item.dbc display x inventory type, visible slots), %zu race/sex\n",
                cases_.size(), o_.races.size() * o_.sexes.size());
    checkCharacters();
    dataPass();
    size_t done = 0;
    for (const Case& c : cases_) {
        for (int race : o_.races)
            for (int sex : o_.sexes) runCase(c, static_cast<uint8_t>(race), static_cast<uint8_t>(sex));
        if (++done % 1000 == 0) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  %zu / %zu cases, %.0f s\n", done, cases_.size(), sec);
            std::fflush(stdout);
        }
    }
    // Outfits: every ItemSet (the layer order of several worn items) and a few slot pairs.
    if (o_.sets && itemSet_.rows) {
        for (uint32_t r = 0; r < itemSet_.rows; ++r) {
            uint32_t disp[19] = {};
            uint8_t inv[19] = {};
            int worn = 0;
            for (uint32_t f = 18; f < 35 && f < itemSet_.fields; ++f) {
                auto it = itemRow_.find(itemSet_.u32(r, f));
                if (it == itemRow_.end() || !it->second.first) continue;
                const int slot = slotFor(it->second.second);
                if (slot < 0 || disp[slot]) continue;
                disp[slot] = it->second.first;
                inv[slot] = it->second.second;
                ++worn;
            }
            if (worn < 2) continue;
            const std::string name = "set " + std::to_string(itemSet_.u32(r, 0));
            for (int race : o_.races)
                for (int sex : o_.sexes) runOutfit(name, disp, inv, static_cast<uint8_t>(race), static_cast<uint8_t>(sex));
        }
    }
    if (o_.combos) {
        // Slot pairs whose order on the body matters (task 2): the first displays of each type with body art.
        auto firstOf = [&](uint8_t invType, size_t n) {
            std::vector<uint32_t> out;
            for (const Case& c : cases_)
                if (c.inv == invType && out.size() < n) {
                    const rp::ItemDisplay* d = bank_.get(c.display);
                    bool art = false;
                    if (d)
                        for (int r = 0; r < rp::kItemRegions; ++r) art = art || d->layer[0][r] != rp::kNoItemLayer;
                    if (art) out.push_back(c.display);
                }
            return out;
        };
        const std::vector<std::pair<std::string, std::vector<uint8_t>>> combos = {
            {"robe+legs", {20, 7}},      {"chest+legs", {5, 7}},       {"shirt+chest+tabard", {4, 5, 19}},
            {"chest+hands", {5, 10}},    {"chest+wrists", {5, 9}},     {"legs+feet", {7, 8}},
            {"robe+waist", {20, 6}},     {"chest+waist+legs", {5, 6, 7}}, {"cloak+tabard", {16, 19}},
            {"shirt+wrists+hands", {4, 9, 10}},
        };
        for (const auto& [name, types] : combos) {
            std::vector<std::vector<uint32_t>> lists;
            for (uint8_t t : types) lists.push_back(firstOf(t, 6));
            for (size_t i = 0; i < 6; ++i) {
                uint32_t disp[19] = {};
                uint8_t inv[19] = {};
                bool ok = true;
                for (size_t k = 0; k < types.size(); ++k) {
                    if (lists[k].empty()) {
                        ok = false;
                        break;
                    }
                    const int slot = slotFor(types[k]);
                    disp[slot] = lists[k][i % lists[k].size()];
                    inv[slot] = types[k];
                }
                if (!ok) continue;
                for (int race : o_.races)
                    for (int sex : o_.sexes) runOutfit("combo " + name, disp, inv, static_cast<uint8_t>(race), static_cast<uint8_t>(sex));
            }
        }
    }
    if (o_.npc && !o_.creatures.empty()) npcPass();
    report_.closeTsv();
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    writeSummary(sec);
    writeSamples();
    // How close the art compared is (calibrates --layer-tol / --tex-tol): percentiles of every pair scored.
    auto pct = [](std::vector<double> v, const char* what) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; };
        std::printf("%s scores (%zu pairs): p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n", what, v.size(), at(0.5),
                    at(0.9), at(0.99), at(0.999), v.back());
    };
    std::vector<double> ls, ts;
    for (const auto& [k, v] : layerScore_) ls.push_back(v);
    for (const auto& [k, v] : texScore_) ts.push_back(v);
    pct(ls, "body layer");
    pct(ts, "texture");
    std::printf("matrix: %zu single-item runs, %zu outfit runs, %zu classes, %.0f s -> %s\n", runs_, outfits_,
                report_.all().size(), sec, o_.out.c_str());
    return 0;
}

void Matrix::writeSummary(double seconds) {
    const std::string path = o_.out + "/matrix_summary.md";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "# equip-matrix summary\n\n%zu single-item cases x %zu race/sex (%zu runs), %zu outfit runs, %.0f s.\n\n",
                 cases_.size(), o_.races.size() * o_.sexes.size(), runs_, outfits_, seconds);
    // by task, then by number of displays
    std::vector<std::pair<std::string, const Finding*>> rows;
    for (const auto& [cls, fd] : report_.all()) rows.emplace_back(cls, &fd);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second->task != b.second->task) return a.second->task < b.second->task;
        return a.second->pairs.size() > b.second->pairs.size();
    });
    std::fprintf(f, "| task | class | displays | cases | race/sex | by inventory type | examples |\n|---|---|---|---|---|---|---|\n");
    for (const auto& [cls, fd] : rows) {
        std::string inv;
        std::vector<std::pair<size_t, int>> byInv;
        for (const auto& [t, n] : fd->byInv) byInv.emplace_back(n, t);
        std::sort(byInv.rbegin(), byInv.rend());
        for (size_t i = 0; i < byInv.size() && i < 6; ++i)
            inv += std::string(inv.empty() ? "" : ", ") + invName(byInv[i].second) + " " + std::to_string(byInv[i].first);
        std::string ex;
        for (size_t i = 0; i < fd->examples.size() && i < 3; ++i) ex += (ex.empty() ? "" : "<br>") + fd->examples[i];
        std::fprintf(f, "| %s | %s | %zu | %zu | %zu | %s | %s |\n", fd->task.c_str(), cls.c_str(), fd->pairs.size(),
                     fd->cases, fd->raceSex.size(), inv.c_str(), ex.c_str());
    }
    writeModelGeosets(f);
    std::fclose(f);
    // The same on stdout, short.
    std::printf("\n%-14s %-90s %8s %8s %5s\n", "task", "class", "displays", "cases", "r/s");
    for (const auto& [cls, fd] : rows)
        std::printf("%-14s %-90s %8zu %8zu %5zu\n", fd->task.c_str(), cls.c_str(), fd->pairs.size(), fd->cases,
                    fd->raceSex.size());
}

// For the eye: the body texture of a sample, upstream's regions (its BLPs resampled to the PSP's 128 atlas the
// way the bake does) | the PSP's layers, both over grey. Upstream / PSP sides of the same outfit.
bool Matrix::sampleImages(const Finding::Sample& s, Rgba& upImg, Rgba& pspImg, std::string& text) {
    const int k = s.race * 2 + s.sex;
    auto pit = psp_.find(k);
    if (pit == psp_.end() || !pit->second.ok) return false;
    uint32_t disp[19] = {};
    uint8_t inv[19] = {};
    rp::Equipment eq;
    if (!s.set.empty()) {
        for (int i = 0; i < 19; ++i) {
            disp[i] = s.disp[static_cast<size_t>(i)];
            inv[i] = s.invs[static_cast<size_t>(i)];
            eq.displayId[i] = disp[i];
            eq.invType[i] = inv[i];
        }
    } else if (s.display) {
        disp[s.slot] = s.display;
        inv[s.slot] = s.inv;
        eq.displayId[s.slot] = s.display;
        eq.invType[s.slot] = s.inv;
    }
    UpResult u;
    const uint8_t hair = static_cast<uint8_t>(o_.hair);
    const bool ok = s.local ? up_.local(s.race, s.sex, hair, 0, disp, inv, false, true, true, u)
                            : up_.online(s.race, s.sex, hair, 0, disp, inv, u);
    if (!ok) return false;
    const PspDress p = pspDress(pit->second, bank_, eq, s.race, s.sex, hair, 0,
                                s.local ? rp::AttachRules::Local : rp::AttachRules::Online, false);
    auto blank = [](Rgba& img) {
        img.w = img.h = rp::kBodyTexSize;
        img.px.assign(size_t{rp::kBodyTexSize} * rp::kBodyTexSize * 4, 0);
        for (size_t i = 0; i < img.px.size(); i += 4) {
            img.px[i] = img.px[i + 1] = img.px[i + 2] = 0x80;
            img.px[i + 3] = 255;
        }
    };
    auto blend = [](Rgba& dst, const Rgba& src, int x0, int y0) {
        for (int y = 0; y < src.h; ++y)
            for (int x = 0; x < src.w; ++x) {
                const int cx = x0 + x, cy = y0 + y;
                if (cx < 0 || cy < 0 || cx >= dst.w || cy >= dst.h) continue;
                const uint8_t* sp = &src.px[(static_cast<size_t>(y) * src.w + x) * 4];
                uint8_t* dp = &dst.px[(static_cast<size_t>(cy) * dst.w + cx) * 4];
                const int a = sp[3];
                for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>((sp[c] * a + dp[c] * (255 - a)) / 255);
            }
    };
    blank(upImg);
    blank(pspImg);
    text = "upstream:";
    for (const auto& [r, path] : u.regions) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        text += " " + std::to_string(r) + "=" + path;
        if (!up_.loadBlp(path, rgba, w, h)) continue;
        const auto* rr = rp::kItemRegionRect256[r];
        blend(upImg, resampleLikeBake(rgba, w, h, rr[2] / 2, rr[3] / 2), rr[0] / 2, rr[1] / 2);
    }
    text += "\nPSP:";
    for (uint16_t q : p.layers) {
        const PspLayer& l = layer(q);
        text += " q_" + std::to_string(q);
        if (l.ok) blend(pspImg, l.img, l.x, l.y);
    }
    text += "\ncape: upstream " + (u.capeTexture.empty() ? std::string("none") : u.capeTexture) + ", PSP " +
            (p.cape == rp::kNoItemLayer ? std::string("none") : "t_" + std::to_string(p.cape)) + "\n";
    return true;
}

void Matrix::writeSamples() {
    if (o_.samples <= 0) return;
    const std::string dir = o_.out + "/samples";
    std::filesystem::create_directories(dir);
    std::FILE* idx = std::fopen((dir + "/index.txt").c_str(), "wb");
    auto write = [&](const Finding::Sample& s, const std::string& name, const std::string& what) {
        Rgba a, b;
        std::string text;
        if (!sampleImages(s, a, b, text)) return;
        std::FILE* f = std::fopen((dir + "/" + name).c_str(), "wb");
        if (!f) return;
        const int w = a.w * 2 + 4, h = a.h;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t rgb[3] = {255, 255, 255};
                const Rgba* img = x < a.w ? &a : (x >= a.w + 4 ? &b : nullptr);
                const int ix = x < a.w ? x : x - a.w - 4;
                if (img) std::memcpy(rgb, &img->px[(static_cast<size_t>(y) * img->w + ix) * 4], 3);
                std::fwrite(rgb, 1, 3, f);
            }
        std::fclose(f);
        if (idx) std::fprintf(idx, "%s (left upstream, right PSP)\n  %s\n  %s", name.c_str(), what.c_str(), text.c_str());
    };
    // Cases of the body classes.
    int n = 0;
    for (const auto& [cls, fd] : report_.all()) {
        if (fd.task != "equip-body" && fd.task != "layer-order") continue;
        ++n;
        for (int i = 0; i < o_.samples && i < static_cast<int>(fd.samples.size()); ++i) {
            const auto& s = fd.samples[static_cast<size_t>(i)];
            char name[96];
            std::snprintf(name, sizeof(name), "body_%02d_%d_d%u_%s%s.ppm", n, i, s.display, rsName(s.race, s.sex).c_str(),
                          s.local ? "_own" : "");
            write(s, name, "class: " + cls + (s.set.empty() ? std::string() : " [" + s.set + "]"));
        }
    }
    // A gallery for the eye: two displays of each body slot type and three ItemSets, on three race/sex.
    const std::pair<int, int> looks[3] = {{1, 0}, {6, 1}, {7, 0}};
    for (uint8_t invType : {4, 5, 20, 7, 8, 9, 10, 6, 19}) {
        std::vector<const Case*> list;
        for (const Case& c : cases_)
            if (c.inv == invType) list.push_back(&c);
        for (int pick = 1; pick <= 2 && !list.empty(); ++pick) {
            const Case& c = *list[list.size() * static_cast<size_t>(pick) / 3];
            for (const auto& [race, sex] : looks) {
                Finding::Sample s;
                s.display = c.display;
                s.inv = c.inv;
                s.slot = c.slot;
                s.race = static_cast<uint8_t>(race);
                s.sex = static_cast<uint8_t>(sex);
                char name[96];
                std::snprintf(name, sizeof(name), "gallery_%s_d%u_%s.ppm", invName(c.inv), c.display, rsName(race, sex).c_str());
                write(s, name, std::string("gallery: ") + invName(c.inv) + " " + armorType(c.cls, c.sub));
            }
        }
    }
    int sets = 0;
    for (uint32_t r = 0; r < itemSet_.rows && sets < 3; ++r) {
        Finding::Sample s;
        int worn = 0;
        for (uint32_t f = 18; f < 35 && f < itemSet_.fields; ++f) {
            auto it = itemRow_.find(itemSet_.u32(r, f));
            if (it == itemRow_.end() || !it->second.first) continue;
            const int slot = slotFor(it->second.second);
            if (slot < 0 || s.disp[static_cast<size_t>(slot)]) continue;
            s.disp[static_cast<size_t>(slot)] = it->second.first;
            s.invs[static_cast<size_t>(slot)] = it->second.second;
            ++worn;
        }
        if (worn < 5 || r % 37 != 0) continue;
        ++sets;
        s.set = "set " + std::to_string(itemSet_.u32(r, 0));
        for (const auto& [race, sex] : looks) {
            s.race = static_cast<uint8_t>(race);
            s.sex = static_cast<uint8_t>(sex);
            char name[96];
            std::snprintf(name, sizeof(name), "gallery_set%u_%s.ppm", itemSet_.u32(r, 0), rsName(race, sex).c_str());
            write(s, name, "gallery: " + s.set);
        }
    }
    if (idx) std::fclose(idx);
}

// The rules against the character model (both sides can be wrong the same way, which a PSP-vs-upstream diff
// cannot see - this check found the tabard: 1201 asked for where the models only carry 1202).
void Matrix::checkModelRules(const char* who, const Case& c, const PspChar& pc, const std::set<uint16_t>& upModel,
                             const PspDress& p, const UpResult& u, const std::set<uint16_t>& pspNaked,
                             const std::set<uint16_t>& upNaked, const Finding::Sample& s) {
    // (a) Switched on for this item (not in the naked character's set), not in the model.
    std::map<uint16_t, int> missing;  // geoset -> 1 PSP, 2 upstream
    for (uint16_t g : p.requested)
        if (!pspNaked.count(g) && !pc.geosets().count(g)) missing[g] |= 1;
    for (uint16_t g : u.geosets)
        if (!upNaked.count(g) && !upModel.count(g)) missing[g] |= 2;
    for (const auto& [g, side] : missing) {
        const char* sides = side == 3 ? "both PSP and upstream" : side == 1 ? "PSP only" : "upstream only";
        const std::string cls = std::string(who) + ": rule switches on a geoset the race model lacks [" + sides +
                                "], group " + std::to_string(g / 100) + " (" + groupName(g / 100) + ")";
        add(cls, side == 2 ? "upstream" : (side == 3 ? "both" : "equip-body"), s, c.display, c.inv,
            "geoset " + std::to_string(g));
    }
    std::set<uint16_t> ud;
    for (uint16_t g : u.geosets)
        if (upModel.count(g)) ud.insert(g);
    geosetGroupCheck(who, c.display, c.inv, upModel, p.geosets, ud, s);
    if (c.inv == 1) helmVisCheck(who, c, pc, p, ud, s);
}

// A helm against its HelmetGeosetVisData row (ItemDisplayInfo columns 13 / 14 by sex): what the helmet visibility
// rule (psp_body_rules.hpp helmetGeosets) hides for the wearer's race - the hair (the style's scalp), facial hair
// (groups 1-3), the ears (group 7) - against what both sides drew. Upstream (core::helmHidesHair) and the older PSP
// bake (hidesHair) swap the hair scalp for the bald cap when any column is non-zero - for every race - and hide
// nothing else.
void Matrix::helmVisCheck(const char* who, const Case& c, const PspChar& pc, const PspDress& p,
                          const std::set<uint16_t>& ud, const Finding::Sample& s) {
    const uint32_t visId = idiField(c.display, s.sex ? 14u : 13u);
    if (!visId) return;
    auto it = hvisRow_.find(visId);
    if (it == hvisRow_.end()) return;
    uint32_t mask[7];
    for (int k = 0; k < 7; ++k) mask[k] = hvis_.u32(it->second, 1u + static_cast<uint32_t>(k));
    const uint16_t scalp = pc.scalp[std::min<size_t>(static_cast<size_t>(o_.hair), pc.scalp.size() - 1)];
    // The rule on a probe set: what of the scalp, facial hair and ears it takes away for this race.
    std::unordered_set<uint16_t> probe = {scalp, 102, 202, 302, 702};
    rp::body::helmetGeosets(probe, mask, s.race);
    const bool ruleHidesHair = !probe.count(scalp);
    const bool ruleHidesEars = !probe.count(702);
    const bool ruleHidesFacial = !probe.count(102) || !probe.count(202) || !probe.count(302);
    // Hidden = the style's scalp is not drawn (the bald cap 1 that replaces it is missing from some models).
    const bool pspHidHair = !p.geosets.count(scalp);
    const bool upHidHair = !ud.count(scalp);
    const std::string row = "HelmetGeosetVisData " + std::to_string(visId);
    if (scalp != 1 && (pspHidHair != ruleHidesHair || upHidHair != ruleHidesHair)) {
        const char* sides = (pspHidHair != ruleHidesHair && upHidHair != ruleHidesHair) ? "both PSP and upstream"
                            : pspHidHair != ruleHidesHair                                ? "PSP only"
                                                                                         : "upstream only";
        add(std::string(who) + ": helm vs helmet rule - hair " +
                (ruleHidesHair ? "shown, the rule hides it" : "hidden, the rule shows it") + " [" + sides + "]",
            "both", s, c.display, c.inv, row + " hair mask 0x" + [&] {
                char b[16];
                std::snprintf(b, sizeof(b), "%X", mask[0]);
                return std::string(b);
            }());
    }
    if (ruleHidesEars && (p.geosets.count(702) || ud.count(702)))
        add(std::string(who) + ": helm vs helmet rule - ears (group 7) not hidden [both PSP and upstream]", "both", s,
            c.display, c.inv, row);
    if (ruleHidesFacial)
        add(std::string(who) + ": helm vs helmet rule - facial hair (groups 1-3) hidden for this race [upstream reads "
                               "no facial columns]",
            "both", s, c.display, c.inv, row);
}

// The item's GeosetGroup: base + value, the arithmetic upstream uses (core::equippedGeoset) - and for the tabard
// (which upstream never reads) the outfit rules' tabard geoset (tabardGeosetOf; the models carry no 1201).
void Matrix::geosetGroupCheck(const char* who, uint32_t display, uint8_t inv, const std::set<uint16_t>& model,
                              const std::set<uint16_t>& pspDrawn, const std::set<uint16_t>& upDrawn,
                              const Finding::Sample& s) {
    struct Rule {
        uint8_t inv;
        int group;
        uint32_t field;  // ItemDisplayInfo column: 7 GeosetGroup1, 9 GeosetGroup3
        uint16_t base;   // 0: the tabard, by the outfit rules
    };
    static const Rule kRules[] = {
        {10, 4, 7, 401},  {8, 5, 7, 501},   {4, 8, 7, 801},    {5, 8, 7, 801},    {20, 8, 7, 801},
        {4, 13, 9, 1301}, {5, 13, 9, 1301}, {20, 13, 9, 1301}, {7, 13, 7, 1301},  {9, 8, 7, 801},
        {6, 18, 7, 1801}, {16, 15, 7, 1501}, {19, 12, 7, 0},
    };
    for (const Rule& r : kRules) {
        if (r.inv != inv) continue;
        const uint32_t gg = idiField(display, r.field);
        if (!gg) continue;
        const uint16_t want = r.base ? static_cast<uint16_t>(r.base + gg) : tabardGeosetOf(gg);
        auto groupList = [&](const std::set<uint16_t>& set) {
            std::string out;
            for (uint16_t g : set)
                if (g / 100 == r.group) out += " " + std::to_string(g);
            return out.empty() ? std::string(" -") : out;
        };
        if (!model.count(want)) {
            add(std::string(who) + " data: item GeosetGroup names a geoset the race model lacks (group " +
                    std::to_string(r.group) + " " + groupName(r.group) + ")",
                "data", s, display, inv,
                "GeosetGroup" + std::to_string(r.field - 6) + "=" + std::to_string(gg) + " -> " + std::to_string(want) +
                    "; model has" + groupList(model));
            continue;
        }
        const bool pspHas = pspDrawn.count(want) > 0, upHas = upDrawn.count(want) > 0;
        if (pspHas && upHas) continue;
        const char* sides = (!pspHas && !upHas) ? "both PSP and upstream" : !pspHas ? "PSP only" : "upstream only";
        add(std::string(who) + ": item GeosetGroup not drawn [" + sides + "], group " + std::to_string(r.group) + " (" +
                groupName(r.group) + ")",
            (!pspHas && !upHas) ? "both" : (!pspHas ? "equip-body" : "upstream"), s, display, inv,
            "GeosetGroup" + std::to_string(r.field - 6) + "=" + std::to_string(gg) + " wants " + std::to_string(want) +
                "; PSP" + groupList(pspDrawn) + ", upstream" + groupList(upDrawn));
    }
}

// ItemDisplayInfo GeosetGroup columns that neither side reads for the slot (upstream: chest/shirt/robe 1 and 3,
// legs, boots, gloves, wrists, belt, cloak 1; the tabard none) - counted per display, for task 2.
void Matrix::dataPass() {
    std::set<std::pair<uint32_t, uint8_t>> seen;
    for (const Case& c : cases_) {
        if (!seen.insert({c.display, c.inv}).second) continue;
        bool used[3] = {false, false, false};
        switch (c.inv) {
            case 4: case 5: case 20: used[0] = used[2] = true; break;
            case 6: case 7: case 8: case 9: case 10: case 16: used[0] = true; break;
            default: break;
        }
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = idiField(c.display, 7u + static_cast<uint32_t>(k));
            if (!v || used[k]) continue;
            Finding::Sample s;
            s.display = c.display;
            s.inv = c.inv;
            s.slot = c.slot;
            add("data: GeosetGroup" + std::to_string(k + 1) + " set on " + invName(c.inv) + ", read by neither PSP nor upstream",
                "data", s, c.display, c.inv, "value " + std::to_string(v));
        }
    }
}

// Which slot's art ends on top where two worn items paint the same body region (upstream compositeWithRegions
// and the PSP: slot order 0..18, each item's regions 0..7) - per outfit, once.
void Matrix::layerOrder(const std::string& name, const uint32_t disp[19], const uint8_t inv[19], uint8_t sex) {
    if (!layerOrderSeen_.insert(name).second) return;
    for (int r = 0; r < rp::kItemRegions; ++r) {
        std::vector<int> painters;
        for (int sl = 0; sl < 19; ++sl) {
            if (!disp[sl]) continue;
            const rp::ItemDisplay* d = bank_.get(disp[sl]);
            if (d && d->layer[sex ? 1 : 0][r] != rp::kNoItemLayer) painters.push_back(sl);
        }
        auto bodySlot = [](int sl) { return (sl >= 3 && sl <= 9) || sl == 18; };
        for (int sl : painters) {
            if (bodySlot(sl)) continue;
            Finding::Sample s;
            s.display = disp[sl];
            s.inv = inv[sl];
            s.slot = static_cast<uint8_t>(sl);
            s.race = static_cast<uint8_t>(o_.races.front());
            s.sex = sex;
            s.set = name;
            for (int k = 0; k < 19; ++k) {
                s.disp[static_cast<size_t>(k)] = disp[k];
                s.invs[static_cast<size_t>(k)] = inv[k];
            }
            add(std::string("data: a ") + slotName(sl) + " item paints the body (upstream and PSP composite every slot's regions)",
                "data", s, disp[sl], inv[sl], regionName(r));
        }
        for (size_t i = 0; i + 1 < painters.size(); ++i)
            for (size_t j = i + 1; j < painters.size(); ++j) {
                const int lo = painters[i], hi = painters[j];
                if (!bodySlot(lo) || !bodySlot(hi)) continue;
                std::string hiName = slotName(hi), loName = slotName(lo);
                if (hi == 4 && inv[4] == 20) hiName = "robe";
                if (lo == 4 && inv[4] == 20) loName = "robe";
                Finding::Sample s;
                s.display = disp[hi];
                s.inv = inv[hi];
                s.slot = static_cast<uint8_t>(hi);
                s.race = static_cast<uint8_t>(o_.races.front());
                s.sex = sex;
                s.set = name;
                for (int k = 0; k < 19; ++k) {
                    s.disp[static_cast<size_t>(k)] = disp[k];
                    s.invs[static_cast<size_t>(k)] = inv[k];
                }
                add("layer order (upstream = PSP: slot order): " + hiName + " over " + loName, "layer-order", s, disp[hi],
                    inv[hi], std::string(regionName(r)) + ", under display " + std::to_string(disp[lo]));
            }
    }
}

void Matrix::writeModelGeosets(std::FILE* f) {
    std::fprintf(f, "\n## Character model geosets (upstream M2 = PSP n_), equipment groups\n\n| race/sex | geosets |\n|---|---|\n");
    for (const auto& [k, set] : upModel_) {
        std::string g;
        for (uint16_t v : set)
            if (v >= 400) g += (g.empty() ? "" : " ") + std::to_string(v);
        std::fprintf(f, "| %s | %s |\n", rsName(k / 2, k % 2).c_str(), g.c_str());
    }
}

// NPCs (task 4): humanoid displays (CreatureDisplayInfoExtra with at least three pieces of equipment, every
// --npc-every-th of them, or --npc-display ids - the displays seen in a city, e.g. from the PSP's npc_equip log
// lines), upstream's spawn (EntitySpawner::spawnOnlineCreature: geosets, cape, skin, the virtual weapon) against
// creatures.pak (the WCDI batch mask over the model's geosets, the cape and skin slots) and the PSP's NPC
// equipment (CreatureRenderer::updateEquipment: helm, shoulders, weapon through equipmentAttachments on the
// model's p_ points).
void Matrix::npcPass() {
    if (o_.creatures.empty()) return;
    pp::PakIndex cp;
    if (!cp.open(o_.creatures.c_str())) {
        std::fprintf(stderr, "matrix: cannot open %s (NPC pass skipped)\n", o_.creatures.c_str());
        return;
    }
    auto blob = [&](const std::string& name, std::vector<uint8_t>& out) {
        pp::PakEntry e{};
        if (!cp.find(name.c_str(), e)) return false;
        void* b = cp.load(e);
        if (!b) return false;
        out.assign(static_cast<uint8_t*>(b), static_cast<uint8_t*>(b) + e.size);
        pp::pakFree(b);
        return true;
    };
    std::unordered_map<uint32_t, uint32_t> extraRow;
    for (uint32_t r = 0; r < cdie_.rows; ++r) extraRow[cdie_.u32(r, 0)] = r;
    std::vector<uint32_t> displays;
    if (!o_.npcDisplays.empty()) {
        for (int d : o_.npcDisplays) displays.push_back(static_cast<uint32_t>(d));
    } else {
        size_t n = 0;
        for (uint32_t r = 0; r < cdi_.rows; ++r) {
            auto it = extraRow.find(cdi_.u32(r, 3));
            if (it == extraRow.end()) continue;
            int pieces = 0;
            for (uint32_t f = 8; f <= 18; ++f) pieces += cdie_.u32(it->second, f) ? 1 : 0;
            if (pieces < 3) continue;
            if (n++ % o_.npcEvery == 0) displays.push_back(cdi_.u32(r, 0));
        }
    }
    struct ModelInfo {
        bool ok = false;
        std::vector<uint16_t> geo;
        uint32_t lod0 = 0;
        std::vector<rp::AttachPoint> points;
    };
    std::map<uint16_t, ModelInfo> models;
    size_t checked = 0;
    std::map<std::pair<uint16_t, std::string>, double> texCache;
    auto creatureTexture = [&](uint16_t x, const std::string& path) {
        const auto key = std::make_pair(x, path);
        if (auto it = texCache.find(key); it != texCache.end()) return it->second;
        std::vector<uint8_t> b;
        Rgba img;
        double sc = 999.0;
        if (blob("x_" + std::to_string(x), b) && decodeWtex(b.data(), static_cast<uint32_t>(b.size()), img))
            sc = rgbaVsPath(img, path);
        texCache.emplace(key, sc);
        return sc;
    };
    for (uint32_t D : displays) {
        Finding::Sample s;
        s.display = D;
        uint32_t cdiRow = 0;
        bool haveRow = false;
        for (uint32_t r = 0; r < cdi_.rows && !haveRow; ++r)
            if (cdi_.u32(r, 0) == D) {
                cdiRow = r;
                haveRow = true;
            }
        if (haveRow)
            if (auto it = extraRow.find(cdi_.u32(cdiRow, 3)); it != extraRow.end()) {
                s.race = static_cast<uint8_t>(cdie_.u32(it->second, 1));
                s.sex = static_cast<uint8_t>(cdie_.u32(it->second, 2));
            }
        std::vector<uint8_t> d;
        if (!blob("d_" + std::to_string(D), d) || d.size() < sizeof(rp::CreatureDisplayHeader) ||
            std::memcmp(d.data(), "WCDI", 4) != 0) {
            add("npc: display not in creatures.pak", "equip-others", s, D, 0, "");
            continue;
        }
        rp::CreatureDisplayHeader h;
        std::memcpy(&h, d.data(), sizeof(h));
        if (d.size() < sizeof(h) + h.batchCount) continue;
        const uint8_t* active = d.data() + sizeof(h);
        const rp::CreatureDisplayEquip* eqb = rp::creatureDisplayEquip(d.data(), static_cast<uint32_t>(d.size()));
        ModelInfo& mi = models[h.model];
        if (!mi.ok) {
            std::vector<uint8_t> n;
            if (blob("n_" + std::to_string(h.model), n)) {
                rp::SkinView v;
                if (v.init(n.data(), static_cast<uint32_t>(n.size()))) {
                    for (uint32_t i = 0; i < v.h->batchCount; ++i) mi.geo.push_back(v.geosets[i]);
                    mi.lod0 = (v.h->flags & rp::kSkinHasLod) ? v.h->reserved[0] : v.h->batchCount;
                    mi.ok = true;
                }
            }
            if (!mi.ok && blob("m_" + std::to_string(h.model), n) && n.size() >= sizeof(rp::ModelHeader)) {
                rp::ModelHeader mh;
                std::memcpy(&mh, n.data(), sizeof(mh));
                const uint32_t nb = uint32_t{mh.batchCount[0]} + mh.batchCount[1];
                if (mh.geosetOffset && mh.geosetOffset + nb * 2u <= n.size()) {
                    for (uint32_t i = 0; i < nb; ++i) mi.geo.push_back(rd<uint16_t>(n.data() + mh.geosetOffset + i * 2));
                    mi.lod0 = mh.batchCount[0];
                    mi.ok = true;
                }
            }
            std::vector<uint8_t> pb;
            if (blob("p_" + std::to_string(h.model), pb) && pb.size() >= sizeof(rp::AttachTable) &&
                std::memcmp(pb.data(), "WATP", 4) == 0) {
                rp::AttachTable t;
                std::memcpy(&t, pb.data(), sizeof(t));
                if (pb.size() >= sizeof(t) + t.count * sizeof(rp::AttachPoint)) {
                    mi.points.resize(t.count);
                    std::memcpy(mi.points.data(), pb.data() + sizeof(t), t.count * sizeof(rp::AttachPoint));
                }
            }
        }
        if (!mi.ok) {
            add("npc: model not readable in creatures.pak", "equip-others", s, D, 0, "m_/n_ " + std::to_string(h.model));
            continue;
        }
        // PSP: what the batch mask shows (LOD 0) and whether a cape batch is drawn.
        std::set<uint16_t> pv;
        bool pspCape = false;
        for (uint32_t i = 0; i < std::min<uint32_t>(mi.lod0, h.batchCount) && i < mi.geo.size(); ++i)
            if (active[i]) {
                pv.insert(mi.geo[i]);
                pspCape = pspCape || active[i] == rp::kBatchCape;
            }
        UpResult u;
        std::string path;
        if (!up_.npc(D, o_.npcWeapon, u, path)) {
            add("npc: upstream did not spawn the display", "matrix", s, D, 0, path);
            continue;
        }
        ++checked;
        // Upstream draws its filter's geosets, or every batch but groups 15/17/18 without one.
        std::set<uint16_t> ud;
        for (uint16_t g : u.modelGeosets) {
            const int grp = g / 100;
            if (u.geosetsSet ? u.geosets.count(g) > 0 : (grp != 15 && grp != 17 && grp != 18)) ud.insert(g);
        }
        if (ud != pv) {
            std::map<int, std::pair<std::string, std::string>> groups;
            for (uint16_t g : ud)
                if (!pv.count(g)) groups[g / 100].second += " " + std::to_string(g);
            for (uint16_t g : pv)
                if (!ud.count(g)) groups[g / 100].first += " " + std::to_string(g);
            for (const auto& [grp, dd] : groups)
                add("npc: geoset group " + std::to_string(grp) + " (" + groupName(grp) + ")", "equip-others", s, D, 0,
                    "PSP" + (dd.first.empty() ? std::string(" -") : dd.first) + " | upstream" +
                        (dd.second.empty() ? std::string(" -") : dd.second));
        }
        // The worn pieces' GeosetGroup (CreatureDisplayInfoExtra slot -> inventory type), on both sides.
        if (eqb) {
            static const uint8_t kInvOfNpcSlot[11] = {1, 3, 4, 5, 6, 7, 8, 9, 10, 19, 16};
            for (int k = 2; k < 11; ++k)
                if (eqb->item[k]) {
                    Finding::Sample ss = s;
                    ss.inv = kInvOfNpcSlot[k];
                    geosetGroupCheck("npc", eqb->item[k], kInvOfNpcSlot[k], u.modelGeosets, pv, ud, ss);
                }
        }
        if (u.geosetsSet)
            for (uint16_t g : u.geosets)
                if (!u.modelGeosets.count(g) && g >= 400)
                    add("npc: upstream rule switches on a geoset the model lacks, group " + std::to_string(g / 100) + " (" +
                            groupName(g / 100) + ")",
                        "upstream", s, D, 0, "geoset " + std::to_string(g));
        // Cape: group 15 + the type-2 slot.
        const bool upCape = !u.capeTexture.empty() && u.capeTexture[0] != '#';
        const uint16_t capeX = h.slotTexture[2];
        if (upCape != pspCape) {
            add(upCape ? "npc: cape drawn only upstream" : "npc: cape drawn only on PSP", "equip-others", s, D, 0,
                upCape ? u.capeTexture : "");
        } else if (upCape) {
            if (capeX == rp::kNoTexture) {
                add("npc: cape texture missing on PSP (slot 2 empty)", "equip-others", s, D, 0, u.capeTexture);
            } else {
                const double sc = creatureTexture(capeX, u.capeTexture);
                if (sc > o_.texTol)
                    add("npc: cape texture differs", "equip-others", s, D, 0,
                        u.capeTexture + " vs x_" + std::to_string(capeX) + " " + std::to_string(sc));
            }
        }
        // Skin (texture type 1): the baked NPC texture.
        if (!u.skinTexture.empty() && u.skinTexture[0] != '#') {
            const uint16_t skinX = h.slotTexture[1];
            if (skinX == rp::kNoTexture) {
                add("npc: skin texture missing on PSP (slot 1 empty)", "equip-others", s, D, 0, u.skinTexture);
            } else {
                const double sc = creatureTexture(skinX, u.skinTexture);
                if (sc > o_.texTol)
                    add("npc: skin (baked) texture differs", "equip-others", s, D, 0,
                        u.skinTexture + " vs x_" + std::to_string(skinX) + " " + std::to_string(sc));
            }
        }
        // Helm, shoulders (PSP: CreatureRenderer::updateEquipment) and the weapon.
        if (!eqb) continue;
        rp::Equipment eq;
        eq.displayId[0] = eqb->item[0];
        eq.invType[0] = 1;
        eq.displayId[2] = eqb->item[1];
        eq.invType[2] = 3;
        uint32_t wd = 0;
        uint8_t winv = 0;
        if (o_.npcWeapon && bank_.itemDisplay(o_.npcWeapon, wd, winv)) {
            eq.displayId[15] = wd;
            eq.invType[15] = 13;
        }
        const auto picks = rp::equipmentAttachments(eq, eqb->race, eqb->sex, rp::AttachRules::Online, false, bank_);
        auto hasPoint = [&](uint16_t id) {
            for (const auto& q : mi.points)
                if (q.id == id) return true;
            return false;
        };
        bool pspWeapon = false;
        for (const auto& pk : picks) {
            const bool drawn = hasPoint(pk.point) && pk.model != rp::kNoItemModel && pspModel(pk.model).ok;
            if (pk.slot == 15) {
                pspWeapon = drawn;
                continue;
            }
            if (drawn)
                add("npc [PSP deviation: drawn in game, upstream's world path does not]: helm/shoulder model only on PSP",
                    "upstream", s, D, 0, std::string(pk.slot == 0 ? "helm " : "shoulder ") + std::to_string(pk.displayId));
            else
                add("npc: helm/shoulder pick not drawn on PSP (no point on the creature model or no model)", "equip-others", s,
                    D, 0, "point " + std::to_string(pk.point));
        }
        for (int slot : {0, 2}) {
            const uint32_t did = eq.displayId[slot];
            auto row = idiRow_.find(did);
            const bool namesModel = row != idiRow_.end() && (!idi_.str(row->second, 1).empty() || !idi_.str(row->second, 2).empty());
            if (did && namesModel && !bank_.models(did))
                add("npc: helm/shoulder display without a baked item model", "equip-others", s, D, 0, std::to_string(did));
        }
        if (o_.npcWeapon) {
            const bool upWeapon = u.attach.count(1) > 0;
            if (upWeapon != pspWeapon)
                add(upWeapon ? "npc: weapon only upstream" : "npc: weapon only on PSP", "equip-others", s, D, 0,
                    "item " + std::to_string(o_.npcWeapon));
        }
    }
    std::printf("npc: %zu of %zu humanoid displays checked\n", checked, displays.size());
}

void Matrix::dump(const char* who, const Case& c, uint8_t race, uint8_t sex, const PspDress& p, const UpResult& u,
                  const std::set<uint16_t>& upModel) {
    std::printf("== %s display %u inv %u (%s) slot %u %s\n  PSP geosets:", who, c.display, c.inv, invName(c.inv), c.slot,
                rsName(race, sex).c_str());
    for (uint16_t g : p.geosets) std::printf(" %u", g);
    std::printf("\n  up  geosets:");
    for (uint16_t g : u.geosets)
        if (upModel.count(g)) std::printf(" %u", g);
    std::printf("\n  PSP layers:");
    for (uint16_t q : p.layers) {
        const PspLayer& l = layer(q);
        std::printf(" q_%u@%d,%d", q, l.x, l.y);
    }
    std::printf("\n  up  regions:");
    for (const auto& [r, path] : u.regions) std::printf(" %d=%s", r, path.c_str());
    const std::string pcape = p.cape == rp::kNoItemLayer ? std::string("-") : "t_" + std::to_string(p.cape);
    std::printf("\n  cape: PSP %s up %s\n", pcape.c_str(), u.capeTexture.empty() ? "-" : u.capeTexture.c_str());
    for (const auto& pk : p.picks)
        std::printf("  PSP pick point %u a_%u t_%u slot %u local %d\n", pk.point, pk.model, pk.texture, pk.slot,
                    pk.hasLocal ? 1 : 0);
    for (const auto& [pt, a] : u.attach)
        std::printf("  up  attach point %u %s tex %s%s local %d\n", pt, a.model.c_str(), a.texture.c_str(),
                    a.textureApplied ? "" : " (not applied)", a.hasLocal ? 1 : 0);
}

}  // namespace

int runMatrix(int argc, char** argv) {
    Options o;
    auto list = [](const std::string& s) {
        std::vector<int> v;
        std::stringstream ss(s);
        std::string t;
        while (std::getline(ss, t, ',')) v.push_back(std::atoi(t.c_str()));
        return v;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool v = i + 1 < argc;
        if (a == "--items" && v) o.items = argv[++i];
        else if (a == "--chars" && v) o.chars = argv[++i];
        else if (a == "--extracted" && v) o.extracted = argv[++i];
        else if (a == "--mirror" && v) o.mirrors.push_back(argv[++i]);
        else if (a == "--repo" && v) o.repo = argv[++i];
        else if (a == "--out" && v) o.out = argv[++i];
        else if (a == "--races" && v) o.races = list(argv[++i]);
        else if (a == "--sexes" && v) o.sexes = list(argv[++i]);
        else if (a == "--inv" && v) o.invs = list(argv[++i]);
        else if (a == "--display" && v) {
            for (int d : list(argv[++i])) o.displays.insert(static_cast<uint32_t>(d));
        } else if (a == "--limit" && v) o.limit = static_cast<size_t>(std::atol(argv[++i]));
        else if (a == "--every" && v) o.every = std::max<size_t>(1, static_cast<size_t>(std::atol(argv[++i])));
        else if (a == "--hair" && v) o.hair = std::atoi(argv[++i]);
        else if (a == "--samples" && v) o.samples = std::atoi(argv[++i]);
        else if (a == "--no-local") o.local = false;
        else if (a == "--no-sets") o.sets = false;
        else if (a == "--no-combos") o.combos = false;
        else if (a == "--no-npc") o.npc = false;
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--creatures" && v) o.creatures = argv[++i];
        else if (a == "--npc-display" && v) o.npcDisplays = list(argv[++i]);
        else if (a == "--npc-every" && v) o.npcEvery = std::max<size_t>(1, static_cast<size_t>(std::atol(argv[++i])));
        else if (a == "--npc-weapon" && v) o.npcWeapon = static_cast<uint32_t>(std::atol(argv[++i]));
        else if (a == "--layer-tol" && v) o.layerTol = std::atof(argv[++i]);
        else if (a == "--tex-tol" && v) o.texTol = std::atof(argv[++i]);
    }
    if (o.races.empty()) o.races.assign(std::begin(kRaces), std::end(kRaces));
    if (o.items.empty() || o.chars.empty() || o.extracted.empty()) {
        std::fprintf(stderr,
                     "psp_equip_preview --matrix --items <items.pak> --chars <characters.pak> --extracted <dir>\n"
                     "    [--mirror <copy of extracted folders>]... [--repo <WoWee root>] [--out <dir>]\n"
                     "    [--races 1,2,..] [--sexes 0,1] [--inv 1,3,..] [--display a,b,..] [--limit N] [--every N] [--hair N]\n"
                     "    [--samples N] [--layer-tol 10] [--tex-tol 20] [--no-local] [--no-sets] [--no-combos] [--verbose]\n"
                     "    [--creatures <creatures.pak> [--npc-display a,b,..] [--npc-every 50] [--npc-weapon <item>]] [--no-npc]\n");
        return 1;
    }
    UpOptions uo;
    uo.roots = o.mirrors;
    uo.roots.push_back(o.extracted);
    uo.dbcLayoutJson = o.repo + "/Data/expansions/wotlk/dbc_layouts.json";
    Upstream up;
    std::string why;
    if (!up.init(uo, why)) {
        std::fprintf(stderr, "matrix: upstream reference: %s\n", why.c_str());
        return 1;
    }
    pp::Pak chars;
    rp::ItemBank bank;
    bank.setBudget(1024, 16u * 1024u * 1024u);
    if (!chars.open(o.chars.c_str()) || !bank.open(o.items.c_str())) {
        std::fprintf(stderr, "matrix: cannot open %s / %s\n", o.chars.c_str(), o.items.c_str());
        return 1;
    }
    Matrix m(o, up, bank, chars);
    return m.run();
}

}  // namespace eqm
