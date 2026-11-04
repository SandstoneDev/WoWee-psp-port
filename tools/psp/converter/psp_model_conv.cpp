// psp_model_conv - bakes the M2 doodads placed on ADT tiles for the PSP.
//
//   psp_model_conv --extracted <dir> --map <map> --out <doodads.pak>
//                  [--tile X_Y]... [--around X Y R] [--tex-max 128]
//                  [--tree-lod2 0.3] [--impostors 1] [--impostor-cell 64]
//   psp_model_conv --verify <doodads.pak> [--dump <dir>]   (PC check)
//   psp_model_conv --extracted <dir> --gameobjects --out <gameobjects.pak>  (GameObjectDisplayInfo M2s)
//   psp_model_conv --extracted <dir> --transports --out <transports.pak> [--transport-all-paths]
//   psp_model_conv --extracted <dir> --transports --wmo-set other --out <gameobjects_wmo.pak>
//                  (GameObjectDisplayInfo WMOs - the transports' with their DBC tables, or the
//                  other ones - transport_bake.hpp; --wmo-set transports (default) | other | all, anything else fails)
//   psp_model_conv --selftest <dir>                        (synthetic tree, no data)
//   particles: [--particles 0] [--ptc-tex 64] [--ptc-list] - the models' particle emitters,
//   ribbons and upstream's smoke in the pak's first entry "pidx" (particle_bake.hpp)
//
// Parsing is upstream's ADTLoader (MMDX/MDDF), M2Loader + loadSkin and
// BLPLoader, unchanged; placement and batch rules follow upstream's
// terrain_manager / M2Renderer (see docs/PSP_PORT_ARCHITECTURE.md, M7):
//   - model: rest pose of skin 00, level-0 submeshes; a lower skin (03..01)
//     is kept as LOD 1 when it has at most 60 % of the indices;
//   - batch texture = textures[textureLookup[textureIndex]]; type-0 files are
//     baked, replaceable slots draw white, a named texture that is missing
//     drops the batch (upstream draws it with opacity 0);
//   - UV set = batch textureUnit, so a vertex used with both sets is emitted
//     twice; lighting is baked per vertex from the model-space normal;
//   - textures: box-filtered to <= tex-max, median-cut to 256 RGBA colours,
//     T8 swizzled with an ABGR8888 CLUT (WTEX, as the UI textures), plus
//     mip levels mapped to the same CLUT (distant foliage: no shimmer, and
//     the GE texture cache stops thrashing).
//   - PSP: foliage models (upstream classifier isFoliageLike) also get a far
//     LOD 2 (simplified mesh) and an impostor "i_<n>" (forest LOD,
//     forest_bake.hpp); buildings and props are unchanged.
//   - global-WMO maps: a map whose WDT names a global WMO
//     (dungeons: no ADTs) bakes that WMO as upstream WorldLoader places it,
//     its parts spread over virtual tiles "d_<x>_<y>" + a "wmo_only" record
//     (bakeGlobalWmo below, include/rendering/psp/psp_map_format.hpp).
//   - dungeons: a global WMO over --global-max-tris source triangles is baked as a big WMO (the capitals'
//     path, bigwmo/ + "wbig") instead of virtual tiles; the doodads' emitters of a global WMO go to "pidx".
// Output layout: include/rendering/psp/psp_model_format.hpp.

#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wdt_loader.hpp"  // global-WMO maps
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/psp/psp_map_format.hpp"  // global-WMO maps
#include "rendering/psp/psp_model_format.hpp"

#include "conv_common.hpp"
#include "forest_bake.hpp"
#include "forest_selftest.hpp"
#include "model_bake.hpp"
#include "particle_bake.hpp"  // particles
#include "wmo_chunk_bake.hpp"  // big WMOs
#include "bigwmo_tools.hpp"    // big WMOs: --bigwmo-verify / --bigwmo-route
#include "transport_bake.hpp"  // transport WMOs

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>
#include <map>
#include <filesystem>  // big WMOs: bigwmo/ next to the pak
#include <tuple>
#include <unordered_map>

using namespace wowee;
using namespace wowee::pspconv;

namespace {

struct BakedModel {
    int id = -1;
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    int effect = -1;  // particles: its "pidx" effect (also for a model without a WMDL: id -1)
};

class ModelBank {
public:
    ModelBank(std::string root, TextureBank& tex, std::vector<PakItem>& out, float maxEdge,
              const ForestOptions& forest)
        : root_(std::move(root)), tex_(tex), out_(out), maxEdge_(maxEdge), forest_(forest), impostor_(root_) {}

    const BakedModel& get(const std::string& adtName) {
        const std::string path = lower(pipeline::modelPathToM2(adtName));
        auto it = models_.find(path);
        if (it != models_.end()) return it->second;
        BakedModel bm;
        bake(path, bm);
        return models_.emplace(path, bm).first->second;
    }

    size_t count() const { return count_; }
    // Upstream WorldLoader marks a global WMO's doodads setSkipWallCollision - their
    // collision meshes are floors only (no kColBlocksWalls). Every M2 of such a pak belongs to the instance.
    void setWallsBlock(bool on) { wallsBlock_ = on; }
    size_t collisions() const { return colCount_; }
    size_t collisionBytes() const { return colBytes_; }
    size_t skipped() const { return skipped_; }
    size_t emitterVolumes() const { return emitterVolumes_; }  // of skipped()
    size_t bytes() const { return bytes_; }
    size_t triangles() const { return tris_; }
    const Lod2Stats& lod2Stats() const { return lod2_; }
    size_t impostors() const { return impostorCount_; }
    // TriangleSplitter stopped short (triangles left longer than the near edge, the GE may drop
    // them next to the camera) - one line per model / WMO group, the totals in the bake's summary (was: silent).
    void noteSplitShort(const std::string& what, size_t depthStops, size_t ceilingStops, float longest, float maxEdge) {
        const size_t n = depthStops + ceilingStops;
        if (!n) return;
        splitShort_ += n;
        ++splitShortItems_;
        std::printf("  split short %s: %zu triangles left longer than %.1f (%d halvings: %zu, %zu-vertex ceiling: %zu; "
                    "longest %.1f)\n",
                    what.c_str(), n, maxEdge, TriangleSplitter::kMaxDepth, depthStops, TriangleSplitter::kMaxVertices,
                    ceilingStops, longest);
    }
    size_t splitShort() const { return splitShort_; }
    size_t splitShortItems() const { return splitShortItems_; }

private:
    size_t splitShort_ = 0, splitShortItems_ = 0;  // split-short counters
    size_t emitterVolumes_ = 0;  // particle emitter volumes not baked (upstream hides them)
    static uint32_t levelZeroIndices(const pipeline::M2Model& m) {
        uint32_t n = 0;
        for (const auto& b : m.batches)
            if (b.submeshLevel == 0) n += b.indexCount;
        return n;
    }

    // collect (forest LOD): the accepted batches before the split, for LOD 2
    // and the impostor pictures.
    void addBatches(const pipeline::M2Model& m, int lod, bool foliage, WmdlBuilder& o, TriangleSplitter& sp,
                    std::vector<ForestBatch>* collect = nullptr) {
        for (const auto& b : m.batches) {
            if (b.submeshLevel != 0 || b.indexCount < 3) continue;
            if (b.indexStart + b.indexCount > m.indices.size()) continue;
            uint16_t blendMode = 0, matFlags = 0;
            if (b.materialIndex < m.materials.size()) {
                blendMode = m.materials[b.materialIndex].blendMode;
                matFlags = m.materials[b.materialIndex].flags;
            }
            fmt::ModelBatch ob{};
            ob.texture = fmt::kNoTexture;
            bool texAlpha = false;
            uint32_t texFlags = 0;
            std::string texName;
            if (b.textureIndex < m.textureLookup.size() && m.textureLookup[b.textureIndex] < m.textures.size()) {
                const auto& t = m.textures[m.textureLookup[b.textureIndex]];
                std::string name = t.filename.c_str();  // stop at the first NUL
                texFlags = t.flags;
                if (t.type == 0 && !name.empty()) {
                    // An opaque material that stays opaque (not foliage made cutout) ignores the alpha.
                    const bool opaque = mapBlend(blendMode) == fmt::kBlendOpaque;
                    const int id = tex_.get(name, texAlpha, opaque, foliage);
                    if (id < 0) continue;  // upstream: batch opacity 0
                    ob.texture = static_cast<uint16_t>(id);
                    texName = name;
                }
            }
            ob.blend = mapBlend(blendMode);
            // Upstream forces foliage with alpha textures to cutout.
            if (foliage && texAlpha && (ob.blend == fmt::kBlendOpaque || ob.blend == fmt::kBlendAlpha))
                ob.blend = fmt::kBlendAlphaKey;
            if (ob.blend == fmt::kBlendAlphaKey && !texAlpha) ob.blend = fmt::kBlendOpaque;
            ob.flags = static_cast<uint8_t>(((texFlags & 1) ? fmt::kBatchWrapU : 0) |
                                            ((texFlags & 2) ? fmt::kBatchWrapV : 0) |
                                            ((matFlags & 1) ? fmt::kBatchUnlit : 0));
            if (collect) {
                ForestBatch fb;
                fb.tris.assign(m.indices.begin() + b.indexStart, m.indices.begin() + b.indexStart + b.indexCount / 3 * 3);
                fb.ob = ob;
                fb.texName = texName;
                fb.uvSet = b.textureUnit == 1 ? 1 : 0;
                collect->push_back(std::move(fb));
            }
            std::vector<uint16_t> idx;
            for (uint32_t k = 0; k + 2 < b.indexCount; k += 3)
                sp.split(m.indices[b.indexStart + k], m.indices[b.indexStart + k + 1], m.indices[b.indexStart + k + 2],
                         idx);
            if (!o.add(idx, 0, static_cast<uint32_t>(idx.size()), lod, b.textureUnit == 1 ? 1 : 0, ob)) return;
        }
    }

