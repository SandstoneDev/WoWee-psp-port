// psp_creature_conv - bakes creature and NPC models for the PSP (M10b; from the DBCs).
//
//   psp_creature_conv --extracted <dir> --out <creatures.pak>
//                     [--display N[,N...]]... [--display-list <file>] [--jobs N]
//                     [--spawns <creature_spawns.tsv> [--map 0]]   (our server only: the old static spawns)
//                     [--tex-max 128] [--anims 0,4,5,1,... | --no-anim] [--npc-emotes 60,64,65,66]
//                     [--no-state-emotes] [--rate 30] [--palette 8] [--max-anim-kb 384] [--all-variations]
//                     [--verify [--verify-limit 0.02]] [--dump <dir>] [--verbose]
//                     [--gameobjects | --no-gameobjects] [--go-display N[,N...]]   (animated game objects)
//   psp_creature_conv --extracted <dir> [--display ...] --list-assets
//
// With no --display / --display-list / --spawns, EVERY CreatureDisplayInfo
// row is baked (~24 000 displays, ~1 100 models): what may have to be drawn is whatever display id
// the server sends (UNIT_FIELD_DISPLAYID, UNIT_FIELD_MOUNTDISPLAYID), so nothing comes from our world DB any
// more (creature_spawns.py and mount_displays.py are out of the bake; --spawns stays as a key for our own
// server's static spawns, off on the PSP by default). Each model is baked once, each texture file once. The
// pak is written sorted with the "~fence" record (conv_common.hpp SortedPakWriter, streamed through a temp
// file) and read on the PSP through PakIndex - nothing per display is in RAM until a unit shows it.
// --jobs N bakes the textures and the models on N threads; ids are planned first (serially, in display
// order), so the pak is byte-identical whatever N is.
//
// What a display id draws follows upstream EntitySpawner:
//   - buildCreatureDisplayLookups (src/core/entity_spawner.cpp:561): the
//     CreatureDisplayInfo -> CreatureModelData path (.mdx -> .m2), both scales,
//     the skin donors for rows with an empty Skin1, CreatureDisplayInfoExtra
//     with BakeName at 18 or 20 by field count;
//   - getModelPathForDisplayId (:826), its fixed ids included; helper models
//     (rendering::isHelperCreatureModel, :2160) are never drawn;
//   - applyCreatureDisplayTextures (:1738): texture types 11/12/13 take
//     Skin1/2/3, resolved in the model's folder first; a humanoid (an Extra
//     row) takes the BakedNpcTextures picture on types 1/11/12/13, hair from
//     CharSections on type 6 (the bake when there is none), the skin row's
//     second texture on type 8 (applyHumanoidInstanceOverrides, :1621);
//   - geosets: normalizeHumanoidClothingGeosets (:1281) for humanoids, with
//     equipment GeosetGroup1 from ItemDisplayInfo and the
//     cape: shown when a cape texture exists (hasRenderableCape), its geoset
//     cloakGeoset(GeosetGroup1), its texture on the cape batches (group 15) and
//     the type-2 slots (setGroupTextureOverride / setTextureSlotOverride);
//     everything else draws all batches but groups 15/17/18
//     (CharacterRenderer::render, src/rendering/character_renderer.cpp:2796).
// The model is baked like psp_char_conv: stand pose frame 0 (M9 adds the
// animation on the same vertices), level-0 submeshes with their ids, batch
// texture per CharacterRenderer::resolveBatchTexture (character_renderer.cpp:
// 3568: hair wins, then the first non-white) - replaceable slots are written
// as kTexSlot + type and filled per display at run time. A lower skin
// (03..01) is kept as LOD 1 when it has at most 60 % of the indices, else a
// model over 1500 triangles gets a meshoptimizer-simplified LOD 1 (same
// batches, so one display mask serves both).
// Not baked: equipment textures on the body of a humanoid without a BakeName
// (it gets the CharSections composite: skin, face, underwear), particles.
// Creature anim - "n_<n>" next to "m_<n>": the same batches (LOD 0
// then LOD 1) skinned for the GE like the characters' n_ (skin_bake.hpp
// bakeSkinned, <= 8 bones per sub-batch, M2 blendTime/frequency kept), with
// the sequences the PSP plays for NPCs (upstream creature sync + callbacks:
// Stand with its variations, Walk, Run, Death, Rise, wounds, attacks, ready
// stances, swim, fly) plus a few NPC one-shot emotes and
// the Emotes.dbc state animations (UNIT_NPC_EMOTESTATE) a model has, last.
// --verify decodes each n_ with the PSP code (psp_skeleton.hpp) and compares
// it with CPU skinning by upstream's rules (skin_bake.hpp verifySkinned), and
// reads the written pak back with the PSP's lookup (psp_pak_index.hpp).
// Mounts without the DB: "p_<n>" for every model (the
// seat points upstream's applyMountPositioning probes + the equipment points,
// psp_model_format.hpp kCreaturePointIds); a model with a seat (attachment 0
// "MountMain" above 0.1 yd - upstream processPendingMount: "every rideable
// model carries one"), character models excepted (their 0 is the shield),
// also gets the mount jump set and rear-up right after Stand / Walk / Run.
// Output layout: include/rendering/psp/psp_model_format.hpp.

#include "core/geoset_rules.hpp"
#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/item_textures.hpp"
#include "platform/psp/psp_pak_index.hpp"  // --verify reads the pak back like the PSP
#include "rendering/psp/psp_body_rules.hpp"  // NPC outfit geosets (outfit_geosets spec)
#include "rendering/m2_model_classifier.hpp"
#include "rendering/psp/psp_gameobject_format.hpp"  // kGoAnimDisplayBase

#include "gameobject_anim_select.hpp"  // clean room: gameobject_anim_select.md
#include "model_bake.hpp"
#include "npc_outfit.hpp"  // clean room: outfit_geosets.md 3.3
#include "skin_bake.hpp"  // creature anim
#include "fsd_bake.hpp"  // sounds ($FSD footfalls)
#include "attach_bake.hpp"  // mounts (seat attachments, "p_<n>")

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

using namespace wowee;
using namespace wowee::pspconv;

namespace {

constexpr uint32_t kKindCreatureModel = 5;
constexpr uint32_t kKindDisplay = 9;
constexpr uint32_t kKindSpawns = 10;
constexpr float kTileSize = 533.33333f;
constexpr size_t kHdCharacterVertexCeiling = 12000;  // upstream kShippedCharacterVertexCeiling
// Simplified LOD 1 (no upstream counterpart: a PSP budget) for models above
// this many triangles, to this share of the indices, relative error bound.
constexpr size_t kSimplifyMinTriangles = 1500;
constexpr size_t kSimplifyRatioPercent = 25;
constexpr float kSimplifyError = 0.05f;

// Creature anim - what the PSP bakes into "n_<n>".
constexpr uint32_t kKindCreatureSkin = 11;
// Mounts - "p_<n>": the attachment points of a model (WATP), baked for every model.
constexpr uint32_t kKindPoints = 12;
// What upstream MountFSM / AnimationController::setMounted look for on a
// mount model besides Stand / Walk / Run: the jump set (JumpStart, Jump,
// JumpEnd, Fall) and the rear-up (MountSpecial). Flight (Fly 135 / Hover
// 193) is in g_anims already. Baked right after Stand / Walk / Run so the
// --max-anim-kb cap drops other things first.
const std::vector<uint32_t> kMountAnims = {37, 38, 39, 40, 94};
bool g_spellAnimations = true;
bool g_animate = true;          // --no-anim: only the static m_ (as before)
// Priority order (a --max-anim-kb cap drops from the end; Stand's other
// variations - the idle fidgets - come after everything listed here, the
// state emotes after them):
// Stand, Walk, Run, Death, Rise, CombatWound, CombatCritical, Attack 1H / 2H /
// 2H loose / unarmed, Ready unarmed / 1H / 2H / 2H loose, Dodge, SwimIdle,
// Swim, Fly (135), Hover (193). A model without one simply lacks it.
std::vector<uint32_t> g_anims = {0, 4, 5, 1, 7, 9, 10, 17, 18, 19, 16, 25, 26, 27, 28, 30, 41, 42, 135, 193,
                                 52, 51, 2, 124};  // spells: NPC cast poses (READY_SPELL_OMNI/DIRECTED, SPELL, CHANNEL)
// NPC one-shot emotes (SMSG_EMOTE from scripts): talk, exclamation, question, bow.
std::vector<uint32_t> g_npcEmotes = {60, 64, 65, 66};
// Emotes.dbc AnimIDs of the state emotes (EmoteSpecProc != 0) - what UNIT_NPC_EMOTESTATE
// can ask any NPC for without the DB to say which; lowest priority (--no-state-emotes: none).
std::vector<uint32_t> g_stateEmotes;
bool g_useStateEmotes = true;
float g_rate = 30.0f;           // --rate: samples per second
uint32_t g_maxAnimKb = 384;     // --max-anim-kb: key+track bytes per model (0 = no cap)
bool g_allVariations = false;   // --all-variations: every variation of every id (else only Stand's)
bool g_verify = false;
float g_verifyLimit = 0.02f;
// An NPC's clothing by the players' rules where upstream's NPC path drops a piece
// (humanoidGeosets): a robe's kilt (the chest's GeosetGroup3, else the shirt's), a belt's buckle (18xx) and the
// tabard's mesh (1202).
// --npc-upstream-geosets: upstream's NPC rules as they were (A/B, and --check-humanoids compares the two).
bool g_npcPlayerRules = true;
// The game objects' animated M2s as displays (see main); default with every
// display, --gameobjects adds them to a --display bake, --no-gameobjects leaves them out, --go-display N,N picks some.
bool g_gameObjects = false, g_noGameObjects = false;
std::vector<uint32_t> g_goDisplays;
std::atomic<int> g_verifyOver{0};  // models whose n_ / p_ verify passed the limit
std::string g_dumpDir;

std::vector<uint32_t> parseIdList(const char* p) {
    std::vector<uint32_t> out;
    while (*p) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 10);
        if (end == p) break;
        out.push_back(static_cast<uint32_t>(id));
        p = *end == ',' ? end + 1 : end;
    }
    return out;
}

