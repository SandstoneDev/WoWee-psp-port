// The particle engine, the world's emitters and the weather - see
// include/rendering/psp/psp_particles.hpp. Upstream:
// src/rendering/m2_renderer_particles.cpp (emitParticles, updateParticles, updateRibbons,
// renderM2Ribbons, renderM2Particles, renderSmokeParticles), m2_renderer_render.cpp (M2Renderer::update:
// the particle pass over the instances, the smoke), src/rendering/weather.cpp.
// The emission / motion / sprite code is the spell effects' (psp_spellfx.cpp), generalised:
// an instance has a model matrix (doodads), a look class (upstream's per-model rules) and lives in
// one pool with the other user's instances.
#include "rendering/psp/psp_particles.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_system.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_frustum.hpp"
#include "rendering/psp/psp_ge_limits.hpp"  // ge::sphereSafe
#include "rendering/psp/psp_particles_l3_gu.hpp"  // the level-3 draws on sceGu (FxGuOut)

#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <unordered_set>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;
namespace prof = wowee::platform::psp::prof;

namespace {

// upstream draws a particle as a point sprite of aSize * 500 / distance pixels (renderM2Particles,
// m2_particle.vert); at 1080 lines that is aSize * 0.463 / distance of the screen height. On the
// PSP's 272 lines with the 54 degree camera (focal 267 px) the same share is aSize * 0.47 yards
// (as the spell effects have it).
constexpr float kSpriteYardsPerScale = 0.47f;
// upstream m2_smoke.vert: aSize * (spark ? 0.12 : 0.3) * screenHeight / distance px, at most 64 px:
// the same share of the screen = aSize * 0.3 (0.12) yd x 272 / 267, at most 64 / 1080 of it.
constexpr float kSmokeYards = 0.3f * 272.0f / 267.0f, kSparkYards = 0.12f * 272.0f / 267.0f;
constexpr float kSmokeMaxShare = 64.0f / 1080.0f;
constexpr float kSmokeInterval = 1.0f / 48.0f;  // upstream kSmokeEmitInterval
constexpr uint32_t kCloseAfterMs = 1500;        // the card's open-file limit: given back soon after a burst
constexpr float kTile = 533.33333f;             // upstream core::coords::TILE_SIZE
constexpr float kMaxReach = 60.0f;              // the converter clamps an effect's reach to it

struct SpriteVert {
    float u, v;
    uint32_t color;
    float x, y, z;
};
static_assert(sizeof(SpriteVert) == 24, "sprite vertex");
constexpr int kSpriteType = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
constexpr int kMeshType = GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
struct LineVert {
    uint32_t color;
    float x, y, z;
};
static_assert(sizeof(LineVert) == 16, "line vertex");
constexpr int kLineType = GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;

bool finiteBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return (u & 0x7F800000u) != 0x7F800000u;
}

// 3x4 column-major: l[0..2] x axis, l[3..5] y, l[6..8] z, t translation (BoneMatrix layout).
// The portable one (psp_particle_sim.hpp: identity, rotate, compose); `apply` of a
// float[3] stays here.
using M34 = ptcsim::M34;
using ptcsim::compose;
using ptcsim::identity;
using ptcsim::rotate;

glm::vec3 apply(const M34& m, const float p[3]) {
    return glm::vec3(m.l[0] * p[0] + m.l[3] * p[1] + m.l[6] * p[2] + m.t[0],
                     m.l[1] * p[0] + m.l[4] * p[1] + m.l[7] * p[2] + m.t[1],
                     m.l[2] * p[0] + m.l[5] * p[1] + m.l[8] * p[2] + m.t[2]);
}

// Frame position of a curve / track at tMs: frame index + fraction.
struct FramePos {
    int k = 0;
    float f = 0.0f;
};

FramePos framePos(uint16_t frames, float pos) {
    FramePos fp;
    if (frames <= 1) return fp;
    const float maxPos = static_cast<float>(frames - 1);
    if (!(pos > 0.0f)) return fp;
    if (pos >= maxPos) {
        fp.k = frames - 2;
        fp.f = 1.0f;
        return fp;
    }
    fp.k = static_cast<int>(pos);
    fp.f = pos - static_cast<float>(fp.k);
    return fp;
}

float curve(const uint8_t* base, const SpellFxEmitter& e, int c, float pos) {
    const auto* v = reinterpret_cast<const float*>(base + e.curveOffset[c]);
    const uint16_t n = e.curveFrames[c];
    if (n <= 1) return v[0];
    const FramePos fp = framePos(n, pos);
    return v[fp.k] + (v[fp.k + 1] - v[fp.k]) * fp.f;
}

void trackMatrix(const uint8_t* base, const SpellFxTrack& t, float pos, M34& out) {
    const FramePos fp = framePos(t.frames, pos);
    const int k1 = t.frames > 1 ? fp.k + 1 : fp.k;
    if (t.flags & kFxTrackMatrix) {
        const auto* keys = reinterpret_cast<const SpellFxMatrixKey*>(base + t.offset);
        const SpellFxMatrixKey& a = keys[fp.k];
        const SpellFxMatrixKey& b = keys[k1];
        for (int i = 0; i < 9; ++i) out.l[i] = (a.m[i] + (b.m[i] - a.m[i]) * fp.f) * t.scaleScale;
        for (int i = 0; i < 3; ++i) out.t[i] = (a.t[i] + (b.t[i] - a.t[i]) * fp.f) * t.translationScale;
        return;
    }
    const auto* keys = reinterpret_cast<const SpellFxKey*>(base + t.offset);
    const SpellFxKey& a = keys[fp.k];
    const SpellFxKey& b = keys[k1];
    // nlerp (the converter keeps consecutive rotations on one hemisphere).
    float q[4];
    float len = 0.0f;
    for (int i = 0; i < 4; ++i) {
        q[i] = a.q[i] + (b.q[i] - a.q[i]) * fp.f;
        len += q[i] * q[i];
    }
    const float inv = len > 1e-6f ? 1.0f / std::sqrt(len) : 0.0f;
    const float x = q[0] * inv, y = q[1] * inv, z = q[2] * inv, w = len > 1e-6f ? q[3] * inv : 1.0f;
    float s[3];
    for (int i = 0; i < 3; ++i) {
        s[i] = (a.s[i] + (b.s[i] - a.s[i]) * fp.f) * t.scaleScale;
        out.t[i] = (a.t[i] + (b.t[i] - a.t[i]) * fp.f) * t.translationScale;
    }
    // Rotation matrix columns (glm::mat3_cast), times the scale.
    out.l[0] = (1.0f - 2.0f * (y * y + z * z)) * s[0];
    out.l[1] = (2.0f * (x * y + w * z)) * s[0];
    out.l[2] = (2.0f * (x * z - w * y)) * s[0];
    out.l[3] = (2.0f * (x * y - w * z)) * s[1];
    out.l[4] = (1.0f - 2.0f * (x * x + z * z)) * s[1];
    out.l[5] = (2.0f * (y * z + w * x)) * s[1];
    out.l[6] = (2.0f * (x * z + w * y)) * s[2];
    out.l[7] = (2.0f * (y * z - w * x)) * s[2];
    out.l[8] = (1.0f - 2.0f * (x * x + y * y)) * s[2];
}

// upstream computeBoneMatrices, kM2BoneSphericalBillboard: the bone's rotation faces the camera
// (M2 cards face +X), its scale and its pivot's place kept. camModel: the camera in model space.
void billboard(M34& m, const float pivot[3], const glm::vec3& camModel) {
    const glm::vec3 pp = apply(m, pivot);
    glm::vec3 toCam = camModel - pp;
    const float len2 = glm::dot(toCam, toCam);
    if (len2 <= 1e-8f) return;
    toCam *= 1.0f / std::sqrt(len2);
    const glm::vec3 up = std::fabs(toCam.z) > 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    const glm::vec3 right = glm::normalize(glm::cross(up, toCam));
    const glm::vec3 trueUp = glm::cross(toCam, right);
    const float sx = std::sqrt(m.l[0] * m.l[0] + m.l[1] * m.l[1] + m.l[2] * m.l[2]);
    const float sy = std::sqrt(m.l[3] * m.l[3] + m.l[4] * m.l[4] + m.l[5] * m.l[5]);
    const float sz = std::sqrt(m.l[6] * m.l[6] + m.l[7] * m.l[7] + m.l[8] * m.l[8]);
    const glm::vec3 c0 = toCam * sx, c1 = right * sy, c2 = trueUp * sz;
    m.l[0] = c0.x; m.l[1] = c0.y; m.l[2] = c0.z;
    m.l[3] = c1.x; m.l[4] = c1.y; m.l[5] = c1.z;
    m.l[6] = c2.x; m.l[7] = c2.y; m.l[8] = c2.z;
    const glm::vec3 t = pp - (c0 * pivot[0] + c1 * pivot[1] + c2 * pivot[2]);
    m.t[0] = t.x;
    m.t[1] = t.y;
    m.t[2] = t.z;
}

void setBlend(uint8_t blend) {
    if (blend == kFxBlendAdd)
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFFu);
    else
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

