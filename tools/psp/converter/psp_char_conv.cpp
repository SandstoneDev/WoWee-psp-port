// psp_char_conv - bakes the playable character models for the PSP.
//
//   psp_char_conv --extracted <dir> --out <characters.pak> [--race N]... [--tex-max 128]
//                 [--anims 0,1,4,5,11,12,13,37,38,39,41,42,96,97,98,<combat> | --no-anim] [--rate 30] [--palette 8]
//                 [--verify] [--dump <dir>]   (<combat> = 9,10,16,17,18,19,25,26,27,28,30,85,87)
//
// Per race and sex (upstream getPlayerModelPath / core::character_paths):
//   - "c_<race>_<sex>": the model in the stand pose, frame 0, skinned on the
//     CPU with upstream's bone rules (T(pivot)*T*R*S*T(-pivot), parents
//     first, m2_track sampler), every submesh kept with its id - the PSP picks
//     the geosets (core::bareCharacterGeosets). Batch texture follows
//     CharacterRenderer::resolveBatchTexture: hair (type 6) wins, then the
//     first non-white; types 1/8 are the body composite.
//   - "n_<race>_<sex>" (M9): the same batches skinned for the GE (<= 8 bones
//     per sub-batch) with the skeleton and the locomotion sequences resampled
//     at --rate (skin_bake.hpp). --verify decodes it the way the PSP does
//     (psp_skeleton.hpp) and compares with CPU skinning by upstream's rules.
//   - "a_<race>_<sex>": appearance tables. Every (skin, face) and
//     (hair style, hair colour) combination is resolved by upstream's
//     resolveCharacterSections, so its fallbacks are the same; hair scalp
//     and facial-hair geosets come from CharHairGeosets /
//     CharacterFacialHairStyles like entity_spawner builds them.
//   - "l_<n>" body layers at half size (the PSP composites them) and "x_<n>"
//     hair textures (T8 + mips, as the doodads).
//   - "p_<race>_<sex>": attachment points for helmets, shoulders
//     and weapons (attach_bake.hpp, upstream findAttachmentBone); their bones
//     are kept in "n_"/"o_". --verify checks the PSP pose at the points.
//   - "emotetext" (chat): EmotesText + EmotesTextData (+ the
//     client's localized /commands from FrameXML when extracted) for the PSP
//     chat; --emotetext-only writes just it, --emotetext-dump <tsv> lists it.
// Output layout: include/rendering/psp/psp_model_format.hpp.

#include "core/character_paths.hpp"
#include "core/geoset_rules.hpp"
#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/psp/psp_emote_text.hpp"  // chat

#include "model_bake.hpp"
#include "skin_bake.hpp"
#include "attach_bake.hpp"  // attachment points
#include "fsd_bake.hpp"  // sounds ($FSD footfalls)

#include <filesystem>

using namespace wowee;
using namespace wowee::pspconv;

namespace {

bool g_bindPose = false;  // --bind: skip the stand pose (debugging)
// M9: animations baked into "n_" (upstream anim ids: STAND, WALK, RUN,
// WALK_BACKWARDS, JUMP_START, JUMP, JUMP_END - what LocomotionFSM plays).
// Stand (every variation = the idle fidgets), Death, Walk, Run, Shuffle
// left/right (strafe), Walk backwards, Jump start/loop/end, Swim idle/swim,
// Sit down/sitting/sit up.
std::vector<uint32_t> g_anims = {0, 1, 4, 5, 11, 12, 13, 37, 38, 39, 41, 42, 96, 97, 98,
                                 // combat - what CombatFSM / the hit reactions play:
                                 // COMBAT_WOUND, COMBAT_CRITICAL, ATTACK_UNARMED/1H/2H/2HL,
                                 // READY_UNARMED/1H/2H/2HL, DODGE, ATTACK_1H_PIERCE, ATTACK_OFF
                                 9, 10, 16, 17, 18, 19, 25, 26, 27, 28, 30, 85, 87,
                                 57, 58, 118, // weapon special attacks (Heroic Strike, etc.)
                                 // spells - what spellCastAnimCallback picks from: SPELL, SPELL_PRECAST,
                                 // SPELL_CAST, SPELL_CAST_AREA, READY_SPELL_DIRECTED/OMNI,
                                 // SPELL_CAST_DIRECTED/OMNI, CHANNEL_CAST_DIRECTED/OMNI
                                 2, 31, 32, 33, 51, 52, 53, 54, 124, 125,
                                 // controls: SHEATHE / HIP_SHEATHE - the reach of the travel/combat (sheathe) switch (ui_world_sheath.cpp)
                                 89, 90,
                                 // mounts - MOUNT, the rider's seat (upstream updateMountedAnimation)
                                 91};
// Combat - the ids --verify expects in every "n_" (an attack and a
// ready stance at least: CombatFSM falls back to Stand without them).
const uint32_t kCombatAnims[] = {9, 10, 16, 17, 18, 19, 25, 26, 27, 28, 30, 85, 87};
// Emote animations (Emotes.dbc AnimID, minus g_anims): "o_<race>_<sex>".
std::vector<uint32_t> g_emoteAnims;
float g_rate = 30.0f;          // --rate: samples per second
bool g_verify = false;         // --verify: compare with CPU skinning
float g_verifyLimit = 0.02f;   // --verify fails above this error (yards)
bool g_verifyFailed = false;
std::string g_dumpDir;         // --dump <dir>: posed triangles for tools/psp/char_anim_render.py

constexpr uint32_t kKindCharModel = 5;
constexpr uint32_t kKindAppearance = 7;
constexpr uint32_t kKindLayer = 8;
constexpr uint32_t kKindSkinned = 9;

class LayerBank {
public:
    LayerBank(std::string root, std::vector<PakItem>& out) : root_(std::move(root)), out_(out) {}