    void bake(const std::string& path, BakedModel& bm) {
        std::vector<uint8_t> m2;
        pipeline::M2Model full;
        if (!readFile(root_ + "/" + path, m2) || !loadM2(m2, root_ + "/" + pipeline::skinPathForM2(path), full)) {
            // An emitter-only model (no vertices - smoke, splashes, fire) has no WMDL, its particles count.
            if (gParticleBank && !m2.empty() && full.vertices.empty()) {
                const auto cls = rendering::classifyM2Model(path, glm::vec3(0.0f), glm::vec3(0.0f), 0, full.particleEmitters.size());
                bm.effect = gParticleBank->bakeModel(path, m2, full, cls);
            }
            if (bm.effect < 0) std::fprintf(stderr, "  model %s: unreadable\n", path.c_str());
            ++skipped_;
            return;
        }
        glm::vec3 bmin(1e9f), bmax(-1e9f);
        for (const auto& v : full.vertices) {
            bmin = glm::min(bmin, v.position);
            bmax = glm::max(bmax, v.position);
        }
        const auto cls = rendering::classifyM2Model(path, bmin, bmax, full.vertices.size(), full.particleEmitters.size());
        if (gParticleBank) bm.effect = gParticleBank->bakeModel(path, m2, full, cls);  // particles
        // Candle flames without candles were seen in a village inn: upstream's guess "3+ emitters, <= 200
        // vertices = a spell effect" (m2_model_classifier.cpp) also catches solid props with flames -
        // a candelabra model (167 vertices, 3 emitters, 307 placements on map 0) - and upstream draws
        // only their flames; the original client draws every placed model's mesh. A placed "spell effect" keeps its WMDL
        // unless its particle effect already carries the mesh (WSFX groups, kPtcEffectMesh): drawn once either way.
        const bool meshInEffect = gParticleBank && gParticleBank->effectHasMesh(bm.effect);
        // PSP (hardware test: a huge black box behind the player in a city): upstream
        // M2Renderer::loadModel hides a particle emitter's volume - a box mesh (<= 24 vertices, <= 36 indices) over
        // 5 yd on a model with emitters is never drawn (isInvisibleTrap). A floating-embers model (84 x 84 x 31 yd,
        // a placeholder texture, opaque) and a smoke-emitter model were baked as solid WMDL boxes. Its
        // emitters stay (bm.effect is placed without a WMDL, as an emitter-only model).
        const glm::vec3 size = bmax - bmin;
        const bool emitterVolume = !cls.isInvisibleTrap && !cls.isGroundDetail && full.vertices.size() <= 24 &&
                                   full.indices.size() <= 36 && !full.particleEmitters.empty() &&
                                   std::max({size.x, size.y, size.z}) > 5.0f;
        if (emitterVolume) {
            ++emitterVolumes_;
            std::printf("  emitter volume hidden %s: %zu verts, %.1f x %.1f x %.1f yd\n", path.c_str(),
                        full.vertices.size(), static_cast<double>(size.x), static_cast<double>(size.y),
                        static_cast<double>(size.z));
        }
        if (cls.isGroundDetail || cls.isInvisibleTrap || (cls.isSpellEffect && (meshInEffect || !gParticleBank)) || cls.isFireflyEffect ||
            cls.isSkyBird || emitterVolume) {
            ++skipped_;
            return;
        }
        const bool foliage = cls.isFoliageLike || cls.isGroundDetail;

        WmdlBuilder o;
        // Big triangles next to the camera vanish at the screen corners (the
        // GE drops a primitive whose vertex leaves its 4096 px space; hardware
        // tests, tree canopies tens of yards wide). The split mesh is kept
        // as a *near* LOD 0 and the original as LOD 1 (kModelNearSplit): only
        // the few instances the camera is close to pay for the extra triangles.
        std::vector<pipeline::M2Vertex> verts = full.vertices;
        std::vector<glm::vec3> unusedLit(verts.size(), glm::vec3(1.0f));
        TriangleSplitter spNear(verts, unusedLit, maxEdge_);
        TriangleSplitter spNone(verts, unusedLit, 1e9f);
        // Forest LOD: trees keep their batches for LOD 2 / the impostor.
        const bool tree = foliage && !cls.isGroundDetail;
        std::vector<ForestBatch> fullBatches, lowBatches;
        addBatches(full, 0, foliage, o, spNear, tree ? &fullBatches : nullptr);
        // The near split stopped short?
        noteSplitShort("model " + path, spNear.depthStops(), spNear.ceilingStops(), spNear.longestLeft(), maxEdge_);
        const uint32_t fullIdx = levelZeroIndices(full);
        bool nearSplit = false;
        if (o.triangles(0) * 10 > static_cast<size_t>(fullIdx / 3) * 11) {
            addBatches(full, 1, foliage, o, spNone);
            nearSplit = true;
        } else {
            const std::string base = path.substr(0, path.size() - 3);
            for (int s = 3; s >= 1; --s) {
                char suffix[16];
                std::snprintf(suffix, sizeof(suffix), "%02d.skin", s);
                pipeline::M2Model low;
                if (!loadM2(m2, root_ + "/" + base + suffix, low)) continue;
                if (levelZeroIndices(low) * 10 > fullIdx * 6) break;  // not worth a LOD
                addBatches(low, 1, foliage, o, spNone, tree ? &lowBatches : nullptr);
                break;
            }
        }
        if (o.empty()) {
            ++skipped_;
            return;
        }
        // PSP (forest LOD): far LOD 2 from the lowest mesh we have,
        // an impostor from the original LOD 0. Not upstream (the desktop GPU
        // draws every tree in full).
        size_t srcTris = 0;
        for (const auto& fb : fullBatches) srcTris += fb.tris.size() / 3;
        size_t lod2Tris = 0;
        const WmdlBuilder withoutLod2 = o;  // fallback: LOD 2 must not cost the model its strips
        if (tree) lod2Tris = buildLod2(lowBatches.empty() ? fullBatches : lowBatches, verts, forest_, o, lod2_);
        std::vector<uint8_t> impBlob;
        if (tree && forest_.impostors && srcTris >= forest_.impostorMinTris) {
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (const auto& fb : fullBatches)
                for (uint32_t i : fb.tris) {
                    lo = glm::min(lo, verts[i].position);
                    hi = glm::max(hi, verts[i].position);
                }
            ImpostorResult ir;
            if (glm::length(hi - lo) * 0.5f >= forest_.impostorMinRadius &&
                impostor_.bake(fullBatches, verts, forest_, ir)) {
                const int texId = tex_.add("impostor:" + path, ir.atlas, std::max(ir.atlas.width, ir.atlas.height));
                ir.h.texture = static_cast<uint16_t>(texId);
                impBlob.assign(reinterpret_cast<const uint8_t*>(&ir.h), reinterpret_cast<const uint8_t*>(&ir.h + 1));
            }
        }
        WmdlInfo info;
        const uint16_t flags = static_cast<uint16_t>((foliage ? fmt::kModelFoliage : 0) |
                                                     (cls.isGroundDetail ? fmt::kModelGroundDetail : 0) |
                                                     (nearSplit ? fmt::kModelNearSplit : 0));
        // Collision: the M2 bounding triangles (upstream M2Renderer), all
        // blocking; ground clutter has none.
        std::vector<uint8_t> col;
        if (!cls.isGroundDetail && !full.collisionVertices.empty() && full.collisionIndices.size() >= 3) {
            std::vector<fmt::CollisionTri> tris;
            for (size_t k = 0; k + 2 < full.collisionIndices.size(); k += 3) {
                const uint16_t a = full.collisionIndices[k], b = full.collisionIndices[k + 1],
                               c = full.collisionIndices[k + 2];
                if (a >= full.collisionVertices.size() || b >= full.collisionVertices.size() ||
                    c >= full.collisionVertices.size())
                    continue;
                tris.push_back(fmt::CollisionTri{a, b, c, static_cast<uint8_t>(wallsBlock_ ? fmt::kColBlocksWalls : 0), 0});
            }
            col = buildCollision(full.collisionVertices, tris);
        }
        std::vector<uint8_t> blob = o.build(verts, flags, false, info);
        if (lod2Tris && !info.strips) {
            WmdlInfo plain;
            std::vector<uint8_t> alt = withoutLod2.build(verts, flags, false, plain);
            if (plain.strips) {
                std::printf("    lod2 dropped: over 65535 strip vertices\n");
                blob.swap(alt);
                info = plain;
                lod2Tris = 0;
            }
        }
        const int effect = bm.effect;  // particles
        bm = add(std::move(blob), info, path + (foliage ? " (foliage)" : ""), std::move(col));
        bm.effect = effect;  // particles
        if (lod2Tris)
            std::printf("    lod2 %zu tris (%.0f%% of %zu)\n", lod2Tris,
                        100.0 * static_cast<double>(lod2Tris) / static_cast<double>(std::max<size_t>(1, srcTris)), srcTris);
        if (!impBlob.empty() && bm.id >= 0) {
            fmt::ImpostorHeader ih;
            std::memcpy(&ih, impBlob.data(), sizeof(ih));
            ++impostorCount_;
            std::printf("    impostor i_%d -> x_%u\n", bm.id, static_cast<unsigned>(ih.texture));
            out_.push_back({"i_" + std::to_string(bm.id), kKindModel, std::move(impBlob)});
        }
    }

public:
    // Registers a finished WMDL (also used for WMO groups).
    BakedModel add(std::vector<uint8_t> blob, const WmdlInfo& info, const std::string& label,
                   std::vector<uint8_t> collision = {}) {
        BakedModel bm;
        bm.id = static_cast<int>(count_++);
        bm.center = info.center;
        bm.radius = info.radius;
        bytes_ += blob.size();
        tris_ += info.triangles;
        std::printf("  m_%d %s: %zu verts, %u tris, %zu+%zu batches\n", bm.id, label.c_str(), info.vertices,
                    info.triangles, info.batches[0], info.batches[1]);
        out_.push_back({"m_" + std::to_string(bm.id), kKindModel, std::move(blob)});
        if (!collision.empty()) {
            colBytes_ += collision.size();
            ++colCount_;
            out_.push_back({"k_" + std::to_string(bm.id), kKindModel, std::move(collision)});
        }
        return bm;
    }

private:

