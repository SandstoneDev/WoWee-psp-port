#include "rendering/psp/psp_models.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/psp/psp_cpu_fast.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_log.hpp"
#include "rendering/psp/psp_frustum.hpp"
#include "rendering/psp/psp_ge_limits.hpp"  // guard band
#include "rendering/psp/psp_lighting_ge.hpp"  // Lighting
#include "rendering/psp/psp_tile_budget.hpp"  // tiles by budget

#include <pspgu.h>
#include <pspgum.h>

#include <glm/gtc/matrix_transform.hpp>

#include <malloc.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;
namespace prof = wowee::platform::psp::prof;

namespace {

constexpr int kVertexType = GU_INDEX_16BIT | GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
constexpr int kStripType = GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;

bool isBlended(uint8_t blend) { return blend >= kBlendAlpha; }

// Batch range of a LOD: 0 full detail, 1 low skin / unsplit mesh, 2 the far
// mesh of trees (kModelLod2).
void lodRange(const ModelHeader& h, uint8_t lod, uint32_t& first, uint32_t& count) {
    first = lod == 0 ? 0u : lod == 1 ? h.batchCount[0] : static_cast<uint32_t>(h.batchCount[0]) + h.batchCount[1];
    count = lod == 0 ? h.batchCount[0] : lod == 1 ? h.batchCount[1] : h.lod2BatchCount;
}

} // namespace

ModelRenderer::~ModelRenderer() { clear(); }

void ModelRenderer::clear() {
    for (Model& m : models_)
        if (m.blob) pp::pakFree(m.blob);
    for (Model& m : models_)
        if (m.colBlob) pp::pakFree(m.colBlob);
    for (Model& m : models_)
        if (m.impBlob) pp::pakFree(m.impBlob);
    for (Tex& t : textures_)
        if (t.ok) pp::textureRelease(t.t);
    for (void* b : tileBlobs_) pp::pakFree(b);
    models_.clear();
    textures_.clear();
    tileBlobs_.clear();
    tileKeys_.clear();  // Streaming
    instances_.clear();
    seen_.clear();
    insts_.clear();
    cells_.clear();
    gridDirty_ = false;
    visible_.clear();
    visFast_.clear();
    mtxFast_.clear();
    impKeys_.clear();
    lattice_.clear();
    deferred_.clear();
    texMissing_.clear();
    ++gridGen_;
    memoryBytes_ = 0;
    stats_ = ModelStats{};
}

bool ModelRenderer::loadTexture(pp::Pak& pak, uint16_t id, bool allowLod) {
    if (id >= textures_.size()) textures_.resize(id + 1u);
    Tex& t = textures_[id];
    if (t.tried) return t.ok;
    t.tried = true;
    char name[16];
    std::snprintf(name, sizeof(name), "x_%u", static_cast<unsigned>(id));
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, e->size, t.t, true)) {
        pp::pakFree(blob);
        return false;
    }
    t.ok = true;
    t.bytes = e->size;
    // Half resolution for the big doodad textures (wowee.cfg
    // m2texlod, KB of level 0 from which to drop it; 0 = off). The PSP screen
    // is 480 x 272 - a 256 x 256 tree bark is sampled at its level 1 or below
    // almost everywhere; the world had filled the 43.8 MB hardware heap.
    if (texLodBytes > 0 && allowLod) {
        if (const uint32_t nb = pp::textureDropTopMip(t.t, texLodBytes)) {
            t.bytes = nb;
            ++stats_.texLodDropped;
        }
    }
    memoryBytes_ += t.bytes;
    ++stats_.textures;
    return true;
}

const pp::Texture* ModelRenderer::texture(uint16_t id) const {
    if (id == kNoTexture || id >= textures_.size() || !textures_[id].ok) return nullptr;
    return &textures_[id].t;
}

const ModelRenderer::Model* ModelRenderer::model(pp::Pak& pak, uint16_t id) {
    if (id >= models_.size()) models_.resize(id + 1u);
    Model& m = models_[id];
    if (m.tried) return m.h ? &m : nullptr;
    m.tried = true;
    char name[16];
    std::snprintf(name, sizeof(name), "m_%u", static_cast<unsigned>(id));
    const pp::PakEntry* e = pak.find(name);
    auto* blob = e ? static_cast<uint8_t*>(pak.load(e)) : nullptr;
    if (!blob) return nullptr;
    const auto* h = reinterpret_cast<const ModelHeader*>(blob);
    const uint32_t batches = static_cast<uint32_t>(h->batchCount[0]) + h->batchCount[1] +
                             ((h->flags & kModelLod2) ? h->lod2BatchCount : 0u);  // Far LOD
    if (e->size < sizeof(ModelHeader) || std::memcmp(h->magic, "WMDL", 4) != 0 ||
        h->version != kModelFormatVersion || h->vertexOffset % 16 || h->indexOffset % 2 ||
        h->vertexOffset + h->vertexCount * sizeof(ModelVertex) > e->size ||
        h->indexOffset + h->indexCount * 2u > e->size || h->batchOffset + batches * sizeof(ModelBatch) > e->size) {
        PSP_LOGE(pp::tag::Gfx, "wmdl_bad name=%s size=%u", name, static_cast<unsigned>(e->size));
        pp::pakFree(blob);
        m.bad = true;  // broken data - forgetFailed() leaves it
        return nullptr;
    }
    m.blob = blob;
    m.h = h;
    m.verts = reinterpret_cast<const ModelVertex*>(blob + h->vertexOffset);
    m.indices = reinterpret_cast<const uint16_t*>(blob + h->indexOffset);
    m.batches = reinterpret_cast<const ModelBatch*>(blob + h->batchOffset);
    const uint32_t limit = (h->flags & kModelStrips) ? h->vertexCount : h->indexCount;
    for (uint32_t i = 0; i < batches; ++i) {
        if (m.batches[i].indexStart + m.batches[i].indexCount > limit) {
            PSP_LOGE(pp::tag::Gfx, "wmdl_bad_batch name=%s", name);
            pp::pakFree(blob);
            m = Model{};
            m.tried = true;
            m.bad = true;
            return nullptr;
        }
    }
    for (uint32_t i = 0; i < batches; ++i)
        if (m.batches[i].texture != kNoTexture) loadTexture(pak, m.batches[i].texture);
    // which LODs have blended batches (the blended pass skips the others).
    for (uint8_t lod = 0; lod < 3; ++lod) {
        if (lod == 2 && !(h->flags & kModelLod2)) continue;
        uint32_t first, count;
        lodRange(*h, lod, first, count);
        for (uint32_t i = first; i < first + count; ++i)
            if (isBlended(m.batches[i].blend)) m.blendMask |= static_cast<uint8_t>(1u << lod);
    }
    memoryBytes_ += e->size;
    ++stats_.models;
    if (h->flags & kModelFoliage) loadImpostor(pak, id, m);  // "i_<n>", when baked
    // Collision mesh, when the model has one.
    std::snprintf(name, sizeof(name), "k_%u", static_cast<unsigned>(id));
    if (const pp::PakEntry* ce = pak.find(name)) {
        auto* cb = static_cast<uint8_t*>(pak.load(ce));
        const auto* ch = reinterpret_cast<const CollisionHeader*>(cb);
        if (cb && ce->size >= sizeof(CollisionHeader) && std::memcmp(ch->magic, "WCOL", 4) == 0 &&
            ch->listOffset + ch->listCount * 4u <= ce->size &&
            ch->cellOffset + static_cast<uint32_t>(ch->nx) * ch->ny * 8u <= ce->size &&
            ch->triangleOffset + ch->triangleCount * sizeof(CollisionTri) <= ce->size &&
            ch->vertexOffset + ch->vertexCount * 12u <= ce->size) {
            m.colBlob = cb;
            m.col = ch;
            collisionBytes_ += ce->size;
        } else if (cb) {
            pp::pakFree(cb);
        }
    }
    return &m;
}

bool ModelRenderer::loadTile(pp::Pak& pak, int tileX, int tileY) {
    char name[32];
    std::snprintf(name, sizeof(name), "d_%d_%d", tileX, tileY);
    const pp::PakEntry* e = pak.find(name);
    auto* blob = e ? static_cast<uint8_t*>(pak.load(e)) : nullptr;
    if (!blob) return false;
    const auto* h = reinterpret_cast<const DoodadTileHeader*>(blob);
    if (e->size < sizeof(DoodadTileHeader) || std::memcmp(h->magic, "WDOD", 4) != 0 ||
        h->version != kModelFormatVersion || sizeof(DoodadTileHeader) + h->count * sizeof(DoodadPlacement) > e->size) {
        PSP_LOGE(pp::tag::Gfx, "wdod_bad name=%s size=%u", name, static_cast<unsigned>(e->size));
        pp::pakFree(blob);
        return false;
    }
    tileBlobs_.push_back(blob);
    tileKeys_.push_back((static_cast<uint32_t>(tileX & 0xFFFF) << 16) | static_cast<uint32_t>(tileY & 0xFFFF));
    memoryBytes_ += e->size;
    const auto* p = reinterpret_cast<const DoodadPlacement*>(blob + sizeof(DoodadTileHeader));
    for (uint32_t i = 0; i < h->count; ++i) {
        if (!seen_.insert(p[i].uniqueId).second) continue;  // already added by a neighbour tile
        if (!model(pak, p[i].model)) continue;
        instances_.push_back(&p[i]);
    }
    gridDirty_ = true;
    stats_.instances = static_cast<uint32_t>(instances_.size());
    stats_.memoryKb = memoryBytes_ / 1024u;
    return true;
}

bool ModelRenderer::setPlacements(pp::Pak& pak, int tileX, int tileY, const DoodadPlacement* p, uint32_t n) {
    const uint32_t bytes = static_cast<uint32_t>(sizeof(DoodadTileHeader) + n * sizeof(DoodadPlacement));
    auto* blob = static_cast<uint8_t*>(memalign(64, (bytes + 63u) & ~63u));  // freed by unloadTile (pakFree)
    if (!blob) return false;
    auto* h = reinterpret_cast<DoodadTileHeader*>(blob);
    std::memcpy(h->magic, "WDOD", 4);
    h->version = kModelFormatVersion;
    h->reserved = 0;
    h->count = n;
    h->reserved1 = 0;
    if (n) std::memcpy(blob + sizeof(DoodadTileHeader), p, n * sizeof(DoodadPlacement));
    tileBlobs_.push_back(blob);
    tileKeys_.push_back((static_cast<uint32_t>(tileX & 0xFFFF) << 16) | static_cast<uint32_t>(tileY & 0xFFFF));
    const auto* q = reinterpret_cast<const DoodadPlacement*>(blob + sizeof(DoodadTileHeader));
    for (uint32_t i = 0; i < n; ++i) {
        if (!seen_.insert(q[i].uniqueId).second) continue;  // still placed by the old set (dropped next)
        if (!model(pak, q[i].model)) continue;
        instances_.push_back(&q[i]);
    }
    gridDirty_ = true;
    stats_.instances = static_cast<uint32_t>(instances_.size());
    stats_.memoryKb = memoryBytes_ / 1024u;
    return true;
}