    // base = the skin that is the canvas itself (full size, placed at 0,0).
    uint16_t get(const std::string& gamePath, bool base) {
        if (gamePath.empty()) return fmt::kNoLayer;
        const std::string key = lower(gamePath);
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second;
        uint16_t id = fmt::kNoLayer;
        std::vector<uint8_t> bytes;
        const Region r = base ? Region{0, 0, 256, 256, true} : regionFor(key);
        if (r.known && readFile(root_ + "/" + key, bytes)) {
            const pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
            if (!img.data.empty()) {
                const int w = r.w / 2, h = r.h / 2;
                const std::vector<Rgb> px = resample(img, w, h);
                std::vector<uint8_t> idx;
                std::array<Rgb, 256> pal;
                quantize(px, idx, pal);
                fmt::LayerHeader lh{};
                std::memcpy(lh.magic, "WLAY", 4);
                lh.w = static_cast<uint16_t>(w);
                lh.h = static_cast<uint16_t>(h);
                lh.x = static_cast<uint16_t>(r.x / 2);
                lh.y = static_cast<uint16_t>(r.y / 2);
                std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&lh),
                                          reinterpret_cast<const uint8_t*>(&lh + 1));
                for (const Rgb& p : pal) put<uint32_t>(blob, to8888(p));
                blob.insert(blob.end(), idx.begin(), idx.end());
                id = static_cast<uint16_t>(count_++);
                bytes_ += blob.size();
                out_.push_back({"l_" + std::to_string(id), kKindLayer, std::move(blob)});
            }
        }
        if (id == fmt::kNoLayer) std::fprintf(stderr, "  layer %s: %s\n", key.c_str(), r.known ? "missing" : "no region");
        ids_.emplace(key, id);
        return id;
    }
    size_t count() const { return count_; }
    size_t bytes() const { return bytes_; }

private:
    std::string root_;
    std::vector<PakItem>& out_;
    std::map<std::string, uint16_t> ids_;
    size_t count_ = 0, bytes_ = 0;
};

bool loadDbc(const std::string& root, const char* name, pipeline::DBCFile& dbc) {
    std::vector<uint8_t> bytes;
    return readFile(root + "/dbfilesclient/" + name, bytes) && dbc.load(bytes);
}

// upstream CharacterRenderer::playAnimation: variation 0 of the id, else the
// first sequence with it; -1 when the model has none.
int findSequence(const pipeline::M2Model& m, uint32_t animId) {
    int first = -1;
    for (size_t i = 0; i < m.sequences.size(); ++i) {
        if (m.sequences[i].id != animId) continue;
        if (m.sequences[i].variationIndex == 0) return static_cast<int>(i);
        if (first < 0) first = static_cast<int>(i);
    }
    return first;
}

// Keyframes of sequence `seq` into the model: in the M2 already (flag 0x20),
// else the external .anim (upstream loadExternalAnimations).
bool loadSequence(const std::string& root, const std::string& m2Path, const std::vector<uint8_t>& m2Bytes,
                  pipeline::M2Model& m, int seq) {
    const auto& s = m.sequences[seq];
    if (s.flags & 0x20) return true;
    std::vector<uint8_t> anim;
    if (!readFile(root + "/" + lower(animPath(m2Path, s.id, s.variationIndex)), anim)) return false;
    pipeline::M2Loader::loadAnimFile(m2Bytes, anim, static_cast<uint32_t>(seq), m);
    return true;
}

// Stand (animation 0, variation 0), frame 0, applied to the vertices.
bool poseStandSkinned(const std::string& root, const std::string& m2Path, const std::vector<uint8_t>& m2Bytes,
                      pipeline::M2Model& m) {
    const int seq = findSequence(m, 0);
    if (seq < 0 || m.bones.empty() || !loadSequence(root, m2Path, m2Bytes, m, seq)) return false;
    std::vector<glm::mat4> bones;
    poseBones(m, seq, 0.0f, bones);
    for (auto& v : m.vertices) {
        glm::vec3 p, n;
        if (skinVertex(v, bones, p, &n)) {
            v.position = p;
            v.normal = n;
        }
    }
    return true;
}

// upstream resolveBatchTexture: hair wins, else the first non-white slot.
uint16_t batchTexture(const pipeline::M2Model& m, const pipeline::M2Batch& b, TextureBank& tex, uint32_t& texFlags) {
    const uint32_t combos = std::min<uint32_t>(b.textureCount ? b.textureCount : 1u, 8u);
    uint16_t firstNonWhite = fmt::kNoTexture;
    for (uint32_t i = 0; i < combos; ++i) {
        const uint32_t pos = b.textureIndex + i;
        if (pos >= m.textureLookup.size()) break;
        const uint16_t slot = m.textureLookup[pos];
        if (slot >= m.textures.size()) continue;
        const auto& t = m.textures[slot];
        uint16_t id = fmt::kNoTexture;
        if (t.type == 6) {
            texFlags = t.flags;
            return fmt::kTexHair;
        }
        if (t.type == 1 || t.type == 8) {
            id = fmt::kTexBody;
        } else if (t.type == 0 && !std::string(t.filename.c_str()).empty()) {
            bool alpha = false;
            const int x = tex.get(t.filename.c_str(), alpha);
            if (x >= 0) id = static_cast<uint16_t>(x);
        }
        if (id != fmt::kNoTexture && firstNonWhite == fmt::kNoTexture) {
            firstNonWhite = id;
            texFlags = t.flags;
        }
    }
    return firstNonWhite;
}

bool fileExists(const std::string& gamePath, void* ctx) {
    return std::filesystem::exists(*static_cast<const std::string*>(ctx) + "/" + lower(gamePath));
}

