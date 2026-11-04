// M2 particle systems for the PSP.
//
// 1. bakeFx - one M2 -> one WSFX blob (include/rendering/psp/psp_spellfx_format.hpp): its particle
//    emitters (the most populous kFxMaxEmitters), its ribbon emitters (the first kFxMaxRibbons) and,
//    for a spell effect, its batches as rigid per-bone strips. Moved here from psp_spellfx_conv.cpp
//    (unchanged for the mesh / emitters / tracks) so the spell effects and the world's
//    emitters are baked by one code: psp_spellfx_conv (data/spellfx.pak) and psp_model_conv (below).
// 2. ParticleBank - psp_model_conv's half: the effect of every doodad / game object model that has
//    emitters, ribbons, upstream's procedural smoke (isSmoke) or - for a model upstream calls a spell
//    effect, which the WMDL bake skips - its effect mesh; where each is placed (per ADT tile, the same
//    uniqueIds as the "d_" lists) or which game object display uses it; the particle textures (their
//    own small T8 set, --ptc-tex); everything in ONE entry "pidx" written as the pak's first record
//    (include/rendering/psp/psp_particle_format.hpp). Hooks in psp_model_conv.cpp are marked
//    "particles".
// Upstream rules kept: M2Renderer::emitParticles / updateParticles / renderM2Particles,
// updateRibbons / renderM2Ribbons, the classifier's flags (classifyM2Model) that pick them.
#pragma once

#include "conv_common.hpp"
#include "fx_pool.hpp"
#include "m2_particle_raw.hpp"
#include "particle_bake_l3.hpp"  // the level-3 section (clean-room builder)
#include "model_bake.hpp"

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/psp/psp_particle_format.hpp"
#include "rendering/psp/psp_particle_sim.hpp"  // the spline lengths (inline)
#include "rendering/psp/psp_spellfx_format.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::pspconv {

namespace sfx = wowee::rendering::psp;

// A rotation / scale split farther than this from the pose (over the mesh bounds) keeps the
// track as matrices.
constexpr float kFxMaxSplitError = 0.02f;

struct FxBakeOptions {
    float fps = 15.0f;
    int maxFrames = 48;
    int maxEmitters = sfx::kFxMaxEmitters;
    int maxRibbons = sfx::kFxMaxRibbons;
    bool mesh = true;      // the batches as WSFX groups (spell effects)
    bool ribbons = true;   // ribbon emitters
    bool forceV2 = false;  // "pidx" effects: always v2 (the look class, the reach)
    // WSFX level-3 section (particle_bake_l3.hpp): per-emitter / per-ribbon
    // level-3 records. false = the older blob without it.
    bool level3 = true;
    uint8_t cls = sfx::kPtcSpell;
    uint8_t attach = sfx::kFxAttachNone;
};

struct FxBakeStats {
    std::string path;
    std::string why;  // failure reason
    bool ok = false;
    size_t bytes = 0, vertices = 0, triangles = 0, groups = 0, tracks = 0, emitters = 0, sourceEmitters = 0;
    size_t textures = 0, ribbons = 0, sourceBatches = 0, ribbonsKept = 0;
    float keyError = 0.0f;  // largest key quantisation / decomposition error over the mesh bounds (yd)
    size_t matrixTracks = 0;  // tracks kept as matrices (a shear)
    uint32_t durationMs = 0;
    bool dominant = false;
    uint8_t attach = 0, cls = 0;
    float reach = 0.0f;
    uint16_t version = 0;
    bool level3 = false;  // the WSFX level-3 section was written
    std::vector<int> emitterIndex;  // the M2 index of every baked emitter (the matrix tool)
};

// Reads a game file (the extracted tree) for the .anim of sequence 0.
using FxReadFn = std::function<bool(const std::string& gamePath, std::vector<uint8_t>& out)>;

// A NaN / inf from a broken M2 would trap the PSP's FPU on its first compare.
inline float fxFiniteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

// upstream SpellVisualSystem::classifyAttachmentId.
inline uint8_t classifyFxAttachment(const std::string& path) {
    const std::string l = lower(path);
    if (l.find("_hand") != std::string::npos || l.find("hand_") != std::string::npos) return sfx::kFxAttachHand;
    if (l.find("_chest") != std::string::npos || l.find("chest_") != std::string::npos) return sfx::kFxAttachChest;
    if (l.find("_head") != std::string::npos || l.find("head_") != std::string::npos) return sfx::kFxAttachHead;
    return sfx::kFxAttachNone;
}

// upstream computeBoneMatrices (M2Renderer, m2_renderer_internal.h) without the camera
// billboard (the PSP turns those bones at run time): local = T(pivot) T R S T(-pivot), a
// scale component below 0.001 reads as 1, world = parent * local. Global sequences run on
// the same clock as sequence 0.
inline void fxPoseBones(const pipeline::M2Model& m, float t, std::vector<glm::mat4>& out) {
    namespace tr = rendering::m2_track;
    const auto& gsd = m.globalSequenceDurations;
    out.assign(m.bones.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const auto& b = m.bones[i];
        const glm::vec3 trans = tr::sampleVec3(b.translation, 0, t, t, gsd, glm::vec3(0.0f));
        const glm::quat rot = tr::sampleQuat(b.rotation, 0, t, t, gsd);
        glm::vec3 scl = tr::sampleVec3(b.scale, 0, t, t, gsd, glm::vec3(1.0f));
        if (scl.x < 0.001f) scl.x = 1.0f;
        if (scl.y < 0.001f) scl.y = 1.0f;
        if (scl.z < 0.001f) scl.z = 1.0f;
        glm::mat4 local = glm::translate(glm::mat4(1.0f), b.pivot);
        local = glm::translate(local, trans);
        local *= glm::mat4_cast(rot);
        local = glm::scale(local, scl);
        local = glm::translate(local, -b.pivot);
        out[i] = (b.parentBone >= 0 && static_cast<size_t>(b.parentBone) < i) ? out[b.parentBone] * local : local;
    }
}

// upstream M2Renderer::interpFBlockFloat / interpFBlockVec3.
inline float fxBlockFloat(const pipeline::M2FBlock& fb, float r) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.empty()) return fb.floatValues[0];
    r = std::clamp(r, 0.0f, 1.0f);
    for (size_t i = 0; i + 1 < fb.timestamps.size(); ++i) {
        if (r <= fb.timestamps[i + 1]) {
            const float t0 = fb.timestamps[i], t1 = fb.timestamps[i + 1], d = t1 - t0;
            const float f = d > 0.0f ? (r - t0) / d : 0.0f;
            const size_t v0 = std::min(i, fb.floatValues.size() - 1), v1 = std::min(i + 1, fb.floatValues.size() - 1);
            return fb.floatValues[v0] + (fb.floatValues[v1] - fb.floatValues[v0]) * f;
        }
    }
    return fb.floatValues.back();
}

inline glm::vec3 fxBlockVec3(const pipeline::M2FBlock& fb, float r) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.empty()) return fb.vec3Values[0];
    r = std::clamp(r, 0.0f, 1.0f);
    for (size_t i = 0; i + 1 < fb.timestamps.size(); ++i) {
        if (r <= fb.timestamps[i + 1]) {
            const float t0 = fb.timestamps[i], t1 = fb.timestamps[i + 1], d = t1 - t0;
            const float f = d > 0.0f ? (r - t0) / d : 0.0f;
            const size_t v0 = std::min(i, fb.vec3Values.size() - 1), v1 = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[v0], fb.vec3Values[v1], f);
        }
    }
    return fb.vec3Values.back();
}

// upstream updateRibbons getFloatVal / getVec3Val: the first key of any sequence, else the fallback.
inline float fxFirstKey(const pipeline::M2AnimationTrack& t, float fallback) {
    for (const auto& s : t.sequences)
        if (!s.floatValues.empty()) return s.floatValues[0];
    return fallback;
}
inline glm::vec3 fxFirstKey3(const pipeline::M2AnimationTrack& t, const glm::vec3& fallback) {
    for (const auto& s : t.sequences)
        if (!s.vec3Values.empty()) return s.vec3Values[0];
    return fallback;
}

