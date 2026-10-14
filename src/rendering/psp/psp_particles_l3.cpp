// Level-3 particle engine - clean-room implementation of cleanroom/specs/particles/particle-runtime.md (RT-*).
// EBOOT-safe: no exceptions, no RTTI; the only allocation is the block made by acquire().
// Integration (docs/PSP_PORT_ARCHITECTURE.md): the port's engine - the older rules plug in through
// FxLegacyRules (psp_particles.cpp); additions are marked "Integration:".
#include "rendering/psp/psp_particles_l3.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace wowee {
namespace rendering {
namespace psp {

using ptcsim::M34;
using ptcsim::Particle;

namespace {

constexpr uint16_t kNoSlot = 0xFFFF;
constexpr uint8_t kFreeOwner = 0xFF;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float dot3(const glm::vec3& a, const glm::vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len3(const glm::vec3& v) { return std::sqrt(dot3(v, v)); }
inline glm::vec3 cross3(const glm::vec3& a, const glm::vec3& b) {
    return glm::vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline glm::vec3 norm3(const glm::vec3& v) {
    float l = len3(v);
    return l > 0.0f ? v / l : v;
}
inline bool finite3(const glm::vec3& v) { return fxFinite(v.x) && fxFinite(v.y) && fxFinite(v.z); }

// RT-PACK
inline uint32_t packChannel(float c) {
    float q = clampf(c * 255.0f + 0.5f, 0.0f, 255.0f);
    return static_cast<uint32_t>(q);
}
inline uint32_t packColor(float r, float g, float b, float a) {
    return packChannel(r) | (packChannel(g) << 8) | (packChannel(b) << 16) | (packChannel(a) << 24);
}

// RT-CURVE
struct CurvePos {
    int k;
    float f;
};
CurvePos curvePos(const SpellFxHeader& h, float timeMs) {
    CurvePos cp = {0, 0.0f};
    if (h.frames <= 1 || h.periodMs == 0) return cp;
    const float period = static_cast<float>(h.periodMs);
    const float last = static_cast<float>(h.frames - 1);
    float pos = (std::fmod(timeMs, period) / period) * last;
    if (!fxFinite(pos) || !(pos > 0.0f)) return cp;
    if (pos >= last) {
        cp.k = h.frames - 2;
        cp.f = 1.0f;
        return cp;
    }
    cp.k = static_cast<int>(pos);
    cp.f = pos - static_cast<float>(cp.k);
    return cp;
}
inline float readF(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, 4);
    return f;
}
float curveValue(const uint8_t* base, uint32_t off, uint16_t frames, const CurvePos& cp, bool held) {
    const uint8_t* p = base + off;
    if (frames <= 1) return readF(p);
    float a = readF(p + 4 * cp.k), b = readF(p + 4 * (cp.k + 1));
    if (held) return cp.f >= 1.0f ? b : a;
    return a + (b - a) * cp.f;
}

ptcsim::EmitterValues valuesAt(const FxView& fx, int e, const CurvePos& cp) {
    const SpellFxEmitter& em = fx.emitters[e];
    const SpellFxL3Emitter& c = fx.level3[e];
    float b[6], x[5];
    for (int k = 0; k < 6; ++k) b[k] = curveValue(fx.base, em.curveOffset[k], em.curveFrames[k], cp, (c.stepMask >> k) & 1u);
    for (int k = 0; k < 5; ++k)
        x[k] = curveValue(fx.base, c.curveOffset[k], c.curveFrames[k], cp, (c.stepMask >> (8 + k)) & 1u);
    ptcsim::EmitterValues v;
    v.rate = b[kFxCurveRate];
    v.life = b[kFxCurveLife];
    v.speed = b[kFxCurveSpeed];
    v.vRange = b[kFxCurveVRange];
    v.hRange = b[kFxCurveHRange];
    v.gravity = b[kFxCurveGravity];
    v.speedVar = x[kFxCurveSpeedVar];
    v.areaL = x[kFxCurveAreaL];
    v.areaW = x[kFxCurveAreaW];
    v.zSource = x[kFxCurveZSource];
    v.enabled = x[kFxCurveEnabled];
    return v;
}

// RT-BONE
void boneMatrix(const FxView& fx, uint16_t track, const CurvePos& cp, M34& out) {
    ptcsim::identity(out);
    if (track == kFxNone || !fx.tracks || track >= fx.h->trackCount) return;
    const SpellFxTrack& t = fx.tracks[track];
    int k0 = 0, k1 = 0;
    float f = 0.0f;
    if (t.frames > 1) {
        k0 = cp.k;
        k1 = cp.k + 1;
        f = cp.f;
    }
    const uint32_t ks = spellFxKeySize(t);
    const uint8_t* p = fx.base + t.offset;
    if (t.flags & kFxTrackMatrix) {
        SpellFxMatrixKey a, b;
        std::memcpy(&a, p + ks * k0, sizeof a);
        std::memcpy(&b, p + ks * k1, sizeof b);
        for (int i = 0; i < 9; ++i) {
            float va = a.m[i], vb = b.m[i];
            out.l[i] = (va + (vb - va) * f) * t.scaleScale;
        }
        for (int i = 0; i < 3; ++i) {
            float va = a.t[i], vb = b.t[i];
            out.t[i] = (va + (vb - va) * f) * t.translationScale;
        }
        return;
    }
    SpellFxKey a, b;
    std::memcpy(&a, p + ks * k0, sizeof a);
    std::memcpy(&b, p + ks * k1, sizeof b);
    float q[4];
    for (int i = 0; i < 4; ++i) {
        float va = a.q[i], vb = b.q[i];
        q[i] = (va + (vb - va) * f) * (1.0f / 32767.0f);
    }
    float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (n2 <= 1e-6f) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    } else {
        float inv = 1.0f / std::sqrt(n2);
        for (float& c : q) c *= inv;
    }
    float tr[3], sc[3];
    for (int i = 0; i < 3; ++i) {
        float ta = a.t[i], tb = b.t[i], sa = a.s[i], sb = b.s[i];
        tr[i] = (ta + (tb - ta) * f) * t.translationScale;
        sc[i] = (sa + (sb - sa) * f) * t.scaleScale;
    }
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float col[9] = {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w), 2.0f * (x * z - y * w),
                          2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w),
                          2.0f * (x * z + y * w), 2.0f * (y * z - x * w), 1.0f - 2.0f * (x * x + y * y)};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) out.l[3 * c + r] = col[3 * c + r] * sc[c];
    for (int i = 0; i < 3; ++i) out.t[i] = tr[i];
}

