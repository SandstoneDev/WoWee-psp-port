#include "rendering/psp/psp_collision_math.hpp"
#include "rendering/psp/psp_collision_stream_policy.hpp"
#include "rendering/psp/psp_collision_neighborhood.hpp"
#include "rendering/psp/psp_wmo_draw_order.hpp"
// Big WMOs on the PSP - see include/rendering/psp/psp_bigwmo.hpp
// (the format, the streaming and the budgets).
#include "rendering/psp/psp_bigwmo.hpp"

#include "platform/psp/psp_bigheap.hpp"  // The big WMOs' arena
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/psp/psp_frustum.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"

#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

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

constexpr int kStripType = GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
constexpr uint32_t kKeyShift = 24;
constexpr float kPlayerRadius = 0.5f;
constexpr float kPlayerHeight = 2.0f;
constexpr float kWallStepHeight = 0.6f;  // same step limit as floor movement
constexpr uint32_t kReadRetryMs = 5000;  // a failed read is asked again this much later
constexpr uint32_t kFdRetryMs = 1000;    // no sceIoOpen more often than this while it fails
constexpr unsigned kErrNoEnt = 0x80010002u;  // SCE_ERROR_ERRNO_ENOENT: the file is not there

// a back-off time has passed (0 = none; wraps with the uptime).
bool due(uint32_t atMs, uint32_t nowMs) { return atMs == 0 || static_cast<int32_t>(nowMs - atMs) >= 0; }

bool isBlended(uint8_t blend) { return blend >= kBlendAlpha; }

const ChunkMeshHeader* meshHeader(const void* blob) { return static_cast<const ChunkMeshHeader*>(blob); }

// A WBCK blob is usable when its tables lie inside it (checked once at install).
bool meshValid(const void* blob, uint32_t size) {
    if (!blob || size < sizeof(ChunkMeshHeader)) return false;
    const ChunkMeshHeader* h = meshHeader(blob);
    if (std::memcmp(h->magic, "WBCK", 4) != 0 || h->version != kBigWmoVersion || (h->vertexOffset & 15u) != 0 ||
        h->vertexOffset + uint64_t{h->vertexCount} * sizeof(ModelVertex) > size ||
        h->batchOffset + uint64_t{h->batchCount} * sizeof(ModelBatch) > size || h->textureOffset + uint64_t{h->textureCount} * 2u > size)
        return false;
    const bool indexed = (h->flags & kChunkIndexed) != 0;
    if (indexed && ((h->indexOffset & 1u) || h->indexOffset + uint64_t{h->indexCount} * 2u > size)) return false;
    const auto* b = reinterpret_cast<const ModelBatch*>(static_cast<const uint8_t*>(blob) + h->batchOffset);
    const uint32_t limit = indexed ? h->indexCount : h->vertexCount;
    for (uint32_t i = 0; i < h->batchCount; ++i)
        if (uint64_t{b[i].indexStart} + b[i].indexCount > limit) return false;
    if (indexed) {
        const auto* ix = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(blob) + h->indexOffset);
        for (uint32_t i = 0; i < h->indexCount; ++i)
            if (ix[i] >= h->vertexCount) return false;
    }
    return true;
}

const uint16_t* meshIndices(const void* blob) {
    return reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(blob) + meshHeader(blob)->indexOffset);
}

const uint16_t* meshTextures(const void* blob) {
    return reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(blob) + meshHeader(blob)->textureOffset);
}
const ModelBatch* meshBatches(const void* blob) {
    return reinterpret_cast<const ModelBatch*>(static_cast<const uint8_t*>(blob) + meshHeader(blob)->batchOffset);
}
const ModelVertex* meshVertices(const void* blob) {
    return reinterpret_cast<const ModelVertex*>(static_cast<const uint8_t*>(blob) + meshHeader(blob)->vertexOffset);
}

// the cells, lists, triangle indices and the grid too (psp_bigwmo_vis.hpp collisionValid) - the
// queries below index them unchecked; nx == 0 read cells[-1].
bool colValid(const void* blob, uint32_t size) { return collisionValid(blob, size); }

struct ColView {
    const CollisionHeader* h;
    const float* v;
    const CollisionTri* t;
    const uint32_t* cells;
    const uint32_t* list;
    glm::vec3 vert(uint16_t i) const { return glm::vec3(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]); }
};

ColView colView(const void* blob) {
    const auto* h = static_cast<const CollisionHeader*>(blob);
    const auto* b = static_cast<const uint8_t*>(blob);
    return ColView{h, reinterpret_cast<const float*>(b + h->vertexOffset),
                   reinterpret_cast<const CollisionTri*>(b + h->triangleOffset),
                   reinterpret_cast<const uint32_t*>(b + h->cellOffset), reinterpret_cast<const uint32_t*>(b + h->listOffset)};
}

// As psp_models.cpp (ModelRenderer's collision): grid cells over [lo, hi] (model XY), Moller-Trumbore, Ericson.
template <typename Fn>
void forCells(const ColView& c, glm::vec2 lo, glm::vec2 hi, Fn&& fn) {
    const CollisionHeader& h = *c.h;
    const int x0 = std::max(0, static_cast<int>((lo.x - h.bmin[0]) / h.cell));
    const int x1 = std::min(h.nx - 1, static_cast<int>((hi.x - h.bmin[0]) / h.cell));
    const int y0 = std::max(0, static_cast<int>((lo.y - h.bmin[1]) / h.cell));
    const int y1 = std::min(h.ny - 1, static_cast<int>((hi.y - h.bmin[1]) / h.cell));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const uint32_t* cell = c.cells + (static_cast<uint32_t>(y) * h.nx + x) * 2u;
            for (uint32_t i = 0; i < cell[1]; ++i) fn(c.list[cell[0] + i]);
        }
}