inline uint32_t fxAbgr(const glm::vec3& c, float a) { return to8888(Rgb{c.r, c.g, c.b, std::min(a, 1.0f)}); }

// A model-space bone matrix as T * R * S (Gram-Schmidt on the rotation; a mirrored one
// negates the x scale).
inline void fxDecompose(const glm::mat4& m, glm::quat& q, glm::vec3& t, glm::vec3& s) {
    glm::vec3 c[3] = {glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2])};
    for (int a = 0; a < 3; ++a) {
        s[a] = glm::length(c[a]);
        if (s[a] < 1e-6f) {
            s[a] = 1e-6f;
            c[a] = glm::vec3(a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f);
        } else {
            c[a] /= s[a];
        }
    }
    if (glm::dot(glm::cross(c[0], c[1]), c[2]) < 0.0f) {
        s.x = -s.x;
        c[0] = -c[0];
    }
    c[0] = glm::normalize(c[0]);
    c[1] = glm::normalize(c[1] - glm::dot(c[1], c[0]) * c[0]);
    c[2] = glm::cross(c[0], c[1]);
    q = glm::normalize(glm::quat_cast(glm::mat3(c[0], c[1], c[2])));
    t = glm::vec3(m[3]);
}

inline glm::mat4 fxCompose(const glm::quat& q, const glm::vec3& t, const glm::vec3& s) {
    const glm::mat3 r = glm::mat3_cast(glm::normalize(q));
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(r[0] * s.x, 0.0f);
    m[1] = glm::vec4(r[1] * s.y, 0.0f);
    m[2] = glm::vec4(r[2] * s.z, 0.0f);
    m[3] = glm::vec4(t, 1.0f);
    return m;
}