// see psp_models.hpp.
bool ModelRenderer::movePlacements(int tileX, int tileY, const DoodadPlacement* p, uint32_t n) {
    const uint32_t key = (static_cast<uint32_t>(tileX & 0xFFFF) << 16) | static_cast<uint32_t>(tileY & 0xFFFF);
    for (size_t i = 0; i < tileKeys_.size(); ++i) {
        if (tileKeys_[i] != key) continue;
        auto* h = static_cast<DoodadTileHeader*>(tileBlobs_[i]);
        if (h->count != n) return false;
        if (n) std::memcpy(reinterpret_cast<uint8_t*>(h) + sizeof(DoodadTileHeader), p, n * sizeof(DoodadPlacement));
        gridDirty_ = true;
        flushGrid();
        return true;
    }
    return false;
}

void ModelRenderer::reserveTables(const pp::Pak& pak) {
    uint32_t maxModel = 0, maxTex = 0;
    for (uint32_t i = 0; i < pak.count(); ++i) {
        const char* n = pak.entry(i)->name;
        if ((n[0] != 'm' && n[0] != 'x') || n[1] != '_' || n[2] < '0' || n[2] > '9') continue;
        const uint32_t id = static_cast<uint32_t>(std::strtoul(n + 2, nullptr, 10));
        if (id >= kNoTexture) continue;
        if (n[0] == 'm') maxModel = std::max(maxModel, id + 1u);
        else maxTex = std::max(maxTex, id + 1u);
    }
    if (models_.size() < maxModel) models_.resize(maxModel);
    if (textures_.size() < maxTex) textures_.resize(maxTex);
}

void ModelRenderer::buildGrid() {
    gridDirty_ = false;
    insts_.clear();
    cells_.clear();
    ++gridGen_;        // the collision memo forgets the old world
    lattice_.clear();
    if (instances_.empty()) return;
    constexpr float kCell = 64.0f;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (const DoodadPlacement* p : instances_) {
        minX = std::min(minX, p->sphere[0]);
        minY = std::min(minY, p->sphere[1]);
        maxX = std::max(maxX, p->sphere[0]);
        maxY = std::max(maxY, p->sphere[1]);
    }
    const int nx = static_cast<int>((maxX - minX) / kCell) + 1;
    const int ny = static_cast<int>((maxY - minY) / kCell) + 1;
    auto cellOf = [&](const DoodadPlacement* p) {
        return static_cast<int>((p->sphere[0] - minX) / kCell) * ny + static_cast<int>((p->sphere[1] - minY) / kCell);
    };
    std::vector<uint32_t> counts(static_cast<size_t>(nx) * ny + 1, 0);
    for (const DoodadPlacement* p : instances_) ++counts[cellOf(p) + 1];
    for (size_t i = 1; i < counts.size(); ++i) counts[i] += counts[i - 1];
    insts_.resize(instances_.size());
    std::vector<uint32_t> fill(counts.begin(), counts.end() - 1);
    for (const DoodadPlacement* p : instances_) {
        const Model* m = &models_[p->model];
        // Upstream m2_view_distance: bigger models reach further, foliage
        // further still, ground clutter stays close.
        const float r = p->sphere[3];
        float f = std::max(1.0f, r / 12.0f);
        if (m->h->flags & kModelFoliage) f *= 1.6f;
        if (m->h->flags & kModelGroundDetail) f *= 0.4f;
        if (m->h->flags & kModelBuilding) f *= 1.5f;  // buildings stay visible (M8)
        else if (r < 3.0f) f *= 0.6f;
        insts_[fill[cellOf(p)]++] = Inst{p->sphere[0], p->sphere[1], p->sphere[2], r, f, p, m};
    }
    for (size_t c = 0; c + 1 < counts.size(); ++c) {
        if (counts[c + 1] == counts[c]) continue;
        Cell cell{0, 0, 0, 0, 0, counts[c], counts[c + 1] - counts[c]};
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (uint32_t i = cell.first; i < cell.first + cell.count; ++i) {
            const Inst& in = insts_[i];
            lo = glm::min(lo, glm::vec3(in.x, in.y, in.z) - in.r);
            hi = glm::max(hi, glm::vec3(in.x, in.y, in.z) + in.r);
            cell.maxReach = std::max(cell.maxReach, in.reach);
        }
        const glm::vec3 ctr = (lo + hi) * 0.5f;
        cell.x = ctr.x;
        cell.y = ctr.y;
        cell.z = ctr.z;
        cell.r = glm::length(hi - ctr);
        cells_.push_back(cell);
    }
    // the cells on their lattice for the collision queries.
    // A cell's sphere holds its doodads' spheres; everything a doodad can
    // answer lies within 1.5 x the cell radius of the cell centre (a ray
    // passing a sphere's test comes within sqrt 2 x r of its centre), so a
    // query visits the slots of its box grown by the largest such reach in
    // slots (gridSpill_). Lattice slot = cellOf's index, cells_ in its order.
    gridMinX_ = minX;
    gridMinY_ = minY;
    gridNx_ = nx;
    gridNy_ = ny;
    gridSpill_ = 0;
    if (cells_.size() < 0xFFFFu && static_cast<size_t>(nx) * static_cast<size_t>(ny) <= 65536u) {
        lattice_.assign(static_cast<size_t>(nx) * ny, 0);
        auto slot = [](float v, float lo) { return static_cast<int>(std::floor((v - lo) / kCell)); };
        for (size_t k = 0; k < cells_.size(); ++k) {
            const Cell& c = cells_[k];
            const int s = cellOf(insts_[c.first].p);
            lattice_[static_cast<size_t>(s)] = static_cast<uint16_t>(k + 1);
            const int ix = s / ny, iy = s % ny;
            const float reach = c.r * 1.5f + 2.0f;
            gridSpill_ = std::max({gridSpill_, ix - slot(c.x - reach, minX), slot(c.x + reach, minX) - ix,
                                   iy - slot(c.y - reach, minY), slot(c.y + reach, minY) - iy});
        }
    }
    if (vramOn)  // The game objects' renderer rebuilds often - its own log line (gameobjects_set)
        PSP_LOGI(pp::tag::Gfx, "%s_grid cells=%u doodads=%u lattice=%dx%d spill=%d", logTag,
                 static_cast<unsigned>(cells_.size()), static_cast<unsigned>(insts_.size()), lattice_.empty() ? 0 : nx,
                 lattice_.empty() ? 0 : ny, gridSpill_);
    for (Model& m : models_) m.instances = 0;
    for (const Inst& in : insts_) ++const_cast<Model*>(in.m)->instances;
    if (streamVram_ || !vramOn) return;  // Streaming places eDRAM incrementally; vramOn false: RAM only
    pp::vramReset();
    moveToVram();
}

bool ModelRenderer::setBatchState(const Model& m, const ModelBatch& b) {
    const pp::Texture* tex = texture(b.texture);
    if (!tex && b.texture != kNoTexture) {
        // PSP (in the emulator: black squares in a busy village): the batch's texture is not resident (its
        // read found no memory) - skipped, not drawn untextured; queued for the streamer's retry, logged once.
        if (b.texture < textures_.size() && !textures_[b.texture].missing) {
            Tex& t = textures_[b.texture];
            t.missing = true;
            if (t.retries < kTexRetries) texMissing_.push_back(b.texture);  // then it stays skipped (not in the pak?)
            if (!t.logged) {
                t.logged = true;
                const size_t mi = (&m >= models_.data() && &m < models_.data() + models_.size())
                                      ? static_cast<size_t>(&m - models_.data()) : 0xFFFFu;
                if (++texMissingLogged_ <= 40)
                    PSP_LOGW(pp::tag::Gfx, "tex_missing model=%u tex=%u r=%s tried=%d n=%u", static_cast<unsigned>(mi),
                             static_cast<unsigned>(b.texture), logTag, t.tried ? 1 : 0,
                             static_cast<unsigned>(texMissingLogged_));
            }
        }
        return false;
    }
    pp::textureBind(tex);
    sceGuTexScale(m.h->uvScale[0], m.h->uvScale[1]);
    sceGuTexOffset(m.h->uvMin[0], m.h->uvMin[1]);
    sceGuTexWrap((b.flags & kBatchWrapU) ? GU_REPEAT : GU_CLAMP, (b.flags & kBatchWrapV) ? GU_REPEAT : GU_CLAMP);
    lightBatch((b.flags & kBatchUnlit) != 0);  // Lighting (unlit WMO materials keep their colour)
    switch (b.blend) {
        case kBlendOpaque: sceGuDisable(GU_ALPHA_TEST); break;
        case kBlendAlphaKey:
            sceGuEnable(GU_ALPHA_TEST);
            sceGuAlphaFunc(GU_GREATER, 0x66, 0xFF);  // upstream m2.frag: 0.4
            break;
        case kBlendAlpha: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0); break;
        case kBlendAdd: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFFu); break;
        default: sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0); break;  // modulate
    }
    return true;
}

void ModelRenderer::drawBatch(const Model& m, const ModelBatch& b) {
    prof::count(prof::kDraws, 1);
    prof::count(prof::kVertices, b.indexCount);
    if (m.h->flags & kModelStrips)
        sceGuDrawArray(GU_TRIANGLE_STRIP, kStripType, static_cast<int>(b.indexCount), nullptr, m.verts + b.indexStart);
    else
        sceGuDrawArray(GU_TRIANGLES, kVertexType, static_cast<int>(b.indexCount), m.indices + b.indexStart, m.verts);
    ++stats_.batches;
    const uint32_t tris = (m.h->flags & kModelStrips) ? (b.indexCount > 2 ? b.indexCount - 2 : 0) : b.indexCount / 3;
    stats_.triangles += tris;
    prof::count(prof::kTriangles, tris);
}

// One instance, every batch of one pass (blended pass: far to near).
void ModelRenderer::drawModel(const Visible& v, bool blendPass) {
    const ModelHeader& h = *v.m->h;
    bool matrixSet = false;
    uint32_t first, count;
    lodRange(h, v.lod, first, count);
    for (uint32_t i = first; i < first + count; ++i) {
        const ModelBatch& b = v.m->batches[i];
        if (isBlended(b.blend) != blendPass) continue;
        if (!matrixSet) {
            sceGuSetMatrix(GU_MODEL, &v.mtx);
            matrixSet = true;
        }
        if (!setBatchState(*v.m, b)) continue;  // Its texture is missing (tex_missing)
        drawBatch(*v.m, b);
    }
}