// RT-FRAME
void emitterFrameOf(const FxInstance& I, int e, const CurvePos& cp, M34& mb, M34& F) {
    const FxView& fx = *I.fx;
    M34 bone;
    boneMatrix(fx, fx.emitters[e].track, cp, bone);
    ptcsim::compose(I.axes, I.pos, bone, mb);
    ptcsim::emitterFrame(mb, fx.emitters[e].position, F);
}

inline int emitterCountOf(const FxView& fx) {
    int n = fx.h->emitterCount;
    return n < ParticleEngine::kMaxEmitters ? n : ParticleEngine::kMaxEmitters;
}
inline int ribbonCountOf(const FxView& fx) {
    int n = fx.level3Ribbons ? fx.ribbonCount : 0;
    return n < 4 ? n : 4;
}

struct CamBasis {
    glm::vec3 f, r, u;
    float focal, nearZ;
    bool safe(const glm::vec3& c, float rho) const {
        return ge::sphereSafe(dot3(c, r), dot3(c, u), dot3(c, f), rho, nearZ + 0.05f, focal);
    }
};
CamBasis basisOf(const TerrainCamera& cam) {
    CamBasis b;
    b.f = norm3(cam.forward);
    b.r = norm3(cross3(b.f, cam.up));
    b.u = cross3(b.r, b.f);
    b.focal = 136.0f / std::tan(cam.fovyDeg * (3.14159265358979f / 180.0f) * 0.5f);
    b.nearZ = cam.nearZ;
    return b;
}

}  // namespace

// ---- blob view, blend states, world phase ----------------------------------------------------------------------
bool fxViewFromBlob(const void* data, uint32_t size, FxView& v) {
    v = FxView();
    if (!spellFxValid(data, size)) return false;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    const SpellFxHeader* h = reinterpret_cast<const SpellFxHeader*>(b);
    v.h = h;
    v.base = b;
    v.tracks = reinterpret_cast<const SpellFxTrack*>(b + h->trackOffset);
    v.emitters = reinterpret_cast<const SpellFxEmitter*>(b + h->emitterOffset);
    const SpellFxHeaderExt* x = spellFxExt(data);
    if (x) {
        v.ribbons = reinterpret_cast<const SpellFxRibbon*>(b + x->ribbonOffset);
        v.ribbonCount = x->ribbonCount;
        v.cls = x->cls;
        v.reach = x->reach;
        v.level3 = spellFxLevel3(data);
        v.level3Ribbons = spellFxLevel3Ribbons(data);
    }
    // Integration: the mesh part (the older draw path).
    v.groups = reinterpret_cast<const SpellFxGroup*>(b + h->groupOffset);
    v.textures = reinterpret_cast<const uint16_t*>(b + h->textureOffset);
    v.verts = reinterpret_cast<const SpellFxVertex*>(b + h->vertexOffset);
    return true;
}

// Integration: the port's setter (the blob passed spellFxValid when it was loaded).
void FxView::set(const void* blob) {
    *this = FxView();
    base = static_cast<const uint8_t*>(blob);
    h = reinterpret_cast<const SpellFxHeader*>(base);
    tracks = reinterpret_cast<const SpellFxTrack*>(base + h->trackOffset);
    groups = reinterpret_cast<const SpellFxGroup*>(base + h->groupOffset);
    emitters = reinterpret_cast<const SpellFxEmitter*>(base + h->emitterOffset);
    textures = reinterpret_cast<const uint16_t*>(base + h->textureOffset);
    verts = reinterpret_cast<const SpellFxVertex*>(base + h->vertexOffset);
    level3 = spellFxLevel3(blob);
    level3Ribbons = spellFxLevel3Ribbons(blob);
    if (const SpellFxHeaderExt* x = spellFxExt(blob)) {
        ribbons = reinterpret_cast<const SpellFxRibbon*>(base + x->ribbonOffset);
        ribbonCount = x->ribbonCount;
        cls = x->cls;
        reach = x->reach;
    } else {  // a v1 blob: a spell effect, reached by its mesh
        const float c = std::sqrt(h->center[0] * h->center[0] + h->center[1] * h->center[1] + h->center[2] * h->center[2]);
        reach = h->radius + c > 4.0f ? h->radius + c : 4.0f;
    }
}

FxDrawState fxBlendState(uint8_t blend) {
    FxDrawState s;
    switch (blend) {
        case 0:  // source x 1 + destination x 0, no test
        case 1:  // as 0, alpha > 223
            s.src = s.dst = FxFactor::Fixed;
            s.srcFix = 0xFFFFFFFFu;
            s.dstFix = 0;
            s.alphaTest = blend == 1;
            s.alphaRef = blend == 1 ? 223 : 0;
            break;
        case 3:  // source x 1 + destination x 1
            s.src = s.dst = FxFactor::Fixed;
            s.srcFix = s.dstFix = 0xFFFFFFFFu;
            break;
        case 4:  // source x source alpha + destination x 1, alpha > 0
            s.src = FxFactor::SrcAlpha;
            s.dst = FxFactor::Fixed;
            s.dstFix = 0xFFFFFFFFu;
            s.alphaTest = true;
            break;
        case 5:  // source x destination + destination x 0
            s.src = FxFactor::DstColor;
            s.dst = FxFactor::Fixed;
            s.dstFix = 0;
            break;
        case 6:  // source x destination + destination x source
            s.src = FxFactor::DstColor;
            s.dst = FxFactor::SrcColor;
            break;
        default:  // 2 and anything not listed: alpha blend, alpha > 0
            s.src = FxFactor::SrcAlpha;
            s.dst = FxFactor::OneMinusSrcAlpha;
            s.alphaTest = true;
            break;
    }
    return s;
}