// One M2 (skin loaded when o.mesh) -> a WSFX blob. Loads the .anim of sequence 0 into `m` when
// it is not in the M2 (through `read`). Missing game files go to `missing`. False: st.why says why.
inline bool bakeFx(const std::string& m2Path, const std::vector<uint8_t>& m2Bytes, pipeline::M2Model& m,
                   const FxBakeOptions& o, TextureBank& bank, const FxReadFn& read, std::set<std::string>& missing,
                   std::vector<uint8_t>& blob, FxBakeStats& st) {
    namespace tr = rendering::m2_track;
    st.path = m2Path;
    st.attach = o.attach;
    st.cls = o.cls;
    const bool useRibbons = o.ribbons && o.maxRibbons > 0;
    // upstream: a model with neither vertices nor particle emitters is "empty" (here also:
    // nor ribbons, nor upstream's procedural smoke).
    if (m.vertices.empty() && m.particleEmitters.empty() && (!useRibbons || m.ribbonEmitters.empty()) &&
        !(o.cls & sfx::kPtcSmoke)) {
        st.why = "empty";
        return false;
    }
    std::vector<uint8_t> anim;  // kept for the raw reader too
    if (!m.sequences.empty() && !(m.sequences[0].flags & 0x20)) {
        const std::string ap = animPath(m2Path, m.sequences[0].id, m.sequences[0].variationIndex);
        if (read(ap, anim)) pipeline::M2Loader::loadAnimFile(m2Bytes, anim, 0, m);
        else missing.insert(ap);
    }
    // The raw particle / ribbon records (m2_particle_raw.hpp) for the level-3 section.
    m2raw::Model raw;
    const bool haveRaw = o.level3 && raw.parse(m2Bytes, anim.empty() ? nullptr : &anim);
    st.durationMs = m.sequences.empty() ? 0u : m.sequences[0].duration;
    st.ribbons = m.ribbonEmitters.size();
    st.sourceEmitters = m.particleEmitters.size();
    st.sourceBatches = m.batches.size();
    // upstream M2_DEFAULT_PARTICLE_ANIM_MS (3333) when the model has no duration.
    const uint32_t period = st.durationMs > 0 ? st.durationMs : 3333u;
    const int frames = std::clamp(static_cast<int>(std::lround(period * o.fps / 1000.0f)) + 1, 2, o.maxFrames);
    std::vector<float> times(frames);
    for (int k = 0; k < frames; ++k) times[k] = static_cast<float>(period) * k / (frames - 1);
    std::vector<std::vector<glm::mat4>> poses(frames);
    for (int k = 0; k < frames; ++k) fxPoseBones(m, times[k], poses[k]);

    // upstream: a particle-dominant spell effect (emitters and <= 2 batches) draws no mesh.
    st.dominant = !m.particleEmitters.empty() && m.batches.size() <= 2;

    // --- mesh groups --------------------------------------------------------------------
    struct Group {
        int batch;
        int bone;  // -1 = none
        std::vector<uint32_t> tris;
        uint16_t texture = sfx::kFxNone;
        uint8_t blend = sfx::kFxBlendAdd, flags = 0;
        uint32_t color = 0xFFFFFFFFu;
        std::vector<glm::vec2> uv;  // per frame (empty = none)
    };
    std::vector<Group> groups;
    // An item visual (kPtcItem) keeps its batches whatever its emitters - upstream
    // attachWeaponEffect draws every batch of the effect model.
    const bool itemFx = (o.cls & sfx::kPtcItem) != 0;
    if (o.mesh && (!st.dominant || itemFx)) {
        for (size_t bi = 0; bi < m.batches.size(); ++bi) {
            const auto& b = m.batches[bi];
            if (b.submeshLevel != 0 || b.indexCount < 3) continue;  // upstream: LOD 0 near the camera
            // Texture: the batch's first texture (combos of several are drawn with the first).
            int texId = -1;
            bool texFailed = true;
            uint8_t wrap = 0;
            if (b.textureIndex < m.textureLookup.size()) {
                const uint16_t ti = m.textureLookup[b.textureIndex];
                if (ti < m.textures.size() && m.textures[ti].type == 0 && !m.textures[ti].filename.empty()) {
                    bool alpha = false;
                    texId = bank.get(m.textures[ti].filename, alpha);
                    if (texId < 0) missing.insert(m.textures[ti].filename);
                    texFailed = texId < 0;
                    wrap = static_cast<uint8_t>(((m.textures[ti].flags & 1u) ? sfx::kFxWrapU : 0) |
                                                ((m.textures[ti].flags & 2u) ? sfx::kFxWrapV : 0));
                }
            }
            if (texFailed) continue;  // upstream: batchOpacity 0 when the texture did not load
            // upstream at-rest opacity: colour alpha x texture weight, zero values skipped.
            float opacity = 1.0f;
            glm::vec3 tint(1.0f);
            if (b.colorIndex < m.colorRGB.size()) tint = m.colorRGB[b.colorIndex];
            if (b.colorIndex < m.colorAlphas.size() && m.colorAlphas[b.colorIndex] > 0.001f)
                opacity *= m.colorAlphas[b.colorIndex];
            if (b.transparencyIndex < m.textureWeights.size() && m.textureWeights[b.transparencyIndex] > 0.001f)
                opacity *= m.textureWeights[b.transparencyIndex];
            if (opacity < 0.01f) continue;
            const uint16_t blendMode = b.materialIndex < m.materials.size() ? m.materials[b.materialIndex].blendMode : 0;
            // UV translation track.
            std::vector<glm::vec2> uv;
            if (b.textureAnimIndex != 0xFFFF && b.textureAnimIndex < m.textureTransformLookup.size()) {
                const uint16_t ti = m.textureTransformLookup[b.textureAnimIndex];
                if (ti < m.textureTransforms.size()) {
                    const auto& tt = m.textureTransforms[ti];
                    bool any = false;
                    for (int k = 0; k < frames; ++k) {
                        const glm::vec3 t3 =
                            tr::sampleVec3(tt.translation, 0, times[k], times[k], m.globalSequenceDurations, glm::vec3(0));
                        uv.emplace_back(fxFiniteOr(t3.x, 0.0f), fxFiniteOr(t3.y, 0.0f));
                        if (std::fabs(uv.back().x) > 1e-5f || std::fabs(uv.back().y) > 1e-5f) any = true;
                    }
                    if (!any) uv.clear();
                }
            }
            // Triangles by their heaviest bone.
            std::map<int, size_t> byBone;
            for (uint32_t k = 0; k + 2 < b.indexCount; k += 3) {
                uint32_t tri[3];
                bool ok = true;
                for (int c = 0; c < 3; ++c) {
                    const uint32_t ii = b.indexStart + k + c;
                    if (ii >= m.indices.size() || m.indices[ii] >= m.vertices.size()) ok = false;
                    else tri[c] = m.indices[ii];
                }
                if (!ok) continue;
                std::map<int, float> w;
                for (int c = 0; c < 3; ++c) {
                    const auto& v = m.vertices[tri[c]];
                    for (int j = 0; j < 4; ++j)
                        if (v.boneWeights[j] && v.boneIndices[j] < m.bones.size()) w[v.boneIndices[j]] += v.boneWeights[j];
                }
                int bone = -1;
                float best = 0.0f;
                for (auto [bb, ww] : w)
                    if (ww > best) {
                        best = ww;
                        bone = bb;
                    }
                auto it = byBone.find(bone);
                if (it == byBone.end()) {
                    it = byBone.emplace(bone, groups.size()).first;
                    Group g;
                    g.batch = static_cast<int>(bi);
                    g.bone = bone;
                    g.texture = static_cast<uint16_t>(texId);
                    // An item visual is additive whatever its material (upstream: "glow cards
                    // drawn on black ... force additive so only the light adds").
                    g.blend = (blendMode == 2 && !itemFx) ? sfx::kFxBlendAlpha : sfx::kFxBlendAdd;
                    g.flags = wrap;
                    g.color = fxAbgr(tint, opacity);
                    g.uv = uv;
                    groups.push_back(std::move(g));
                }
                auto& g = groups[it->second];
                g.tris.insert(g.tris.end(), tri, tri + 3);
            }
        }
    }

    // --- emitters (the most populous first) ---------------------------------------------
    auto sampleF = [&](const pipeline::M2AnimationTrack& t, int k) {
        return fxFiniteOr(tr::sampleFloat(t, 0, times[k], times[k], m.globalSequenceDurations, 0.0f), 0.0f);
    };
    std::vector<int> emitters;
    {
        std::vector<std::pair<float, int>> rank;
        for (size_t i = 0; i < m.particleEmitters.size(); ++i) {
            const auto& e = m.particleEmitters[i];
            float pop = 0.0f;
            for (int k = 0; k < frames; ++k) {
                float r = std::max(sampleF(e.emissionRate, k), 0.0f);
                const float l = std::max(sampleF(e.lifespan, k), 0.0f);
                // Upstream's flame floor (15 live) counts in the ranking.
                if ((o.cls & sfx::kPtcFlame) && r > 0.0f && l > 0.0f) r = std::max(r, 15.0f / std::max(l, 0.1f));
                pop += r * l;
            }
            if (pop <= 0.0f) continue;  // never emits during sequence 0
            rank.emplace_back(pop, static_cast<int>(i));
        }
        std::stable_sort(rank.begin(), rank.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < rank.size() && static_cast<int>(i) < o.maxEmitters; ++i) emitters.push_back(rank[i].second);
        std::sort(emitters.begin(), emitters.end());  // the M2's own order
    }
    st.emitterIndex = emitters;
    // The level-3 builder reads every kept emitter's raw record: without them the base blob is written.
    bool useRaw = haveRaw;
    for (int ei : emitters)
        if (static_cast<size_t>(ei) >= raw.emitters.size()) useRaw = false;
    // --- ribbons (the M2 order, the first kFxMaxRibbons) --------------------------------------
    // With the raw reader the ribbons come at the record's real stride (0xB0; upstream's loader steps 0xAC and
    // misreads every ribbon after the first); a ribbon on bone -1 is skipped as upstream does.
    std::vector<int> ribbons;
    if (useRibbons) {
        if (useRaw) {
            st.ribbons = raw.ribbons.size();
            for (size_t i = 0; i < raw.ribbons.size() && static_cast<int>(ribbons.size()) < o.maxRibbons; ++i)
                if (raw.ribbons[i].bone != 0xFFFFFFFFu) ribbons.push_back(static_cast<int>(i));
        } else {
            for (size_t i = 0; i < m.ribbonEmitters.size() && static_cast<int>(ribbons.size()) < o.maxRibbons; ++i)
                ribbons.push_back(static_cast<int>(i));
        }
    }
    st.ribbonsKept = ribbons.size();
    if (groups.empty() && emitters.empty() && ribbons.empty() && !(o.cls & sfx::kPtcSmoke)) {
        st.why = st.ribbons ? "ribbons only" : "nothing drawn";
        return false;
    }

    // --- tracks ---------------------------------------------------------------------------
    glm::vec3 errMin(-0.5f), errMax(0.5f);  // where the key error is measured: the mesh bounds
    for (const auto& g : groups)
        for (uint32_t v : g.tris) {
            errMin = glm::min(errMin, m.vertices[v].position);
            errMax = glm::max(errMax, m.vertices[v].position);
        }
    FxPool pool;
    std::map<int, uint16_t> trackOf;
    std::vector<sfx::SpellFxTrack> tracks;
    auto trackFor = [&](int bone) -> uint16_t {
        if (bone < 0 || static_cast<size_t>(bone) >= m.bones.size()) return sfx::kFxNone;
        auto it = trackOf.find(bone);
        if (it != trackOf.end()) return it->second;
        bool constant = true;
        for (int k = 1; k < frames && constant; ++k)
            for (int c = 0; c < 4; ++c)
                if (glm::length(poses[k][bone][c] - poses[0][bone][c]) > 1e-5f) constant = false;
        const int n = constant ? 1 : frames;
        std::vector<glm::quat> qs(n);
        std::vector<glm::vec3> ts(n), ss(n);
        float tMax = 1e-6f, sMax = 1e-6f;
        for (int k = 0; k < n; ++k) {
            fxDecompose(poses[k][bone], qs[k], ts[k], ss[k]);
            if (k > 0 && glm::dot(qs[k], qs[k - 1]) < 0.0f) qs[k] = -qs[k];
            for (int a = 0; a < 3; ++a) {
                tMax = std::max(tMax, std::fabs(ts[k][a]));
                sMax = std::max(sMax, std::fabs(ss[k][a]));
            }
        }
        sfx::SpellFxTrack t{};
        t.frames = static_cast<uint16_t>(n);
        t.translationScale = tMax / 32767.0f;
        t.scaleScale = sMax / 32767.0f;
        auto q16 = [](float v) { return static_cast<int16_t>(std::lround(std::clamp(fxFiniteOr(v, 0.0f), -1.0f, 1.0f) * 32767.0f)); };
        auto pointError = [&](const glm::mat4& a, const glm::mat4& b) {
            float e = 0.0f;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec4 p((corner & 1) ? errMax.x : errMin.x, (corner & 2) ? errMax.y : errMin.y,
                                  (corner & 4) ? errMax.z : errMin.z, 1.0f);
                e = std::max(e, glm::length(glm::vec3(a * p - b * p)));
            }
            return e;
        };
        // A shear the rotation / scale split cannot hold: keep the matrices.
        float split = 0.0f;
        for (int k = 0; k < n; ++k) split = std::max(split, pointError(fxCompose(qs[k], ts[k], ss[k]), poses[k][bone]));
        if (split > kFxMaxSplitError) {
            float lMax = 1e-6f;
            for (int k = 0; k < n; ++k)
                for (int c = 0; c < 3; ++c)
                    for (int r = 0; r < 3; ++r) lMax = std::max(lMax, std::fabs(poses[k][bone][c][r]));
            t.flags = sfx::kFxTrackMatrix;
            t.scaleScale = lMax / 32767.0f;
            std::vector<sfx::SpellFxMatrixKey> keys(n);
            for (int k = 0; k < n; ++k) {
                const glm::mat4& a = poses[k][bone];
                glm::mat4 back(1.0f);
                for (int c = 0; c < 3; ++c) {
                    for (int r = 0; r < 3; ++r) {
                        keys[k].m[c * 3 + r] = q16(a[c][r] / lMax);
                        back[c][r] = keys[k].m[c * 3 + r] * t.scaleScale;
                    }
                    keys[k].t[c] = q16(ts[k][c] / tMax);
                    back[3][c] = keys[k].t[c] * t.translationScale;
                }
                st.keyError = std::max(st.keyError, pointError(back, a));
            }
            ++st.matrixTracks;
            t.offset = pool.addKeys(keys.data(), keys.size());  // pool-relative; fixed below
        } else {
            std::vector<sfx::SpellFxKey> keys(n);
            for (int k = 0; k < n; ++k) {
                auto& key = keys[k];
                key.q[0] = q16(qs[k].x);
                key.q[1] = q16(qs[k].y);
                key.q[2] = q16(qs[k].z);
                key.q[3] = q16(qs[k].w);
                for (int a = 0; a < 3; ++a) {
                    key.t[a] = q16(ts[k][a] / tMax);
                    key.s[a] = q16(ss[k][a] / sMax);
                }
                // What the PSP will rebuild, against the pose, over the mesh bounds.
                const glm::quat dq(key.q[3] / 32767.0f, key.q[0] / 32767.0f, key.q[1] / 32767.0f, key.q[2] / 32767.0f);
                const glm::vec3 dt(key.t[0] * t.translationScale, key.t[1] * t.translationScale,
                                   key.t[2] * t.translationScale);
                const glm::vec3 ds(key.s[0] * t.scaleScale, key.s[1] * t.scaleScale, key.s[2] * t.scaleScale);
                st.keyError = std::max(st.keyError, pointError(fxCompose(dq, dt, ds), poses[k][bone]));
            }
            t.offset = pool.addKeys(keys.data(), keys.size());  // pool-relative; fixed below
        }
        if (m.bones[bone].flags & 0x8u) t.flags = static_cast<uint16_t>(t.flags | sfx::kFxTrackBillboard);
        for (int a = 0; a < 3; ++a) t.pivot[a] = fxFiniteOr(m.bones[bone].pivot[a], 0.0f);
        t.bone = static_cast<uint16_t>(bone);
        const uint16_t id = static_cast<uint16_t>(tracks.size());
        tracks.push_back(t);
        trackOf.emplace(bone, id);
        return id;
    };
    // A bone-space point at sequence time 0 in model space (the reach).
    auto atRest = [&](int bone, const glm::vec3& p) {
        if (bone < 0 || static_cast<size_t>(bone) >= m.bones.size()) return p;
        return glm::vec3(poses[0][bone] * glm::vec4(p, 1.0f));
    };
    float reach = 0.0f;

    // --- vertices: strips per group, bind pose -----------------------------------------
    glm::vec3 pmin(1e9f), pmax(-1e9f);
    for (const auto& g : groups)
        for (uint32_t v : g.tris) {
            pmin = glm::min(pmin, m.vertices[v].position);
            pmax = glm::max(pmax, m.vertices[v].position);
        }
    const glm::vec3 center = groups.empty() ? glm::vec3(0.0f) : (pmin + pmax) * 0.5f;
    const glm::vec3 half = groups.empty() ? glm::vec3(1.0f) : glm::max((pmax - pmin) * 0.5f, glm::vec3(0.01f));
    float radius = 0.0f;
    std::vector<sfx::SpellFxVertex> verts;
    std::vector<sfx::SpellFxGroup> outGroups;
    std::set<uint16_t> texUsed;
    for (auto& g : groups) {
        // Local vertex list, then meshoptimizer strips (as WmdlBuilder: two-sided, winding free).
        std::unordered_map<uint32_t, uint32_t> local;
        std::vector<uint32_t> src;
        std::vector<unsigned int> idx;
        for (uint32_t v : g.tris) {
            auto it = local.find(v);
            if (it == local.end()) {
                it = local.emplace(v, static_cast<uint32_t>(src.size())).first;
                src.push_back(v);
            }
            idx.push_back(it->second);
        }
        meshopt_optimizeVertexCacheStrip(idx.data(), idx.data(), idx.size(), src.size());
        std::vector<unsigned int> strip(meshopt_stripifyBound(idx.size()));
        strip.resize(meshopt_stripify(strip.data(), idx.data(), idx.size(), src.size(), 0));
        if (strip.size() < 3 || strip.size() > 65535) continue;
        glm::vec2 tmin(1e9f), tmax(-1e9f);
        for (uint32_t v : src) {
            tmin = glm::min(tmin, m.vertices[v].texCoords[0]);
            tmax = glm::max(tmax, m.vertices[v].texCoords[0]);
        }
        const glm::vec2 trange = glm::max(tmax - tmin, glm::vec2(1e-4f));
        sfx::SpellFxGroup og{};
        og.vertexStart = static_cast<uint32_t>(verts.size());
        og.vertexCount = static_cast<uint16_t>(strip.size());
        og.texture = g.texture;
        og.track = trackFor(g.bone);
        og.blend = g.blend;
        og.flags = g.flags;
        og.color = g.color;
        og.uvMin[0] = tmin.x;
        og.uvMin[1] = tmin.y;
        og.uvScale[0] = trange.x * 32768.0f / 65535.0f;
        og.uvScale[1] = trange.y * 32768.0f / 65535.0f;
        if (!g.uv.empty()) {
            bool constant = true;
            for (int k = 1; k < frames; ++k)
                if (glm::length(g.uv[k] - g.uv[0]) > 1e-5f) constant = false;
            og.uvFrames = static_cast<uint16_t>(constant ? 1 : frames);
            std::vector<float> f;
            for (int k = 0; k < og.uvFrames; ++k) {
                f.push_back(g.uv[k].x);
                f.push_back(g.uv[k].y);
            }
            og.uvOffset = pool.add(f.data(), f.size()) + 1u;  // +1: "set" marker until the pool is placed
        }
        if (g.texture != sfx::kFxNone) texUsed.insert(g.texture);
        for (unsigned int li : strip) {
            const auto& v = m.vertices[src[li]];
            const glm::vec3 p = (v.position - center) / half;
            radius = std::max(radius, glm::length(v.position - center));
            sfx::SpellFxVertex ov{};
            ov.x = static_cast<int16_t>(std::lround(std::clamp(p.x, -1.0f, 1.0f) * 32767.0f));
            ov.y = static_cast<int16_t>(std::lround(std::clamp(p.y, -1.0f, 1.0f) * 32767.0f));
            ov.z = static_cast<int16_t>(std::lround(std::clamp(p.z, -1.0f, 1.0f) * 32767.0f));
            ov.u = static_cast<uint16_t>(std::lround(std::clamp((v.texCoords[0].x - tmin.x) / trange.x, 0.0f, 1.0f) * 65535.0f));
            ov.v = static_cast<uint16_t>(std::lround(std::clamp((v.texCoords[0].y - tmin.y) / trange.y, 0.0f, 1.0f) * 65535.0f));
            verts.push_back(ov);
        }
        st.triangles += strip.size() - 2;
        outGroups.push_back(og);
    }
    if (!outGroups.empty()) reach = std::max(reach, glm::length(center) + radius + 1.0f);  // + bone motion

    std::vector<sfx::SpellFxEmitter> outEmitters;
    std::vector<l3bake::BaseCurves> baseCurves;  // the six base curves of every kept emitter, every frame (the builder)
    for (int ei : emitters) {
        const auto& e = m.particleEmitters[ei];
        sfx::SpellFxEmitter oe{};
        const int bone = e.bone < m.bones.size() ? static_cast<int>(e.bone) : -1;
        oe.track = trackFor(bone);
        oe.texture = sfx::kFxNone;
        if (e.texture < m.textures.size() && m.textures[e.texture].type == 0 && !m.textures[e.texture].filename.empty()) {
            bool alpha = false;
            const int id = bank.get(m.textures[e.texture].filename, alpha);
            if (id >= 0) {
                oe.texture = static_cast<uint16_t>(id);
                texUsed.insert(oe.texture);
            } else {
                missing.insert(m.textures[e.texture].filename);
            }
        }
        // upstream renderM2Particles: blending 3 / 4 -> additive pipeline, 1 -> alpha key, else alpha.
        oe.blend = (e.blendingType == 3 || e.blendingType == 4) ? sfx::kFxBlendAdd
                   : e.blendingType == 1                       ? sfx::kFxBlendAlphaKey
                                                               : sfx::kFxBlendAlpha;
        oe.flags = static_cast<uint8_t>(e.flags & (sfx::kFxEmitRandomTile | sfx::kFxEmitTiled));
        oe.cols = static_cast<uint8_t>(std::clamp<int>(e.textureCols, 1, 255));
        oe.rows = static_cast<uint8_t>(std::clamp<int>(e.textureRows, 1, 255));
        for (int a = 0; a < 3; ++a) oe.position[a] = fxFiniteOr(e.position[a], 0.0f);
        float maxScale = 0.0f;
        for (int k = 0; k < sfx::kFxLifeKeys; ++k) {
            const float r = static_cast<float>(k) / (sfx::kFxLifeKeys - 1);
            const glm::vec3 c = fxBlockVec3(e.particleColor, r);
            oe.color[k] = fxAbgr(glm::clamp(glm::vec3(fxFiniteOr(c.r, 1.0f), fxFiniteOr(c.g, 1.0f), fxFiniteOr(c.b, 1.0f)),
                                            glm::vec3(0.0f), glm::vec3(1.0f)),
                                 std::clamp(fxFiniteOr(fxBlockFloat(e.particleAlpha, r), 1.0f), 0.0f, 1.0f));
            oe.scale[k] = fxFiniteOr(fxBlockFloat(e.particleScale, r), 1.0f);
            maxScale = std::max(maxScale, std::fabs(oe.scale[k]));
        }
        const pipeline::M2AnimationTrack* curves[sfx::kFxCurveCount] = {
            &e.emissionRate, &e.lifespan, &e.emissionSpeed, &e.verticalRange, &e.horizontalRange, &e.gravity};
        float travel = 0.0f;
        l3bake::BaseCurves base;
        for (int c = 0; c < sfx::kFxCurveCount; ++c) {
            std::vector<float> f;
            bool constant = true;
            for (int k = 0; k < frames; ++k) {
                f.push_back(sampleF(*curves[c], k));
                if (k > 0 && std::fabs(f[k] - f[0]) > 1e-5f) constant = false;
            }
            base.c[c] = f;
            if (constant) f.resize(1);
            oe.curveFrames[c] = static_cast<uint16_t>(f.size());
            oe.curveOffset[c] = pool.add(f.data(), f.size());
        }
        // How far its particles get (the reach): the emission point at rest, the
        // flight (speed x life, the drift when it stands still) and the fall (upstream's default
        // gravity 4 / 1.5 when the curve is 0), the sprite.
        for (int k = 0; k < frames; ++k) {
            const float life = std::clamp(sampleF(e.lifespan, k), 0.0f, 30.0f);
            const float speed = std::fabs(sampleF(e.emissionSpeed, k));
            float g = std::fabs(sampleF(e.gravity, k));
            if (g == 0.0f && !(o.cls & sfx::kPtcFirefly)) g = speed > 0.1f ? 4.0f : 1.5f;
            const float fly = speed < 0.01f ? ((o.cls & sfx::kPtcFirefly) ? 0.7f : 1.2f) * life : speed * life;
            travel = std::max(travel, fly + 0.5f * g * life * life);
        }
        const float sprite = 0.5f * 0.47f * std::max(maxScale * ((o.cls & sfx::kPtcSpell) ? 1.5f : 1.0f), 0.15f);
        reach = std::max(reach, glm::length(atRest(bone, glm::vec3(oe.position[0], oe.position[1], oe.position[2]))) +
                                    std::min(travel, 50.0f) + sprite);
        outEmitters.push_back(oe);
        baseCurves.push_back(std::move(base));
    }

    // Ribbons (upstream updateRibbons: the first key of each track). With the raw records the
    // level-3 builder makes them (below).
    std::vector<sfx::SpellFxRibbon> outRibbons;
    for (int ri : ribbons) {
        if (useRaw) break;
        const auto& r = m.ribbonEmitters[ri];
        sfx::SpellFxRibbon orb{};
        // upstream: an out-of-range bone reads as bone 0 (spell effects leave it unset); no bones: the origin.
        int bone = static_cast<int>(r.bone);
        if ((r.bone >= m.bones.size()) && !m.bones.empty()) bone = 0;
        if (m.bones.empty()) bone = -1;
        orb.track = trackFor(bone);
        // upstream: textureIndex straight into the textures, else through the lookup; white when neither.
        orb.texture = sfx::kFxNone;
        int ti = -1;
        if (r.textureIndex < m.textures.size()) ti = r.textureIndex;
        else if (r.textureIndex < m.textureLookup.size() && m.textureLookup[r.textureIndex] < m.textures.size())
            ti = m.textureLookup[r.textureIndex];
        if (ti >= 0 && m.textures[ti].type == 0 && !m.textures[ti].filename.empty()) {
            bool alpha = false;
            const int id = bank.get(m.textures[ti].filename, alpha);
            if (id >= 0) {
                orb.texture = static_cast<uint16_t>(id);
                texUsed.insert(orb.texture);
            } else {
                missing.insert(m.textures[ti].filename);
            }
        }
        // upstream renderM2Ribbons: additive when the batch at materialIndex blends with mode >= 3.
        bool additive = false;
        if (r.materialIndex < m.batches.size()) {
            const uint16_t mi = m.batches[r.materialIndex].materialIndex;
            additive = mi < m.materials.size() && m.materials[mi].blendMode >= 3;
        }
        orb.blend = additive ? sfx::kFxBlendAdd : sfx::kFxBlendAlpha;
        for (int a = 0; a < 3; ++a) orb.position[a] = fxFiniteOr(r.position[a], 0.0f);
        const glm::vec3 col = fxFirstKey3(r.colorTrack, glm::vec3(1.0f));
        orb.color = fxAbgr(glm::clamp(glm::vec3(fxFiniteOr(col.r, 1.0f), fxFiniteOr(col.g, 1.0f), fxFiniteOr(col.b, 1.0f)),
                                      glm::vec3(0.0f), glm::vec3(1.0f)),
                           std::clamp(fxFiniteOr(fxFirstKey(r.alphaTrack, 1.0f), 1.0f), 0.0f, 1.0f));
        orb.heightAbove = std::clamp(fxFiniteOr(fxFirstKey(r.heightAboveTrack, 0.5f), 0.5f), -100.0f, 100.0f);
        orb.heightBelow = std::clamp(fxFiniteOr(fxFirstKey(r.heightBelowTrack, 0.5f), 0.5f), -100.0f, 100.0f);
        orb.visibility = fxFiniteOr(fxFirstKey(r.visibilityTrack, 1.0f), 1.0f);
        orb.edgesPerSecond = std::clamp(fxFiniteOr(r.edgesPerSecond, 15.0f), 0.0f, 1000.0f);
        orb.edgeLifetime = std::clamp(fxFiniteOr(r.edgeLifetime, 0.5f), 0.01f, 100.0f);
        orb.gravity = std::clamp(fxFiniteOr(r.gravity, 0.0f), -1.0e5f, 1.0e5f);
        reach = std::max(reach, glm::length(atRest(bone, glm::vec3(orb.position[0], orb.position[1], orb.position[2]))) +
                                    std::max(std::fabs(orb.heightAbove), std::fabs(orb.heightBelow)) + 2.0f);
        outRibbons.push_back(orb);
    }
    // upstream M2Renderer::update's smoke: up 3..5 yd/s, slowed x0.98 a frame, 4..7 s.
    if (o.cls & sfx::kPtcSmoke) reach = std::max(reach, 12.0f);
    reach = std::clamp(reach, 0.5f, 60.0f);
    // The level-3 section (particle_bake_l3.hpp): its records and pool, the ribbons of the raw records, the blob's
    // version and the reach with the level-3 and ribbon terms.
    l3bake::Input l3in;
    l3in.raw = &raw;
    l3in.rawParsed = useRaw;
    l3in.kept = emitters;
    l3in.frames = frames;
    l3in.periodMs = period;
    l3in.cls = o.cls;
    l3in.forceV2 = o.forceV2;
    l3in.level3 = o.level3;
    l3in.maxRibbons = useRibbons ? o.maxRibbons : 0;
    l3in.upstreamRibbons = !outRibbons.empty();
    l3in.base = std::move(baseCurves);
    l3in.existingReach = reach;
    l3in.rest = [&](int bone, const glm::vec3& p) { return atRest(bone, p); };
    l3in.trackOfBone = [&](int bone) { return trackFor(bone); };
    l3in.textureOf = [&](uint32_t ti) -> uint16_t {
        if (ti >= m.textures.size() || m.textures[ti].type != 0 || m.textures[ti].filename.empty()) return sfx::kFxNone;
        bool alpha = false;
        const int id = bank.get(m.textures[ti].filename, alpha);
        if (id < 0) {
            missing.insert(m.textures[ti].filename);
            return sfx::kFxNone;
        }
        texUsed.insert(static_cast<uint16_t>(id));
        return static_cast<uint16_t>(id);
    };
    l3bake::Output l3;
    const bool level3 = l3bake::buildLevel3Section(l3in, l3);
    if (useRaw && useRibbons) {
        if (l3.keptRibbons != ribbons) {
            st.why = "level-3 ribbon selection differs";
            return false;
        }
        outRibbons = l3.ribbonsV2;
    }
    reach = l3.reach;
    st.reach = reach;
    if (outGroups.empty() && outEmitters.empty() && outRibbons.empty() && !(o.cls & sfx::kPtcSmoke)) {
        st.why = "nothing drawn";
        return false;
    }

    // --- blob -------------------------------------------------------------------------------
    // v2 only when it adds something (ribbons, a look other than a spell effect's):
    // a spell effect without ribbons and without the level-3 section stays the v1 blob older EBOOTs read.
    // The level-3 section lives in the extension (version and section decision: particle_bake_l3.hpp).
    const bool v2 = l3.version == sfx::kSpellFxVersion2;
    sfx::SpellFxHeader h{};
    std::memcpy(h.magic, "WSFX", 4);
    h.version = v2 ? sfx::kSpellFxVersion2 : sfx::kSpellFxVersion;
    h.attach = st.attach;
    h.flags = static_cast<uint8_t>((st.dominant ? sfx::kFxParticleDominant : 0) | (st.ribbons ? sfx::kFxHasRibbons : 0));
    h.durationMs = st.durationMs;
    h.periodMs = period;
    h.frames = static_cast<uint16_t>(frames);
    h.trackCount = static_cast<uint16_t>(tracks.size());
    h.groupCount = static_cast<uint16_t>(outGroups.size());
    h.emitterCount = static_cast<uint16_t>(outEmitters.size());
    h.textureCount = static_cast<uint16_t>(texUsed.size());
    h.sourceEmitters = static_cast<uint16_t>(std::min<size_t>(m.particleEmitters.size(), 0xFFFF));
    for (int a = 0; a < 3; ++a) {
        h.center[a] = fxFiniteOr(center[a], 0.0f);
        h.half[a] = fxFiniteOr(half[a], 1.0f);
    }
    h.radius = fxFiniteOr(radius, 0.0f);
    sfx::SpellFxHeaderExt x{};
    x.cls = o.cls;
    x.ribbonCount = static_cast<uint16_t>(outRibbons.size());
    x.reach = reach;
    blob.assign(sizeof(h), 0);
    if (v2) blob.resize(blob.size() + sizeof(x), 0);
    alignTo(blob, 4);
    h.trackOffset = static_cast<uint32_t>(blob.size());
    blob.resize(blob.size() + tracks.size() * sizeof(sfx::SpellFxTrack));
    h.groupOffset = static_cast<uint32_t>(blob.size());
    blob.resize(blob.size() + outGroups.size() * sizeof(sfx::SpellFxGroup));
    h.emitterOffset = static_cast<uint32_t>(blob.size());
    blob.resize(blob.size() + outEmitters.size() * sizeof(sfx::SpellFxEmitter));
    if (v2) {
        x.ribbonOffset = static_cast<uint32_t>(blob.size());
        blob.resize(blob.size() + outRibbons.size() * sizeof(sfx::SpellFxRibbon));
    }
    if (level3) {  // SpellFxL3Emitter[emitters] + SpellFxL3Ribbon[ribbons]
        alignTo(blob, 4);
        x.extFlags = l3.extFlags;
        x.level3Offset = static_cast<uint32_t>(blob.size());
        blob.resize(blob.size() + l3.emitters.size() * sizeof(sfx::SpellFxL3Emitter) +
                    l3.ribbons.size() * sizeof(sfx::SpellFxL3Ribbon));
    }
    h.textureOffset = static_cast<uint32_t>(blob.size());
    for (uint16_t t : texUsed) put<uint16_t>(blob, t);
    alignTo(blob, 4);
    h.poolOffset = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), pool.b.begin(), pool.b.end());
    // The level-3 pool follows the base pool (both end 4-aligned).
    alignTo(blob, 4);
    l3bake::relocate(l3, static_cast<uint32_t>(blob.size()));
    blob.insert(blob.end(), l3.pool.b.begin(), l3.pool.b.end());
    alignTo(blob, 16);
    h.vertexOffset = static_cast<uint32_t>(blob.size());
    h.vertexCount = static_cast<uint32_t>(verts.size());
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(verts.data()),
                reinterpret_cast<const uint8_t*>(verts.data() + verts.size()));
    alignTo(blob, 16);
    h.size = static_cast<uint32_t>(blob.size());
    for (auto& t : tracks) t.offset += h.poolOffset;
    for (auto& g : outGroups)
        if (g.uvOffset) g.uvOffset = g.uvOffset - 1u + h.poolOffset;
    for (auto& e : outEmitters)
        for (auto& c : e.curveOffset) c += h.poolOffset;
    std::memcpy(blob.data(), &h, sizeof(h));
    if (v2) std::memcpy(blob.data() + sizeof(h), &x, sizeof(x));
    if (!tracks.empty()) std::memcpy(blob.data() + h.trackOffset, tracks.data(), tracks.size() * sizeof(tracks[0]));
    if (!outGroups.empty())
        std::memcpy(blob.data() + h.groupOffset, outGroups.data(), outGroups.size() * sizeof(outGroups[0]));
    if (!outEmitters.empty())
        std::memcpy(blob.data() + h.emitterOffset, outEmitters.data(), outEmitters.size() * sizeof(outEmitters[0]));
    if (!outRibbons.empty())
        std::memcpy(blob.data() + x.ribbonOffset, outRibbons.data(), outRibbons.size() * sizeof(outRibbons[0]));
    if (level3) {
        if (!l3.emitters.empty())
            std::memcpy(blob.data() + x.level3Offset, l3.emitters.data(), l3.emitters.size() * sizeof(l3.emitters[0]));
        if (!l3.ribbons.empty())
            std::memcpy(blob.data() + x.level3Offset + l3.emitters.size() * sizeof(l3.emitters[0]), l3.ribbons.data(),
                        l3.ribbons.size() * sizeof(l3.ribbons[0]));
    }
    st.level3 = level3;
    if (!sfx::spellFxValid(blob.data(), static_cast<uint32_t>(blob.size()))) {
        st.why = "self-check failed";
        return false;
    }
    st.ok = true;
    st.version = h.version;
    st.bytes = blob.size();
    st.vertices = verts.size();
    st.groups = outGroups.size();
    st.tracks = tracks.size();
    st.emitters = outEmitters.size();
    st.textures = texUsed.size();
    return true;
}