// M9: the skinned model with its skeleton and locomotion sequences
// ("n_<race>_<sex>", WSKN), optionally checked against CPU skinning.
void bakeAnimated(const std::string& root, uint32_t race, uint32_t sex, const std::string& m2Path,
                  const std::vector<uint8_t>& m2, pipeline::M2Model& model,
                  const std::vector<pipeline::M2Vertex>& bindVertices, const std::vector<SkinBatchIn>& batches,
                  std::vector<PakItem>& out) {
    std::vector<int> seqs;
    for (uint32_t id : g_anims) {
        // Every variation of the id (variation 0 first): the client picks the
        // next one by frequency when a loop ends (Stand's fidgets).
        const int first = findSequence(model, id);
        if (first < 0) {
            std::printf("  anim %u: not in the model\n", id);
            continue;
        }
        std::vector<int> vars = {first};
        for (size_t i = 0; i < model.sequences.size(); ++i)
            if (model.sequences[i].id == id && static_cast<int>(i) != first) vars.push_back(static_cast<int>(i));
        for (int si : vars) {
            if (!loadSequence(root, m2Path, m2, model, si)) {
                std::fprintf(stderr, "  anim %u var %u: .anim file missing, skipped\n", id,
                             model.sequences[si].variationIndex);
                continue;
            }
            if (model.sequences[si].flags & 0x40)
                std::printf("  anim %u: alias flag 0x40 (sampled as is, like upstream)\n", id);
            seqs.push_back(si);
        }
    }
    {
        // Combat - which melee animations made it into "n_"; with
        // --verify a model without any attack or ready stance fails.
        size_t baked = 0;
        bool attack = false, ready = false;
        std::string missing;
        for (uint32_t id : kCombatAnims) {
            if (std::find(g_anims.begin(), g_anims.end(), id) == g_anims.end()) continue;
            const int s = findSequence(model, id);
            if (s >= 0 && std::find(seqs.begin(), seqs.end(), s) != seqs.end()) {
                ++baked;
                if (id == 16 || id == 17 || id == 18 || id == 19) attack = true;
                if (id >= 25 && id <= 28) ready = true;
            } else {
                missing += (missing.empty() ? "" : ",") + std::to_string(id);
            }
        }
        std::printf("  combat anims: %zu baked, missing [%s], attack=%d ready=%d\n", baked, missing.c_str(),
                    attack ? 1 : 0, ready ? 1 : 0);
        const bool wanted = std::find(g_anims.begin(), g_anims.end(), 16u) != g_anims.end() ||
                            std::find(g_anims.begin(), g_anims.end(), 17u) != g_anims.end();
        if (g_verify && wanted && (!attack || !ready)) {
            std::fprintf(stderr, "  verify: no melee attack / ready stance baked for race %u sex %u\n", race, sex);
            g_verifyFailed = true;
        }
    }
    const int standSeq = findSequence(model, 0);
    const std::vector<uint16_t> keep = attachmentKeepBones(model);  // attachment bones posed too
    SkinBakeInfo si;
    std::vector<uint8_t> blob = bakeSkinned(model, bindVertices, batches, seqs, standSeq, g_rate, si, false, &keep);
    char name[32];
    std::snprintf(name, sizeof(name), "n_%u_%u", race, sex);
    if (blob.empty()) {
        std::fprintf(stderr, "%s: skinned bake failed\n", name);
        return;
    }
    std::printf("  footstep events: %zu\n", appendFootstepEvents(blob, root, m2Path, m2, model, seqs));  // sounds
    std::printf("%s: %zu bones, %zu seqs, %zu sub-batches (palettes", name, si.bones, si.sequences, si.subBatches);
    for (size_t n = 0; n < si.paletteHistogram.size(); ++n) std::printf(" %zu", si.paletteHistogram[n]);
    std::printf("), %zu verts, %zu tris, %zu keys, vertices %zu KB, anim %zu KB, total %zu KB, stand %.2f yd",
                si.vertices, si.triangles, si.keys, si.vertexBytes / 1024, si.animBytes / 1024, blob.size() / 1024,
                si.standHeight);
    if (si.trimmedInfluences || si.weightless)
        std::printf(", trimmed %zu, weightless %zu", si.trimmedInfluences, si.weightless);
    std::printf("\n");
    for (int s : seqs)
        std::printf("    anim %3u var %u: %5u ms, flags 0x%x, freq %d, replay %u-%u\n", model.sequences[s].id,
                    model.sequences[s].variationIndex, model.sequences[s].duration, model.sequences[s].flags,
                    model.sequences[s].frequency, model.sequences[s].replayMin, model.sequences[s].replayMax);
    if (g_verify) {
        std::vector<uint8_t> dump;
        std::set<uint16_t> shown;
        for (uint16_t g : core::bareCharacterGeosets(1, 1, 1, 1, race)) shown.insert(g);
        shown.insert(core::kGeosetNoCape);
        const SkinVerifyResult r =
            verifySkinned(blob, model, bindVertices, seqs, si, batches, g_dumpDir.empty() ? nullptr : &dump, &shown);
        std::printf("  verify %s: max %.4f yd, rms %.5f yd over %zu vertex samples; worst: %s\n", name, r.maxErr,
                    r.samples ? std::sqrt(r.sumSq / static_cast<double>(r.samples)) : 0.0, r.samples, r.worst.c_str());
        if (!dump.empty()) {
            const std::string path = g_dumpDir + "/" + name + ".tris";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(dump.data(), 1, dump.size(), f);
                std::fclose(f);
            }
        }
        if (r.maxErr > g_verifyLimit) g_verifyFailed = true;
    }
    out.push_back({name, kKindSkinned, std::move(blob)});

    // M9c: the emote sequences (variation 0), same skeleton, on demand.
    std::vector<int> emo;
    for (uint32_t id : g_emoteAnims) {
        const int e = findSequence(model, id);
        if (e < 0 || std::find(seqs.begin(), seqs.end(), e) != seqs.end()) continue;
        if (!loadSequence(root, m2Path, m2, model, e)) continue;
        emo.push_back(e);
    }
    // One entry per emote animation ("o_<race>_<sex>_<anim>"): a model loads
    // only the one it plays (all 80+ together are 1.2-1.6 MB).
    fmt::SkinView mainView;
    const std::vector<uint8_t>& nblob = out.back().blob;
    const bool mainOk = mainView.init(nblob.data(), static_cast<uint32_t>(nblob.size()));
    size_t total = 0, count = 0;
    for (int e : emo) {
        SkinBakeInfo ei;
        std::vector<uint8_t> eblob =
            bakeSkinned(model, bindVertices, batches, {e}, standSeq, g_rate, ei, true, &keep);  // same bones
        appendFootstepEvents(eblob, root, m2Path, m2, model, {e});  // sounds ($FSD in WSKA too)
        fmt::SkinView extra;
        if (eblob.empty() || !mainOk ||
            !extra.init(eblob.data(), static_cast<uint32_t>(eblob.size()), true, mainView.h->boneCount)) {
            std::fprintf(stderr, "o_%u_%u_%u: emote bake failed\n", race, sex, model.sequences[e].id);
            g_verifyFailed = true;
            continue;
        }
        std::snprintf(name, sizeof(name), "o_%u_%u_%u", race, sex, model.sequences[e].id);
        total += eblob.size();
        ++count;
        out.push_back({name, kKindSkinned, std::move(eblob)});
    }
    std::printf("o_%u_%u_*: %zu emote animations, %zu KB (avg %zu KB)\n", race, sex, count, total / 1024,
                count ? total / 1024 / count : 0);
}