// printf into a string (a model's log, printed in model order after the threads). gnu_printf: MinGW's plain
// "printf" check is the MS one (no %zu), the UCRT vsnprintf is C99.
#if defined(__MINGW32__)
void appendf(std::string& s, const char* fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
#else
void appendf(std::string& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
void appendf(std::string& s, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, static_cast<size_t>(std::min<int>(n, static_cast<int>(sizeof(buf)) - 1)));
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// `f(i)` for i in [0, n) on `jobs` threads (1 = this thread), in the order `order` gives (all of [0, n)).
void parallelFor(size_t n, unsigned jobs, const std::vector<size_t>& order, const std::function<void(size_t)>& f) {
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (size_t k; (k = next.fetch_add(1)) < n;) f(order.empty() ? k : order[k]);
    };
    if (jobs <= 1 || n <= 1) {
        worker();
        return;
    }
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < jobs && t < n; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
}

// upstream CharacterRenderer::playAnimation: variation 0 of the id, else the
// first sequence with it; -1 when the model has none (psp_char_conv's copy).
int findSequence(const pipeline::M2Model& m, uint32_t animId) {
    int first = -1;
    for (size_t i = 0; i < m.sequences.size(); ++i) {
        if (m.sequences[i].id != animId) continue;
        if (m.sequences[i].variationIndex == 0) return static_cast<int>(i);
        if (first < 0) first = static_cast<int>(i);
    }
    return first;
}

// Keyframes of sequence `seq`: in the M2 (flag 0x20), else the external .anim
// (upstream loadExternalAnimations; psp_char_conv's copy).
bool loadSequence(const std::string& root, const std::string& m2Path, const std::vector<uint8_t>& m2Bytes,
                  pipeline::M2Model& m, int seq) {
    const auto& s = m.sequences[seq];
    if (s.flags & 0x20) return true;
    std::vector<uint8_t> anim;
    if (!readFile(root + "/" + lower(animPath(m2Path, s.id, s.variationIndex)), anim)) return false;
    pipeline::M2Loader::loadAnimFile(m2Bytes, anim, static_cast<uint32_t>(seq), m);
    return true;
}

bool loadDbc(const std::string& root, const char* name, pipeline::DBCFile& dbc) {
    std::vector<uint8_t> bytes;
    return readFile(root + "/dbfilesclient/" + name, bytes) && dbc.load(bytes);
}

// Existence checks against directory listings, each directory listed once (the first
// time a path in it is asked for). The full bake asks ~100 000 times (skins, bakes, CharSections, capes);
// one stat each took minutes on a busy disk. The extracted tree is lower case (psp_mpq_extract).
class FileIndex {
public:
    bool exists(const std::string& root, const std::string& gamePath) {
        const std::string key = lower(gamePath);
        const size_t slash = key.find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string() : key.substr(0, slash);
        const std::string name = slash == std::string::npos ? key : key.substr(slash + 1);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = dirs_.find(dir);
        if (it == dirs_.end()) {
            std::unordered_set<std::string> names;
            std::error_code ec;
            for (std::filesystem::directory_iterator d(root + "/" + dir, ec), end; !ec && d != end; d.increment(ec))
                names.insert(lower(d->path().filename().string()));
            it = dirs_.emplace(dir, std::move(names)).first;
        }
        return it->second.count(name) != 0;
    }
    size_t directories() const { return dirs_.size(); }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> dirs_;
};
FileIndex g_files;

bool fileExists(const std::string& root, const std::string& gamePath) { return g_files.exists(root, gamePath); }

bool fileExistsCb(const std::string& gamePath, void* ctx) {
    return fileExists(*static_cast<const std::string*>(ctx), gamePath);
}

std::string backslashLower(std::string s) {
    for (char& c : s) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The anim ids a model is asked for (ordered: the list, NPC emotes; mounts reorder).
std::vector<uint32_t> wantedAnims(bool rideable, const std::set<uint32_t>& extra) {
    std::vector<uint32_t> ids = g_anims;
    auto addId = [&](uint32_t id) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    };
    for (uint32_t id : extra) addId(id);
    if (rideable) {  // mounts - the jump set and rear-up right after Stand / Walk / Run.
        std::vector<uint32_t> first = {0, 4, 5};
        for (uint32_t id : kMountAnims) first.push_back(id);
        for (uint32_t id : ids)
            if (std::find(first.begin(), first.end(), id) == first.end()) first.push_back(id);
        ids.swap(first);
    }
    for (uint32_t id : g_npcEmotes) addId(id);
    return ids;
}

// --list-assets: a wanted sequence of this model lives in an .anim file that
// is not extracted yet (then the whole model folder is listed).
bool missingAnims(const std::string& root, const std::string& m2Path) {
    std::vector<uint8_t> m2;
    if (!readFile(root + "/" + lower(m2Path), m2)) return false;  // listed for the model anyway
    const pipeline::M2Model m = pipeline::M2Loader::load(m2);
    std::vector<uint32_t> ids = wantedAnims(true, {});
    ids.insert(ids.end(), g_stateEmotes.begin(), g_stateEmotes.end());
    for (const auto& q : m.sequences) {
        if (std::find(ids.begin(), ids.end(), q.id) == ids.end() || (q.flags & 0x20)) continue;
        if (q.variationIndex != 0 && q.id != 0 && !g_allVariations) continue;
        if (!fileExists(root, animPath(m2Path, q.id, q.variationIndex))) return true;
    }
    return false;
}

// ---- DBC tables (upstream buildCreatureDisplayLookups; Data/expansions/wotlk/dbc_layouts.json columns) ----

struct DisplayRow {
    uint32_t modelId = 0;
    uint32_t extraId = 0;
    std::string skin[3];
    float scale = 1.0f;
};

struct ExtraRow {
    uint8_t race = 0, sex = 0, skin = 0, face = 0, hairStyle = 0, hairColor = 0, facial = 0;
    uint32_t equip[11] = {};
    std::string bake;
};

struct Tables {
    std::map<uint32_t, DisplayRow> displays;
    std::map<uint32_t, ExtraRow> extras;
    std::map<uint32_t, std::string> modelPath;
    std::map<uint32_t, float> modelScale;
    std::map<uint32_t, uint16_t> hairGeoset;  // appearanceKey -> scalp geoset
    struct Facial {
        uint16_t g100, g200, g300;
    };
    std::map<uint32_t, Facial> facial;
    pipeline::DBCFile sections, items;
    pipeline::CharSectionsFields sectionFields{};

    bool load(const std::string& root) {
        pipeline::DBCFile cdi, cdie, cmd, chg, cfh;
        if (!loadDbc(root, "creaturedisplayinfo.dbc", cdi) || !loadDbc(root, "creaturedisplayinfoextra.dbc", cdie) ||
            !loadDbc(root, "creaturemodeldata.dbc", cmd) || !loadDbc(root, "charsections.dbc", sections) ||
            !loadDbc(root, "charhairgeosets.dbc", chg) || !loadDbc(root, "characterfacialhairstyles.dbc", cfh) ||
            !loadDbc(root, "itemdisplayinfo.dbc", items))
            return false;
        // CreatureDisplayInfo: ID 0, ModelID 1, ExtraDisplayId 3, CreatureModelScale 4, Skin1-3 6-8.
        for (uint32_t i = 0; i < cdi.getRecordCount(); ++i) {
            DisplayRow d;
            d.modelId = cdi.getUInt32(i, 1);
            d.extraId = cdi.getUInt32(i, 3);
            for (int s = 0; s < 3; ++s) d.skin[s] = cdi.getString(i, 6 + s);
            const float sc = cdi.getFloat(i, 4);
            if (sc > 0.0f) d.scale = sc;  // zero means unset
            displays[cdi.getUInt32(i, 0)] = d;
        }
        // CreatureDisplayInfoExtra: 19 fields -> 10 equip slots, BakeName 18;
        // 21 fields -> 11 slots (8-18), Flags 19, BakeName 20.
        const bool shortExtra = cdie.getFieldCount() <= 19;
        const int equipSlots = shortExtra ? 10 : 11;
        const uint32_t bakeField = shortExtra ? 18 : 20;
        for (uint32_t i = 0; i < cdie.getRecordCount(); ++i) {
            ExtraRow e;
            e.race = static_cast<uint8_t>(cdie.getUInt32(i, 1));
            e.sex = static_cast<uint8_t>(cdie.getUInt32(i, 2));
            e.skin = static_cast<uint8_t>(cdie.getUInt32(i, 3));
            e.face = static_cast<uint8_t>(cdie.getUInt32(i, 4));
            e.hairStyle = static_cast<uint8_t>(cdie.getUInt32(i, 5));
            e.hairColor = static_cast<uint8_t>(cdie.getUInt32(i, 6));
            e.facial = static_cast<uint8_t>(cdie.getUInt32(i, 7));
            for (int s = 0; s < equipSlots; ++s) e.equip[s] = cdie.getUInt32(i, 8 + s);
            e.bake = cdie.getString(i, bakeField);
            extras[cdie.getUInt32(i, 0)] = e;
        }
        // CreatureModelData: ID 0, ModelPath 2 (.mdx -> .m2), ModelScale 4.
        for (uint32_t i = 0; i < cmd.getRecordCount(); ++i) {
            std::string mdx = cmd.getString(i, 2);
            if (mdx.empty()) continue;
            if (mdx.size() >= 4) mdx = mdx.substr(0, mdx.size() - 4) + ".m2";
            const uint32_t id = cmd.getUInt32(i, 0);
            modelPath[id] = mdx;
            const float sc = cmd.getFloat(i, 4);
            if (sc > 0.0f) modelScale[id] = sc;
        }
        // Skin donors (entity_spawner.cpp:697): a display with no Skin1 and no
        // Extra row borrows the skins of the lowest display on the same file.
        std::map<std::string, std::pair<uint32_t, const DisplayRow*>> donor;
        for (const auto& [id, d] : displays) {
            if (d.skin[0].empty()) continue;
            auto p = modelPath.find(d.modelId);
            if (p == modelPath.end()) continue;
            const std::string key = backslashLower(p->second);
            auto it = donor.find(key);
            if (it == donor.end() || id < it->second.first) donor[key] = {id, &d};
        }
        int recovered = 0;
        for (auto& [id, d] : displays) {
            if (!d.skin[0].empty() || d.extraId != 0) continue;
            auto p = modelPath.find(d.modelId);
            if (p == modelPath.end()) continue;
            auto it = donor.find(backslashLower(p->second));
            if (it == donor.end()) continue;
            for (int s = 0; s < 3; ++s) d.skin[s] = it->second.second->skin[s];
            ++recovered;
        }
        // CharHairGeosets: RaceID 1, SexID 2, Variation 3, GeosetID 4, Showscalp 5.
        for (uint32_t i = 0; i < chg.getRecordCount(); ++i) {
            const bool scalp = chg.getFieldCount() > 5 && chg.getUInt32(i, 5) != 0;
            hairGeoset[core::appearanceKey(static_cast<uint8_t>(chg.getUInt32(i, 1)),
                                           static_cast<uint8_t>(chg.getUInt32(i, 2)),
                                           static_cast<uint8_t>(chg.getUInt32(i, 3)))] =
                static_cast<uint16_t>(scalp ? 1 : chg.getUInt32(i, 4));
        }
        const pipeline::FacialHairFields fh = pipeline::detectFacialHairFields(&cfh, nullptr);
        for (uint32_t i = 0; i < cfh.getRecordCount(); ++i) {
            facial[core::appearanceKey(static_cast<uint8_t>(cfh.getUInt32(i, 0)),
                                       static_cast<uint8_t>(cfh.getUInt32(i, 1)),
                                       static_cast<uint8_t>(cfh.getUInt32(i, 2)))] =
                Facial{static_cast<uint16_t>(cfh.getUInt32(i, fh.geoset100)),
                       static_cast<uint16_t>(cfh.getUInt32(i, fh.geoset200)),
                       static_cast<uint16_t>(cfh.getUInt32(i, fh.geoset300))};
        }
        sectionFields = pipeline::detectCharSectionsFields(&sections, nullptr);
        std::printf("dbc: %zu displays, %zu extras, %zu model paths, %d skins recovered\n", displays.size(),
                    extras.size(), modelPath.size(), recovered);
        return true;
    }

    // upstream getModelPathForDisplayId.
    std::string pathFor(uint32_t displayId) const {
        if (displayId == 0 || displayId > 100000) return "";
        if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
        if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";
        if (displayId == 31048 || displayId == 31049) return "Creature\\Wolf\\Wolf.m2";
        auto d = displays.find(displayId);
        if (d == displays.end()) {
            auto p = modelPath.find(displayId);  // a model id given directly
            return p == modelPath.end() ? "" : p->second;
        }
        auto p = modelPath.find(d->second.modelId);
        return p == modelPath.end() ? "" : p->second;
    }

    // ItemDisplayInfo GeosetGroup1 (column 7) of an item display.
    uint32_t geosetGroup1(uint32_t itemDisplayId) const {
        if (!itemDisplayId) return 0;
        const int32_t r = items.findRecordById(itemDisplayId);
        return r >= 0 ? items.getUInt32(static_cast<uint32_t>(r), 7) : 0;
    }
    // GeosetGroup3 (column 9) - a robe's kilt over the legs (psp_item_conv: geosetGroup3).
    uint32_t geosetGroup3(uint32_t itemDisplayId) const {
        if (!itemDisplayId) return 0;
        const int32_t r = items.findRecordById(itemDisplayId);
        return r >= 0 ? items.getUInt32(static_cast<uint32_t>(r), 9) : 0;
    }
    // The ItemDisplayInfo columns the NPC outfit builder reads (npc_outfit.hpp):
    // GeosetGroup[0..2] (columns 7..9), Flags (10), which body sections it paints (Texture[8], columns 15..22 -
    // only whether a section is painted matters; an NPC's body texture is baked).
    class ItemDisplayRows : public ItemDisplayLookup {
    public:
        explicit ItemDisplayRows(const pipeline::DBCFile& items) : items_(items) {}
        bool find(uint32_t displayId, NpcItemDisplayRow& out) const override {
            const int32_t r = items_.findRecordById(displayId);
            if (r < 0) return false;
            const auto row = static_cast<uint32_t>(r);
            for (uint32_t g = 0; g < 3; ++g) out.geosetGroup[g] = items_.getUInt32(row, 7 + g);
            out.flags = items_.getUInt32(row, 10);
            for (uint32_t s = 0; s < 8; ++s)
                out.textureNonEmpty[s] = items_.getFieldCount() > 15 + s && !items_.getString(row, 15 + s).empty();
            return true;
        }

    private:
        const pipeline::DBCFile& items_;
    };

    // Upstream normalizeHumanoidClothingGeosets' cape - the first existing file of
    // pipeline::capeTextureCandidates over LeftModelTexture (3) then RightModelTexture (4), "" = no cape.
    std::string capeTexture(const std::string& root, uint32_t capeDisplayId, bool female) const {
        if (!capeDisplayId) return "";
        const int32_t r = items.findRecordById(capeDisplayId);
        if (r < 0) return "";
        std::vector<std::string> names;
        for (uint32_t f : {3u, 4u}) {
            const std::string n = items.getString(static_cast<uint32_t>(r), f);
            if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        }
        std::vector<std::string> candidates;
        for (const auto& raw : names)
            for (auto& c : pipeline::capeTextureCandidates(raw, female))
                if (std::find(candidates.begin(), candidates.end(), c) == candidates.end())
                    candidates.push_back(std::move(c));
        for (const auto& p : candidates)
            if (fileExists(root, p)) return p;
        return "";
    }
};

// ---- textures: ids planned in display order, baked on the threads ----

struct TexJob {
    std::string key;    // lower-case game path (+ "|opaque"), or "composite:..."
    std::string path;   // game path (file textures)
    bool composite = false;
    bool opaque = false;  // TextureBank::get(..., opaque) - baked with alpha 1
    pipeline::CharacterSectionTextures sec;  // composite: skin, face, underwear
};

// "x_<id>" for every texture a display or a model asks for, in the order of the first request (the serial
// planning pass), so the ids do not depend on the threads. A file gets its id when it is there (TextureBank::
// get's test without reading it twice); one that then fails to decode leaves its id without a record - the
// PSP draws such a batch white, as upstream draws a texture it cannot load.
class TexturePlan {
public:
    explicit TexturePlan(std::string root) : root_(std::move(root)) {}

    // `opaque` - the use draws without alpha (blend 0, as psp_model_conv's
    // addBatches does): its own "x_<n>" baked with alpha 1 (the real colours, the whole CLUT for them),
    // key "<path>|opaque"; the same file for an alpha use stays a second texture.
    int request(const std::string& gamePath, bool opaque = false) {
        if (gamePath.empty()) return -1;
        const std::string file = lower(gamePath);
        const std::string key = opaque ? file + "|opaque" : file;
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second;
        int id = -1;
        if (isBlp(file)) {
            id = static_cast<int>(jobs_.size());
            TexJob j;
            j.key = key;
            j.path = gamePath;
            j.opaque = opaque;
            jobs_.push_back(std::move(j));
            if (opaque) ++opaque_;
        } else {
            ++missing_;
            if (missing_ <= 40) std::fprintf(stderr, "  missing texture %s\n", key.c_str());
        }
        ids_.emplace(key, id);
        return id;
    }
    // Skin, face and underwear composited (a humanoid without a usable bake): -1 without a skin file.
    int requestComposite(const std::string& key, const pipeline::CharacterSectionTextures& sec) {
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second;
        int id = -1;
        if (!sec.bodySkin.empty() && isBlp(lower(sec.bodySkin))) {
            id = static_cast<int>(jobs_.size());
            TexJob j;
            j.key = key;
            j.composite = true;
            j.sec = sec;
            jobs_.push_back(std::move(j));
        }
        ids_.emplace(key, id);
        return id;
    }
    // Lookup only (the model threads): the id planned for the path, -1 when none. `opaque`
    // takes the opaque variant when one was planned, else the plain one (a file only opaque batches draw may have
    // no plain one: planModel).
    int id(const std::string& gamePath, bool opaque = false) const {
        const std::string file = lower(gamePath);
        if (opaque) {
            auto o = ids_.find(file + "|opaque");
            if (o != ids_.end()) return o->second;
        }
        auto it = ids_.find(file);
        return it == ids_.end() ? -1 : it->second;
    }
    std::vector<TexJob>& jobs() { return jobs_; }
    size_t missing() const { return missing_; }
    size_t opaqueCount() const { return opaque_; }  // textures baked opaque
    // The file is there (request() would give it an id).
    bool has(const std::string& gamePath) const { return !gamePath.empty() && isBlp(lower(gamePath)); }

private:
    // The file is there (the directory listings of FileIndex: no open per texture on a busy disk).
    bool isBlp(const std::string& key) const { return fileExists(root_, key); }
    std::string root_;
    std::unordered_map<std::string, int> ids_;
    std::vector<TexJob> jobs_;
    size_t missing_ = 0;
    size_t opaque_ = 0;
};

// Skin, face and underwear composited on the 256 atlas (upstream
// compositeTextures order and regionFor), for a humanoid without a bake.
int compositeSkin(const std::string& root, const pipeline::CharacterSectionTextures& t, const std::string& key,
                  TextureBank& tex) {
    std::vector<uint8_t> bytes;
    if (t.bodySkin.empty() || !readFile(root + "/" + lower(t.bodySkin), bytes)) return -1;
    const pipeline::BLPImage baseImg = pipeline::BLPLoader::load(bytes);
    if (baseImg.data.empty()) return -1;
    constexpr int kSize = 256;
    std::vector<Rgb> canvas = resample(baseImg, kSize, kSize);
    for (Rgb& p : canvas) p.a = 1.0f;
    std::vector<std::string> overlays = {t.faceLower, t.faceUpper};
    overlays.insert(overlays.end(), t.underwear.begin(), t.underwear.end());
    for (const std::string& path : overlays) {
        if (path.empty()) continue;
        const Region r = regionFor(lower(path));
        if (!r.known || !readFile(root + "/" + lower(path), bytes)) continue;
        const pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
        if (img.data.empty()) continue;
        const std::vector<Rgb> px = resample(img, r.w, r.h);
        for (int y = 0; y < r.h; ++y) {
            for (int x = 0; x < r.w; ++x) {
                const Rgb& s = px[static_cast<size_t>(y) * r.w + x];
                Rgb& d = canvas[static_cast<size_t>(r.y + y) * kSize + r.x + x];
                d.r = s.r * s.a + d.r * (1.0f - s.a);
                d.g = s.g * s.a + d.g * (1.0f - s.a);
                d.b = s.b * s.a + d.b * (1.0f - s.a);
            }
        }
    }
    pipeline::BLPImage out;
    out.width = out.height = kSize;
    out.data.resize(static_cast<size_t>(kSize) * kSize * 4);
    for (size_t i = 0; i < canvas.size(); ++i) {
        out.data[i * 4 + 0] = static_cast<uint8_t>(quant(canvas[i].r, 255));
        out.data[i * 4 + 1] = static_cast<uint8_t>(quant(canvas[i].g, 255));
        out.data[i * 4 + 2] = static_cast<uint8_t>(quant(canvas[i].b, 255));
        out.data[i * 4 + 3] = 255;
    }
    return tex.add(key, out);
}

// One texture job -> its WTEX blob (empty: unreadable). A TextureBank of its own per job (thread-safe).
std::vector<uint8_t> bakeTexture(const std::string& root, int texMax, const TexJob& j) {
    std::vector<PakItem> out;
    TextureBank bank(root, texMax, out);
    if (j.composite) {
        compositeSkin(root, j.sec, j.key, bank);
    } else {
        bool alpha = false;
        bank.get(j.path, alpha, j.opaque);  // the opaque variant when asked
    }
    return out.empty() ? std::vector<uint8_t>() : std::move(out[0].blob);
}

// ---- models ----

// Batch texture per resolveBatchTexture: hair (type 6) wins, then the first
// slot that is not white. A named type-0 file is baked; a missing one is
// white. Replaceable slots count as filled (the display fills them).
// `texId` gives a type-0 file's id (the plan's request, or its lookup on the threads).
uint16_t batchTexture(const pipeline::M2Model& m, const pipeline::M2Batch& b,
                      const std::function<int(const std::string&)>& texId, uint32_t& texFlags) {
    const uint32_t combos = std::min<uint32_t>(b.textureCount ? b.textureCount : 1u, 8u);
    uint16_t firstNonWhite = fmt::kNoTexture;
    for (uint32_t i = 0; i < combos; ++i) {
        const uint32_t pos = b.textureIndex + i;
        if (b.textureIndex == 0xFFFF || pos >= m.textureLookup.size()) break;
        const uint16_t slot = m.textureLookup[pos];
        if (slot >= m.textures.size()) continue;
        const auto& t = m.textures[slot];
        uint16_t id = fmt::kNoTexture;
        if (t.type == 6) {
            texFlags = t.flags;
            return static_cast<uint16_t>(fmt::kTexSlot + 6);
        }
        if (t.type != 0 && t.type < static_cast<uint32_t>(fmt::kTexSlotCount)) {
            id = static_cast<uint16_t>(fmt::kTexSlot + t.type);
        } else if (t.type == 0 && !std::string(t.filename.c_str()).empty()) {
            const int x = texId(t.filename.c_str());
            if (x >= 0) id = static_cast<uint16_t>(x);
        }
        if (id != fmt::kNoTexture && firstNonWhite == fmt::kNoTexture) {
            firstNonWhite = id;
            texFlags = t.flags;
        }
    }
    return firstNonWhite;
}

uint32_t levelZeroIndices(const pipeline::M2Model& m) {
    uint32_t n = 0;
    for (const auto& b : m.batches)
        if (b.submeshLevel == 0) n += b.indexCount;
    return n;
}

// upstream EntitySpawner::processPendingMount: "the seat ... attachment 0 ("MountMain"), and every rideable
// model carries one" - a seat when attachment 0's z > 0.1 (haveSeatPoint). PSP bake policy: a character
// model's 0 is its shield point, never a seat (its animation budget stays with the NPC set).
bool rideable(const pipeline::M2Model& m, const std::string& key) {
    if (key.rfind("character/", 0) == 0) return false;
    for (const auto& a : m.attachments)
        if (a.id == 0) return a.position.z > 0.1f;
    return false;
}

struct ModelPlan {
    std::string path, key;     // game path (.m2) / lower case
    int id = -1;               // m_/n_/p_ number; -1: unreadable (no display uses it)
    size_t m2Vertices = 0;     // upstream's HD-model test
    std::unordered_set<uint16_t> allGeosets;  // every submesh of skin 00 (upstream md->batches)
    bool rideable = false;
    uintmax_t m2Bytes = 0;     // biggest first on the threads
    std::set<uint32_t> extraAnims;  // --spawns: the spawns' emote states (Emotes.dbc AnimID)
    // Per replaceable slot type, the skin-00 batches drawing it - any, any not opaque.
    bool slotUsed[fmt::kTexSlotCount] = {};
    bool slotAlpha[fmt::kTexSlotCount] = {};
    std::unordered_set<std::string> opaqueFiles;  // lower type-0 files only opaque batches draw (baked opaque)
    size_t bothWays = 0;  // files the model draws both opaque and with alpha (they keep the plain texture)
    // Bake results.
    bool baked = false;
    std::vector<uint16_t> batchGeoset;   // per output batch, LOD 0 then LOD 1
    std::vector<uint16_t> batchTex;      // per output batch: x_<n>, kNoTexture or kTexSlot + type
    float height = 0.0f;
    uint32_t triangles = 0;
    size_t mBytes = 0, nBytes = 0, animBytes = 0, pBytes = 0, points = 0;
    std::string log;
};

// The serial half of a model: read it (and skin 00: its geosets), and request its fixed textures - every
// named type-0 texture of the M2, in table order (a superset of what the batches use; the bake looks them up).
// False when unreadable.
bool planModel(const std::string& root, ModelPlan& mp, TexturePlan& tex) {
    std::vector<uint8_t> m2;
    pipeline::M2Model model;
    if (!readFile(root + "/" + mp.key, m2) || !loadM2(m2, root + "/" + lower(pipeline::skinPathForM2(mp.path)), model)) {
        std::fprintf(stderr, "  model %s: unreadable\n", mp.key.c_str());
        return false;
    }
    mp.m2Bytes = m2.size();
    mp.m2Vertices = model.vertices.size();
    for (const auto& b : model.batches) mp.allGeosets.insert(b.submeshId);
    mp.rideable = rideable(model, mp.key);
    // What each skin-00 batch draws (the batches addBatches bakes), picked as
    // batchTexture picks it: hair wins, else the first slot that is not white. A file only opaque batches draw
    // (M2 blend mode 0 = opaque: the PSP draws it without alpha test) is baked opaque; the others keep
    // the plain texture, as before - also a file the model draws both ways (one texture in RAM, not two: the
    // NPC budget; the halving keeps its colours). A replaceable slot remembers whether every
    // batch drawing it is opaque (DisplayPlanner bakes the display's texture for it the same way).
    std::unordered_map<std::string, uint8_t> fileUse;  // lower file -> 1: an opaque batch draws it, 2: another one
    for (const auto& b : model.batches) {
        if (b.submeshLevel != 0 || b.indexCount < 3) continue;
        const uint16_t blendMode = b.materialIndex < model.materials.size() ? model.materials[b.materialIndex].blendMode : 0;
        const bool opaque = mapBlend(blendMode) == fmt::kBlendOpaque;
        const uint32_t combos = std::min<uint32_t>(b.textureCount ? b.textureCount : 1u, 8u);
        int pick = -1;
        for (uint32_t i = 0; i < combos; ++i) {
            const uint32_t pos = b.textureIndex + i;
            if (b.textureIndex == 0xFFFF || pos >= model.textureLookup.size()) break;
            const uint16_t slot = model.textureLookup[pos];
            if (slot >= model.textures.size()) continue;
            const auto& t = model.textures[slot];
            if (t.type == 6) {
                pick = slot;
                break;
            }
            if (pick < 0 && ((t.type != 0 && t.type < static_cast<uint32_t>(fmt::kTexSlotCount)) ||
                             (t.type == 0 && tex.has(t.filename.c_str()))))
                pick = slot;
        }
        if (pick < 0) continue;
        const auto& t = model.textures[pick];
        if (t.type != 0) {
            mp.slotUsed[t.type] = true;
            if (!opaque) mp.slotAlpha[t.type] = true;
        } else {
            fileUse[lower(t.filename.c_str())] |= opaque ? 1 : 2;
        }
    }
    for (const auto& t : model.textures) {
        if (t.type != 0 || std::string(t.filename.c_str()).empty()) continue;
        const auto it = fileUse.find(lower(t.filename.c_str()));
        const uint8_t use = it == fileUse.end() ? 0 : it->second;
        tex.request(t.filename.c_str(), use == 1);
    }
    for (const auto& u : fileUse) {
        if (u.second == 1) mp.opaqueFiles.insert(u.first);
        mp.bothWays += u.second == 3 ? 1 : 0;
    }
    return true;
}

// The display's texture for a replaceable slot type is baked opaque when every batch of the
// model drawing that slot is opaque (one texture per slot: an alpha use keeps the alpha for all).
bool slotOpaque(const ModelPlan& mp, std::initializer_list<uint32_t> types) {
    bool any = false;
    for (uint32_t t : types) {
        if (t >= static_cast<uint32_t>(fmt::kTexSlotCount) || !mp.slotUsed[t]) continue;
        if (mp.slotAlpha[t]) return false;
        any = true;
    }
    return any;
}

class ModelBaker {
public:
    ModelBaker(std::string root, const TexturePlan& tex, SortedPakWriter& out, std::mutex& outMutex)
        : root_(std::move(root)), tex_(tex), out_(out), outMutex_(outMutex) {}

    void bake(ModelPlan& mp) {
        std::vector<uint8_t> m2;
        pipeline::M2Model model;
        if (!readFile(root_ + "/" + mp.key, m2) ||
            !loadM2(m2, root_ + "/" + lower(pipeline::skinPathForM2(mp.path)), model)) {
            appendf(mp.log, "  model %s: unreadable\n", mp.key.c_str());
            return;
        }
        const std::vector<pipeline::M2Vertex> bindVertices = model.vertices;  // before the stand pose
        const bool posed = poseStand(root_, mp.path, m2, model);
        WmdlBuilder o;
        std::vector<SkinBatchIn> skinBatches;  // creature anim
        std::vector<SkinBatchIn>* sb = g_animate ? &skinBatches : nullptr;
        bool ok = addBatches(model, 0, o, mp, false, sb);
        const size_t lod0Batches = mp.batchGeoset.size();
        // LOD 1: the last lower skin with at most 60 % of the indices, on the
        // posed vertices (a skin only replaces indices and batches).
        const uint32_t fullIdx = levelZeroIndices(model);
        const std::string base = mp.key.substr(0, mp.key.size() - 3);
        for (int s = 3; ok && s >= 1; --s) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "%02d.skin", s);
            std::vector<uint8_t> skin;
            if (!readFile(root_ + "/" + base + suffix, skin)) continue;
            pipeline::M2Model low = model;
            if (!pipeline::M2Loader::loadSkin(skin, low)) continue;
            if (levelZeroIndices(low) * 10 > fullIdx * 6) break;
            if (!addBatches(low, 1, o, mp, false, sb)) {
                mp.batchGeoset.resize(lod0Batches);
                mp.batchTex.resize(lod0Batches);
            }
            break;
        }
        // No lower skin worth it (the shipped character models repeat skin 00
        // in 01..03): heavy models get a simplified LOD 1 for the distance.
        if (ok && mp.batchGeoset.size() == lod0Batches && o.triangles(0) > kSimplifyMinTriangles) {
            if (!addBatches(model, 1, o, mp, true, sb)) {
                mp.batchGeoset.resize(lod0Batches);
                mp.batchTex.resize(lod0Batches);
            }
        }
        // A failed LOD 1 was dropped from the WMDL: drop it from the skin too.
        if (skinBatches.size() > mp.batchGeoset.size()) skinBatches.resize(lod0Batches);
        if (o.empty()) {
            appendf(mp.log, "  model %s: no batches\n", mp.key.c_str());
            mp.batchGeoset.clear();
            mp.batchTex.clear();
            return;
        }
        WmdlInfo info;
        std::vector<uint8_t> blob = o.build(model.vertices, 0, true, info);
        const auto* h = reinterpret_cast<const fmt::ModelHeader*>(blob.data());
        if (mp.batchGeoset.size() != static_cast<size_t>(h->batchCount[0]) + h->batchCount[1]) {
            appendf(mp.log, "  model %s: batch count mismatch\n", mp.key.c_str());
            mp.batchGeoset.clear();
            mp.batchTex.clear();
            return;
        }
        mp.triangles = info.triangles;
        mp.height = h->center[2] + h->half[2];
        mp.mBytes = blob.size();
        appendf(mp.log, "  m_%d %s: %zu verts, %u tris, %zu+%zu batches, pose %s, height %.2f%s\n", mp.id,
                mp.key.c_str(), info.vertices, info.triangles, info.batches[0], info.batches[1],
                posed ? "stand" : "bind", mp.height, mp.rideable ? ", seat" : "");
        add("m_" + std::to_string(mp.id), kKindCreatureModel, blob);
        std::vector<uint8_t> nBlob;
        if (g_animate && posed)
            nBlob = bakeAnimated(mp, m2, model, bindVertices, skinBatches, lod0Batches);
        else if (g_animate)
            appendf(mp.log, "  n_%d: no Stand pose (no bones or Stand .anim missing) - static only\n", mp.id);
        bakePoints(mp, model, bindVertices, posed, nBlob.empty() ? nullptr : &nBlob);
        mp.baked = true;
    }