// ---------------------------------------------------------------------------------------------
// psp_model_conv: the world's emitters ("pidx")

// upstream M2ModelGPU flags (classifyM2Model) -> the PSP's ParticleClass bits.
inline uint8_t ptcClassOf(const rendering::M2ClassificationResult& c) {
    uint8_t b = 0;
    if (c.isSpellEffect || c.isInstancePortal) b |= sfx::kPtcSpell;
    if (c.isFireflyEffect) b |= sfx::kPtcFirefly;
    if (c.isLanternLike || c.isTorch || c.isBrazierOrFire || c.isKoboldFlame) b |= sfx::kPtcFlame;
    if (c.isSmoke) b |= sfx::kPtcSmoke;
    if (c.isInstancePortal) b |= sfx::kPtcSpin;
    return b;
}

struct ParticleBankOptions {
    bool enabled = true;
    int texMax = 64;          // --ptc-tex: particle textures (their own set: sprites are small on the PSP)
    FxBakeOptions fx = [] {   // world models: every emitter up to kFxMaxEmitters (16)
        FxBakeOptions o;
        o.maxEmitters = sfx::kFxMaxEmitters;
        return o;
    }();
    bool list = false;        // --ptc-list: one line per effect
};

class ParticleBank {
public:
    ParticleBank(std::string root, const ParticleBankOptions& o)
        : root_(std::move(root)), opt_(o), tex_(root_, o.texMax, texItems_) {}