bool bakeRaceSex(const std::string& root, uint32_t race, uint32_t sex, const pipeline::DBCFile& sections,
                 const pipeline::DBCFile& hairGeo, const pipeline::DBCFile& facialDbc, TextureBank& tex,
                 LayerBank& layers, std::vector<PakItem>& out) {
    const std::string prefix = core::characterArtPrefix(race, sex);
    const std::string m2Path = std::string("Character\\") + core::raceModelFolder(race) + "\\" +
                               core::sexModelFolder(sex) + "\\" + prefix + ".m2";
    std::vector<uint8_t> m2;
    pipeline::M2Model model;
    const std::string file = lower(m2Path);
    if (!readFile(root + "/" + file, m2) || !loadM2(m2, root + "/" + lower(pipeline::skinPathForM2(m2Path)), model)) {
        std::fprintf(stderr, "%s: model unreadable\n", file.c_str());
        return false;
    }
    const std::vector<pipeline::M2Vertex> bindVertices = model.vertices;
    const bool posed = !g_bindPose && poseStandSkinned(root, m2Path, m2, model);

    WmdlBuilder o;
    std::vector<SkinBatchIn> skinBatches;
    for (const auto& b : model.batches) {
        if (b.submeshLevel != 0 || b.indexCount < 3 || b.indexStart + b.indexCount > model.indices.size()) continue;
        uint16_t blendMode = 0, matFlags = 0;
        if (b.materialIndex < model.materials.size()) {
            blendMode = model.materials[b.materialIndex].blendMode;
            matFlags = model.materials[b.materialIndex].flags;
        }
        fmt::ModelBatch ob{};
        uint32_t texFlags = 0;
        ob.texture = batchTexture(model, b, tex, texFlags);
        ob.blend = mapBlend(blendMode);
        ob.flags = static_cast<uint8_t>(((texFlags & 1) ? fmt::kBatchWrapU : 0) |
                                        ((texFlags & 2) ? fmt::kBatchWrapV : 0) |
                                        ((matFlags & 1) ? fmt::kBatchUnlit : 0));
        if (!o.add(model.indices, b.indexStart, b.indexCount, 0, 0, ob, b.submeshId)) break;
        SkinBatchIn sb;
        sb.material = ob;
        sb.geoset = b.submeshId;
        sb.tris.assign(model.indices.begin() + b.indexStart, model.indices.begin() + b.indexStart + (b.indexCount / 3) * 3);
        skinBatches.push_back(std::move(sb));
    }
    if (o.empty()) return false;
    WmdlInfo info;
    std::vector<uint8_t> blob = o.build(model.vertices, 0, true, info);
    char name[32];
    std::snprintf(name, sizeof(name), "c_%u_%u", race, sex);
    std::printf("%s %s: %zu verts, %u tris, %zu batches, pose %s\n", name, file.c_str(), info.vertices,
                info.triangles, info.batches[0], posed ? "stand" : "bind");
    out.push_back({name, kKindCharModel, std::move(blob)});

    if (!g_anims.empty()) bakeAnimated(root, race, sex, m2Path, m2, model, bindVertices, skinBatches, out);

    // Appearance ranges from CharSections.
    const pipeline::CharSectionsFields f = pipeline::detectCharSectionsFields(&sections, nullptr);
    uint32_t skins = 1, faces = 1, styles = 1, colors = 1;
    for (uint32_t i = 0; i < sections.getRecordCount(); ++i) {
        if (sections.getUInt32(i, f.raceId) != race || sections.getUInt32(i, f.sexId) != sex) continue;
        const uint32_t base = sections.getUInt32(i, f.baseSection);
        const uint32_t var = sections.getUInt32(i, f.variationIndex), col = sections.getUInt32(i, f.colorIndex);
        if (base == 0) skins = std::max(skins, col + 1);
        if (base == 1) faces = std::max(faces, var + 1);
        if (base == 3) {
            styles = std::max(styles, var + 1);
            colors = std::max(colors, col + 1);
        }
    }
    skins = std::min(skins, 32u);
    faces = std::min(faces, 32u);
    styles = std::min(styles, 32u);
    colors = std::min(colors, 32u);

    std::string rootCopy = root;
    std::vector<uint16_t> body;
    for (uint32_t s = 0; s < skins; ++s) {
        for (uint32_t fc = 0; fc < faces; ++fc) {
            pipeline::CharacterAppearance who;
            who.raceId = race;
            who.sexId = sex;
            who.skinId = static_cast<uint8_t>(s);
            who.faceId = static_cast<uint8_t>(fc);
            const auto t = pipeline::resolveCharacterSections(&sections, f, who, fileExists, &rootCopy);
            const std::string skin = t.bodySkin.empty() ? core::defaultBodySkinPath(race, sex) : t.bodySkin;
            body.push_back(layers.get(skin, true));
            body.push_back(layers.get(t.faceLower, false));
            body.push_back(layers.get(t.faceUpper, false));
            for (size_t u = 0; u < 3; ++u)
                body.push_back(u < t.underwear.size() ? layers.get(t.underwear[u], false) : fmt::kNoLayer);
        }
    }
    std::vector<uint16_t> hair;
    for (uint32_t st = 0; st < styles; ++st) {
        for (uint32_t c = 0; c < colors; ++c) {
            pipeline::CharacterAppearance who;
            who.raceId = race;
            who.sexId = sex;
            who.hairStyleId = static_cast<uint8_t>(st);
            who.hairColorId = static_cast<uint8_t>(c);
            const auto t = pipeline::resolveCharacterSections(&sections, f, who, fileExists, &rootCopy);
            // upstream applyCharacterTextures: last-resort hair.
            const std::string path = t.hair.empty()
                                         ? std::string("Character\\") + core::raceModelFolder(race) + "\\Hair00_00.blp"
                                         : t.hair;
            bool alpha = false;
            const int id = tex.get(path, alpha);
            hair.push_back(id < 0 ? fmt::kNoTexture : static_cast<uint16_t>(id));
        }
    }
    // CharHairGeosets: RaceID 1, SexID 2, Variation 3, GeosetID 4, Showscalp 5 (entity_spawner).
    std::vector<uint16_t> hairGeosets(styles, 0);
    for (uint32_t i = 0; i < hairGeo.getRecordCount(); ++i) {
        if (hairGeo.getUInt32(i, 1) != race || hairGeo.getUInt32(i, 2) != sex) continue;
        const uint32_t var = hairGeo.getUInt32(i, 3);
        if (var >= styles) continue;
        const bool scalp = hairGeo.getFieldCount() > 5 && hairGeo.getUInt32(i, 5) != 0;
        hairGeosets[var] = static_cast<uint16_t>(scalp ? 1 : hairGeo.getUInt32(i, 4));
    }
    // CharacterFacialHairStyles: RaceID 0, SexID 1, Variation 2; no row = (1, 1, 1).
    const pipeline::FacialHairFields fh = pipeline::detectFacialHairFields(&facialDbc, nullptr);
    uint32_t facials = 1;
    for (uint32_t i = 0; i < facialDbc.getRecordCount(); ++i)
        if (facialDbc.getUInt32(i, 0) == race && facialDbc.getUInt32(i, 1) == sex)
            facials = std::max(facials, facialDbc.getUInt32(i, 2) + 1);
    facials = std::min(facials, 32u);
    std::vector<uint16_t> facial(facials * 3, 1);
    for (uint32_t i = 0; i < facialDbc.getRecordCount(); ++i) {
        if (facialDbc.getUInt32(i, 0) != race || facialDbc.getUInt32(i, 1) != sex) continue;
        const uint32_t var = facialDbc.getUInt32(i, 2);
        if (var >= facials) continue;
        facial[var * 3 + 0] = static_cast<uint16_t>(facialDbc.getUInt32(i, fh.geoset100));
        facial[var * 3 + 1] = static_cast<uint16_t>(facialDbc.getUInt32(i, fh.geoset200));
        facial[var * 3 + 2] = static_cast<uint16_t>(facialDbc.getUInt32(i, fh.geoset300));
    }

    fmt::AppearanceHeader ah{};
    std::memcpy(ah.magic, "WAPP", 4);
    ah.version = fmt::kModelFormatVersion;
    ah.race = static_cast<uint8_t>(race);
    ah.sex = static_cast<uint8_t>(sex);
    ah.skins = static_cast<uint8_t>(skins);
    ah.faces = static_cast<uint8_t>(faces);
    ah.hairStyles = static_cast<uint8_t>(styles);
    ah.hairColors = static_cast<uint8_t>(colors);
    ah.facials = static_cast<uint8_t>(facials);
    std::vector<uint8_t> app(sizeof(ah));
    auto putArray = [&](const std::vector<uint16_t>& v) {
        alignTo(app, 4);
        const uint32_t off = static_cast<uint32_t>(app.size());
        for (uint16_t x : v) put<uint16_t>(app, x);
        return off;
    };
    ah.bodyOffset = putArray(body);
    ah.hairOffset = putArray(hair);
    ah.hairGeosetOffset = putArray(hairGeosets);
    ah.facialOffset = putArray(facial);
    std::memcpy(app.data(), &ah, sizeof(ah));
    std::snprintf(name, sizeof(name), "a_%u_%u", race, sex);
    std::printf("%s: skins %u faces %u hair %ux%u facial %u\n", name, skins, faces, styles, colors, facials);
    out.push_back({name, kKindAppearance, std::move(app)});

    // Optional facial/scalp layers, indexed by style and hair color. WAPP stays
    // compatible with older packs and readers; missing rows remain transparent.
    std::vector<uint16_t> detail((facials+styles)*colors*2,fmt::kNoLayer);
    for (uint32_t i=0;i<sections.getRecordCount();++i) {
        if (sections.getUInt32(i,f.raceId)!=race || sections.getUInt32(i,f.sexId)!=sex) continue;
        const uint32_t base=sections.getUInt32(i,f.baseSection);
        const uint32_t var=sections.getUInt32(i,f.variationIndex), color=sections.getUInt32(i,f.colorIndex);
        if (color>=colors || (base!=2 && base!=3)) continue;
        if (var>=(base==2 ? facials : styles)) continue;
        const uint32_t at=((base==2 ? var : facials+var)*colors+color)*2;
        // Facial section textures 1/2, scalp section textures 2/3.
        detail[at]=layers.get(sections.getString(i,base==2 ? f.texture1 : f.texture2),false);
        detail[at+1]=layers.get(sections.getString(i,base==2 ? f.texture2 : f.texture2+1),false);
    }
    std::vector<uint8_t> extra={'W','A','F','L',static_cast<uint8_t>(facials),static_cast<uint8_t>(styles),static_cast<uint8_t>(colors),0};
    for (uint16_t id:detail) put<uint16_t>(extra,id);
    std::snprintf(name,sizeof(name),"f_%u_%u",race,sex);
    out.push_back({name,kKindAppearance,std::move(extra)});

    // Attachments - "p_<race>_<sex>" (attach_bake.hpp): upstream
    // findAttachmentBone per attachment id, the WSKN bone of "n_" and the
    // Stand frame 0 matrix; --verify compares the PSP pose at the points.
    {
        char nName[32];
        std::snprintf(nName, sizeof(nName), "n_%u_%u", race, sex);
        const std::vector<uint8_t>* nBlob = nullptr;
        for (const PakItem& it : out)
            if (it.name == nName) nBlob = &it.blob;
        AttachBakeInfo ai;
        std::vector<uint8_t> watp = bakeAttachTable(model, posed ? findSequence(model, 0) : -1, nBlob, g_verify, ai);
        std::snprintf(name, sizeof(name), "p_%u_%u", race, sex);
        std::printf("%s: %zu attachment points, %zu in n_ (lookup %zu, attachments %zu)\n", name, ai.points, ai.inSkin,
                    model.attachmentLookup.size(), model.attachments.size());
        if (g_verify && nBlob) {
            std::printf("  verify %s: max %.4f yd, axis %.4f over %zu samples; worst: %s\n", name, ai.maxErr,
                        ai.maxAxisErr, ai.samples, ai.worst.c_str());
            if (ai.maxErr > g_verifyLimit) g_verifyFailed = true;
        }
        out.push_back({name, kKindAppearance, std::move(watp)});
    }
    return true;
}

