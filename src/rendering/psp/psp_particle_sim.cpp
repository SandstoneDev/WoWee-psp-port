// Portable particle rules - clean-room implementation of cleanroom/specs/particles/particle-sim.md.
// EBOOT-safe: no exceptions, no RTTI, no heap, single-precision float math.
#include "rendering/psp/psp_particle_sim.hpp"

#include <cmath>

namespace wowee {
namespace rendering {
namespace psp {
namespace ptcsim {

namespace {

bool g_fixedDraw = false;

constexpr float kFixedU = 0.75f;
constexpr float kFixedS = 0.5f;
constexpr uint32_t kFixedN = 3221225472u;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline float len3(const glm::vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// SIM-TW-1: the first 128 u01() outputs of a generator seeded with 0x5A17 (SIM-RNG-3), built at compile time.
struct TwinkleTable {
    float v[128];
    constexpr TwinkleTable() : v() {
        uint32_t s = (0x5A17u * 2654435761u) ^ 0x9E3779B9u;
        if (s == 0) s = 1;
        for (int i = 0; i < 128; ++i) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            v[i] = static_cast<float>(s >> 8) / 16777216.0f;
        }
    }
};
constexpr TwinkleTable kTwinkle{};

// The particle's own look variates are derived from its 16-bit `aux` and a per-role salt by an integer hash, so
// that they are fixed for the particle (every evaluation gives the same values) and independent between roles.
enum LookRole : uint32_t { kRoleCell = 1, kRoleScale = 2, kRoleScaleX = 3, kRoleScaleY = 4, kRoleBase = 5, kRoleRate = 6 };

inline uint32_t lookHash(uint16_t aux, uint32_t role) {
    uint32_t h = static_cast<uint32_t>(aux) * 0x9E3779B1u + role * 0x85EBCA6Bu + 0x68E31DA4u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
inline uint32_t lookN(uint16_t aux, uint32_t role) { return g_fixedDraw ? kFixedN : lookHash(aux, role); }
inline float lookU(uint16_t aux, uint32_t role) {
    return g_fixedDraw ? kFixedU : static_cast<float>(lookHash(aux, role) >> 8) * (1.0f / 16777216.0f);
}
inline float lookS(uint16_t aux, uint32_t role) { return g_fixedDraw ? kFixedS : 2.0f * lookU(aux, role) - 1.0f; }

// SIM-UPD-5
inline float snap1(float x) {
    float a = x < 0.0f ? -x : x;
    return (a > 0.0f && a < 1e-8f) ? 0.0f : x;
}
inline glm::vec3 snap(const glm::vec3& v) { return glm::vec3(snap1(v.x), snap1(v.y), snap1(v.z)); }

inline bool finite3(const glm::vec3& v) { return fxFinite(v.x) && fxFinite(v.y) && fxFinite(v.z); }

// derivative of a cubic Bernstein segment (SIM-SPL-2)
inline glm::vec3 bezierTangent(const float* p, float u) {
    float a = 1.0f - u;
    float d0 = -3.0f * a * a;
    float d1 = 3.0f * a * a - 6.0f * u * a;
    float d2 = 6.0f * u * a - 3.0f * u * u;
    float d3 = 3.0f * u * u;
    return glm::vec3(d0 * p[0] + d1 * p[3] + d2 * p[6] + d3 * p[9], d0 * p[1] + d1 * p[4] + d2 * p[7] + d3 * p[10],
                     d0 * p[2] + d1 * p[5] + d2 * p[8] + d3 * p[11]);
}

}  // namespace

// ---- SIM-MAT ---------------------------------------------------------------------------------------------------
void identity(M34& m) {
    for (int i = 0; i < 9; ++i) m.l[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    m.t[0] = m.t[1] = m.t[2] = 0.0f;
}

glm::vec3 rotate(const M34& m, const glm::vec3& d) {
    return glm::vec3(m.l[0] * d.x + m.l[3] * d.y + m.l[6] * d.z, m.l[1] * d.x + m.l[4] * d.y + m.l[7] * d.z,
                     m.l[2] * d.x + m.l[5] * d.y + m.l[8] * d.z);
}

glm::vec3 apply(const M34& m, const glm::vec3& p) {
    glm::vec3 r = rotate(m, p);
    return glm::vec3(r.x + m.t[0], r.y + m.t[1], r.z + m.t[2]);
}

void compose(const float axes[9], const glm::vec3& origin, const M34& b, M34& out) {
    M34 r;
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            r.l[3 * c + row] = axes[row] * b.l[3 * c] + axes[3 + row] * b.l[3 * c + 1] + axes[6 + row] * b.l[3 * c + 2];
    for (int row = 0; row < 3; ++row)
        r.t[row] = axes[row] * b.t[0] + axes[3 + row] * b.t[1] + axes[6 + row] * b.t[2] + origin[row];
    out = r;
}

void emitterFrame(const M34& mb, const float position[3], M34& out) {
    M34 r;
    glm::vec3 t = apply(mb, glm::vec3(position[0], position[1], position[2]));
    r.l[0] = mb.l[3];
    r.l[1] = mb.l[4];
    r.l[2] = mb.l[5];
    r.l[3] = -mb.l[0];
    r.l[4] = -mb.l[1];
    r.l[5] = -mb.l[2];
    r.l[6] = mb.l[6];
    r.l[7] = mb.l[7];
    r.l[8] = mb.l[8];
    r.t[0] = t.x;
    r.t[1] = t.y;
    r.t[2] = t.z;
    out = r;
}

// ---- SIM-RNG ---------------------------------------------------------------------------------------------------
void Rng::seed(uint32_t v) {
    s = (v * 2654435761u) ^ 0x9E3779B9u;
    if (s == 0) s = 1;
}
uint32_t Rng::next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
float Rng::u01() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
float Rng::sym() { return 2.0f * u01() - 1.0f; }

// ---- SIM-FIX ---------------------------------------------------------------------------------------------------
void setFixedDraw(bool on) { g_fixedDraw = on; }
bool fixedDraw() { return g_fixedDraw; }
float drawU(Rng& r) { return g_fixedDraw ? kFixedU : r.u01(); }
float drawS(Rng& r) { return g_fixedDraw ? kFixedS : r.sym(); }
uint32_t drawN(Rng& r) { return g_fixedDraw ? kFixedN : r.next(); }

// ---- SIM-SPL ---------------------------------------------------------------------------------------------------
glm::vec3 splinePoint(const float* pts, int n, const float* lens, float t, glm::vec3* tangent) {
    if (!pts || n <= 0) {
        if (tangent) *tangent = glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::vec3(0.0f);
    }
    glm::vec3 p0(pts[0], pts[1], pts[2]);
    int m = splineSegments(n);
    if (m < 1) {
        if (tangent) *tangent = glm::vec3(0.0f, 0.0f, 1.0f);
        return p0;
    }
    float measured[21];
    if (!lens) {
        splineLengths(pts, n, measured);
        lens = measured;
    }
    float c = clampf(t, 0.0f, 1.0f);
    int k = 0;
    float u = c;
    if (m >= 2) {
        float total = 0.0f;
        for (int i = 0; i < m; ++i) total += lens[i];
        float target = c * total;
        float ck = 0.0f;
        for (k = 0; k < m - 1; ++k) {
            float nxt = ck + lens[k];
            if (nxt > target) break;
            ck = nxt;
        }
        u = lens[k] > 0.0f ? (target - ck) / lens[k] : 0.0f;
    }
    const float* seg = pts + 9 * k;
    if (tangent) *tangent = bezierTangent(seg, u);
    if (t <= 0.0f) return p0;
    if (t >= 1.0f) return glm::vec3(pts[3 * (n - 1)], pts[3 * (n - 1) + 1], pts[3 * (n - 1) + 2]);
    return bezier(seg, u);
}

// ---- SIM-SPAWN -------------------------------------------------------------------------------------------------
void spawn(const SpellFxL3Emitter& c, const EmitterValues& v, const float* spline, const M34& frame,
           const glm::vec3& inherit, Rng& rng, float subFrame, Particle& p) {
    const float sigma = (1.0f + drawS(rng) * v.speedVar) * v.speed;
    p.life = drawU(rng) * subFrame;
    p.maxLife = (c.lifespanVary != 0.0f) ? drawS(rng) * c.lifespanVary : 0.0f;
    p.aux = static_cast<uint16_t>(drawN(rng) >> 16);

    glm::vec3 P(0.0f), V(0.0f);
    const glm::vec3 Z(0.0f, 0.0f, v.zSource);
    const bool radial = v.zSource != 0.0f;

    if (c.type == 2) {  // SIM-SPAWN-SPHERE
        float uRadius = drawU(rng), sElev = drawS(rng), sAzim = drawS(rng);
        float rho = v.areaL + uRadius * (v.areaW - v.areaL);
        float th = sElev * v.vRange, ph = sAzim * v.hRange;
        float ct = std::cos(th);
        glm::vec3 nrm(std::cos(ph) * ct, std::sin(ph) * ct, std::sin(th));
        P = rho * nrm;
        if (!radial) {
            V = (c.flags & kM2PtcSphereUp) ? glm::vec3(0.0f, 0.0f, sigma) : sigma * nrm;
        } else {
            glm::vec3 D = P - Z;
            float d2 = D.x * D.x + D.y * D.y + D.z * D.z;
            glm::vec3 dir = d2 > 2.384185791015625e-7f ? D / std::sqrt(d2) : D;
            V = sigma * dir;
        }
    } else if (c.type == 3) {  // SIM-SPAWN-SPLINE
        float t0 = clampf(v.areaL, 0.0f, 1.0f), t1 = clampf(v.areaW, 0.0f, 1.0f);
        float t = t0 + drawU(rng) * (t1 - t0);
        int n = spline ? static_cast<int>(c.splineCount) : 0;
        const float* lens = nullptr;
        if (n > 0 && c.splineLens == splineSegments(n)) lens = spline + 3 * n;
        glm::vec3 T;
        P = splinePoint(spline, n, lens, t, &T);
        if (!radial) {
            if (v.vRange != 0.0f) {
                float tl = len3(T);
                glm::vec3 k = tl > 1e-6f ? T / tl : glm::vec3(0.0f, 0.0f, 1.0f);
                float al = drawS(rng) * v.vRange;
                float ca = std::cos(al), sa = std::sin(al), oc = 1.0f - ca;
                glm::vec3 d(k.x * k.z * oc + k.y * sa, k.y * k.z * oc - k.x * sa, k.z * k.z * oc + ca);
                if (v.hRange != 0.0f) P += d * (v.hRange * drawU(rng));
                V = sigma * d;
            } else {
                V = glm::vec3(0.0f);
            }
        } else {
            glm::vec3 D = P - Z;
            float dl = len3(D);
            V = dl > 1e-6f ? sigma * (D / dl) : D;
        }
    } else {  // SIM-SPAWN-PLANE (any other type)
        float sx = drawS(rng), sy = drawS(rng);
        P = glm::vec3(sx * v.areaL * 0.5f, sy * v.areaW * 0.5f, 0.0f);
        if (!radial) {
            float th = drawS(rng) * v.vRange, ph = drawS(rng) * v.hRange;
            float st = std::sin(th);
            V = sigma * glm::vec3(std::cos(ph) * st, std::sin(ph) * st, std::cos(th));
        } else {
            glm::vec3 D = P - Z;
            float dl = len3(D);
            V = dl > 1e-6f ? sigma * (D / dl) : D;
        }
    }

    // SIM-SPAWN-XFORM
    if (c.flags & kM2PtcLocal) {
        p.pos = P;
        p.vel = V;
    } else {
        p.pos = apply(frame, P);
        p.vel = rotate(frame, V);
    }
    // SIM-SPAWN-INHERIT
    if (c.flags & kM2PtcInheritVelocity) p.vel += inherit * (1.0f + drawS(rng) * v.speedVar);
    // SIM-SPAWN-GUARD
    if (!finite3(p.pos)) p.pos = glm::vec3(frame.t[0], frame.t[1], frame.t[2]);
    if (!finite3(p.vel)) p.vel = glm::vec3(0.0f);
}

// ---- SIM-LIFE --------------------------------------------------------------------------------------------------
float lifespanOf(const EmitterValues& v, const Particle& p) {
    float l = v.life + p.maxLife;
    return l > 0.001f ? l : 0.001f;
}

// ---- SIM-UPD ---------------------------------------------------------------------------------------------------
bool update(const SpellFxL3Emitter& c, const EmitterValues& v, const glm::vec3& origin, const glm::vec3& followStep,
            float dt, Particle& p) {
    const float age = p.life;
    const glm::vec3 p0 = p.pos, v0 = p.vel;
    glm::vec3 v1 = v0;
    if (age < c.windTime) v1 = snap(v0 + glm::vec3(c.wind[0], c.wind[1], c.wind[2]) * dt);
    glm::vec3 f(0.0f);
    if ((c.flags & kM2PtcFollow) && 2.0f * dt < age) f = followStep;
    const float g = v.gravity;
    glm::vec3 p1 = p0 + f + v1 * dt;
    p1.z -= g * dt * dt * 0.5f;
    glm::vec3 v2 = v1;
    v2.z -= g * dt;
    glm::vec3 v3 = v2;
    if (c.drag != 0.0f) {
        float k = dt * c.drag;
        if (k > 1.0f) k = 1.0f;
        v3 = v2 * (1.0f - k);
    }
    p.pos = p1;
    p.vel = snap(v3);
    if ((c.flags & kM2PtcSphereInward) && c.type == 2) {
        glm::vec3 r = p1 - origin, m = v1 * dt;
        if (r.x * m.x + r.y * m.y + r.z * m.z > 0.0f) return false;
    }
    return true;
}

// ---- SIM-THR ---------------------------------------------------------------------------------------------------
float distanceThrottle(float d) { return clampf(1.0f - (d - 50.0f) * 0.02f, 0.25f, 1.0f); }

// ---- SIM-TW ----------------------------------------------------------------------------------------------------
float twinkleValue(uint32_t i) { return kTwinkle.v[i & 127u]; }

// ---- SIM-BLK ---------------------------------------------------------------------------------------------------
void blockValue(const uint8_t* base, const SpellFxL3Emitter& c, int curve, float t, float* out) {
    const int n = c.lifeKeys[curve];
    const int dim = fxLifeDim(curve);
    if (n == 0) {
        float def = curve == kFxLifeColor ? 255.0f : ((curve == kFxLifeAlpha || curve == kFxLifeScale) ? 1.0f : 0.0f);
        for (int d = 0; d < dim; ++d) out[d] = def;
        return;
    }
    const uint16_t* s = fxLifeTimes(base, c, curve);
    const float* w = fxLifeValues(base, c, curve);
    if (n == 1) {
        for (int d = 0; d < dim; ++d) out[d] = w[d];
        return;
    }
    const float tau = t * 32767.0f;
    int j = 0;
    for (int k = n - 2; k >= 0; --k) {
        if (static_cast<float>(s[k]) <= tau) {
            j = k;
            break;
        }
    }
    float f = 0.0f;
    if (s[j + 1] > s[j]) f = (tau - static_cast<float>(s[j])) / static_cast<float>(s[j + 1] - s[j]);
    for (int d = 0; d < dim; ++d) {
        float a = w[j * dim + d], b = w[(j + 1) * dim + d];
        out[d] = a + (b - a) * f;
    }
}

// ---- SIM-LOOK --------------------------------------------------------------------------------------------------
namespace {
inline float byteChannel(float x) { return clampf(std::floor(x + 0.5f), 0.0f, 255.0f) / 255.0f; }

inline uint16_t cellIndex(float x) {
    float c = std::floor(x + 0.5f);
    if (!fxFinite(c) || c <= 0.0f) return 0;
    if (c >= 65535.0f) return 65535;
    return static_cast<uint16_t>(c);
}

inline uint32_t twinkleStep(float speed, float age) {
    float kf = std::floor(speed * age + 0.5f);
    if (!fxFinite(kf)) return 0;
    if (kf > -2147483648.0f && kf < 2147483648.0f) return static_cast<uint32_t>(static_cast<int32_t>(kf));
    // beyond 32 bits only the residue mod 128 matters for the index
    float r = std::fmod(kf, 128.0f);
    return static_cast<uint32_t>(static_cast<int32_t>(r));
}
}  // namespace

bool look(const uint8_t* base, const SpellFxL3Emitter& c, const EmitterValues& v, uint32_t slot, uint32_t cells,
          float modelScale, float alphaMul, const Particle& p, Look& out) {
    // SIM-LOOK-TW
    uint32_t idx = 0;
    if (!(c.twinklePercent >= 1.0f && c.twinkleRange == 0.0f)) idx = (slot + twinkleStep(c.twinkleSpeed, p.life)) & 127u;
    const float tau = twinkleValue(idx);
    if (c.twinklePercent < 1.0f && tau > c.twinklePercent) return false;
    const float w = tau * c.twinkleRange + c.twinkleMin;

    // SIM-LOOK-COL
    const float span = lifespanOf(v, p);
    const float r = clampf(p.life / span, 0.0f, 1.0f);
    float col[3];
    blockValue(base, c, kFxLifeColor, r, col);
    out.rgba[0] = byteChannel(col[0]);
    out.rgba[1] = byteChannel(col[1]);
    out.rgba[2] = byteChannel(col[2]);
    float a;
    blockValue(base, c, kFxLifeAlpha, r, &a);
    out.rgba[3] = byteChannel(a * alphaMul * 255.0f);

    // SIM-LOOK-CELL
    if (c.lifeKeys[kFxLifeHead] > 0) {
        float h;
        blockValue(base, c, kFxLifeHead, r, &h);
        out.head = cellIndex(h);
    } else if ((c.flags & kM2PtcRandomCell) && cells > 1) {
        uint64_t prod = static_cast<uint64_t>(lookN(p.aux, kRoleCell)) * cells;
        out.head = static_cast<uint16_t>(prod >> 32);
    } else {
        out.head = 0;
    }
    if (c.lifeKeys[kFxLifeTail] > 0) {
        float t;
        blockValue(base, c, kFxLifeTail, r, &t);
        out.tail = cellIndex(t);
    } else {
        out.tail = 0;
    }

    // SIM-LOOK-SV
    float fx, fy;
    if (c.flags & kM2PtcScaleVaryXY) {
        fx = 1.0f + lookS(p.aux, kRoleScaleX) * c.scaleVary[0];
        fy = 1.0f + lookS(p.aux, kRoleScaleY) * c.scaleVary[1];
        if (fx <= 1e-4f) fx = 1e-4f;
        if (fy <= 1e-4f) fy = 1e-4f;
    } else {
        float f = 1.0f + lookS(p.aux, kRoleScale) * c.scaleVary[0];
        if (f < 1e-4f) f = 1e-4f;
        fx = fy = f;
    }

    // SIM-LOOK-SIZE
    float sc[2];
    blockValue(base, c, kFxLifeScale, r, sc);
    out.half[0] = sc[0] * fx * w;
    out.half[1] = sc[1] * fy * w;
    if (c.flags & kM2PtcModelScale) {
        out.half[0] *= modelScale;
        out.half[1] *= modelScale;
    }

    // SIM-LOOK-SPIN
    out.spin = (c.spin != 0.0f || c.spinVary != 0.0f);
    out.angle = 0.0f;
    if (out.spin) {
        float b = c.baseSpin;
        if (c.baseSpinVary != 0.0f) b = c.baseSpin + lookS(p.aux, kRoleBase) * c.baseSpinVary;
        float rate = c.spin;
        if (c.spinVary != 0.0f) rate = c.spin + lookS(p.aux, kRoleRate) * c.spinVary;
        float ang = p.life * rate + b;
        if ((c.flags & kM2PtcRandomSpin) && (slot & 1u)) ang = -ang;
        out.angle = ang;
    }
    return true;
}

// ---- SIM-UV ----------------------------------------------------------------------------------------------------
void cellUv(const SpellFxEmitter& e, const SpellFxL3Emitter& c, uint32_t cell, float uv[4]) {
    uint32_t cols = e.cols ? e.cols : 1u, rows = e.rows ? e.rows : 1u;
    float du = 1.0f / static_cast<float>(cols), dv = 1.0f / static_cast<float>(rows);
    uv[0] = static_cast<float>(cell & (cols - 1u)) * du;
    uv[1] = static_cast<float>(cell >> c.cellShift) * dv;
    uv[2] = du;
    uv[3] = dv;
}

}  // namespace ptcsim
}  // namespace psp
}  // namespace rendering
}  // namespace wowee