private:
    void add(const std::string& name, uint32_t kind, const std::vector<uint8_t>& blob) {
        std::lock_guard<std::mutex> lock(outMutex_);
        out_.add(name, kind, blob.data(), blob.size());
    }

    // simplify: LOD 1 made from the same batches by meshoptimizer (the same
    // batch list and geosets as LOD 0, so one display mask serves both).
    // skin (creature anim): the same batches as source triangles for n_.
    bool addBatches(const pipeline::M2Model& m, int lod, WmdlBuilder& o, ModelPlan& mp, bool simplify = false,
                    std::vector<SkinBatchIn>* skin = nullptr) {
        for (const auto& b : m.batches) {
            if (b.submeshLevel != 0 || b.indexCount < 3 || b.indexStart + b.indexCount > m.indices.size()) continue;
            uint16_t blendMode = 0, matFlags = 0;
            if (b.materialIndex < m.materials.size()) {
                blendMode = m.materials[b.materialIndex].blendMode;
                matFlags = m.materials[b.materialIndex].flags;
            }
            // An opaque batch takes the opaque variant of a file the model bakes opaque
            // (planModel's opaqueFiles - not one another model asked for: one texture per file here).
            const bool opaque = mapBlend(blendMode) == fmt::kBlendOpaque;
            auto lookup = [this, &mp, opaque](const std::string& p) {
                return tex_.id(p, opaque && mp.opaqueFiles.count(lower(p)) != 0);
            };
            fmt::ModelBatch ob{};
            uint32_t texFlags = 0;
            ob.texture = batchTexture(m, b, lookup, texFlags);
            ob.blend = mapBlend(blendMode);
            ob.flags = static_cast<uint8_t>(((texFlags & 1) ? fmt::kBatchWrapU : 0) |
                                            ((texFlags & 2) ? fmt::kBatchWrapV : 0) |
                                            ((matFlags & 1) ? fmt::kBatchUnlit : 0));
            if (simplify) {
                std::vector<unsigned int> src(m.indices.begin() + b.indexStart,
                                              m.indices.begin() + b.indexStart + b.indexCount);
                std::vector<unsigned int> dst(src.size());
                const size_t target = std::max<size_t>(3, src.size() * kSimplifyRatioPercent / 100 / 3 * 3);
                dst.resize(meshopt_simplify(dst.data(), src.data(), src.size(), &m.vertices[0].position.x,
                                            m.vertices.size(), sizeof(pipeline::M2Vertex), target, kSimplifyError,
                                            0, nullptr));
                if (dst.size() < 3) dst = src;
                const std::vector<uint16_t> idx(dst.begin(), dst.end());
                if (!o.add(idx, 0, static_cast<uint32_t>(idx.size()), lod, 0, ob, b.submeshId)) return false;
                if (skin)
                    skin->push_back({std::vector<uint32_t>(dst.begin(), dst.begin() + dst.size() / 3 * 3), ob,
                                     b.submeshId});
            } else if (!o.add(m.indices, b.indexStart, b.indexCount, lod, 0, ob, b.submeshId)) {
                return false;
            } else if (skin) {
                skin->push_back({std::vector<uint32_t>(m.indices.begin() + b.indexStart,
                                                       m.indices.begin() + b.indexStart + b.indexCount / 3 * 3),
                                 ob, b.submeshId});
            }
            mp.batchGeoset.push_back(b.submeshId);
            mp.batchTex.push_back(ob.texture);
        }
        return true;
    }

    // Mounts, every model - "p_<id>": the points of kCreaturePointIds the
    // model resolves (findAttachmentBone; their bones are kept in n_) and the rider height upstream guesses
    // from the tight bounds of the M2's vertices (EntitySpawner::processPendingMount / loadRemoteMountModel:
    // maxZ * 0.8, else extent * 0.75, else 1.8).
    void bakePoints(ModelPlan& mp, const pipeline::M2Model& model, const std::vector<pipeline::M2Vertex>& bind,
                    bool posed, const std::vector<uint8_t>* nBlob) {
        float riderHeight = 1.8f;
        if (!bind.empty()) {
            float minZ = bind[0].position.z, maxZ = bind[0].position.z;
            for (const auto& v : bind) {
                minZ = std::min(minZ, v.position.z);
                maxZ = std::max(maxZ, v.position.z);
            }
            const float extent = maxZ - minZ;
            if (extent > 0.5f) {
                riderHeight = maxZ * 0.8f;
                if (riderHeight < 1.0f) riderHeight = extent * 0.75f;
                if (riderHeight < 1.0f) riderHeight = 1.8f;
            }
        }
        std::vector<uint32_t> ids(std::begin(fmt::kCreaturePointIds), std::end(fmt::kCreaturePointIds));
        // + the vehicle seats' attachments (points only - "n_" keeps kCreaturePointIds' bones).
        ids.insert(ids.end(), std::begin(fmt::kVehicleSeatPointIds), std::end(fmt::kVehicleSeatPointIds));
        fmt::AttachPoint rider{};
        rider.id = fmt::kAttachRiderHeight;
        rider.bone = fmt::kNoAttachBone;
        rider.position[2] = riderHeight;
        rider.stand[0] = rider.stand[4] = rider.stand[8] = 1.0f;
        const std::vector<fmt::AttachPoint> extra = {rider};
        AttachBakeInfo ai;
        std::vector<uint8_t> watp =
            bakeAttachTable(model, posed ? findSequence(model, 0) : -1, nBlob, g_verify, ai, &ids, &extra);
        float seatZ = -1.0f;
        for (const auto& a : model.attachments)
            if (a.id == 0) {
                seatZ = a.position.z;
                break;
            }
        appendf(mp.log, "  p_%d: %zu points (%zu in n_), seat0 z %.2f, rider height %.2f", mp.id, ai.points,
                ai.inSkin, seatZ, riderHeight);
        if (g_verify && ai.samples) appendf(mp.log, ", verify max %.4f yd (%s)", ai.maxErr, ai.worst.c_str());
        appendf(mp.log, "\n");
        if (g_verify && ai.maxErr > g_verifyLimit) {
            ++g_verifyOver;
            appendf(mp.log, "  VERIFY p_%d over the limit\n", mp.id);
        }
        mp.points = ai.points;
        mp.pBytes = watp.size();
        add("p_" + std::to_string(mp.id), kKindPoints, watp);
    }

    // Creature anim - "n_<id>": the WMDL's batches (LOD 0 then LOD 1)
    // skinned for the GE with the NPC sequences. Returns the blob (empty: static only).
    std::vector<uint8_t> bakeAnimated(ModelPlan& mp, const std::vector<uint8_t>& m2, pipeline::M2Model& model,
                                      const std::vector<pipeline::M2Vertex>& bind,
                                      const std::vector<SkinBatchIn>& batches, size_t lod0Batches) {
        if (batches.size() != mp.batchGeoset.size()) {
            appendf(mp.log, "  n_%d: %zu skin batches for %zu model batches - static only\n", mp.id, batches.size(),
                    mp.batchGeoset.size());
            return {};
        }
        // Wanted ids in priority order: the list (mounts: the jump set after Stand / Walk / Run), the spawns'
        // emote states, the NPC one-shot emotes; Stand's other variations; the state emotes last.
        const std::vector<uint32_t> ids = wantedAnims(mp.rideable, mp.extraAnims);
        std::vector<uint16_t> keep;  // the bones of every point "p_" names
        for (uint16_t id : fmt::kCreaturePointIds) {
            uint16_t bone = 0;
            glm::vec3 off;
            if (findAttachmentBone(model, id, bone, off)) keep.push_back(bone);
        }
        std::vector<int> seqs, late;
        auto addFirst = [&](uint32_t id, std::vector<int>& to) -> int {
            const int first = findSequence(model, id);
            if (first < 0) return -1;
            if (std::find(seqs.begin(), seqs.end(), first) != seqs.end() ||
                std::find(late.begin(), late.end(), first) != late.end())
                return -1;
            if (!loadSequence(root_, mp.path, m2, model, first)) {
                appendf(mp.log, "  n_%d anim %u: .anim file missing, skipped\n", mp.id, id);
                return -1;
            }
            to.push_back(first);
            return first;
        };
        for (uint32_t id : ids) {
            const int first = addFirst(id, seqs);
            if (first < 0) continue;
            if (id != 0 && !g_allVariations) continue;
            for (size_t i = 0; i < model.sequences.size(); ++i) {
                if (model.sequences[i].id != id || static_cast<int>(i) == first) continue;
                if (loadSequence(root_, mp.path, m2, model, static_cast<int>(i))) late.push_back(static_cast<int>(i));
            }
        }
        seqs.insert(seqs.end(), late.begin(), late.end());
        late.clear();
        if (g_useStateEmotes)
            for (uint32_t id : g_stateEmotes)
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) addFirst(id, seqs);
        if (seqs.empty()) {
            appendf(mp.log, "  n_%d: no wanted sequence - static only\n", mp.id);
            return {};
        }
        const int standSeq = findSequence(model, 0);
        SkinBakeInfo si;
        auto bakeFirst = [&](size_t n, SkinBakeInfo& info) {
            const std::vector<int> prefix(seqs.begin(), seqs.begin() + static_cast<std::ptrdiff_t>(n));
            return bakeSkinned(model, bind, batches, prefix, standSeq, g_rate, info, false,
                               keep.empty() ? nullptr : &keep);  // mounts (seat bones)
        };
        // The cap keeps the longest prefix of the priority list that fits (the animation bytes grow with
        // every sequence): one bake when all fit, else a binary search (was one bake per dropped sequence).
        std::vector<uint8_t> blob = bakeFirst(seqs.size(), si);
        size_t dropped = 0;
        if (!blob.empty() && g_maxAnimKb && si.animBytes > g_maxAnimKb * 1024u && seqs.size() > 1) {
            size_t lo = 1, hi = seqs.size() - 1;  // the answer is in [lo, hi]; prefix lo is kept even over the cap
            while (lo < hi) {
                const size_t mid = (lo + hi + 1) / 2;
                SkinBakeInfo probe;
                const std::vector<uint8_t> b = bakeFirst(mid, probe);
                if (!b.empty() && probe.animBytes <= g_maxAnimKb * 1024u) lo = mid;
                else hi = mid - 1;
            }
            dropped = seqs.size() - lo;
            seqs.resize(lo);
            blob = bakeFirst(seqs.size(), si);
        }
        if (blob.empty()) {
            appendf(mp.log, "  n_%d: skinned bake failed - static only\n", mp.id);
            return {};
        }
        // LOD split for the PSP (psp_model_format.hpp kSkinHasLod).
        fmt::SkinHeader h;
        std::memcpy(&h, blob.data(), sizeof(h));
        if (lod0Batches < batches.size()) {
            h.flags |= fmt::kSkinHasLod;
            h.reserved[0] = static_cast<uint32_t>(lod0Batches);
        }
        std::memcpy(blob.data(), &h, sizeof(h));
        const size_t fsd = appendFootstepEvents(blob, root_, mp.path, m2, model, seqs);  // sounds
        if (fsd) appendf(mp.log, "  n_%d: %zu footstep events\n", mp.id, fsd);           // sounds
        appendf(mp.log,
                "  n_%d: %zu bones, %zu seqs, %zu sub-batches (%zu+%zu batches), %zu verts, %zu tris, "
                "vertices %zu KB, anim %zu KB, total %zu KB",
                mp.id, si.bones, si.sequences, si.subBatches, lod0Batches, batches.size() - lod0Batches, si.vertices,
                si.triangles, si.vertexBytes / 1024, si.animBytes / 1024, blob.size() / 1024);
        if (dropped) appendf(mp.log, ", %zu seqs over --max-anim-kb dropped", dropped);
        if (si.trimmedInfluences || si.weightless)
            appendf(mp.log, ", trimmed %zu, weightless %zu", si.trimmedInfluences, si.weightless);
        appendf(mp.log, "\n    anims:");
        for (int q : seqs) {
            appendf(mp.log, " %u", model.sequences[q].id);
            if (model.sequences[q].variationIndex) appendf(mp.log, ".%u", model.sequences[q].variationIndex);
        }
        appendf(mp.log, "\n");
        if (g_verify) {
            std::vector<uint8_t> dump;
            const SkinVerifyResult r =
                verifySkinned(blob, model, bind, seqs, si, batches, g_dumpDir.empty() ? nullptr : &dump, nullptr);
            appendf(mp.log, "  verify n_%d: max %.4f yd, rms %.5f yd over %zu vertex samples; worst: %s\n", mp.id,
                    r.maxErr, r.samples ? std::sqrt(r.sumSq / static_cast<double>(r.samples)) : 0.0, r.samples,
                    r.worst.c_str());
            if (!dump.empty()) {
                const std::string path = g_dumpDir + "/n_" + std::to_string(mp.id) + ".tris";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(dump.data(), 1, dump.size(), f);
                    std::fclose(f);
                }
            }
            if (r.maxErr > g_verifyLimit) {
                ++g_verifyOver;
                appendf(mp.log, "  VERIFY n_%d over the limit\n", mp.id);
            }
        }
        if (!g_dumpDir.empty()) dumpSkinText(blob, mp.key, g_dumpDir + "/n_" + std::to_string(mp.id) + ".txt");
        mp.nBytes = blob.size();
        mp.animBytes = si.animBytes;
        add("n_" + std::to_string(mp.id), kKindCreatureSkin, blob);
        return blob;
    }

    // --dump: the blob as the PSP reads it (SkinView), one line per sequence.
    static void dumpSkinText(const std::vector<uint8_t>& blob, const std::string& key, const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return;
        fmt::SkinView v;
        if (v.init(blob.data(), static_cast<uint32_t>(blob.size()))) {
            std::fprintf(f, "model %s\nbones %u seqs %u batches %u (lod0 %u) subbatches %u keys %u\n", key.c_str(),
                         v.h->boneCount, v.h->sequenceCount, v.h->batchCount,
                         (v.h->flags & fmt::kSkinHasLod) ? v.h->reserved[0] : v.h->batchCount, v.h->subBatchCount,
                         v.h->keyCount);
            for (int q = 0; q < v.h->sequenceCount; ++q) {
                const auto& a = v.sequences[q];
                std::fprintf(f, "seq %d anim %u var %u ms %u frames %u blend %u freq %d replay %u-%u\n", q, a.animId,
                             a.variation, a.durationMs, a.frameCount, a.blendTimeMs, a.frequency, a.replayMin,
                             a.replayMax);
            }
        } else {
            std::fprintf(f, "blob rejected by SkinView::init\n");
        }
        std::fclose(f);
    }

    std::string root_;
    const TexturePlan& tex_;
    SortedPakWriter& out_;
    std::mutex& outMutex_;
};

