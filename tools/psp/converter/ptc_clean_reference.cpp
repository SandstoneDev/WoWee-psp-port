// Reference side of the particle matrix tool - clean-room implementation of particle-matrix-tool.md TOOL-1..4.
#include "ptc_clean_reference.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace wowee {
namespace pspconv {
namespace ptcref {

using namespace rendering::psp;
using ptcsim::EmitterValues;
using ptcsim::Particle;

namespace {
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float len3(const glm::vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
const float kIdentityAxes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
}  // namespace

// ---- ReferenceSim (TOOL-3) -------------------------------------------------------------------------------------
bool ReferenceSim::init(const m2raw::Model& raw, const std::vector<int>& kept, BoneSampler bones, uint32_t periodMs) {
    raw_ = &raw;
    bones_ = std::move(bones);
    periodMs_ = periodMs ? periodMs : 3333u;
    em_.clear();
    parts_.clear();
    prevOrigin_ = lastDisp_ = inherit_ = glm::vec3(0.0f);
    clock_ = 0.0f;
    valid_ = false;
    // The level-3 statics and life curves of the kept emitters, built in memory with the bake's conversion rules
    // (BAKE-EM, BAKE-LIFE, BAKE-SPL). Its sampled curves are not used: the reference samples the raw tracks.
    l3bake::Input in;
    in.raw = &raw;
    in.rawParsed = true;
    in.kept = kept;
    in.frames = 2;
    in.periodMs = periodMs_;
    in.cls = kPtcFlame;  // any non-spell class: the records are always produced
    in.forceV2 = true;
    in.level3 = true;
    in.maxRibbons = 0;
    l3bake::buildLevel3Section(in, rec_);
    if (rec_.emitters.size() != kept.size()) return false;
    for (size_t i = 0; i < kept.size(); ++i) {
        const m2raw::Emitter& e = raw.emitters[static_cast<size_t>(kept[i])];
        Em m;
        m.m2Index = kept[i];
        m.cols = static_cast<uint16_t>(e.cols < 1 ? 1 : (e.cols > 255 ? 255 : e.cols));
        m.rows = static_cast<uint16_t>(e.rows < 1 ? 1 : (e.rows > 255 ? 255 : e.rows));
        m.cells = static_cast<uint32_t>(m.cols) * m.rows;
        m.rng.seed(toolSeed(static_cast<int>(i)));
        m.acc = 0.0f;
        m.fired = false;
        m.births = 0;
        m.life = m.gravity = 0.0f;
        m.origin = glm::vec3(0.0f);
        em_.push_back(m);
    }
    return true;
}

EmitterValues ReferenceSim::valuesAt(int i, float tMs) const {
    const m2raw::Emitter& e = raw_->emitters[static_cast<size_t>(em_[static_cast<size_t>(i)].m2Index)];
    const float tau = std::fmod(tMs, static_cast<float>(periodMs_));
    auto s = [&](const m2raw::Track& t, float def) {
        float v = raw_->sampleF(t, 0, tau, def);
        return fxFinite(v) ? v : def;
    };
    EmitterValues v;
    v.rate = s(e.rate, 0.0f);
    v.life = s(e.life, 0.0f);
    v.speed = s(e.speed, 0.0f);
    v.vRange = s(e.vRange, 0.0f);
    v.hRange = s(e.hRange, 0.0f);
    v.gravity = s(e.gravity, 0.0f);
    v.speedVar = s(e.speedVar, 0.0f);
    v.areaL = s(e.areaL, 0.0f);
    v.areaW = s(e.areaW, 0.0f);
    v.zSource = s(e.zSource, 0.0f);
    if (std::fabs(v.zSource) < 0.001f) v.zSource = 0.0f;  // the level-3 data's definition (BAKE-CC-1)
    v.enabled = s(e.enabledIn, 1.0f) != 0.0f ? 1.0f : 0.0f;
    return v;
}

void ReferenceSim::frameAt(int i, float tMs, M34& F) const {
    const m2raw::Emitter& e = raw_->emitters[static_cast<size_t>(em_[static_cast<size_t>(i)].m2Index)];
    const float tau = std::fmod(tMs, static_cast<float>(periodMs_));
    M34 bone, mb;
    ptcsim::identity(bone);
    if (bones_) bones_(static_cast<int>(e.bone), tau, bone);
    ptcsim::compose(kIdentityAxes, glm::vec3(0.0f), bone, mb);  // the instance is at the origin, identity axes
    const float pos[3] = {e.position.x, e.position.y, e.position.z};
    ptcsim::emitterFrame(mb, pos, F);
}

void ReferenceSim::step(float timeMs, float dt) {
    if (!fxFinite(dt) || dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    int parts = dt > 0.1f ? static_cast<int>(std::ceil(dt / 0.1f)) : 1;
    float h = dt / static_cast<float>(parts);
    for (int p = 0; p < parts; ++p) subStep(timeMs, h);
}

void ReferenceSim::subStep(float timeMs, float dt) {
    // RT-EMIT-10 for the still instance at the origin
    const glm::vec3 origin(0.0f);
    glm::vec3 D = valid_ ? origin - prevOrigin_ : glm::vec3(0.0f);
    clock_ += dt;
    if (clock_ > 1.0f / 30.0f) {
        inherit_ = D / (clock_ * 29.999998f);
        clock_ = 0.0f;
    }
    const float throttle = 1.0f;  // camera 10 yards away (TOOL-1); distanceThrottle(10) = 1
    const uint8_t* base = rec_.pool.b.data();
    for (size_t i = 0; i < em_.size(); ++i) {
        Em& m = em_[i];
        const SpellFxL3Emitter& c = rec_.emitters[i];
        const EmitterValues v = valuesAt(static_cast<int>(i), timeMs);
        M34 F;
        frameAt(static_cast<int>(i), timeMs, F);
        m.life = v.life;
        m.gravity = v.gravity;
        m.origin = glm::vec3(F.t[0], F.t[1], F.t[2]);
        // RT-EMIT-2..7, the engine's order of draws (rate variate, then per birth: path variate, birth)
        const bool burstOnce = (c.flags & kM2PtcBurstOnce) != 0;
        bool burst = false;
        float baseRate;
        if (burstOnce) {
            if (v.rate > 0.0f) {
                if (!m.fired) {
                    burst = true;
                    m.fired = true;
                }
            } else {
                m.fired = false;
            }
            baseRate = v.rate;
        } else {
            baseRate = v.enabled >= 0.5f ? (v.rate > 0.0f ? v.rate : 0.0f) : 0.0f;
        }
        if (c.type == 3) baseRate *= clampf(v.areaW, 0.0f, 1.0f);
        float rate = (baseRate + ptcsim::drawS(m.rng) * c.rateVary) * 1.0f * throttle;
        int count = 0;
        if (burstOnce) {
            if (burst && rate > 0.0f) count = static_cast<int>(rate);
        } else {  // no budgets: the cap limit of RT-EMIT-6 does not apply
            m.acc += rate * dt;
            float a = m.acc + 0.5f;
            count = a > 0.0f ? static_cast<int>(a) : 0;
        }
        if (count <= 0) {
            if (m.acc > 2.0f) m.acc = 0.0f;
            continue;
        }
        const float* spline = c.splineCount > 0 ? reinterpret_cast<const float*>(base + c.splineOffset) : nullptr;
        const glm::vec3 inh = inherit_ * c.burst;
        for (int b = 0; b < count; ++b) {
            Part np;
            std::memset(&np, 0, sizeof np);
            M34 Fb = F;
            if ((c.flags & kM2PtcAlongPath) && !burst) {  // the U draw is consumed; the still instance has D = 0
                float u = ptcsim::drawU(m.rng);
                for (int k = 0; k < 3; ++k) Fb.t[k] = F.t[k] - D[k] * (1.0f - u);
            }
            ptcsim::spawn(c, v, spline, Fb, inh, m.rng, burst ? 0.0f : dt, np.p);
            np.p.owner = 0;
            np.p.emitter = static_cast<uint8_t>(i);
            np.serial = serialOf(static_cast<int>(i), m.births++);
            parts_.push_back(np);
        }
        if (!burstOnce) {
            m.acc -= static_cast<float>(count);
            if (m.acc > 2.0f) m.acc = 0.0f;
        }
    }
    prevOrigin_ = origin;
    lastDisp_ = D;
    valid_ = true;
    // RT-STEP-3..6
    size_t w = 0;
    for (size_t r = 0; r < parts_.size(); ++r) {
        Part& q = parts_[r];
        Particle& p = q.p;
        const Em& m = em_[p.emitter];
        const SpellFxL3Emitter& c = rec_.emitters[p.emitter];
        p.life += dt;
        float span = m.life + p.maxLife;
        if (span < 0.001f) span = 0.001f;
        if (p.life >= span) continue;
        glm::vec3 org = (c.flags & kM2PtcLocal) ? glm::vec3(0.0f) : m.origin;
        glm::vec3 follow(0.0f);
        if (c.flags & kM2PtcFollow) {
            float speed = dt > 0.0f ? len3(lastDisp_) / dt : 0.0f;
            follow = lastDisp_ * clampf(c.followBase + c.followSlope * speed, 0.0f, 1.0f);
        }
        EmitterValues v;
        std::memset(&v, 0, sizeof v);
        v.gravity = m.gravity;
        if (!ptcsim::update(c, v, org, follow, dt, p)) continue;
        parts_[w++] = q;
    }
    parts_.resize(w);
}

void ReferenceSim::snapshot(float timeMs, std::vector<SimParticle>& out) const {
    out.clear();
    const uint8_t* base = rec_.pool.b.data();
    std::vector<EmitterValues> vals(em_.size());
    std::vector<M34> frames(em_.size());
    for (size_t i = 0; i < em_.size(); ++i) {
        vals[i] = valuesAt(static_cast<int>(i), timeMs);
        frameAt(static_cast<int>(i), timeMs, frames[i]);
    }
    for (const Part& q : parts_) {
        const Particle& p = q.p;
        const SpellFxL3Emitter& c = rec_.emitters[p.emitter];
        const M34& F = frames[p.emitter];
        SimParticle s;
        s.serial = q.serial;
        s.emitter = p.emitter;
        const bool local = (c.flags & kM2PtcLocal) != 0;
        s.pos = local ? ptcsim::apply(F, p.pos) : p.pos;
        s.vel = local ? ptcsim::rotate(F, p.vel) : p.vel;
        s.age = p.life;
        ptcsim::Look L;
        s.visible = ptcsim::look(base, c, vals[p.emitter], q.serial, em_[p.emitter].cells, 1.0f, 1.0f, p, L);
        for (int k = 0; k < 2; ++k) s.half[k] = s.visible ? L.half[k] : 0.0f;
        for (int k = 0; k < 4; ++k) s.rgba[k] = s.visible ? L.rgba[k] : 0.0f;
        out.push_back(s);
    }
}

// ---- EngineSide (TOOL-2) ---------------------------------------------------------------------------------------
bool EngineSide::init(const FxView& fx) {
    if (!fx.h || !fx.level3) return false;
    fx_ = &fx;
    eng_.reset(new ParticleEngine());
    FxPoolConfig cfg;
    cfg.spellShare = 0;
    cfg.worldShare = 60000;  // no budgets (TOOL-2): a pool larger than any model needs
    cfg.weaponShare = 0;
    if (!eng_->acquire(cfg)) return false;
    serial_.assign(static_cast<size_t>(eng_->capacity()), 0u);
    for (int e = 0; e < 16; ++e) {
        rngs_[e].seed(toolSeed(e));
        births_[e] = 0;
        acc_[e] = 0.0f;
    }
    eng_->setEmitterRngs(rngs_);
    eng_->setBirthObserver(&EngineSide::onBirth, this);
    inst_ = FxInstance();
    inst_.fx = fx_;
    inst_.owner = 0;
    inst_.acc = acc_;
    inst_.cap = INT_MAX / 4;  // no per-emitter cap
    inst_.rate = 1.0f;
    inst_.camDist = 10.0f;
    inst_.alpha = 1.0f;
    return true;
}

void EngineSide::onBirth(void* user, int slot, uint8_t, uint8_t emitter) {
    EngineSide* self = static_cast<EngineSide*>(user);
    self->serial_[static_cast<size_t>(slot)] = serialOf(emitter, self->births_[emitter]++);
}

void EngineSide::step(float timeMs, float dt) {
    inst_.timeMs = timeMs;
    FxFrameStats st;
    eng_->step(&inst_, 1, 0, 1, eng_->capacity(), dt, st);
}

void EngineSide::snapshot(float timeMs, std::vector<SimParticle>& out) const {
    out.clear();
    FxInstance I = inst_;
    I.timeMs = timeMs;
    const int ne = std::min<int>(fx_->h->emitterCount, 16);
    EmitterValues vals[16];
    M34 frames[16];
    uint32_t cells[16];
    for (int e = 0; e < ne; ++e) {
        vals[e] = fxEmitterValuesAt(*fx_, e, timeMs);
        fxEmitterFrameAt(I, e, frames[e]);
        const SpellFxEmitter& em = fx_->emitters[e];
        cells[e] = static_cast<uint32_t>(em.cols ? em.cols : 1) * static_cast<uint32_t>(em.rows ? em.rows : 1);
    }
    const Particle* pool = eng_->particles();
    for (int s = 0; s < eng_->capacity(); ++s) {
        const Particle& p = pool[s];
        if (p.owner != 0 || p.emitter >= ne) continue;
        const SpellFxL3Emitter& c = fx_->level3[p.emitter];
        const M34& F = frames[p.emitter];
        SimParticle sp;
        sp.serial = serial_[static_cast<size_t>(s)];
        sp.emitter = p.emitter;
        const bool local = (c.flags & kM2PtcLocal) != 0;
        sp.pos = local ? ptcsim::apply(F, p.pos) : p.pos;
        sp.vel = local ? ptcsim::rotate(F, p.vel) : p.vel;
        sp.age = p.life;
        ptcsim::Look L;
        sp.visible = ptcsim::look(fx_->base, c, vals[p.emitter], sp.serial, cells[p.emitter], 1.0f, 1.0f, p, L);
        for (int k = 0; k < 2; ++k) sp.half[k] = sp.visible ? L.half[k] : 0.0f;
        for (int k = 0; k < 4; ++k) sp.rgba[k] = sp.visible ? L.rgba[k] : 0.0f;
        out.push_back(sp);
    }
}

// ---- comparison (TOOL-4) ---------------------------------------------------------------------------------------
void compareStep(const std::vector<SimParticle>& engine, const std::vector<SimParticle>& reference, CompareStats& st) {
    ++st.steps;
    std::vector<const SimParticle*> a, b;
    for (const SimParticle& s : engine) a.push_back(&s);
    for (const SimParticle& s : reference) b.push_back(&s);
    auto bySerial = [](const SimParticle* x, const SimParticle* y) { return x->serial < y->serial; };
    std::sort(a.begin(), a.end(), bySerial);
    std::sort(b.begin(), b.end(), bySerial);
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j >= b.size() || (i < a.size() && a[i]->serial < b[j]->serial)) {
            ++st.engineOnly;
            ++i;
            continue;
        }
        if (i >= a.size() || b[j]->serial < a[i]->serial) {
            ++st.refOnly;
            ++j;
            continue;
        }
        const SimParticle& x = *a[i++];
        const SimParticle& y = *b[j++];
        ++st.matched;
        double pe = len3(x.pos - y.pos), ve = len3(x.vel - y.vel);
        st.posErrSum += pe;
        st.posErrMax = std::max(st.posErrMax, pe);
        st.velErrSum += ve;
        st.velErrMax = std::max(st.velErrMax, ve);
        if (x.visible != y.visible) {
            ++st.visibilityMismatch;
        } else if (x.visible) {
            ++st.lookPairs;
            double he = std::max(std::fabs(x.half[0] - y.half[0]), std::fabs(x.half[1] - y.half[1]));
            double ce = 0;
            for (int k = 0; k < 4; ++k) ce = std::max(ce, static_cast<double>(std::fabs(x.rgba[k] - y.rgba[k])));
            st.halfErrSum += he;
            st.halfErrMax = std::max(st.halfErrMax, he);
            st.colErrSum += ce;
            st.colErrMax = std::max(st.colErrMax, ce);
        }
    }
}

bool runCompare(const m2raw::Model& raw, const std::vector<int>& kept, const BoneSampler& bones, uint32_t periodMs,
                const FxView& blob, const RunConfig& cfg, CompareStats& st, const StepHook& perStep) {
    EngineSide eng;
    ReferenceSim ref;
    if (!eng.init(blob) || !ref.init(raw, kept, bones, periodMs)) return false;
    const float fps = cfg.fps > 0.0f ? cfg.fps : 30.0f;
    const float dt = 1.0f / fps;
    const int steps = static_cast<int>(std::lround(cfg.seconds * fps));
    std::vector<SimParticle> pe, pr;
    for (int k = 0; k < steps; ++k) {
        const float tMs = 1000.0f * dt * static_cast<float>(k + 1);
        eng.step(tMs, dt);
        ref.step(tMs, dt);
        eng.snapshot(tMs, pe);
        ref.snapshot(tMs, pr);
        compareStep(pe, pr, st);
        if (perStep) perStep(k, tMs, pe, pr);
    }
    return true;
}

}  // namespace ptcref
}  // namespace pspconv
}  // namespace wowee