// Chat -----------------------------------------------------------------------
// "emotetext": EmotesText.dbc + EmotesTextData.dbc the way upstream
// EmoteRegistry::loadFromDbc reads them (layout Data/expansions/wotlk/
// dbc_layouts.json: ID 0, Command 1, EmoteRef 2, OthersTarget 3,
// SenderTarget 5, OthersNoTarget 7, SenderNoTarget 9; EmotesTextData ID 0,
// Text 1). PSP additions: the text is taken from the locale slot the client's
// DBC actually fills (a ruRU client writes slot 8, upstream reads slot 0 of an
// enUS file), and the client's localized slash commands (FrameXML
// GlobalStrings.lua EMOTEn_CMDm + ChatFrame.lua EMOTEn_TOKEN, like the 3.3.5a
// client's hash_EmoteTokenList) are added as aliases when those files were
// extracted.
namespace emotetext {

int g_localeSlot = -1;          // --emotetext-locale N (0..15), -1 = the fullest slot
bool g_enabled = true;          // --no-emotetext
bool g_only = false;            // --emotetext-only: a pak with just the tables (quick check, not for the card)
std::string g_dumpTsv;          // --emotetext-dump <file.tsv>
std::string g_globalStrings;    // --globalstrings <GlobalStrings.lua>
std::string g_chatFrame;        // --chatframe <ChatFrame.lua>

// upstream emote_registry.cpp parseEmoteCommands (static there).
std::vector<std::string> parseEmoteCommands(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// EMOTE<n>_TOKEN = "X" / EMOTE<n>_CMD<m> = "/x" assignments of a FrameXML file.
void scanEmoteLua(const std::string& path, std::map<int, std::string>& tokens,
                  std::map<int, std::vector<std::string>>& cmds) {
    std::vector<uint8_t> bytes;
    if (path.empty() || !readFile(path, bytes)) return;
    const std::string s(bytes.begin(), bytes.end());
    size_t pos = 0;
    while ((pos = s.find("EMOTE", pos)) != std::string::npos) {
        size_t p = pos + 5;
        pos = p;
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) continue;
        const int n = std::atoi(s.c_str() + p);
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
        bool token = false;
        if (s.compare(p, 6, "_TOKEN") == 0) {
            token = true;
            p += 6;
        } else if (s.compare(p, 4, "_CMD") == 0) {
            p += 4;
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
        } else {
            continue;
        }
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
        if (p >= s.size() || s[p] != '=') continue;
        ++p;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
        if (p >= s.size() || s[p] != '"') continue;
        const size_t end = s.find('"', p + 1);
        if (end == std::string::npos) break;
        const std::string value = s.substr(p + 1, end - p - 1);
        if (token) tokens[n] = value;
        else if (!value.empty()) cmds[n].push_back(value);
        pos = end;
    }
}

bool bake(const std::string& root, std::vector<PakItem>& out, bool verify) {
    pipeline::DBCFile et, etd;
    if (!loadDbc(root, "emotestext.dbc", et) || !loadDbc(root, "emotestextdata.dbc", etd)) {
        std::printf("emotetext: EmotesText/EmotesTextData missing - skipped (the PSP uses its built-in table)\n");
        return false;
    }
    // Locale slot of EmotesTextData.Text (fields 1..16).
    int slot = g_localeSlot;
    if (slot < 0) {
        uint32_t best = 0;
        slot = 0;
        for (int l = 0; l < 16 && 1u + l < etd.getFieldCount(); ++l) {
            uint32_t filled = 0;
            for (uint32_t r = 0; r < etd.getRecordCount(); ++r)
                if (!etd.getStringView(r, 1 + l).empty()) ++filled;
            if (filled > best) {
                best = filled;
                slot = l;
            }
        }
    }
    std::map<uint32_t, std::string> textData;
    for (uint32_t r = 0; r < etd.getRecordCount(); ++r) {
        std::string t = etd.getString(r, 1 + static_cast<uint32_t>(slot));
        if (!t.empty()) textData.emplace(etd.getUInt32(r, 0), std::move(t));
    }

    struct Row {
        uint16_t id = 0, emoteRef = 0;
        std::string name;
        std::string text[fmt::kEtSlotCount];
    };
    std::map<uint16_t, Row> rows;  // sorted by id
    static constexpr uint32_t kTextField[fmt::kEtSlotCount] = {3, 5, 7, 9};
    for (uint32_t r = 0; r < et.getRecordCount(); ++r) {
        Row row;
        row.id = static_cast<uint16_t>(et.getUInt32(r, 0));
        row.name = et.getString(r, 1);
        if (row.name.empty() || row.id == 0) continue;  // upstream: no command, no entry
        row.emoteRef = static_cast<uint16_t>(et.getUInt32(r, 2));
        for (uint32_t s = 0; s < fmt::kEtSlotCount; ++s) {
            auto it = textData.find(et.getUInt32(r, kTextField[s]));
            if (it != textData.end()) row.text[s] = it->second;
        }
        rows[row.id] = std::move(row);
    }

    // Aliases: upstream commands from the name, then the client's localized ones.
    std::map<std::string, std::pair<uint16_t, uint8_t>> aliases;  // key -> (id, source)
    std::map<std::string, uint16_t> byToken;
    for (const auto& [id, row] : rows) {
        std::string upper = row.name;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        byToken.emplace(upper, id);
        for (const std::string& cmd : parseEmoteCommands(row.name)) aliases.emplace(cmd, std::make_pair(id, uint8_t{0}));
    }
    std::map<int, std::string> tokens;
    std::map<int, std::vector<std::string>> cmds;
    scanEmoteLua(g_chatFrame.empty() ? root + "/interface/framexml/chatframe.lua" : g_chatFrame, tokens, cmds);
    scanEmoteLua(g_globalStrings.empty() ? root + "/interface/framexml/globalstrings.lua" : g_globalStrings, tokens,
                 cmds);
    size_t localized = 0;
    for (const auto& [n, list] : cmds) {
        auto t = tokens.find(n);
        if (t == tokens.end()) continue;
        auto row = byToken.find(t->second);
        if (row == byToken.end()) continue;
        for (const std::string& c : list) {
            const std::string key = rendering::psp::emoteCommandKey(c);
            if (!key.empty() && aliases.emplace(key, std::make_pair(row->second, uint8_t{1})).second) ++localized;
        }
    }

    // Blob: header, rows, aliases (std::map order = strcmp order), pool.
    std::vector<char> pool;
    std::map<std::string, uint32_t> pooled;
    auto intern = [&](const std::string& s) -> uint32_t {
        auto it = pooled.find(s);
        if (it != pooled.end()) return it->second;
        const auto off = static_cast<uint32_t>(pool.size());
        pool.insert(pool.end(), s.begin(), s.end());
        pool.push_back('\0');
        pooled.emplace(s, off);
        return off;
    };
    std::vector<fmt::EmoteTextRow> outRows;
    std::map<uint16_t, uint16_t> rowIndex;
    for (const auto& [id, row] : rows) {
        fmt::EmoteTextRow o{};
        o.textEmoteId = id;
        o.emoteId = row.emoteRef;
        for (uint32_t s = 0; s < fmt::kEtSlotCount; ++s)
            o.text[s] = row.text[s].empty() ? fmt::kEmoteTextNone : intern(row.text[s]);
        rowIndex[id] = static_cast<uint16_t>(outRows.size());
        outRows.push_back(o);
    }
    std::vector<fmt::EmoteTextAlias> outAliases;
    for (const auto& [key, v] : aliases) {
        fmt::EmoteTextAlias a{};
        a.command = intern(key);
        a.row = rowIndex[v.first];
        a.source = v.second;
        outAliases.push_back(a);
    }
    if (pool.empty()) pool.push_back('\0');
    fmt::EmoteTextHeader h{};
    std::memcpy(h.magic, "WETX", 4);
    h.version = 1;
    h.slots = fmt::kEtSlotCount;
    h.rowCount = static_cast<uint32_t>(outRows.size());
    h.aliasCount = static_cast<uint32_t>(outAliases.size());
    h.poolSize = static_cast<uint32_t>(pool.size());
    std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(outRows.data()),
                reinterpret_cast<const uint8_t*>(outRows.data() + outRows.size()));
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(outAliases.data()),
                reinterpret_cast<const uint8_t*>(outAliases.data() + outAliases.size()));
    blob.insert(blob.end(), pool.begin(), pool.end());
    std::printf("emotetext: %u rows, %u commands (%zu localized, lua tokens %zu), text slot %d, %zu KB\n",
                h.rowCount, h.aliasCount, localized, tokens.size(), slot, blob.size() / 1024);

    // Self-check with the PSP reader (always: it is cheap), details on --verify.
    rendering::psp::EmoteTextView view;
    const char* why = nullptr;
    if (!view.attach(blob.data(), static_cast<uint32_t>(blob.size()), &why)) {
        std::fprintf(stderr, "emotetext: PSP reader rejects the blob (%s)\n", why ? why : "?");
        g_verifyFailed = true;
        return false;
    }
    for (uint32_t i = 0; i < view.aliasCount(); ++i) {
        const int r = view.findCommand(view.str(view.alias(i).command));
        if (r < 0 || view.row(static_cast<uint32_t>(r)).textEmoteId != outRows[view.alias(i).row].textEmoteId) {
            std::fprintf(stderr, "emotetext: command '%s' does not resolve\n", view.str(view.alias(i).command));
            g_verifyFailed = true;
        }
    }
    if (verify) {
        // The ids the PSP already sends from R + D-pad (M9c) must be what the
        // DBC calls them.
        const struct { const char* cmd; uint32_t id; } known[] = {{"wave", 101}, {"cheer", 21}, {"dance", 34}, {"bow", 17}};
        for (const auto& k : known) {
            const int r = view.findCommand(k.cmd);
            const uint32_t got = r < 0 ? 0 : view.row(static_cast<uint32_t>(r)).textEmoteId;
            std::printf("  verify emotetext /%s -> %u (want %u)%s\n", k.cmd, got, k.id, got == k.id ? "" : "  MISMATCH");
            if (got != k.id) g_verifyFailed = true;
            if (r >= 0) {
                const char* o = view.text(static_cast<uint32_t>(r), fmt::kEtOthersNoTarget);
                std::printf("    others: %s\n", o ? o : "(none)");
            }
        }
    }
    if (!g_dumpTsv.empty()) {
        if (FILE* f = std::fopen(g_dumpTsv.c_str(), "wb")) {
            std::fprintf(f, "id\temote\tcommands\tothers_target\tsender_target\tothers_notarget\tsender_notarget\n");
            for (uint32_t r = 0; r < view.rowCount(); ++r) {
                std::string list;
                for (uint32_t i = 0; i < view.aliasCount(); ++i)
                    if (view.alias(i).row == r) list += std::string(list.empty() ? "" : ",") + view.str(view.alias(i).command);
                std::fprintf(f, "%u\t%u\t%s", view.row(r).textEmoteId, view.row(r).emoteId, list.c_str());
                for (uint32_t s = 0; s < fmt::kEtSlotCount; ++s) {
                    const char* t = view.text(r, static_cast<fmt::EmoteTextSlot>(s));
                    std::fprintf(f, "\t%s", t ? t : "");
                }
                std::fprintf(f, "\n");
            }
            std::fclose(f);
            std::printf("emotetext: dump -> %s\n", g_dumpTsv.c_str());
        }
    }
    out.push_back({"emotetext", kKindAppearance, std::move(blob)});
    return true;
}

} // namespace emotetext
// Chat (end) ------------------------------------------------------------------

} // namespace

