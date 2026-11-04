// Level-3 section builder - clean-room implementation of cleanroom/specs/particles/particle-bake-level3.md.
#include "particle_bake_l3.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rendering/psp/psp_particle_sim.hpp"

namespace wowee {
namespace pspconv {
namespace l3bake {

using namespace rendering::psp;

namespace {

inline bool fin(float f) { return fxFinite(f); }
inline float orDef(float f, float def) { return fin(f) ? f : def; }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float len3(const glm::vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// constant reduction (section 4): all samples within 1e-5 of the first -> 1 frame
uint32_t addCurve(FxPool& pool, const std::vector<float>& s, uint16_t& framesOut) {
    bool constant = true;
    for (float v : s)
        if (!(std::fabs(v - s[0]) <= 1e-5f)) {
            constant = false;
            break;
        }
    framesOut = static_cast<uint16_t>(constant ? 1 : s.size());
    return pool.add(s.data(), framesOut);
}

float baseAt(const BaseCurves& b, int curve, int k) {
    const std::vector<float>& v = b.c[curve];
    if (v.empty()) return 0.0f;
    return v[static_cast<size_t>(k) < v.size() ? static_cast<size_t>(k) : v.size() - 1];
}

struct LifeCurve {
    std::vector<uint16_t> ts;
    std::vector<float> v;
};

// BAKE-LIFE-1..4 for curve k (0 colour, 1 alpha, 2 scale, 3 head cell, 4 tail cell)
LifeCurve convertLife(const m2raw::FBlock& raw, int k) {
    LifeCurve o;
    const int dim = fxLifeDim(k);
    const size_t ns = raw.ts.size();
    const size_t nv = raw.dim == dim ? raw.keys() : 0;
    const bool cell = (k == kFxLifeHead || k == kFxLifeTail);
    if (cell && ns == 0) return o;  // BAKE-LIFE-1
    if (nv == 1 || (nv >= 1 && ns == 0)) {  // BAKE-LIFE-2
        o.ts.push_back(0);
        o.v.assign(raw.v.begin(), raw.v.begin() + dim);
    } else {  // BAKE-LIFE-3
        size_t n = std::min(std::min(ns, nv), static_cast<size_t>(kFxMaxBlockKeys));
        if (n == 0) return o;
        uint16_t prev = 0;
        for (size_t i = 0; i < n; ++i) {
            uint16_t s = raw.ts[i] > 32767 ? uint16_t(32767) : raw.ts[i];
            if (i > 0 && s < prev) s = prev;
            o.ts.push_back(s);
            prev = s;
            for (int d = 0; d < dim; ++d) o.v.push_back(raw.v[i * dim + d]);
        }
        if (n == 2 || n == 3) {
            o.ts.front() = 0;
            o.ts.back() = 32767;
        }
    }
    // BAKE-LIFE-4
    const float def = k == kFxLifeColor ? 255.0f : (cell ? 0.0f : 1.0f);
    for (float& f : o.v)
        if (!fin(f)) f = def;
    return o;
}

// one bit of BAKE-STEP
bool held(const m2raw::Model& m, const m2raw::Track& t) { return t.interp == 0 && m.keyCount(t, 0) > 1; }

glm::vec3 restOf(const Input& in, int bone, const glm::vec3& p) { return in.rest ? in.rest(bone, p) : p; }

// ABGR8888, each channel round(c x 255) = trunc(c x 255 + 0.5) (inputs already clamped to [0, 1])
uint32_t pack(float r, float g, float b, float a) {
    auto q = [](float c) { return static_cast<uint32_t>(c * 255.0f + 0.5f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

}  // namespace

uint8_t cellShiftFor(uint32_t cols) {
    uint32_t c = cols > 1 ? cols : 1;
    for (uint8_t s = 0; s <= 8; ++s)
        if ((1u << s) >= c) return s;
    return 8;
}

void bakeFrames(const m2raw::Model* raw, float fps, int maxFrames, int& frames, uint32_t& periodMs) {
    uint32_t period = 0;
    if (raw && !raw->sequences.empty()) period = raw->sequences[0].duration;
    if (period == 0) period = 3333;
    periodMs = period;
    long f = std::lround(static_cast<double>(period) * fps / 1000.0) + 1;
    if (f > maxFrames) f = maxFrames;
    if (f < 2) f = 2;
    frames = static_cast<int>(f);
}

float frameTimeMs(uint32_t periodMs, int k, int frames) {
    return static_cast<float>(static_cast<double>(periodMs) * k / static_cast<double>(frames - 1));
}

bool buildLevel3Section(const Input& in, Output& out) {
    out = Output();
    const float existing = in.existingReach;
    float reach = existing;
    const bool spellOnly = in.cls == kPtcSpell;

    bool canWrite = in.level3 && in.rawParsed && in.raw;
    if (canWrite)
        for (int idx : in.kept)
            if (idx < 0 || static_cast<size_t>(idx) >= in.raw->emitters.size()) canWrite = false;

    if (canWrite) {
        const m2raw::Model& m = *in.raw;
        const int frames = in.frames < 1 ? 1 : in.frames;
        std::vector<float> tk(static_cast<size_t>(frames));
        for (int k = 0; k < frames; ++k) tk[static_cast<size_t>(k)] = frames > 1 ? frameTimeMs(in.periodMs, k, frames) : 0.0f;

        // ---- emitters (section 5) ----
        for (size_t i = 0; i < in.kept.size(); ++i) {
            const m2raw::Emitter& e = m.emitters[static_cast<size_t>(in.kept[i])];
            SpellFxL3Emitter c;
            std::memset(&c, 0, sizeof c);
            // BAKE-EM-1
            c.flags = e.flags;
            c.type = static_cast<uint8_t>(e.type < 1 ? 1 : (e.type > 3 ? 3 : e.type));
            c.blend = static_cast<uint8_t>(e.blend > 6 ? 6 : e.blend);
            const bool lut = m2raw::lutEligible(e);
            c.lut = lut ? 1 : 0;
            c.cellShift = cellShiftFor(e.cols);
            // BAKE-EM-2
            c.lifespanVary = orDef(e.lifeVary, 0.0f);
            c.rateVary = orDef(e.rateVary, 0.0f);
            c.tailLength = orDef(e.tailLength, 0.0f);
            c.twinkleSpeed = orDef(e.twinkleSpeed, 0.0f);
            c.twinklePercent = orDef(e.twinklePercent, 1.0f);
            c.twinkleMin = orDef(e.twinkleMin, 1.0f);
            c.twinkleRange = orDef(e.twinkleMax - e.twinkleMin, 0.0f);
            c.burst = orDef(e.burst, 0.0f);
            c.drag = orDef(e.drag, 0.0f);
            c.baseSpin = orDef(e.baseSpin, 0.0f);
            c.baseSpinVary = orDef(e.baseSpinVary, 0.0f);
            c.spin = orDef(e.spin, 0.0f);
            c.spinVary = orDef(e.spinVary, 0.0f);
            c.wind[0] = orDef(e.wind.x, 0.0f);
            c.wind[1] = orDef(e.wind.y, 0.0f);
            c.wind[2] = orDef(e.wind.z, 0.0f);
            c.windTime = orDef(e.windTime, 0.0f);
            c.scaleVary[0] = orDef(e.scaleVary.x, 0.0f);
            c.scaleVary[1] = orDef(e.scaleVary.y, 0.0f);
            // BAKE-EM-3
            {
                float ds = e.followSpeed2 - e.followSpeed1;
                if (std::fabs(ds) >= 2.384185791015625e-7f) {
                    float slope = (e.followScale2 - e.followScale1) / ds;
                    float base = e.followScale1 - slope * e.followSpeed1;
                    c.followSlope = orDef(slope, 0.0f);
                    c.followBase = orDef(base, 0.0f);
                }
            }
            // BAKE-LIFE
            const m2raw::FBlock* blocks[5] = {&e.color, &e.alpha, &e.scale, &e.headCell, &e.tailCell};
            bool scaleHasKeys = false;
            for (int k = 0; k < 5; ++k) {
                LifeCurve lc = convertLife(*blocks[k], k);
                if (lc.ts.empty()) continue;
                if (k == kFxLifeScale) {
                    scaleHasKeys = true;
                    if (lut)  // BAKE-LIFE-5
                        for (size_t j = 0; j + 1 < lc.v.size(); j += 2) lc.v[j + 1] = lc.v[j];
                }
                c.lifeKeys[k] = static_cast<uint16_t>(lc.ts.size());
                c.lifeOffset[k] = out.pool.addLife(lc.ts, lc.v);
            }
            if (lut) c.scaleVary[0] = c.scaleVary[1] = 0.0f;
            // BAKE-STEP
            const m2raw::Track* base6[6] = {&e.rate, &e.life, &e.speed, &e.vRange, &e.hRange, &e.gravity};
            const m2raw::Track* l3t[5] = {&e.speedVar, &e.areaL, &e.areaW, &e.zSource, &e.enabledIn};
            uint16_t mask = 0;
            for (int k = 0; k < 6; ++k)
                if (held(m, *base6[k])) mask = static_cast<uint16_t>(mask | (1u << k));
            for (int k = 0; k < 5; ++k)
                if (held(m, *l3t[k])) mask = static_cast<uint16_t>(mask | (1u << (8 + k)));
            c.stepMask = mask;
            // BAKE-CC-1
            const float defs[5] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            std::vector<float> samples[5];
            for (int k = 0; k < 5; ++k) {
                samples[k].resize(static_cast<size_t>(frames));
                for (int f = 0; f < frames; ++f) {
                    float s = m.sampleF(*l3t[k], 0, tk[static_cast<size_t>(f)], defs[k]);
                    if (!fin(s)) s = defs[k];
                    if (k == kFxCurveZSource && std::fabs(s) < 0.001f) s = 0.0f;
                    if (k == kFxCurveEnabled) s = (s != 0.0f) ? 1.0f : 0.0f;
                    samples[k][static_cast<size_t>(f)] = s;
                }
                c.curveOffset[k] = addCurve(out.pool, samples[k], c.curveFrames[k]);
            }
            // BAKE-SPL
            if (e.type == 3 && !e.spline.empty()) {
                int n = static_cast<int>(std::min<size_t>(e.spline.size(), kFxMaxSplinePoints));
                n = n - ((n - 1) % 3);
                std::vector<float> data;
                for (int p = 0; p < n; ++p) {
                    const glm::vec3& q = e.spline[static_cast<size_t>(p)];
                    data.push_back(orDef(q.x, 0.0f));
                    data.push_back(orDef(q.y, 0.0f));
                    data.push_back(orDef(q.z, 0.0f));
                }
                int segs = ptcsim::splineSegments(n);
                if (segs >= 2) {
                    float lens[21];
                    ptcsim::splineLengths(data.data(), n, lens);
                    data.insert(data.end(), lens, lens + segs);
                    c.splineLens = static_cast<uint16_t>(segs);
                }
                c.splineCount = static_cast<uint16_t>(n);
                c.splineOffset = out.pool.add(data.data(), data.size());
            }
            // BAKE-REACH, level-3 term
            {
                const BaseCurves* bc = i < in.base.size() ? &in.base[i] : nullptr;
                float far = 0.0f;
                const float windLen = len3(glm::vec3(c.wind[0], c.wind[1], c.wind[2]));
                for (int f = 0; f < frames; ++f) {
                    float lifespan = bc ? baseAt(*bc, kFxCurveLife, f) : 0.0f;
                    float speed = bc ? baseAt(*bc, kFxCurveSpeed, f) : 0.0f;
                    float grav = bc ? baseAt(*bc, kFxCurveGravity, f) : 0.0f;
                    float life = clampf(lifespan + std::fabs(c.lifespanVary), 0.0f, 30.0f);
                    float spd = std::fabs(speed) * (1.0f + std::fabs(samples[kFxCurveSpeedVar][static_cast<size_t>(f)]));
                    float g = std::fabs(grav);
                    float aL = std::fabs(samples[kFxCurveAreaL][static_cast<size_t>(f)]);
                    float aW = std::fabs(samples[kFxCurveAreaW][static_cast<size_t>(f)]);
                    float area = e.type == 2 ? std::max(aL, aW) : 0.5f * std::sqrt(aL * aL + aW * aW);
                    float wind = windLen * std::min(c.windTime, life) * life;
                    float d = area + spd * life + 0.5f * g * life * life + wind;
                    if (d > far) far = d;
                }
                float size = 1.0f;
                if (scaleHasKeys) {
                    size = 0.0f;
                    for (float v : e.scale.v)
                        if (fin(v)) size = std::max(size, std::fabs(v));
                }
                size *= (1.0f + std::max(std::fabs(c.scaleVary[0]), std::fabs(c.scaleVary[1]))) *
                        std::max(1.0f, std::fabs(c.twinkleMin) + std::fabs(c.twinkleRange));
                glm::vec3 pos(orDef(e.position.x, 0.0f), orDef(e.position.y, 0.0f), orDef(e.position.z, 0.0f));
                float term = len3(restOf(in, e.bone, pos)) + std::min(far, 50.0f) + size;
                if (fin(term) && term > reach) reach = term;
            }
            out.emitters.push_back(c);
        }

        // ---- ribbons (section 6) ----
        for (size_t r = 0; r < m.ribbons.size() && static_cast<int>(out.keptRibbons.size()) < in.maxRibbons; ++r)
            if (m.ribbons[r].bone != 0xFFFFFFFFu) out.keptRibbons.push_back(static_cast<int>(r));  // BAKE-SEL

        for (int ri : out.keptRibbons) {
            const m2raw::Ribbon& rb = m.ribbons[static_cast<size_t>(ri)];
            // BAKE-RIB-1
            int bone = static_cast<int>(rb.bone & 0xFFFFu);
            if (static_cast<uint32_t>(bone) >= m.boneCount) bone = m.boneCount > 0 ? 0 : -1;
            uint16_t track = (bone >= 0 && in.trackOfBone) ? in.trackOfBone(bone) : kFxNone;
            // BAKE-RIB-2
            uint16_t texture = (!rb.textures.empty() && in.textureOf) ? in.textureOf(rb.textures[0]) : kFxNone;
            uint32_t matBlend = 0;
            bool haveMat = !rb.materials.empty() && rb.materials[0] < m.materials.size();
            if (haveMat) matBlend = m.materials[rb.materials[0]].blend;
            const uint8_t blend = static_cast<uint8_t>(matBlend > 6 ? 6 : matBlend);
            // BAKE-RIB-3
            SpellFxL3Ribbon q;
            std::memset(&q, 0, sizeof q);
            q.blend = blend;
            q.rows = static_cast<uint8_t>(rb.rows < 1 ? 1 : (rb.rows > 255 ? 255 : rb.rows));
            q.cols = static_cast<uint8_t>(rb.cols < 1 ? 1 : (rb.cols > 255 ? 255 : rb.cols));
            q.edgesPerSecond = fin(rb.edgesPerSecond) ? clampf(rb.edgesPerSecond, 0.0f, 1000.0f) : 15.0f;
            q.edgeLifetime = fin(rb.edgeLifetime) ? clampf(rb.edgeLifetime, 0.25f, 100.0f) : 0.5f;
            q.gravity = fin(rb.gravity) ? clampf(rb.gravity, -1e5f, 1e5f) : 0.0f;
            std::vector<float> cv[8];
            float maxH = 0.0f;
            for (int k = 0; k < 8; ++k) cv[k].resize(static_cast<size_t>(frames));
            for (int f = 0; f < frames; ++f) {
                const float t = tk[static_cast<size_t>(f)];
                float col[3] = {1.0f, 1.0f, 1.0f};
                if (!m.sample(rb.color, 0, t, col, 3)) col[0] = col[1] = col[2] = 1.0f;
                for (int ch = 0; ch < 3; ++ch) cv[ch][static_cast<size_t>(f)] = clampf(orDef(col[ch], 1.0f), 0.0f, 1.0f);
                cv[kFxRibbonAlpha][static_cast<size_t>(f)] = clampf(orDef(m.sampleF(rb.alpha, 0, t, 1.0f), 1.0f), 0.0f, 1.0f);
                float ab = clampf(orDef(m.sampleF(rb.above, 0, t, 0.0f), 0.0f), -100.0f, 100.0f);
                float be = clampf(orDef(m.sampleF(rb.below, 0, t, 0.0f), 0.0f), -100.0f, 100.0f);
                cv[kFxRibbonAbove][static_cast<size_t>(f)] = ab;
                cv[kFxRibbonBelow][static_cast<size_t>(f)] = be;
                maxH = std::max(maxH, std::max(std::fabs(ab), std::fabs(be)));
                float vis = orDef(m.sampleF(rb.visibility, 0, t, 1.0f), 1.0f);
                cv[kFxRibbonVisible][static_cast<size_t>(f)] = vis != 0.0f ? 1.0f : 0.0f;
                cv[kFxRibbonSlot][static_cast<size_t>(f)] = clampf(orDef(m.sampleF(rb.texSlot, 0, t, 0.0f), 0.0f), 0.0f, 65535.0f);
            }
            for (int k = 0; k < 8; ++k) q.curveOffset[k] = addCurve(out.pool, cv[k], q.curveFrames[k]);
            out.ribbons.push_back(q);

            // BAKE-RIB-4 (v2 fields)
            SpellFxRibbon v2;
            std::memset(&v2, 0, sizeof v2);
            v2.track = track;
            v2.texture = texture;
            v2.blend = (matBlend == 3 || matBlend == 4) ? kFxBlendAdd : kFxBlendAlpha;
            v2.flags = 0;
            v2.position[0] = orDef(rb.position.x, 0.0f);
            v2.position[1] = orDef(rb.position.y, 0.0f);
            v2.position[2] = orDef(rb.position.z, 0.0f);
            float c0[3] = {1.0f, 1.0f, 1.0f};
            if (!m.sample(rb.color, 0, 0.0f, c0, 3)) c0[0] = c0[1] = c0[2] = 1.0f;
            float a0 = m.sampleF(rb.alpha, 0, 0.0f, 1.0f);
            v2.color = pack(clampf(orDef(c0[0], 1.0f), 0.0f, 1.0f), clampf(orDef(c0[1], 1.0f), 0.0f, 1.0f),
                            clampf(orDef(c0[2], 1.0f), 0.0f, 1.0f), clampf(orDef(a0, 1.0f), 0.0f, 1.0f));
            v2.heightAbove = clampf(orDef(m.sampleF(rb.above, 0, 0.0f, 0.0f), 0.0f), -100.0f, 100.0f);
            v2.heightBelow = clampf(orDef(m.sampleF(rb.below, 0, 0.0f, 0.0f), 0.0f), -100.0f, 100.0f);
            v2.visibility = orDef(m.sampleF(rb.visibility, 0, 0.0f, 1.0f), 1.0f);
            v2.edgesPerSecond = q.edgesPerSecond;
            v2.edgeLifetime = q.edgeLifetime;
            v2.gravity = q.gravity;
            out.ribbonsV2.push_back(v2);
            // ribbon reach term
            glm::vec3 pos(v2.position[0], v2.position[1], v2.position[2]);
            float term = len3(restOf(in, bone, pos)) + maxH + std::fabs(q.gravity) * q.edgeLifetime * q.edgeLifetime + 2.0f;
            if (fin(term) && term > reach) reach = term;
        }

        // BAKE-VER
        const size_t content = out.emitters.size() + out.ribbons.size();
        out.writeSection = in.forceV2 || !spellOnly || content > 0;
    }

    if (!out.writeSection) {
        // no level-3 section: none of its records, ribbons or reach terms are used
        out.emitters.clear();
        out.ribbons.clear();
        out.ribbonsV2.clear();
        out.keptRibbons.clear();
        out.pool.b.clear();
        reach = existing;
    }
    const bool hasRibbons = !out.keptRibbons.empty() || (!out.writeSection && in.upstreamRibbons);
    out.version = (in.forceV2 || hasRibbons || !spellOnly || out.writeSection) ? kSpellFxVersion2 : kSpellFxVersion;
    out.extFlags = out.writeSection ? kFxExtLevel3 : 0;
    if (in.cls & kPtcSmoke) reach = std::max(reach, 12.0f);
    out.reach = clampf(reach, 0.5f, 60.0f);
    return out.writeSection;
}

void relocate(Output& out, uint32_t poolBlobOffset) {
    for (SpellFxL3Emitter& c : out.emitters) {
        for (int k = 0; k < 5; ++k) {
            if (c.lifeKeys[k] > 0) c.lifeOffset[k] += poolBlobOffset;
            if (c.curveFrames[k] > 0) c.curveOffset[k] += poolBlobOffset;
        }
        if (c.splineCount > 0) c.splineOffset += poolBlobOffset;
    }
    for (SpellFxL3Ribbon& q : out.ribbons)
        for (int k = 0; k < 8; ++k)
            if (q.curveFrames[k] > 0) q.curveOffset[k] += poolBlobOffset;
}

}  // namespace l3bake
}  // namespace pspconv
}  // namespace wowee