    std::string root_;
    TextureBank& tex_;
    std::vector<PakItem>& out_;
    std::map<std::string, BakedModel> models_;
    float maxEdge_;
    ForestOptions forest_;
    ImpostorBaker impostor_;
    Lod2Stats lod2_;
    size_t impostorCount_ = 0;
    size_t count_ = 0, skipped_ = 0, bytes_ = 0, tris_ = 0, colCount_ = 0, colBytes_ = 0;
    bool wallsBlock_ = true;  // global-WMO maps
};

// WMO buildings (M8): every group becomes an ordinary WMDL model, so the PSP
// draws, culls and grids them like doodads. Rules from upstream WMORenderer /
// terrain_manager (docs/PSP_PORT_ARCHITECTURE.md, M8):
//   - texture = material texture1 (MOTX offset via textureOffsetToIndex), then
//     texture2/3; blend 0 opaque, 1 alpha test, >= 2 alpha; flag 0x01 unlit;
//     everything two-sided; groups with flag 0x4000000 are skipped;
//   - light baked per vertex: interior groups (0x2000) max(MOCV, ambient,
//     0.15), exterior (0.5 + 0.5 sun) * max(MOCV, 0.25);
//   - doodads: set 0 + the placement's set, local = T(pos) * R(quat) * S.
struct BakedWmo {
    bool ok = false;
    std::vector<BakedModel> groups;
    std::vector<std::pair<uint32_t, uint32_t>> sets;  // (start, count) into doodads
    struct Doodad {
        BakedModel model;
        glm::mat4 local;
    };
    std::vector<Doodad> doodads;
    const BigWmoBaked* big = nullptr;  // over the budget, baked as its own pak (ok stays false)
};

class WmoBank {
public:
    WmoBank(std::string root, TextureBank& tex, ModelBank& models, size_t maxTris, float maxEdge)
        : root_(std::move(root)), tex_(tex), models_(models), maxTris_(maxTris), maxEdge_(maxEdge) {}

    const BakedWmo& get(const std::string& gamePath) {
        const std::string key = lower(gamePath);
        auto it = wmos_.find(key);
        if (it != wmos_.end()) return it->second;
        BakedWmo w;
        bake(gamePath, key, w);
        return wmos_.emplace(key, std::move(w)).first->second;
    }
    size_t count() const { return wmos_.size(); }
    size_t overBudget() const { return overBudget_; }
    // A global WMO is the whole map - its own triangle budget (0 = none).
    void setMaxTris(size_t n) { maxTris_ = n ? n : static_cast<size_t>(-1); }
    // A WMO over the budget becomes <outDir>/bigwmo/<map>_<stem>.pak (wmo_chunk_bake.hpp) instead
    // of being skipped; outDir empty = skipped as before.
    void setBigWmo(std::string outDir, std::string map, uint32_t stamp, BigWmoOptions opt, int texMax) {
        bigDir_ = std::move(outDir);
        bigMap_ = lower(std::move(map));
        bigStamp_ = stamp;
        bigOpt_ = opt;
        bigTexMax_ = texMax;
    }
    size_t bigCount() const { return bigs_.size(); }
    size_t bigFailed() const { return bigFailed_; }
    // A dungeon's global WMO over this many source triangles (the drawn groups, before the near split)
    // goes the big-WMO way too, although the map has no triangle budget (setMaxTris(0)); 0 = never (map-transfer).
    void setBigSourceLimit(size_t n) { bigSrcLimit_ = n; }

private:
    int textureOf(const pipeline::WMOModel& m, const pipeline::WMOMaterial& mat, bool& alpha) {
        for (uint32_t off : {mat.texture1, mat.texture2, mat.texture3}) {
            uint32_t idx = UINT32_MAX;
            auto it = m.textureOffsetToIndex.find(off);
            if (it != m.textureOffsetToIndex.end()) idx = it->second;
            else if (off < m.textures.size()) idx = off;
            if (idx >= m.textures.size() || m.textures[idx].empty()) continue;
            // Blend 0 = opaque, the alpha is a specular mask (black wood was seen in an inn) - ignored.
            const int id = tex_.get(m.textures[idx], alpha, mat.blendMode == 0);
            if (id >= 0) return id;
        }
        return -1;
    }

    void bake(const std::string& gamePath, const std::string& key, BakedWmo& w) {
        std::vector<uint8_t> bytes;
        if (!readFile(root_ + "/" + key, bytes)) {
            std::fprintf(stderr, "  wmo %s: missing\n", key.c_str());
            return;
        }
        pipeline::WMOModel m = pipeline::WMOLoader::load(bytes);
        for (uint32_t gi = 0; gi < m.groups.size(); ++gi) {
            for (const std::string& cand : pipeline::wmoGroupCandidates(gamePath, gi)) {
                std::vector<uint8_t> gb;
                if (readFile(root_ + "/" + lower(cand), gb)) {
                    pipeline::WMOLoader::loadGroup(gb, m, gi);
                    break;
                }
            }
        }
        // A WMO over the budget is counted first (source triangles of the drawn groups) and,
        // with a big-WMO directory, baked whole as its own pak - the split below would only be thrown away.
        if (!bigDir_.empty()) {
            size_t src = 0;
            for (const auto& g : m.groups)
                if (!(g.flags & 0x4000000u)) src += g.indices.size() / 3;
            if (src > maxTris_ || (bigSrcLimit_ && src > bigSrcLimit_)) {  // + the global WMO's limit
                bakeBig(gamePath, key, m, w, src);
                return;
            }
        }
        size_t tris = 0, skipped = 0;
        std::vector<std::tuple<std::vector<uint8_t>, WmdlInfo, std::string>> built;
        std::vector<std::vector<uint8_t>> builtCol;
        struct SplitShort {  // TriangleSplitter stopped short in a group (logged if the WMO stays here)
            uint32_t group;
            size_t depth, ceiling;
            float longest;
        };
        std::vector<SplitShort> splitShort;
        for (uint32_t gi = 0; gi < m.groups.size(); ++gi) {
            const pipeline::WMOGroup& g = m.groups[gi];
            if (g.vertices.empty() || g.indices.empty() || g.batches.empty()) continue;
            if (g.flags & 0x4000000u) {  // upstream: antiportal
                ++skipped;
                continue;
            }
            const bool interior = (g.flags & 0x2000u) != 0;
            std::vector<pipeline::M2Vertex> verts(g.vertices.size());
            std::vector<glm::vec3> lit(g.vertices.size());
            for (size_t i = 0; i < g.vertices.size(); ++i) {
                const auto& v = g.vertices[i];
                pipeline::M2Vertex& o = verts[i];
                o = pipeline::M2Vertex{};
                o.position = v.position;
                o.normal = v.normal;
                o.texCoords[0] = o.texCoords[1] = v.texCoord;
                lit[i] = wmoVertexLight(v, interior, m.ambientColor);  // shared with the big-WMO bake
            }
            WmdlBuilder o;
            // Near LOD 0 split to maxEdge_ (the GE drops a triangle whose vertex
            // projects past its 4096 px space; safe when edges <= ~4x the near
            // plane), LOD 1 the original mesh - used beyond 12 yd of the group,
            // where no vertex is close enough to the eye (kModelNearSplit).
            TriangleSplitter splitter(verts, lit, maxEdge_);
            TriangleSplitter noSplit(verts, lit, 1e9f);
            std::vector<uint16_t> splitIdx, plainIdx;
            size_t plainTris = 0;
            std::vector<std::pair<fmt::ModelBatch, std::pair<uint32_t, uint32_t>>> plainBatches;
            for (const auto& b : g.batches) {
                if (b.indexCount < 3 || b.startIndex + b.indexCount > g.indices.size()) continue;
                fmt::ModelBatch ob{};
                ob.texture = fmt::kNoTexture;
                uint32_t matFlags = 0, blend = 0;
                bool alpha = false;
                if (b.materialId < m.materials.size()) {
                    const auto& mat = m.materials[b.materialId];
                    matFlags = mat.flags;
                    blend = mat.blendMode;
                    const int id = textureOf(m, mat, alpha);
                    if (id >= 0) ob.texture = static_cast<uint16_t>(id);
                }
                ob.blend = blend == 0 ? fmt::kBlendOpaque : blend == 1 ? fmt::kBlendAlphaKey : fmt::kBlendAlpha;
                if (ob.blend == fmt::kBlendAlphaKey && !alpha) ob.blend = fmt::kBlendOpaque;
                ob.flags = static_cast<uint8_t>(((matFlags & 0x40u) ? 0 : fmt::kBatchWrapU) |
                                                ((matFlags & 0x80u) ? 0 : fmt::kBatchWrapV) |
                                                ((matFlags & 0x01u) ? fmt::kBatchUnlit : 0));
                const uint32_t start = static_cast<uint32_t>(splitIdx.size());
                const uint32_t pstart = static_cast<uint32_t>(plainIdx.size());
                for (uint32_t k = 0; k + 2 < b.indexCount; k += 3) {
                    const uint16_t ia = g.indices[b.startIndex + k], ib = g.indices[b.startIndex + k + 1],
                                   ic = g.indices[b.startIndex + k + 2];
                    splitter.split(ia, ib, ic, splitIdx);
                    noSplit.split(ia, ib, ic, plainIdx);
                }
                plainTris += (plainIdx.size() - pstart) / 3;
                if (!o.add(splitIdx, start, static_cast<uint32_t>(splitIdx.size()) - start, 0, 0, ob)) break;
                plainBatches.push_back({ob, {pstart, static_cast<uint32_t>(plainIdx.size()) - pstart}});
            }
            if (splitter.depthStops() + splitter.ceilingStops())  // stopped short
                splitShort.push_back({gi, splitter.depthStops(), splitter.ceilingStops(), splitter.longestLeft()});
            bool nearSplit = false;
            if (o.triangles(0) * 10 > plainTris * 11) {
                nearSplit = true;
                for (auto& [ob, range] : plainBatches)
                    if (!o.add(plainIdx, range.first, range.second, 1, 0, ob)) break;
            }
            if (o.empty()) continue;
            WmdlInfo info;
            std::vector<uint8_t> blob = o.build(
                verts, static_cast<uint16_t>(fmt::kModelBuilding | (nearSplit ? fmt::kModelNearSplit : 0)), false, info,
                &lit);
            char label[32];
            std::snprintf(label, sizeof(label), " group %u%s", gi, interior ? " (in)" : "");
            tris += info.triangles;
            // Collision: the group's own triangles. Walls block per upstream
            // WMORenderer::checkWallCollision: MOPY 0x08, or 0x20 without 0x04;
            // a group with no such triangle blocks with all of them.
            std::vector<glm::vec3> cverts(g.vertices.size());
            for (size_t i = 0; i < g.vertices.size(); ++i) cverts[i] = g.vertices[i].position;
            std::vector<fmt::CollisionTri> ctris;
            bool anyBlocking = false;
            for (size_t t = 0; t * 3 + 2 < g.indices.size(); ++t) {
                const uint8_t f = t < g.triFlags.size() ? g.triFlags[t] : 0;
                const bool blocks = (f & 0x08) || ((f & 0x20) && !(f & 0x04));
                anyBlocking = anyBlocking || blocks;
                ctris.push_back(fmt::CollisionTri{g.indices[t * 3], g.indices[t * 3 + 1], g.indices[t * 3 + 2],
                                                  static_cast<uint8_t>(blocks ? fmt::kColBlocksWalls : 0), 0});
            }
            if (!anyBlocking)
                for (auto& ct : ctris) ct.flags |= fmt::kColBlocksWalls;
            built.emplace_back(std::move(blob), info, key + label);
            builtCol.push_back(buildCollision(cverts, ctris));
        }
        // PSP budget: a city (a capital city: 667k triangles, 6k doodads) does not
        // fit next to the zone around it; it needs LOD groups / streaming.
        if (tris > maxTris_ && !bigDir_.empty()) {  // over only after the near split
            size_t src = 0;
            for (const auto& g : m.groups)
                if (!(g.flags & 0x4000000u)) src += g.indices.size() / 3;
            bakeBig(gamePath, key, m, w, src);
            return;
        }
        if (tris > maxTris_) {
            std::printf("wmo %s: %zu tris over budget %zu, skipped\n", key.c_str(), tris, maxTris_);
            ++overBudget_;
            return;
        }
        for (size_t i = 0; i < built.size(); ++i) {
            auto& [blob, info, label] = built[i];
            w.groups.push_back(models_.add(std::move(blob), info, label, std::move(builtCol[i])));
        }
        for (const SplitShort& ss : splitShort)  // logged per group
            models_.noteSplitShort("wmo " + key + " group " + std::to_string(ss.group), ss.depth, ss.ceiling, ss.longest,
                                   maxEdge_);
        for (const auto& set : m.doodadSets) w.sets.emplace_back(set.startIndex, set.count);
        for (const auto& d : m.doodads) {
            BakedWmo::Doodad dd;
            auto nameIt = m.doodadNames.find(d.nameIndex);
            if (nameIt != m.doodadNames.end()) dd.model = models_.get(nameIt->second);
            const glm::quat q(d.rotation.w, d.rotation.x, d.rotation.y, d.rotation.z);
            dd.local = glm::scale(glm::translate(glm::mat4(1.0f), d.position) * glm::mat4_cast(q), glm::vec3(d.scale));
            w.doodads.push_back(dd);
        }
        w.ok = !w.groups.empty();
        std::printf("wmo %s: %zu groups (%zu skipped), %zu tris, %zu doodads in %zu sets\n", key.c_str(),
                    w.groups.size(), skipped, tris, w.doodads.size(), w.sets.size());
    }