float fxWorldPhaseMs(uint32_t uniqueId) { return static_cast<float>((uniqueId * 2654435761u) >> 12); }

ptcsim::EmitterValues fxEmitterValuesAt(const FxView& fx, int e, float timeMs) {
    return valuesAt(fx, e, curvePos(*fx.h, timeMs));
}

void fxEmitterFrameAt(const FxInstance& I, int e, M34& F) {
    M34 mb;
    emitterFrameOf(I, e, curvePos(*I.fx->h, I.timeMs), mb, F);
}

// ---- engine lifetime, pool, owners, lanes ----------------------------------------------------------------------
ParticleEngine::ParticleEngine() {
    std::memset(ownerAcc_, 0, sizeof ownerAcc_);
    std::memset(counts_, 0, sizeof counts_);
    for (int i = 0; i < kOwners; ++i) ownerInst_[i] = -1;
}
ParticleEngine::~ParticleEngine() { release(); }

bool ParticleEngine::acquire(const FxPoolConfig& cfg) {
    release();
    int cap = cfg.spellShare + cfg.worldShare + cfg.weaponShare;
    if (cap <= 0 || cap > 65535) return false;
    size_t szPool = sizeof(Particle) * static_cast<size_t>(cap);
    size_t szStack = sizeof(uint16_t) * static_cast<size_t>(cap);
    size_t szNext = sizeof(uint16_t) * static_cast<size_t>(cap);
    laneCount_ = cfg.lanes < 0 ? 0 : (cfg.lanes > kMaxLanes ? kMaxLanes : cfg.lanes);  // Integration
    size_t szLanes = sizeof(Lane) * static_cast<size_t>(laneCount_);
    size_t szCache = sizeof(EmitCache) * kOwners * kMaxEmitters;
    auto up = [](size_t v) { return (v + 15) & ~static_cast<size_t>(15); };
    size_t total = up(szPool) + up(szStack) + up(szNext) + up(szLanes) + up(szCache);
    uint8_t* blk = static_cast<uint8_t*>(std::malloc(total));
    if (!blk) return false;
    block_ = blk;
    pool_ = reinterpret_cast<Particle*>(blk);
    blk += up(szPool);
    freeStack_ = reinterpret_cast<uint16_t*>(blk);
    blk += up(szStack);
    bucketNext_ = reinterpret_cast<uint16_t*>(blk);
    blk += up(szNext);
    lanes_ = reinterpret_cast<Lane*>(blk);
    blk += up(szLanes);
    cache_ = reinterpret_cast<EmitCache*>(blk);
    cap_ = cap;
    std::memset(pool_, 0, szPool);
    for (int i = 0; i < cap; ++i) pool_[i].owner = kFreeOwner;
    // LIFO stack of free slots; a fresh pool hands out 0, 1, 2, ...
    freeTop_ = cap;
    for (int i = 0; i < cap; ++i) freeStack_[i] = static_cast<uint16_t>(cap - 1 - i);
    std::memset(lanes_, 0, szLanes);
    std::memset(cache_, 0, szCache);
    for (int i = 0; i < kOwners; ++i) owners_[i] = OwnerState();
    std::memset(ownerAcc_, 0, sizeof ownerAcc_);
    return true;
}

void ParticleEngine::release() {
    std::free(block_);
    block_ = nullptr;
    pool_ = nullptr;
    freeStack_ = nullptr;
    bucketNext_ = nullptr;
    lanes_ = nullptr;
    cache_ = nullptr;
    cap_ = 0;
    freeTop_ = 0;
    laneCount_ = 0;  // Integration
}

int ParticleEngine::popSlot() {
    if (freeTop_ <= 0) return -1;
    return freeStack_[--freeTop_];
}

void ParticleEngine::freeSlot(int slot) {
    pool_[slot].owner = kFreeOwner;
    freeStack_[freeTop_++] = static_cast<uint16_t>(slot);
}

int ParticleEngine::liveCount(uint8_t lo, uint8_t hi) const {
    int n = 0;
    for (int s = 0; s < cap_; ++s) {
        uint8_t o = pool_[s].owner;
        if (o != kFreeOwner && o >= lo && o < hi) ++n;
    }
    return n;
}

int ParticleEngine::allocLane() {
    if (!lanes_) return -1;
    for (int i = 0; i < laneCount_; ++i) {
        if (!lanes_[i].used) {
            std::memset(&lanes_[i], 0, sizeof(Lane));
            lanes_[i].used = true;
            return i;
        }
    }
    return -1;
}

void ParticleEngine::freeLane(int lane) {
    if (lanes_ && lane >= 0 && lane < laneCount_) std::memset(&lanes_[lane], 0, sizeof(Lane));
}

void ParticleEngine::freeOwner(uint8_t owner) {
    if (owner >= kOwners) return;
    for (int s = 0; s < cap_; ++s)
        if (pool_[s].owner == owner) freeSlot(s);
    owners_[owner] = OwnerState();
    for (int e = 0; e < kMaxEmitters; ++e) ownerAcc_[owner][e] = 0.0f;
}

void ParticleEngine::shiftOwnersDown(uint8_t removed, uint8_t hi) {
    if (removed >= kOwners) return;
    if (hi > kOwners) hi = kOwners;
    freeOwner(removed);
    for (int s = 0; s < cap_; ++s) {
        uint8_t o = pool_[s].owner;
        if (o != kFreeOwner && o > removed && o < hi) pool_[s].owner = static_cast<uint8_t>(o - 1);
    }
    for (int o = removed + 1; o < hi; ++o) {
        owners_[o - 1] = owners_[o];
        for (int e = 0; e < kMaxEmitters; ++e) ownerAcc_[o - 1][e] = ownerAcc_[o][e];
    }
    if (hi > removed) {
        owners_[hi - 1] = OwnerState();
        for (int e = 0; e < kMaxEmitters; ++e) ownerAcc_[hi - 1][e] = 0.0f;
    }
}