    // The effect of a model (its emitters / ribbons / smoke; the mesh only for an upstream spell
    // effect, which the WMDL bake skips), or -1. psp_model_conv calls it once per model path.
    int bakeModel(const std::string& path, const std::vector<uint8_t>& m2Bytes, pipeline::M2Model& m,
                  const rendering::M2ClassificationResult& cls) {
        if (!opt_.enabled || cls.isInvisibleTrap || cls.isSkyBird) return -1;
        const uint8_t pc = ptcClassOf(cls);
        const bool spellMesh = (pc & sfx::kPtcSpell) && !m.vertices.empty() && !m.batches.empty();
        if (m.particleEmitters.empty() && m.ribbonEmitters.empty() && !spellMesh && !(pc & sfx::kPtcSmoke)) return -1;
        FxBakeOptions fo = opt_.fx;
        fo.mesh = (pc & sfx::kPtcSpell) != 0;
        fo.forceV2 = true;
        fo.cls = pc;
        fo.attach = sfx::kFxAttachNone;
        std::vector<uint8_t> blob;
        FxBakeStats st;
        const std::string root = root_;
        auto read = [&root](const std::string& gp, std::vector<uint8_t>& out) { return readFile(root + "/" + lower(gp), out); };
        std::set<std::string> missing;
        const bool ok = bakeFx(path, m2Bytes, m, fo, tex_, read, missing, blob, st);
        missing_.insert(missing.begin(), missing.end());
        if (!ok) {
            ++failed_[st.why];
            if (opt_.list) std::printf("    ptc %s: not baked (%s)\n", path.c_str(), st.why.c_str());
            return -1;
        }
        Effect e;
        e.blob = std::move(blob);
        e.reach = st.reach;
        e.cls = pc;
        e.flags = static_cast<uint8_t>((st.groups ? sfx::kPtcEffectMesh : 0) | (st.level3 ? sfx::kPtcEffectLevel3 : 0));
        level3Effects_ += st.level3 ? 1 : 0;
        e.emitters = static_cast<uint8_t>(st.emitters);
        e.ribbons = static_cast<uint8_t>(st.ribbonsKept);
        const int id = static_cast<int>(effects_.size());
        bytes_ += e.blob.size();
        emitters_ += st.emitters;
        sourceEmitters_ += st.sourceEmitters;
        ribbons_ += st.ribbonsKept;
        meshes_ += st.groups ? 1 : 0;
        effects_.push_back(std::move(e));
        if (opt_.list || st.sourceEmitters > static_cast<size_t>(fo.maxEmitters))
            std::printf("    ptc e_%d %s cls=%s%s%s%s%s emit=%zu/%zu ribbons=%zu/%zu groups=%zu tex=%zu reach=%.1f kb=%.1f\n", id,
                        path.c_str(), (pc & sfx::kPtcSpell) ? "S" : "-", (pc & sfx::kPtcFlame) ? "F" : "-",
                        (pc & sfx::kPtcFirefly) ? "Y" : "-", (pc & sfx::kPtcSmoke) ? "K" : "-",
                        (pc & sfx::kPtcSpin) ? "P" : "-", st.emitters, st.sourceEmitters, st.ribbonsKept, st.ribbons,
                        st.groups, st.textures, st.reach, st.bytes / 1024.0);
        return id;
    }