// ---- displays ----

// upstream applyCreatureDisplayTextures resolveCreatureSkinPath: a bare name
// is looked for in the model's folder first.
std::string creatureSkinPath(const std::string& root, const std::string& field, const std::string& modelDir) {
    std::string raw = field;
    for (char& c : raw)
        if (c == '/') c = '\\';
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(), notSpace));
    raw.erase(std::find_if(raw.rbegin(), raw.rend(), notSpace).base(), raw.end());
    if (raw.empty()) return "";
    const bool hasExt = raw.size() >= 4 && backslashLower(raw.substr(raw.size() - 4)) == ".blp";
    std::vector<std::string> cand;
    if (raw.find('\\') != std::string::npos) {
        cand.push_back(raw);
        if (!hasExt) cand.push_back(raw + ".blp");
    } else {
        cand.push_back(modelDir + raw);
        if (!hasExt) cand.push_back(modelDir + raw + ".blp");
        cand.push_back(raw);
        if (!hasExt) cand.push_back(raw + ".blp");
    }
    for (const auto& c : cand)
        if (fileExists(root, c)) return c;
    return "";
}

// upstream normalizeHumanoidClothingGeosets (entity_spawner.cpp:1281) for a
// display with an Extra row. The cape as upstream - shown
// (cloakGeoset of its GeosetGroup1, group 16 kept) when a cape texture exists,
// else group 15 keeps the "no cape" panel and group 16 goes.
std::unordered_set<uint16_t> humanoidGeosets(const Tables& db, const ExtraRow& e,
                                             const std::unordered_set<uint16_t>& all, bool cape, bool& applies) {
    std::unordered_map<uint16_t, uint16_t> firstByGroup;
    bool has[21] = {};
    for (uint16_t sid : all) {
        const uint16_t g = static_cast<uint16_t>(sid / 100);
        auto it = firstByGroup.find(g);
        if (it == firstByGroup.end() || sid < it->second) firstByGroup[g] = sid;
        if (g < 21) has[g] = true;
    }
    std::unordered_set<uint16_t> out;
    applies = has[3] || has[4] || has[5] || has[8] || has[12] || has[13] || has[15];
    if (!applies) return out;
    uint16_t scalp = 1, f100 = 100, f200 = 200, f300 = 300;
    auto hg = db.hairGeoset.find(core::appearanceKey(e.race, e.sex, e.hairStyle));
    if (hg != db.hairGeoset.end() && hg->second > 0) scalp = hg->second;
    auto fc = db.facial.find(core::appearanceKey(e.race, e.sex, e.facial));
    if (fc != db.facial.end()) {
        f100 = static_cast<uint16_t>(100 + fc->second.g100);
        f200 = static_cast<uint16_t>(200 + fc->second.g200);
        f300 = static_cast<uint16_t>(300 + fc->second.g300);
    }
    const bool tabard = e.equip[9] != 0;
    uint32_t chestGG = db.geosetGroup1(e.equip[3]);
    if (chestGG == 0) chestGG = db.geosetGroup1(e.equip[2]);  // shirt
    const uint32_t legsGG = db.geosetGroup1(e.equip[5]);
    const uint32_t feetGG = db.geosetGroup1(e.equip[6]);
    const uint32_t glovesGG = db.geosetGroup1(e.equip[8]);
    const uint32_t capeGG = db.geosetGroup1(e.equip[10]);
    // The players' rules for a robe (the chest's GeosetGroup3 -> the kilt, unless leggings name
    // their own) and a belt (GeosetGroup1 -> its buckle, 18xx) - upstream's NPC path read neither (core::equipment::
    // kRobeKiltBare's note: "why an NPC in a robe wore trousers under it while a player in the same robe did not").
    // The chest's GeosetGroup3, else the shirt's (as the sleeves above: the chest first) - a long shirt's skirt
    // (ItemDisplayInfo 9719 and the like).
    uint32_t robeGG = g_npcPlayerRules ? db.geosetGroup3(e.equip[3]) : 0u;
    if (g_npcPlayerRules && robeGG == 0) robeGG = db.geosetGroup3(e.equip[2]);
    const uint32_t beltGG = g_npcPlayerRules ? db.geosetGroup1(e.equip[4]) : 0u;
    for (uint16_t sid : all) {
        const uint16_t g = static_cast<uint16_t>(sid / 100);
        if (g == 3 || g == 4 || g == 8 || g == 12 || g == 13 || g == 15 || g == 17 || g == 18) continue;
        if (!cape && g == 16) continue;  // no cape
        if (sid < 100 && sid != 0 && sid != scalp) continue;
        if (g == 1) {
            uint16_t want = f100;
            if (!all.count(want)) want = all.count(101) ? 101 : firstByGroup[1];
            if (sid != want) continue;
        }
        if (g == 2) {
            uint16_t want = f200;
            if (!all.count(want)) {
                if (all.count(201)) want = 201;
                else if (all.count(200)) want = 200;
                else {
                    auto f = firstByGroup.find(2);
                    want = f != firstByGroup.end() ? f->second : 0;
                }
            }
            if (sid != want) continue;
        }
        out.insert(sid);
    }
    auto add = [&](uint16_t want) {
        const uint16_t s = core::resolveGeoset(want, all);
        if (s != 0) out.insert(s);
    };
    if (has[4]) add(glovesGG ? core::equippedGeoset(core::equipment::kGlovesBare, glovesGG) : core::kGeosetBareForearms);
    if (has[5]) add(feetGG ? core::equippedGeoset(core::equipment::kBootsBare, feetGG) : core::kGeosetBareShins);
    if (has[8] && chestGG) add(core::equippedGeoset(core::equipment::kChestBare, chestGG));
    if (has[12] && tabard) {
        const uint32_t tg = db.geosetGroup1(e.equip[9]);
        if (g_npcPlayerRules) {
            // The tabard's mesh. ItemDisplayInfo counts from the bare variant (1201, "none" - no
            // 3.3.5a model has it); a tabard's G 1 is 1202, the cloth over the hips every character model carries.
            // Upstream's kTabardBase 1200 + G asked for 1201 and resolveGeoset gave nothing: no tabard mesh.
            add(core::equippedGeoset(core::kGeosetDefaultTabard, tg ? tg : 1u));
        } else {
            add(tg ? core::equippedGeoset(core::equipment::kTabardBase, tg) : core::kGeosetDefaultTabard);
        }
    }
    {
        const uint16_t s = core::resolveGeoset(f300, all);
        if (s != 0) out.insert(s);
        else if (all.count(300)) out.insert(300);
        else if (all.count(301)) out.insert(301);
    }
    if (g_npcPlayerRules) {
        // The NPC's equipment groups by the outfit geoset rules (outfit_geosets.md 3.3,
        // clean-room implementation npc_outfit.hpp): CreatureDisplayInfoExtra's items, kinds 0..10 in its column
        // order, dressed as a player's outfit; no cape texture found (upstream hasRenderableCape) = no cape. Then
        // only the ids the model carries are kept (no substitute from the group).
        namespace body = rendering::psp::body;
        body::Outfit o;
        buildNpcOutfit(e.equip, Tables::ItemDisplayRows(db.items), cape, o);
        applyNpcOutfitGeosets(out, o);
        for (auto it = out.begin(); it != out.end();) it = all.count(*it) ? std::next(it) : out.erase(it);
        if (e.race == 4) add(core::kGeosetEyeGlow);
        if (has[15] && !cape && all.count(core::kGeosetNoCape)) out.insert(core::kGeosetNoCape);
        return out;
    }
    if (e.race == 4) add(core::kGeosetEyeGlow);
    if (has[13]) {
        uint16_t pants = legsGG ? core::equippedGeoset(core::equipment::kLegsBare, legsGG) : core::kGeosetBarePants;
        if (robeGG && !legsGG) pants = core::equippedGeoset(core::equipment::kRobeKiltBare, robeGG);  // NPC clothing rules
        add(pants);
    }
    if (has[18] && beltGG) add(core::equippedGeoset(core::equipment::kBeltBase, beltGG));  // NPC clothing rules
    if (has[15]) {
        if (cape) add(core::cloakGeoset(capeGG));
        else if (all.count(core::kGeosetNoCape)) out.insert(core::kGeosetNoCape);
    }
    return out;
}