// ---- step (RT-STEP) --------------------------------------------------------------------------------------------
void ParticleEngine::step(FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt, FxFrameStats& stats) {
    stats.emitted = 0;
    stats.live = 0;
    if (!pool_) return;
    if (hi > kOwners) hi = kOwners;
    if (!fxFinite(dt) || dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;  // RT-STEP-1
    // Integration: wowee.cfg ptclevel3=0 - the older rules run every instance.
    if (!level3Rules_) {
        if (legacy_) legacy_->step(*this, in, n, lo, hi, share, dt, false, stats);
        stats.live = static_cast<uint32_t>(liveCount(lo, hi));
        return;
    }
    int parts = 1;
    if (dt > 0.1f) parts = static_cast<int>(std::ceil(dt / 0.1f));
    const float h = dt / static_cast<float>(parts);
    for (int p = 0; p < parts; ++p) subStep(in, n, lo, hi, share, h, stats);
    // Integration: the instances without the level-3 section (older paks) - the older rules on the same pool.
    if (legacy_) {
        legacy_->step(*this, in, n, lo, hi, share, dt, true, stats);
        stats.live = static_cast<uint32_t>(liveCount(lo, hi));
    }
}

void ParticleEngine::subStep(FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt, FxFrameStats& stats) {
    // instances of this user, by owner
    for (int o = 0; o < kOwners; ++o) ownerInst_[o] = -1;
    for (int i = 0; i < n; ++i) {
        uint8_t o = in[i].owner;
        if (o >= lo && o < hi && o < kOwners && ownerInst_[o] < 0) ownerInst_[o] = static_cast<int16_t>(i);
    }
    // RT-STEP-2: particles of owners without an instance are freed before anything is counted
    for (int s = 0; s < cap_; ++s) {
        uint8_t o = pool_[s].owner;
        if (o != kFreeOwner && o >= lo && o < hi && ownerInst_[o] < 0) freeSlot(s);
    }
    // live counts at the start of the step
    std::memset(counts_, 0, sizeof counts_);
    userLive_ = 0;
    for (int s = 0; s < cap_; ++s) {
        uint8_t o = pool_[s].owner;
        if (o == kFreeOwner || o < lo || o >= hi) continue;
        ++userLive_;
        if (pool_[s].emitter < kMaxEmitters) ++counts_[o][pool_[s].emitter];
    }
    // emission of all instances (list order) before any particle is aged
    for (int i = 0; i < n; ++i) {
        FxInstance& I = in[i];
        if (!level3Instance(I) || I.owner < lo || I.owner >= hi || ownerInst_[I.owner] != i) continue;
        emitInstance(I, i, share, dt, stats);
    }
    // aging and motion of every live particle of the user, in slot order
    for (int s = 0; s < cap_; ++s) {
        Particle& p = pool_[s];
        uint8_t o = p.owner;
        if (o == kFreeOwner || o < lo || o >= hi) continue;
        const FxInstance& I = in[ownerInst_[o]];
        if (!level3Instance(I) || p.emitter >= emitterCountOf(*I.fx)) continue;  // not ours (older path)
        const SpellFxL3Emitter& c = I.fx->level3[p.emitter];
        const EmitCache& ec = cache_[o * kMaxEmitters + p.emitter];
        p.life += dt;
        float span = ec.life + p.maxLife;
        if (span < 0.001f) span = 0.001f;
        if (p.life >= span) {  // RT-STEP-3
            freeSlot(s);
            continue;
        }
        glm::vec3 origin(0.0f);
        if (!(c.flags & kM2PtcLocal)) origin = glm::vec3(ec.ox, ec.oy, ec.oz);  // RT-STEP-5
        glm::vec3 follow(0.0f);
        if (c.flags & kM2PtcFollow) {  // RT-STEP-6
            const glm::vec3& dl = owners_[o].lastDisp;
            float speed = dt > 0.0f ? len3(dl) / dt : 0.0f;
            follow = dl * clampf(c.followBase + c.followSlope * speed, 0.0f, 1.0f);
        }
        ptcsim::EmitterValues v;
        std::memset(&v, 0, sizeof v);
        v.gravity = ec.gravity;
        if (!ptcsim::update(c, v, origin, follow, dt, p)) freeSlot(s);  // RT-STEP-4, SIM-UPD-6
    }
    // ribbons of all instances, list order, after the particles
    for (int i = 0; i < n; ++i) {
        FxInstance& I = in[i];
        if (!level3Instance(I)) continue;
        int nr = ribbonCountOf(*I.fx);
        for (int r = 0; r < nr; ++r) {
            int ln = I.lanes[r];
            if (ln >= 0 && ln < laneCount_ && lanes_[ln].used) updateRibbon(I, r, lanes_[ln], dt);
        }
    }
    stats.live = liveCount(lo, hi);
}

void ParticleEngine::emitInstance(FxInstance& I, int /*idx*/, int share, float dt, FxFrameStats& stats) {
    const FxView& fx = *I.fx;
    OwnerState& os = owners_[I.owner];
    // RT-EMIT-10, before the emitters
    glm::vec3 D(0.0f);
    if (os.valid) D = I.pos - os.prevOrigin;
    os.clock += dt;
    if (os.clock > 1.0f / 30.0f) {
        os.inherit = D / (os.clock * 29.999998f);
        os.clock = 0.0f;
    }
    const CurvePos cp = curvePos(*fx.h, I.timeMs);
    const int ne = emitterCountOf(fx);
    const int capEff = I.cap > 1 ? I.cap : 1;
    const float throttle = I.camDist > 0.0f ? ptcsim::distanceThrottle(I.camDist) : 1.0f;  // RT-EMIT-5
    float* acc = I.acc ? I.acc : ownerAcc_[I.owner];
    for (int e = 0; e < ne; ++e) {
        const SpellFxL3Emitter& c = fx.level3[e];
        const ptcsim::EmitterValues v = valuesAt(fx, e, cp);  // RT-EMIT-1
        M34 mb, F;
        emitterFrameOf(I, e, cp, mb, F);
        EmitCache& ec = cache_[I.owner * kMaxEmitters + e];
        ec.life = v.life;
        ec.gravity = v.gravity;
        ec.ox = F.t[0];
        ec.oy = F.t[1];
        ec.oz = F.t[2];

        // RT-EMIT-2
        const bool burstOnce = (c.flags & kM2PtcBurstOnce) != 0;
        bool burst = false;
        float base;
        if (burstOnce) {
            const uint16_t bit = static_cast<uint16_t>(1u << e);
            if (v.rate > 0.0f) {
                if (!(os.burstBits & bit)) {
                    burst = true;
                    os.burstBits = static_cast<uint16_t>(os.burstBits | bit);
                }
            } else {
                os.burstBits = static_cast<uint16_t>(os.burstBits & ~bit);
            }
            base = v.rate;
        } else {
            base = v.enabled >= 0.5f ? (v.rate > 0.0f ? v.rate : 0.0f) : 0.0f;
        }
        if (c.type == 3) base *= clampf(v.areaW, 0.0f, 1.0f);  // RT-EMIT-3
        ptcsim::Rng& R = emitterRng(e);
        float rate = (base + ptcsim::drawS(R) * c.rateVary) * I.rate * throttle;  // RT-EMIT-4

        // RT-EMIT-6
        int count = 0;
        if (burstOnce) {
            if (burst && rate > 0.0f) count = static_cast<int>(rate);
        } else {
            float lifeNow = v.life > 0.05f ? v.life : 0.05f;
            float lim = static_cast<float>(capEff) / lifeNow;
            if (rate > lim) rate = lim;
            acc[e] += rate * dt;
            float a = acc[e] + 0.5f;
            count = a > 0.0f ? static_cast<int>(a) : 0;
        }
        // RT-EMIT-7
        if (count <= 0) {
            if (acc[e] > 2.0f) acc[e] = 0.0f;
            continue;
        }
        // RT-EMIT-8
        const float* spline = c.splineCount > 0 ? reinterpret_cast<const float*>(fx.base + c.splineOffset) : nullptr;
        const glm::vec3 inherit = os.inherit * c.burst;
        int born = 0;
        while (born < count && counts_[I.owner][e] < capEff && userLive_ < share) {
            int slot = popSlot();
            if (slot < 0) break;
            Particle& p = pool_[slot];
            std::memset(&p, 0, sizeof p);
            p.owner = I.owner;
            p.emitter = static_cast<uint8_t>(e);
            M34 Fb = F;
            if ((c.flags & kM2PtcAlongPath) && !burst) {  // RT-EMIT-11
                float u = ptcsim::drawU(R);
                Fb.t[0] = F.t[0] - D.x * (1.0f - u);
                Fb.t[1] = F.t[1] - D.y * (1.0f - u);
                Fb.t[2] = F.t[2] - D.z * (1.0f - u);
            }
            ptcsim::spawn(c, v, spline, Fb, inherit, R, burst ? 0.0f : dt, p);
            p.owner = I.owner;
            p.emitter = static_cast<uint8_t>(e);
            ++counts_[I.owner][e];
            ++userLive_;
            ++born;
            if (birthFn_) birthFn_(birthUser_, slot, I.owner, static_cast<uint8_t>(e));
        }
        stats.emitted += born;
        if (!burstOnce) {
            acc[e] -= static_cast<float>(born);
            if (acc[e] > 2.0f) acc[e] = 0.0f;
        }
    }
    // RT-EMIT-10, after the emitters
    os.prevOrigin = I.pos;
    os.lastDisp = D;
    os.valid = true;
}

// ---- ribbons (RT-RIB) ------------------------------------------------------------------------------------------
void ParticleEngine::updateRibbon(FxInstance& I, int r, Lane& L, float dt) {
    const FxView& fx = *I.fx;
    const SpellFxRibbon& rb = fx.ribbons[r];
    const SpellFxL3Ribbon& q = fx.level3Ribbons[r];
    const CurvePos cp = curvePos(*fx.h, I.timeMs);
    M34 bone, W;
    boneMatrix(fx, rb.track, cp, bone);
    ptcsim::compose(I.axes, I.pos, bone, W);
    const glm::vec3 S = ptcsim::apply(W, glm::vec3(rb.position[0], rb.position[1], rb.position[2]));
    const glm::vec3 Y(W.l[3], W.l[4], W.l[5]);
    if (!finite3(S)) return;  // RT-RIB-1

    float h;
    if (!L.started) {  // RT-RIB-2
        h = q.edgesPerSecond > 0.0f ? 1.0f / q.edgesPerSecond + 1e-4f : dt;
        L.prevSpine = S;
        L.prevY = Y;
        L.started = true;
    } else {
        h = dt;
    }
    h = clampf(h, 0.0f, q.edgeLifetime);

    // RT-RIB-3: expiry before births
    while (L.count > 0 && L.ring[L.first].age + h > q.edgeLifetime) {
        L.first = static_cast<int16_t>((L.first + 1) % kRingEdges);
        --L.count;
    }
    float cv[kFxRibbonCurves];
    for (int k = 0; k < kFxRibbonCurves; ++k) cv[k] = curveValue(fx.base, q.curveOffset[k], q.curveFrames[k], cp, false);
    // RT-RIB-4
    if (cv[kFxRibbonVisible] >= 0.5f) {
        const float acc0 = L.acc;
        const float E = h * q.edgesPerSecond + acc0;
        if (E >= 1.0f) {
            const int m = static_cast<int>(E);
            const uint32_t col = packColor(cv[kFxRibbonR], cv[kFxRibbonG], cv[kFxRibbonB],
                                           clampf(cv[kFxRibbonAlpha] * I.alpha, 0.0f, 1.0f));
            // only the newest kRingEdges births can survive the ring; skipping the others gives the same ring
            int k0 = m > kRingEdges ? m - kRingEdges + 1 : 1;
            for (int k = k0; k <= m; ++k) {
                float t = (static_cast<float>(k) - acc0) / (E - acc0);
                glm::vec3 sp = L.prevSpine + (S - L.prevSpine) * t;
                glm::vec3 yk = L.prevY + (Y - L.prevY) * t;
                Edge ed;
                ed.bottom = sp - yk * cv[kFxRibbonBelow];
                ed.top = sp + yk * cv[kFxRibbonAbove];
                ed.color = col;
                ed.age = -t * h;
                if (L.count >= kRingEdges) {  // a full ring drops its oldest edge first
                    L.first = static_cast<int16_t>((L.first + 1) % kRingEdges);
                    --L.count;
                }
                L.ring[(L.first + L.count) % kRingEdges] = ed;
                ++L.count;
            }
        }
        L.acc = E - std::floor(E);
    }
    // RT-RIB-5
    for (int i = 0; i < L.count; ++i) {
        Edge& ed = L.ring[(L.first + i) % kRingEdges];
        float dz = (2.0f * ed.age + h) * q.gravity * h;
        ed.bottom.z += dz;
        ed.top.z += dz;
        ed.age += h;
    }
    // RT-RIB-6
    L.prevSpine = S;
    L.prevY = Y;
}

// ---- drawing (RT-DRAW, RT-RDRAW) -------------------------------------------------------------------------------
void ParticleEngine::draw(FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam, int maxPx,
                          FxFrameStats& stats) {
    stats.drawCalls = 0;
    stats.vertices = 0;
    if (!pool_) return;
    if (hi > kOwners) hi = kOwners;
    // Integration: the older draw path first (GE state, the WSFX meshes, the older rules' sprites) ...
    if (legacy_) legacy_->drawBefore(*this, in, n, lo, hi, cam, maxPx, level3Rules_, stats);
    if (level3Rules_ && out_) drawLevel3(in, n, lo, hi, cam, maxPx, stats);
    // ... and after the level-3 draws its ribbons and the GE state back.
    if (legacy_) legacy_->drawAfter(*this, in, n, lo, hi, cam, level3Rules_, stats);
}

void ParticleEngine::drawLevel3(FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam, int maxPx,
                                FxFrameStats& stats) {
    const CamBasis cb = basisOf(cam);
    // buckets (owner, emitter) in ascending slot order; counts_ holds the bucket sizes during the draw
    uint16_t* next = bucketNext_;
    for (int o = lo; o < hi; ++o)
        for (int e = 0; e < kMaxEmitters; ++e) {
            bucketHead_[o][e] = kNoSlot;
            bucketTail_[o][e] = kNoSlot;
            counts_[o][e] = 0;
        }
    for (int s = 0; s < cap_; ++s) {
        uint8_t o = pool_[s].owner;
        uint8_t e = pool_[s].emitter;
        if (o == kFreeOwner || o < lo || o >= hi || e >= kMaxEmitters) continue;
        next[s] = kNoSlot;
        if (bucketTail_[o][e] == kNoSlot) bucketHead_[o][e] = static_cast<uint16_t>(s);
        else next[bucketTail_[o][e]] = static_cast<uint16_t>(s);
        bucketTail_[o][e] = static_cast<uint16_t>(s);
        ++counts_[o][e];
    }
    for (int i = 0; i < n; ++i) {
        FxInstance& I = in[i];
        if (!level3Instance(I) || I.owner < lo || I.owner >= hi) continue;
        int ne = emitterCountOf(*I.fx);
        for (int e = 0; e < ne; ++e) drawEmitter(I, e, cam, cb.focal, maxPx, stats);
    }
    for (int i = 0; i < n; ++i) {
        FxInstance& I = in[i];
        if (!level3Instance(I)) continue;
        int nr = ribbonCountOf(*I.fx);
        for (int r = 0; r < nr; ++r) {
            int ln = I.lanes[r];
            if (ln >= 0 && ln < laneCount_ && lanes_[ln].used && lanes_[ln].count > 0)
                drawRibbon(I, r, lanes_[ln], cam, cb.focal, stats);
        }
    }
}

void ParticleEngine::drawEmitter(FxInstance& I, int e, const TerrainCamera& cam, float focal, int maxPx,
                                 FxFrameStats& stats) {
    const FxView& fx = *I.fx;
    const SpellFxEmitter& em = fx.emitters[e];
    const SpellFxL3Emitter& c = fx.level3[e];
    const uint32_t fl = c.flags;
    const int cnt = counts_[I.owner][e];
    if (cnt == 0) return;
    if (!(fl & (kM2PtcHead | kM2PtcTail))) return;  // RT-DRAW-1
    const void* tex = nullptr;
    if (em.texture != kFxNone) {
        tex = I.tex ? I.tex(I.texUser, em.texture) : nullptr;
        if (!tex) return;
    }
    const bool head = (fl & kM2PtcHead) != 0, tail = (fl & kM2PtcTail) != 0;
    const bool velQuad = (fl & kM2PtcVelocityQuad) != 0;
    const bool quad = tail || c.spin != 0.0f || c.spinVary != 0.0f || (fl & kM2PtcXYQuad) || velQuad;  // RT-DRAW-2
    FxDrawState st = fxBlendState(c.blend);  // RT-DRAW-3
    st.prim = quad ? FxPrim::Triangles : FxPrim::Sprites;
    st.textureId = em.texture == kFxNone ? -1 : static_cast<int32_t>(em.texture);
    st.texture = tex;
    const bool keepZeroAlpha = c.blend == 3 || c.blend == 5 || c.blend == 6;

    const CamBasis cb = basisOf(cam);
    const CurvePos cp = curvePos(*fx.h, I.timeMs);
    const ptcsim::EmitterValues v = valuesAt(fx, e, cp);
    M34 mb, F;
    emitterFrameOf(I, e, cp, mb, F);
    const glm::vec3 fx0(F.l[0], F.l[1], F.l[2]), fy0(F.l[3], F.l[4], F.l[5]);
    const float modelScale = len3(fx0);
    const uint32_t cells = static_cast<uint32_t>(em.cols ? em.cols : 1) * static_cast<uint32_t>(em.rows ? em.rows : 1);
    const bool local = (fl & kM2PtcLocal) != 0;
    const glm::vec3 camPos = cam.pos;

    // world tint (RT-DRAW-7)
    float tint[3] = {1.0f, 1.0f, 1.0f};
    if (fl & kM2PtcLit) {
        const FrameLight& L = frameLight();
        if (L.tint == kTintGeAmbient) {
            float k = L.twoX ? 2.0f : 1.0f;
            tint[0] = static_cast<float>(L.ambientABGR & 0xFF) / 255.0f * k;
            tint[1] = static_cast<float>((L.ambientABGR >> 8) & 0xFF) / 255.0f * k;
            tint[2] = static_cast<float>((L.ambientABGR >> 16) & 0xFF) / 255.0f * k;
        }
    }
    // XY plane axes (RT-DRAW-9)
    glm::vec3 axA = cb.r, axB = cb.u;
    if ((fl & kM2PtcXYQuad) && !velQuad) {
        if (local) {
            axA = fx0;
            axB = fy0;
            if (modelScale > 1e-6f) {
                axA = fx0 / modelScale;
                axB = fy0 / modelScale;
            }
        } else {
            axA = glm::vec3(1.0f, 0.0f, 0.0f);
            axB = glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    // order: ascending slots, or by depth for sorted emitters (RT-DRAW-8)
    uint16_t order[256];
    int nOrder = 0;
    const bool sorted = (fl & kM2PtcSort) && cnt <= 256;
    if (sorted) {
        float depth[256];
        for (uint16_t s = bucketHead_[I.owner][e]; s != kNoSlot; s = bucketNext_[s]) {
            const Particle& p = pool_[s];
            glm::vec3 w = local ? ptcsim::apply(F, p.pos) : p.pos;
            float d = dot3(w - camPos, cb.f);
            int j = nOrder++;
            while (j > 0 && depth[j - 1] < d) {  // farthest first, stable for equal depths
                depth[j] = depth[j - 1];
                order[j] = order[j - 1];
                --j;
            }
            depth[j] = d;
            order[j] = s;
        }
    }

    const int per = quad ? (head ? 6 : 0) + (tail ? 6 : 0) : 2;
    FxVertex* vb = out_->reserve(cnt * per);
    if (!vb) return;
    int nv = 0;
    int drawn = 0;  // Integration: particles drawn (the port's frame lines)
    const float nearLimit = cam.nearZ + 0.05f;
    int idx = 0;
    for (uint16_t s = sorted ? (nOrder ? order[0] : kNoSlot) : bucketHead_[I.owner][e]; s != kNoSlot;
         s = sorted ? (++idx < nOrder ? order[idx] : kNoSlot) : bucketNext_[s]) {
        const Particle& p = pool_[s];
        ptcsim::Look L;
        if (!ptcsim::look(fx.base, c, v, s, cells, modelScale, I.alpha, p, L)) continue;  // RT-DRAW-4
        // RT-DRAW-5
        const glm::vec3 w = local ? ptcsim::apply(F, p.pos) : p.pos;
        const glm::vec3 vel = local ? ptcsim::rotate(F, p.vel) : p.vel;
        const glm::vec3 cc = w - camPos;
        const float depth = dot3(cc, cb.f);
        if (depth < nearLimit) continue;
        if (L.rgba[3] <= 0.0f && !keepZeroAlpha) continue;
        float hx = L.half[0], hy = L.half[1];
        const float lim = 0.5f * static_cast<float>(maxPx) / focal * depth;
        const float big = std::fabs(hx) > std::fabs(hy) ? std::fabs(hx) : std::fabs(hy);
        if (big > lim && big > 0.0f) {
            const float k = lim / big;
            hx *= k;
            hy *= k;
        }
        const uint32_t col = packColor(L.rgba[0] * tint[0], L.rgba[1] * tint[1], L.rgba[2] * tint[2], L.rgba[3]);
        float uv[4];
        ptcsim::cellUv(em, c, L.head, uv);

        ++drawn;
        if (!quad) {  // RT-DRAW-6
            glm::vec3 a = cc - cb.r * hx + cb.u * hy, b = cc + cb.r * hx - cb.u * hy;
            vb[nv++] = FxVertex{uv[0], uv[1], col, a.x, a.y, a.z};
            vb[nv++] = FxVertex{uv[0] + uv[2], uv[1] + uv[3], col, b.x, b.y, b.z};
            continue;
        }
        static const float kx[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        static const float ky[4] = {1.0f, -1.0f, 1.0f, -1.0f};
        static const int kOrder[6] = {0, 1, 2, 3, 2, 1};
        if (head) {  // RT-DRAW-9 / RT-DRAW-10
            float sx = hx, dx = 1.0f, dy = 0.0f;
            bool turned = false;
            if (velQuad && dot3(vel, vel) > 2.384185791015625e-7f) {
                float vx = -dot3(vel, cb.r), vy = -dot3(vel, cb.u), vz = -dot3(vel, cb.f);
                float l2 = vx * vx + vy * vy;
                if (l2 > 2.384185791015625e-7f) {
                    float l = std::sqrt(l2);
                    dx = vx / l;
                    dy = vy / l;
                    sx = hx * l / std::sqrt(l2 + vz * vz);
                    turned = true;
                }
            } else if (L.spin) {
                dx = std::cos(L.angle);
                dy = std::sin(L.angle);
                turned = true;
            }
            glm::vec3 corner[4];
            for (int j = 0; j < 4; ++j) {
                float ox = kx[j] * sx, oy = ky[j] * hy;
                if (turned) {
                    float tx = ox * dx - oy * dy, ty = ox * dy + oy * dx;
                    ox = tx;
                    oy = ty;
                }
                corner[j] = cc + axA * ox + axB * oy;
            }
            const float cu[4] = {uv[0], uv[0], uv[0] + uv[2], uv[0] + uv[2]};
            const float cvv[4] = {uv[1], uv[1] + uv[3], uv[1], uv[1] + uv[3]};
            for (int j : kOrder) vb[nv++] = FxVertex{cu[j], cvv[j], col, corner[j].x, corner[j].y, corner[j].z};
        }
        if (tail) {  // RT-DRAW-11
            float tuv[4];
            ptcsim::cellUv(em, c, L.tail, tuv);
            float length = c.tailLength;
            if ((fl & kM2PtcTailGrows) && p.life < length) length = p.life;
            glm::vec3 T = -vel * length;
            const float rho = std::fabs(hx) > std::fabs(hy) ? std::fabs(hx) : std::fabs(hy);
            if (!cb.safe(cc, rho)) {
                T = glm::vec3(0.0f);
            } else {
                int halvings = 0;
                while (!cb.safe(cc + T, rho) && halvings < 4) {
                    T *= 0.5f;
                    ++halvings;
                }
                if (!cb.safe(cc + T, rho)) T = glm::vec3(0.0f);
            }
            const float tx = dot3(T, cb.r), ty = dot3(T, cb.u);
            glm::vec3 corner[4];
            if (tx * tx + ty * ty > 1.0f / 1296.0f) {
                float l = std::sqrt(tx * tx + ty * ty);
                float dx = tx / l, dy = ty / l;
                glm::vec3 side = cb.r * (-hy * dy) + cb.u * (hx * dx);
                corner[0] = cc + side;
                corner[1] = cc - side;
                corner[2] = cc + T + side;
                corner[3] = cc + T - side;
            } else {
                corner[0] = cc - cb.r * hx + cb.u * hy;
                corner[1] = cc - cb.r * hx - cb.u * hy;
                corner[2] = cc + cb.r * hx + cb.u * hy;
                corner[3] = cc + cb.r * hx - cb.u * hy;
            }
            const float cu[4] = {tuv[0], tuv[0], tuv[0] + tuv[2], tuv[0] + tuv[2]};
            const float cvv[4] = {tuv[1], tuv[1] + tuv[3], tuv[1], tuv[1] + tuv[3]};
            for (int j : kOrder) vb[nv++] = FxVertex{cu[j], cvv[j], col, corner[j].x, corner[j].y, corner[j].z};
        }
    }
    if (nv > 0) {  // RT-DRAW-12
        out_->submit(st, vb, nv);
        ++stats.drawCalls;
        stats.vertices += nv;
        ++stats.draws;  // Integration
        stats.sprites += static_cast<uint32_t>(drawn);
        I.drawn = true;
    }
}

void ParticleEngine::drawRibbon(FxInstance& I, int r, const Lane& L, const TerrainCamera& cam, float /*focal*/,
                                FxFrameStats& stats) {
    const FxView& fx = *I.fx;
    const SpellFxRibbon& rb = fx.ribbons[r];
    const SpellFxL3Ribbon& q = fx.level3Ribbons[r];
    const void* tex = nullptr;
    if (rb.texture != kFxNone) {  // RT-RDRAW-1
        tex = I.tex ? I.tex(I.texUser, rb.texture) : nullptr;
        if (!tex) return;
    }
    FxDrawState st = fxBlendState(q.blend);
    st.prim = FxPrim::TriangleStrip;
    st.textureId = rb.texture == kFxNone ? -1 : static_cast<int32_t>(rb.texture);
    st.texture = tex;

    const CamBasis cb = basisOf(cam);
    const CurvePos cp = curvePos(*fx.h, I.timeMs);
    const float above = curveValue(fx.base, q.curveOffset[kFxRibbonAbove], q.curveFrames[kFxRibbonAbove], cp, false);
    const float below = curveValue(fx.base, q.curveOffset[kFxRibbonBelow], q.curveFrames[kFxRibbonBelow], cp, false);
    const float slotF = curveValue(fx.base, q.curveOffset[kFxRibbonSlot], q.curveFrames[kFxRibbonSlot], cp, false);
    // RT-RDRAW-3
    float sl = std::trunc(slotF + 0.5f);
    uint32_t slot = (fxFinite(sl) && sl > 0.0f) ? (sl < 4294967040.0f ? static_cast<uint32_t>(sl) : 0xFFFFFF00u) : 0u;
    const uint32_t cols = q.cols ? q.cols : 1u, rows = q.rows ? q.rows : 1u;
    const float du = 1.0f / static_cast<float>(cols), dv = 1.0f / static_cast<float>(rows);
    const float u0 = static_cast<float>(slot % cols) * du;
    const float v0 = static_cast<float>((slot / cols) % rows) * dv;

    const int pairs = L.count + 1;
    FxVertex* vb = out_->reserve(2 * pairs);
    if (!vb) return;
    const glm::vec3 camPos = cam.pos;
    // RT-RDRAW-2: head pair, then edges newest to oldest
    int runStart = -1;  // vertex index of the first pair of the current run of safe pairs
    int runPairs = 0;
    int nv = 0;
    auto flush = [&]() {
        if (runPairs >= 2) {  // RT-RDRAW-4
            out_->submit(st, vb + runStart, 2 * runPairs);
            ++stats.drawCalls;
            stats.vertices += 2 * runPairs;
            ++stats.draws;  // Integration
            stats.ribbonVerts += static_cast<uint32_t>(2 * runPairs);
            I.drawn = true;
        }
        runStart = -1;
        runPairs = 0;
    };
    for (int i = 0; i < pairs; ++i) {
        glm::vec3 bottom, top;
        float age;
        uint32_t col;
        const Edge& newest = L.edge(L.count - 1);
        if (i == 0) {
            bottom = L.prevSpine - L.prevY * below;
            top = L.prevSpine + L.prevY * above;
            age = 0.0f;
            col = newest.color;
        } else {
            const Edge& ed = L.edge(L.count - i);
            bottom = ed.bottom;
            top = ed.top;
            age = ed.age;
            col = ed.color;
        }
        const glm::vec3 cb0 = bottom - camPos, ct0 = top - camPos;
        if (!(cb.safe(cb0, 0.0f) && cb.safe(ct0, 0.0f))) {
            flush();
            continue;
        }
        const float u = u0 + clampf(age / q.edgeLifetime, 0.0f, 1.0f) * du;
        if (runStart < 0) runStart = nv;
        vb[nv++] = FxVertex{u, v0, col, cb0.x, cb0.y, cb0.z};
        vb[nv++] = FxVertex{u, v0 + dv, col, ct0.x, ct0.y, ct0.z};
        ++runPairs;
    }
    flush();
}

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