// Vertex data of the most used models into eDRAM (SPP_EXP), until full.
void ModelRenderer::moveToVram() {
    std::vector<Model*> byUse;
    for (Model& m : models_)
        if (m.h && m.instances) byUse.push_back(&m);
    std::sort(byUse.begin(), byUse.end(), [](const Model* a, const Model* b) {
        return a->instances * a->h->vertexCount > b->instances * b->h->vertexCount;
    });
    uint32_t moved = 0;
    for (Model* m : byUse) {
        const uint32_t bytes = m->h->vertexCount * sizeof(ModelVertex);
        const void* v = pp::vramStore(m->verts, bytes);
        if (!v) break;
        m->verts = static_cast<const ModelVertex*>(v);
        ++moved;
    }
    PSP_LOGI(pp::tag::Gfx, "m2_vram models=%u of %u kb=%u", static_cast<unsigned>(moved),
             static_cast<unsigned>(byUse.size()), static_cast<unsigned>(pp::vramUsedBytes() / 1024u));
}

void ModelRenderer::draw(const TerrainCamera& cam, uint32_t fogColorABGR) {
    // cpufast=0 keeps the build-104 pass (A/B on the same build).
    ++g_cpuStats.frames;
    if (g_cpuFast) drawFast(cam, fogColorABGR);
    else drawOld(cam, fogColorABGR);
}

// The build-104 pass, unchanged (cpufast=0).
void ModelRenderer::drawOld(const TerrainCamera& cam, uint32_t fogColorABGR) {
    stats_.drawn = stats_.culled = stats_.lowLod = stats_.batches = stats_.triangles = 0;
    stats_.lod2 = stats_.impostors = stats_.impostorDraws = 0;
    stats_.guardNear = 0;  // no guard test in this pass (m2_guard reads 0 under cpufast=0)
    impostorList_.clear();
    if (instances_.empty()) return;

    const float aspect = static_cast<float>(pp::kScreenW) / pp::kScreenH;
    const glm::mat4 proj = glm::perspective(glm::radians(cam.fovyDeg), aspect, cam.nearZ, cam.farZ);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), cam.forward, cam.up);
    const Frustum fr = makeFrustum(proj * view);
    const float farLimit = cam.farZ * 0.95f;

    PROF_SCOPE("m2");
    PROF_GE_PASS("m2");
    if (gridDirty_) buildGrid();
    static const uint16_t kCullZone = prof::zoneId("m2.cull");
    prof::zoneEnter(kCullZone);
    visible_.clear();
    for (const Cell& cell : cells_) {
        const glm::vec3 cc(cell.x - cam.pos.x, cell.y - cam.pos.y, cell.z - cam.pos.z);
        const float cellReach = std::min(viewDistance * cell.maxReach, farLimit) + cell.r;
        if (glm::dot(cc, cc) > cellReach * cellReach || fr.sphereOutside(cc, cell.r)) {
            stats_.culled += cell.count;
            continue;
        }
        for (uint32_t i = cell.first; i < cell.first + cell.count; ++i) {
            const Inst& in = insts_[i];
            const glm::vec3 c(in.x - cam.pos.x, in.y - cam.pos.y, in.z - cam.pos.z);
            const float reach = std::min(viewDistance * in.reach, farLimit) + in.r;
            const float d2 = glm::dot(c, c);
            if (d2 > reach * reach || fr.sphereOutside(c, in.r)) {
                ++stats_.culled;
                continue;
            }
            if (groupHidden(in.p->reserved)) {  // a big WMO's doodad, its group not visible
                ++stats_.culled;
                continue;
            }
            const float dist = std::max(0.0f, std::sqrt(d2) - in.r);
            // PSP (forest LOD): far trees become impostors (drawn after
            // the opaque pass), the rest pick LOD 0 / 1 / 2 (psp_impostors.cpp).
            if (in.m->imp && pickImpostor(in, dist)) continue;
            const uint8_t lod = pickLod(in, dist);
            Visible v{in.p, in.m, dist, lod, {}};
            // Model matrix: placement axes scaled by the s16 half extent,
            // translation = the model centre, camera-relative.
            const DoodadPlacement& p = *in.p;
            const ModelHeader& h = *in.m->h;
            const glm::vec3 ax(p.axes[0], p.axes[1], p.axes[2]);
            const glm::vec3 ay(p.axes[3], p.axes[4], p.axes[5]);
            const glm::vec3 az(p.axes[6], p.axes[7], p.axes[8]);
            const glm::vec3 t = glm::vec3(p.position[0], p.position[1], p.position[2]) + ax * h.center[0] +
                                ay * h.center[1] + az * h.center[2] - cam.pos;
            v.mtx.x = {ax.x * h.half[0], ax.y * h.half[0], ax.z * h.half[0], 0.0f};
            v.mtx.y = {ay.x * h.half[1], ay.y * h.half[1], ay.z * h.half[1], 0.0f};
            v.mtx.z = {az.x * h.half[2], az.y * h.half[2], az.z * h.half[2], 0.0f};
            v.mtx.w = {t.x, t.y, t.z, 1.0f};
            visible_.push_back(v);
        }
    }
    prof::zoneLeave(kCullZone);
    // Nearest first: fewer overdrawn pixels, and the cap drops the far ones.
    std::sort(visible_.begin(), visible_.end(), [](const Visible& a, const Visible& b) { return a.dist < b.dist; });

    pp::textureResetBinding();
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);  // M2 batches are two-sided (upstream: cull none)
    sceGuShadeModel(GU_SMOOTH);
    lightBegin(cam, fogColorABGR);  // Lighting (fog from the bands, light tint)
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);

    // Opaque pass grouped by model and LOD: each batch's texture, CLUT and
    // state go to the GE once, then every instance only sends its matrix and
    // a draw (hardware: ~16 us CPU per batch when set per instance).
    // The triangle cap still keeps the nearest instances (visible_ is sorted).
    size_t drawn = 0;
    uint32_t capTris = 0;
    bool listFull = false;  // The display list's reserve (psp_display.hpp)
    for (; drawn < visible_.size() && capTris < triangleCap; ++drawn) {
        const Visible& v = visible_[drawn];
        uint32_t first, count;
        lodRange(*v.m->h, v.lod, first, count);
        for (uint32_t i = first; i < first + count; ++i) capTris += v.m->batches[i].indexCount;
        if (v.lod == 1) ++stats_.lowLod;
        else if (v.lod == 2) ++stats_.lod2;
    }
    order_.resize(drawn);
    for (size_t i = 0; i < drawn; ++i) order_[i] = static_cast<uint32_t>(i);
    std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        const Visible& va = visible_[a];
        const Visible& vb = visible_[b];
        return va.m != vb.m ? va.m < vb.m : va.lod < vb.lod;
    });
    for (size_t g = 0; g < order_.size();) {
        const Visible& head = visible_[order_[g]];
        size_t end = g + 1;
        while (end < order_.size() && visible_[order_[end]].m == head.m && visible_[order_[end]].lod == head.lod) ++end;
        uint32_t first, count;
        lodRange(*head.m->h, head.lod, first, count);
        for (uint32_t i = first; i < first + count; ++i) {
            const ModelBatch& b = head.m->batches[i];
            if (isBlended(b.blend)) continue;
            // Never into the display list's reserve for the NPCs and the interface (psp_display.hpp).
            if (pp::displayListFree() < pp::kListReserveM2) {
                listFull = true;
                break;
            }
            if (!setBatchState(*head.m, b)) continue;  // Its texture is missing (tex_missing)
            for (size_t k = g; k < end; ++k) {
                sceGuSetMatrix(GU_MODEL, &visible_[order_[k]].mtx);
                drawBatch(*head.m, b);
            }
        }
        if (listFull) break;
        g = end;
    }
    stats_.drawn = static_cast<uint32_t>(drawn);
    stats_.culled += static_cast<uint32_t>(visible_.size() - drawn);
    drawImpostors(cam);  // alpha-tested like the opaque pass, before the blended one

    // Blended batches: far to near, depth test on, depth writes off.
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    for (size_t i = drawn; i-- > 0;) {
        if (pp::displayListFree() < pp::kListReserveM2) {
            listFull = true;
            continue;
        }
        drawModel(visible_[i], true);
    }
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    if (listFull) pp::displayListFull("m2");

    lightEnd();  // Lighting
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();
}

// ---------------------------------------------------------------------------
// the same pass as drawOld - the same GE list
// words in the same order (tools/psp/host_tests/frame_cpu_test.cpp compares
// them) - with less CPU:
//  - a cell wholly inside the frustum (1 yd margin) skips its doodads' six
//    plane tests: each doodad sphere lies inside the cell sphere, so
//    sphereOutside could only answer "no";
//  - the matrices go to mtxFast_ and the sort moves 16-byte records instead
//    of 80-byte ones; std::sort's steps depend only on the comparator's
//    answers, so the order (ties included) is the one drawOld gets;
//  - the blended pass skips instances whose LOD has no blended batch (drawModel
//    sent nothing for them);
//  - opaque instance draws go out without sceGuDrawArray's stall-address
//    syscall, except every 8th and the last of a run (drawBatchNoStall).

namespace {

constexpr float kInsideMargin = 1.0f;  // yd: covers the rounding of camera-relative centres
constexpr uint32_t kStallEvery = 8;

bool sphereInside(const Frustum& f, const glm::vec3& c, float r) {
    for (const auto& p : f.planes)
        if (glm::dot(glm::vec3(p), c) + p.w < r) return false;
    return true;
}

} // namespace

void ModelRenderer::drawBatchNoStall(const Model& m, const ModelBatch& b, bool flush) {
    if (flush) {
        drawBatch(m, b);
        return;
    }
    // The words sceGuDrawArray writes (pspsdk libgu: VTYPE, BASE + IADDR with
    // indices, BASE + VADDR, PRIM), without its sceGeListUpdateStallAddr
    // syscall: the GE runs up to them at the next draw that flushes.
    prof::count(prof::kDraws, 1);
    prof::count(prof::kVertices, b.indexCount);
    const bool strips = (m.h->flags & kModelStrips) != 0;
    const int prim = strips ? GU_TRIANGLE_STRIP : GU_TRIANGLES;
    sceGuSendCommandi(18, strips ? kStripType : kVertexType);
    if (!strips) {
        const uint32_t a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m.indices + b.indexStart));
        sceGuSendCommandi(16, static_cast<int>((a >> 8) & 0xF0000u));
        sceGuSendCommandi(2, static_cast<int>(a & 0xFFFFFFu));
    }
    const uint32_t a = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(strips ? static_cast<const void*>(m.verts + b.indexStart) : static_cast<const void*>(m.verts)));
    sceGuSendCommandi(16, static_cast<int>((a >> 8) & 0xF0000u));
    sceGuSendCommandi(1, static_cast<int>(a & 0xFFFFFFu));
    sceGuSendCommandi(4, (prim << 16) | static_cast<int>(b.indexCount));
    ++stats_.batches;
    const uint32_t tris = strips ? (b.indexCount > 2 ? b.indexCount - 2 : 0) : b.indexCount / 3;
    stats_.triangles += tris;
    prof::count(prof::kTriangles, tris);
    ++g_cpuStats.drawsNoStall;
}