    // The whole WMO as its own pak; its doodads' models go to this map's ModelBank.
    void bakeBig(const std::string& gamePath, const std::string& key, const pipeline::WMOModel& m, BakedWmo& w,
                 size_t srcTris) {
        std::string stem = key.substr(key.find_last_of("/\\") + 1);
        if (stem.size() > 4) stem.resize(stem.size() - 4);
        std::string rel = "bigwmo/" + bigMap_ + "_" + stem + ".pak";
        bool clash = false;
        for (const auto& [k, b] : bigs_) clash = clash || b.pakRel == rel;
        // Two WMOs of one name on a map (other folders), or a name too long for BigWmoPlacement::pak (43 chars):
        // the stem shortened and a hash of the whole path.
        if (clash || rel.size() > sizeof(fmt::BigWmoPlacement::pak) - 1) {
            char h[16];
            uint32_t f = 2166136261u;
            for (char c : key) f = (f ^ static_cast<uint8_t>(c)) * 16777619u;
            std::snprintf(h, sizeof(h), "_%08x", f);
            const size_t cap = sizeof(fmt::BigWmoPlacement::pak) - 1;
            // A map name too long for even the hash (a dungeon map name of 23 chars: the name was
            // cut at 43 by snprintf - "...<hash>.pa", a pak --verify could not find) - its name cut first.
            std::string mapPart = bigMap_;
            if (7 + mapPart.size() + 1 + 9 + 4 > cap) mapPart.resize(cap - (7 + 1 + 9 + 4));
            const size_t fixed = 7 + mapPart.size() + 1 + 9 + 4;
            const size_t room = fixed < cap ? cap - fixed : 0;
            rel = "bigwmo/" + mapPart + "_" + stem.substr(0, room) + h + ".pak";
        }
        std::filesystem::create_directories(bigDir_ + "/bigwmo");
        std::printf("wmo %s: %zu tris over budget %zu -> big WMO %s\n", key.c_str(), srcTris, maxTris_, rel.c_str());
        BigWmoBaker baker(root_, bigTexMax_, bigOpt_);
        BigWmoBaked out;
        auto models = [&](const std::string& name) {
            const BakedModel& bm = models_.get(name);
            BigWmoModelRef r;
            r.id = bm.id;
            r.center = bm.center;
            r.radius = bm.radius;
            r.effect = bm.effect;
            return r;
        };
        if (!baker.bake(gamePath, m, bigDir_, rel, bigStamp_, models, out)) {
            std::fprintf(stderr, "wmo %s: big WMO bake failed, skipped\n", key.c_str());
            ++bigFailed_;
            ++overBudget_;
            return;
        }
        auto it = bigs_.emplace(key, std::move(out)).first;
        w.big = &it->second;
    }

    std::string root_;
    TextureBank& tex_;
    ModelBank& models_;
    size_t maxTris_;
    float maxEdge_;
    size_t overBudget_ = 0;
    std::map<std::string, BakedWmo> wmos_;
    std::string bigDir_, bigMap_;  // big WMOs
    uint32_t bigStamp_ = 0;
    BigWmoOptions bigOpt_;
    int bigTexMax_ = 128;
    size_t bigFailed_ = 0;
    std::map<std::string, BigWmoBaked> bigs_;
    size_t bigSrcLimit_ = 0;  // dungeons
};

// The big WMOs this bake placed ("wbig" of the doodads pak), one per MODF uniqueId.
struct BigWmoPlacements {
    uint32_t stamp = 0;
    std::vector<fmt::BigWmoPlacement> list;
    std::vector<uint32_t> uids;
};
BigWmoPlacements* gBigPlacements = nullptr;

// Upstream terrain_manager placementEuler(): verified against all MODF boxes.
glm::mat3 placementAxes(const float rotDeg[3], float scale) {
    constexpr float kDeg = core::coords::PI / 180.0f;
    const glm::vec3 e(rotDeg[2] * kDeg, rotDeg[0] * kDeg, (rotDeg[1] + 180.0f) * kDeg);
    glm::mat4 m(1.0f);
    m = glm::rotate(m, e.z, glm::vec3(0, 0, 1));
    m = glm::rotate(m, e.y, glm::vec3(0, 1, 0));
    m = glm::rotate(m, e.x, glm::vec3(1, 0, 0));
    m = glm::scale(m, glm::vec3(scale));
    return glm::mat3(m);
}

// A placed model: world = matrix * model space.
fmt::DoodadPlacement place(uint32_t uniqueId, const BakedModel& bm, const glm::mat4& world) {
    const glm::mat3 axes(world);
    const glm::vec3 pos(world[3]);
    const glm::vec3 c = pos + axes * bm.center;
    const float scale = std::max({glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2])});
    fmt::DoodadPlacement d{};
    d.uniqueId = uniqueId;
    d.model = static_cast<uint16_t>(bm.id);
    for (int a = 0; a < 3; ++a) d.position[a] = pos[a];
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row) d.axes[col * 3 + row] = axes[col][row];
    d.sphere[0] = c.x;
    d.sphere[1] = c.y;
    d.sphere[2] = c.z;
    d.sphere[3] = bm.radius * scale;
    return d;
}

// Ids for WMO groups and WMO doodads: the WMO's uniqueId mixed with the part,
// high bit set so they never meet an MDDF uniqueId (dedupe across tiles).
uint32_t partId(uint32_t wmoUniqueId, uint32_t part) {
    uint32_t h = 2166136261u;
    for (uint32_t v : {wmoUniqueId, part})
        for (int b = 0; b < 4; ++b) h = (h ^ ((v >> (b * 8)) & 0xFFu)) * 16777619u;
    return h | 0x80000000u;
}