int main(int argc, char** argv) {
    std::string root, out;
    int texMax = 128;
    std::vector<uint32_t> races;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) root = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--tex-max" && i + 1 < argc) texMax = std::atoi(argv[++i]);
        else if (a == "--bind") g_bindPose = true;
        else if (a == "--race" && i + 1 < argc) races.push_back(static_cast<uint32_t>(std::atoi(argv[++i])));
        else if (a == "--rate" && i + 1 < argc) g_rate = std::max(1.0f, static_cast<float>(std::atof(argv[++i])));
        else if (a == "--verify") g_verify = true;
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--palette" && i + 1 < argc) g_maxPalette = std::clamp(std::atoi(argv[++i]), 4, 8);
        else if (a == "--verify-limit" && i + 1 < argc) g_verifyLimit = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--dump" && i + 1 < argc) g_dumpDir = argv[++i];
        else if (a == "--no-anim") g_anims.clear();
        // chat
        else if (a == "--no-emotetext") emotetext::g_enabled = false;
        else if (a == "--emotetext-only") emotetext::g_only = true;
        else if (a == "--emotetext-locale" && i + 1 < argc) emotetext::g_localeSlot = std::atoi(argv[++i]);
        else if (a == "--emotetext-dump" && i + 1 < argc) emotetext::g_dumpTsv = argv[++i];
        else if (a == "--globalstrings" && i + 1 < argc) emotetext::g_globalStrings = argv[++i];
        else if (a == "--chatframe" && i + 1 < argc) emotetext::g_chatFrame = argv[++i];
        else if (a == "--anims" && i + 1 < argc) {
            g_anims.clear();
            for (const char* p = argv[++i]; *p;) {
                char* end = nullptr;
                const unsigned long id = std::strtoul(p, &end, 10);
                if (end == p) break;
                g_anims.push_back(static_cast<uint32_t>(id));
                p = *end == ',' ? end + 1 : end;
            }
        }
    }
    if (root.empty() || out.empty()) {
        std::fprintf(stderr,
                     "usage: psp_char_conv --extracted <dir> --out <pak> [--race N]... [--tex-max 128]\n"
                     "       [--anims 0,1,4,5,11,12,13,37,38,39,41,42,96,97,98,9,10,16,17,18,19,25,26,27,28,30,85,87 | --no-anim]\n"
                     "       [--rate 30] [--verify [--verify-limit 0.02]]\n"
                     "       [--palette 8] [--dump <dir>]\n"
                     "       [--no-emotetext | --emotetext-only] [--emotetext-locale N] [--emotetext-dump <tsv>]\n"  // chat
                     "       [--globalstrings <GlobalStrings.lua>] [--chatframe <ChatFrame.lua>]\n");
        return 2;
    }
    if (races.empty()) races = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
    pipeline::DBCFile sections, hairGeo, facial;
    if (!loadDbc(root, "charsections.dbc", sections) || !loadDbc(root, "charhairgeosets.dbc", hairGeo) ||
        !loadDbc(root, "characterfacialhairstyles.dbc", facial)) {
        std::fprintf(stderr, "missing DBC under %s/dbfilesclient\n", root.c_str());
        return 1;
    }
    std::vector<PakItem> models, textures, layerItems;
    // Emotes.dbc (upstream EmoteRegistry: ID 0, AnimID 2, EmoteSpecProc 4).
    pipeline::DBCFile emotesDbc;
    if (!g_anims.empty() && loadDbc(root, "emotes.dbc", emotesDbc)) {
        std::vector<fmt::EmoteRow> rows;
        std::set<uint32_t> ids;
        for (uint32_t r = 0; r < emotesDbc.getRecordCount(); ++r) {
            fmt::EmoteRow row{};
            row.emoteId = static_cast<uint16_t>(emotesDbc.getUInt32(r, 0));
            row.animId = static_cast<uint16_t>(emotesDbc.getUInt32(r, 2));
            row.state = emotesDbc.getUInt32(r, 4) != 0 ? 1 : 0;
            rows.push_back(row);
            if (row.animId && std::find(g_anims.begin(), g_anims.end(), row.animId) == g_anims.end())
                ids.insert(row.animId);
        }
        // The sit-in-chair loops a chair's / the barber chair's stand state plays
        // (SMSG_STANDSTATE_UPDATE 4..6: SitChairLow / Med / High, upstream ActivityFSM) - with the emotes: loaded on
        // demand (CharacterModel::playAnimation -> loadExtra), nothing more in "n_".
        for (uint32_t id : {50u, 102u, 103u, 104u, 133u, 134u, 114u, 115u, 116u, 188u, 189u})
            if (std::find(g_anims.begin(), g_anims.end(), id) == g_anims.end()) ids.insert(id);
        // SpellVisualKit animations outside the base set remain on-demand extras,
        // so adding ability coverage does not enlarge every resident base model.
        pipeline::DBCFile spellKits;
        if(loadDbc(root,"spellvisualkit.dbc",spellKits) && spellKits.getFieldCount()>2)
            for(uint32_t row=0;row<spellKits.getRecordCount();++row)for(uint32_t field:{1u,2u}) {
                const uint32_t id=spellKits.getUInt32(row,field);
                if(id && id<65535 && std::find(g_anims.begin(),g_anims.end(),id)==g_anims.end())ids.insert(id);
            }
        g_emoteAnims.assign(ids.begin(), ids.end());
        fmt::EmoteTable t{};
        std::memcpy(t.magic, "WEMO", 4);
        t.count = static_cast<uint32_t>(rows.size());
        std::vector<uint8_t> blob(sizeof(t) + rows.size() * sizeof(fmt::EmoteRow));
        std::memcpy(blob.data(), &t, sizeof(t));
        std::memcpy(blob.data() + sizeof(t), rows.data(), rows.size() * sizeof(fmt::EmoteRow));
        std::printf("emotes: %zu rows, %zu emote animations\n", rows.size(), g_emoteAnims.size());
        models.push_back({"emotes", kKindAppearance, std::move(blob)});
    }
    // chat
    if (emotetext::g_enabled) {
        const bool ok = emotetext::bake(root, models, g_verify);
        if (emotetext::g_only) return ok && !g_verifyFailed && writePak(out, models) ? 0 : 1;
    }
    TextureBank tex(root, texMax, textures);
    LayerBank layers(root, layerItems);
    int baked = 0;
    for (uint32_t r : races)
        for (uint32_t s = 0; s < 2; ++s)
            if (bakeRaceSex(root, r, s, sections, hairGeo, facial, tex, layers, models)) ++baked;
    if (!baked) return 1;
    std::printf("characters %d, layers %zu (%zu KB), textures %zu (%zu KB)\n", baked, layers.count(),
                layers.bytes() / 1024, tex.count(), tex.bytes() / 1024);
    std::vector<PakItem> all;
    for (auto* v : {&models, &layerItems, &textures})
        for (auto& e : *v) all.push_back(std::move(e));
    if (!writePak(out, all)) return 1;
    if (g_verifyFailed) {
        // 4: the pak is written and complete; some animation samples are off by more than the limit (a quality
        // warning, not a broken pak - the caller decides).
        std::fprintf(stderr, "verify: error above %.3f yd\n", g_verifyLimit);
        return 4;
    }
    return 0;
}
