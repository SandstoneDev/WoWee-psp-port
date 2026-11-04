// Reference side of the PC particle matrix tool (particle-matrix-tool.md TOOL-1..4), written from the specs:
//  * ReferenceSim - TOOL-3: the RT-EMIT / RT-STEP / SIM rules with every emitter value sampled from the raw M2 tracks
//    at the wrapped time (REC-SAMP), life curves from the raw curves with the BAKE-LIFE conversion applied in memory,
//    bone poses from the caller's full-precision sampler;
//  * EngineSide   - TOOL-2: the EBOOT engine (ParticleEngine) over the baked blob, no budgets;
//  * compareStep  - TOOL-4: matching by serial (emitter index x 1 000 000 + birth number) and error statistics.
// The integrator wires these into psp_particle_matrix (TSV / PPM output and the upstream side stay as they are,
// TOOL-5). PC only.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "m2_particle_raw.hpp"
#include "particle_bake_l3.hpp"
#include "rendering/psp/psp_particles_l3.hpp"

namespace wowee {
namespace pspconv {
namespace ptcref {

using rendering::psp::ptcsim::M34;

// Model-space matrix of M2 bone `bone` at time tMs (sequence 0, already wrapped); identity for bone < 0.
using BoneSampler = std::function<void(int bone, float tMs, M34& out)>;

struct RunConfig {
    float seconds = 6.0f;
    float fps = 30.0f;
    float cameraDistance = 10.0f;  // TOOL-1: throttle 1
};

// One particle's compared state (world space; local-space particles after RT-DRAW-5's transform).
struct SimParticle {
    uint32_t serial;
    int emitter;
    glm::vec3 pos, vel;
    float age;
    bool visible;
    float half[2];
    float rgba[4];
};

constexpr uint32_t kSerialStride = 1000000u;
inline uint32_t serialOf(int emitter, uint32_t birth) { return static_cast<uint32_t>(emitter) * kSerialStride + birth; }
inline uint32_t toolSeed(int emitter) { return 1234u + 7919u * static_cast<uint32_t>(emitter); }  // TOOL-2

class ReferenceSim {
public:
    // kept = the bake's kept emitter M2 indices; periodMs = the bake period (section 4 of the bake spec)
    bool init(const m2raw::Model& raw, const std::vector<int>& kept, BoneSampler bones, uint32_t periodMs);
    void step(float timeMs, float dt);  // one RT-STEP step (with RT-STEP-1 sub-steps) at instance time timeMs
    void snapshot(float timeMs, std::vector<SimParticle>& out) const;
    size_t liveCount() const { return parts_.size(); }

private:
    struct Part {
        rendering::psp::ptcsim::Particle p;
        uint32_t serial;
    };
    struct Em {
        int m2Index;
        uint32_t cells;
        uint16_t cols, rows;
        rendering::psp::ptcsim::Rng rng;
        float acc;
        bool fired;
        uint32_t births;
        float life, gravity;
        glm::vec3 origin;
    };
    rendering::psp::ptcsim::EmitterValues valuesAt(int i, float tMs) const;
    void frameAt(int i, float tMs, M34& F) const;
    void subStep(float timeMs, float dt);

    const m2raw::Model* raw_ = nullptr;
    BoneSampler bones_;
    uint32_t periodMs_ = 3333;
    l3bake::Output rec_;  // in-memory level-3 records (pool offsets relative to rec_.pool.b)
    std::vector<Em> em_;
    std::vector<Part> parts_;
    // owner state of the single, still instance
    glm::vec3 prevOrigin_{0.0f}, lastDisp_{0.0f}, inherit_{0.0f};
    float clock_ = 0.0f;
    bool valid_ = false;
};

class EngineSide {
public:
    bool init(const rendering::psp::FxView& fx);
    void step(float timeMs, float dt);
    void snapshot(float timeMs, std::vector<SimParticle>& out) const;

private:
    static void onBirth(void* user, int slot, uint8_t owner, uint8_t emitter);
    std::unique_ptr<rendering::psp::ParticleEngine> eng_;
    const rendering::psp::FxView* fx_ = nullptr;
    rendering::psp::FxInstance inst_;
    float acc_[16] = {};
    rendering::psp::ptcsim::Rng rngs_[16];
    std::vector<uint32_t> serial_;  // per pool slot
    uint32_t births_[16] = {};
};

struct CompareStats {
    long steps = 0;
    long matched = 0, engineOnly = 0, refOnly = 0;
    long lookPairs = 0, visibilityMismatch = 0;
    double posErrSum = 0, posErrMax = 0;
    double velErrSum = 0, velErrMax = 0;
    double halfErrSum = 0, halfErrMax = 0;
    double colErrSum = 0, colErrMax = 0;
};

// TOOL-4: compare one step's particles of both sides by serial and accumulate the statistics.
void compareStep(const std::vector<SimParticle>& engine, const std::vector<SimParticle>& reference, CompareStats& st);

// TOOL-1: run both sides for cfg.seconds at cfg.fps and compare every step. `perStep` (optional) sees each step's
// particle lists (for the tool's pictures / TSV rows).
using StepHook = std::function<void(int step, float timeMs, const std::vector<SimParticle>& engine,
                                    const std::vector<SimParticle>& reference)>;
bool runCompare(const m2raw::Model& raw, const std::vector<int>& kept, const BoneSampler& bones, uint32_t periodMs,
                const rendering::psp::FxView& blob, const RunConfig& cfg, CompareStats& st, const StepHook& perStep = StepHook());

}  // namespace ptcref
}  // namespace pspconv
}  // namespace wowee