bool bakeTile(const std::string& root, const std::string& map, int tx, int ty, ModelBank& models, WmoBank& wmos,
              std::vector<uint8_t>& blob, size_t& instances) {
    const std::string adtPath = root + "/world/maps/" + lower(map) + "/" + lower(map) + "_" +
                                std::to_string(tx) + "_" + std::to_string(ty) + ".adt";
    std::vector<uint8_t> bytes;
    if (!readFile(adtPath, bytes)) return false;
    const pipeline::ADTTerrain terrain = pipeline::ADTLoader::load(bytes);

    std::vector<fmt::DoodadPlacement> out;
    for (const auto& p : terrain.doodadPlacements) {
        if (p.nameId >= terrain.doodadNames.size()) continue;
        const BakedModel& bm = models.get(terrain.doodadNames[p.nameId]);
        if (bm.id < 0 && bm.effect < 0) continue;  // particles: an emitter-only model is placed too
        const float scale = p.scale / 1024.0f;
        const glm::mat3 axes = placementAxes(p.rotation, scale);
        const glm::vec3 pos = core::coords::adtToWorld(p.position[0], p.position[1], p.position[2]);
        if (gParticleBank) gParticleBank->place(tx, ty, p.uniqueId, bm.effect, axes, pos);  // particles
        if (bm.id < 0) continue;                                                            // particles
        const glm::vec3 c = pos + axes * bm.center;
        fmt::DoodadPlacement d{};
        d.uniqueId = p.uniqueId;
        d.model = static_cast<uint16_t>(bm.id);
        for (int a = 0; a < 3; ++a) d.position[a] = pos[a];
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row) d.axes[col * 3 + row] = axes[col][row];
        d.sphere[0] = c.x;
        d.sphere[1] = c.y;
        d.sphere[2] = c.z;
        d.sphere[3] = bm.radius * scale;
        out.push_back(d);
    }
    for (const auto& p : terrain.wmoPlacements) {
        if (p.nameId >= terrain.wmoNames.size()) continue;
        const BakedWmo& w = wmos.get(terrain.wmoNames[p.nameId]);
        if (!w.ok && !w.big) continue;
        glm::mat4 world(placementAxes(p.rotation, (p.scale ? p.scale : 1024) / 1024.0f));
        world[3] = glm::vec4(core::coords::adtToWorld(p.position[0], p.position[1], p.position[2]), 1.0f);
        if (w.big) {  // big WMOs: one "wbig" placement per uniqueId; its doodads' particles on this tile
            if (!gBigPlacements ||
                std::find(gBigPlacements->uids.begin(), gBigPlacements->uids.end(), p.uniqueId) != gBigPlacements->uids.end())
                continue;
            fmt::BigWmoPlacement bp{};
            bp.uniqueId = p.uniqueId;
            bp.doodadSet = p.doodadSet;
            bp.tileX = static_cast<int16_t>(tx);
            bp.tileY = static_cast<int16_t>(ty);
            std::snprintf(bp.pak, sizeof(bp.pak), "%s", w.big->pakRel.c_str());
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 3; ++r) bp.matrix[c * 3 + r] = world[c][r];
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (int k = 0; k < 8; ++k) {
                const glm::vec3 corner((k & 1) ? w.big->bmax.x : w.big->bmin.x, (k & 2) ? w.big->bmax.y : w.big->bmin.y,
                                       (k & 4) ? w.big->bmax.z : w.big->bmin.z);
                const glm::vec3 wc = glm::vec3(world * glm::vec4(corner, 1.0f));
                lo = glm::min(lo, wc);
                hi = glm::max(hi, wc);
            }
            for (int a = 0; a < 3; ++a) {
                bp.bmin[a] = lo[a];
                bp.bmax[a] = hi[a];
            }
            gBigPlacements->list.push_back(bp);
            gBigPlacements->uids.push_back(p.uniqueId);
            if (gParticleBank)
                for (const auto& d : w.big->doodads) {
                    if (d.model.effect < 0 || !(d.set == 0 || d.set == p.doodadSet)) continue;
                    const glm::mat4 dw = world * d.local;
                    gParticleBank->place(tx, ty, partId(p.uniqueId, 0x20000u + d.index), d.model.effect, glm::mat3(dw),
                                         glm::vec3(dw[3]));
                }
            std::printf("tile %d_%d: big wmo %s uid %u at (%.1f, %.1f, %.1f)\n", tx, ty, bp.pak, p.uniqueId, world[3].x,
                        world[3].y, world[3].z);
            continue;
        }
        for (size_t g = 0; g < w.groups.size(); ++g)
            out.push_back(place(partId(p.uniqueId, 0x10000u + static_cast<uint32_t>(g)), w.groups[g], world));
        std::vector<bool> done(w.doodads.size(), false);
        for (uint32_t set : {0u, static_cast<uint32_t>(p.doodadSet)}) {
            if (set >= w.sets.size()) continue;
            const auto [start, count] = w.sets[set];
            for (uint32_t i = start; i < start + count && i < w.doodads.size(); ++i) {
                const BakedModel& dm = w.doodads[i].model;
                if (done[i] || (dm.id < 0 && dm.effect < 0)) continue;  // particles: emitter-only too
                done[i] = true;
                const glm::mat4 dw = world * w.doodads[i].local;
                if (gParticleBank)  // particles
                    gParticleBank->place(tx, ty, partId(p.uniqueId, 0x20000u + i), dm.effect, glm::mat3(dw), glm::vec3(dw[3]));
                if (dm.id < 0) continue;
                out.push_back(place(partId(p.uniqueId, 0x20000u + i), dm, dw));
            }
        }
    }
    fmt::DoodadTileHeader h{};
    std::memcpy(h.magic, "WDOD", 4);
    h.version = fmt::kModelFormatVersion;
    h.count = static_cast<uint32_t>(out.size());
    blob.assign(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(out.data()),
                reinterpret_cast<const uint8_t*>(out.data() + out.size()));
    instances = out.size();
    return true;
}

// Every GameObjectDisplayInfo M2 (upstream EntitySpawner::buildGameObjectDisplayLookups: ID, ModelName;
// .mdx / .mdl name the .m2) through the same ModelBank as the doodads, and the "gidx" index of the pak.
bool bakeGameObjects(const std::string& root, ModelBank& models, PakItem& index) {
    std::vector<uint8_t> bytes;
    pipeline::DBCFile godi;
    if (!readFile(root + "/dbfilesclient/gameobjectdisplayinfo.dbc", bytes) || !godi.load(bytes)) {
        std::fprintf(stderr, "missing dbfilesclient/gameobjectdisplayinfo.dbc\n");
        return false;
    }
    std::vector<fmt::GameObjectDisplayRec> recs;
    size_t wmo = 0, noModel = 0, failed = 0;
    for (uint32_t r = 0; r < godi.getRecordCount(); ++r) {
        const uint32_t id = godi.getUInt32(r, 0);
        const std::string name = lower(godi.getString(r, 1));
        if (name.empty()) {
            ++noModel;
            continue;
        }
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".wmo") == 0) {
            ++wmo;  // transports, some doors and gates - a later build
            continue;
        }
        const BakedModel& bm = models.get(name);
        if (gParticleBank) gParticleBank->addDisplay(id, bm.effect);  // particles (also without a WMDL)
        if (bm.id < 0) {
            ++failed;
            continue;
        }
        fmt::GameObjectDisplayRec g{};
        g.displayId = id;
        g.model = static_cast<uint16_t>(bm.id);
        g.radiusCm = static_cast<uint16_t>(std::min(65535.0f, std::ceil(bm.radius * 100.0f)));
        for (int a = 0; a < 3; ++a)
            g.centerCm[a] = static_cast<int16_t>(std::clamp(std::round(bm.center[a] * 100.0f), -32767.0f, 32767.0f));
        recs.push_back(g);
    }
    std::sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.displayId < b.displayId; });
    fmt::GameObjectIndexHeader h{};
    std::memcpy(h.magic, "WGOI", 4);
    h.version = fmt::kModelFormatVersion;
    h.count = static_cast<uint32_t>(recs.size());
    index.name = "gidx";
    index.kind = kKindDoodads;
    index.blob.assign(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
    index.blob.insert(index.blob.end(), reinterpret_cast<const uint8_t*>(recs.data()),
                      reinterpret_cast<const uint8_t*>(recs.data() + recs.size()));
    std::printf("gameobjects: %u displays, %zu baked, %zu wmo (not yet), %zu without a model, %zu failed\n",
                godi.getRecordCount(), recs.size(), wmo, noModel, failed);
    return !recs.empty();
}

// Every GameObjectDisplayInfo WMO (the displays
// bakeGameObjects leaves out: ships, zeppelins, the icebreaker, WMO lifts and doors, a PvP zone's walls...) as a
// "gw_<displayId>" record of data/transports.pak (include/rendering/psp/psp_transport_format.hpp): the WMO's groups
// as WmoBank bakes a building's (WMDL kModelBuilding + collision by MOPY) and the doodads of its set 0 (upstream
// EntitySpawner loads a game object's WMO with the default set), in WMO model space - the runtime stands it where
// the object is, turned by its orientation (as the M2 displays). The box is the root's MOHD box (the riding test).
// A WMO over --wmo-max-tris (a capital city's WMO, display 471) is left out as in the tiles.
// Two paks (the runtime keeps a pak's TOC in RAM while it is open): set 1 = the transports' WMOs (upstream
// EntitySpawner's test: "transport" in the path, + the raid gunships) for data/transports.pak, open in the world;
// set 2 = the rest (doors, walls, buildings: ~2.3k entries) for data/gameobjects_wmo.pak, open only while one is near.
bool isTransportWmo(const std::string& name) {
    return name.find("transport") != std::string::npos || name.find("gunship") != std::string::npos;
}

size_t bakeGameObjectWmos(const std::string& root, WmoBank& wmos, std::vector<PakItem>& out, int set) {
    std::vector<uint8_t> bytes;
    pipeline::DBCFile godi;
    if (!readFile(root + "/dbfilesclient/gameobjectdisplayinfo.dbc", bytes) || !godi.load(bytes)) {
        std::fprintf(stderr, "missing dbfilesclient/gameobjectdisplayinfo.dbc\n");
        return 0;
    }
    size_t baked = 0, failed = 0, parts = 0;
    for (uint32_t r = 0; r < godi.getRecordCount(); ++r) {
        const uint32_t id = godi.getUInt32(r, 0);
        const std::string name = lower(godi.getString(r, 1));
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".wmo") != 0) continue;
        if (set && isTransportWmo(name) != (set == 1)) continue;
        const BakedWmo& w = wmos.get(name);
        if (!w.ok) {
            ++failed;
            continue;
        }
        std::vector<fmt::TransportPart> list;
        glm::vec3 lo(1e30f), hi(-1e30f);
        auto add = [&](const BakedModel& bm, const glm::mat4& local) {
            fmt::TransportPart p{};
            p.model = static_cast<uint16_t>(bm.id);
            const glm::mat3 axes(local);
            const glm::vec3 pos(local[3]);
            for (int a = 0; a < 3; ++a) p.position[a] = pos[a];
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 3; ++row) p.axes[col * 3 + row] = axes[col][row];
            const float scale = std::max({glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2])});
            const glm::vec3 c = pos + axes * bm.center;
            p.sphere[0] = c.x;
            p.sphere[1] = c.y;
            p.sphere[2] = c.z;
            p.sphere[3] = bm.radius * scale;
            lo = glm::min(lo, c - p.sphere[3]);
            hi = glm::max(hi, c + p.sphere[3]);
            list.push_back(p);
        };
        for (const BakedModel& g : w.groups) add(g, glm::mat4(1.0f));
        const size_t groups = list.size();
        if (!w.sets.empty()) {
            const auto [start, count] = w.sets[0];
            for (uint32_t i = start; i < start + count && i < w.doodads.size(); ++i)
                if (w.doodads[i].model.id >= 0) add(w.doodads[i].model, w.doodads[i].local);
        }
        if (list.empty() || list.size() > 65535) {
            ++failed;
            continue;
        }
        fmt::TransportWmoHeader h{};
        std::memcpy(h.magic, "WTRW", 4);
        h.version = fmt::kTransportFormatVersion;
        h.partCount = static_cast<uint16_t>(list.size());
        h.groupCount = static_cast<uint16_t>(groups);
        // The root's MOHD box (the groups' geometry); the parts' spheres when the root does not read.
        glm::vec3 bmin = lo, bmax = hi;
        std::vector<uint8_t> rootBytes;
        if (readFile(root + "/" + name, rootBytes)) {
            const pipeline::WMOModel m = pipeline::WMOLoader::load(rootBytes);
            if (m.boundingBoxMax.x > m.boundingBoxMin.x) {
                bmin = m.boundingBoxMin;
                bmax = m.boundingBoxMax;
            }
        }
        for (int a = 0; a < 3; ++a) {
            h.bmin[a] = bmin[a];
            h.bmax[a] = bmax[a];
        }
        const glm::vec3 c = (lo + hi) * 0.5f;
        for (int a = 0; a < 3; ++a) h.center[a] = c[a];
        h.radius = glm::length(hi - c);
        std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(list.data()),
                    reinterpret_cast<const uint8_t*>(list.data() + list.size()));
        out.push_back({"gw_" + std::to_string(id), 3, std::move(blob)});  // kind 3 = raw
        parts += list.size();
        ++baked;
        std::printf("gameobject wmo %u %s: %zu groups, %zu doodads, box (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", id,
                    name.c_str(), groups, list.size() - groups, bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z);
    }
    std::printf("gameobject wmos: %zu baked (%zu parts), %zu missing or over budget\n", baked, parts, failed);
    return baked;
}

