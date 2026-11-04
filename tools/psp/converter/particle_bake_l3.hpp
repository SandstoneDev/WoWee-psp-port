// Level-3 section builder of the WSFX bake (converter, PC only).
// Clean-room implementation of cleanroom/specs/particles/particle-bake-level3.md 2.1 and sections 4-7 (BAKE-*).
// The existing bakeFx calls buildLevel3Section() with the inputs of 2.1 and places the records and the pool.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "fx_pool.hpp"
#include "m2_particle_raw.hpp"
#include "rendering/psp/psp_spellfx_format.hpp"

namespace wowee {
namespace pspconv {
namespace l3bake {

// The six existing base curves of one kept emitter (SpellFxCurve order), one value per bake frame
// (a single value = constant). Used by BAKE-REACH only.
struct BaseCurves {
    std::vector<float> c[6];  // rate, lifespan, speed, vertical range, horizontal range, gravity
};

struct Input {
    const m2raw::Model* raw = nullptr;  // the raw reader over the same M2 bytes (+ sequence-0 .anim when given)
    bool rawParsed = false;             // m2raw::Model::parse succeeded
    std::vector<int> kept;              // kept emitter M2 indices (the existing selection, ascending)
    int frames = 2;                     // bake frames (section 4)
    uint32_t periodMs = 3333;
    uint8_t cls = rendering::psp::kPtcSpell;  // class bits
    bool forceV2 = false;
    bool level3 = true;
    int maxRibbons = 4;
    bool upstreamRibbons = false;       // the blob has ribbons from the existing path (only used with level3 off)
    std::vector<BaseCurves> base;       // per kept emitter
    float existingReach = 0.5f;         // existing reach without level-3 terms and ribbons (already clamped)
    // rest(bone, p): the bone-space point p at rest; bone < 0 = none (identity when not set)
    std::function<glm::vec3(int, const glm::vec3&)> rest;
    // our WSFX track index of a bone, or kFxNone (kFxNone when not set)
    std::function<uint16_t(int)> trackOfBone;
    // our texture id of a model texture index, or kFxNone (kFxNone when not set)
    std::function<uint16_t(uint32_t)> textureOf;
};

struct Output {
    bool writeSection = false;  // BAKE-VER
    uint16_t version = 1;
    uint8_t extFlags = 0;
    float reach = 0.5f;
    std::vector<rendering::psp::SpellFxL3Emitter> emitters;  // record i <-> kept emitter i (pool-relative offsets)
    std::vector<rendering::psp::SpellFxL3Ribbon> ribbons;    // record i <-> kept ribbon i (pool-relative offsets)
    std::vector<rendering::psp::SpellFxRibbon> ribbonsV2;    // the v2 fields of the kept ribbons
    std::vector<int> keptRibbons;                            // M2 ribbon indices
    FxPool pool;
};

// Section 4: period and frame count of the bake (sequence 0 of the raw reader).
void bakeFrames(const m2raw::Model* raw, float fps, int maxFrames, int& frames, uint32_t& periodMs);
// t_k in ms
float frameTimeMs(uint32_t periodMs, int k, int frames);

// Fills `out` (sections 5-7). Returns out.writeSection.
bool buildLevel3Section(const Input& in, Output& out);

// Adds the pool's blob offset to every pool offset inside the records (offsets of absent data stay 0).
void relocate(Output& out, uint32_t poolBlobOffset);

// cellShift of BAKE-EM-1
uint8_t cellShiftFor(uint32_t cols);

}  // namespace l3bake
}  // namespace pspconv
}  // namespace wowee