// What a display draws, planned before the models are baked (the textures get their ids here).
struct DisplayPlan {
    uint32_t id = 0;
    int model = -1;            // ModelPlan index
    float scale = 1.0f;
    uint16_t flags = 0;
    uint16_t slot[fmt::kTexSlotCount];
    const ExtraRow* extra = nullptr;
    bool cape = false;         // hasRenderableCape
};

class DisplayPlanner {
public:
    DisplayPlanner(std::string root, const Tables& db, TexturePlan& tex) : root_(std::move(root)), db_(db), tex_(tex) {}

    // False: not drawn (no model path, a helper model, an unreadable model).
    bool plan(uint32_t displayId, DisplayPlan& dp) {
        dp = DisplayPlan{};
        dp.id = displayId;
        for (auto& s : dp.slot) s = fmt::kNoTexture;
        const std::string path = db_.pathFor(displayId);
        if (path.empty()) {
            ++noPath;
            if (noPath <= 20) std::fprintf(stderr, "d_%u: no model path\n", displayId);
            return false;
        }
        if (rendering::isHelperCreatureModel(backslashLower(path))) {
            ++helpers;
            if (helpers <= 20) std::printf("d_%u: helper model %s, not drawn\n", displayId, path.c_str());
            return false;
        }
        dp.model = modelIndex(path);
        if (dp.model < 0) return false;
        const ModelPlan& mp = models[static_cast<size_t>(dp.model)];
        const DisplayRow* d = nullptr;
        if (auto it = db_.displays.find(displayId); it != db_.displays.end()) {
            d = &it->second;
            dp.scale = d->scale;
            if (auto ms = db_.modelScale.find(d->modelId); ms != db_.modelScale.end()) dp.scale *= ms->second;
        }
        const ExtraRow* e = nullptr;
        if (d && d->extraId) {
            auto it = db_.extras.find(d->extraId);
            if (it != db_.extras.end()) e = &it->second;
            else std::fprintf(stderr, "d_%u: extra %u missing\n", displayId, d->extraId);
        }
        if (e) {
            dp.extra = e;
            dp.flags |= fmt::kCreatureHumanoid;
            ++humanoids;
            pipeline::CharacterAppearance who;
            who.raceId = e->race;
            who.sexId = e->sex;
            who.skinId = e->skin;
            who.faceId = e->face;
            who.hairStyleId = e->hairStyle;
            who.hairColorId = e->hairColor;
            std::string rootCopy = root_;
            const auto sec = pipeline::resolveCharacterSections(&db_.sections, db_.sectionFields, who, fileExistsCb,
                                                                &rootCopy);
            int skin = -1;
            if (!e->bake.empty() && mp.m2Vertices <= kHdCharacterVertexCeiling) {
                // The bake fills slots 1/11/12/13, and 6 on a bald NPC (below) - opaque when
                // every batch drawing them is.
                const uint32_t scalp = tex_.has(sec.hair) ? 1u : 6u;
                skin = tex_.request("Textures\\BakedNpcTextures\\" + e->bake,
                                    slotOpaque(mp, {1u, 11u, 12u, 13u, scalp}));
                if (skin >= 0) {
                    dp.flags |= fmt::kCreatureBaked;
                    ++baked;
                }
            }
            if (skin < 0) {
                char key[64];
                std::snprintf(key, sizeof(key), "composite:%u_%u_%u_%u", e->race, e->sex, e->skin, e->face);
                skin = tex_.requestComposite(key, sec);
                if (skin >= 0) ++composites;
            }
            if (skin >= 0)
                for (uint32_t t : {1u, 11u, 12u, 13u}) dp.slot[t] = static_cast<uint16_t>(skin);
            int hair = tex_.request(sec.hair, slotOpaque(mp, {6u}));  // opaque variant
            if (hair < 0 && (dp.flags & fmt::kCreatureBaked)) hair = skin;  // bald NPC: the bake on the scalp
            if (hair >= 0) dp.slot[6] = static_cast<uint16_t>(hair);
            const int extra = tex_.request(sec.skinExtra.empty() ? sec.bodySkin : sec.skinExtra,
                                           slotOpaque(mp, {8u}));  // opaque variant
            if (extra >= 0) dp.slot[8] = static_cast<uint16_t>(extra);
            // The cape (upstream hasRenderableCape + setGroupTextureOverride(15)).
            const std::string capePath = db_.capeTexture(root_, e->equip[10], e->sex == 1);
            if (!capePath.empty()) {
                const int cape = tex_.request(capePath, slotOpaque(mp, {2u}));  // opaque variant
                if (cape >= 0) {
                    dp.slot[2] = static_cast<uint16_t>(cape);
                    dp.cape = true;
                    ++capes;
                }
            }
            for (uint32_t s : {0u, 1u}) equipped += e->equip[s] ? 1 : 0;
        } else {
            const std::string dir = path.substr(0, path.find_last_of("\\/") + 1);
            for (int s = 0; d && s < 3; ++s) {
                if (d->skin[s].empty()) continue;
                const std::string p = creatureSkinPath(root_, d->skin[s], dir);
                const int id = tex_.request(p, slotOpaque(mp, {static_cast<uint32_t>(11 + s)}));  // opaque variant
                if (id >= 0) dp.slot[11 + s] = static_cast<uint16_t>(id);
                else if (++skinMisses <= 40)
                    std::fprintf(stderr, "d_%u: skin %d '%s' not found\n", displayId, s + 1, d->skin[s].c_str());
            }
        }
        return true;
    }