// A map whose WDT names a global WMO (MPHD 0x1, upstream
// WDTInfo::isWMOOnly: most dungeons, e.g. maps 389 and 43) has no ADT. Upstream
// WorldLoader::loadMapGeometry loads its root WMO with every group at
//   position: adtToWorld(MODF position), or the origin when that position is zero ("the server sends
//             coordinates relative to the WMO, not relative to the map corner"),
//   rotation: (-rot[2], -rot[0], rot[1] + 180) degrees as Rz * Ry * Rx (the outdoor WMOs' +180 yaw),
// and the doodads of the MODF set only (doodadSets[min(set, n - 1)], WMO-local T * R * S under that
// matrix) - each marked setSkipWallCollision (ModelBank::setWallsBlock(false) before this).
// PSP: every group and doodad becomes an ordinary placement (like an outdoor WMO's, M8) in a virtual
// tile "d_<x>_<y>" picked by the centre of its bounding sphere (tile = 32 - position / TILE_SIZE, the
// PSP world's server coordinates): the runtime's tile loader, doodad radius and streamer take them as
// they take a continent's tiles. Returns the tiles (empty: nothing baked).
struct GlobalWmoResult {
    std::vector<PakItem> tiles;  // "d_<x>_<y>"
    fmt::WmoOnlyHeader header{};
    size_t placements = 0;
    bool big = false;  // the WMO is a big WMO ("wbig"), no "d_" tiles
};