glm::vec3 closestOnTri(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
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

ScePspFMatrix4 toGe(const glm::mat4& m) {
    ScePspFMatrix4 o;
    o.x = {m[0][0], m[0][1], m[0][2], m[0][3]};
    o.y = {m[1][0], m[1][1], m[1][2], m[1][3]};
    o.z = {m[2][0], m[2][1], m[2][2], m[2][3]};
    o.w = {m[3][0], m[3][1], m[3][2], m[3][3]};
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
bool BigWmo::open(const char* absPath, const float matrix[12], uint32_t uniqueId, uint16_t doodadSet, uint32_t stamp) {
    close();
    const char* slash = std::strrchr(absPath, '/');
    std::snprintf(name_, sizeof(name_), "%s", slash ? slash + 1 : absPath);
    std::snprintf(path_, sizeof(path_), "%s", absPath);
    openErr_ = kOpenOk;
    fd_ = sceIoOpen(absPath, PSP_O_RDONLY, 0);
    if (fd_ < 0) {
        pp::pakLogOpenFail(absPath, fd_);
        fdErr_ = fd_;
        // no such file - for good; any other error (0x80020320: no descriptor left) - later.
        openErr_ = static_cast<unsigned>(fd_) == kErrNoEnt ? kOpenMissing : kOpenRetry;
        fd_ = -1;
        return false;
    }
    pp::pakNoteOpen(fd_, absPath);
    fileId_ = pp::pakFileId(absPath);
    struct {
        char magic[4];
        uint32_t version, count, tocOffset;
    } ph{};
    pp::PakDiskEntry e0{};
    // a read that fails is worth another try (kOpenRetry); a pak that is not a big-WMO pak, or whose
    // "!wmo" record is bad, is not (kOpenBad).
    const bool readOk = pp::pakRead(fd_, 0, &ph, sizeof(ph));
    const bool pakOk = readOk && std::memcmp(ph.magic, "WPAK", 4) == 0 && ph.version == 1 && ph.count != 0;
    const bool tocOk = pakOk && pp::pakRead(fd_, ph.tocOffset, &e0, sizeof(e0));
    if (!tocOk || std::strncmp(e0.name, "!wmo", sizeof(e0.name)) != 0 || e0.size < sizeof(BigWmoHeader) ||
        e0.size > (4u << 20)) {
        PSP_LOGE(pp::tag::Gfx, "bigwmo_bad_pak name=%s", name_);
        close();
        openErr_ = (!readOk || (pakOk && !tocOk)) ? kOpenRetry : kOpenBad;
        return false;
    }
    // The big WMOs' arena first (an open / close no longer punches a ~730 KB hole in newlib).
    header_ = pp::wmoArenaAlloc((e0.size + 63u) & ~63u, 64);
    if (!header_) header_ = memalign(64, (e0.size + 63u) & ~63u);
    const bool headerRead = header_ && pp::pakRead(fd_, e0.offset, header_, e0.size);
    if (!headerRead || !tables_.bind(header_, e0.size)) {
        PSP_LOGE(pp::tag::Gfx, "bigwmo_bad_header name=%s size=%u", name_, static_cast<unsigned>(e0.size));
        close();
        openErr_ = headerRead ? kOpenBad : kOpenRetry;  // no memory / a read error: later
        return false;
    }
    headerSize_ = e0.size;
    uniqueId_ = uniqueId;
    doodadSet_ = doodadSet;
    doodadsOk_ = stamp != 0 && tables_.h->stamp == stamp;
    const BigWmoHeader& h = *tables_.h;
    uint32_t landmarkCount=0;
    const auto* landmarks=bigWmoLandmarks(header_,e0.size,landmarkCount);
    if (doodadsOk_ && landmarks) {
        landmarks_.assign(landmarks,landmarks+landmarkCount);
        for (auto& d:landmarks_) d.flags |= 1u;
        PSP_LOGI(pp::tag::Gfx,"bigwmo_landmarks name=%s n=%u",name_,static_cast<unsigned>(landmarkCount));
    }
    mesh_.assign(h.chunkCount, Res{});
    near_.assign(h.chunkCount, Res{});
    lod_.assign(h.chunkCount, Res{});
    col_.assign(h.chunkCount, Res{});
    shell_.assign(h.cellCount, Res{});
    cellDoodads_.assign(h.cellCount, Res{});
    tex_.assign(h.textureCount, Tex{});
    cellShellMode_.assign(h.cellCount, 0);
    cellMtx_.assign(h.cellCount, ScePspFMatrix4{});
    cellMtxFrame_.assign(h.cellCount, 0xFFFFFFFFu);
    groupChunks_.assign(h.groupCount, {});
    for (uint32_t c = 0; c < h.chunkCount; ++c) groupChunks_[tables_.chunks[c].group].push_back(c);
    std::vector<CollisionBoundsIndex::Entry> collisionBounds;
    for (uint32_t c = 0; c < h.chunkCount; ++c) {
        const auto& k = tables_.chunks[c];
        if (!k.collision.size || (tables_.groups[k.group].flags & kWmoGroupAntiportal)) continue;
        CollisionBoundsIndex::Entry e{};
        tables_.chunkBox(c, e.lo, e.hi);
        e.id = c;
        collisionBounds.push_back(e);
    }
    collisionBounds_.build(std::move(collisionBounds));
    headerSize_ += static_cast<uint32_t>(collisionBounds_.bytes()); // include index in the WMO memory budget
    PSP_LOGI(pp::tag::Gfx, "collision_bounds_index bytes=%u chunks=%u", static_cast<unsigned>(collisionBounds_.bytes()),
             static_cast<unsigned>(h.chunkCount));
    groupSeen_.assign(h.groupCount, 0);
    setPlacement(matrix);
    PSP_LOGI(pp::tag::Gfx,
             "bigwmo_open name=%s groups=%u cells=%u chunks=%u portals=%u textures=%u doodads=%u tris=%u header_kb=%u "
             "step=1/%.0f doodads_ok=%d set=%u",
             name_, h.groupCount, h.cellCount, static_cast<unsigned>(h.chunkCount), h.portalCount, h.textureCount,
             static_cast<unsigned>(h.doodadTotal), static_cast<unsigned>(h.totalTriangles),
             static_cast<unsigned>(headerSize_ / 1024u), 1.0f / h.step, doodadsOk_ ? 1 : 0, doodadSet);
    return true;
}

void BigWmo::close() {
    drawCache_.clear();
    groupCache_.invalidate();
    for (std::vector<Res>* v : {&mesh_, &near_, &lod_, &col_, &shell_, &cellDoodads_})
        for (Res& r : *v)
            if (r.blob) std::free(r.blob);
    for (Tex& t : tex_)
        if (t.ok) pp::textureRelease(t.t);
    mesh_.clear();
    near_.clear();
    lod_.clear();
    col_.clear();
    collisionBounds_.clear();
    shell_.clear();
    cellDoodads_.clear();
    landmarks_.clear();
    tex_.clear();
    groupChunks_.clear();
    colResident_.clear();
    collisionProbeActive_ = false;
    cellShellMode_.clear();
    groupSeen_.clear();
    planScratch_.clear();reqQueue_.clear();residentKeys_.clear();reqNext_=0;
    planDirty_=queueDirty_=collisionDirty_=true;buildingPlan_=false;
    requestRefreshMs_=lastPlanBudget_=0;lastCollisionOnly_=false;
    lastCam_=lastPlayerPlan_=lastCollisionPlayer_=glm::vec3(1e9f);
    prevCam_=vel_=glm::vec3(0.f);planFrame_=curFrame_=0;
    items_.clear();
    if (fd_ >= 0) {
        pp::pakPrefetchDrop(fileId_);
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
    }
    fd_ = -1;
    path_[0] = '\0';
    std::free(header_);
    header_ = nullptr;
    headerSize_ = 0;
    tables_ = BigWmoTables{};
    residentBytes_ = textureBytes_ = collisionBytes_ = 0;
    inFlight_ = 0;
    stats_ = BigWmoStats{};
}

bool BigWmo::ensureFd() {
    if (fd_ >= 0) return true;
    if (!path_[0]) return false;
    // after a failed open no new sceIoOpen for kFdRetryMs (not one a frame while the card has no
    // descriptor left), and the failure logged once per streak.
    const uint32_t now = pp::logUptimeMs();
    if (!due(fdRetryAtMs_, now)) return false;
    fd_ = sceIoOpen(path_, PSP_O_RDONLY, 0);
    if (fd_ < 0) {
        if (!fdRetryAtMs_) pp::pakLogOpenFail(path_, fd_);
        fdErr_ = fd_;
        fdRetryAtMs_ = (now + kFdRetryMs) | 1u;
        fd_ = -1;
        return false;
    }
    fdRetryAtMs_ = 0;
    pp::pakNoteOpen(fd_, path_);
    return true;
}

void BigWmo::releaseFd() {
    if (fd_ < 0 || inFlight_) return;
    pp::pakNoteClose(fd_);
    sceIoClose(fd_);
    fd_ = -1;
}

void BigWmo::setPlacement(const float m[12]) {
    bool changed=false;
    for(int c=0;c<4;++c)for(int r=0;r<3;++r)changed|=M_[c][r]!=m[c*3+r];
    if(changed) planDirty_=collisionDirty_=true;
    M_ = glm::mat4(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r) M_[c][r] = m[c * 3 + r];
    Minv_ = glm::inverse(M_);
    A_ = glm::mat3(M_);
    Ai_ = glm::inverse(A_);
}

void BigWmo::setCollisionProbe(const glm::vec3* world) {
    const bool active=world!=nullptr;
    // Match collisionReadyAt's arithmetic, including boundary rounding.
    const glm::vec3 local=active ? Ai_*(*world-glm::vec3(M_[3])) : glm::vec3(0.f);
    if(active!=collisionProbeActive_ || (active && local!=collisionProbeLocal_)) collisionDirty_=true;
    collisionProbeActive_=active;
    collisionProbeLocal_=local;
}

void BigWmo::worldBox(glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(1e30f);
    hi = glm::vec3(-1e30f);
    if (!tables_.h) return;
    const BigWmoHeader& h = *tables_.h;
    for (int k = 0; k < 8; ++k) {
        const glm::vec3 c((k & 1) ? h.bmax[0] : h.bmin[0], (k & 2) ? h.bmax[1] : h.bmin[1], (k & 4) ? h.bmax[2] : h.bmin[2]);
        const glm::vec3 w = glm::vec3(M_ * glm::vec4(c, 1.0f));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }
}

BigWmo::Res* BigWmo::resOf(uint32_t kind, uint32_t index) {
    std::vector<Res>* v = kind == kMesh ? &mesh_ : kind == kNear ? &near_ : kind == kLod ? &lod_ : kind == kCol ? &col_
                        : kind == kShell ? &shell_ : kind == kDoodads ? &cellDoodads_ : nullptr;
    return v && index < v->size() ? &(*v)[index] : nullptr;
}

bool BigWmo::meshReady(const Res& r) const {
    if (!r.blob) return false;
    const uint16_t* t = meshTextures(r.blob);
    for (uint32_t i = 0; i < meshHeader(r.blob)->textureCount; ++i)
        if (t[i] >= tex_.size() || !tex_[t[i]].ok) return false;
    return true;
}

void BigWmo::freeRes(Res& r, uint32_t kind) {
    if (kind==kMesh || kind==kNear || kind==kLod || kind==kShell) drawCache_.clear();
    if (!r.blob) return;
    if (kind == kCol) groupCache_.invalidate();
    residentBytes_ -= std::min(residentBytes_, r.size);
    if (kind == kCol) collisionBytes_ -= std::min(collisionBytes_, r.size);
    std::free(r.blob);
    r.blob = nullptr;
    if (kind == kDoodads) doodadsChanged_ = true;
    ++stats_.evictions;
}

// ---------------------------------------------------------------------------------------------------------------
BigWmoBlobRef BigWmo::planRef(uint32_t kind,uint32_t index) const {
    return kind==kTex ? tables_.textures[index] : kind==kMesh ? tables_.chunks[index].mesh
        : kind==kNear ? tables_.chunkNear[index] : kind==kLod ? tables_.chunkLod[index]
        : kind==kCol ? tables_.chunks[index].collision : kind==kShell ? tables_.cells[index].shell
        : tables_.cells[index].doodads;
}

void BigWmo::want(uint32_t kind,uint32_t index,float priority,uint32_t) {
    const BigWmoBlobRef ref=planRef(kind,index);
    if(!ref.offset || !ref.size) return;
    Res* r=kind==kTex ? nullptr : resOf(kind,index);
    bool& planned=kind==kTex ? tex_[index].planned : r->planned;
    float& old=kind==kTex ? tex_[index].prio : r->prio;
    const uint32_t key=(kind<<kKeyShift)|index;
    if(planned) {
        if(priority>=old) return;
        auto it=std::find_if(planScratch_.begin(),planScratch_.end(),[&](const PlanItem& p){return p.key==key;});
        if(buildingPlan_ && it!=planScratch_.end()) {it->prio=old=priority;return;}
        if(it!=planScratch_.end()) planScratch_.erase(it);
    }
    planned=true;old=priority;
    const PlanItem item{priority,ref.size,key};
    if(buildingPlan_) planScratch_.push_back(item);
    else planScratch_.insert(std::lower_bound(planScratch_.begin(),planScratch_.end(),priority,
        [](const PlanItem& p,float v){return p.prio<v;}),item);
    queueDirty_=true;
}

void BigWmo::planCollision() {
    // Only the indexed neighborhoods change on a physics step. Retain the
    // cached visual candidates and their ordering; insert nearby collision.
    planScratch_.erase(std::remove_if(planScratch_.begin(),planScratch_.end(),[&](const PlanItem& p) {
        if((p.key>>kKeyShift)!=kCol) return false;
        Res& r=col_[p.key&0xFFFFFFu];r.planned=r.wanted=false;return true;
    }),planScratch_.end());
    const float scale=std::max(.01f,glm::length(A_[0]));
    collisionNeighborhood(collisionBounds_,playerLocal_,scale,collisionPrefetchRadius(cfg.collisionYd),
        [&](uint32_t c,float d2){want(kCol,c,collisionStreamPriority(std::sqrt(d2)*scale),curFrame_);});
    if(collisionProbeActive_)
        collisionNeighborhood(collisionBounds_,collisionProbeLocal_,scale,kCollisionLandingRadius,
            [&](uint32_t c,float d2){want(kCol,c,-20000.f+std::sqrt(d2)*scale,curFrame_);});
    collisionDirty_=false;lastCollisionPlayer_=playerLocal_;queueDirty_=true;
    ++stats_.collisionPlans;
}

// Residency plan (main thread, update). Every blob gets "wanted" + a priority (yards: nearer first); the wanted
// set is cut at the budget in priority order; what is not wanted goes when the budget needs the room (least
// recently used first); the reads are the wanted, absent, not yet requested blobs, nearest first.
void BigWmo::update(const glm::vec3& camWorld, const glm::vec3& playerWorld, float dtSec, uint32_t frame,
                    std::vector<Request>& out, uint32_t maxRequests, bool collisionOnly) {
    out.clear();
    if (!tables_.h) return;
    PROF_SCOPE("bigwmo.plan");
    curFrame_ = frame;
    const uint32_t nowMs = pp::logUptimeMs();  // read back-off
    const BigWmoHeader& h = *tables_.h;
    const glm::vec3 cam = glm::vec3(Minv_ * glm::vec4(camWorld, 1.0f));
    playerLocal_ = glm::vec3(Minv_ * glm::vec4(playerWorld, 1.0f));
    if (dtSec > 1e-4f) {
        const glm::vec3 v = (cam - prevCam_) / dtSec;
        if (glm::dot(v, v) < 60.0f * 60.0f) vel_ += (v - vel_) * std::min(1.0f, dtSec * 4.0f);  // a teleport is no speed
    }
    prevCam_ = cam;
    const glm::vec3 d0 = cam - lastCam_;
    const bool moved = glm::dot(d0, d0) > 2.0f * 2.0f;
    const bool configChanged=cfg.lod.lodDistance!=plannedCfg_.lod.lodDistance ||
        cfg.lod.shellDistance!=plannedCfg_.lod.shellDistance || cfg.lod.viewDistance!=plannedCfg_.lod.viewDistance ||
        cfg.lod.hysteresis!=plannedCfg_.lod.hysteresis || cfg.collisionYd!=plannedCfg_.collisionYd ||
        cfg.doodadYd!=plannedCfg_.doodadYd || cfg.prefetchSec!=plannedCfg_.prefetchSec ||
        cfg.portals!=plannedCfg_.portals || cfg.visMode!=plannedCfg_.visMode;
    planDirty_|=configChanged;
    if(lastPlanBudget_!=cfg.budgetBytes || lastCollisionOnly_!=collisionOnly ||
       (requestRefreshMs_ && due(requestRefreshMs_,nowMs))) queueDirty_=true;
    lastPlanBudget_=cfg.budgetBytes;lastCollisionOnly_=collisionOnly;
    if (!moved && glm::length(playerLocal_ - lastPlayerPlan_) < 1.0f && !planDirty_ && frame - planFrame_ < 20) {
        // No new plan: only re-issue what the last plan wanted and is still absent.
    } else {
        planDirty_ = false;
        planFrame_ = frame;
        lastCam_ = cam;
        lastPlayerPlan_ = playerLocal_;
        plannedCfg_=cfg;
        ++stats_.spatialPlans;
        // Clear only the previous working set, rather than six chunk tables.
        for(const PlanItem& p:planScratch_) {
            const uint32_t kind=p.key>>kKeyShift,index=p.key&0xFFFFFFu;
            if(kind==kTex) tex_[index].planned=tex_[index].wanted=false;
            else if(Res* r=resOf(kind,index)) r->planned=r->wanted=false;
        }
        planScratch_.clear();buildingPlan_=true;queueDirty_=true;collisionDirty_=true;
        const glm::vec3 pred = cam + vel_ * cfg.prefetchSec;
        auto dist = [&](const glm::vec3& lo, const glm::vec3& hi) {
            return std::min(boxDistance(cam, lo, hi), boxDistance(pred, lo, hi));
        };
        const BigWmoLod& L = cfg.lod;
        const uint32_t seenWindow = 90;  // frames: a group seen this recently keeps its interior chunks wanted
        for (uint32_t ci = 0; ci < h.cellCount; ++ci) {
            const BigWmoCell& cl = tables_.cells[ci];
            glm::vec3 clo(cl.bmin[0], cl.bmin[1], cl.bmin[2]), chi(cl.bmax[0], cl.bmax[1], cl.bmax[2]);
            // a cell of doodads only, baked before the fix, has its box at the WMO's origin
            // (wmo_chunk_bake.hpp) - its "dd_" would never load: such a cell goes by the WMO's box.
            if (!cl.chunkCount && clo == chi) {
                clo = glm::vec3(h.bmin[0], h.bmin[1], h.bmin[2]);
                chi = glm::vec3(h.bmax[0], h.bmax[1], h.bmax[2]);
            }
            const float cd = dist(clo, chi);
            if (cd > L.viewDistance + 30.0f) continue;
            if (doodadsOk_ && cl.doodads.size && cd < cfg.doodadYd) {
                want(kDoodads,ci,cd+20.f,frame);
            }
            const bool outside = vis_.outside;
            if (cl.shell.size && cd > L.shellDistance - 40.0f) {
                want(kShell,ci,cd*.5f,frame); // small: before far chunks
            }
            const bool shellCell = cl.shell.size && cd > L.shellDistance + L.hysteresis;
            if (outside && shellCell) continue;  // the shell stands for it
            for (uint32_t c = cl.chunkStart; c < cl.chunkStart + cl.chunkCount; ++c) {
                const BigWmoChunk& k = tables_.chunks[c];
                const BigWmoGroup& g = tables_.groups[k.group];
                if (g.flags & kWmoGroupAntiportal) continue;
                if (shellCell && (k.flags & kChunkSeenOutside)) continue;  // drawn as the shell
                glm::vec3 lo, hi;
                tables_.chunkBox(c, lo, hi);
                const float d = dist(lo, hi);
                if (d > L.viewDistance) continue;
                // In no group (outside the WMO) only the outer surface draws (PortalVisibility::seenOutsideOnly).
                if (vis_.seenOutsideOnly && !(k.flags & kChunkSeenOutside) && d > 30.0f) continue;
                const bool seen = frame - groupSeen_[k.group] < seenWindow;
                const bool open = (g.flags & (kWmoGroupExterior | kWmoGroupAlwaysDraw)) || !(g.flags & kWmoGroupInterior);
                if (!seen && !(open && outside) && d > 30.0f) continue;  // an interior not seen lately: only nearby
                const float bias = seen ? 0.0f : 60.0f;
                if (d < L.lodDistance + 20.0f || !tables_.chunkLod[c].size) {
                    want(kMesh,c,d+bias,frame);
                } else {
                    want(kLod,c,d*.7f+bias,frame);
                }
                const float r = glm::length(hi - lo) * 0.5f;
                if (tables_.chunkNear[c].size && d < r + 12.0f) {
                    want(kNear,c,d+bias+1.f,frame);
                }
            }
        }
        // Textures of the wanted meshes that are resident (an absent mesh names its textures when it arrives).
        for(size_t pi=0;pi<planScratch_.size();++pi) {
            const PlanItem p=planScratch_[pi]; // want() may grow the vector
            const uint32_t kind=p.key>>kKeyShift,index=p.key&0xFFFFFFu;
            if(kind!=kMesh && kind!=kNear && kind!=kLod) continue;
            const Res& r=*resOf(kind,index);
            if(!r.blob) continue;
            const uint16_t* t=meshTextures(r.blob);
            for(uint32_t i=0;i<meshHeader(r.blob)->textureCount;++i)
                if(t[i]<tex_.size()) want(kTex,t[i],r.prio-.5f,frame);
        }
        std::sort(planScratch_.begin(),planScratch_.end(),[](const PlanItem& a,const PlanItem& b){return a.prio<b.prio;});
        buildingPlan_=false;
    }
    // Read completions and centimeter-scale collision changes do not revisit
    // every cell/chunk or re-sort the building's visual plan.
    if(collisionDirty_ || playerLocal_!=lastCollisionPlayer_) planCollision();
    if(queueDirty_) {
        queueDirty_=false;requestRefreshMs_=0;++stats_.queueRefreshes;
        stats_.planCandidates=static_cast<uint32_t>(planScratch_.size());
        for(PlanItem& p:planScratch_) {
            const uint32_t kind=p.key>>kKeyShift,index=p.key&0xFFFFFFu;
            if(kind==kTex) {
                Tex& t=tex_[index];t.wanted=!t.bad;
                p.bytes=t.ok?t.bytes:planRef(kind,index).size;
            } else if(Res* r=resOf(kind,index)) {
                r->wanted=!r->bad;p.bytes=r->blob?r->size:planRef(kind,index).size;
            }
        }
        // Cut at the budget, in priority order.
        std::vector<PlanItem>& ws = planScratch_;
        uint32_t total = headerSize_;
        uint32_t cut = 0;
        for (const PlanItem& w : ws) {
            // Rejected blobs do not consume space: smaller nearby textures/chunks
            // later in the queue must still be able to fit.
            const uint32_t kind = w.key >> kKeyShift, idx = w.key & 0xFFFFFFu;
            if(kind==kTex ? tex_[idx].bad : resOf(kind,idx)->bad) continue;
            // Never budget out the collision that collisionReadyAt waits for.
            // Sorted first, it displaces visual detail; allocation failures still
            // use the normal read back-off rather than allowing missing walls.
            const bool essential = kind == kCol && w.prio < 0.0f;
            if (collisionPlanFits(total,w.bytes,cfg.budgetBytes,essential)) { total+=w.bytes; continue; }
            ++cut;
            if (kind == kTex) tex_[idx].wanted = false;
            else if (Res* r = resOf(kind, idx)) r->wanted = false;
        }
        stats_.overBudget = cut;
        // Requests: wanted, absent, not in flight - nearest first (not a blob that failed its
        // check, nor one whose read failed less than kReadRetryMs ago).
        reqQueue_.clear();
        auto retryAt=[&](uint32_t at) {
            if(at && !due(at,nowMs) && (!requestRefreshMs_ || static_cast<int32_t>(at-requestRefreshMs_)<0))
                requestRefreshMs_=at;
        };
        for (const PlanItem& w : ws) {
            const uint32_t kind = w.key >> kKeyShift, idx = w.key & 0xFFFFFFu;
            if (kind == kTex) {
                const Tex& t = tex_[idx];
                if(t.wanted && !t.ok && !t.requested) retryAt(t.retryAtMs);
                if (t.wanted && !t.ok && !t.requested && !t.bad && due(t.retryAtMs, nowMs)) reqQueue_.push_back(w.key);
            } else if (const Res* r = resOf(kind, idx)) {
                if(r->wanted && !r->blob && !r->requested) retryAt(r->retryAtMs);
                if (r->wanted && !r->blob && !r->requested && !r->bad && due(r->retryAtMs, nowMs)) reqQueue_.push_back(w.key);
            }
        }
        reqNext_ = 0;
    }
    // Evict what is not wanted while over the budget (least recently used first), then hand out the reads.
    uint32_t resident = headerSize_ + residentBytes_ + textureBytes_ + static_cast<uint32_t>(drawCache_.bytes());
    if (collisionOnly || resident>cfg.budgetBytes) {
        resident-=static_cast<uint32_t>(drawCache_.bytes());
        drawCache_.clear(); // optional batching goes before world geometry under pressure
    }
    if (resident > cfg.budgetBytes) {
        struct E {
            uint32_t lastUse, key;
        };
        std::vector<E> ev;
        for(uint32_t key:residentKeys_) {
            const uint32_t kind=key>>kKeyShift,idx=key&0xFFFFFFu;
            if(kind==kTex) {
                const Tex& t=tex_[idx];
                if(t.ok && !t.wanted && t.lastUse+2<frame) ev.push_back(E{t.lastUse,key});
            } else if(const Res* r=resOf(kind,idx)) {
                if(r->blob && !r->wanted && r->lastUse+2<frame) ev.push_back(E{r->lastUse,key});
            }
        }
        std::sort(ev.begin(), ev.end(), [](const E& a, const E& b) { return a.lastUse < b.lastUse; });
        for (const E& e : ev) {
            if (resident <= cfg.budgetBytes) break;
            const uint32_t kind = e.key >> kKeyShift, idx = e.key & 0xFFFFFFu;
            if (kind == kTex) {
                Tex& t = tex_[idx];
                resident -= std::min(resident, t.bytes);
                textureBytes_ -= std::min(textureBytes_, t.bytes);
                pp::textureRelease(t.t);
                t.ok=false;t.bytes=0;t.requested=false; // retain cached candidate membership/priority
                ++stats_.evictions;
            } else if (Res* r = resOf(kind, idx)) {
                resident -= std::min(resident, r->size);
                freeRes(*r, kind);
                if (kind == kCol) colResident_.erase(std::remove(colResident_.begin(), colResident_.end(), idx), colResident_.end());
            }
        }
        residentKeys_.erase(std::remove_if(residentKeys_.begin(),residentKeys_.end(),[&](uint32_t key) {
            const uint32_t kind=key>>kKeyShift,idx=key&0xFFFFFFu;
            return kind==kTex ? !tex_[idx].ok : !resOf(kind,idx)->blob;
        }),residentKeys_.end());
    }
    while (reqNext_ < reqQueue_.size() && out.size() < maxRequests) {
        const uint32_t key = reqQueue_[reqNext_++];
        const uint32_t kind = key >> kKeyShift, idx = key & 0xFFFFFFu;
        const Res* collision = kind == kCol ? resOf(kind, idx) : nullptr;
        if (!collisionReadAllowed(collisionOnly, collision != nullptr, collision ? collision->prio : 0.0f)) {
            // Pressure transitions rebuild only this cached request queue.
            continue;
        }
        BigWmoBlobRef ref{0, 0};
        if (kind == kTex) {
            Tex& t = tex_[idx];
            if (!t.wanted || t.ok || t.requested || t.bad || !due(t.retryAtMs, nowMs)) continue;
            t.requested = true;
            ref = tables_.textures[idx];
        } else {
            Res* r = resOf(kind, idx);
            if (!r || !r->wanted || r->blob || r->requested || r->bad || !due(r->retryAtMs, nowMs)) continue;
            r->requested = true;
            ref = kind == kMesh ? tables_.chunks[idx].mesh : kind == kNear ? tables_.chunkNear[idx] : kind == kLod ? tables_.chunkLod[idx]
                : kind == kCol ? tables_.chunks[idx].collision : kind == kShell ? tables_.cells[idx].shell : tables_.cells[idx].doodads;
        }
        if (!ref.offset || !ref.size) continue;
        out.push_back(Request{ref.offset, ref.size, key});
        ++inFlight_;
    }
    stats_.residentBytes = residentBytes_ + static_cast<uint32_t>(drawCache_.bytes());
    stats_.textureBytes = textureBytes_;
    stats_.collisionBytes = collisionBytes_;
    stats_.headerBytes = headerSize_;
}

void BigWmo::install(uint32_t key, void* blob, uint32_t size) {
    if (inFlight_) --inFlight_;
    const uint32_t kind = key >> kKeyShift, idx = key & 0xFFFFFFu;
    queueDirty_ = true;
    if (!tables_.h) {
        std::free(blob);
        return;
    }
    ++stats_.reads;
    if (kind == kTex) {
        if (idx >= tex_.size()) {
            std::free(blob);
            return;
        }
        Tex& t = tex_[idx];
        t.requested = false;
        if (!blob) {  // the read failed - again in kReadRetryMs, not next frame
            t.retryAtMs = (pp::logUptimeMs() + kReadRetryMs) | 1u;
            return;
        }
        if (t.ok) {
            std::free(blob);
            return;
        }
        if (!pp::textureFromBlob(blob, size, t.t, true)) {
            // textureFromBlob fails only on a bad WTEX - never read again.
            std::free(blob);
            t.bad = true;
            ++stats_.badBlobs;
            PSP_LOGW(pp::tag::Gfx, "bigwmo_bad_blob name=%s kind=%u index=%u", name_, static_cast<unsigned>(kind),
                     static_cast<unsigned>(idx));
            return;
        }
        t.ok = true;
        t.bytes = size;
        if (cfg.texLodBytes)
            if (const uint32_t nb = pp::textureDropTopMip(t.t, cfg.texLodBytes)) t.bytes = nb;  // as m2texlod
        textureBytes_ += t.bytes;
        t.lastUse = curFrame_;
        residentKeys_.push_back(key);
        return;
    }
    Res* r = resOf(kind, idx);
    if (!r) {
        std::free(blob);
        return;
    }
    r->requested = false;
    if (!blob) {  // the read failed - again in kReadRetryMs, not next frame
        r->retryAtMs = (pp::logUptimeMs() + kReadRetryMs) | 1u;
        return;
    }
    if (r->blob) {
        std::free(blob);
        return;
    }
    const bool ok = (kind == kCol) ? colValid(blob, size)
                  : (kind == kDoodads) ? (size % sizeof(BigWmoDoodad)) == 0
                                       : meshValid(blob, size);
    if (!ok) {
        PSP_LOGW(pp::tag::Gfx, "bigwmo_bad_blob name=%s kind=%u index=%u", name_, static_cast<unsigned>(kind),
                 static_cast<unsigned>(idx));
        std::free(blob);
        r->bad = true;  // never read again (was: every frame, maxReads of them)
        ++stats_.badBlobs;
        return;
    }
    r->blob = blob;
    r->size = size;
    r->lastUse = curFrame_;
    residentBytes_ += size;
    residentKeys_.push_back(key);
    if(r->planned && (kind==kMesh || kind==kNear || kind==kLod)) {
        const uint16_t* textures=meshTextures(blob);
        for(uint32_t i=0;i<meshHeader(blob)->textureCount;++i)
            if(textures[i]<tex_.size()) want(kTex,textures[i],r->prio-.5f,curFrame_);
    }
    if (kind == kCol) {
        groupCache_.invalidate();
        collisionBytes_ += size;
        colResident_.push_back(idx);
    }
    if (kind == kDoodads) doodadsChanged_ = true;
}

// see the header.
void BigWmo::cancel(uint32_t key) {
    queueDirty_=true;
    if (inFlight_) --inFlight_;
    const uint32_t kind = key >> kKeyShift, idx = key & 0xFFFFFFu;
    if (kind == kTex) {
        if (idx < tex_.size()) tex_[idx].requested = false;
    } else if (Res* r = resOf(kind, idx)) {
        r->requested = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------
int BigWmo::findGroup(const glm::vec3& p) const {
    return groupCache_.resolve(p,cfg.visMode,[&]() {
    auto floorBelow = [&](uint32_t g, const glm::vec3& q, float& z) {
        bool found = false;
        float best = -1e30f;
        for (uint32_t c : groupChunks_[g]) {
            const Res& r = col_[c];
            if (!r.blob) continue;
            float cz;
            if (collisionFloorBelow(static_cast<const CollisionHeader*>(r.blob), r.size, q, cz) && cz > best) {
                best = cz;
                found = true;
            }
        }
        if (found) z = best;
        return found;
    };
    auto haveGeometry = [&](uint32_t g) {
        // Every chunk of the group under the point has its collision: then "no floor" is an answer.
        for (uint32_t c : groupChunks_[g]) {
            glm::vec3 lo, hi;
            tables_.chunkBox(c, lo, hi);
            if (p.x < lo.x || p.y < lo.y || p.x > hi.x || p.y > hi.y || p.z < lo.z) continue;
            if (tables_.chunks[c].collision.size && !col_[c].blob) return false;
        }
        return true;
    };
    if (cfg.visMode == PortalVisibility::kVisWalkOutdoor) {
        // wmovis 4: the highest surface of a group under the point - any of its triangles (findBigWmoGroupBySurface).
        auto surfaceBelow = [&](uint32_t g, const glm::vec3& q, float& z) {
            bool found = false;
            float best = -1e30f;
            for (uint32_t c : groupChunks_[g]) {
                const Res& r = col_[c];
                if (!r.blob) continue;
                float cz;
                if (collisionFloorBelow(static_cast<const CollisionHeader*>(r.blob), r.size, q, cz, 0.0f) && cz > best) {
                    best = cz;
                    found = true;
                }
            }
            if (found) z = best;
            return found;
        };
        auto haveAll = [&](uint32_t g) {  // the group's collision under the point is resident
            for (uint32_t c : groupChunks_[g]) {
                glm::vec3 lo, hi;
                tables_.chunkBox(c, lo, hi);
                if (p.x < lo.x || p.y < lo.y || p.x > hi.x || p.y > hi.y || p.z < lo.z) continue;
                if (tables_.chunks[c].collision.size && !col_[c].blob) return false;
            }
            return true;
        };
        return findBigWmoGroupBySurface(tables_, p, surfaceBelow, haveAll);
    }
    // No floor of a group under the point: outside (the route measured the smallest-box fallback: no better);
    // wmovis=3 takes upstream's box rule alone.
    return findBigWmoGroup(tables_, p, floorBelow, haveGeometry, false, cfg.visMode == PortalVisibility::kVisUpstream);
    });
}

void BigWmo::draw(const TerrainCamera& cam, uint32_t fogColorABGR) {
    stats_.chunks = stats_.nearChunks = stats_.lodChunks = stats_.shellCells = stats_.missing = 0;
    stats_.guardSkipped = 0;
    batchSaved_=0;
    stats_.interiorDraws = stats_.tintedDraws = 0;
    stats_.triangles = stats_.draws = stats_.binds = stats_.matrices = 0;
    if (!tables_.h) return;
    ++drawFrame_;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    PROF_SCOPE("bigwmo");
    PROF_GE_PASS("wmo");
    const BigWmoHeader& h = *tables_.h;
    const float aspect = static_cast<float>(pp::kScreenW) / pp::kScreenH;
    const glm::mat4 proj = glm::perspective(glm::radians(cam.fovyDeg), aspect, cam.nearZ, cam.farZ);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), cam.forward, cam.up);
    glm::mat4 Mrel = M_;  // camera-relative placement (the GE's 24-bit matrices)
    Mrel[3] = glm::vec4(glm::vec3(M_[3]) - cam.pos, 1.0f);
    const glm::mat4 viewM = view * Mrel;
    const glm::mat4 toClip = proj * viewM;
    const glm::vec3 camLocal = glm::vec3(Minv_ * glm::vec4(cam.pos, 1.0f));
    const float focal = wmoFocalPx(cam.fovyDeg);
    const float placeScale = std::max({glm::length(A_[0]), glm::length(A_[1]), glm::length(A_[2])});

    // Visibility.
    const int camGroup = findGroup(camLocal);
    const int playerGroup = findGroup(playerLocal_ + glm::vec3(0.0f, 0.0f, 0.5f));
    const bool wasOutside=vis_.outside,wasOutsideOnly=vis_.seenOutsideOnly;
    if (cfg.portals)
        vis_.compute(tables_, camLocal, toClip, camGroup, playerGroup, (h.flags & kBigWmoSideFlipped) ? -1 : 1, cfg.visMode);
    else
        vis_.all(tables_);
    if(wasOutside!=vis_.outside || wasOutsideOnly!=vis_.seenOutsideOnly) planDirty_=true;
    for (uint32_t g = 0; g < h.groupCount; ++g)
        if (vis_.isVisible(g)) {
            if(curFrame_-groupSeen_[g]>=90u) planDirty_=true;
            groupSeen_[g] = curFrame_;
        }
    stats_.camGroup = camGroup;
    stats_.outside = vis_.outside;
    stats_.visibleGroups = vis_.visibleGroups;
    stats_.walked = vis_.walked;

    // Chunks and shells to draw.
    const Frustum fr = makeFrustum(proj * view);  // camera-relative world (the spheres below are Mrel * local)
    const BigWmoLod& L = cfg.lod;
    items_.clear();
    auto pushMesh = [&](Res& r, uint16_t cell, float dist, bool interior, uint16_t group) {
        r.lastUse = curFrame_;
        const ModelBatch* b = meshBatches(r.blob);
        const ChunkMeshHeader* mh = meshHeader(r.blob);
        for (uint16_t i = 0; i < mh->batchCount; ++i)
            items_.push_back(DrawItem{r.blob, b[i].texture, cell, i, b[i].blend,
                                      static_cast<uint8_t>(interior ? 1 : 0), group,
                                      static_cast<uint16_t>(wmoDepthBucket(dist)), dist});
        stats_.triangles += mh->triangles;
        const uint16_t* t = meshTextures(r.blob);
        for (uint32_t i = 0; i < mh->textureCount; ++i)
            if (t[i] < tex_.size()) tex_[t[i]].lastUse = curFrame_;
    };
    for (uint32_t ci = 0; ci < h.cellCount; ++ci) {
        const BigWmoCell& cl = tables_.cells[ci];
        const glm::vec3 clo(cl.bmin[0], cl.bmin[1], cl.bmin[2]), chi(cl.bmax[0], cl.bmax[1], cl.bmax[2]);
        const float cd = boxDistance(camLocal, clo, chi) * placeScale;
        if (cd > L.viewDistance) continue;
        // The shell: outside, far (hysteresis keeps the choice near the line).
        uint8_t& mode = cellShellMode_[ci];
        const bool wantShell = cl.shell.size && (mode ? cd > L.shellDistance - L.hysteresis : cd > L.shellDistance + L.hysteresis);
        mode = wantShell ? 1 : 0;
        const glm::vec3 cellCenter = glm::vec3(Mrel * glm::vec4((clo + chi) * 0.5f, 1.0f));
        const float cellRadius = glm::length(chi - clo) * 0.5f * placeScale;
        // Include the outward rounding of quantized chunk bounds.
        if (fr.sphereOutside(cellCenter, cellRadius + 4.0f * h.step * placeScale)) continue;
        bool shellDrawn = false;
        if (wantShell && shell_[ci].blob) {
            Res& s = shell_[ci];
            shellDrawn = true;
            if (!fr.sphereOutside(cellCenter, cellRadius)) {
                pushMesh(s, static_cast<uint16_t>(ci), cd, false, 0xFFFFu);  // outer surface: zone tinted
                ++stats_.shellCells;
            }
            if (vis_.outside) continue;  // outside: the shell is the cell
        }
        // (A shell not resident yet: the chunks below, as far as they are.)
        for (uint32_t c = cl.chunkStart; c < cl.chunkStart + cl.chunkCount; ++c) {
            const BigWmoChunk& k = tables_.chunks[c];
            if (!vis_.chunkVisible(k)) continue;
            if (shellDrawn && (k.flags & kChunkSeenOutside)) continue;  // its outer surface is in the shell
            glm::vec3 lo, hi;
            tables_.chunkBox(c, lo, hi);
            const glm::vec3 cc = glm::vec3(Mrel * glm::vec4((lo + hi) * 0.5f, 1.0f));
            const float rad = glm::length(hi - lo) * 0.5f * placeScale;
            if (fr.sphereOutside(cc, rad)) continue;
            const float d = boxDistance(camLocal, lo, hi) * placeScale;
            if (d > L.viewDistance) continue;
            const NdcRect& gr = vis_.rects[k.group];
            if (!(gr.x0 <= -1.0f && gr.y0 <= -1.0f && gr.x1 >= 1.0f && gr.y1 >= 1.0f) &&
                projectBox(toClip, lo, hi).intersect(gr).empty())
                continue;
            // Guard band (the GE rule): the unsplit mesh only when its sphere is safe.
            const glm::vec4 cv = viewM * glm::vec4((lo + hi) * 0.5f, 1.0f);
            const bool safe = wmoSphereSafe(cv.x, cv.y, -cv.z, rad, cam.nearZ, focal);
            Res* use = nullptr;
            if (!safe) {
                // an unsafe sphere draws only a mesh split to the bake's edge - "n_", or "c_" of a
                // chunk without "n_" (none of its triangles was longer than that edge). Not "l_" (the original,
                // unsplit triangles) and not "c_" while its "n_" is still being read: the chunk is left out for those
                // frames (guardSkipped), as a chunk not read yet, rather than drawn with triangles the GE drops or
                // smears (vertices past the 4096 px window, more than 2730 px tall).
                if (meshReady(near_[c])) {
                    use = &near_[c];
                    ++stats_.nearChunks;
                } else if (tables_.chunkNear[c].size) {
                    ++stats_.guardSkipped;
                    continue;
                } else if (meshReady(mesh_[c])) {
                    use = &mesh_[c];
                }
            } else if (d > L.lodDistance && meshReady(lod_[c])) {
                use = &lod_[c];
                ++stats_.lodChunks;
            } else if (meshReady(mesh_[c])) {
                use = &mesh_[c];
            } else if (meshReady(lod_[c])) {
                use = &lod_[c];
                ++stats_.lodChunks;
            }
            if (!use) {
                ++stats_.missing;
                continue;
            }
            ++stats_.chunks;
            pushMesh(*use, static_cast<uint16_t>(ci), d,
                     (tables_.groups[k.group].flags & kWmoGroupInterior) != 0, k.group);
        }
    }
    // Near opaque surfaces establish depth before distant rooms. Keep texture
    // batching inside coarse bands rather than binding materials per triangle.
    std::sort(items_.begin(), items_.end(), [&](const DrawItem& a, const DrawItem& b) {
        return wmoDrawBefore(a,b,cfg.frontToBack);
    });

    pp::textureResetBinding();
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);  // upstream WMO: two-sided
    sceGuShadeModel(GU_SMOOTH);
    lightBegin(cam, fogColorABGR);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    int curCell = -1;
    const void* curBlob = nullptr;
    bool blendOn = false, listFull = false;
    uint16_t lastTex = 0xFFFE;
    uint32_t curState = 0xFFFFFFFFu;
    uint8_t curWrap = 0xFFu;  // the GE wrap sent last (kBatchWrapU | kBatchWrapV), 0xFF unknown
    for (size_t itemIndex=0;itemIndex<items_.size();++itemIndex) {
        const DrawItem& it=items_[itemIndex];
        if (pp::displayListFree() < pp::kListReserveM2) {
            listFull = true;
            break;
        }
        const ChunkMeshHeader* mh = meshHeader(it.blob);
        if (static_cast<int>(it.cell) != curCell) {
            curCell = it.cell;
            if (cellMtxFrame_[it.cell] != drawFrame_) {
                const BigWmoCell& cl = tables_.cells[it.cell];
                const float s = h.step * 32768.0f;
                glm::mat4 cm = Mrel;
                cm = glm::translate(cm, glm::vec3(cl.center[0], cl.center[1], cl.center[2]));
                cm = glm::scale(cm, glm::vec3(s));
                cellMtx_[it.cell] = toGe(cm);
                cellMtxFrame_[it.cell] = drawFrame_;
            }
            sceGuSetMatrix(GU_MODEL, &cellMtx_[it.cell]);
            ++stats_.matrices;
        }
        if (it.blob != curBlob) {
            curBlob = it.blob;
            sceGuTexScale(mh->uvScale[0], mh->uvScale[1]);
            sceGuTexOffset(mh->uvMin[0], mh->uvMin[1]);
        }
        const ModelBatch& b = meshBatches(it.blob)[it.batch];
        const bool blended = isBlended(b.blend);
        if (blended && !blendOn) {
            sceGuDisable(GU_ALPHA_TEST);
            sceGuEnable(GU_BLEND);
            sceGuDepthMask(GU_TRUE);
            blendOn = true;
            curState = 0xFFFFFFFFu;
        }
        const pp::Texture* tex = (b.texture != kNoTexture && b.texture < tex_.size() && tex_[b.texture].ok) ? &tex_[b.texture].t : nullptr;
        if (b.texture != lastTex) {
            lastTex = b.texture;
            ++stats_.binds;
            curWrap = 0xFFu;  // PSP: textureBind below resets the GE wrap to CLAMP on a new texture
        }
        pp::textureBind(tex);
        // PSP: the wrap is sent after every new bind, not with the cached state below - the opaque items are sorted
        // by texture, so a run of textures with one (flags, blend) state kept the CLAMP that textureBind sets, and
        // tiled WMO materials (a capital city's streets UV -13..+13, walls) sampled their edge texels: one-colour
        // streets, "stretched" walls (87 % of the wrapped triangles around its trade quarter).
        const uint8_t wrap = static_cast<uint8_t>(b.flags & (kBatchWrapU | kBatchWrapV));
        if (wrap != curWrap) {
            curWrap = wrap;
            sceGuTexWrap((wrap & kBatchWrapU) ? GU_REPEAT : GU_CLAMP, (wrap & kBatchWrapV) ? GU_REPEAT : GU_CLAMP);
        }
        // The batch state as ModelRenderer::setBatchState, sent only when it changes (texture-sorted runs).
        // Most underground interiors keep their baked light. Undercity's courtyard01 (root WMO group 15) was too
        // bright on PSP because the interior path forced white ambient. Let that one section receive zone ambient,
        // matching the darker courtyard look while keeping the surrounding indoor sections unchanged.
        const bool undercityCourtyard = it.interior && it.group == 15 &&
                                        std::strcmp(name_, "azeroth_undercity.pak") == 0;
        const bool unlit = (b.flags & kBatchUnlit) != 0 ||
            (it.group != 0xFFFFu && !bigWmoGroupUsesZoneLight(tables_.groups[it.group].flags) && !undercityCourtyard);
        const uint32_t state = (static_cast<uint32_t>(unlit) << 16) | (static_cast<uint32_t>(b.flags) << 8) | b.blend;
        if (it.interior) ++stats_.interiorDraws;
        else ++stats_.tintedDraws;
        if (state != curState) {
            curState = state;
            lightBatch(unlit);
            switch (b.blend) {
                case kBlendOpaque: sceGuDisable(GU_ALPHA_TEST); break;
                case kBlendAlphaKey:
                    sceGuEnable(GU_ALPHA_TEST);
                    sceGuAlphaFunc(GU_GREATER, 0x66, 0xFF);  // as ModelRenderer (upstream 0.4)
                    break;
                case kBlendAlpha: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0); break;
                case kBlendAdd: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFFu); break;
                default: sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0); break;
            }
        }
        // Join only visible, same-cell/group, opaque material runs. Each source
        // keeps its own decoded UVs; alpha blending and portal selection are untouched.
        WmoBatchCache::Key mergeKeys[32];size_t mergeCount=0,mergeVertices=0;
        if(b.blend<kBlendAlpha) {
            for(size_t j=itemIndex;j<items_.size() && mergeCount<32;++j) {
                const auto& other=items_[j];
                const auto& ob=meshBatches(other.blob)[other.batch];
                if(other.cell!=it.cell || other.group!=it.group || other.interior!=it.interior ||
                   other.depthBand!=it.depthBand || ob.texture!=b.texture || ob.blend!=b.blend || ob.flags!=b.flags || ob.indexCount<3)break;
                const size_t next=mergeVertices+(mergeCount?2+(mergeVertices&1u):0)+ob.indexCount;
                if(next>WmoBatchCache::kMaxVertices)break;
                mergeVertices=next;mergeKeys[mergeCount++]={other.blob,other.batch};
            }
        }
        const WmoBatchCache::Slot* merged=mergeCount>=2?drawCache_.find(mergeKeys,mergeCount,drawFrame_):nullptr;
        if(!merged && mergeCount>=2 && batchBuildFrame_!=drawFrame_) {
            batchBuildFrame_=drawFrame_; // at most 832 vertex conversions per frame
            const uint32_t held=headerSize_+residentBytes_+textureBytes_+static_cast<uint32_t>(drawCache_.bytes());
            if(held+32u*1024u<cfg.budgetBytes && pp::heapGivesKb(256)) {
                merged=drawCache_.build(mergeKeys,mergeCount,drawFrame_);
                if(merged) {
                    sceKernelDcacheWritebackRange(merged->vertices.data(),static_cast<unsigned>(merged->vertices.size()*sizeof(WmoBatchVertex)));
                    sceKernelDcacheWritebackRange(merged->indices.data(),static_cast<unsigned>(merged->indices.size()*sizeof(uint16_t)));
                }
            }
        }
        if(merged) {
            sceGuTexScale(1.f,1.f);sceGuTexOffset(0.f,0.f);
            sceGuDrawArray(GU_TRIANGLE_STRIP,GU_TEXTURE_32BITF|GU_COLOR_5650|GU_VERTEX_16BIT|GU_INDEX_16BIT|GU_TRANSFORM_3D,
                static_cast<int>(merged->indices.size()),merged->indices.data(),merged->vertices.data());
            itemIndex+=mergeCount-1;batchSaved_+=static_cast<uint32_t>(mergeCount-1);
            if(it.interior)stats_.interiorDraws+=static_cast<uint32_t>(mergeCount-1);
            else stats_.tintedDraws+=static_cast<uint32_t>(mergeCount-1);
            curBlob=nullptr; // restore source UV transform for the next ordinary draw
        } else if (mh->flags & kChunkIndexed)
            sceGuDrawArray(GU_TRIANGLE_STRIP, kStripType | GU_INDEX_16BIT, static_cast<int>(b.indexCount),
                           meshIndices(it.blob) + b.indexStart, meshVertices(it.blob));
        else
            sceGuDrawArray(GU_TRIANGLE_STRIP, kStripType, static_cast<int>(b.indexCount), nullptr,
                           meshVertices(it.blob) + b.indexStart);
        ++stats_.draws;
        prof::count(prof::kDraws, 1);
        const uint32_t emitted=merged?static_cast<uint32_t>(merged->indices.size()):b.indexCount;
        prof::count(prof::kVertices, emitted);
        prof::count(prof::kTriangles, emitted > 2 ? emitted - 2 : 0);
    }
    if (blendOn) {
        sceGuDepthMask(GU_FALSE);
        sceGuDisable(GU_BLEND);
    }
    sceGuDisable(GU_ALPHA_TEST);
    if (listFull) pp::displayListFull("wmo");
    lightEnd();
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pp::textureResetBinding();
    stats_.cpuUs = static_cast<uint32_t>(sceKernelGetSystemTimeWide() - t0);
}