    std::vector<ModelPlan> models;  // in first-use order; id = index among the readable ones
    size_t noPath = 0, helpers = 0, humanoids = 0, baked = 0, composites = 0, capes = 0, equipped = 0;
    size_t skinMisses = 0;

    int modelIndex(const std::string& path) {
        const std::string key = lower(path);
        auto it = modelByKey_.find(key);
        if (it != modelByKey_.end()) return it->second;
        ModelPlan mp;
        mp.path = path;
        mp.key = key;
        int idx = -1;
        if (planModel(root_, mp, tex_)) {
            mp.id = nextModelId_++;
            idx = static_cast<int>(models.size());
            models.push_back(std::move(mp));
        }
        modelByKey_.emplace(key, idx);
        return idx;
    }

private:
    std::string root_;
    const Tables& db_;
    TexturePlan& tex_;
    std::unordered_map<std::string, int> modelByKey_;
    int nextModelId_ = 0;
};

// Without a geoset filter upstream draws every batch but the cloak, the
// eye glow and group 18.
void applyDefaultMask(const ModelPlan& mp, std::vector<uint8_t>& active) {
    for (size_t i = 0; i < active.size(); ++i) {
        const uint16_t g = static_cast<uint16_t>(mp.batchGeoset[i] / 100);
        active[i] = (g == 15 || g == 17 || g == 18) ? 0 : 1;
    }
}

// The WCDI blob of a planned display whose model is baked.
std::vector<uint8_t> displayBlob(const Tables& db, const DisplayPlan& dp, const ModelPlan& mp) {
    fmt::CreatureDisplayHeader h{};
    std::memcpy(h.magic, "WCDI", 4);
    h.version = fmt::kModelFormatVersion;
    h.model = static_cast<uint16_t>(mp.id);
    h.flags = dp.flags;
    h.batchCount = static_cast<uint16_t>(mp.batchGeoset.size());
    h.scale = dp.scale;
    std::memcpy(h.slotTexture, dp.slot, sizeof(h.slotTexture));
    std::vector<uint8_t> active(mp.batchGeoset.size(), 1);
    if (dp.extra) {
        bool applies = false;
        const auto geosets = humanoidGeosets(db, *dp.extra, mp.allGeosets, dp.cape, applies);
        bool any = false;
        if (applies && !geosets.empty()) {
            for (size_t i = 0; i < active.size(); ++i) {
                active[i] = geosets.count(mp.batchGeoset[i]) ? 1 : 0;
                any = any || active[i];
            }
        }
        if (!any) applyDefaultMask(mp, active);
        // The cape batches take the cape texture (group 15 and the type-2 slots).
        if (dp.cape)
            for (size_t i = 0; i < active.size(); ++i)
                if (active[i] && (mp.batchGeoset[i] / 100 == 15 || mp.batchTex[i] == fmt::kTexSlot + 2))
                    active[i] = fmt::kBatchCape;
        bool equip = false;
        for (uint32_t it : dp.extra->equip) equip = equip || it != 0;
        if (equip) h.flags = static_cast<uint16_t>(h.flags | fmt::kCreatureEquip);
    } else {
        applyDefaultMask(mp, active);
    }
    std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
    blob.insert(blob.end(), active.begin(), active.end());
    if (h.flags & fmt::kCreatureEquip) {  // CreatureDisplayEquip
        alignTo(blob, 4);
        fmt::CreatureDisplayEquip q{};
        std::memcpy(q.magic, "WCDE", 4);
        q.race = dp.extra->race;
        q.sex = dp.extra->sex;
        for (int s = 0; s < 11; ++s) q.item[s] = dp.extra->equip[s];
        put(blob, q);
    }
    return blob;
}

// ---- spawns (our own server only: tools/psp/creature_spawns.py, a debug fallback) ----

struct SpawnRow {
    fmt::CreatureSpawn s{};
    uint32_t flags = 0;
    std::string name;
    uint32_t emote = 0;  // creature anim - UNIT_NPC_EMOTESTATE (creature_addon / template_addon)
};

bool readSpawns(const std::string& path, std::vector<SpawnRow>& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> c;
        size_t a = 0;
        for (;;) {
            const size_t b = line.find('\t', a);
            c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (c.size() < 9) continue;
        SpawnRow r;
        r.s.guid = static_cast<uint32_t>(std::stoul(c[0]));
        r.s.entry = static_cast<uint32_t>(std::stoul(c[1]));
        r.s.displayId = static_cast<uint32_t>(std::stoul(c[2]));
        r.s.scale = std::stof(c[3]);
        r.s.position[0] = std::stof(c[4]);
        r.s.position[1] = std::stof(c[5]);
        r.s.position[2] = std::stof(c[6]);
        r.s.orientation = std::stof(c[7]);
        r.flags = static_cast<uint32_t>(std::stoul(c[8]));
        r.name = c.size() > 9 ? c[9] : "";
        // Creature anim - column 11 (older TSVs have none).
        r.emote = c.size() > 10 && !c[10].empty() ? static_cast<uint32_t>(std::stoul(c[10])) : 0u;
        out.push_back(r);
    }
    return true;
}

// ---- --verify: the written pak read back the way the PSP reads it (psp_pak_index.hpp pakIndexLookup) ----

class PakReadBack {
public:
    bool open(const std::string& path) {
        f_ = std::fopen(path.c_str(), "rb");
        if (!f_) return false;
        uint32_t head[4];
        if (std::fread(head, 4, 4, f_) != 4 || std::memcmp(head, "WPAK", 4) != 0 || head[1] != 1 || !head[2]) return false;
        records_ = head[2];
        tocOffset_ = head[3];
        platform::psp::PakDiskEntry last{};
        if (!read(tocOffset_ + (records_ - 1) * 64ull, &last, sizeof(last))) return false;
        last.name[47] = '\0';
        if (std::strcmp(last.name, platform::psp::kPakFenceName) != 0) return false;
        fence_.resize(last.size);
        if (!read(last.offset, fence_.data(), last.size) || fence_.size() < sizeof(platform::psp::PakFenceHeader))
            return false;
        std::memcpy(&fh_, fence_.data(), sizeof(fh_));
        fenceBytes_ = last.size;
        return std::memcmp(fh_.magic, "WFNC", 4) == 0 && fh_.records == records_ && fh_.step >= 1;
    }
    ~PakReadBack() {
        if (f_) std::fclose(f_);
    }
    bool find(const std::string& name, platform::psp::PakDiskEntry& out) {
        std::vector<platform::psp::PakDiskEntry> scratch(fh_.step);
        ++lookups_;
        return platform::psp::pakIndexLookup(
            name.c_str(), records_, fh_.step, fh_.fences,
            [this](uint32_t i) {
                uint32_t o;
                std::memcpy(&o, fence_.data() + sizeof(platform::psp::PakFenceHeader) + i * 4u, 4);
                return reinterpret_cast<const char*>(fence_.data() + o);
            },
            [this](uint32_t first, uint32_t n, platform::psp::PakDiskEntry* dst) {
                ++blockReads_;
                return read(tocOffset_ + first * 64ull, dst, n * 64u);
            },
            scratch.data(), out);
    }
    bool load(const platform::psp::PakDiskEntry& e, std::vector<uint8_t>& out) {
        out.resize(e.size);
        return read(e.offset, out.data(), e.size);
    }
    uint32_t records() const { return records_; }
    uint32_t step() const { return fh_.step; }
    uint32_t fenceBytes() const { return fenceBytes_; }
    uint64_t lookups_ = 0, blockReads_ = 0;

private:
    bool read(uint64_t off, void* dst, size_t n) {
#if defined(_WIN32)
        if (_fseeki64(f_, static_cast<long long>(off), SEEK_SET) != 0) return false;
#else
        if (fseeko(f_, static_cast<off_t>(off), SEEK_SET) != 0) return false;
#endif
        return n == 0 || std::fread(dst, 1, n, f_) == n;
    }
    std::FILE* f_ = nullptr;
    uint32_t records_ = 0, tocOffset_ = 0, fenceBytes_ = 0;
    platform::psp::PakFenceHeader fh_{};
    std::vector<uint8_t> fence_;
};

// Every display record of the pak: it parses like CreatureRenderer::loadDisplay, its model ("n_" or "m_")
// and every texture its shown batches use exist. Returns the errors.
size_t verifyPak(const std::string& path, const std::vector<DisplayPlan>& displays,
                 const std::vector<ModelPlan>& models, size_t& checked) {
    PakReadBack pak;
    checked = 0;
    if (!pak.open(path)) {
        std::fprintf(stderr, "verify pak: %s does not open as a sorted pak\n", path.c_str());
        return 1;
    }
    size_t errors = 0;
    std::unordered_map<uint32_t, bool> texOk;
    auto err = [&](const char* fmt, uint32_t a, uint32_t b) {
        if (++errors <= 30) {
            std::fprintf(stderr, "verify pak: ");
            std::fprintf(stderr, fmt, a, b);
            std::fprintf(stderr, "\n");
        }
    };
    for (const DisplayPlan& dp : displays) {
        if (dp.model < 0 || !models[static_cast<size_t>(dp.model)].baked) continue;
        platform::psp::PakDiskEntry e{};
        std::vector<uint8_t> blob;
        if (!pak.find("d_" + std::to_string(dp.id), e) || !pak.load(e, blob)) {
            err("d_%u missing (%u)", dp.id, 0);
            continue;
        }
        ++checked;
        fmt::CreatureDisplayHeader h{};
        if (blob.size() < sizeof(h)) {
            err("d_%u short (%u bytes)", dp.id, static_cast<uint32_t>(blob.size()));
            continue;
        }
        std::memcpy(&h, blob.data(), sizeof(h));
        if (std::memcmp(h.magic, "WCDI", 4) != 0 || h.version != fmt::kModelFormatVersion ||
            sizeof(h) + h.batchCount > blob.size()) {
            err("d_%u bad header (batches %u)", dp.id, h.batchCount);
            continue;
        }
        if ((h.flags & fmt::kCreatureEquip) && !fmt::creatureDisplayEquip(blob.data(), static_cast<uint32_t>(blob.size())))
            err("d_%u equipment block unreadable (%u)", dp.id, 0);
        const ModelPlan& mp = models[static_cast<size_t>(dp.model)];
        platform::psp::PakDiskEntry me{};
        const bool haveN = pak.find("n_" + std::to_string(h.model), me);
        const bool haveM = pak.find("m_" + std::to_string(h.model), me);
        if (!haveM || (g_animate && mp.nBytes && !haveN)) err("d_%u: model %u record missing", dp.id, h.model);
        if (h.batchCount != mp.batchGeoset.size()) err("d_%u: %u batches for its model", dp.id, h.batchCount);
        for (size_t i = 0; i < h.batchCount && i < mp.batchTex.size(); ++i) {
            const uint8_t a = blob[sizeof(h) + i];
            if (!a) continue;
            uint16_t t = mp.batchTex[i];
            if (a == fmt::kBatchCape && h.slotTexture[2] != fmt::kNoTexture) t = h.slotTexture[2];
            if (t >= fmt::kTexSlot && t != fmt::kNoTexture) t = h.slotTexture[t - fmt::kTexSlot];
            if (t == fmt::kNoTexture) continue;
            auto it = texOk.find(t);
            if (it == texOk.end()) {
                platform::psp::PakDiskEntry te{};
                it = texOk.emplace(t, pak.find("x_" + std::to_string(t), te)).first;
            }
            if (!it->second) err("d_%u: texture x_%u missing (white on the PSP)", dp.id, t);
        }
    }
    std::printf("verify pak: %zu display records read back through the PSP lookup (fence step %u, %u records, "
                "%llu lookups, %.2f TOC block reads each), %zu errors\n",
                checked, pak.step(), pak.records(), static_cast<unsigned long long>(pak.lookups_),
                pak.lookups_ ? static_cast<double>(pak.blockReads_) / static_cast<double>(pak.lookups_) : 0.0, errors);
    return errors;
}