uint32_t packColor(float r, float g, float b, float a) {
    auto q = [](float v) {
        const float c = v * 255.0f + 0.5f;
        return static_cast<uint32_t>(c <= 0.0f ? 0.0f : c >= 255.0f ? 255.0f : c);
    };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

// The animation position of an instance's curves / tracks: sequence 0 looping (upstream).
float curvePos(const SpellFxHeader& h, float timeMs) {
    const float tMs = std::fmod(timeMs, static_cast<float>(h.periodMs));
    return tMs / static_cast<float>(h.periodMs) * static_cast<float>(h.frames - 1);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The older rules on the engine's pool (FxLegacyRules, psp_particles_l3.hpp): upstream's rules (wowee.cfg
// ptclevel3=0: every instance) and the port's first ones (ptclevel3=1: the instances whose blob has no level-3 section).
// The level-3 rules themselves are psp_particles_l3.cpp (clean-room).

namespace {

using Particle = ptcsim::Particle;
using Edge = ParticleEngine::Edge;
using Lane = ParticleEngine::Lane;

class LegacyRules final : public FxLegacyRules {
public:
    bool init(int capacity) {
        order_ = static_cast<uint16_t*>(std::malloc(sizeof(uint16_t) * static_cast<size_t>(std::max(1, capacity))));
        return order_ != nullptr;
    }
    void shutdown() {
        std::free(order_);
        order_ = nullptr;
    }
    uint32_t bytes(int capacity) const { return static_cast<uint32_t>(sizeof(uint16_t) * static_cast<size_t>(capacity)); }

    void step(ParticleEngine& e, FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt, bool level3Rules,
              FxFrameStats& st) override;
    void drawBefore(ParticleEngine& e, FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam, int maxPx,
                    bool level3Rules, FxFrameStats& st) override;
    void drawAfter(ParticleEngine& e, FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam,
                   bool level3Rules, FxFrameStats& st) override;

private:
    // The instances these rules run: all of them (level-3 rules off) or those without the level-3 section.
    static bool mine(const FxInstance& in, bool level3Rules) { return !(level3Rules && in.fx->level3); }
    void stepOnce(ParticleEngine& e, FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt,
                  bool level3Rules, FxFrameStats& st);
    void emit(ParticleEngine& e, FxInstance& in, float dt, int* live, int& userLive, int share, bool build115,
              FxFrameStats& st);
    void ribbons(ParticleEngine& e, FxInstance& in, float dt);
    void drawMesh(FxInstance& in, const TerrainCamera& cam, FxFrameStats& st);
    void drawSprites(ParticleEngine& e, FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam,
                     int maxPx, bool level3Rules, FxFrameStats& st);
    void drawRibbons(ParticleEngine& e, FxInstance& in, const TerrainCamera& cam, FxFrameStats& st);
    Particle* alloc(ParticleEngine& e, uint8_t owner, uint8_t emitter) {
        const int slot = e.popSlot();
        if (slot < 0) return nullptr;
        Particle* p = &e.pool()[slot];
        p->owner = owner;
        p->emitter = emitter;
        p->aux = 0;
        p->life = 0.0f;
        return p;
    }
    uint32_t rnd() {  // xorshift32
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }
    float rnd01() { return static_cast<float>(rnd() >> 8) * (1.0f / 16777216.0f); }

    uint16_t* order_ = nullptr;  // draw: particle indices by (instance, emitter)
    uint32_t rng_ = 0x2545F491u;
};

LegacyRules g_legacy;
FxGuOut g_geOut(kListReserveFx);

// upstream M2Renderer::emitParticles for one instance (+ M2Renderer::update's smoke for kPtcSmoke).
// build115: wowee.cfg ptclevel3=1 on a blob without the level-3 section - the emitter rules set from the format
// description.
void LegacyRules::emit(ParticleEngine& eng, FxInstance& in, float dt, int* live, int& userLive, int share, bool build115,
                       FxFrameStats& st) {
    const FxView& L = *in.fx;
    const SpellFxHeader& h = *L.h;
    const float pos = curvePos(h, in.timeMs);
    const bool firefly = (L.cls & kPtcFirefly) != 0, flame = (L.cls & kPtcFlame) != 0;
    const int cap = std::max(1, in.cap);
    for (int ei = 0; ei < h.emitterCount; ++ei) {
        const SpellFxEmitter& e = L.emitters[ei];
        float rate = curve(L.base, e, kFxCurveRate, pos);
        const float life = curve(L.base, e, kFxCurveLife, pos);
        rate *= in.rate;  // upstream: x particleDensity_ before the flame floor (PSP: x the distance LOD too)
        // upstream's flame floor (15 / lifespan a second) only under upstream's rules.
        if (!build115 && flame && rate > 0.0f && life > 0.0f) rate = std::max(rate, 15.0f / std::max(life, 0.1f));
        if (rate <= 0.0f || life <= 0.0f) continue;
        // PSP: the steady population (rate x lifespan) is held to the emitter's share.
        const float capRate = static_cast<float>(cap) / std::max(life, 0.05f);
        if (rate > capRate) rate = capRate;
        float& acc = in.acc[ei];
        acc += rate * dt;
        M34 world;
        bool haveBone = false;
        while (acc >= 1.0f && live[ei] < cap && userLive < share) {
            acc -= 1.0f;
            Particle* p = alloc(eng, in.owner, static_cast<uint8_t>(ei));
            if (!p) break;
            if (!haveBone) {
                M34 bone;
                if (e.track != kFxNone) trackMatrix(L.base, L.tracks[e.track], pos, bone);
                else identity(bone);
                compose(in.axes, in.pos, bone, world);  // upstream: modelMatrix * boneXform
                haveBone = true;
            }
            p->maxLife = life;
            p->pos = apply(world, e.position);
            const float speed = curve(L.base, e, kFxCurveSpeed, pos);
            const float vr = curve(L.base, e, kFxCurveVRange, pos);
            const float hr = curve(L.base, e, kFxCurveHRange, pos);
            glm::vec3 dir;
            if (build115) {
                // verticalRange is the cone's half angle from the emitter's up (z), horizontalRange the
                // azimuth around it (the format's documented plane emitter) - a torch with vr 0.09, hr 2 pi rises
                // nearly straight. (upstream took both as lengths: (+-hr, +-hr, 1 +- vr) - with hr 2 pi the flames
                // flew sideways.)
                const float polar = (rnd01() * 2.0f - 1.0f) * vr;
                const float az = (rnd01() * 2.0f - 1.0f) * hr;
                const float sp = std::sin(polar);
                dir = glm::vec3(sp * std::cos(az), sp * std::sin(az), std::cos(polar));
            } else {
                dir = glm::vec3((rnd01() * 2.0f - 1.0f) * hr, (rnd01() * 2.0f - 1.0f) * hr, 1.0f + (rnd01() * 2.0f - 1.0f) * vr);
                const float l2 = glm::dot(dir, dir);
                if (l2 > 1e-6f) dir *= 1.0f / std::sqrt(l2);
            }
            p->vel = rotate(world, dir) * speed;  // upstream: mat3(model * bone) * dir * speed
            // a weapon's glow keeps its still particles where they were emitted (the trail of a
            // swing) - the drift is upstream's doodad rule (upstream draws no particles of a weapon's effect).
            // A still particle stays still (a lantern's glow) - the drift for fireflies only.
            if (std::fabs(speed) < 0.01f && !(L.cls & kPtcItem) && (!build115 || firefly)) {
                // upstream: a drift instead of a pile-up (fireflies drift their own way).
                p->vel = firefly ? rotate(world, glm::vec3((rnd01() * 2.0f - 1.0f) * 0.6f, (rnd01() * 2.0f - 1.0f) * 0.6f,
                                                           (rnd01() * 2.0f - 1.0f) * 0.3f))
                                 : rotate(world, glm::vec3(rnd01() * 2.0f - 1.0f, rnd01() * 2.0f - 1.0f, -rnd01() * 0.5f));
            }
            const int total = static_cast<int>(e.cols) * e.rows;
            if ((e.flags & kFxEmitTiled) && (e.flags & kFxEmitRandomTile) && total > 1)
                p->aux = static_cast<uint16_t>(std::min(total - 1, static_cast<int>(rnd01() * total)));
            ++live[ei];
            ++userLive;
            ++st.emitted;
        }
        if (acc > 2.0f) acc = 0.0f;  // upstream: no burst after a stall
    }
    // upstream M2Renderer::update: an isSmoke model puffs from its origin every 1/48 s (one per
    // frame at most), 1 in 8 a spark. PSP: at most its share alive (rate cut to share / 5.5 s).
    if (L.cls & kPtcSmoke) {
        float& acc = in.acc[kFxSmokeEmitter];
        const float rate = std::min(1.0f / kSmokeInterval, static_cast<float>(cap) / 5.5f) * in.rate;
        acc += rate * dt;
        if (acc >= 1.0f && live[kFxSmokeEmitter] + live[kFxSparkEmitter] < cap && userLive < share) {
            acc -= 1.0f;
            const bool spark = (rnd() % 8u) == 0u;  // upstream SPARK_PROBABILITY_DENOM
            if (Particle* p = alloc(eng, in.owner, static_cast<uint8_t>(spark ? kFxSparkEmitter : kFxSmokeEmitter))) {
                p->pos = in.pos + glm::vec3(rnd01() * 0.8f - 0.4f, rnd01() * 0.8f - 0.4f, 0.0f);
                const float vx = rnd01() * 0.6f - 0.3f, vy = rnd01() * 0.6f - 0.3f, vz = 3.0f + rnd01() * 2.0f;
                if (spark) {
                    p->vel = glm::vec3(vx * 2.0f, vy * 2.0f, vz * 1.5f);
                    p->maxLife = 0.8f + static_cast<float>(rnd() % 100u) / 100.0f * 1.2f;
                } else {
                    p->vel = glm::vec3(vx, vy, vz);
                    p->maxLife = 4.0f + rnd01() * 3.0f;
                }
                ++live[spark ? kFxSparkEmitter : kFxSmokeEmitter];
                ++userLive;
                ++st.emitted;
            }
        }
        if (acc > 2.0f) acc = 0.0f;
    }
}

// upstream M2Renderer::updateRibbons for one instance (the spine kept in the edge's `bottom`).
void LegacyRules::ribbons(ParticleEngine& eng, FxInstance& in, float dt) {
    const FxView& L = *in.fx;
    const float pos = curvePos(*L.h, in.timeMs);
    for (int r = 0; r < L.ribbonCount && r < kFxMaxRibbons; ++r) {
        Lane* lane = eng.laneAt(in.lanes[r]);
        if (!lane || !lane->used) continue;
        const SpellFxRibbon& R = L.ribbons[r];
        M34 bone, world;
        if (R.track != kFxNone) trackMatrix(L.base, L.tracks[R.track], pos, bone);
        else identity(bone);
        compose(in.axes, in.pos, bone, world);
        const glm::vec3 spine = apply(world, R.position);
        if (!finiteBits(spine.x) || !finiteBits(spine.y) || !finiteBits(spine.z)) continue;  // upstream: NaN skipped
        Lane& ln = *lane;
        Edge* edges = ln.ring;
        const float fall = R.gravity * dt * dt * 0.5f;  // upstream: per frame
        for (int k = 0; k < ln.count; ++k) {
            Edge& e = edges[(ln.first + k) % kRibbonEdges];
            e.age += dt;
            e.bottom.z -= fall;
        }
        while (ln.count && edges[ln.first].age >= R.edgeLifetime) {
            ln.first = static_cast<int16_t>((ln.first + 1) % kRibbonEdges);
            --ln.count;
        }
        if (R.visibility > 0.5f) {
            ln.acc += R.edgesPerSecond * dt;
            while (ln.acc >= 1.0f) {
                ln.acc -= 1.0f;
                if (ln.count >= kRibbonEdges) {  // PSP: 64 (upstream caps the trail at 128)
                    ln.first = static_cast<int16_t>((ln.first + 1) % kRibbonEdges);
                    --ln.count;
                }
                edges[(ln.first + ln.count) % kRibbonEdges] = Edge{spine, 0.0f, glm::vec3(0.0f), 0u};
                ++ln.count;
            }
        } else {
            ln.acc = 0.0f;
        }
    }
}

void LegacyRules::step(ParticleEngine& eng, FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt,
                       bool level3Rules, FxFrameStats& st) {
    // With the level-3 rules on and no instance of ours there is nothing to do (the level-3 step freed the
    // particles of the owners that are gone); with them off every instance is ours (and n = 0 still frees).
    bool any = !level3Rules;
    for (int i = 0; i < n && !any; ++i) any = mine(in[i], level3Rules);
    if (!any) return;
    dt = std::clamp(dt, 0.0f, 0.25f);
    // ptclevel3=1: a frame longer than 0.1 s steps in equal 0.1-s parts, births included (as the
    // level-3 rules do).
    if (level3Rules && dt > 0.1f) {
        const int parts = static_cast<int>(std::ceil(dt / 0.1f));
        for (int k = 0; k < parts; ++k) stepOnce(eng, in, n, lo, hi, share, dt / static_cast<float>(parts), level3Rules, st);
        return;
    }
    stepOnce(eng, in, n, lo, hi, share, dt, level3Rules, st);
}

void LegacyRules::stepOnce(ParticleEngine& eng, FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt,
                           bool level3Rules, FxFrameStats& st) {
    n = std::min(n, 64);
    Particle* pool = eng.pool();
    const int capacity = eng.capacity();
    int8_t slotOf[256];
    std::memset(slotOf, -1, sizeof(slotOf));
    for (int i = 0; i < n; ++i)
        if (in[i].owner >= lo && in[i].owner < hi) slotOf[in[i].owner] = static_cast<int8_t>(i);
    // Live per instance / emitter; particles of owners that are gone (upstream removeInstance) go.
    int live[64][kFxEmitterSlots];
    std::memset(live, 0, sizeof(live));
    int userLive = 0;
    for (int i = 0; i < capacity; ++i) {
        const Particle& p = pool[i];
        if (p.owner == 0xFF || p.owner < lo || p.owner >= hi) continue;
        const int s = slotOf[p.owner];
        if (s < 0) {
            eng.freeSlot(i);
            continue;
        }
        ++live[s][p.emitter];
        ++userLive;
    }
    // Gravity per instance / emitter (upstream updateParticles: 0 -> 4 / 1.5 by the speed; not fireflies).
    // ptclevel3=1: the model's gravity as it is, 0 = no fall (beer bubbles rise from the Brewfest mugs,
    // flames from the torches; upstream's 4 yd/s^2 made them stream down).
    float grav[64][kFxMaxEmitters];
    std::memset(grav, 0, sizeof(grav));
    for (int i = 0; i < n; ++i) {
        if (!mine(in[i], level3Rules)) continue;
        const FxView& L = *in[i].fx;
        const float pos = curvePos(*L.h, in[i].timeMs);
        for (int e = 0; e < L.h->emitterCount && e < kFxMaxEmitters; ++e) {
            float g = curve(L.base, L.emitters[e], kFxCurveGravity, pos);
            // a weapon's glow falls by its own gravity only (see emit).
            if (!level3Rules && g == 0.0f && !(L.cls & (kPtcFirefly | kPtcItem)))
                g = std::fabs(curve(L.base, L.emitters[e], kFxCurveSpeed, pos)) > 0.1f ? 4.0f : 1.5f;
            grav[i][e] = g;
        }
    }
    // Emission, nearest first (the list order): the share runs out at the far end.
    for (int i = 0; i < n; ++i)
        if (mine(in[i], level3Rules)) emit(eng, in[i], dt, live[i], userLive, share, level3Rules, st);
    // upstream updateParticles: age, gravity on world z, move; the smoke: up, slowed x0.98 a frame, drifting.
    for (int i = 0; i < capacity; ++i) {
        Particle& p = pool[i];
        if (p.owner == 0xFF || p.owner < lo || p.owner >= hi) continue;
        const int s = slotOf[p.owner];
        if (s >= 0 && !mine(in[s], level3Rules)) continue;  // the level-3 rules aged it (psp_particles_l3.cpp)
        p.life += dt;
        if (p.life >= p.maxLife || s < 0) {
            eng.freeSlot(i);
            continue;
        }
        if (p.emitter < kFxMaxEmitters) {
            p.vel.z -= grav[s][p.emitter] * dt;
            p.pos += p.vel * dt;
        } else {
            p.pos += p.vel * dt;
            p.vel.z *= 0.98f;  // upstream SMOKE_Z_VEL_DAMPING (per frame)
            p.vel.x += (rnd01() * 0.4f - 0.2f) * dt;
            p.vel.y += (rnd01() * 0.4f - 0.2f) * dt;
        }
    }
    for (int i = 0; i < n; ++i)
        if (in[i].fx->ribbonCount && mine(in[i], level3Rules)) ribbons(eng, in[i], dt);
}

// A spell effect's (or a spell-effect doodad's) batches: one strip per group on its bone's matrix.
void LegacyRules::drawMesh(FxInstance& in, const TerrainCamera& cam, FxFrameStats& st) {
    const FxView& L = *in.fx;
    const SpellFxHeader& h = *L.h;
    const float pos = curvePos(h, in.timeMs);
    const glm::vec3 rel = in.pos - cam.pos;
    // The camera in model space (billboards): axes^-1 * (camera - origin).
    glm::vec3 camModel = cam.pos - in.pos;
    {
        const glm::mat3 A(in.axes[0], in.axes[1], in.axes[2], in.axes[3], in.axes[4], in.axes[5], in.axes[6], in.axes[7],
                          in.axes[8]);
        const float det = glm::determinant(A);
        if (std::fabs(det) > 1e-9f) camModel = glm::inverse(A) * camModel;
    }
    for (uint32_t gi = 0; gi < h.groupCount; ++gi) {
        const SpellFxGroup& g = L.groups[gi];
        if (pp::displayListFree() < kListReserveFx + 1024u) {
            ++st.listFull;
            return;
        }
        if (g.texture != kFxNone) {
            const pp::Texture* t = in.tex ? in.tex(in.texUser, g.texture) : nullptr;
            if (!t) continue;  // upstream hides a batch whose texture did not load
            pp::textureBind(t);
        } else {
            pp::textureBind(nullptr);
        }
        sceGuTexWrap((g.flags & kFxWrapU) ? GU_REPEAT : GU_CLAMP, (g.flags & kFxWrapV) ? GU_REPEAT : GU_CLAMP);
        float uo = 0.0f, vo = 0.0f;
        if (g.uvOffset) {
            const auto* uv = reinterpret_cast<const float*>(L.base + g.uvOffset);
            if (g.uvFrames > 1) {
                const FramePos fp = framePos(g.uvFrames, pos);
                uo = uv[fp.k * 2] + (uv[fp.k * 2 + 2] - uv[fp.k * 2]) * fp.f;
                vo = uv[fp.k * 2 + 1] + (uv[fp.k * 2 + 3] - uv[fp.k * 2 + 1]) * fp.f;
            } else {
                uo = uv[0];
                vo = uv[1];
            }
        }
        sceGuTexScale(g.uvScale[0], g.uvScale[1]);
        sceGuTexOffset(g.uvMin[0] + uo, g.uvMin[1] + vo);
        setBlend(g.blend);
        sceGuColor(g.color);
        M34 b;
        if (g.track != kFxNone) {
            const SpellFxTrack& tr = L.tracks[g.track];
            trackMatrix(L.base, tr, pos, b);
            if (tr.flags & kFxTrackBillboard) billboard(b, tr.pivot, camModel);
        } else {
            identity(b);
        }
        // model = T(instance - camera) * axes * bone * T(center) * S(half)
        M34 w;
        compose(in.axes, rel, b, w);
        ScePspFMatrix4 m;
        m.x = {w.l[0] * h.half[0], w.l[1] * h.half[0], w.l[2] * h.half[0], 0.0f};
        m.y = {w.l[3] * h.half[1], w.l[4] * h.half[1], w.l[5] * h.half[1], 0.0f};
        m.z = {w.l[6] * h.half[2], w.l[7] * h.half[2], w.l[8] * h.half[2], 0.0f};
        const glm::vec3 c = apply(w, h.center);
        m.w = {c.x, c.y, c.z, 1.0f};
        sceGumLoadMatrix(&m);
        prof::count(prof::kDraws, 1);
        prof::count(prof::kVertices, g.vertexCount);
        prof::count(prof::kTriangles, g.vertexCount - 2u);
        sceGumDrawArray(GU_TRIANGLE_STRIP, kMeshType, g.vertexCount, nullptr, L.verts + g.vertexStart);
        ++st.meshDraws;
        ++st.draws;
        in.drawn = true;
    }
}

// upstream renderM2Particles (+ renderSmokeParticles): camera-facing GE rectangles, one draw per
// instance / emitter.
void LegacyRules::drawSprites(ParticleEngine& eng, FxInstance* in, int n, uint8_t lo, uint8_t hi,
                              const TerrainCamera& cam, int maxPx, bool level3Rules, FxFrameStats& st) {
    const Particle* pool = eng.pool();
    const int capacity = eng.capacity();
    // Counting sort of the user's live particles by (instance, emitter) - the instances these rules draw.
    int8_t slotOf[256];
    std::memset(slotOf, -1, sizeof(slotOf));
    for (int i = 0; i < n; ++i)
        if (in[i].owner >= lo && in[i].owner < hi && mine(in[i], level3Rules)) slotOf[in[i].owner] = static_cast<int8_t>(i);
    constexpr int kBuckets = 64 * kFxEmitterSlots;
    uint16_t start[kBuckets + 1];
    std::memset(start, 0, sizeof(start));
    for (int i = 0; i < capacity; ++i) {
        const Particle& p = pool[i];
        if (p.owner == 0xFF || p.owner < lo || p.owner >= hi || slotOf[p.owner] < 0) continue;
        ++start[slotOf[p.owner] * kFxEmitterSlots + p.emitter + 1];
    }
    const int buckets = n * kFxEmitterSlots;
    for (int b = 0; b < buckets; ++b) start[b + 1] = static_cast<uint16_t>(start[b + 1] + start[b]);
    if (start[buckets] == 0) return;
    uint16_t fill[kBuckets];
    std::memcpy(fill, start, sizeof(uint16_t) * buckets);
    for (int i = 0; i < capacity; ++i) {
        const Particle& p = pool[i];
        if (p.owner == 0xFF || p.owner < lo || p.owner >= hi || slotOf[p.owner] < 0) continue;
        order_[fill[slotOf[p.owner] * kFxEmitterSlots + p.emitter]++] = static_cast<uint16_t>(i);
    }

    sceGumLoadIdentity();
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuEnable(GU_ALPHA_TEST);
    const glm::vec3 f = glm::normalize(cam.forward);
    const glm::vec3 r = glm::normalize(glm::cross(f, cam.up));
    const glm::vec3 u = glm::cross(r, f);
    const float focal = (static_cast<float>(pp::kScreenH) * 0.5f) / std::tan(glm::radians(cam.fovyDeg) * 0.5f);
    const float maxHalf = 0.5f * static_cast<float>(maxPx) / focal;  // x depth
    const float minHalf = 0.5f / focal;                              // 1 px (upstream clamps to 1)
    const float smokeMaxHalf = 0.5f * kSmokeMaxShare * static_cast<float>(pp::kScreenH) / focal;
    for (int b = 0; b < buckets; ++b) {
        const int count = start[b + 1] - start[b];
        if (!count) continue;
        const int i = b / kFxEmitterSlots, ei = b % kFxEmitterSlots;
        FxInstance& inst = in[i];
        const FxView& L = *inst.fx;
        const uint32_t need = static_cast<uint32_t>(count) * 2u * sizeof(SpriteVert) + 256u;
        if (pp::displayListFree() < kListReserveFx + need) {
            ++st.listFull;
            continue;
        }
        const bool smoke = ei >= kFxMaxEmitters;
        const SpellFxEmitter* e = smoke ? nullptr : &L.emitters[ei];
        if (smoke) {
            pp::textureBind(fxDotTexture());
            setBlend(kFxBlendAlpha);  // upstream smokePipeline_: blendAlpha
            sceGuAlphaFunc(GU_GREATER, 0x03, 0xFF);
        } else {
            if (e->texture != kFxNone) {
                const pp::Texture* t = inst.tex ? inst.tex(inst.texUser, e->texture) : nullptr;
                if (!t) continue;
                pp::textureBind(t);
            } else {
                pp::textureBind(nullptr);
            }
            setBlend(e->blend);
            // upstream discards near-black texels of an alpha-key emitter; everything else skips empty ones.
            sceGuAlphaFunc(GU_GREATER, e->blend == kFxBlendAlphaKey ? 0x0C : 0x03, 0xFF);
        }
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        const bool spell = (L.cls & kPtcSpell) != 0, firefly = (L.cls & kPtcFirefly) != 0, flame = (L.cls & kPtcFlame) != 0;
        const bool item = (L.cls & kPtcItem) != 0;  // a weapon's glow - as authored
        const bool generic = !spell && !firefly && !flame && !item;
        const bool additive = e && e->blend == kFxBlendAdd;
        const int cols = (e && e->cols) ? e->cols : 1, rows = (e && e->rows) ? e->rows : 1, total = cols * rows;
        const bool tiled = e && (e->flags & kFxEmitTiled) && total > 1;
        // upstream: the tile frame from the instance's animation time (animTime wraps at the sequence duration -
        // 3333 ms for a particle-only model; the effect's period here, as the spell effects did).
        const float animSec = std::fmod(inst.timeMs, static_cast<float>(L.h->periodMs)) * 0.001f;
        const int animTile = tiled ? static_cast<int>(std::floor(animSec * total)) % total : 0;
        auto* v = static_cast<SpriteVert*>(sceGuGetMemory(static_cast<int>(count * 2 * sizeof(SpriteVert))));
        int out = 0;
        for (int k = start[b]; k < start[b + 1]; ++k) {
            const Particle& p = pool[order_[k]];
            const glm::vec3 c = p.pos - cam.pos;
            const float depth = glm::dot(c, f);
            if (depth < cam.nearZ + 0.05f) continue;  // a rectangle crossing the near plane is dropped whole
            const float ratio = std::clamp(p.life / std::max(p.maxLife, 0.001f), 0.0f, 1.0f);
            float cr, cg, cb, ca, half;
            int tile = 0;
            if (!smoke) {
                const float x = ratio * (kFxLifeKeys - 1);
                const int kk = std::min(static_cast<int>(x), kFxLifeKeys - 2);
                const float fr = x - static_cast<float>(kk);
                const uint32_t c0 = e->color[kk], c1 = e->color[kk + 1];
                auto ch = [&](int s) {
                    const float a0 = static_cast<float>((c0 >> s) & 0xFFu), a1 = static_cast<float>((c1 >> s) & 0xFFu);
                    return (a0 + (a1 - a0) * fr) * (1.0f / 255.0f);
                };
                cr = ch(0);
                cg = ch(8);
                cb = ch(16);
                ca = ch(24);
                const float raw = e->scale[kk] + (e->scale[kk + 1] - e->scale[kk]) * fr;
                // upstream renderM2Particles' per-model rules.
                if (generic) {
                    cr += (1.0f - cr) * 0.7f;
                    cg += (1.0f - cg) * 0.7f;
                    cb += (1.0f - cb) * 0.7f;
                    if (raw > 2.0f) ca *= 0.02f;
                    if (additive) ca *= 0.05f;
                }
                if (flame) {
                    cr = std::max(cr, 0.50f);
                    cg = std::max(cg, 0.26f);
                    cb = std::max(cb, 0.09f);
                    ca = std::max(ca, 0.30f);
                }
                float scale = raw;
                if (spell) {
                    scale = std::max(raw * 1.5f, 0.15f);
                } else if (item) {
                    scale = std::fabs(raw);  // the authored size
                } else if (!firefly) {
                    scale = std::min(raw, 1.5f);
                    if (flame) scale = std::max(scale, 0.15f);
                }
                // upstream m2_particle.frag: rgb x alpha out, SRC_ALPHA blending (the texture's alpha once here).
                // Spell effects keep the straight colour unless wowee.cfg spellfxpremul=1.
                if (inst.premultiply) {
                    cr *= ca;
                    cg *= ca;
                    cb *= ca;
                }
                half = std::clamp(0.5f * scale * kSpriteYardsPerScale, minHalf * depth, maxHalf * depth);
                tile = p.aux & 0xFFu;
                if (tiled) tile = (tile + animTile) % total;
            } else if (ei == kFxSmokeEmitter) {
                // upstream m2_smoke.frag: grey 0.5, 0.4 x fade in x fade out; size 1 -> 3.5 over the life.
                const float fadeIn = std::clamp(ratio / 0.2f, 0.0f, 1.0f), fadeOut = 1.0f - std::clamp((ratio - 0.6f) / 0.4f, 0.0f, 1.0f);
                cr = cg = cb = 0.5f;
                ca = 0.4f * fadeIn * fadeIn * (3.0f - 2.0f * fadeIn) * fadeOut * fadeOut * (3.0f - 2.0f * fadeOut);
                half = std::clamp(0.5f * (1.0f + ratio * 2.5f) * kSmokeYards, minHalf * depth, smokeMaxHalf * depth);
            } else {
                // a spark: orange -> red, fading.
                cr = 1.0f;
                cg = 0.6f - 0.4f * ratio;
                cb = 0.1f - 0.1f * ratio;
                ca = 1.0f - ratio;
                cr *= ca;
                cg *= ca;
                cb *= ca;
                half = std::clamp(0.5f * (1.0f + ratio * 2.5f) * kSparkYards, minHalf * depth, smokeMaxHalf * depth);
            }
            if (ca <= 0.004f) continue;
            const uint32_t color = packColor(cr, cg, cb, ca);
            const float u0 = static_cast<float>(tile % cols) / cols, v0 = static_cast<float>(tile / cols) / rows;
            const float u1 = u0 + 1.0f / cols, v1 = v0 + 1.0f / rows;
            const glm::vec3 a = c - r * half + u * half, bb = c + r * half - u * half;
            v[out * 2] = {u0, v0, color, a.x, a.y, a.z};
            v[out * 2 + 1] = {u1, v1, color, bb.x, bb.y, bb.z};  // the second vertex's colour is the sprite's
            ++out;
        }
        if (!out) continue;
        prof::count(prof::kDraws, 1);
        prof::count(prof::kVertices, static_cast<uint32_t>(out * 2));
        sceGumDrawArray(GU_SPRITES, kSpriteType, out * 2, nullptr, v);
        st.sprites += static_cast<uint32_t>(out);
        ++st.draws;
        inst.drawn = true;
    }
    sceGuDisable(GU_ALPHA_TEST);
}

// upstream renderM2Ribbons: a strip of 2 vertices per edge, widened across the trail and the sight line.
void LegacyRules::drawRibbons(ParticleEngine& eng, FxInstance& in, const TerrainCamera& cam, FxFrameStats& st) {
    const FxView& L = *in.fx;
    for (int r = 0; r < L.ribbonCount && r < kFxMaxRibbons; ++r) {
        const Lane* lane = eng.laneAt(in.lanes[r]);
        if (!lane || !lane->used) continue;
        const Lane& ln = *lane;
        if (ln.count < 2) continue;
        const SpellFxRibbon& R = L.ribbons[r];
        const uint32_t need = static_cast<uint32_t>(ln.count) * 2u * sizeof(SpriteVert) + 256u;
        if (pp::displayListFree() < kListReserveFx + need) {
            ++st.listFull;
            continue;
        }
        if (R.texture != kFxNone) {
            const pp::Texture* t = in.tex ? in.tex(in.texUser, R.texture) : nullptr;
            if (!t) continue;  // upstream: a ribbon without its texture set is not drawn
            pp::textureBind(t);
        } else {
            pp::textureBind(nullptr);
        }
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        setBlend(R.blend);
        const Edge* edges = ln.ring;
        const float cr = static_cast<float>(R.color & 0xFFu) / 255.0f, cg = static_cast<float>((R.color >> 8) & 0xFFu) / 255.0f,
                    cb = static_cast<float>((R.color >> 16) & 0xFFu) / 255.0f, ca = static_cast<float>(R.color >> 24) / 255.0f;
        const float above = R.heightAbove * in.scale, below = R.heightBelow * in.scale;  // upstream: x inst.scale
        auto* v = static_cast<SpriteVert*>(sceGuGetMemory(static_cast<int>(ln.count * 2 * sizeof(SpriteVert))));
        const int n = ln.count;
        for (int k = 0; k < n; ++k) {
            const Edge& e = edges[(ln.first + k) % kRibbonEdges];
            const glm::vec3 p = e.bottom;
            const float t = R.edgeLifetime > 0.0f ? 1.0f - e.age / R.edgeLifetime : 1.0f;
            const float a = std::clamp(ca * t, 0.0f, 1.0f);
            const float uu = static_cast<float>(k) / static_cast<float>(n - 1);
            const Edge& ep = edges[(ln.first + (k > 0 ? k - 1 : k)) % kRibbonEdges];
            const Edge& en = edges[(ln.first + (k + 1 < n ? k + 1 : k)) % kRibbonEdges];
            const glm::vec3 dir = en.bottom - ep.bottom;
            glm::vec3 side(0.0f, 0.0f, 1.0f);  // upstream: world up where the trail has no direction
            const float dl = glm::length(dir);
            if (dl > 1e-5f) {
                const glm::vec3 cross = glm::cross(dir / dl, cam.pos - p);
                const float cl = glm::length(cross);
                if (cl > 1e-5f) side = cross / cl;
            }
            const uint32_t color = packColor(cr, cg, cb, a);
            const glm::vec3 top = p + side * above - cam.pos, bot = p - side * below - cam.pos;
            v[k * 2] = {uu, 0.0f, color, top.x, top.y, top.z};
            v[k * 2 + 1] = {uu, 1.0f, color, bot.x, bot.y, bot.z};
        }
        prof::count(prof::kDraws, 1);
        prof::count(prof::kVertices, static_cast<uint32_t>(n * 2));
        sceGumDrawArray(GU_TRIANGLE_STRIP, kSpriteType, n * 2, nullptr, v);
        st.ribbonVerts += static_cast<uint32_t>(n * 2);
        ++st.draws;
        in.drawn = true;
    }
}

void LegacyRules::drawBefore(ParticleEngine& eng, FxInstance* in, int n, uint8_t lo, uint8_t hi,
                             const TerrainCamera& cam, int maxPx, bool level3Rules, FxFrameStats& st) {
    if (n <= 0) return;
    n = std::min(n, 64);
    pp::textureResetBinding();
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);  // no depth writes: everything here is blended
    sceGuEnable(GU_BLEND);
    sceGuShadeModel(GU_SMOOTH);
    sceGumMatrixMode(GU_MODEL);
    for (int i = 0; i < n; ++i)
        if (in[i].mesh && in[i].fx->h->groupCount) drawMesh(in[i], cam, st);
    drawSprites(eng, in, n, lo, hi, cam, maxPx, level3Rules, st);
    // The level-3 draws (FxGuOut) and the ribbons: camera-relative vertices on the identity model matrix.
    sceGumLoadIdentity();
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

void LegacyRules::drawAfter(ParticleEngine& eng, FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam,
                            bool level3Rules, FxFrameStats& st) {
    (void)lo;
    (void)hi;
    if (n <= 0) return;
    n = std::min(n, 64);
    sceGuDisable(GU_ALPHA_TEST);
    for (int i = 0; i < n; ++i)
        if (in[i].fx->ribbonCount && mine(in[i], level3Rules)) drawRibbons(eng, in[i], cam, st);
    st.listFull += g_geOut.takeListFull();
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_FALSE);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuColor(0xFFFFFFFFu);
    pp::textureResetBinding();
    if (st.listFull) pp::displayListFull("fx");
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ParticleEngine: the users' shared use (wowee.cfg read once at the first acquire).

ParticleEngine& particleEngine() {
    static ParticleEngine engine;
    return engine;
}

bool ParticleEngine::acquireShared() {
    if (refs_++ > 0) return pool_ != nullptr;
    // One pool, one allocation, both shares (wowee.cfg read once here).
    const bool spells = pp::configGetInt("spellfx", 0) != 0;
    const bool world = pp::configGetInt("particles", 1) != 0;
    FxPoolConfig cfg;
    cfg.spellShare = spells ? std::clamp(pp::configGetInt("spellfxsprites", 96), 0, 256) : 0;
    cfg.worldShare = world ? std::clamp(pp::configGetInt("particlesprites", 256), 0, 1024) : 0;
    // the weapons' glows (enchantfx=0: none).
    // A glowing weapon (5 effects) keeps ~10 particles an effect alive at the full rate: 96 = the own two weapons.
    cfg.weaponShare =
        pp::configGetInt("enchantfx", 1) != 0 ? std::clamp(pp::configGetInt("enchantsprites", 96), 0, 256) : 0;
    cfg.lanes = std::clamp(pp::configGetInt("particleribbons", 16), 0, kMaxLanes);
    // The level-3 rules (wowee.cfg ptclevel3, default 1; 0 = upstream's rules for every blob). The key's older name
    // ptcclient is read too (a card's wowee.cfg may carry it); ptclevel3 wins when both are there.
    const char* rulesKey = "default";
    int rules = 1;
    if (pp::configGetInt("ptclevel3", -1) >= 0) {
        rules = pp::configGetInt("ptclevel3", 1);
        rulesKey = "ptclevel3";
    } else if (pp::configGetInt("ptcclient", -1) >= 0) {
        rules = pp::configGetInt("ptcclient", 1);
        rulesKey = "ptcclient";
    }
    level3Rules_ = rules != 0;
    shares_ = cfg;
    FxPoolConfig pool = cfg;
    if (pool.spellShare + pool.worldShare + pool.weaponShare <= 0) pool.worldShare = 1;  // at least one slot
    if (!g_legacy.init(pool.spellShare + pool.worldShare + pool.weaponShare) || !acquire(pool)) {
        PSP_LOGE(pp::tag::Mem, "fx_engine oom pool=%d lanes=%d", pool.spellShare + pool.worldShare + pool.weaponShare,
                 cfg.lanes);
        g_legacy.shutdown();
        release();
        refs_ = 0;  // nobody holds it: a later acquire tries again
        return false;
    }
    setOutput(&g_geOut);
    setLegacy(&g_legacy);
    memoryBytes_ = static_cast<uint32_t>(cap_ * (sizeof(ptcsim::Particle) + 4) + laneCount_ * sizeof(Lane) +
                                         kOwners * kMaxEmitters * sizeof(EmitCache)) +
                   g_legacy.bytes(cap_);
    PSP_LOGI(pp::tag::Gfx,
             "fx_engine pool=%d spell_share=%d world_share=%d weapon_share=%d lanes=%d kb=%u ptclevel3=%d key=%s",
             cap_, shares_.spellShare, shares_.worldShare, shares_.weaponShare, laneCount_,
             static_cast<unsigned>((memoryBytes_ + 1023u) / 1024u), level3Rules_ ? 1 : 0, rulesKey);
    return true;
}

void ParticleEngine::releaseShared() {
    if (refs_ <= 0 || --refs_ > 0) return;
    setLegacy(nullptr);
    setOutput(nullptr);
    release();
    g_legacy.shutdown();
    shares_ = FxPoolConfig{};
    shares_.worldShare = shares_.weaponShare = 0;
    memoryBytes_ = 0;
}

// 32 x 32 T8: alpha = upstream's point-sprite edge (1 - smoothstep(0.3, 0.5, r)), white.
const pp::Texture* fxDotTexture() {
    constexpr int kSize = 32;
    alignas(64) static uint8_t block[256 * 4 + kSize * kSize];
    static pp::Texture tex;
    static bool made = false;
    if (made) return &tex;
    auto* clut = reinterpret_cast<uint32_t*>(block);
    uint8_t* px = block + 256 * 4;
    for (int i = 0; i < 256; ++i) clut[i] = 0x00FFFFFFu | (static_cast<uint32_t>(i) << 24);
    // Linear T8 (16-byte rows, 8-row blocks: 32 x 32 needs no swizzle).
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = (x + 0.5f) / kSize - 0.5f, dy = (y + 0.5f) / kSize - 0.5f;
            const float rr = std::sqrt(dx * dx + dy * dy);
            const float t = std::clamp((rr - 0.3f) / 0.2f, 0.0f, 1.0f);
            const float a = 1.0f - t * t * (3.0f - 2.0f * t);
            px[y * kSize + x] = static_cast<uint8_t>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    sceKernelDcacheWritebackRange(block, sizeof(block));
    tex.pixels = px;
    tex.clut = clut;
    tex.w = tex.h = tex.uw = tex.uh = kSize;
    tex.clutCount = 256;
    tex.psm = GU_PSM_T8;
    tex.swizzled = 0;
    tex.flags = pp::kTexHasAlpha | pp::kTexLinear;
    tex.levels = 1;
    tex.mip[0] = px;
    tex.name = "fx_dot";
    made = true;
    return &tex;
}

// ---------------------------------------------------------------------------------------------
// WorldParticles

struct WorldParticles::Loaded {
    void* blob = nullptr;
    uint32_t bytes = 0;
    FxView view;
    uint32_t lastFrame = 0;
    int users = 0;      // active slots
    uint8_t file = 0;
    ~Loaded() { std::free(blob); }
};

struct WorldParticles::Tex {
    pp::Texture tex;
    uint32_t bytes = 0;
    uint32_t lastFrame = 0, triedFrame = 0;
    int refs = 0;       // loaded effects using it
    bool ok = false;
    bool broken = false;  // read / parse failed: not retried (a full cache is retried while drawn)
    ~Tex() {
        if (ok) pp::textureRelease(tex);
    }
};

struct WorldParticles::Slot {
    WorldParticles* self = nullptr;
    uint64_t key = 0;
    bool used = false;
    uint32_t seen = 0;
    Loaded* fx = nullptr;
    uint8_t file = 0;
    float acc[kFxEmitterSlots] = {};
    int16_t lanes[kFxMaxRibbons] = {-1, -1, -1, -1};
    float pos[3] = {0, 0, 0};
    float axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float scale = 1.0f;
    float d2 = 0.0f;
    uint8_t owner = 0;
};

WorldParticles::WorldParticles() = default;

WorldParticles::~WorldParticles() { close(); }

uint32_t WorldParticles::indexBytes() const {
    uint32_t b = 0;
    for (const File& f : files_) b += static_cast<uint32_t>(f.dir.size() * 4u);
    return b;
}

bool WorldParticles::read(int file, uint32_t offset, void* dst, uint32_t size) {
    // One descriptor for both paks: the other one's is closed first (the card's open-file limit).
    if (fd_ >= 0 && fdFile_ != file) {
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
        fd_ = -1;
    }
    if (fd_ < 0) {
        fd_ = pp::pakOpenRetry(files_[file].path.c_str());  // + one retry (rare pak relieved)
        if (fd_ < 0) {
            PSP_LOGE(pp::tag::Fs, "particles_pak open err=%08X %s", static_cast<unsigned>(fd_), files_[file].path.c_str());
            fd_ = -1;
            return false;
        }
        pp::pakNoteOpen(fd_, files_[file].path.c_str());  // in the descriptor budget
        fdFile_ = file;
    }
    lastReadMs_ = pp::logUptimeMs();
    return pp::pakRead(fd_, offset, dst, size);
}

void WorldParticles::closeIdle(uint32_t nowMs) {
    if (fd_ >= 0 && nowMs - lastReadMs_ > kCloseAfterMs) {
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
        fd_ = -1;
    }
}

// The pak's first TOC record must be "pidx" (psp_particle_format.hpp): read without the TOC.
bool WorldParticles::openFile(File& f, const char* path) {
    f = File{};
    if (!path || !*path) return false;
    f.path = path;
    const int idx = (&f == &files_[0]) ? 0 : 1;
    uint32_t head[4];
    if (!read(idx, 0, head, sizeof(head)) || std::memcmp(head, "WPAK", 4) != 0 || head[2] == 0) return false;
    pp::PakDiskEntry rec;
    if (!read(idx, head[3], &rec, sizeof(rec))) return false;
    rec.name[47] = 0;
    if (std::strcmp(rec.name, "pidx") != 0 || rec.size < sizeof(ParticleIndexHeader)) {
        PSP_LOGI(pp::tag::Fs, "particles_none %s (no pidx: rebake with psp_model_conv)", path);
        return false;
    }
    ParticleIndexHeader h;
    if (!read(idx, rec.offset, &h, sizeof(h)) || std::memcmp(h.magic, "WPFX", 4) != 0 ||
        h.directoryBytes < sizeof(ParticleIndexHeader) || h.directoryBytes > rec.size || h.directoryBytes > (1u << 20)) {
        PSP_LOGE(pp::tag::Fs, "particles_bad_index %s", path);
        return false;
    }
    f.dir.assign((h.directoryBytes + 3u) / 4u, 0u);
    if (!read(idx, rec.offset, f.dir.data(), h.directoryBytes) ||
        !particleIndexValid(f.dir.data(), h.directoryBytes, rec.size)) {
        PSP_LOGE(pp::tag::Fs, "particles_bad_index %s", path);
        f.dir.clear();
        return false;
    }
    const auto* b = reinterpret_cast<const uint8_t*>(f.dir.data());
    f.h = reinterpret_cast<const ParticleIndexHeader*>(b);
    f.tiles = reinterpret_cast<const ParticleTileRec*>(b + f.h->tileOffset);
    f.displays = reinterpret_cast<const ParticleDisplayRec*>(b + f.h->displayOffset);
    f.effects = reinterpret_cast<const ParticleEffectRec*>(b + f.h->effectOffset);
    f.textures = reinterpret_cast<const ParticleTextureRec*>(b + f.h->textureOffset);
    f.entryOffset = rec.offset;
    f.entrySize = rec.size;
    f.ok = true;
    return true;
}

bool WorldParticles::open(const WorldFxConfig& cfg, const char* doodadsPak, const char* gameObjectsPak) {
    close();
    cfg_ = cfg;
    cfg_.maxInstances = std::clamp(cfg_.maxInstances, 1, static_cast<int>(kWorldOwnerEnd - kWorldOwnerBase));
    cfg_.distance = std::clamp(cfg_.distance, 5.0f, 150.0f);
    cfg_.lodDistance = std::clamp(cfg_.lodDistance, 0.0f, cfg_.distance);
    cfg_.emitterCap = std::clamp(cfg_.emitterCap, 2, 256);
    cfg_.modelCap = std::clamp(cfg_.modelCap, 2, 1024);
    cfg_.cacheBytes = std::clamp<uint32_t>(cfg_.cacheBytes, 32u * 1024u, 4096u * 1024u);
    cfg_.maxPx = std::clamp(cfg_.maxPx, 4, 256);
    cfg_.density = std::clamp(cfg_.density, 0.05f, 4.0f);
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    openFile(files_[0], doodadsPak);
    openFile(files_[1], gameObjectsPak);
    if (fd_ >= 0) {  // the directories are in: the descriptor goes back at once (world entry)
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
        fd_ = -1;
    }
    if (!files_[0].ok && !files_[1].ok) {
        close();
        return false;
    }
    if (!particleEngine().acquireShared()) {
        close();
        return false;
    }
    slots_.reset(new (std::nothrow) Slot[cfg_.maxInstances]);
    if (!slots_) {
        particleEngine().releaseShared();
        close();
        return false;
    }
    for (int i = 0; i < cfg_.maxInstances; ++i) {
        slots_[i].self = this;
        slots_[i].owner = static_cast<uint8_t>(kWorldOwnerBase + i);
    }
    frameList_.reserve(static_cast<size_t>(cfg_.maxInstances));
    frameSlot_.reserve(static_cast<size_t>(cfg_.maxInstances));
    const ParticleIndexHeader* d = files_[0].ok ? files_[0].h : nullptr;
    const ParticleIndexHeader* g = files_[1].ok ? files_[1].h : nullptr;
    PSP_LOGI(pp::tag::Gfx,
             "particles on tiles=%u placements=%u effects=%u textures=%u go_displays=%u go_effects=%u index_kb=%u "
             "inst=%d dist=%.0f lod=%.0f emit=%d model=%d kb=%u px=%d density=%.2f ribbons=%d share=%d ms=%u",
             d ? static_cast<unsigned>(d->tileCount) : 0u, d ? static_cast<unsigned>(d->placementCount) : 0u,
             d ? static_cast<unsigned>(d->effectCount) : 0u, d ? static_cast<unsigned>(d->textureCount) : 0u,
             g ? static_cast<unsigned>(g->displayCount) : 0u, g ? static_cast<unsigned>(g->effectCount) : 0u,
             static_cast<unsigned>((indexBytes() + 1023u) / 1024u), cfg_.maxInstances, cfg_.distance, cfg_.lodDistance,
             cfg_.emitterCap, cfg_.modelCap, static_cast<unsigned>(cfg_.cacheBytes / 1024u), cfg_.maxPx, cfg_.density,
             cfg_.ribbons ? 1 : 0, particleEngine().worldShare(),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    pp::logMemory("particles_open");
    return true;
}

void WorldParticles::releaseSlot(Slot& s) {
    if (!s.used) return;
    particleEngine().freeOwner(s.owner);
    for (int16_t& l : s.lanes) {
        particleEngine().freeLane(l);
        l = -1;
    }
    if (s.fx) {
        if (s.fx->users > 0) --s.fx->users;
        s.fx->lastFrame = frame_;  // its blob may be in this frame's display list
    }
    s.used = false;
    s.fx = nullptr;
    s.key = 0;
}

void WorldParticles::close() {
    if (slots_) {
        for (int i = 0; i < cfg_.maxInstances; ++i) releaseSlot(slots_[i]);
        slots_.reset();
        particleEngine().releaseShared();
    }
    loaded_.clear();
    textures_.clear();
    broken_.clear();
    tiles_.clear();
    candidates_.clear();
    objects_.clear();
    picks_.clear();
    frameList_.clear();
    frameSlot_.clear();
    cacheBytes_ = 0;
    for (File& f : files_) f = File{};
    if (fd_ >= 0) {
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
        fd_ = -1;
    }
    tileX_ = tileY_ = -10000;
    candidatesDirty_ = true;
}

void WorldParticles::setObjects(const std::vector<WorldFxObject>& objects) {
    if (!files_[1].ok) return;
    objects_.clear();
    const ParticleDisplayRec* d = files_[1].displays;
    const uint32_t n = files_[1].h->displayCount;
    for (const WorldFxObject& o : objects) {
        const auto* it = std::lower_bound(d, d + n, o.displayId,
                                          [](const ParticleDisplayRec& r, uint32_t id) { return r.displayId < id; });
        if (it != d + n && it->displayId == o.displayId) objects_.push_back(o);
    }
    candidatesDirty_ = true;
}

// The doodads' tiles around the camera (their particle placement lists), when it entered another tile.
void WorldParticles::updateTiles(const glm::vec3& camPos) {
    if (!files_[0].ok) return;
    const int tx = static_cast<int>(std::floor(32.0f - camPos.y / kTile));
    const int ty = static_cast<int>(std::floor(32.0f - camPos.x / kTile));
    // Reach: the farthest model the selection can take (+ the converter's largest reach).
    const float want = cfg_.distance + kMaxReach, keep = want + 32.0f;
    auto yardsTo = [&](int x, int y) {  // Chebyshev to the tile's square (TileStreamer::yardsTo)
        const float xLo = (31 - y) * kTile, xHi = (32 - y) * kTile;
        const float yLo = (31 - x) * kTile, yHi = (32 - x) * kTile;
        return std::max(std::max({0.0f, xLo - camPos.x, camPos.x - xHi}), std::max({0.0f, yLo - camPos.y, camPos.y - yHi}));
    };
    // Re-checked when the camera's 64-yd cell changes (cheap otherwise).
    const int cx = static_cast<int>(std::floor(camPos.x / 64.0f)), cy = static_cast<int>(std::floor(camPos.y / 64.0f));
    if (cx == tileX_ && cy == tileY_) return;
    tileX_ = cx;
    tileY_ = cy;
    bool changed = false;
    for (size_t i = tiles_.size(); i-- > 0;) {
        if (yardsTo(tiles_[i].x, tiles_[i].y) > keep) {
            tiles_.erase(tiles_.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
        }
    }
    const File& f = files_[0];
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            const int x = tx + dx, y = ty + dy;
            if (yardsTo(x, y) > want) continue;
            bool have = false;
            for (const TileData& t : tiles_) have = have || (t.x == x && t.y == y);
            if (have) continue;
            const ParticleTileRec* rec = std::lower_bound(
                f.tiles, f.tiles + f.h->tileCount, std::make_pair(x, y), [](const ParticleTileRec& r, const std::pair<int, int>& k) {
                    return r.x < k.first || (r.x == k.first && r.y < k.second);
                });
            if (rec == f.tiles + f.h->tileCount || rec->x != x || rec->y != y || !rec->count) continue;
            TileData t;
            t.x = static_cast<int16_t>(x);
            t.y = static_cast<int16_t>(y);
            t.list.resize(rec->count);
            if (!read(0, f.entryOffset + rec->offset, t.list.data(), rec->count * static_cast<uint32_t>(sizeof(ParticlePlacement)))) {
                ++loadFails_;
                continue;
            }
            ++tileLoads_;
            tiles_.push_back(std::move(t));
            changed = true;
        }
    }
    if (changed) candidatesDirty_ = true;
}

void WorldParticles::rebuildCandidates() {
    candidatesDirty_ = false;
    candidates_.clear();
    std::unordered_set<uint32_t> seen;
    auto add = [&](uint8_t file, uint32_t uid, uint16_t effect, const float* pos, const float* axes) {
        const File& f = files_[file];
        if (!f.ok || effect >= f.h->effectCount) return;
        Candidate c;
        c.key = (static_cast<uint64_t>(file) << 32) | uid;
        c.effect = effect;
        c.file = file;
        float scale = 0.0f;
        for (int a = 0; a < 3; ++a) {
            c.pos[a] = pos[a];
            const float l = std::sqrt(axes[a * 3] * axes[a * 3] + axes[a * 3 + 1] * axes[a * 3 + 1] + axes[a * 3 + 2] * axes[a * 3 + 2]);
            scale = std::max(scale, l);
        }
        for (int k = 0; k < 9; ++k) c.axes[k] = axes[k];
        if (!finiteBits(scale) || !finiteBits(c.pos[0]) || !finiteBits(c.pos[1]) || !finiteBits(c.pos[2])) return;
        c.scale = std::clamp(scale, 0.01f, 100.0f);
        c.reach = f.effects[effect].reach * c.scale;
        candidates_.push_back(c);
    };
    for (const TileData& t : tiles_)
        for (const ParticlePlacement& p : t.list)
            if (seen.insert(p.uniqueId).second) add(0, p.uniqueId, p.effect, p.position, p.axes);  // a border doodad: once
    objectStart_ = candidates_.size();
    if (files_[1].ok) {
        const ParticleDisplayRec* d = files_[1].displays;
        const uint32_t n = files_[1].h->displayCount;
        for (const WorldFxObject& o : objects_) {
            const auto* it = std::lower_bound(d, d + n, o.displayId,
                                              [](const ParticleDisplayRec& r, uint32_t id) { return r.displayId < id; });
            if (it != d + n && it->displayId == o.displayId) add(1, o.uniqueId, it->effect, o.position, o.axes);
        }
    }
    picks_.reserve(candidates_.size() + 1);  // no allocation in the frame
}

bool WorldParticles::makeRoom(uint32_t need) {
    while (cacheBytes_ + need > cfg_.cacheBytes) {
        auto tv = textures_.end();
        for (auto it = textures_.begin(); it != textures_.end(); ++it)
            if (it->second->refs == 0 && frame_ - it->second->lastFrame > 2 &&
                (tv == textures_.end() || it->second->lastFrame < tv->second->lastFrame))
                tv = it;
        if (tv != textures_.end()) {
            cacheBytes_ -= tv->second->bytes;
            textures_.erase(tv);
            ++evicts_;
            continue;
        }
        auto ev = loaded_.end();
        for (auto it = loaded_.begin(); it != loaded_.end(); ++it)
            if (it->second->users == 0 && frame_ - it->second->lastFrame > 2 &&
                (ev == loaded_.end() || it->second->lastFrame < ev->second->lastFrame))
                ev = it;
        if (ev == loaded_.end()) return false;
        const Loaded& L = *ev->second;
        for (uint32_t i = 0; i < L.view.h->textureCount; ++i) {
            auto t = textures_.find((static_cast<uint32_t>(L.file) << 16) | L.view.textures[i]);
            if (t != textures_.end() && t->second->refs > 0) --t->second->refs;
        }
        cacheBytes_ -= L.bytes;
        loaded_.erase(ev);
        ++evicts_;
    }
    return true;
}

const pp::Texture* WorldParticles::texture(int file, uint16_t id) {
    auto it = textures_.find((static_cast<uint32_t>(file) << 16) | id);
    if (it == textures_.end()) return nullptr;
    Tex& t = *it->second;
    t.lastFrame = frame_;
    if (!t.ok && !t.broken && t.triedFrame != frame_) {  // the cache was full at load: try again
        t.triedFrame = frame_;
        const File& f = files_[file];
        if (id < f.h->textureCount && makeRoom(f.textures[id].size)) {
            const uint32_t ts = f.textures[id].size;
            void* tb = memalign(64, (ts + 63u) & ~63u);
            if (tb && read(file, f.entryOffset + f.textures[id].offset, tb, ts)) {
                sceKernelDcacheWritebackRange(tb, ts);
                if (pp::textureFromBlob(tb, ts, t.tex, true)) {
                    t.ok = true;
                    t.bytes = ts;
                    cacheBytes_ += ts;
                } else {
                    std::free(tb);
                    t.broken = true;
                }
            } else {
                std::free(tb);
                t.broken = true;
            }
        }
    }
    return t.ok ? &t.tex : nullptr;
}

const pp::Texture* WorldParticles::texFn(void* user, uint16_t id) {
    auto* s = static_cast<Slot*>(user);
    return s->self->texture(s->file, id);
}

// An effect into the cache (its textures with it), or nullptr (not now / broken).
WorldParticles::Loaded* WorldParticles::effect(int file, uint16_t id, bool allowLoad) {
    const uint32_t key = (static_cast<uint32_t>(file) << 16) | id;
    auto found = loaded_.find(key);
    if (found != loaded_.end()) {
        found->second->lastFrame = frame_;
        return found->second.get();
    }
    if (!allowLoad || broken_.count(key)) return nullptr;
    const File& f = files_[file];
    if (!f.ok || id >= f.h->effectCount) return nullptr;
    PROF_SCOPE("particles.load");
    const uint32_t size = f.effects[id].size;
    if (!makeRoom(size)) return nullptr;
    void* blob = memalign(64, (size + 63u) & ~63u);
    if (!blob || !read(file, f.entryOffset + f.effects[id].offset, blob, size) || !spellFxValid(blob, size) ||
        !spellFxExt(blob)) {
        const bool full = blob == nullptr;
        std::free(blob);
        if (!full) {
            broken_.insert(key);  // not read again
            if (++loadFails_ <= 10)
                PSP_LOGW(pp::tag::Gfx, "particles_bad effect=%u file=%d (rebake)", static_cast<unsigned>(id), file);
        }
        return nullptr;
    }
    sceKernelDcacheWritebackRange(blob, size);
    auto L = std::make_unique<Loaded>();
    L->blob = blob;
    L->bytes = size;
    L->file = static_cast<uint8_t>(file);
    L->view.set(blob);
    L->lastFrame = frame_;
    cacheBytes_ += size;
    for (uint32_t i = 0; i < L->view.h->textureCount; ++i) {
        const uint16_t tid = L->view.textures[i];
        const uint32_t tkey = (static_cast<uint32_t>(file) << 16) | tid;
        auto it = textures_.find(tkey);
        if (it == textures_.end()) {
            auto te = std::make_unique<Tex>();
            te->lastFrame = frame_;
            te->triedFrame = frame_;
            if (tid < f.h->textureCount && makeRoom(f.textures[tid].size)) {
                const uint32_t ts = f.textures[tid].size;
                void* tb = memalign(64, (ts + 63u) & ~63u);
                if (tb && read(file, f.entryOffset + f.textures[tid].offset, tb, ts)) {
                    sceKernelDcacheWritebackRange(tb, ts);
                    if (pp::textureFromBlob(tb, ts, te->tex, true)) {
                        te->ok = true;
                        te->bytes = ts;
                        cacheBytes_ += ts;
                    } else {
                        std::free(tb);
                        te->broken = true;
                    }
                } else {
                    std::free(tb);
                    te->broken = tid >= f.h->textureCount;
                }
            } else if (tid >= f.h->textureCount) {
                te->broken = true;
            }
            it = textures_.emplace(tkey, std::move(te)).first;
        }
        ++it->second->refs;
    }
    Loaded* out = L.get();
    loaded_.emplace(key, std::move(L));
    if (++loads_ <= 60)
        PSP_LOGI(pp::tag::Gfx, "particles_load effect=%u file=%d kb=%u emitters=%u ribbons=%u groups=%u cls=%u cache_kb=%u",
                 static_cast<unsigned>(id), file, static_cast<unsigned>((size + 1023u) / 1024u),
                 static_cast<unsigned>(out->view.h->emitterCount), static_cast<unsigned>(out->view.ribbonCount),
                 static_cast<unsigned>(out->view.h->groupCount), static_cast<unsigned>(out->view.cls),
                 static_cast<unsigned>(cacheBytes_ / 1024u));
    return out;
}

void WorldParticles::frame(const TerrainCamera& cam, float dtSec) {
    if (!slots_) return;
    ++frame_;
    const uint32_t nowMs = pp::logUptimeMs();
    const float dt = std::clamp(dtSec, 0.0f, 0.25f);
    clockMs_ += dt * 1000.0f;
    if (clockMs_ > 6.0e6f) clockMs_ = std::fmod(clockMs_, 3.6e6f);  // float ms stays exact enough (wrap ~100 min)
    updateTiles(cam.pos);
    if (candidatesDirty_) rebuildCandidates();
    closeIdle(nowMs);
    bool anyActive = false;
    for (int i = 0; i < cfg_.maxInstances; ++i) anyActive = anyActive || slots_[i].used;
    if (candidates_.empty() && !anyActive) {
        log(nowMs);
        return;
    }
    PROF_SCOPE("particles");
    const uint32_t t0 = sceKernelGetSystemTimeLow();

    // The emitter models in reach (the frustum beyond 15 yd), nearest first.
    const float aspect = static_cast<float>(pp::kScreenW) / pp::kScreenH;
    const glm::mat4 proj = glm::perspective(glm::radians(cam.fovyDeg), aspect, cam.nearZ, cam.farZ);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), cam.forward, cam.up);
    const Frustum fr = makeFrustum(proj * view);
    picks_.clear();
    for (uint32_t ci = 0; ci < candidates_.size(); ++ci) {
        const Candidate& c = candidates_[ci];
        const glm::vec3 d(c.pos[0] - cam.pos.x, c.pos[1] - cam.pos.y, c.pos[2] - cam.pos.z);
        const float d2 = glm::dot(d, d);
        const float lim = cfg_.distance + c.reach;
        if (d2 > lim * lim) continue;
        if (d2 > 225.0f && fr.sphereOutside(d, c.reach)) continue;
        picks_.push_back(Pick{d2, ci});
    }
    const size_t k = std::min(picks_.size(), static_cast<size_t>(cfg_.maxInstances));
    std::partial_sort(picks_.begin(), picks_.begin() + static_cast<std::ptrdiff_t>(k), picks_.end(),
                      [](const Pick& a, const Pick& b) { return a.d2 < b.d2; });
    // Keep the slots still picked, free the others, give new picks a free slot (one effect read a frame).
    for (int i = 0; i < cfg_.maxInstances; ++i) slots_[i].seen = 0;
    for (size_t pi = 0; pi < k; ++pi) {
        const Candidate& c = candidates_[picks_[pi].c];
        for (int i = 0; i < cfg_.maxInstances; ++i) {
            if (slots_[i].used && slots_[i].key == c.key) {
                slots_[i].seen = frame_;
                slots_[i].d2 = picks_[pi].d2;
                break;
            }
        }
    }
    for (int i = 0; i < cfg_.maxInstances; ++i)
        if (slots_[i].used && slots_[i].seen != frame_) releaseSlot(slots_[i]);
    int readsLeft = 1;
    for (size_t pi = 0; pi < k; ++pi) {
        const Candidate& c = candidates_[picks_[pi].c];
        bool have = false;
        for (int i = 0; i < cfg_.maxInstances && !have; ++i) have = slots_[i].used && slots_[i].key == c.key;
        if (have) continue;
        int free = -1;
        for (int i = 0; i < cfg_.maxInstances && free < 0; ++i)
            if (!slots_[i].used) free = i;
        if (free < 0) break;
        const bool resident = loaded_.count((static_cast<uint32_t>(c.file) << 16) | c.effect) != 0;
        if (!resident && readsLeft <= 0) continue;  // read next frame
        if (!resident) --readsLeft;
        Loaded* L = effect(c.file, c.effect, true);
        if (!L) continue;
        Slot& s = slots_[free];
        s.used = true;
        s.key = c.key;
        s.seen = frame_;
        s.d2 = picks_[pi].d2;
        s.fx = L;
        s.file = c.file;
        ++L->users;
        for (float& a : s.acc) a = 0.0f;
        for (int a = 0; a < 3; ++a) s.pos[a] = c.pos[a];
        for (int a = 0; a < 9; ++a) s.axes[a] = c.axes[a];
        s.scale = c.scale;
        for (int r = 0; r < kFxMaxRibbons; ++r)
            s.lanes[r] = (cfg_.ribbons && r < L->view.ribbonCount) ? static_cast<int16_t>(particleEngine().allocLane())
                                                                   : static_cast<int16_t>(-1);
    }
    // This frame's instances, nearest first.
    frameList_.clear();
    frameSlot_.clear();
    for (int i = 0; i < cfg_.maxInstances; ++i)
        if (slots_[i].used) frameSlot_.push_back(i);
    std::sort(frameSlot_.begin(), frameSlot_.end(), [&](int a, int b) { return slots_[a].d2 < slots_[b].d2; });
    const float span = std::max(1.0f, cfg_.distance - cfg_.lodDistance);
    for (int si : frameSlot_) {
        Slot& s = slots_[si];
        FxInstance in;
        in.fx = &s.fx->view;
        in.pos = glm::vec3(s.pos[0], s.pos[1], s.pos[2]);
        float axes[9];
        std::memcpy(axes, s.axes, sizeof(axes));
        // RT-WORLD-2: with the level-3 rules and a level-3 blob the engine does not turn portal-class models by
        // itself; upstream's spin (below) otherwise.
        if ((s.fx->view.cls & kPtcSpin) && !(particleEngine().level3Rules() && s.fx->view.level3)) {
            // upstream: an instance portal turns about z at 1.2 rad/s (M2Renderer::update).
            const float a = std::fmod(clockMs_ * 0.0012f, 6.2831853f), ca = std::cos(a), sa = std::sin(a);
            for (int col = 0; col < 3; ++col) {
                const float x = s.axes[col * 3], y = s.axes[col * 3 + 1];
                axes[col * 3] = ca * x - sa * y;
                axes[col * 3 + 1] = sa * x + ca * y;
            }
        }
        std::memcpy(in.axes, axes, sizeof(axes));
        in.scale = s.scale;
        in.timeMs = clockMs_;
        // RT-WORLD-1: with the level-3 rules and a level-3 blob, the world clock + the instance's phase from its
        // uniqueId (instances of one model do not animate in step).
        if (particleEngine().level3Rules() && s.fx->view.level3) in.timeMs += fxWorldPhaseMs(static_cast<uint32_t>(s.key));
        in.acc = s.acc;
        for (int r = 0; r < kFxMaxRibbons; ++r) in.lanes[r] = s.lanes[r];
        in.owner = s.owner;
        in.mesh = true;
        // PSP LOD: full density inside particlelod, down to 1/4 at particledist.
        const float d = std::sqrt(s.d2);
        const float w = d <= cfg_.lodDistance ? 1.0f : std::max(0.25f, 1.0f - 0.75f * (d - cfg_.lodDistance) / span);
        in.rate = cfg_.density * w;
        in.camDist = d;  // RT-WORLD-3: the camera's distance to the model origin (the level-3 emission throttle)
        // PSP: an emitter's share - particleemit, and particlemodel split over the model's emitters (the
        // inn's chandelier: 12 candles, each still a flame).
        const int emitters = std::max(1, s.fx->view.h->emitterCount + ((s.fx->view.cls & kPtcSmoke) ? 1 : 0));
        const int perEmitter = std::min(cfg_.emitterCap, std::max(2, cfg_.modelCap / emitters));
        in.cap = std::max(2, static_cast<int>(static_cast<float>(perEmitter) * w + 0.5f));
        in.tex = &WorldParticles::texFn;
        in.texUser = &s;
        frameList_.push_back(in);
    }
    FxFrameStats st;
    ParticleEngine& eng = particleEngine();
    const int n = static_cast<int>(frameList_.size());
    eng.step(frameList_.data(), n, kWorldOwnerBase, kWorldOwnerEnd, eng.worldShare(), dt, st);
    PROF_GE_PASS("particles");
    eng.draw(frameList_.data(), n, kWorldOwnerBase, kWorldOwnerEnd, cam, cfg_.maxPx, st);
    PROF_GE_PASS("char");  // what follows (spell effects set their own; the target portrait) is a character render
    // What the GE may still read stays cached two more frames.
    for (int si : frameSlot_) {
        Loaded* L = slots_[si].fx;
        L->lastFrame = frame_;
        for (uint32_t t = 0; t < L->view.h->textureCount; ++t) {
            auto it = textures_.find((static_cast<uint32_t>(L->file) << 16) | L->view.textures[t]);
            if (it != textures_.end()) it->second->lastFrame = frame_;
        }
    }
    last_ = st;
    lastActive_ = static_cast<uint32_t>(n);
    lastCandidates_ = static_cast<uint32_t>(candidates_.size());
    peakLive_ = std::max(peakLive_, st.live);
    const uint32_t us = sceKernelGetSystemTimeLow() - t0;
    usSum_ += us;
    usMax_ = std::max(usMax_, us);
    ++usFrames_;
    log(nowMs);
}

void WorldParticles::log(uint32_t nowMs) {
    if (nowMs < nextLogMs_) return;
    nextLogMs_ = nowMs + 10000u;
    if (!usFrames_ && logs_ > 0 && !lastActive_) return;  // quiet while nothing is near
    ++logs_;
    const uint32_t avg = usFrames_ ? usSum_ / usFrames_ : 0u;
    PSP_LOGI(pp::tag::Gfx,
             "particles_frame inst=%u cand=%u tiles=%u live=%u peak=%u sprites=%u emitted=%u draws=%u mesh=%u "
             "ribbon_verts=%u list_full=%u kb=%u effects=%u textures=%u loads=%u fails=%u evict=%u us_avg=%u us_max=%u "
             "frames=%u fd=%d",
             static_cast<unsigned>(lastActive_), static_cast<unsigned>(lastCandidates_),
             static_cast<unsigned>(tiles_.size()), static_cast<unsigned>(last_.live), static_cast<unsigned>(peakLive_),
             static_cast<unsigned>(last_.sprites), static_cast<unsigned>(last_.emitted), static_cast<unsigned>(last_.draws),
             static_cast<unsigned>(last_.meshDraws), static_cast<unsigned>(last_.ribbonVerts),
             static_cast<unsigned>(last_.listFull), static_cast<unsigned>(cacheBytes_ / 1024u),
             static_cast<unsigned>(loaded_.size()), static_cast<unsigned>(textures_.size()), static_cast<unsigned>(loads_),
             static_cast<unsigned>(loadFails_), static_cast<unsigned>(evicts_), static_cast<unsigned>(avg),
             static_cast<unsigned>(usMax_), static_cast<unsigned>(usFrames_), fd_ >= 0 ? 1 : 0);
    usSum_ = usMax_ = usFrames_ = 0;
    peakLive_ = 0;
}

// ---------------------------------------------------------------------------------------------
// WeatherFx: upstream rendering::Weather (weather.cpp). Upstream is written y-up (spawn at y + 80,
// falls along -y, recycled below y - 20); WoWee's world is z-up (camera.cpp) - the PSP runs the same
// logic on z (else the rain flies sideways).

WeatherFx::~WeatherFx() { std::free(drops_); }

float WeatherFx::rnd01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
}

void WeatherFx::configure(const WeatherConfig& cfg) {
    cfg_ = cfg;
    cfg_.maxDrops = std::clamp(cfg_.maxDrops, 0, 2000);
    cfg_.range = std::clamp(cfg_.range, 5.0f, 100.0f);
    std::free(drops_);
    drops_ = nullptr;
    capacity_ = count_ = 0;
    if (cfg_.maxDrops > 0) {
        drops_ = static_cast<Drop*>(std::malloc(sizeof(Drop) * cfg_.maxDrops));
        if (drops_) capacity_ = cfg_.maxDrops;
    }
}

void WeatherFx::set(WeatherKind kind, float intensity, bool dark) {
    if (!finiteBits(intensity)) intensity = 0.0f;
    if (kind != kind_) count_ = 0;  // upstream: a new kind starts over (resetParticles)
    kind_ = kind;
    intensity_ = std::clamp(intensity, 0.0f, 1.0f);
    dark_ = dark;
}

// upstream getRandomPosition + the spawn height (SPAWN_HEIGHT 80 of a 100 yd box: 0.8 x range).
void WeatherFx::spawn(Drop& d, const glm::vec3& cam, bool anywhere) {
    const float R = cfg_.range, H = 0.8f * cfg_.range;
    d.pos = glm::vec3(cam.x + (rnd01() * 2.0f - 1.0f) * R, cam.y + (rnd01() * 2.0f - 1.0f) * R,
                      cam.z + H * (anywhere ? rnd01() : 1.0f));
    d.life = 0.0f;
    switch (kind_) {
        case WeatherKind::Rain:
            d.vel = glm::vec3(0.0f, 0.0f, -50.0f);
            d.maxLife = 5.0f;
            break;
        case WeatherKind::Storm:
        case WeatherKind::Sand:  // PSP: a sandstorm moves as upstream's storm
            d.vel = glm::vec3(15.0f, 8.0f, -70.0f);
            d.maxLife = 3.5f;
            break;
        default:  // snow
            d.vel = glm::vec3(0.0f, 0.0f, -5.0f);
            d.maxLife = 10.0f;
            break;
    }
}

void WeatherFx::frame(const TerrainCamera& cam, float dtSec, uint32_t& usOut) {
    usOut = 0;
    if (!enabled_ || kind_ == WeatherKind::None || !drops_ || intensity_ <= 0.0f) return;
    PROF_SCOPE("weather");
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    const float dt = std::clamp(dtSec, 0.0f, 0.25f);
    const int target = std::min(capacity_, static_cast<int>(static_cast<float>(capacity_) * intensity_));
    if (count_ == 0)  // upstream resetParticles: the box filled at once
        while (count_ < target) spawn(drops_[count_++], cam.pos, true);
    while (count_ < target) spawn(drops_[count_++], cam.pos, false);
    count_ = std::min(count_, target);
    const float R = cfg_.range;
    for (int i = 0; i < count_; ++i) {
        Drop& d = drops_[i];
        d.life += dt;
        const glm::vec3 to = d.pos - cam.pos;
        if (d.life >= d.maxLife || glm::dot(to, to) > R * R || d.pos.z < cam.pos.z - 0.2f * R) spawn(d, cam.pos, false);
        if (kind_ == WeatherKind::Snow) {  // upstream: wind for snow
            d.vel.x = std::sin(d.life * 0.5f) * 2.0f;
            d.vel.y = std::cos(d.life * 0.3f) * 2.0f;
        } else if (kind_ == WeatherKind::Storm || kind_ == WeatherKind::Sand) {  // upstream: gusts
            d.vel.x = 15.0f + std::sin(d.life * 1.5f + d.pos.x * 0.1f) * 5.0f;
            d.vel.y = 8.0f + std::cos(d.life * 2.0f) * 3.0f;
        }
        d.pos += d.vel * dt;
    }
    if (count_ > 0) {
        PROF_GE_PASS("weather");
        // upstream weather.frag colours (rain 0.7 0.8 0.9 0.6, storm 0.6 0.65 0.75 0.7, snow white 0.9);
        // PSP: sand 0.85 0.75 0.55 0.6; black rain / snow darker.
        float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 0.9f;
        if (kind_ == WeatherKind::Rain) cr = 0.7f, cg = 0.8f, cb = 0.9f, ca = 0.6f;
        else if (kind_ == WeatherKind::Storm) cr = 0.6f, cg = 0.65f, cb = 0.75f, ca = 0.7f;
        else if (kind_ == WeatherKind::Sand) cr = 0.85f, cg = 0.75f, cb = 0.55f, ca = 0.6f;
        if (dark_) {
            cr *= 0.3f;
            cg *= 0.3f;
            cb *= 0.3f;
        }
        const uint32_t color = packColor(cr, cg, cb, ca);
        pp::textureResetBinding();
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_CULL_FACE);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_TRUE);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        const bool streaks = kind_ == WeatherKind::Rain || kind_ == WeatherKind::Storm;
        const uint32_t need = static_cast<uint32_t>(count_) * 2u * (streaks ? sizeof(LineVert) : sizeof(SpriteVert)) + 256u;
        if (pp::displayListFree() >= kListReserveFx + need) {
            if (streaks) {
                // PSP: upstream's rain is a 3 px point at 1080 lines - under a pixel here; a short streak
                // along the fall (0.03 s of it) reads as the same rain.
                pp::textureBind(nullptr);
                auto* v = static_cast<LineVert*>(sceGuGetMemory(static_cast<int>(count_ * 2 * sizeof(LineVert))));
                for (int i = 0; i < count_; ++i) {
                    const glm::vec3 a = drops_[i].pos - cam.pos, b = a - drops_[i].vel * 0.03f;
                    v[i * 2] = {color, a.x, a.y, a.z};
                    v[i * 2 + 1] = {color, b.x, b.y, b.z};
                }
                prof::count(prof::kDraws, 1);
                prof::count(prof::kVertices, static_cast<uint32_t>(count_ * 2));
                sceGumDrawArray(GU_LINES, kLineType, count_ * 2, nullptr, v);
            } else {
                // Snow / sand: upstream's soft point (8 px at 1080 lines -> 2 px here; PSP: 3 px) on the dot.
                pp::textureBind(fxDotTexture());
                sceGuTexWrap(GU_CLAMP, GU_CLAMP);
                sceGuTexScale(1.0f, 1.0f);
                sceGuTexOffset(0.0f, 0.0f);
                const glm::vec3 f = glm::normalize(cam.forward);
                const glm::vec3 r = glm::normalize(glm::cross(f, cam.up));
                const glm::vec3 u = glm::cross(r, f);
                const float focal = (static_cast<float>(pp::kScreenH) * 0.5f) / std::tan(glm::radians(cam.fovyDeg) * 0.5f);
                const float halfPx = (kind_ == WeatherKind::Sand ? 1.0f : 1.5f) / focal;
                auto* v = static_cast<SpriteVert*>(sceGuGetMemory(static_cast<int>(count_ * 2 * sizeof(SpriteVert))));
                int out = 0;
                for (int i = 0; i < count_; ++i) {
                    const glm::vec3 c = drops_[i].pos - cam.pos;
                    const float depth = glm::dot(c, f);
                    if (depth < cam.nearZ + 0.05f) continue;
                    const float half = halfPx * depth;
                    const glm::vec3 a = c - r * half + u * half, b = c + r * half - u * half;
                    v[out * 2] = {0.0f, 0.0f, color, a.x, a.y, a.z};
                    v[out * 2 + 1] = {1.0f, 1.0f, color, b.x, b.y, b.z};
                    ++out;
                }
                if (out) {
                    prof::count(prof::kDraws, 1);
                    prof::count(prof::kVertices, static_cast<uint32_t>(out * 2));
                    sceGumDrawArray(GU_SPRITES, kSpriteType, out * 2, nullptr, v);
                }
            }
        } else {
            pp::displayListFull("weather");
        }
        sceGuDisable(GU_BLEND);
        sceGuDepthMask(GU_FALSE);
        pp::textureResetBinding();
        PROF_GE_PASS("char");  // the target portrait follows
    }
    usOut = sceKernelGetSystemTimeLow() - t0;
}

} // namespace wowee::rendering::psp
