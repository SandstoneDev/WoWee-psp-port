// Portable particle rules (birth, step, look) for level-3 emitters.
// Clean-room implementation of cleanroom/specs/particles/particle-sim.md (SIM-*). No PSP headers: glm + blob format.
// Compiled into the EBOOT (runtime), the converter (spline helpers) and the PC comparison tool.
#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include "rendering/psp/psp_spellfx_format.hpp"

namespace wowee {
namespace rendering {
namespace psp {
namespace ptcsim {

// ---- matrices (SIM-MAT) ----------------------------------------------------------------------------------------
struct M34 {
    float l[9];  // column-major 3x3: l[0..2] = x axis, l[3..5] = y axis, l[6..8] = z axis
    float t[3];  // translation
};
void identity(M34&);
glm::vec3 apply(const M34&, const glm::vec3& p);   // L p + t
glm::vec3 rotate(const M34&, const glm::vec3& d);  // L d
void compose(const float axes[9], const glm::vec3& origin, const M34& b, M34& out);
void emitterFrame(const M34& modelBone, const float position[3], M34& out);

// ---- random generator (SIM-RNG) --------------------------------------------------------------------------------
struct Rng {
    uint32_t s = 625341585u;
    void seed(uint32_t v);
    uint32_t next();
    float u01();
    float sym();
};

// ---- fixed-draw test switch (SIM-FIX) --------------------------------------------------------------------------
// When on, every random variate of this module and of the runtime takes a fixed value: U = 0.75, S = 0.5,
// N = 3221225472 (including the particle's own look variates, whatever `aux` holds). Off by default.
void setFixedDraw(bool on);
bool fixedDraw();
// Variate sources honouring the switch (the raw Rng methods above never do).
float drawU(Rng&);       // uniform [0, 1)
float drawS(Rng&);       // uniform [-1, 1)
uint32_t drawN(Rng&);    // uniform 32-bit

// ---- emitter values and particle -------------------------------------------------------------------------------
struct EmitterValues {
    float rate, life, speed, vRange, hRange, gravity, speedVar, areaL, areaW, zSource, enabled;
};

struct Particle {
    glm::vec3 pos, vel;
    float life;      // age in seconds
    float maxLife;   // life offset (added to the emitter's current lifespan, SIM-LIFE)
    uint8_t owner;   // pool owner (0xFF = free); not touched here
    uint8_t emitter; // emitter index; not touched here
    uint16_t aux;    // the particle's own look randomness (SIM-SPAWN-0)
};
static_assert(sizeof(Particle) == 36, "Particle must be 36 bytes (particle-sim.md 2.1)");

// ---- spline helpers (SIM-SPL) ----------------------------------------------------------------------------------
inline int splineSegments(int n) {
    if (n < 4) return 0;
    int m = (n - 1) / 3;
    return m < 21 ? m : 21;
}

inline glm::vec3 bezier(const float* p, float u) {
    float a = 1.0f - u;
    float b0 = a * a * a, b1 = 3.0f * u * a * a, b2 = 3.0f * u * u * a, b3 = u * u * u;
    return glm::vec3(b0 * p[0] + b1 * p[3] + b2 * p[6] + b3 * p[9],
                     b0 * p[1] + b1 * p[4] + b2 * p[7] + b3 * p[10],
                     b0 * p[2] + b1 * p[5] + b2 * p[8] + b3 * p[11]);
}

inline void splineLengths(const float* pts, int n, float* out) {
    int m = splineSegments(n);
    for (int k = 0; k < m; ++k) {
        const float* seg = pts + 9 * k;
        float len = 0.0f;
        glm::vec3 prev = bezier(seg, 0.0f);
        for (int i = 1; i <= 20; ++i) {
            glm::vec3 cur = bezier(seg, static_cast<float>(i) * 0.05f);
            glm::vec3 d = cur - prev;
            len += glm::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            prev = cur;
        }
        out[k] = len;
    }
}

glm::vec3 splinePoint(const float* pts, int n, const float* lens, float t, glm::vec3* tangent);

// ---- birth, lifespan, step (SIM-SPAWN, SIM-LIFE, SIM-UPD) ------------------------------------------------------
void spawn(const SpellFxL3Emitter&, const EmitterValues&, const float* spline, const M34& frame,
           const glm::vec3& inherit, Rng&, float subFrame, Particle&);
float lifespanOf(const EmitterValues&, const Particle&);
bool update(const SpellFxL3Emitter&, const EmitterValues&, const glm::vec3& origin, const glm::vec3& followStep,
            float dt, Particle&);

// ---- throttle, twinkle, look, cells, life curves ---------------------------------------------------------------
float distanceThrottle(float d);
float twinkleValue(uint32_t i);

struct Look {
    float rgba[4];
    float half[2];
    uint16_t head, tail;
    float angle;
    bool spin;
};
bool look(const uint8_t* base, const SpellFxL3Emitter&, const EmitterValues&, uint32_t slot, uint32_t cells,
          float modelScale, float alphaMul, const Particle&, Look&);
void cellUv(const SpellFxEmitter&, const SpellFxL3Emitter&, uint32_t cell, float uv[4]);
void blockValue(const uint8_t* base, const SpellFxL3Emitter&, int curve, float t, float* out);

}  // namespace ptcsim
}  // namespace psp
}  // namespace rendering
}  // namespace wowee