void ModelRenderer::drawFast(const TerrainCamera& cam, uint32_t fogColorABGR) {
    stats_.drawn = stats_.culled = stats_.lowLod = stats_.batches = stats_.triangles = 0;
    stats_.lod2 = stats_.impostors = stats_.impostorDraws = 0;
    stats_.guardNear = 0;
    impostorList_.clear();
    if (instances_.empty()) return;

    const float aspect = static_cast<float>(pp::kScreenW) / pp::kScreenH;
    const glm::mat4 proj = glm::perspective(glm::radians(cam.fovyDeg), aspect, cam.nearZ, cam.farZ);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), cam.forward, cam.up);
    const Frustum fr = makeFrustum(proj * view);
    const float farLimit = cam.farZ * 0.95f;
    // camera axes (as the view: lookAt from the origin) and the focal length for ge::sphereSafe;
    // no guard test on a degenerate basis (no NaN reaches a compare - the FPU traps on them).
    const glm::vec3 side = glm::cross(cam.forward, cam.up);
    const float sideLen = glm::length(side);
    const bool guardOn = guardLod != 0 && sideLen > 1e-4f;
    const glm::vec3 camRight = guardOn ? side / sideLen : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 camUp = glm::cross(camRight, cam.forward);
    const float focal = ge::focalPx(cam.fovyDeg, static_cast<float>(pp::kScreenH));

    PROF_SCOPE("m2");
    PROF_GE_PASS("m2");
    if (gridDirty_) buildGrid();
    static const uint16_t kCullZone = prof::zoneId("m2.cull");
    prof::zoneEnter(kCullZone);
    visFast_.clear();
    mtxFast_.clear();
    for (const Cell& cell : cells_) {
        const glm::vec3 cc(cell.x - cam.pos.x, cell.y - cam.pos.y, cell.z - cam.pos.z);
        const float cellReach = std::min(viewDistance * cell.maxReach, farLimit) + cell.r;
        if (glm::dot(cc, cc) > cellReach * cellReach || fr.sphereOutside(cc, cell.r)) {
            stats_.culled += cell.count;
            continue;
        }
        const bool inside = sphereInside(fr, cc, cell.r + kInsideMargin);
        for (uint32_t i = cell.first; i < cell.first + cell.count; ++i) {
            const Inst& in = insts_[i];
            const glm::vec3 c(in.x - cam.pos.x, in.y - cam.pos.y, in.z - cam.pos.z);
            const float reach = std::min(viewDistance * in.reach, farLimit) + in.r;
            const float d2 = glm::dot(c, c);
            if (d2 > reach * reach || (!inside && fr.sphereOutside(c, in.r))) {
                ++stats_.culled;
                continue;
            }
            if (groupHidden(in.p->reserved)) {  // as drawOld
                ++stats_.culled;
                continue;
            }
            const float dist = std::max(0.0f, std::sqrt(d2) - in.r);
            if (in.m->imp && pickImpostor(in, dist)) continue;
            // past the 12-yd rule a near-split model's unsplit mesh is only safe while its sphere
            // stays in front of the near plane and inside the GE window (psp_ge_limits.hpp); else the split LOD 0.
            float lodDist = dist;
            if (guardOn && dist > 0.0f && (in.m->h->flags & kModelNearSplit) &&
                !ge::sphereSafe(glm::dot(c, camRight), glm::dot(c, camUp), glm::dot(c, cam.forward), in.r, cam.nearZ,
                                focal)) {
                if (pickLod(in, dist) != 0) ++stats_.guardNear;
                lodDist = 0.0f;
            }
            const uint8_t lod = pickLod(in, lodDist);
            const DoodadPlacement& p = *in.p;
            const ModelHeader& h = *in.m->h;
            const glm::vec3 ax(p.axes[0], p.axes[1], p.axes[2]);
            const glm::vec3 ay(p.axes[3], p.axes[4], p.axes[5]);
            const glm::vec3 az(p.axes[6], p.axes[7], p.axes[8]);
            const glm::vec3 t = glm::vec3(p.position[0], p.position[1], p.position[2]) + ax * h.center[0] +
                                ay * h.center[1] + az * h.center[2] - cam.pos;
            ScePspFMatrix4 mtx;
            mtx.x = {ax.x * h.half[0], ax.y * h.half[0], ax.z * h.half[0], 0.0f};
            mtx.y = {ay.x * h.half[1], ay.y * h.half[1], ay.z * h.half[1], 0.0f};
            mtx.z = {az.x * h.half[2], az.y * h.half[2], az.z * h.half[2], 0.0f};
            mtx.w = {t.x, t.y, t.z, 1.0f};
            visFast_.push_back(VisFast{in.m, dist, static_cast<uint32_t>(mtxFast_.size()), lod});
            mtxFast_.push_back(mtx);
        }
    }
    prof::zoneLeave(kCullZone);
    std::sort(visFast_.begin(), visFast_.end(), [](const VisFast& a, const VisFast& b) { return a.dist < b.dist; });

    pp::textureResetBinding();
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);  // M2 batches are two-sided (upstream: cull none)
    sceGuShadeModel(GU_SMOOTH);
    lightBegin(cam, fogColorABGR);  // Lighting (fog from the bands, light tint)
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);

    size_t drawn = 0;
    uint32_t capTris = 0;
    bool listFull = false;  // The display list's reserve (psp_display.hpp)
    for (; drawn < visFast_.size() && capTris < triangleCap; ++drawn) {
        const VisFast& v = visFast_[drawn];
        uint32_t first, count;
        lodRange(*v.m->h, v.lod, first, count);
        for (uint32_t i = first; i < first + count; ++i) capTris += v.m->batches[i].indexCount;
        if (v.lod == 1) ++stats_.lowLod;
        else if (v.lod == 2) ++stats_.lod2;
    }
    order_.resize(drawn);
    for (size_t i = 0; i < drawn; ++i) order_[i] = static_cast<uint32_t>(i);
    std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        const VisFast& va = visFast_[a];
        const VisFast& vb = visFast_[b];
        return va.m != vb.m ? va.m < vb.m : va.lod < vb.lod;
    });
    for (size_t g = 0; g < order_.size();) {
        const VisFast& head = visFast_[order_[g]];
        size_t end = g + 1;
        while (end < order_.size() && visFast_[order_[end]].m == head.m && visFast_[order_[end]].lod == head.lod) ++end;
        uint32_t first, count;
        lodRange(*head.m->h, head.lod, first, count);
        for (uint32_t i = first; i < first + count; ++i) {
            const ModelBatch& b = head.m->batches[i];
            if (isBlended(b.blend)) continue;
            if (pp::displayListFree() < pp::kListReserveM2) {  // Before a batch (its group stalls at the end)
                listFull = true;
                break;
            }
            if (!setBatchState(*head.m, b)) continue;  // Its texture is missing (tex_missing)
            for (size_t k = g; k < end; ++k) {
                sceGuSetMatrix(GU_MODEL, &mtxFast_[visFast_[order_[k]].mi]);
                drawBatchNoStall(*head.m, b, (k - g) % kStallEvery == kStallEvery - 1 || k + 1 == end);
            }
        }
        if (listFull) break;
        g = end;
    }
    stats_.drawn = static_cast<uint32_t>(drawn);
    stats_.culled += static_cast<uint32_t>(visFast_.size() - drawn);
    drawImpostors(cam);  // alpha-tested like the opaque pass, before the blended one

    // Blended batches: far to near, depth test on, depth writes off.
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    for (size_t k = drawn; k-- > 0;) {
        const VisFast& v = visFast_[k];
        if (!(v.m->blendMask & (1u << v.lod))) continue;  // drawModel would send nothing
        if (pp::displayListFree() < pp::kListReserveM2) {
            listFull = true;
            continue;
        }
        bool matrixSet = false;
        uint32_t first, count;
        lodRange(*v.m->h, v.lod, first, count);
        for (uint32_t i = first; i < first + count; ++i) {
            const ModelBatch& b = v.m->batches[i];
            if (!isBlended(b.blend)) continue;
            if (!matrixSet) {
                sceGuSetMatrix(GU_MODEL, &mtxFast_[v.mi]);
                matrixSet = true;
            }
            if (!setBatchState(*v.m, b)) continue;  // Its texture is missing (tex_missing)
            drawBatch(*v.m, b);
        }
    }
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    if (listFull) pp::displayListFull("m2");

    lightEnd();  // Lighting
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();
}


namespace {

constexpr float kMinWalkableNormalZ = 0.642787635f;  // cos 50 deg (upstream movement_limits.hpp)
constexpr float kPlayerRadius = 0.5f;
constexpr float kPlayerHeight = 2.0f;
constexpr float kWallStepHeight = 1.0f;  // upstream checkWallCollision MAX_STEP_HEIGHT

struct ColView {
    const CollisionHeader* h;
    const float* v;
    const CollisionTri* t;
    const uint32_t* cells;
    const uint32_t* list;
    glm::vec3 vert(uint16_t i) const { return glm::vec3(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]); }
};

ColView view(const CollisionHeader* h) {
    const auto* b = reinterpret_cast<const uint8_t*>(h);
    return ColView{h, reinterpret_cast<const float*>(b + h->vertexOffset),
                   reinterpret_cast<const CollisionTri*>(b + h->triangleOffset),
                   reinterpret_cast<const uint32_t*>(b + h->cellOffset),
                   reinterpret_cast<const uint32_t*>(b + h->listOffset)};
}

// Calls fn(triIndex) for triangles in the grid cells overlapping [lo, hi] (model XY).
template <typename Fn>
void forCells(const ColView& c, glm::vec2 lo, glm::vec2 hi, Fn&& fn) {
    const CollisionHeader& h = *c.h;
    const int x0 = std::max(0, static_cast<int>((lo.x - h.bmin[0]) / h.cell));
    const int x1 = std::min(h.nx - 1, static_cast<int>((hi.x - h.bmin[0]) / h.cell));
    const int y0 = std::max(0, static_cast<int>((lo.y - h.bmin[1]) / h.cell));
    const int y1 = std::min(h.ny - 1, static_cast<int>((hi.y - h.bmin[1]) / h.cell));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const uint32_t* cell = c.cells + (static_cast<uint32_t>(y) * h.nx + x) * 2u;
            for (uint32_t i = 0; i < cell[1]; ++i) fn(c.list[cell[0] + i]);
        }
    }
}