// ---------------------------------------------------------------------------------------------------------------
// Collision (world space in, WMO-local inside), ModelRenderer's building rules.
bool BigWmo::floorHeight(float x, float y, float probeZ, float& outZ) const {
    if (!tables_.h || colResident_.empty()) return false;
    PROF_SCOPE("col.bigwmo");
    const glm::vec3 pos(M_[3]);
    const glm::vec3 o = Ai_ * (glm::vec3(x, y, probeZ) - pos);
    const glm::vec3 d = Ai_ * glm::vec3(0.0f, 0.0f, -1.0f);
    const float supportRadius=.12f/std::max(.01f,glm::length(A_[0]));
    bool found = false;
    float best = -1e30f;
    for (uint32_t c : colResident_) {
        const Res& r = col_[c];
        if (!r.blob) continue;
        const ColView cv = colView(r.blob);
        const CollisionHeader& ch = *cv.h;
        const glm::vec3 e = o + d * 400.0f;  // the whole drop below probeZ (the highest walkable floor wins)
        const glm::vec2 lo = glm::min(glm::vec2(o), glm::vec2(e))-supportRadius, hi = glm::max(glm::vec2(o), glm::vec2(e))+supportRadius;
        if (hi.x < ch.bmin[0] || hi.y < ch.bmin[1] || lo.x > ch.bmax[0] || lo.y > ch.bmax[1]) continue;
        forCells(cv, lo, hi, [&](uint32_t ti) {
            const CollisionTri& t = cv.t[ti];
            const glm::vec3 a = cv.vert(t.a), b = cv.vert(t.b), cc = cv.vert(t.c);
            const glm::vec3 n = glm::cross(A_ * (b - a), A_ * (cc - a));
            if (!collisionWalkable(n)) return;
            const float hit = collisionFloorRayTri(o, d, a, b, cc,supportRadius);
            if (hit < 0.0f) return;
            const float z = probeZ - hit;
            if (z <= best) return;
            best = z;
            found = true;
        });
    }
    if (found) outZ = best;
    return found;
}