    // A placed emitter model (upstream placement: world = axes * model + pos), listed for its tile.
    void place(int tileX, int tileY, uint32_t uniqueId, int effect, const glm::mat3& axes, const glm::vec3& pos) {
        if (effect < 0) return;
        sfx::ParticlePlacement p{};
        p.uniqueId = uniqueId;
        p.effect = static_cast<uint16_t>(effect);
        for (int a = 0; a < 3; ++a) p.position[a] = pos[a];
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) p.axes[c * 3 + r] = axes[c][r];
        tiles_[{tileX, tileY}].push_back(p);
        ++placements_;
    }

    // Game objects: the display's model has an effect.
    void addDisplay(uint32_t displayId, int effect) {
        if (effect >= 0) displays_[displayId] = static_cast<uint16_t>(effect);
    }

    size_t effects() const { return effects_.size(); }
    // The effect draws the model's own mesh (WSFX groups) - the WMDL must not draw it again.
    bool effectHasMesh(int id) const {
        return id >= 0 && static_cast<size_t>(id) < effects_.size() && (effects_[id].flags & sfx::kPtcEffectMesh) != 0;
    }
    size_t placements() const { return placements_; }

    // The "pidx" entry (psp_particle_format.hpp); false when there is nothing to write.
    bool build(PakItem& out) {
        if (effects_.empty()) return false;
        std::vector<uint8_t> b(sizeof(sfx::ParticleIndexHeader), 0);
        sfx::ParticleIndexHeader h{};
        std::memcpy(h.magic, "WPFX", 4);
        h.version = sfx::kParticleIndexVersion;
        h.flags = level3Effects_ ? sfx::kPtcIndexLevel3 : 0;
        h.tileCount = static_cast<uint32_t>(tiles_.size());
        h.displayCount = static_cast<uint32_t>(displays_.size());
        h.effectCount = static_cast<uint32_t>(effects_.size());
        h.textureCount = static_cast<uint32_t>(texItems_.size());
        h.placementCount = static_cast<uint32_t>(placements_);
        h.tileOffset = static_cast<uint32_t>(b.size());
        b.resize(b.size() + tiles_.size() * sizeof(sfx::ParticleTileRec));
        h.displayOffset = static_cast<uint32_t>(b.size());
        b.resize(b.size() + displays_.size() * sizeof(sfx::ParticleDisplayRec));
        h.effectOffset = static_cast<uint32_t>(b.size());
        b.resize(b.size() + effects_.size() * sizeof(sfx::ParticleEffectRec));
        h.textureOffset = static_cast<uint32_t>(b.size());
        b.resize(b.size() + texItems_.size() * sizeof(sfx::ParticleTextureRec));
        alignTo(b, 64);
        h.directoryBytes = static_cast<uint32_t>(b.size());
        std::vector<sfx::ParticleTileRec> tiles;
        for (const auto& [key, list] : tiles_) {  // std::map: sorted by (x, y)
            alignTo(b, 64);
            sfx::ParticleTileRec t{};
            t.x = static_cast<int16_t>(key.first);
            t.y = static_cast<int16_t>(key.second);
            t.offset = static_cast<uint32_t>(b.size());
            t.count = static_cast<uint32_t>(list.size());
            b.insert(b.end(), reinterpret_cast<const uint8_t*>(list.data()),
                     reinterpret_cast<const uint8_t*>(list.data() + list.size()));
            tiles.push_back(t);
        }
        std::vector<sfx::ParticleDisplayRec> disp;
        for (const auto& [id, fx] : displays_) disp.push_back(sfx::ParticleDisplayRec{id, fx, 0});  // sorted
        std::vector<sfx::ParticleEffectRec> fx;
        for (const Effect& e : effects_) {
            alignTo(b, 64);
            sfx::ParticleEffectRec r{};
            r.offset = static_cast<uint32_t>(b.size());
            r.size = static_cast<uint32_t>(e.blob.size());
            r.reach = e.reach;
            r.cls = e.cls;
            r.flags = e.flags;
            r.emitters = e.emitters;
            r.ribbons = e.ribbons;
            b.insert(b.end(), e.blob.begin(), e.blob.end());
            fx.push_back(r);
        }
        std::vector<sfx::ParticleTextureRec> tex;
        for (const PakItem& t : texItems_) {  // in id order (TextureBank pushes them so)
            alignTo(b, 64);
            tex.push_back(sfx::ParticleTextureRec{static_cast<uint32_t>(b.size()), static_cast<uint32_t>(t.blob.size())});
            b.insert(b.end(), t.blob.begin(), t.blob.end());
            texBytes_ += t.blob.size();
        }
        alignTo(b, 64);
        h.size = static_cast<uint32_t>(b.size());
        std::memcpy(b.data(), &h, sizeof(h));
        if (!tiles.empty()) std::memcpy(b.data() + h.tileOffset, tiles.data(), tiles.size() * sizeof(tiles[0]));
        if (!disp.empty()) std::memcpy(b.data() + h.displayOffset, disp.data(), disp.size() * sizeof(disp[0]));
        std::memcpy(b.data() + h.effectOffset, fx.data(), fx.size() * sizeof(fx[0]));
        if (!tex.empty()) std::memcpy(b.data() + h.textureOffset, tex.data(), tex.size() * sizeof(tex[0]));
        if (!sfx::particleIndexValid(b.data(), h.directoryBytes, h.size)) {
            std::fprintf(stderr, "particles: pidx self-check failed\n");
            return false;
        }
        out.name = "pidx";
        out.kind = 3;
        out.blob = std::move(b);
        return true;
    }

    void printStats() const {
        size_t texBytes = 0;
        for (const PakItem& t : texItems_) texBytes += t.blob.size();
        std::printf("particles: %zu effects (%zu KB, %zu emitters of %zu, %zu ribbons, %zu with a mesh, %zu with level-3 data), %zu placements in"
                    " %zu tiles, %zu game object displays, %zu textures (%zu KB, max %d px)\n",
                    effects_.size(), bytes_ / 1024, emitters_, sourceEmitters_, ribbons_, meshes_, level3Effects_, placements_,
                    tiles_.size(), displays_.size(), texItems_.size(), texBytes / 1024, opt_.texMax);
        for (const auto& [why, n] : failed_) std::printf("  particles not baked: %d %s\n", n, why.c_str());
        if (!missing_.empty()) std::printf("  particles: %zu game files missing (first: %s)\n", missing_.size(), missing_.begin()->c_str());
    }