glm::mat3 axesOf(const DoodadPlacement& p) {
    return glm::mat3(glm::vec3(p.axes[0], p.axes[1], p.axes[2]), glm::vec3(p.axes[3], p.axes[4], p.axes[5]),
                     glm::vec3(p.axes[6], p.axes[7], p.axes[8]));
}

// Moller-Trumbore; returns t along d (d need not be unit), or < 0 on a miss.
float rayTri(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e1 = b - a, e2 = c - a;
    const glm::vec3 pv = glm::cross(d, e2);
    const float det = glm::dot(e1, pv);
    if (std::fabs(det) < 1e-9f) return -1.0f;
    const float inv = 1.0f / det;
    const glm::vec3 tv = o - a;
    const float u = glm::dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const glm::vec3 qv = glm::cross(tv, e1);
    const float v = glm::dot(d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    return glm::dot(e2, qv) * inv;
}

glm::vec3 closestOnTri(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    // Ericson, Real-Time Collision Detection 5.1.5.
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// the float bits of a query input (memo keys: -0 and NaN as they are).
uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

constexpr float kCellMargin = 1.0f;  // yd: cell tests stay conservative under rounding

} // namespace

// the doodads of the lattice slots around a query, in
// insts_ order (slots ascending = cells_ ascending = insts_ ascending). A
// cell outside the box grown by gridSpill_ cannot hold a doodad the query
// accepts (see buildGrid), cellOk is conservative: the doodads left out are
// exactly ones the old scan's own test rejects, the rest see the same test.
template <class CellOk, class Fn>
void ModelRenderer::forGrid(float x0, float y0, float x1, float y1, CellOk&& cellOk, Fn&& fn) const {
    auto slot = [](float v, float lo) { return static_cast<int>(std::floor((v - lo) / 64.0f)); };
    const int ix0 = std::max(0, slot(x0, gridMinX_) - gridSpill_);
    const int ix1 = std::min(gridNx_ - 1, slot(x1, gridMinX_) + gridSpill_);
    const int iy0 = std::max(0, slot(y0, gridMinY_) - gridSpill_);
    const int iy1 = std::min(gridNy_ - 1, slot(y1, gridMinY_) + gridSpill_);
    for (int ix = ix0; ix <= ix1; ++ix) {
        for (int iy = iy0; iy <= iy1; ++iy) {
            const uint16_t k = lattice_[static_cast<size_t>(ix) * gridNy_ + iy];
            if (!k) continue;
            const Cell& cell = cells_[k - 1u];
            ++g_cpuStats.colCells;
            if (!cellOk(cell)) continue;
            for (uint32_t i = cell.first; i < cell.first + cell.count; ++i) fn(insts_[i]);
        }
    }
}

// The lattice only for finite input it was built for (box within +-2^20 yd)
// and a unit direction (the sqrt 2 reach above assumes |dir| = 1).
bool ModelRenderer::rayGridOk(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const {
    if (!g_cpuFast || lattice_.empty() || !(maxDist >= 0.0f) || !(maxDist < 1.0e5f)) return false;
    for (int i = 0; i < 3; ++i)
        if (!(std::fabs(origin[i]) < 1.0e6f) || !(std::fabs(dir[i]) <= 1.01f)) return false;
    const float len2 = glm::dot(dir, dir);
    return len2 > 0.999f && len2 < 1.001f;
}

bool ModelRenderer::floorHeight(float x, float y, float probeZ, float& outZ) const {
    PROF_SCOPE("col.floor");
    ++g_cpuStats.colQueries;
    bool found = false;
    float best = -1e30f;
    // same input, same world -> the last answer.
    const uint32_t key[3] = {bitsOf(x), bitsOf(y), bitsOf(probeZ)};
    if (g_cpuFast) {
        for (const FloorMemo& m : floorMemo_) {
            if (m.gen == gridGen_ && std::memcmp(m.key, key, sizeof(key)) == 0) {
                ++g_cpuStats.colMemoHits;
                if (m.found) outZ = m.z;
                return m.found;
            }
        }
    }
    auto visit = [&](const Inst& in) {  // the build-104 loop body
        ++g_cpuStats.colInsts;
        if (!in.m->col) return;
        const float dx = in.x - x, dy = in.y - y;
        if (dx * dx + dy * dy > in.r * in.r || in.z - in.r > probeZ) return;
        const DoodadPlacement& p = *in.p;
        const glm::mat3 A = axesOf(p);
        const glm::mat3 Ai = glm::inverse(A);
        const glm::vec3 pos(p.position[0], p.position[1], p.position[2]);
        const glm::vec3 o = Ai * (glm::vec3(x, y, probeZ) - pos);
        const glm::vec3 d = Ai * glm::vec3(0.0f, 0.0f, -1.0f);
        const float maxT = probeZ - (in.z - in.r);
        const glm::vec3 e = o + d * maxT;
        const ColView c = view(in.m->col);
        forCells(c, glm::min(glm::vec2(o), glm::vec2(e)), glm::max(glm::vec2(o), glm::vec2(e)), [&](uint32_t ti) {
            const CollisionTri& t = c.t[ti];
            const glm::vec3 a = c.vert(t.a), b = c.vert(t.b), cc = c.vert(t.c);
            const float hit = rayTri(o, d, a, b, cc);
            if (hit < 0.0f || hit > maxT) return;
            const float z = probeZ - hit;
            if (z <= best) return;
            const glm::vec3 n = glm::cross(A * (b - a), A * (cc - a));
            const float nl = glm::length(n);
            if (nl < 1e-9f || std::fabs(n.z) / nl < kMinWalkableNormalZ) return;  // too steep to stand on
            best = z;
            found = true;
        });
    };
    const bool finite = std::fabs(x) < 1.0e6f && std::fabs(y) < 1.0e6f && std::fabs(probeZ) < 1.0e6f;
    if (g_cpuFast && finite && !lattice_.empty()) {
        // A doodad passing the test above holds (x, y) in its disk and
        // reaches down to probeZ - so does its cell's sphere.
        forGrid(x, y, x, y,
                [&](const Cell& c) {
                    const float dx = c.x - x, dy = c.y - y, rm = c.r + kCellMargin;
                    return !(dx * dx + dy * dy > rm * rm || c.z - rm > probeZ);
                },
                visit);
    } else {
        for (const Inst& in : insts_) visit(in);
    }
    if (g_cpuFast) {
        FloorMemo& m = floorMemo_[floorMemoNext_];
        floorMemoNext_ = static_cast<uint8_t>((floorMemoNext_ + 1u) % 4u);
        std::memcpy(m.key, key, sizeof(key));
        m.gen = gridGen_;
        m.z = best;
        m.found = found;
    }
    if (found) outZ = best;
    return found;
}

float ModelRenderer::raycastBuildings(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const {
    PROF_SCOPE("col.camera");
    ++g_cpuStats.colQueries;
    float best = maxDist;
    // same input, same world -> the last answer (key[8] = 0: buildings).
    const uint32_t key[9] = {bitsOf(origin.x), bitsOf(origin.y), bitsOf(origin.z), bitsOf(dir.x), bitsOf(dir.y),
                             bitsOf(dir.z),    bitsOf(maxDist),  0u,               0u};
    if (g_cpuFast) {
        for (const RayMemo& m : rayMemo_) {
            if (m.gen == gridGen_ && std::memcmp(m.key, key, sizeof(key)) == 0) {
                ++g_cpuStats.colMemoHits;
                return m.best;
            }
        }
    }
    auto visit = [&](const Inst& in) {  // the build-104 loop body
        ++g_cpuStats.colInsts;
        if (!in.m->col || !(in.m->h->flags & kModelBuilding)) return;
        // Ray vs bounding sphere first.
        const glm::vec3 oc = glm::vec3(in.x, in.y, in.z) - origin;
        const float tc = glm::dot(oc, dir);
        const float d2 = glm::dot(oc, oc) - tc * tc;
        if (d2 > in.r * in.r || tc + in.r < 0.0f || tc - in.r > best) return;
        const DoodadPlacement& p = *in.p;
        const glm::mat3 A = axesOf(p);
        const glm::mat3 Ai = glm::inverse(A);
        const glm::vec3 pos(p.position[0], p.position[1], p.position[2]);
        const glm::vec3 o = Ai * (origin - pos);
        const glm::vec3 d = Ai * dir;  // t stays in world yards
        const glm::vec3 e = o + d * best;
        const ColView c = view(in.m->col);
        forCells(c, glm::min(glm::vec2(o), glm::vec2(e)), glm::max(glm::vec2(o), glm::vec2(e)), [&](uint32_t ti) {
            const CollisionTri& t = c.t[ti];
            const float hit = rayTri(o, d, c.vert(t.a), c.vert(t.b), c.vert(t.c));
            if (hit > 0.0f && hit < best) best = hit;
        });
    };
    if (rayGridOk(origin, dir, maxDist)) {
        // A sphere passing the ray test above lies within its cell's sphere,
        // which then passes the same test (unit dir, any best).
        const glm::vec3 end = origin + dir * maxDist;
        forGrid(std::min(origin.x, end.x), std::min(origin.y, end.y), std::max(origin.x, end.x),
                std::max(origin.y, end.y),
                [&](const Cell& cl) {
                    const glm::vec3 oc = glm::vec3(cl.x, cl.y, cl.z) - origin;
                    const float tc = glm::dot(oc, dir);
                    const float d2 = glm::dot(oc, oc) - tc * tc;
                    const float rm = cl.r + kCellMargin;
                    return !(d2 > rm * rm || tc + rm < 0.0f || tc - rm > best);
                },
                visit);
    } else {
        for (const Inst& in : insts_) visit(in);
    }
    if (g_cpuFast) {
        RayMemo& m = rayMemo_[rayMemoNext_];
        rayMemoNext_ = static_cast<uint8_t>((rayMemoNext_ + 1u) % 6u);
        std::memcpy(m.key, key, sizeof(key));
        m.gen = gridGen_;
        m.best = best;
    }
    return best;
}

void ModelRenderer::collideWalls(glm::vec3& to, float feetZ) const {
    PROF_SCOPE("col.walls");
    ++g_cpuStats.colQueries;
    // `to` moves while the doodads push it, so no lattice
    // box here: every cell in order, each with a conservative test against
    // the current `to` (a cell left out holds only doodads the test below
    // rejects at that moment, and nothing moves `to` in between).
    auto cellOk = [&](const Cell& c) {
        const float dx = c.x - to.x, dy = c.y - to.y, rm = c.r + kCellMargin;
        const float reach = rm + kPlayerRadius;
        return !(dx * dx + dy * dy > reach * reach || feetZ + kPlayerHeight < c.z - rm || feetZ > c.z + rm);
    };
    auto visit = [&](const Inst& in) {  // the build-104 loop body
        ++g_cpuStats.colInsts;
        if (!in.m->col) return;
        const float dx = in.x - to.x, dy = in.y - to.y;
        const float reach = in.r + kPlayerRadius;
        if (dx * dx + dy * dy > reach * reach) return;
        if (feetZ + kPlayerHeight < in.z - in.r || feetZ > in.z + in.r) return;
        const DoodadPlacement& p = *in.p;
        const glm::mat3 A = axesOf(p);
        const glm::mat3 Ai = glm::inverse(A);
        const glm::vec3 pos(p.position[0], p.position[1], p.position[2]);
        const float scale = std::max(0.01f, glm::length(A[0]));
        // Step-over height: buildings keep upstream's 1.0 (WMO
        // checkWallCollision); props only what the feet can step (0.6) -
        // a 1.28 yd stone fence on a 0.4 yd slope was walked through
        // (on the hardware).
        const float stepOver = (in.m->h->flags & kModelBuilding) ? kWallStepHeight : 0.6f;
        const glm::vec3 local = Ai * (to - pos);
        const float margin = (kPlayerRadius + kPlayerHeight) / scale;
        const ColView c = view(in.m->col);
        forCells(c, glm::vec2(local) - margin, glm::vec2(local) + margin, [&](uint32_t ti) {
            const CollisionTri& t = c.t[ti];
            if (!(t.flags & kColBlocksWalls)) return;
            const glm::vec3 a = A * c.vert(t.a) + pos, b = A * c.vert(t.b) + pos, cc = A * c.vert(t.c) + pos;
            const float minZ = std::min({a.z, b.z, cc.z}), maxZ = std::max({a.z, b.z, cc.z});
            if (maxZ <= feetZ + stepOver || minZ >= feetZ + kPlayerHeight) return;  // step over / above head
            if (maxZ - minZ < 1.0f && maxZ <= feetZ + 1.2f) return;                        // stair riser
            const glm::vec3 n = glm::cross(b - a, cc - a);
            const float nl = glm::length(n);
            if (nl < 1e-9f || std::fabs(n.z) / nl >= kMinWalkableNormalZ) return;  // floors are not walls
            // Sample the cylinder at knee and chest height.
            for (float h : {stepOver + 0.1f, 1.7f}) {
                const glm::vec3 q(to.x, to.y, feetZ + h);
                const glm::vec3 cp = closestOnTri(q, a, b, cc);
                glm::vec2 push(q.x - cp.x, q.y - cp.y);
                if (std::fabs(cp.z - q.z) > 0.5f) continue;
                float dist = glm::length(push);
                if (dist >= kPlayerRadius) continue;
                if (dist < 1e-4f) {
                    push = glm::vec2(n.x, n.y);
                    dist = glm::length(push);
                    if (dist < 1e-6f) continue;
                    push /= dist;
                    dist = 0.0f;
                } else {
                    push /= dist;
                }
                to.x += push.x * (kPlayerRadius - dist);
                to.y += push.y * (kPlayerRadius - dist);
            }
        });
    };
    for (int iter = 0; iter < 2; ++iter) {
        const bool finite = std::fabs(to.x) < 1.0e6f && std::fabs(to.y) < 1.0e6f && std::fabs(feetZ) < 1.0e6f;
        if (g_cpuFast && finite) {
            for (const Cell& c : cells_) {
                ++g_cpuStats.colCells;
                if (!cellOk(c)) continue;
                for (uint32_t i = c.first; i < c.first + c.count; ++i) visit(insts_[i]);
            }
        } else {
            for (const Inst& in : insts_) visit(in);
        }
    }
}

// ---------------------------------------------------------------------------
// Tile streaming (src/ui/psp/ui_world_streaming.cpp). Main thread,
// between frames only.

namespace {
uint32_t streamTileKey(int x, int y) {
    return (static_cast<uint32_t>(x & 0xFFFF) << 16) | static_cast<uint32_t>(y & 0xFFFF);
}
uint32_t entrySize(const pp::Pak& pak, const char* prefix, unsigned id) {
    char name[32];
    std::snprintf(name, sizeof(name), "%s%u", prefix, id);
    const pp::PakEntry* e = pak.find(name);
    return e ? e->size : 0u;
}
} // namespace

bool ModelRenderer::inVram(const Model& m) const {
    return m.blob && m.h &&
           m.verts != reinterpret_cast<const ModelVertex*>(static_cast<const uint8_t*>(m.blob) + m.h->vertexOffset);
}

void ModelRenderer::streamBegin(const pp::Pak& pak) {
    uint32_t maxModel = 0, maxTex = 0;
    bool anyModel = false, anyTex = false;
    for (uint32_t i = 0; i < pak.count(); ++i) {
        const char* n = pak.entry(i)->name;
        if ((n[0] != 'm' && n[0] != 'x') || n[1] != '_' || n[2] < '0' || n[2] > '9') continue;
        const uint32_t id = static_cast<uint32_t>(std::strtoul(n + 2, nullptr, 10));
        if (id >= kNoTexture) continue;
        if (n[0] == 'm') {
            maxModel = std::max(maxModel, id);
            anyModel = true;
        } else {
            maxTex = std::max(maxTex, id);
            anyTex = true;
        }
    }
    if (anyModel && models_.size() < maxModel + 1u) models_.resize(maxModel + 1u);
    if (anyTex && textures_.size() < maxTex + 1u) textures_.resize(maxTex + 1u);
    // eDRAM from scratch, placed incrementally from now on.
    for (Model& m : models_)
        if (m.blob && m.h) m.verts = reinterpret_cast<const ModelVertex*>(static_cast<uint8_t*>(m.blob) + m.h->vertexOffset);
    pp::vramReset();
    streamVram_ = true;
    vramFull_ = false;
    gridDirty_ = true;
    flushGrid();
}

bool ModelRenderer::hasTile(int tileX, int tileY) const {
    const uint32_t key = streamTileKey(tileX, tileY);
    for (uint32_t k : tileKeys_)
        if (k == key) return true;
    return false;
}

bool ModelRenderer::modelKnown(uint16_t id) const { return id < models_.size() && models_[id].tried; }

bool ModelRenderer::textureKnown(uint16_t id) const { return id < textures_.size() && textures_[id].tried; }

void ModelRenderer::flushGrid() {
    if (!gridDirty_) return;
    buildGrid();       // clears gridDirty_
    vramFull_ = false;  // new models may fit
}

uint32_t ModelRenderer::unloadTile(const pp::Pak& pak, int tileX, int tileY) {
    const uint32_t key = streamTileKey(tileX, tileY);
    size_t idx = tileKeys_.size();
    for (size_t i = 0; i < tileKeys_.size(); ++i)
        if (tileKeys_[i] == key) idx = i;
    if (idx == tileKeys_.size()) return 0;
    uint32_t freed = 0;
    void* blob = tileBlobs_[idx];
    tileBlobs_.erase(tileBlobs_.begin() + static_cast<std::ptrdiff_t>(idx));
    tileKeys_.erase(tileKeys_.begin() + static_cast<std::ptrdiff_t>(idx));
    for (size_t i = 0; i < deferred_.size(); ++i)  // its deferred placements go with it
        if (deferred_[i].key == key) {
            deferred_.erase(deferred_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }

    // Instances again from the remaining tiles, in load order, by loadTile's
    // rules: the first tile holding a uniqueId places it; a doodad whose
    // model failed is skipped (its uniqueId still counts as seen). PSP:
    // upstream unloadTile drops the doodads the tile placed even when a
    // loaded neighbour holds them too (they vanish until it reloads).
    instances_.clear();
    seen_.clear();
    for (size_t t = 0; t < tileBlobs_.size(); ++t) {
        const void* b = tileBlobs_[t];
        const auto* h = static_cast<const DoodadTileHeader*>(b);
        const auto* p = reinterpret_cast<const DoodadPlacement*>(static_cast<const uint8_t*>(b) + sizeof(DoodadTileHeader));
        const Deferred* dfr = deferred_.empty() ? nullptr : deferredOf(tileKeys_[t]);
        for (uint32_t i = 0; i < h->count; ++i) {
            if (dfr && i < dfr->flag.size() && dfr->flag[i]) continue;  // deferred - not seen yet
            if (!seen_.insert(p[i].uniqueId).second) continue;
            if (p[i].model >= models_.size() || !models_[p[i].model].h) continue;
            instances_.push_back(&p[i]);
        }
    }
    {
        char name[32];
        std::snprintf(name, sizeof(name), "d_%d_%d", tileX, tileY);
        const pp::PakEntry* e = pak.find(name);
        const uint32_t bytes = e ? e->size : 0u;
        memoryBytes_ -= std::min(memoryBytes_, bytes);
        freed += bytes;
    }
    pp::pakFree(blob);

    // Models no instance uses any more: blob, collision mesh, eDRAM copy.
    std::vector<uint32_t> uses(models_.size(), 0);
    for (const DoodadPlacement* p : instances_) ++uses[p->model];
    for (size_t id = 0; id < models_.size(); ++id) {
        Model& m = models_[id];
        if (!m.h || uses[id]) continue;
        if (inVram(m)) pp::vramFree(m.verts, m.h->vertexCount * sizeof(ModelVertex));
        const uint32_t mb = entrySize(pak, "m_", static_cast<unsigned>(id));
        memoryBytes_ -= std::min(memoryBytes_, mb);
        freed += mb;
        if (m.colBlob) {
            const uint32_t kb = entrySize(pak, "k_", static_cast<unsigned>(id));
            collisionBytes_ -= std::min(collisionBytes_, kb);
            freed += kb;
            pp::pakFree(m.colBlob);
        }
        if (m.impBlob) {  // The impostor goes with its model
            const uint32_t ib = entrySize(pak, "i_", static_cast<unsigned>(id));
            memoryBytes_ -= std::min(memoryBytes_, ib);
            freed += ib;
            pp::pakFree(m.impBlob);
        }
        pp::pakFree(m.blob);
        m = Model{};  // loadable again
        if (stats_.models) --stats_.models;
    }
    // Textures no resident model's batches use.
    std::vector<uint8_t> used(textures_.size(), 0);
    for (const Model& m : models_) {
        if (!m.h) continue;
        const uint32_t batches = static_cast<uint32_t>(m.h->batchCount[0]) + m.h->batchCount[1] +
                                 ((m.h->flags & kModelLod2) ? m.h->lod2BatchCount : 0u);  // Far LOD
        for (uint32_t i = 0; i < batches; ++i)
            if (m.batches[i].texture < used.size()) used[m.batches[i].texture] = 1;
        if (m.imp && m.imp->texture < used.size()) used[m.imp->texture] = 1;  // Impostor atlas
    }
    for (size_t id = 0; id < textures_.size(); ++id) {
        Tex& t = textures_[id];
        if (!t.ok || used[id]) continue;
        const uint32_t tb = t.bytes;  // The held size (m2texlod)
        memoryBytes_ -= std::min(memoryBytes_, tb);
        freed += tb;
        pp::textureRelease(t.t);
        t = Tex{};
        if (stats_.textures) --stats_.textures;
    }
    forgetFailed();
    stats_.instances = static_cast<uint32_t>(instances_.size());
    stats_.memoryKb = memoryBytes_ / 1024u;
    visible_.clear();
    visFast_.clear();
    gridDirty_ = true;
    flushGrid();
    return freed;
}

// see psp_models.hpp.
void ModelRenderer::forgetFailed() {
    for (Model& m : models_)
        if (m.tried && !m.h && !m.bad) m.tried = false;
    for (Tex& t : textures_)
        if (t.tried && !t.ok) t.tried = false;
}

// See psp_models.hpp.
uint32_t ModelRenderer::takeMissingTextures(std::vector<uint16_t>& out, uint32_t max) {
    uint32_t n = 0;
    size_t kept = 0;
    for (size_t i = 0; i < texMissing_.size(); ++i) {
        const uint16_t id = texMissing_[i];
        if (id >= textures_.size()) continue;
        Tex& t = textures_[id];
        if (t.ok) {
            t.missing = false;
            continue;
        }
        if (n >= max) {
            texMissing_[kept++] = id;
            continue;
        }
        t.tried = false;    // loadable again (loadTexture reads it)
        t.missing = false;  // a batch that still finds it missing queues it again (kTexRetries times)
        ++t.retries;
        out.push_back(id);
        ++n;
    }
    texMissing_.resize(kept);
    return n;
}

uint32_t ModelRenderer::retryTextures(pp::Pak& pak, const std::vector<uint16_t>& ids) {
    uint32_t ok = 0;
    for (uint16_t id : ids)
        if (id < textures_.size() && !textures_[id].ok && loadTexture(pak, id)) ++ok;
    return ok;
}

// ---- tiles by budget (psp_models.hpp, psp_tile_budget.hpp) ---------------------
namespace {
// Whether the heap still gives `bytes` now (one block, written: gcc drops a malloc/free pair it can see through).
// The pool a request of that size lands in (psp_bigheap.hpp); bigheap=0 - the old malloc probe.
// The headroom AND the guard block (pp::heapGuardOk: a doodad is a low priority - it
// never takes the heap's last big hole; an emulator autopilot run filled it to 1.3 MB free and shredded it).
bool heapGives(uint32_t bytes) { return pp::heapGivesBytes(bytes + pp::heapGuardKb() * 1024u) && pp::heapGuardOk(); }
// PSP (in the emulator a busy village's own tile kept 96 placements in view deferred - free 4.9 MB, the
// probe above asked 1536 + 512 + 4096): doodads the player can see may use the guard block (LoadBudget::nearRule).
bool heapGivesNear(uint32_t bytes) { return pp::heapGivesBytes(bytes); }
}  // namespace

ModelRenderer::BudgetProbe ModelRenderer::budgetProbe = &heapGives;
ModelRenderer::BudgetProbe ModelRenderer::budgetNearProbe = &heapGivesNear;

ModelRenderer::Deferred* ModelRenderer::deferredOf(uint32_t key) {
    for (Deferred& d : deferred_)
        if (d.key == key) return &d;
    return nullptr;
}

const ModelRenderer::Deferred* ModelRenderer::deferredOf(uint32_t key) const {
    for (const Deferred& d : deferred_)
        if (d.key == key) return &d;
    return nullptr;
}

const void* ModelRenderer::tileBlob(uint32_t key) const {
    for (size_t i = 0; i < tileKeys_.size(); ++i)
        if (tileKeys_[i] == key) return tileBlobs_[i];
    return nullptr;
}

// m_ + k_ + i_ of a model; parked = its m_ blob was read ahead (the loader thread parked it).
uint32_t ModelRenderer::modelNeedBytes(const pp::Pak& pak, uint16_t id, bool& parked) const {
    char name[16];
    std::snprintf(name, sizeof(name), "m_%u", static_cast<unsigned>(id));
    const pp::PakEntry* m = pak.find(name);
    parked = true;
    if (!m) return 0;  // model() fails the same way: nothing to read
    parked = pp::pakPrefetchPeek(pak.fileId(), m->offset) != nullptr;
    if (parked) return 0;  // already in the heap (the install takes the blob over)
    uint32_t need = m->size;
    std::snprintf(name, sizeof(name), "k_%u", static_cast<unsigned>(id));
    if (const pp::PakEntry* k = pak.find(name)) need += k->size;
    std::snprintf(name, sizeof(name), "i_%u", static_cast<unsigned>(id));
    if (const pp::PakEntry* i = pak.find(name)) need += i->size;
    return need;
}

uint32_t ModelRenderer::placeBudgeted(pp::Pak& pak, const DoodadPlacement* p, const std::vector<uint32_t>& order,
                                      const LoadBudget& budget, std::vector<uint32_t>& left) {
    TileBudgetMeter meter;
    meter.reserveBytes = budget.reserveKb * 1024u;
    meter.stepBytes = std::max<uint32_t>(1u, budget.stepKb) * 1024u;
    uint32_t placed = 0;
    for (uint32_t i : order) {
        const DoodadPlacement& q = p[i];
        if (seen_.count(q.uniqueId)) continue;  // already placed (a neighbour tile): as loadTile
        const uint16_t id = q.model;
        const bool known = id < models_.size() && models_[id].tried;  // resident, failed or bad: no read
        if (!known) {
            bool parked = true;
            const uint32_t need = modelNeedBytes(pak, id, parked);
            // PSP (emulator emu_memdiet7: a top-up read 1.5 MB ahead every 5 s and placed none of it): a
            // model the loader parked is in the heap already - placing it costs nothing, no probe.
            const bool inHeap = parked && need == 0;
            if ((budget.parkedOnly && !parked) || (!inHeap && !meter.admit(need, budget.nearRule ? budgetNearProbe : budgetProbe))) {
                left.push_back(i);
                continue;
            }
        }
        seen_.insert(q.uniqueId);
        if (!model(pak, id)) continue;  // failed / bad: seen, not drawn (as loadTile)
        instances_.push_back(&q);
        ++placed;
    }
    budgetStats_.probes += meter.probes;
    if (meter.stopped) ++budgetStats_.stops;
    return placed;
}

bool ModelRenderer::loadTile(pp::Pak& pak, int tileX, int tileY, const LoadBudget& budget) {
    if (!budget.reserveKb && !budget.parkedOnly) return loadTile(pak, tileX, tileY);
    char name[32];
    std::snprintf(name, sizeof(name), "d_%d_%d", tileX, tileY);
    const pp::PakEntry* e = pak.find(name);
    auto* blob = e ? static_cast<uint8_t*>(pak.load(e)) : nullptr;
    if (!blob) return false;
    const auto* h = reinterpret_cast<const DoodadTileHeader*>(blob);
    if (e->size < sizeof(DoodadTileHeader) || std::memcmp(h->magic, "WDOD", 4) != 0 ||
        h->version != kModelFormatVersion || sizeof(DoodadTileHeader) + h->count * sizeof(DoodadPlacement) > e->size) {
        PSP_LOGE(pp::tag::Gfx, "wdod_bad name=%s size=%u", name, static_cast<unsigned>(e->size));
        pp::pakFree(blob);
        return false;
    }
    const uint32_t key = streamTileKey(tileX, tileY);
    tileBlobs_.push_back(blob);
    tileKeys_.push_back(key);
    memoryBytes_ += e->size;
    const auto* p = reinterpret_cast<const DoodadPlacement*>(blob + sizeof(DoodadTileHeader));
    std::vector<uint32_t> order, left;
    orderNearest(p, h->count, budget.x, budget.y, order);
    placeBudgeted(pak, p, order, budget, left);
    if (!left.empty()) {
        Deferred d;
        d.key = key;
        d.flag.assign(h->count, false);
        for (uint32_t i : left) d.flag[i] = true;
        d.idx.swap(left);
        budgetStats_.deferred += static_cast<uint32_t>(d.idx.size());
        deferred_.push_back(std::move(d));
    }
    gridDirty_ = true;
    stats_.instances = static_cast<uint32_t>(instances_.size());
    stats_.memoryKb = memoryBytes_ / 1024u;
    return true;
}

uint32_t ModelRenderer::resumeTile(pp::Pak& pak, int tileX, int tileY, const LoadBudget& budget) {
    const uint32_t key = streamTileKey(tileX, tileY);
    Deferred* d = deferredOf(key);
    const void* b = tileBlob(key);
    if (!d || !b) return 0;
    const auto* h = static_cast<const DoodadTileHeader*>(b);
    const auto* p = reinterpret_cast<const DoodadPlacement*>(static_cast<const uint8_t*>(b) + sizeof(DoodadTileHeader));
    // The deferred ones nearest the player now (it moved since).
    std::vector<std::pair<float, uint32_t>> byYd;
    byYd.reserve(d->idx.size());
    for (uint32_t i : d->idx) {
        if (i >= h->count) continue;
        const float dx = p[i].sphere[0] - budget.x, dy = p[i].sphere[1] - budget.y;
        byYd.emplace_back(std::sqrt(dx * dx + dy * dy) - p[i].sphere[3], i);
    }
    std::stable_sort(byYd.begin(), byYd.end(),
                     [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& c) { return a.first < c.first; });
    std::vector<uint32_t> order, left;
    order.reserve(byYd.size());
    for (const auto& [yd, i] : byYd) order.push_back(i);
    std::vector<std::pair<float, uint32_t>>().swap(byYd);
    // Not deferred any more while placing (placeBudgeted reads seen_, not the flags); what is left is again.
    for (uint32_t i : order) d->flag[i] = false;
    const uint32_t placed = placeBudgeted(pak, p, order, budget, left);
    for (uint32_t i : left) d->flag[i] = true;
    budgetStats_.resumed += static_cast<uint32_t>(order.size() - left.size());
    if (left.empty()) {
        for (size_t k = 0; k < deferred_.size(); ++k)
            if (&deferred_[k] == d) {
                deferred_.erase(deferred_.begin() + static_cast<std::ptrdiff_t>(k));
                break;
            }
    } else {
        d->idx.swap(left);
    }
    if (placed) {
        gridDirty_ = true;
        stats_.instances = static_cast<uint32_t>(instances_.size());
        stats_.memoryKb = memoryBytes_ / 1024u;
    }
    return placed;
}

uint32_t ModelRenderer::deferredCount(int tileX, int tileY) const {
    const Deferred* d = deferredOf(streamTileKey(tileX, tileY));
    return d ? static_cast<uint32_t>(d->idx.size()) : 0u;
}

// See psp_models.hpp.
uint32_t ModelRenderer::deferredInView(int tileX, int tileY, float x, float y) const {
    const uint32_t key = streamTileKey(tileX, tileY);
    const Deferred* d = deferredOf(key);
    const void* b = tileBlob(key);
    if (!d || !b) return 0;
    const auto* h = static_cast<const DoodadTileHeader*>(b);
    const auto* p = reinterpret_cast<const DoodadPlacement*>(static_cast<const uint8_t*>(b) + sizeof(DoodadTileHeader));
    uint32_t n = 0;
    for (uint32_t i : d->idx) {
        if (i >= h->count || seen_.count(p[i].uniqueId)) continue;
        const float r = p[i].sphere[3];
        const float dx = p[i].sphere[0] - x, dy = p[i].sphere[1] - y;
        const float reach = viewDistance * std::max(1.0f, r / 12.0f) * 1.6f + r;
        if (dx * dx + dy * dy < reach * reach) ++n;
    }
    return n;
}

uint32_t ModelRenderer::deferredTotal() const {
    uint32_t n = 0;
    for (const Deferred& d : deferred_) n += static_cast<uint32_t>(d.idx.size());
    return n;
}

void ModelRenderer::deferredModels(const pp::Pak& pak, int tileX, int tileY, float x, float y, uint32_t maxBytes,
                                   std::vector<uint16_t>& out) const {
    out.clear();
    const uint32_t key = streamTileKey(tileX, tileY);
    const Deferred* d = deferredOf(key);
    const void* b = tileBlob(key);
    if (!d || !b) return;
    const auto* h = static_cast<const DoodadTileHeader*>(b);
    const auto* p = reinterpret_cast<const DoodadPlacement*>(static_cast<const uint8_t*>(b) + sizeof(DoodadTileHeader));
    std::vector<std::pair<float, uint16_t>> byYd;
    byYd.reserve(d->idx.size());
    for (uint32_t i : d->idx) {
        if (i >= h->count || seen_.count(p[i].uniqueId)) continue;
        const uint16_t id = p[i].model;
        if (id < models_.size() && models_[id].tried) continue;  // resident (placed next resume) or failed
        const float dx = p[i].sphere[0] - x, dy = p[i].sphere[1] - y;
        byYd.emplace_back(std::sqrt(dx * dx + dy * dy) - p[i].sphere[3], id);
    }
    std::stable_sort(byYd.begin(), byYd.end(),
                     [](const std::pair<float, uint16_t>& a, const std::pair<float, uint16_t>& c) { return a.first < c.first; });
    uint32_t bytes = 0;
    for (const auto& [yd, id] : byYd) {
        if (std::find(out.begin(), out.end(), id) != out.end()) continue;
        if (!out.empty() && bytes >= maxBytes) break;
        bool parked = true;
        bytes += modelNeedBytes(pak, id, parked);
        out.push_back(id);
    }
}
// ---- (end of tiles by budget) --------------------------------------------------------

uint32_t ModelRenderer::streamVramStep(uint32_t maxBytes) {
    if (!streamVram_ || vramFull_) return 0;
    std::vector<Model*> byUse;
    for (Model& m : models_)
        if (m.h && m.instances && !inVram(m)) byUse.push_back(&m);
    if (byUse.empty()) return 0;
    std::sort(byUse.begin(), byUse.end(), [](const Model* a, const Model* b) {
        return a->instances * a->h->vertexCount > b->instances * b->h->vertexCount;
    });
    uint32_t copied = 0;
    for (Model* m : byUse) {
        const uint32_t bytes = m->h->vertexCount * sizeof(ModelVertex);
        if (copied && copied + bytes > maxBytes) break;
        const void* v = pp::vramStore(m->verts, bytes);
        if (!v) continue;  // a smaller one may still fit
        m->verts = static_cast<const ModelVertex*>(v);
        copied += bytes;
    }
    if (!copied) vramFull_ = true;
    return copied;
}

// Camera vs props (see the header): raycastBuildings' loop over the
// props - bounding sphere, then the model's grid cells along the ray.
float ModelRenderer::raycastProps(const glm::vec3& origin, const glm::vec3& dir, float maxDist,
                                  float minHeight) const {
    PROF_SCOPE("col.camera");
    ++g_cpuStats.colQueries;
    float best = maxDist;
    // same input, same world -> the last answer (key[8] = 1: props).
    const uint32_t key[9] = {bitsOf(origin.x), bitsOf(origin.y), bitsOf(origin.z), bitsOf(dir.x), bitsOf(dir.y),
                             bitsOf(dir.z),    bitsOf(maxDist),  bitsOf(minHeight), 1u};
    if (g_cpuFast) {
        for (const RayMemo& m : rayMemo_) {
            if (m.gen == gridGen_ && std::memcmp(m.key, key, sizeof(key)) == 0) {
                ++g_cpuStats.colMemoHits;
                return m.best;
            }
        }
    }
    auto visit = [&](const Inst& in) {  // the build-104 loop body
        ++g_cpuStats.colInsts;
        if (!in.m->col || (in.m->h->flags & (kModelBuilding | kModelGroundDetail))) return;
        const glm::vec3 oc = glm::vec3(in.x, in.y, in.z) - origin;
        const float tc = glm::dot(oc, dir);
        const float d2 = glm::dot(oc, oc) - tc * tc;
        if (d2 > in.r * in.r || tc + in.r < 0.0f || tc - in.r > best) return;
        const DoodadPlacement& p = *in.p;
        const glm::mat3 A = axesOf(p);
        const CollisionHeader& ch = *in.m->col;
        if ((ch.bmax[2] - ch.bmin[2]) * glm::length(A[2]) < minHeight) return;  // fences, crates, bushes
        const glm::mat3 Ai = glm::inverse(A);
        const glm::vec3 pos(p.position[0], p.position[1], p.position[2]);
        const glm::vec3 o = Ai * (origin - pos);
        const glm::vec3 d = Ai * dir;  // t stays in world yards
        const glm::vec3 e = o + d * best;
        const ColView c = view(in.m->col);
        forCells(c, glm::min(glm::vec2(o), glm::vec2(e)), glm::max(glm::vec2(o), glm::vec2(e)), [&](uint32_t ti) {
            const CollisionTri& t = c.t[ti];
            const float hit = rayTri(o, d, c.vert(t.a), c.vert(t.b), c.vert(t.c));
            if (hit > 0.0f && hit < best) best = hit;
        });
    };
    if (rayGridOk(origin, dir, maxDist)) {  // as raycastBuildings
        const glm::vec3 end = origin + dir * maxDist;
        forGrid(std::min(origin.x, end.x), std::min(origin.y, end.y), std::max(origin.x, end.x),
                std::max(origin.y, end.y),
                [&](const Cell& cl) {
                    const glm::vec3 oc = glm::vec3(cl.x, cl.y, cl.z) - origin;
                    const float tc = glm::dot(oc, dir);
                    const float d2 = glm::dot(oc, oc) - tc * tc;
                    const float rm = cl.r + kCellMargin;
                    return !(d2 > rm * rm || tc + rm < 0.0f || tc - rm > best);
                },
                visit);
    } else {
        for (const Inst& in : insts_) visit(in);
    }
    if (g_cpuFast) {
        RayMemo& m = rayMemo_[rayMemoNext_];
        rayMemoNext_ = static_cast<uint8_t>((rayMemoNext_ + 1u) % 6u);
        std::memcpy(m.key, key, sizeof(key));
        m.gen = gridGen_;
        m.best = best;
    }
    return best;
}

// the object under the PSP's "cursor" (ui_world_gouse.cpp) drawn once more over
// itself, additively - the original client's mouseover highlight of an interactive object (its model a little brighter):
// LOD 0's batches as the frame drew them (textures, alpha test), blended src * addABGR + dst, no depth writes (the
// same depth: the GEQUAL test passes). False: not drawn (not placed / not in yet / the display list's reserve).
bool ModelRenderer::drawHighlight(uint32_t uniqueId, const TerrainCamera& cam, uint32_t fogColorABGR, uint32_t addABGR) {
    if (gridDirty_) return false;  // the next frame (draw builds the grid)
    const Inst* found = nullptr;
    for (const Inst& in : insts_)
        if (in.p->uniqueId == uniqueId) {
            found = &in;
            break;
        }
    if (!found || !found->m || !found->m->h || pp::displayListFree() < pp::kListReserveM2) return false;
    const DoodadPlacement& p = *found->p;
    const ModelHeader& h = *found->m->h;
    const glm::vec3 ax(p.axes[0], p.axes[1], p.axes[2]);
    const glm::vec3 ay(p.axes[3], p.axes[4], p.axes[5]);
    const glm::vec3 az(p.axes[6], p.axes[7], p.axes[8]);
    const glm::vec3 t = glm::vec3(p.position[0], p.position[1], p.position[2]) + ax * h.center[0] + ay * h.center[1] +
                        az * h.center[2] - cam.pos;
    ScePspFMatrix4 mtx;
    mtx.x = {ax.x * h.half[0], ax.y * h.half[0], ax.z * h.half[0], 0.0f};
    mtx.y = {ay.x * h.half[1], ay.y * h.half[1], ay.z * h.half[1], 0.0f};
    mtx.z = {az.x * h.half[2], az.y * h.half[2], az.z * h.half[2], 0.0f};
    mtx.w = {t.x, t.y, t.z, 1.0f};
    pp::textureResetBinding();
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    lightBegin(cam, fogColorABGR);
    sceGuDepthMask(GU_TRUE);
    sceGuSetMatrix(GU_MODEL, &mtx);
    uint32_t first = 0, count = 0;
    lodRange(h, 0, first, count);
    for (uint32_t i = first; i < first + count; ++i) {
        const ModelBatch& b = found->m->batches[i];
        setBatchState(*found->m, b);  // the highlight: a missing texture draws the flat colour
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, addABGR, 0xFFFFFFFFu);
        drawBatch(*found->m, b);
    }
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    lightEnd();
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();
    return true;
}
// (end of the hover highlight)

} // namespace wowee::rendering::psp