bool BigWmo::collisionReadyAt(float x, float y, float z, float radiusYd, uint32_t& wanted, uint32_t& resident) const {
    wanted = resident = 0;
    if (!tables_.h) return false;
    const glm::vec3 o = Ai_ * (glm::vec3(x, y, z) - glm::vec3(M_[3]));
    const float scale = std::max(0.01f, glm::length(A_[0]));
    collisionNeighborhood(collisionBounds_,o,scale,radiusYd,[&](uint32_t c,float) {
        ++wanted;
        if (col_[c].blob || col_[c].bad) ++resident;  // a bad blob will never come: not worth waiting for
    });
    return resident == wanted;
}

void BigWmo::collideWalls(glm::vec3& to, float feetZ) const {
    if (!tables_.h || colResident_.empty()) return;
    PROF_SCOPE("col.bigwmo");
    const glm::vec3 pos(M_[3]);
    const float scale = std::max(0.01f, glm::length(A_[0]));
    for (int iter = 0; iter < 2; ++iter) {
        const glm::vec3 local = Ai_ * (to - pos);
        const float margin = (kPlayerRadius + kPlayerHeight) / scale;
        for (uint32_t c : colResident_) {
            const Res& r = col_[c];
            if (!r.blob) continue;
            const ColView cv = colView(r.blob);
            const CollisionHeader& ch = *cv.h;
            if (local.x + margin < ch.bmin[0] || local.y + margin < ch.bmin[1] || local.x - margin > ch.bmax[0] ||
                local.y - margin > ch.bmax[1])
                continue;
            forCells(cv, glm::vec2(local) - margin, glm::vec2(local) + margin, [&](uint32_t ti) {
                const CollisionTri& t = cv.t[ti];
                if (!(t.flags & kColBlocksWalls)) return;
                const glm::vec3 a = A_ * cv.vert(t.a) + pos, b = A_ * cv.vert(t.b) + pos, cc = A_ * cv.vert(t.c) + pos;
                const float minZ = std::min({a.z, b.z, cc.z}), maxZ = std::max({a.z, b.z, cc.z});
                if (maxZ <= feetZ + kWallStepHeight || minZ >= feetZ + kPlayerHeight) return;
                const glm::vec3 n = glm::cross(b - a, cc - a);
                if (glm::dot(n, n) < 1e-18f || collisionWalkable(n)) return;
                for (float hh : {kWallStepHeight + 0.1f, 1.7f}) {
                    const glm::vec3 q(to.x, to.y, feetZ + hh);
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
        }
    }
}

float BigWmo::raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const {
    float best = maxDist;
    if (!tables_.h || colResident_.empty()) return best;
    PROF_SCOPE("col.bigwmo");
    const glm::vec3 pos(M_[3]);
    const glm::vec3 o = Ai_ * (origin - pos);
    const glm::vec3 d = Ai_ * dir;  // t stays in world yards
    collisionBounds_.rayQuery(o,d,best,[&](const CollisionBoundsIndex::Entry& entry) {
        const uint32_t c=entry.id;
        const Res& r = col_[c];
        if (!r.blob) return;
        const ColView cv = colView(r.blob);
        const CollisionHeader& ch = *cv.h;
        const glm::vec3 e = o + d * best;
        const glm::vec2 lo = glm::min(glm::vec2(o), glm::vec2(e)), hi = glm::max(glm::vec2(o), glm::vec2(e));
        if (hi.x < ch.bmin[0] || hi.y < ch.bmin[1] || lo.x > ch.bmax[0] || lo.y > ch.bmax[1]) return;
        const float zlo = std::min(o.z, e.z), zhi = std::max(o.z, e.z);
        if (zhi < ch.bmin[2] || zlo > ch.bmax[2]) return;
        forCells(cv, lo, hi, [&](uint32_t ti) {
            const CollisionTri& t = cv.t[ti];
            const float hit = collisionRayTri(o, d, cv.vert(t.a), cv.vert(t.b), cv.vert(t.c));
            if (hit > 0.0f && hit < best) best = hit;
        });
    });
    return best;
}

}  // namespace wowee::rendering::psp