private:
    struct Effect {
        std::vector<uint8_t> blob;
        float reach = 0.0f;
        uint8_t cls = 0, flags = 0, emitters = 0, ribbons = 0;
    };
    std::string root_;
    ParticleBankOptions opt_;
    std::vector<PakItem> texItems_;
    TextureBank tex_;
    std::vector<Effect> effects_;
    std::map<std::pair<int, int>, std::vector<sfx::ParticlePlacement>> tiles_;
    std::map<uint32_t, uint16_t> displays_;
    std::map<std::string, int> failed_;
    std::set<std::string> missing_;
    size_t placements_ = 0, bytes_ = 0, texBytes_ = 0, emitters_ = 0, sourceEmitters_ = 0, ribbons_ = 0, meshes_ = 0;
    size_t level3Effects_ = 0;  // effects with the WSFX level-3 section
};

// psp_model_conv's hooks reach the bank through this (null: --particles 0, or another converter).
inline ParticleBank* gParticleBank = nullptr;

// --verify: the "pidx" entry of a pak (when it has one): first in the TOC, the directory, every
// placement list, effect (spellFxValid, its textures) and texture (WTEX). Returns the errors.
inline int verifyParticleIndex(const std::string& path) {
    std::vector<uint8_t> d;
    if (!readFile(path, d) || d.size() < 16 + 64 || std::memcmp(d.data(), "WPAK", 4) != 0) return 0;
    uint32_t count, tocOff;
    std::memcpy(&count, d.data() + 8, 4);
    std::memcpy(&tocOff, d.data() + 12, 4);
    int errors = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* r = d.data() + tocOff + static_cast<size_t>(i) * 64;
        if (tocOff + static_cast<size_t>(i) * 64 + 64 > d.size()) break;
        char nm[49] = {};
        std::memcpy(nm, r, 48);
        if (std::strcmp(nm, "pidx") != 0) continue;
        if (i != 0) {
            std::fprintf(stderr, "verify: pidx is not the first TOC record (the PSP reads record 0)\n");
            ++errors;
        }
        uint32_t off, size;
        std::memcpy(&off, r + 48, 4);
        std::memcpy(&size, r + 52, 4);
        if (static_cast<uint64_t>(off) + size > d.size() || size < sizeof(sfx::ParticleIndexHeader)) {
            std::fprintf(stderr, "verify: pidx out of the file\n");
            return errors + 1;
        }
        const uint8_t* b = d.data() + off;
        sfx::ParticleIndexHeader h;
        std::memcpy(&h, b, sizeof(h));
        if (!sfx::particleIndexValid(b, std::min<uint32_t>(size, h.directoryBytes), size)) {
            std::fprintf(stderr, "verify: pidx directory invalid\n");
            return errors + 1;
        }
        const auto* tiles = reinterpret_cast<const sfx::ParticleTileRec*>(b + h.tileOffset);
        size_t placements = 0;
        for (uint32_t t = 0; t < h.tileCount; ++t) {
            const auto* p = reinterpret_cast<const sfx::ParticlePlacement*>(b + tiles[t].offset);
            for (uint32_t k = 0; k < tiles[t].count; ++k, ++placements) {
                bool ok = p[k].effect < h.effectCount;
                for (float v : p[k].position) ok = ok && std::isfinite(v);
                for (float v : p[k].axes) ok = ok && std::isfinite(v);
                if (!ok && ++errors <= 20) std::fprintf(stderr, "verify: tile %d_%d placement %u bad\n", tiles[t].x, tiles[t].y, k);
            }
        }
        const auto* fx = reinterpret_cast<const sfx::ParticleEffectRec*>(b + h.effectOffset);
        size_t emit = 0, rib = 0, mesh = 0, maxKb = 0;
        for (uint32_t e = 0; e < h.effectCount; ++e) {
            const uint8_t* eb = b + fx[e].offset;
            if (!sfx::spellFxValid(eb, fx[e].size)) {
                if (++errors <= 20) std::fprintf(stderr, "verify: effect %u invalid\n", e);
                continue;
            }
            sfx::SpellFxHeader eh;
            std::memcpy(&eh, eb, sizeof(eh));
            const sfx::SpellFxHeaderExt* x = sfx::spellFxExt(eb);
            if (!x || x->cls != fx[e].cls || x->reach != fx[e].reach) {
                if (++errors <= 20) std::fprintf(stderr, "verify: effect %u: v2 extension / directory mismatch\n", e);
                continue;
            }
            for (uint32_t t = 0; t < eh.textureCount; ++t) {
                uint16_t id;
                std::memcpy(&id, eb + eh.textureOffset + t * 2, 2);
                if (id >= h.textureCount && ++errors <= 20) std::fprintf(stderr, "verify: effect %u texture %u missing\n", e, id);
            }
            emit += eh.emitterCount;
            rib += x->ribbonCount;
            mesh += eh.groupCount ? 1 : 0;
            maxKb = std::max<size_t>(maxKb, fx[e].size);
        }
        const auto* tx = reinterpret_cast<const sfx::ParticleTextureRec*>(b + h.textureOffset);
        size_t texBytes = 0;
        for (uint32_t t = 0; t < h.textureCount; ++t) {
            if (std::memcmp(b + tx[t].offset, "WTEX", 4) != 0 && ++errors <= 20) std::fprintf(stderr, "verify: texture %u not WTEX\n", t);
            texBytes += tx[t].size;
        }
        std::printf("verify particles: pidx %u KB (directory %u B): %u tiles, %zu placements, %u displays, %u effects"
                    " (%zu emitters, %zu ribbons, %zu with a mesh, largest %zu B), %u textures (%zu KB) errors=%d\n",
                    size / 1024, h.directoryBytes, h.tileCount, placements, h.displayCount, h.effectCount, emit, rib, mesh,
                    maxKb, h.textureCount, texBytes / 1024, errors);
        return errors;
    }
    return errors;
}

} // namespace wowee::pspconv