// The ModelBank is not used here (WmoBank bakes the doodads through its own ModelBank) - unnamed
// for -Wextra's -Wunused-parameter; the signature and the call stay as they are.
GlobalWmoResult bakeGlobalWmo(const pipeline::WDTInfo& wdt, ModelBank& /*models*/, WmoBank& wmos) {
    GlobalWmoResult r;
    std::memcpy(r.header.magic, "WWMO", 4);
    r.header.version = fmt::kWmoOnlyVersion;
    r.header.mphdFlags = wdt.mphdFlags;
    for (int a = 0; a < 3; ++a) r.header.rotation[a] = wdt.rotation[a];
    r.header.doodadSet = wdt.doodadSet;
    std::snprintf(r.header.wmo, sizeof(r.header.wmo), "%s", lower(wdt.rootWMOPath).c_str());
    const BakedWmo& w = wmos.get(wdt.rootWMOPath);
    if (!w.ok && !w.big) {  // or baked as a big WMO (below)
        std::fprintf(stderr, "global wmo %s: not baked\n", wdt.rootWMOPath.c_str());
        return r;
    }
    constexpr float kDeg = core::coords::PI / 180.0f;
    glm::vec3 pos(0.0f);
    if (wdt.position[0] != 0.0f || wdt.position[1] != 0.0f || wdt.position[2] != 0.0f)
        pos = core::coords::adtToWorld(wdt.position[0], wdt.position[1], wdt.position[2]);
    const glm::vec3 rot(-wdt.rotation[2] * kDeg, -wdt.rotation[0] * kDeg, (wdt.rotation[1] + 180.0f) * kDeg);
    glm::mat4 world = glm::translate(glm::mat4(1.0f), pos);
    world = glm::rotate(world, rot.z, glm::vec3(0, 0, 1));
    world = glm::rotate(world, rot.y, glm::vec3(0, 1, 0));
    world = glm::rotate(world, rot.x, glm::vec3(1, 0, 0));
    for (int a = 0; a < 3; ++a) r.header.origin[a] = pos[a];
    constexpr uint32_t kUid = 0xFFFFFFFFu;  // the WDT's MODF uniqueId of a global WMO
    std::map<std::pair<int, int>, std::vector<fmt::DoodadPlacement>> byTile;
    glm::vec3 lo(1e30f), hi(-1e30f);
    // The virtual tile of a point (the tiles' rule below), for the particles and a big WMO's "wbig".
    auto tileOf = [](const glm::vec3& p, int& tx, int& ty) {
        const float tileSize = core::coords::TILE_SIZE;
        tx = std::clamp(static_cast<int>(std::floor(32.0f - p.y / tileSize)), 0, 63);
        ty = std::clamp(static_cast<int>(std::floor(32.0f - p.x / tileSize)), 0, 63);
    };
    auto add = [&](const fmt::DoodadPlacement& d) {
        int tx = 0, ty = 0;
        tileOf(glm::vec3(d.sphere[0], d.sphere[1], d.sphere[2]), tx, ty);
        byTile[{tx, ty}].push_back(d);
        ++r.placements;
    };
    // A global WMO over --global-max-tris source triangles is a big WMO (wmo_chunk_bake.hpp, the
    // capitals' path: chunks by cells, portals, streaming under a budget) - one "wbig" placement at the WDT's matrix
    // (uniqueId 0xFFFFFFFF), its doodads drawn by the runtime from that pak (the MODF set: upstream's
    // doodadSets[min(set, n - 1)]; the runtime adds set 0 - the same set for every 3.3.5a dungeon WDT, set 0),
    // their emitters on the virtual tiles. No "d_" tiles then: the doodads pak holds the models and "wbig".
    if (w.big) {
        const uint32_t set = w.big->sets.empty()
                                 ? 0u
                                 : std::min<uint32_t>(wdt.doodadSet, static_cast<uint32_t>(w.big->sets.size() - 1));
        fmt::BigWmoPlacement bp{};
        bp.uniqueId = kUid;
        bp.doodadSet = static_cast<uint16_t>(set);
        std::snprintf(bp.pak, sizeof(bp.pak), "%s", w.big->pakRel.c_str());
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 3; ++rr) bp.matrix[c * 3 + rr] = world[c][rr];
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 corner((k & 1) ? w.big->bmax.x : w.big->bmin.x, (k & 2) ? w.big->bmax.y : w.big->bmin.y,
                                   (k & 4) ? w.big->bmax.z : w.big->bmin.z);
            const glm::vec3 wc = glm::vec3(world * glm::vec4(corner, 1.0f));
            lo = glm::min(lo, wc);
            hi = glm::max(hi, wc);
        }
        int tx = 0, ty = 0;
        tileOf((lo + hi) * 0.5f, tx, ty);
        bp.tileX = static_cast<int16_t>(tx);
        bp.tileY = static_cast<int16_t>(ty);
        for (int a = 0; a < 3; ++a) {
            bp.bmin[a] = lo[a];
            bp.bmax[a] = hi[a];
            r.header.bmin[a] = lo[a];
            r.header.bmax[a] = hi[a];
        }
        size_t emitters = 0;
        if (gBigPlacements) {
            gBigPlacements->list.push_back(bp);
            gBigPlacements->uids.push_back(kUid);
        }
        if (gParticleBank)
            for (const auto& d : w.big->doodads) {
                if (d.model.effect < 0 || !(d.set == 0 || d.set == set)) continue;
                const glm::mat4 dw = world * d.local;
                int ex = 0, ey = 0;
                tileOf(glm::vec3(dw[3]), ex, ey);
                gParticleBank->place(ex, ey, partId(kUid, 0x20000u + d.index), d.model.effect, glm::mat3(dw), glm::vec3(dw[3]));
                ++emitters;
            }
        r.big = true;
        std::printf("global wmo %s: big WMO %s, origin (%.1f, %.1f, %.1f) rot (%.1f, %.1f, %.1f) set %u (WDT %u), tile "
                    "%d_%d, box (%.0f, %.0f, %.0f)-(%.0f, %.0f, %.0f), %zu doodads, %zu emitters\n",
                    wdt.rootWMOPath.c_str(), bp.pak, pos.x, pos.y, pos.z, wdt.rotation[0], wdt.rotation[1], wdt.rotation[2],
                    static_cast<unsigned>(set), static_cast<unsigned>(wdt.doodadSet), tx, ty, lo.x, lo.y, lo.z, hi.x, hi.y,
                    hi.z, w.big->doodads.size(), emitters);
        return r;
    }
    for (size_t g = 0; g < w.groups.size(); ++g) {
        const fmt::DoodadPlacement d = place(partId(kUid, 0x10000u + static_cast<uint32_t>(g)), w.groups[g], world);
        add(d);
        const glm::vec3 c(d.sphere[0], d.sphere[1], d.sphere[2]);
        lo = glm::min(lo, c - glm::vec3(d.sphere[3]));
        hi = glm::max(hi, c + glm::vec3(d.sphere[3]));
    }
    size_t doodads = 0;
    if (!w.sets.empty()) {
        const uint32_t set = std::min<uint32_t>(wdt.doodadSet, static_cast<uint32_t>(w.sets.size() - 1));
        const auto [start, count] = w.sets[set];
        for (uint32_t i = start; i < start + count && i < w.doodads.size(); ++i) {
            const BakedModel& dm = w.doodads[i].model;
            if (dm.id < 0 && dm.effect < 0) continue;  // an emitter-only model is placed too
            const glm::mat4 dw = world * w.doodads[i].local;
            if (gParticleBank && dm.effect >= 0) {  // the torches' emitters (was: none in a dungeon)
                int ex = 0, ey = 0;
                tileOf(glm::vec3(dw[3]), ex, ey);
                gParticleBank->place(ex, ey, partId(kUid, 0x20000u + i), dm.effect, glm::mat3(dw), glm::vec3(dw[3]));
            }
            if (dm.id < 0) continue;
            add(place(partId(kUid, 0x20000u + i), dm, dw));
            ++doodads;
        }
    }
    for (int a = 0; a < 3; ++a) {
        r.header.bmin[a] = lo[a];
        r.header.bmax[a] = hi[a];
    }
    r.header.groups = static_cast<uint16_t>(w.groups.size());
    r.header.tileCount = static_cast<uint16_t>(byTile.size());
    for (auto& [key, list] : byTile) {
        fmt::DoodadTileHeader h{};
        std::memcpy(h.magic, "WDOD", 4);
        h.version = fmt::kModelFormatVersion;
        h.count = static_cast<uint32_t>(list.size());
        std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(list.data()),
                    reinterpret_cast<const uint8_t*>(list.data() + list.size()));
        std::printf("tile %d_%d: %zu placements (global wmo)\n", key.first, key.second, list.size());
        r.tiles.push_back({"d_" + std::to_string(key.first) + "_" + std::to_string(key.second), kKindDoodads,
                           std::move(blob)});
    }
    std::printf("global wmo %s: origin (%.1f, %.1f, %.1f) rot (%.1f, %.1f, %.1f) set %u: %zu groups, %zu doodads, "
                "%zu tiles, box (%.0f, %.0f, %.0f)-(%.0f, %.0f, %.0f)\n",
                wdt.rootWMOPath.c_str(), pos.x, pos.y, pos.z, wdt.rotation[0], wdt.rotation[1], wdt.rotation[2],
                static_cast<unsigned>(wdt.doodadSet), w.groups.size(), doodads, r.tiles.size(), lo.x, lo.y, lo.z,
                hi.x, hi.y, hi.z);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    std::string root, map = "Azeroth", out;
    int texMax = 128;
    size_t wmoMaxTris = 60000;
    // A global WMO's budget (0 = none: it is the map). The global WMO's source
    // triangles over this -> a big-WMO pak (bigwmo/, "wbig"; bake_world.sh passes 110000 for a dungeon), 0 = never.
    size_t globalMaxTris = 0;
    // GE limits (psp_ge_limits.hpp safeEdgePerNear at near 1.0: 5.03 yd at cfg fov 90, 3.22 at fov 50):
    // M2 8 -> 5 (safe at the default fov 90; guardlod covers the rest). WMO stays 4 (safe at cfg fov >= 66): 3 yd pushed
    // mid-size WMOs over --wmo-max-tris after the split (a starting area's main building: 17.3k source -> 65k) into the big-WMO path.
    float wmoMaxEdge = 4.0f;  // yards, near LOD only
    float m2MaxEdge = 5.0f;   // model units (doodad scale is ~1), near LOD only
    ForestOptions forest;     // forest LOD
    std::string verify, dumpDir, selftest;
    bool gameObjects = false;  // game objects
    bool transports = false, transportAllPaths = false;  // transport WMOs
    int wmoSet = 1;  // 1 transport WMOs + tables, 2 the other WMOs, 0 all
    ParticleBankOptions ptc;   // particles
    // WMOs over --wmo-max-tris become big-WMO paks in <bigwmo-dir>/bigwmo/ (default: next to
    // --out); --bigwmo 0 skips them as before.
    bool bigOn = true;
    std::string bigDir, bigVerify, bigRoute, bigRouteOut, bigOnly;
    BigWmoOptions bigOpt;
    std::vector<std::pair<int, int>> tiles;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) root = argv[++i];
        else if (a == "--verify" && i + 1 < argc) verify = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dumpDir = argv[++i];
        else if (a == "--selftest" && i + 1 < argc) selftest = argv[++i];
        else if (a == "--tree-lod2" && i + 1 < argc) forest.lod2Ratio = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--lod2-error" && i + 1 < argc) forest.lod2Error = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--impostors" && i + 1 < argc) forest.impostors = std::atoi(argv[++i]) != 0;
        else if (a == "--impostor-cell" && i + 1 < argc) forest.impostorCell = std::atoi(argv[++i]);
        else if (a == "--impostor-views" && i + 1 < argc) forest.impostorViews = std::atoi(argv[++i]);
        else if (a == "--map" && i + 1 < argc) map = argv[++i];
        else if (a == "--gameobjects") gameObjects = true;
        else if (a == "--transports") transports = true;                  // transport WMOs
        else if (a == "--transport-all-paths") transportAllPaths = true;  // transport WMOs
        else if (a == "--wmo-set" && i + 1 < argc) {                      // transport WMOs
            // An unknown name stops the bake (a typo was baked as "transports").
            const std::string v = argv[++i];
            wmoSet = v == "transports" ? 1 : v == "other" ? 2 : v == "all" ? 0 : -1;
            if (wmoSet < 0) {
                std::fprintf(stderr, "psp_model_conv: --wmo-set %s: unknown (transports / other / all)\n", v.c_str());
                return 2;
            }
        }
        else if (a == "--particles" && i + 1 < argc) ptc.enabled = std::atoi(argv[++i]) != 0;    // particles
        else if (a == "--ptc-tex" && i + 1 < argc) ptc.texMax = std::clamp(std::atoi(argv[++i]), 16, 128);
        else if (a == "--ptc-list") ptc.list = true;
        else if (a == "--bigwmo" && i + 1 < argc) bigOn = std::atoi(argv[++i]) != 0;  // big WMOs
        else if (a == "--bigwmo-dir" && i + 1 < argc) bigDir = argv[++i];
        else if (a == "--bigwmo-cell" && i + 1 < argc) bigOpt.cellSize = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--bigwmo-chunk" && i + 1 < argc) bigOpt.chunkTriangles = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--bigwmo-edge" && i + 1 < argc) bigOpt.maxEdge = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--bigwmo-base-edge" && i + 1 < argc) bigOpt.baseEdge = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--bigwmo-extent" && i + 1 < argc) bigOpt.maxChunkExtent = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--bigwmo-indexed" && i + 1 < argc) bigOpt.indexed = std::atoi(argv[++i]) != 0;
        else if (a == "--bigwmo-lod" && i + 2 < argc) {
            bigOpt.lodRatio = static_cast<float>(std::atof(argv[++i]));
            bigOpt.lodError = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--bigwmo-shell" && i + 2 < argc) {
            bigOpt.shellRatio = static_cast<float>(std::atof(argv[++i]));
            bigOpt.shellError = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--bigwmo-verify" && i + 1 < argc) bigVerify = argv[++i];
        else if (a == "--bigwmo-only" && i + 1 < argc) bigOnly = argv[++i];
        else if (a == "--bigwmo-route" && i + 2 < argc) {
            bigRoute = argv[++i];
            bigRouteOut = argv[++i];
        }
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--tex-max" && i + 1 < argc) texMax = std::atoi(argv[++i]);
        else if (a == "--m2-max-edge" && i + 1 < argc) m2MaxEdge = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--wmo-max-edge" && i + 1 < argc) wmoMaxEdge = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--wmo-max-tris" && i + 1 < argc) wmoMaxTris = static_cast<size_t>(std::atol(argv[++i]));
        else if (a == "--global-max-tris" && i + 1 < argc) globalMaxTris = static_cast<size_t>(std::atol(argv[++i]));
        else if (a == "--tile" && i + 1 < argc) {
            int x, y;
            if (std::sscanf(argv[++i], "%d_%d", &x, &y) == 2) tiles.emplace_back(x, y);
        } else if (a == "--around" && i + 3 < argc) {
            int x = std::atoi(argv[++i]), y = std::atoi(argv[++i]), r = std::atoi(argv[++i]);
            for (int dx = -r; dx <= r; ++dx)
                for (int dy = -r; dy <= r; ++dy) tiles.emplace_back(x + dx, y + dy);
        }
    }
    if (!bigVerify.empty()) return bigWmoVerify(bigVerify) ? 0 : 1;                       // big WMOs
    if (!bigRoute.empty()) return bigWmoRoute(bigRoute, bigRouteOut, root) ? 0 : 1;       // big WMOs
    if (!bigOnly.empty() && !root.empty() && !out.empty()) {
        // One WMO alone into <out>/bigwmo/ (parameters, PC checks) - no doodads pak, so its
        // doodads have no models (stamp 0: the PSP draws it without them).
        std::vector<uint8_t> bytes;
        const std::string key = lower(bigOnly);
        if (!readFile(root + "/" + key, bytes)) return 1;
        pipeline::WMOModel m = pipeline::WMOLoader::load(bytes);
        for (uint32_t gi = 0; gi < m.groups.size(); ++gi)
            for (const std::string& cand : pipeline::wmoGroupCandidates(bigOnly, gi)) {
                std::vector<uint8_t> gb;
                if (readFile(root + "/" + lower(cand), gb)) {
                    pipeline::WMOLoader::loadGroup(gb, m, gi);
                    break;
                }
            }
        std::string stem = key.substr(key.find_last_of("/\\") + 1);
        if (stem.size() > 4) stem.resize(stem.size() - 4);
        std::filesystem::create_directories(out + "/bigwmo");
        BigWmoBaker baker(root, texMax, bigOpt);
        BigWmoBaked res;
        return baker.bake(bigOnly, m, out, "bigwmo/" + lower(map) + "_" + stem + ".pak", 0,
                          [](const std::string&) { return BigWmoModelRef{}; }, res)
                   ? 0
                   : 1;
    }
    if (!verify.empty())  // + particles, + transport WMOs
        return verifyPak(verify, dumpDir) + verifyParticleIndex(verify) + verifyTransports(verify) +
                       verifyBigWmoIndex(verify) ==  // "wbig", its bigwmo/*.pak, the stamp
                   0
                   ? 0
                   : 1;
    if (!selftest.empty()) return forestSelfTest(selftest, forest) ? 0 : 1;
    if (transports && !root.empty() && !out.empty() && texMax >= 16) {  // transport WMOs
        std::vector<PakItem> textures, models, records;
        TextureBank tex(root, texMax, textures);
        ForestOptions noForest = forest;
        noForest.impostors = false;  // as the game objects: no forest LOD / impostors
        noForest.lod2Ratio = 0.0f;
        ModelBank bank(root, tex, models, m2MaxEdge, noForest);
        gParticleBank = nullptr;  // the WMOs' emitters are not baked into this pak
        WmoBank wmos(root, tex, bank, wmoMaxTris, wmoMaxEdge);
        const size_t wmoCount = bakeGameObjectWmos(root, wmos, records, wmoSet);
        const size_t animCount = wmoSet == 2 ? 0 : bakeTransportAnimations(root, records);
        PakItem paths;
        const bool havePaths = wmoSet != 2 && bakeTransportPaths(root, transportAllPaths, paths);
        if (!wmoCount && !animCount && !havePaths) return 1;
        std::printf("models %zu (%zu skipped, %zu KB, %zu tris), textures %zu (%zu KB), collision %zu (%zu KB)\n",
                    bank.count(), bank.skipped(), bank.bytes() / 1024, bank.triangles(), tex.count(), tex.bytes() / 1024,
                    bank.collisions(), bank.collisionBytes() / 1024);
        std::vector<PakItem> all;
        if (havePaths) all.push_back(std::move(paths));
        for (auto* v : {&records, &models, &textures})
            for (auto& e : *v) all.push_back(std::move(e));
        return writePak(out, all) ? 0 : 1;
    }
    if (gameObjects && !root.empty() && !out.empty() && texMax >= 16) {  // game objects
        std::vector<PakItem> textures, models;
        TextureBank tex(root, texMax, textures);
        ForestOptions noForest = forest;
        noForest.impostors = false;  // game objects: no forest LOD / impostors
        noForest.lod2Ratio = 0.0f;
        ModelBank bank(root, tex, models, m2MaxEdge, noForest);
        ParticleBank particles(root, ptc);  // particles
        gParticleBank = &particles;
        PakItem index;
        if (!bakeGameObjects(root, bank, index)) return 1;
        std::printf("models %zu (%zu skipped, %zu KB, %zu tris), textures %zu (%zu KB), collision %zu (%zu KB)\n",
                    bank.count(), bank.skipped(), bank.bytes() / 1024, bank.triangles(), tex.count(), tex.bytes() / 1024,
                    bank.collisions(), bank.collisionBytes() / 1024);
        std::vector<PakItem> all;
        PakItem pidx;  // particles: the pak's first record
        particles.printStats();
        if (particles.build(pidx)) all.push_back(std::move(pidx));
        all.push_back(std::move(index));
        for (auto* v : {&models, &textures})
            for (auto& e : *v) all.push_back(std::move(e));
        return writePak(out, all) ? 0 : 1;
    }
    if (root.empty() || out.empty() || tiles.empty() || texMax < 16) {
        std::fprintf(stderr, "usage: psp_model_conv --extracted <dir> --map <Name> --out <pak> "
                             "(--tile X_Y | --around X Y R)... [--tex-max 128]\n"
                             "       [--tree-lod2 0.3] [--lod2-error 0.04] [--impostors 1] [--impostor-cell 64]"
                             " [--impostor-views 8]\n"
                             "       [--global-max-tris 0]   (a map with a global WMO: that WMO, tiles ignored; source\n"
                             "                                triangles over N: a big-WMO pak)\n"
                             "       psp_model_conv --verify <pak> [--dump <dir>]\n"
                             "       psp_model_conv --selftest <dir>\n");
        return 2;
    }
    std::vector<PakItem> textures, models, doodads;
    TextureBank tex(root, texMax, textures);
    ModelBank bank(root, tex, models, m2MaxEdge, forest);
    ParticleBank particles(root, ptc);  // particles
    gParticleBank = &particles;
    WmoBank wmos(root, tex, bank, wmoMaxTris, wmoMaxEdge);
    // Big WMOs and the "wbig" record; the stamp ties this doodads pak to those paks.
    BigWmoPlacements bigPlacements;
    if (bigOn) {
        if (bigDir.empty()) bigDir = std::filesystem::path(out).parent_path().string();
        if (bigDir.empty()) bigDir = ".";
        // The stamp is a function of what decides this pak's model ids ("m_<id>" the big WMOs'
        // doodads name) - the map, the tiles in bake order, every option but the paths, the formats - was the map ^
        // time(): the same input gives the same stamp, other tiles another one (a bigwmo/ pak of another run is
        // then drawn without doodads, doodads_ok=0, instead of with this pak's wrong models; --verify names it).
        uint32_t st = 2166136261u;
        auto mix = [&st](const std::string& v) {
            for (char c : v) st = (st ^ static_cast<uint8_t>(c)) * 16777619u;
            st = (st ^ 0x1Fu) * 16777619u;  // separator
        };
        mix(lower(map));
        mix("ground-effects-v2-density");
        mix("foliage-material-alpha-v2"); // changed model-ID allocation: old big-WMO companions must not match
        mix(std::to_string(fmt::kBigWmoVersion) + "/" + std::to_string(fmt::kModelFormatVersion));
        for (auto [x, y] : tiles) mix(std::to_string(x) + "_" + std::to_string(y));
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if ((a == "--out" || a == "--extracted" || a == "--bigwmo-dir" || a == "--map" || a == "--tile") && i + 1 < argc) {
                ++i;  // paths; the map and the tiles are in already
                continue;
            }
            if (a == "--around" && i + 3 < argc) {
                i += 3;
                continue;
            }
            mix(a);
        }
        bigPlacements.stamp = st ? st : 1u;  // 0 = "no stamp" on the PSP
        wmos.setBigWmo(bigDir, map, bigPlacements.stamp, bigOpt, texMax);
        gBigPlacements = &bigPlacements;
    }
    size_t total = 0;
    // The WDT decides, as upstream WorldLoader::loadMapGeometry does - a global
    // WMO (and its name) makes the whole map that WMO; the tile list is not used then.
    pipeline::WDTInfo wdt;
    {
        std::vector<uint8_t> wdtBytes;
        if (readFile(root + "/world/maps/" + lower(map) + "/" + lower(map) + ".wdt", wdtBytes))
            wdt = pipeline::parseWDT(wdtBytes);
    }
    if (wdt.isWMOOnly() && !wdt.rootWMOPath.empty()) {
        bank.setWallsBlock(false);       // upstream setSkipWallCollision for the instance's doodads
        // No triangle budget for the map itself (was setMaxTris(globalMaxTris): with the big-WMO
        // directory a WMO over it became a big WMO bakeGlobalWmo then refused, "not baked"); --global-max-tris is the
        // source-triangle limit of the big-WMO way.
        wmos.setMaxTris(0);
        wmos.setBigSourceLimit(globalMaxTris);
        GlobalWmoResult g = bakeGlobalWmo(wdt, bank, wmos);
        if (g.tiles.empty() && !g.big) return 1;  // a big global WMO has "wbig" and no tiles
        total = g.placements;
        for (auto& t : g.tiles) doodads.push_back(std::move(t));
        std::vector<uint8_t> rec(reinterpret_cast<const uint8_t*>(&g.header),
                                 reinterpret_cast<const uint8_t*>(&g.header + 1));
        doodads.push_back({"wmo_only", 3, std::move(rec)});  // kind 3 = raw (psp_pak.hpp)
    }
    for (auto [x, y] : tiles) {
        if (wdt.isWMOOnly() && !wdt.rootWMOPath.empty()) break;  // global-WMO map: no ADTs to read
        std::vector<uint8_t> blob;
        size_t n = 0;
        if (!bakeTile(root, map, x, y, bank, wmos, blob, n)) {
            std::printf("  tile %d_%d: no ADT, skipped\n", x, y);
            continue;
        }
        std::printf("tile %d_%d: %zu doodads\n", x, y, n);
        total += n;
        doodads.push_back({"d_" + std::to_string(x) + "_" + std::to_string(y), kKindDoodads, std::move(blob)});
    }
    if (doodads.empty()) return 1;
    std::printf("models %zu (%zu skipped, %zu KB, %zu tris), textures %zu (%zu KB), doodads %zu\n", bank.count(),
                bank.skipped(), bank.bytes() / 1024, bank.triangles(), tex.count(), tex.bytes() / 1024, total);
    std::printf("collision meshes %zu (%zu KB)\n", bank.collisions(), bank.collisionBytes() / 1024);
    std::printf("emitter volumes hidden: %zu (of the skipped models; upstream M2Renderer)\n",
                bank.emitterVolumes());
    std::printf("split short: %zu triangles left longer than the near edge in %zu models / WMO groups\n",  // split-short totals
                bank.splitShort(), bank.splitShortItems());
    std::printf("forest: lod2 %zu of %zu source tris (cards %zu of %zu kept), impostors %zu\n", bank.lod2Stats().tris,
                bank.lod2Stats().sourceTris, bank.lod2Stats().cardsKept, bank.lod2Stats().cardsTotal, bank.impostors());
    std::vector<PakItem> all;
    PakItem pidx;  // particles: the pak's first record
    particles.printStats();
    if (particles.build(pidx)) all.push_back(std::move(pidx));
    if (!bigPlacements.list.empty()) {  // big WMOs: "wbig" (psp_bigwmo_format.hpp)
        fmt::BigWmoIndexHeader bh{};
        std::memcpy(bh.magic, "WBGI", 4);
        bh.version = fmt::kBigWmoVersion;
        bh.count = static_cast<uint16_t>(bigPlacements.list.size());
        bh.stamp = bigPlacements.stamp;
        std::vector<uint8_t> rec(reinterpret_cast<const uint8_t*>(&bh), reinterpret_cast<const uint8_t*>(&bh + 1));
        rec.insert(rec.end(), reinterpret_cast<const uint8_t*>(bigPlacements.list.data()),
                   reinterpret_cast<const uint8_t*>(bigPlacements.list.data() + bigPlacements.list.size()));
        all.push_back({"wbig", 3, std::move(rec)});
        std::printf("big wmos: %zu baked (%zu failed), %zu placements, stamp %08x, dir %s/bigwmo\n", wmos.bigCount(),
                    wmos.bigFailed(), bigPlacements.list.size(), bigPlacements.stamp, bigDir.c_str());
    }
    for (auto* v : {&doodads, &models, &textures})
        for (auto& e : *v) all.push_back(std::move(e));
    return writePak(out, all) ? 0 : 1;
}