// --check-humanoids - the humanoid NPCs' clothing as baked, without baking: per
// CreatureDisplayInfoExtra display, the geosets of this converter's rules (g_npcPlayerRules) against upstream's NPC
// rules (--npc-upstream-geosets) group by group, the cape found, the helm whose HelmetGeosetVisData hides the hair
// (the PSP hides it while the helm model is on, CreatureRenderer), the pieces the PSP hangs as item models (helm,
// shoulders: items.pak at run time). Counts first, then the displays asked for (--display) or the first of each kind.
int checkHumanoidClothing(const std::string& root, const Tables& db, const DisplayPlanner& planner,
                          const std::vector<DisplayPlan>& plans, bool listAll) {
    pipeline::DBCFile vis;
    const bool haveVis = loadDbc(root, "helmetgeosetvisdata.dbc", vis);
    auto hidesHair = [&](uint32_t helm, uint8_t sex) {  // psp_item_conv's rule: any non-zero mask of the sex's row
        if (!helm || !haveVis) return false;
        const int32_t r = db.items.findRecordById(helm);
        if (r < 0) return false;
        const uint32_t visId = db.items.getUInt32(static_cast<uint32_t>(r), sex ? 14 : 13);
        const int32_t v = visId ? vis.findRecordById(visId) : -1;
        if (v < 0) return false;
        for (uint32_t f = 1; f < vis.getFieldCount(); ++f)
            if (vis.getUInt32(static_cast<uint32_t>(v), f)) return true;
        return false;
    };
    static const char* kSlots[11] = {"helm", "shoulder", "shirt", "chest", "belt", "legs", "boots", "wrist",
                                     "gloves", "tabard", "cape"};
    size_t humanoids = 0, slotUse[11] = {}, capes = 0, helmHair = 0, changed = 0;
    std::map<uint16_t, size_t> groupChanged;               // group -> displays whose group changed
    std::map<std::string, std::vector<uint32_t>> samples;  // kind -> first display ids
    auto note = [&](const std::string& kind, uint32_t id) {
        auto& v = samples[kind];
        if (v.size() < 8) v.push_back(id);
    };
    auto groupsOf = [](const std::unordered_set<uint16_t>& s, uint16_t g) {
        std::vector<uint16_t> out;
        for (uint16_t id : s)
            if (id / 100 == g && (g != 0 || id < 100)) out.push_back(id);
        std::sort(out.begin(), out.end());
        return out;
    };
    auto join = [](const std::vector<uint16_t>& v) {
        std::string s;
        for (uint16_t id : v) s += (s.empty() ? "" : ",") + std::to_string(id);
        return s.empty() ? std::string("-") : s;
    };
    const bool saved = g_npcPlayerRules;
    for (const DisplayPlan& dp : plans) {
        if (!dp.extra || dp.model < 0) continue;
        const ModelPlan& mp = planner.models[static_cast<size_t>(dp.model)];
        const ExtraRow& e = *dp.extra;
        ++humanoids;
        for (int s = 0; s < 11; ++s) slotUse[s] += e.equip[s] ? 1 : 0;
        capes += dp.cape ? 1 : 0;
        const bool hair = hidesHair(e.equip[0], e.sex);
        helmHair += hair ? 1 : 0;
        if (hair) note("helm hides the hair (PSP: the scalp swapped while the helm model is on)", dp.id);
        bool applies = false;
        g_npcPlayerRules = false;
        const auto before = humanoidGeosets(db, e, mp.allGeosets, dp.cape, applies);
        g_npcPlayerRules = true;
        const auto after = humanoidGeosets(db, e, mp.allGeosets, dp.cape, applies);
        bool any = false;
        std::string diff;
        for (uint16_t g = 0; g < 21; ++g) {
            const auto a = groupsOf(before, g), b = groupsOf(after, g);
            if (a == b) continue;
            any = true;
            ++groupChanged[g];
            diff += " g" + std::to_string(g) + " " + join(a) + "->" + join(b);
            note("group " + std::to_string(g) + " changed", dp.id);
        }
        changed += any ? 1 : 0;
        if (listAll) {
            std::string items;
            for (int s = 0; s < 11; ++s)
                if (e.equip[s]) items += std::string(" ") + kSlots[s] + "=" + std::to_string(e.equip[s]);
            std::printf("d_%u race %u sex %u model %s:%s%s%s%s\n", dp.id, e.race, e.sex, mp.path.c_str(), items.c_str(),
                        dp.cape ? " [cape found]" : (e.equip[10] ? " [cape texture missing]" : ""),
                        hair ? " [helm hides hair]" : "", diff.empty() ? " geosets as upstream" : diff.c_str());
        }
    }
    g_npcPlayerRules = saved;
    std::printf("humanoid displays %zu: worn ", humanoids);
    for (int s = 0; s < 11; ++s) std::printf("%s %zu%s", kSlots[s], slotUse[s], s < 10 ? ", " : "\n");
    std::printf("capes found %zu of %zu; helms hiding the hair %zu; displays whose geosets differ from upstream's NPC "
                "rules %zu:", capes, slotUse[10], helmHair, changed);
    for (const auto& [g, n] : groupChanged) std::printf(" group %u: %zu", static_cast<unsigned>(g), n);
    std::printf("\n");
    for (const auto& [kind, ids] : samples) {
        std::printf("  %s:", kind.c_str());
        for (uint32_t id : ids) std::printf(" %u", id);
        std::printf("\n");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string root, out, spawnsPath, displayListPath;
    int texMax = 128;
    uint32_t mapId = 0;  // the map the spawn TSV was dumped for
    bool listAssets = false;
    bool checkHumanoids = false;  // NPC clothing check
    std::vector<uint32_t> displayArgs;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency() / 2);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) root = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--spawns" && i + 1 < argc) spawnsPath = argv[++i];
        else if (a == "--tex-max" && i + 1 < argc) texMax = std::atoi(argv[++i]);
        else if (a == "--display" && i + 1 < argc) {
            for (uint32_t id : parseIdList(argv[++i])) displayArgs.push_back(id);
        } else if (a == "--display-list" && i + 1 < argc) displayListPath = argv[++i];
        else if (a == "--mounts" && i + 1 < argc) {  // obsolete: every display is baked now
            ++i;
            std::fprintf(stderr, "--mounts is ignored: every display and its seat table are baked from the DBCs\n");
        } else if (a == "--jobs" && i + 1 < argc) jobs = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
        else if (a == "--list-assets") listAssets = true;
        else if (a == "--map" && i + 1 < argc) mapId = static_cast<uint32_t>(std::atoi(argv[++i]));
        // Creature anim.
        else if (a == "--no-anim") g_animate = false;
        else if (a == "--anims" && i + 1 < argc) {g_anims = parseIdList(argv[++i]);g_spellAnimations=false;}
        else if (a == "--npc-emotes" && i + 1 < argc) g_npcEmotes = parseIdList(argv[++i]);
        else if (a == "--no-state-emotes") g_useStateEmotes = false;
        else if (a == "--rate" && i + 1 < argc) g_rate = std::max(1.0f, static_cast<float>(std::atof(argv[++i])));
        else if (a == "--palette" && i + 1 < argc) g_maxPalette = std::clamp(std::atoi(argv[++i]), 4, 8);
        else if (a == "--max-anim-kb" && i + 1 < argc) g_maxAnimKb = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--all-variations") g_allVariations = true;
        else if (a == "--verify") g_verify = true;
        else if (a == "--verify-limit" && i + 1 < argc) g_verifyLimit = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--dump" && i + 1 < argc) g_dumpDir = argv[++i];
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--check-humanoids") checkHumanoids = true;            // NPC clothing check
        else if (a == "--npc-upstream-geosets") g_npcPlayerRules = false;    // NPC clothing rules
        else if (a == "--gameobjects") g_gameObjects = true;                  // animated game objects
        else if (a == "--no-gameobjects") g_noGameObjects = true;
        else if (a == "--go-display" && i + 1 < argc) {
            for (uint32_t id : parseIdList(argv[++i])) g_goDisplays.push_back(id);
            g_gameObjects = true;
        }
    }
    if (!displayListPath.empty()) {  // a test set, one id per line ('#' comments)
        std::ifstream lf(displayListPath);
        if (!lf) {
            std::fprintf(stderr, "cannot read %s\n", displayListPath.c_str());
            return 1;
        }
        std::string line;
        while (std::getline(lf, line)) {
            if (line.empty() || line[0] == '#') continue;
            const unsigned long id = std::strtoul(line.c_str(), nullptr, 10);
            if (id) displayArgs.push_back(static_cast<uint32_t>(id));
        }
    }
    if (root.empty() || (out.empty() && !listAssets && !checkHumanoids) || texMax < 16) {
        std::fprintf(stderr,
                     "usage: psp_creature_conv --extracted <dir> --out <pak> [--display N[,N]]... [--display-list <file>]\n"
                     "       [--jobs N] [--spawns <tsv> [--map 0]] [--tex-max 128] [--list-assets]\n"
                     "       [--anims 0,4,5,1,... | --no-anim] [--npc-emotes 60,64,65,66] [--no-state-emotes] [--rate 30]\n"
                     "       [--palette 8] [--max-anim-kb 384] [--all-variations] [--verify [--verify-limit 0.02]]\n"
                     "       [--dump <dir>] [--check-humanoids [--display N,...]] [--npc-upstream-geosets]\n"
                     "  Without --display / --display-list / --spawns: every CreatureDisplayInfo row.\n");
        return 2;
    }
    Tables db;
    if (!db.load(root)) {
        std::fprintf(stderr, "missing DBC under %s/dbfilesclient\n", root.c_str());
        return 1;
    }
    if(g_animate && g_spellAnimations) {
        pipeline::DBCFile spellKits;
        if(loadDbc(root,"spellvisualkit.dbc",spellKits) && spellKits.getFieldCount()>2)
            for(uint32_t row=0;row<spellKits.getRecordCount();++row)for(uint32_t field:{1u,2u}) {
                const uint32_t id=spellKits.getUInt32(row,field);
                if(id && id<65535 && std::find(g_anims.begin(),g_anims.end(),id)==g_anims.end())g_anims.push_back(id);
            }
    }
    std::vector<SpawnRow> spawns;
    if (!spawnsPath.empty() && !readSpawns(spawnsPath, spawns)) {
        std::fprintf(stderr, "cannot read %s\n", spawnsPath.c_str());
        return 1;
    }
    // The displays - every row, or the ones asked for (plus the spawns' when given).
    // Triggers are invisible to players; a spawn outside phase 1 is not sent.
    std::set<uint32_t> displayIds(displayArgs.begin(), displayArgs.end());
    for (const auto& r : spawns)
        if (!(r.flags & 1)) displayIds.insert(r.s.displayId);
    const bool all = displayArgs.empty() && spawns.empty();
    if (all)
        for (const auto& [id, d] : db.displays) displayIds.insert(id);
    // Emotes.dbc (upstream EmoteRegistry: ID 0, AnimID 2, EmoteSpecProc 4): the state emotes' animations,
    // and (--spawns) the spawns' emote states, what upstream getEmoteAnimByEmotesId resolves.
    std::map<uint32_t, uint32_t> emoteAnim;
    {
        pipeline::DBCFile emotesDbc;
        if (loadDbc(root, "emotes.dbc", emotesDbc)) {
            for (uint32_t r = 0; r < emotesDbc.getRecordCount(); ++r) {
                const uint32_t anim = emotesDbc.getUInt32(r, 2);
                emoteAnim[emotesDbc.getUInt32(r, 0)] = anim;
                if (anim && emotesDbc.getUInt32(r, 4) != 0 &&
                    std::find(g_stateEmotes.begin(), g_stateEmotes.end(), anim) == g_stateEmotes.end() &&
                    std::find(g_anims.begin(), g_anims.end(), anim) == g_anims.end() &&
                    std::find(g_npcEmotes.begin(), g_npcEmotes.end(), anim) == g_npcEmotes.end())
                    g_stateEmotes.push_back(anim);
            }
        }
    }
    std::printf("displays: %zu (%s), state emote anims %zu%s, jobs %u\n", displayIds.size(),
                all ? "every CreatureDisplayInfo row" : "asked for", g_stateEmotes.size(),
                g_useStateEmotes ? "" : " (off)", jobs);
    std::fflush(stdout);

    if (listAssets) {
        // What psp_mpq_extract --prefix needs: model folders (models, skins,
        // .anim files) and the NPC bakes, for everything not extracted yet.
        std::set<std::string> want;
        std::set<std::string> seen;
        for (uint32_t id : displayIds) {
            const std::string path = db.pathFor(id);
            if (path.empty() || rendering::isHelperCreatureModel(backslashLower(path))) continue;
            if (seen.insert(lower(path)).second &&
                (!fileExists(root, path) || !fileExists(root, pipeline::skinPathForM2(path)) ||
                 (g_animate && missingAnims(root, path))))  // creature anim (.anim files)
                want.insert(backslashLower(path.substr(0, path.find_last_of("\\/") + 1)));
            auto d = db.displays.find(id);
            if (d == db.displays.end() || !d->second.extraId) continue;
            auto e = db.extras.find(d->second.extraId);
            if (e != db.extras.end() && !e->second.bake.empty()) {
                const std::string bake = "Textures\\BakedNpcTextures\\" + e->second.bake;
                if (!fileExists(root, bake)) want.insert(backslashLower(bake));
            }
        }
        for (const auto& w : want) std::printf("%s\n", w.c_str());
        std::fprintf(stderr, "%zu displays, %zu paths to extract\n", displayIds.size(), want.size());
        return 0;
    }

    // ---- 1. plan (serial, display order): models read, ids for models and textures ----
    TexturePlan tex(root);
    DisplayPlanner planner(root, db, tex);
    std::vector<DisplayPlan> plans;
    plans.reserve(displayIds.size());
    for (uint32_t id : displayIds) {
        DisplayPlan dp;
        if (planner.plan(id, dp)) plans.push_back(dp);
    }
    // Creature anim - the spawns' emote states join their model's n_ (--spawns only).
    size_t withEmote = 0;
    for (const auto& r : spawns) {
        if (!r.emote || (r.flags & 1)) continue;
        auto it = emoteAnim.find(r.emote);
        const std::string path = db.pathFor(r.s.displayId);
        if (it == emoteAnim.end() || !it->second || path.empty()) continue;
        const int m = planner.modelIndex(path);
        if (m < 0) continue;
        planner.models[static_cast<size_t>(m)].extraAnims.insert(it->second);
        ++withEmote;
    }
    size_t rideableModels = 0;
    for (const auto& m : planner.models) rideableModels += m.rideable ? 1 : 0;
    std::printf("plan: %zu displays drawn (%zu no model path, %zu helpers), %zu models (%zu with a seat), "
                "%zu textures (%zu opaque variants, %zu missing files); humanoids %zu: %zu bakes, %zu composites, "
                "%zu capes, %zu helm/shoulder pieces; %.1f s\n",
                plans.size(), planner.noPath, planner.helpers, planner.models.size(), rideableModels,
                tex.jobs().size(), tex.opaqueCount(), tex.missing(), planner.humanoids, planner.baked,
                planner.composites, planner.capes, planner.equipped, secondsSince(t0));
    {
        // Files one model draws both opaque and with alpha keep the plain texture.
        size_t both = 0, bothModels = 0;
        for (const auto& m : planner.models) {
            both += m.bothWays;
            bothModels += m.bothWays ? 1 : 0;
        }
        std::printf("opaque: %zu model files drawn both ways by one model (%zu models) keep the plain texture\n",
                    both, bothModels);
    }
    if (g_animate)
        std::printf("anim: %zu base ids, %zu NPC emotes, %zu state emotes, rate %.0f Hz, cap %u KB/model; "
                    "%zu spawns with an emote state\n",
                    g_anims.size(), g_npcEmotes.size(), g_useStateEmotes ? g_stateEmotes.size() : 0, g_rate,
                    g_maxAnimKb, withEmote);
    std::fflush(stdout);
    // The game objects whose M2 has object-state animations (AnimationData.dbc
    // 145 Spawn .. 157 Despawn) - selection by gameobject_anim_select.md (clean-room implementation,
    // gameobject_anim_select.hpp): "d_<kGoAnimDisplayBase + GameObjectDisplayInfo id>" (the model's n_ gets those
    // sequences as extra animations; no slots, scale 1: the object's scale is the unit's) and "goanim" (the ids).
    // The PSP draws them as units of the creature renderer (ui_world_gouse.cpp).
    static_assert(goanim::kGoAnimDisplayBase == fmt::kGoAnimDisplayBase, "one display id base for the PSP's reader");
    std::vector<uint32_t> goAnimIds;
    goanim::GoAnimOptions goOptions;
    goOptions.noGameObjects = g_noGameObjects;
    goOptions.gameObjects = g_gameObjects;
    goOptions.allCreatureDisplays = all;
    goOptions.humanoidCheck = checkHumanoids;
    goOptions.hasDisplayFilter = !g_goDisplays.empty();
    goOptions.displayFilter.assign(g_goDisplays.begin(), g_goDisplays.end());
    if (goanim::goAnimPassRuns(goOptions)) {
        pipeline::DBCFile godi;
        if (!loadDbc(root, "gameobjectdisplayinfo.dbc", godi)) {
            std::fprintf(stderr, "gameobjects: no dbfilesclient/gameobjectdisplayinfo.dbc\n");
        } else {
            std::vector<goanim::GoDisplayRow> rows;
            rows.reserve(godi.getRecordCount());
            for (uint32_t r = 0; r < godi.getRecordCount(); ++r) {
                goanim::GoDisplayRow row;
                row.id = godi.getUInt32(r, 0);
                row.modelName = godi.getString(r, 1);
                rows.push_back(std::move(row));
            }
            // The models read from --extracted (lower-case path), their sequence ids by the M2 loader.
            struct ExtractedModels : goanim::GoModelSource {
                const std::string& root;
                explicit ExtractedModels(const std::string& r) : root(r) {}
                goanim::ModelReadResult readSequenceIds(const std::string& path, std::vector<uint16_t>& ids) override {
                    std::vector<uint8_t> m2;
                    if (!readFile(root + "/" + lower(path), m2)) return goanim::ModelReadResult::kMissing;
                    if (m2.size() < 4 || std::memcmp(m2.data(), "MD20", 4) != 0) return goanim::ModelReadResult::kNotM2;
                    const pipeline::M2Model m = pipeline::M2Loader::load(m2);
                    ids.clear();
                    for (const auto& q : m.sequences) ids.push_back(static_cast<uint16_t>(q.id));
                    return goanim::ModelReadResult::kOk;
                }
            } source(root);
            const goanim::GoAnimSelection sel = goanim::selectGameObjectAnims(rows, goOptions, source);
            for (const goanim::GoAnimSelected& g : sel.selected) {
                const int mi = planner.modelIndex(g.modelPath);
                if (mi < 0) continue;
                ModelPlan& mp = planner.models[static_cast<size_t>(mi)];
                for (uint16_t a : g.stateAnims) mp.extraAnims.insert(a);
                DisplayPlan dp;
                dp.id = goanim::goAnimDisplayId(g.displayId);
                dp.model = mi;
                for (auto& sl : dp.slot) sl = fmt::kNoTexture;
                plans.push_back(dp);
                goAnimIds.push_back(g.displayId);
            }
            std::printf("gameobjects: %zu animated displays (%zu M2 read, %zu unreadable)\n", goAnimIds.size(),
                        sel.modelsRead, sel.modelsUnreadable);
        }
    }
    if (checkHumanoids) return checkHumanoidClothing(root, db, planner, plans, !displayArgs.empty());  // NPC clothing check
    if (tex.jobs().size() >= fmt::kTexSlot || planner.models.size() >= 0xFFF0) {
        std::fprintf(stderr, "too many textures (%zu) or models (%zu) for the u16 ids\n", tex.jobs().size(),
                     planner.models.size());
        return 1;
    }
    if (plans.empty()) return 1;
    SortedPakWriter writer(out);
    if (!writer.ok()) {
        std::fprintf(stderr, "cannot write %s.tmp\n", out.c_str());
        return 1;
    }
    std::mutex writerMutex;
    std::mutex progressMutex;

    // ---- 2. textures (threads) ----
    const auto tTex = std::chrono::steady_clock::now();
    std::atomic<size_t> texDone{0}, texFailed{0};
    std::atomic<uint64_t> texBytes{0};
    {
        std::vector<TexJob>& tj = tex.jobs();
        parallelFor(tj.size(), jobs, {}, [&](size_t i) {
            std::vector<uint8_t> blob = bakeTexture(root, texMax, tj[i]);
            if (blob.empty()) {
                ++texFailed;
            } else {
                texBytes += blob.size();
                std::lock_guard<std::mutex> lock(writerMutex);
                writer.add("x_" + std::to_string(i), kKindTexture, blob.data(), blob.size());
            }
            const size_t n = ++texDone;
            if (n % 2000 == 0 || n == tj.size()) {
                std::lock_guard<std::mutex> lock(progressMutex);
                std::fprintf(stderr, "textures %zu/%zu, %.0f s\n", n, tj.size(), secondsSince(tTex));
            }
        });
    }
    std::printf("textures: %zu baked (%zu unreadable), %llu KB, %.1f s\n", texDone.load() - texFailed.load(),
                texFailed.load(), static_cast<unsigned long long>(texBytes.load() / 1024), secondsSince(tTex));
    std::fflush(stdout);

    // ---- 3. models (threads, biggest first) ----
    const auto tModels = std::chrono::steady_clock::now();
    std::vector<ModelPlan>& models = planner.models;
    std::vector<size_t> order(models.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return models[a].m2Bytes > models[b].m2Bytes; });
    std::atomic<size_t> modelsDone{0};
    ModelBaker baker(root, tex, writer, writerMutex);
    parallelFor(models.size(), jobs, order, [&](size_t i) {
        try {
            baker.bake(models[i]);
        } catch (const std::exception& ex) {
            appendf(models[i].log, "  model %s: exception %s\n", models[i].key.c_str(), ex.what());
            models[i].baked = false;
        }
        const size_t n = ++modelsDone;
        if (n % 50 == 0 || n == models.size()) {
            std::lock_guard<std::mutex> lock(progressMutex);
            std::fprintf(stderr, "models %zu/%zu, %.0f s\n", n, models.size(), secondsSince(tModels));
        }
    });
    size_t modelsBaked = 0, skins = 0, pointTables = 0;
    uint64_t mBytes = 0, nBytes = 0, animBytes = 0, pBytes = 0;
    for (const ModelPlan& m : models) {
        std::fputs(m.log.c_str(), stdout);
        if (!m.baked) continue;
        ++modelsBaked;
        skins += m.nBytes ? 1 : 0;
        pointTables += m.pBytes ? 1 : 0;
        mBytes += m.mBytes;
        nBytes += m.nBytes;
        animBytes += m.animBytes;
        pBytes += m.pBytes;
    }
    std::printf("models: %zu of %zu baked, m_ %llu KB, %zu n_ %llu KB (animation %llu KB), %zu p_ %llu KB, %.1f s\n",
                modelsBaked, models.size(), static_cast<unsigned long long>(mBytes / 1024), skins,
                static_cast<unsigned long long>(nBytes / 1024), static_cast<unsigned long long>(animBytes / 1024),
                pointTables, static_cast<unsigned long long>(pBytes / 1024), secondsSince(tModels));
    std::fflush(stdout);

    // ---- 4. displays (serial) ----
    size_t written = 0, dropped = 0, capeDisplays = 0, equipDisplays = 0;
    uint64_t dBytes = 0;
    for (const DisplayPlan& dp : plans) {
        const ModelPlan& mp = models[static_cast<size_t>(dp.model)];
        if (!mp.baked) {
            ++dropped;
            continue;
        }
        const std::vector<uint8_t> blob = displayBlob(db, dp, mp);
        const auto* h = reinterpret_cast<const fmt::CreatureDisplayHeader*>(blob.data());
        capeDisplays += dp.cape ? 1 : 0;
        equipDisplays += (h->flags & fmt::kCreatureEquip) ? 1 : 0;
        dBytes += blob.size();
        writer.add("d_" + std::to_string(dp.id), kKindDisplay, blob.data(), blob.size());
        ++written;
    }
    if (!goAnimIds.empty()) {  // the animated game object displays written
        std::vector<uint32_t> baked;
        for (const DisplayPlan& dp : plans)
            if (dp.id >= goanim::kGoAnimDisplayBase && models[static_cast<size_t>(dp.model)].baked)
                baked.push_back(dp.id - goanim::kGoAnimDisplayBase);
        const std::vector<uint8_t> blob = goanim::writeGoAnimRecord(baked);  // gameobject_rules.md 2.4
        writer.add("goanim", 3, blob.data(), blob.size());
        std::printf("goanim: %zu game object displays\n", baked.size());
    }
    std::printf("displays: %zu written (%zu dropped: model not baked), %llu KB, %zu with a cape, %zu with "
                "equipment\n",
                written, dropped, static_cast<unsigned long long>(dBytes / 1024), capeDisplays, equipDisplays);

    // Static spawn lists per ADT tile (our server only - the debug path from before live units).
    std::map<std::pair<int, int>, std::vector<fmt::CreatureSpawn>> tiles;
    size_t spawned = 0, spawnDropped = 0;
    std::set<uint32_t> writtenIds;
    for (const DisplayPlan& dp : plans)
        if (models[static_cast<size_t>(dp.model)].baked) writtenIds.insert(dp.id);
    for (const auto& r : spawns) {
        if (r.s.guid == 0) continue;
        if ((r.flags & 7) || !writtenIds.count(r.s.displayId)) {  // trigger, other phase, [DND] helper
            ++spawnDropped;
            continue;
        }
        const int tx = static_cast<int>(std::floor(32.0f - r.s.position[1] / kTileSize));
        const int ty = static_cast<int>(std::floor(32.0f - r.s.position[0] / kTileSize));
        tiles[{tx, ty}].push_back(r.s);
        ++spawned;
    }
    for (auto& [t, v] : tiles) {
        fmt::CreatureSpawnHeader h{};
        std::memcpy(h.magic, "WCSP", 4);
        h.version = fmt::kModelFormatVersion;
        h.count = static_cast<uint32_t>(v.size());
        std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(v.data()),
                    reinterpret_cast<const uint8_t*>(v.data() + v.size()));
        const std::string name = "s_" + std::to_string(mapId) + "_" + std::to_string(t.first) + "_" + std::to_string(t.second);
        writer.add(name, kKindSpawns, blob.data(), blob.size());
    }
    if (!spawns.empty())
        std::printf("spawns (our server only): %zu in %zu tiles (%zu dropped)\n", spawned, tiles.size(), spawnDropped);
    if (!written) return 1;
    if (!writer.finish()) return 1;
    std::error_code ec;
    const uintmax_t pakBytes = std::filesystem::file_size(out, ec);
    // The PSP's RAM for the open pak (PakIndex): the fence + three TOC blocks of 128 records.
    PakReadBack rb;
    if (rb.open(out))
        std::printf("pak: %llu bytes (%.1f MB), %u records; PSP PakIndex RAM: fence %u B + blocks %u B\n",
                    static_cast<unsigned long long>(pakBytes), static_cast<double>(pakBytes) / 1048576.0, rb.records(),
                    rb.fenceBytes(), 3u * rb.step() * 64u);
    int rc = 0;
    if (g_verify) {
        size_t checked = 0;
        const size_t errors = verifyPak(out, plans, models, checked);
        if (errors) rc = 3;
        if (g_verifyOver.load()) {
            std::fprintf(stderr, "verify: %d models above %.3f yd (lines \"VERIFY ... over the limit\")\n",
                         g_verifyOver.load(), g_verifyLimit);
            // 4: the pak reads back fine; some animations are off by more than the limit (a quality warning).
            // A read-back error stays 3 (a broken pak).
            if (!rc) rc = 4;
        }
    }
    std::printf("bake time %.1f s (%.1f min), jobs %u\n", secondsSince(t0), secondsSince(t0) / 60.0, jobs);
    return rc;
}
