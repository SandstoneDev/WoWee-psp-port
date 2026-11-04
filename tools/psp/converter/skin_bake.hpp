// M9: skinned character bake for the GE (WSKN, include/rendering/psp/
// psp_model_format.hpp) and its offline check against upstream-style CPU
// skinning (verifySkinned).
//
// Bone rules are upstream CharacterRenderer's: local = T(pivot) * T * R * S *
// T(-pivot) from rendering/m2_track_sampler.hpp, world = parent * local, M2
// vertex bone indices are absolute bones, weights / 255.
#pragma once

#include "rendering/m2_track_sampler.hpp"
#include "rendering/psp/psp_skeleton.hpp"

#include "model_bake.hpp"

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <set>

namespace wowee::pspconv {

inline bool g_verbose = false;  // per-frame verify errors
// --palette: bones per GE draw (4..8); fewer = cheaper vertices, more draws.
inline int g_maxPalette = fmt::kSkinMaxPalette;
using fmt::kSkinMaxPalette;
using fmt::kSkinScaleOne;
using fmt::kSkinWeightOne;

// upstream CharacterRenderer::getBoneTransform: T(pivot)*T*R*S*T(-pivot).
inline glm::mat4 boneLocal(const pipeline::M2Model& m, const pipeline::M2Bone& b, int seq, float timeMs) {
    namespace tr = rendering::m2_track;
    const glm::vec3 t = tr::sampleVec3(b.translation, seq, timeMs, 0.0f, m.globalSequenceDurations, glm::vec3(0.0f));
    const glm::quat q = tr::sampleQuat(b.rotation, seq, timeMs, 0.0f, m.globalSequenceDurations);
    const glm::vec3 s = tr::sampleVec3(b.scale, seq, timeMs, 0.0f, m.globalSequenceDurations, glm::vec3(1.0f));
    const glm::mat3 r = glm::mat3_cast(q);
    const glm::vec3 c0 = r[0] * s.x, c1 = r[1] * s.y, c2 = r[2] * s.z;
    const glm::vec3 tt = (b.pivot + t) - (c0 * b.pivot.x + c1 * b.pivot.y + c2 * b.pivot.z);
    glm::mat4 out;
    out[0] = glm::vec4(c0, 0.0f);
    out[1] = glm::vec4(c1, 0.0f);
    out[2] = glm::vec4(c2, 0.0f);
    out[3] = glm::vec4(tt, 1.0f);
    return out;
}

// Every bone of the model (upstream CharacterRenderer::calculateBoneMatrices).
inline void poseBones(const pipeline::M2Model& m, int seq, float timeMs, std::vector<glm::mat4>& bones) {
    bones.assign(m.bones.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const auto& b = m.bones[i];
        const glm::mat4 local = boneLocal(m, b, seq, timeMs);
        bones[i] = (b.parentBone >= 0 && static_cast<size_t>(b.parentBone) < i) ? bones[b.parentBone] * local : local;
    }
}

// CPU skinning of one vertex with its M2 weights (normalised by their sum,
// as the stand-pose bake does; upstream's weights sum to 255 anyway).
inline bool skinVertex(const pipeline::M2Vertex& v, const std::vector<glm::mat4>& bones, glm::vec3& p, glm::vec3* n) {
    glm::vec3 sp(0.0f), sn(0.0f);
    float wsum = 0.0f;
    for (int k = 0; k < 4; ++k) {
        const float w = v.boneWeights[k] / 255.0f;
        if (w <= 0.0f || v.boneIndices[k] >= bones.size()) continue;
        const glm::mat4& bm = bones[v.boneIndices[k]];
        sp += w * glm::vec3(bm * glm::vec4(v.position, 1.0f));
        sn += w * (glm::mat3(bm) * v.normal);
        wsum += w;
    }
    if (wsum <= 0.0f) return false;
    p = sp / wsum;
    if (n) *n = sn;
    return true;
}

// One source batch: triangles over the M2 vertex array + its material.
struct SkinBatchIn {
    std::vector<uint32_t> tris;  // 3 source vertex indices per triangle
    fmt::ModelBatch material{};  // texture, blend, flags (ranges are rewritten)
    uint16_t geoset = 0;
};

struct SkinBakeInfo {
    size_t bones = 0, sequences = 0, subBatches = 0, vertices = 0, triangles = 0, keys = 0;
    size_t vertexBytes = 0, animBytes = 0, trimmedInfluences = 0, weightless = 0;
    std::array<size_t, fmt::kSkinMaxPalette> paletteHistogram{};
    std::vector<uint32_t> emittedSource;  // source vertex per emitted vertex, in blob order (verify)
    float standHeight = 0.0f;
};

namespace skin_detail {

struct Influences {
    int n = 0;
    uint16_t bone[4] = {};
    float w[4] = {};
};

inline Influences influencesOf(const pipeline::M2Vertex& v, size_t boneCount) {
    Influences in;
    for (int k = 0; k < 4; ++k) {
        if (v.boneWeights[k] == 0 || v.boneIndices[k] >= boneCount) continue;
        int j = 0;
        while (j < in.n && in.bone[j] != v.boneIndices[k]) ++j;
        if (j == in.n) {
            in.bone[in.n] = v.boneIndices[k];
            in.w[in.n++] = 0.0f;
        }
        in.w[j] += v.boneWeights[k] / 255.0f;
    }
    // Heaviest first.
    for (int a = 0; a < in.n; ++a)
        for (int b = a + 1; b < in.n; ++b)
            if (in.w[b] > in.w[a]) {
                std::swap(in.w[a], in.w[b]);
                std::swap(in.bone[a], in.bone[b]);
            }
    return in;
}

// Normalised weights -> u8 with 128 = 1.0, summing to exactly 128.
inline void quantizeWeights(const Influences& in, uint8_t out[4]) {
    float sum = 0.0f;
    for (int k = 0; k < in.n; ++k) sum += in.w[k];
    int q[4] = {}, total = 0;
    float frac[4] = {};
    for (int k = 0; k < in.n; ++k) {
        const float x = in.w[k] / sum * kSkinWeightOne;
        q[k] = static_cast<int>(std::floor(x));
        frac[k] = x - q[k];
        total += q[k];
    }
    while (total < 128) {
        int best = 0;
        for (int k = 1; k < in.n; ++k)
            if (frac[k] > frac[best]) best = k;
        ++q[best];
        frac[best] = -1.0f;
        ++total;
    }
    for (int k = 0; k < 4; ++k) out[k] = static_cast<uint8_t>(k < in.n ? q[k] : 0);
}

} // namespace skin_detail

// Builds the WSKN blob. `bind`: the M2 vertices in the bind pose; `model`: the
// M2 with the sequences in `sequenceIndices` loaded (.anim files merged);
// `standSeq`: Stand (lighting and standHeight), -1 = bind pose.
inline std::vector<uint8_t> bakeSkinned(const pipeline::M2Model& model, const std::vector<pipeline::M2Vertex>& bind,
                                        const std::vector<SkinBatchIn>& batches,
                                        const std::vector<int>& sequenceIndices, int standSeq, float rate,
                                        SkinBakeInfo& info, bool animOnly = false,
                                        const std::vector<uint16_t>* keepBones = nullptr) {
    using namespace skin_detail;
    info = SkinBakeInfo{};
    const size_t nBonesM2 = model.bones.size();
    std::vector<uint8_t> blob;
    if (nBonesM2 == 0 || batches.empty()) return blob;

    // Influences of every used vertex; weightless vertices follow bone 0
    // (upstream's shader would collapse them to the origin).
    std::vector<Influences> infl(bind.size());
    std::vector<uint8_t> used(bind.size(), 0);
    for (const auto& b : batches)
        for (uint32_t v : b.tris) used[v] = 1;
    for (size_t v = 0; v < bind.size(); ++v) {
        if (!used[v]) continue;
        infl[v] = influencesOf(bind[v], nBonesM2);
        if (infl[v].n == 0) {
            infl[v].n = 1;
            infl[v].bone[0] = 0;
            infl[v].w[0] = 1.0f;
            ++info.weightless;
        }
    }
    auto triBones = [&](const uint32_t* t, std::set<uint16_t>& s) {
        s.clear();
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < infl[t[c]].n; ++k) s.insert(infl[t[c]].bone[k]);
    };
    // A triangle over more than 8 bones: drop its lightest influence (the
    // vertex keeps the rest, renormalised) until it fits one palette.
    std::set<uint16_t> tb;
    for (const auto& b : batches) {
        for (size_t t = 0; t + 2 < b.tris.size(); t += 3) {
            for (triBones(&b.tris[t], tb); tb.size() > static_cast<size_t>(g_maxPalette); triBones(&b.tris[t], tb)) {
                int bestV = -1, bestK = -1;
                float bestW = 2.0f;
                for (int c = 0; c < 3; ++c) {
                    const Influences& in = infl[b.tris[t + c]];
                    if (in.n > 1 && in.w[in.n - 1] < bestW) {
                        bestW = in.w[in.n - 1];
                        bestV = static_cast<int>(b.tris[t + c]);
                        bestK = in.n - 1;
                    }
                }
                if (bestV < 0) break;
                infl[bestV].n = bestK;
                ++info.trimmedInfluences;
            }
        }
    }

    // Partition each batch into sub-batches of <= 8 bones (greedy, in skin
    // triangle order: neighbouring triangles share bones).
    struct Sub {
        size_t batch;
        std::vector<uint32_t> tris;
        std::array<int, kSkinMaxPalette> slot;  // slot -> M2 bone
        int n = 0;
    };
    std::vector<Sub> subs;
    std::vector<std::pair<uint32_t, uint32_t>> batchRange(batches.size());
    std::array<int, kSkinMaxPalette> prevSlot;
    prevSlot.fill(-1);
    for (size_t bi = 0; bi < batches.size(); ++bi) {
        std::vector<uint32_t> remaining = batches[bi].tris;
        batchRange[bi].first = static_cast<uint32_t>(subs.size());
        while (remaining.size() >= 3) {
            std::set<uint16_t> pal, u;
            std::vector<uint32_t> take, rest;
            for (size_t t = 0; t + 2 < remaining.size(); t += 3) {
                triBones(&remaining[t], tb);
                u = pal;
                u.insert(tb.begin(), tb.end());
                auto& dst = u.size() <= static_cast<size_t>(g_maxPalette) ? take : rest;
                if (&dst == &take) pal.swap(u);
                dst.insert(dst.end(), remaining.begin() + t, remaining.begin() + t + 3);
            }
            if (take.empty()) break;  // cannot happen after the trim above
            Sub s;
            s.batch = bi;
            s.tris = std::move(take);
            s.n = static_cast<int>(pal.size());
            // Slots: a bone the previous palette had in a slot < n stays there
            // (the PSP skips re-uploading an unchanged slot); the rest fill up.
            // (A bone can sit in two stale slots of prevSlot: keep the first.)
            s.slot.fill(-1);
            std::set<int> kept;
            for (int j = 0; j < s.n; ++j)
                if (prevSlot[j] >= 0 && pal.count(static_cast<uint16_t>(prevSlot[j])) && kept.insert(prevSlot[j]).second)
                    s.slot[j] = prevSlot[j];
            for (uint16_t bone : pal) {
                bool placed = false;
                for (int j = 0; j < s.n && !placed; ++j) placed = s.slot[j] == bone;
                for (int j = 0; j < s.n && !placed; ++j)
                    if (s.slot[j] < 0) {
                        s.slot[j] = bone;
                        placed = true;
                    }
            }
            for (int j = 0; j < s.n; ++j) {
                if (s.slot[j] < 0) {
                    std::fprintf(stderr, "  palette slot %d unfilled\n", j);
                    return {};
                }
                prevSlot[j] = s.slot[j];
            }
            ++info.paletteHistogram[s.n - 1];
            subs.push_back(std::move(s));
            remaining.swap(rest);
        }
        batchRange[bi].second = static_cast<uint32_t>(subs.size()) - batchRange[bi].first;
    }

    // Bones: those the palettes use plus ancestors, M2 order (parents first).
    std::vector<uint8_t> need(nBonesM2, 0);
    for (const auto& s : subs)
        for (int j = 0; j < s.n; ++j)
            for (int b = s.slot[j]; b >= 0 && !need[b]; b = model.bones[b].parentBone) need[b] = 1;
    // Bones the caller needs posed although no vertex uses them
    // (attachment points), with their ancestors.
    if (keepBones)
        for (uint16_t kb : *keepBones)
            for (int b = kb; b >= 0 && static_cast<size_t>(b) < nBonesM2 && !need[b]; b = model.bones[b].parentBone)
                need[b] = 1;
    std::vector<int> toSkin(nBonesM2, -1);
    std::vector<uint16_t> skinBones;
    for (size_t b = 0; b < nBonesM2; ++b) {
        if (!need[b]) continue;
        const int parent = model.bones[b].parentBone;
        if (parent >= 0 && (static_cast<size_t>(parent) >= b || toSkin[parent] < 0)) {
            std::fprintf(stderr, "  bone %zu: parent %d not before it\n", b, parent);
            return {};
        }
        toSkin[b] = static_cast<int>(skinBones.size());
        skinBones.push_back(static_cast<uint16_t>(b));
    }
    if (skinBones.size() > 255) {
        std::fprintf(stderr, "  %zu bones > 255\n", skinBones.size());
        return {};
    }

    // Stand pose, frame 0: lighting normals and the height.
    std::vector<glm::mat4> stand;
    if (standSeq >= 0) poseBones(model, standSeq, 0.0f, stand);
    const glm::vec3 sun = glm::normalize(glm::vec3(0.35f, 0.25f, 0.9f));
    std::vector<uint8_t> unlit(bind.size(), 0);
    for (const auto& b : batches)
        if (b.material.flags & fmt::kBatchUnlit)
            for (uint32_t v : b.tris) unlit[v] = 1;
    glm::vec3 pmin(1e9f), pmax(-1e9f);
    glm::vec2 tmin(1e9f), tmax(-1e9f);
    float standTop = -1e9f;
    std::vector<uint16_t> light(bind.size(), 0xFFFF);
    for (size_t v = 0; v < bind.size(); ++v) {
        if (!used[v]) continue;
        pmin = glm::min(pmin, bind[v].position);
        pmax = glm::max(pmax, bind[v].position);
        tmin = glm::min(tmin, bind[v].texCoords[0]);
        tmax = glm::max(tmax, bind[v].texCoords[0]);
        glm::vec3 p = bind[v].position, n = bind[v].normal;
        if (!stand.empty()) skinVertex(bind[v], stand, p, &n);
        standTop = std::max(standTop, p.z);
        glm::vec3 l(1.0f);
        if (!unlit[v]) {
            const float nl = glm::length(n);
            const glm::vec3 nn = nl > 1e-4f ? n / nl : glm::vec3(0, 0, 1);
            l = glm::vec3(std::min(1.0f, 0.5f + 0.5f * std::max(0.0f, glm::dot(nn, sun))));
        }
        light[v] = to565(Rgb{l.r, l.g, l.b, 1.0f});
    }
    const glm::vec3 center = (pmin + pmax) * 0.5f;
    const glm::vec3 half = glm::max((pmax - pmin) * 0.5f, glm::vec3(0.01f));
    const glm::vec2 trange = glm::max(tmax - tmin, glm::vec2(1e-4f));
    float radius = 0.0f;
    for (size_t v = 0; v < bind.size(); ++v)
        if (used[v]) radius = std::max(radius, glm::length(bind[v].position - center));
    info.standHeight = standTop;

    // Vertices: one strip per sub-batch (meshoptimizer, degenerate joins).
    std::vector<uint8_t> vbytes;
    std::vector<fmt::SkinSubBatch> outSubs;
    for (const auto& s : subs) {
        std::unordered_map<uint32_t, uint32_t> local;
        std::vector<uint32_t> src;
        std::vector<unsigned int> tri;
        for (uint32_t v : s.tris) {
            auto it = local.find(v);
            if (it == local.end()) {
                it = local.emplace(v, static_cast<uint32_t>(src.size())).first;
                src.push_back(v);
            }
            tri.push_back(it->second);
        }
        meshopt_optimizeVertexCacheStrip(tri.data(), tri.data(), tri.size(), src.size());
        std::vector<unsigned int> strip(meshopt_stripifyBound(tri.size()));
        strip.resize(meshopt_stripify(strip.data(), tri.data(), tri.size(), src.size(), 0));
        if (strip.size() > 65535) {
            std::fprintf(stderr, "  sub-batch strip too long\n");
            return {};
        }
        alignTo(vbytes, 16);
        fmt::SkinSubBatch sb{};
        sb.vertexOffset = static_cast<uint32_t>(vbytes.size());  // relative for now
        sb.vertexCount = static_cast<uint16_t>(strip.size());
        sb.weights = static_cast<uint8_t>(s.n);
        for (int j = 0; j < s.n; ++j) sb.palette[j] = static_cast<uint8_t>(toSkin[s.slot[j]]);
        const int stride = fmt::skinVertexStride(s.n);
        for (unsigned int li : strip) {
            const uint32_t v = src[li];
            const Influences& in = infl[v];
            uint8_t q[4];
            quantizeWeights(in, q);
            std::vector<uint8_t> vert(static_cast<size_t>(stride), 0);
            for (int k = 0; k < in.n; ++k)
                for (int j = 0; j < s.n; ++j)
                    if (s.slot[j] == in.bone[k]) vert[j] = static_cast<uint8_t>(vert[j] + q[k]);
            size_t o = static_cast<size_t>((s.n + 1) & ~1);
            const glm::vec2 uv = bind[v].texCoords[0];
            const glm::vec3 p = (bind[v].position - center) / half;
            auto w16 = [&](uint16_t x) {
                std::memcpy(&vert[o], &x, 2);
                o += 2;
            };
            w16(static_cast<uint16_t>(std::lround(std::clamp((uv.x - tmin.x) / trange.x, 0.0f, 1.0f) * 65535.0f)));
            w16(static_cast<uint16_t>(std::lround(std::clamp((uv.y - tmin.y) / trange.y, 0.0f, 1.0f) * 65535.0f)));
            w16(light[v]);
            for (int a = 0; a < 3; ++a)
                w16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(p[a], -1.0f, 1.0f) * 32767.0f))));
            vbytes.insert(vbytes.end(), vert.begin(), vert.end());
            info.emittedSource.push_back(v);
        }
        info.triangles += s.tris.size() / 3;
        info.vertices += strip.size();
        outSubs.push_back(sb);
    }

    // Sequences: every bone sampled with the upstream sampler at a fixed rate.
    struct Sample {
        glm::quat r;
        glm::vec3 t, s;
    };
    const size_t nb = skinBones.size();
    std::vector<fmt::AnimSequence> seqs;
    std::vector<std::vector<Sample>> samples;  // [seq][bone * frames + k]
    float maxT = 1e-6f;
    for (int si : sequenceIndices) {
        const auto& ms = model.sequences[si];
        const uint32_t k = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(ms.duration * rate / 1000.0f)));
        fmt::AnimSequence as{};
        as.animId = static_cast<uint16_t>(ms.id);
        as.variation = static_cast<uint16_t>(ms.variationIndex);
        as.durationMs = ms.duration;
        as.frameCount = static_cast<uint16_t>(std::min<uint32_t>(k + 1u, 65535u));
        as.flags = static_cast<uint16_t>(ms.flags);
        as.movingSpeed = ms.movingSpeed;
        as.frequency = ms.frequency;
        as.replayMin = static_cast<uint16_t>(std::min<uint32_t>(ms.replayMin, 65535u));
        as.replayMax = static_cast<uint16_t>(std::min<uint32_t>(ms.replayMax, 65535u));
        as.blendTimeMs = static_cast<uint16_t>(std::min<uint32_t>(ms.blendTime, 65535u));
        const uint32_t frames = as.frameCount;
        std::vector<Sample> sm(nb * frames);
        for (size_t b = 0; b < nb; ++b) {
            const auto& bone = model.bones[skinBones[b]];
            namespace tr = rendering::m2_track;
            for (uint32_t f = 0; f < frames; ++f) {
                const float t = static_cast<float>(ms.duration) * static_cast<float>(f) / static_cast<float>(frames - 1);
                Sample& x = sm[b * frames + f];
                x.t = tr::sampleVec3(bone.translation, si, t, 0.0f, model.globalSequenceDurations, glm::vec3(0.0f));
                x.r = tr::sampleQuat(bone.rotation, si, t, 0.0f, model.globalSequenceDurations);
                x.s = tr::sampleVec3(bone.scale, si, t, 0.0f, model.globalSequenceDurations, glm::vec3(1.0f));
                if (f > 0 && glm::dot(x.r, sm[b * frames + f - 1].r) < 0.0f) x.r = -x.r;
                for (int a = 0; a < 3; ++a) maxT = std::max(maxT, std::fabs(x.t[a]));
            }
        }
        seqs.push_back(as);
        samples.push_back(std::move(sm));
    }
    const float tScale = maxT / 32767.0f;
    using Key = std::array<int16_t, 4>;
    std::vector<Key> keys;
    std::map<Key, uint32_t> constKeys;
    auto q16 = [](float x) { return static_cast<int16_t>(std::lround(std::clamp(x, -32767.0f, 32767.0f))); };
    std::vector<fmt::BoneTrack> tracks;
    for (size_t si = 0; si < seqs.size(); ++si) {
        const uint32_t frames = seqs[si].frameCount;
        for (size_t b = 0; b < nb; ++b) {
            fmt::BoneTrack tr{fmt::kTrackNone, fmt::kTrackNone, fmt::kTrackNone};
            for (int comp = 0; comp < 3; ++comp) {
                std::vector<Key> ks(frames);
                for (uint32_t f = 0; f < frames; ++f) {
                    const Sample& x = samples[si][b * frames + f];
                    if (comp == 0)
                        ks[f] = {q16(x.r.x * 32767.0f), q16(x.r.y * 32767.0f), q16(x.r.z * 32767.0f),
                                 q16(x.r.w * 32767.0f)};
                    else if (comp == 1)
                        ks[f] = {q16(x.t.x / tScale), q16(x.t.y / tScale), q16(x.t.z / tScale), 0};
                    else
                        ks[f] = {q16(x.s.x * kSkinScaleOne), q16(x.s.y * kSkinScaleOne), q16(x.s.z * kSkinScaleOne), 0};
                }
                bool constant = true;
                for (uint32_t f = 1; f < frames && constant; ++f) constant = ks[f] == ks[0];
                const Key def = comp == 0 ? Key{0, 0, 0, 32767}
                                          : comp == 1 ? Key{0, 0, 0, 0}
                                                      : Key{4096, 4096, 4096, 0};
                const Key defNeg{0, 0, 0, -32767};
                uint32_t ref = fmt::kTrackNone;
                if (constant && (ks[0] == def || (comp == 0 && ks[0] == defNeg))) {
                    ref = fmt::kTrackNone;
                } else if (constant) {
                    auto it = constKeys.find(ks[0]);
                    if (it == constKeys.end()) {
                        it = constKeys.emplace(ks[0], static_cast<uint32_t>(keys.size())).first;
                        keys.push_back(ks[0]);
                    }
                    ref = it->second;
                } else {
                    ref = static_cast<uint32_t>(keys.size()) | fmt::kTrackAnimated;
                    keys.insert(keys.end(), ks.begin(), ks.end());
                }
                (comp == 0 ? tr.rotation : comp == 1 ? tr.translation : tr.scale) = ref;
            }
            tracks.push_back(tr);
        }
    }

    // Assemble.
    fmt::SkinHeader h{};
    std::memcpy(h.magic, animOnly ? "WSKA" : "WSKN", 4);
    h.version = fmt::kSkinFormatVersion;
    h.boneCount = static_cast<uint16_t>(nb);
    h.sequenceCount = static_cast<uint16_t>(seqs.size());
    h.batchCount = animOnly ? 0 : static_cast<uint16_t>(batches.size());
    h.subBatchCount = animOnly ? 0 : static_cast<uint16_t>(outSubs.size());
    for (int a = 0; a < 3; ++a) {
        h.center[a] = center[a];
        h.half[a] = half[a];
    }
    h.uvMin[0] = tmin.x;
    h.uvMin[1] = tmin.y;
    h.uvScale[0] = trange.x * 32768.0f / 65535.0f;
    h.uvScale[1] = trange.y * 32768.0f / 65535.0f;
    h.standHeight = standTop;
    h.radius = radius;
    h.translationScale = tScale;
    h.sampleRate = rate;
    blob.resize(sizeof(h));
    auto section = [&](const void* p, size_t bytes) {
        alignTo(blob, 4);
        const uint32_t off = static_cast<uint32_t>(blob.size());
        blob.insert(blob.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + bytes);
        return off;
    };
    std::vector<fmt::SkinBone> bonesOut(nb);
    for (size_t b = 0; b < nb; ++b) {
        const auto& mb = model.bones[skinBones[b]];
        bonesOut[b].parent = static_cast<int16_t>(mb.parentBone >= 0 ? toSkin[mb.parentBone] : -1);
        bonesOut[b].m2Bone = skinBones[b];
        for (int a = 0; a < 3; ++a) bonesOut[b].pivot[a] = mb.pivot[a];
    }
    h.boneOffset = section(bonesOut.data(), bonesOut.size() * sizeof(fmt::SkinBone));
    h.sequenceOffset = section(seqs.data(), seqs.size() * sizeof(fmt::AnimSequence));
    if (animOnly) {  // WSKA: skeleton + sequences only (emotes, loaded on demand)
        h.batchOffset = h.geosetOffset = h.subBatchOffset = h.sequenceOffset;
        h.trackOffset = section(tracks.data(), tracks.size() * sizeof(fmt::BoneTrack));
        h.keyOffset = section(keys.data(), keys.size() * sizeof(Key));
        h.keyCount = static_cast<uint32_t>(keys.size());
        alignTo(blob, 4);
        std::memcpy(blob.data(), &h, sizeof(h));
        info.bones = nb;
        info.sequences = seqs.size();
        info.keys = keys.size();
        info.animBytes = tracks.size() * sizeof(fmt::BoneTrack) + keys.size() * sizeof(Key);
        return blob;
    }
    std::vector<fmt::ModelBatch> mbs(batches.size());
    std::vector<uint16_t> geos(batches.size());
    for (size_t b = 0; b < batches.size(); ++b) {
        mbs[b] = batches[b].material;
        mbs[b].indexStart = batchRange[b].first;
        mbs[b].indexCount = batchRange[b].second;
        geos[b] = batches[b].geoset;
    }
    h.batchOffset = section(mbs.data(), mbs.size() * sizeof(fmt::ModelBatch));
    h.geosetOffset = section(geos.data(), geos.size() * sizeof(uint16_t));
    h.subBatchOffset = section(outSubs.data(), outSubs.size() * sizeof(fmt::SkinSubBatch));
    h.trackOffset = section(tracks.data(), tracks.size() * sizeof(fmt::BoneTrack));
    h.keyOffset = section(keys.data(), keys.size() * sizeof(Key));
    h.keyCount = static_cast<uint32_t>(keys.size());
    alignTo(blob, 16);
    const uint32_t vbase = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), vbytes.begin(), vbytes.end());
    h.vertexBytes = static_cast<uint32_t>(vbytes.size());
    for (size_t i = 0; i < outSubs.size(); ++i) {
        fmt::SkinSubBatch sb = outSubs[i];
        sb.vertexOffset += vbase;
        std::memcpy(&blob[h.subBatchOffset + i * sizeof(sb)], &sb, sizeof(sb));
    }
    std::memcpy(blob.data(), &h, sizeof(h));

    info.bones = nb;
    info.sequences = seqs.size();
    info.subBatches = outSubs.size();
    info.keys = keys.size();
    info.vertexBytes = vbytes.size();
    info.animBytes = tracks.size() * sizeof(fmt::BoneTrack) + keys.size() * sizeof(Key);
    return blob;
}

// Offline proof of the PSP path: decode the blob exactly as the GE sees it
// (s16 / 32768, u8 weights / 128, bone matrices from psp_skeleton.hpp with the
// dequantisation folded in) and compare every emitted vertex with upstream-
// style CPU skinning of its source vertex (m2_track sampler at the same
// time, all M2 bones). Times off the baked frame grid measure the resampling.
struct SkinVerifyResult {
    double maxErr = 0.0, sumSq = 0.0;
    size_t samples = 0;
    std::string worst;
};

// Posed triangles for the offline renderer: float xyz[3] + u16 geoset + u16 pad.
inline void appendTri(std::vector<uint8_t>& dump, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                      uint16_t geoset) {
    for (const glm::vec3* p : {&a, &b, &c})
        for (int k = 0; k < 3; ++k) put<float>(dump, (*p)[k]);
    put<uint16_t>(dump, geoset);
    put<uint16_t>(dump, 0);
}

inline glm::vec3 geSkin(const fmt::SkinView& view, const std::vector<fmt::BoneMatrix>& pose,
                        const fmt::SkinSubBatch& sb, const uint8_t* vert) {
    int16_t xyz[3];
    std::memcpy(xyz, vert + ((sb.weights + 1) & ~1) + 6, 6);
    const glm::vec3 pn(xyz[0] / 32768.0f, xyz[1] / 32768.0f, xyz[2] / 32768.0f);
    glm::vec3 out(0.0f);
    for (int j = 0; j < sb.weights; ++j) {
        const float w = vert[j] / kSkinWeightOne;
        if (w == 0.0f) continue;
        float g[12];
        fmt::geBoneMatrix(pose[sb.palette[j]], *view.h, g);
        for (int r = 0; r < 3; ++r) out[r] += w * (g[r] * pn.x + g[3 + r] * pn.y + g[6 + r] * pn.z + g[9 + r]);
    }
    return out;
}

inline SkinVerifyResult verifySkinned(const std::vector<uint8_t>& blob, const pipeline::M2Model& model,
                                      const std::vector<pipeline::M2Vertex>& bind,
                                      const std::vector<int>& sequenceIndices, const SkinBakeInfo& info,
                                      const std::vector<SkinBatchIn>& batches, std::vector<uint8_t>* dump,
                                      const std::set<uint16_t>* dumpGeosets) {
    SkinVerifyResult r;
    fmt::SkinView view;
    if (!view.init(blob.data(), static_cast<uint32_t>(blob.size()))) {
        r.maxErr = 1e9;
        r.worst = "blob rejected by SkinView::init";
        return r;
    }
    std::vector<fmt::BoneMatrix> pose(view.h->boneCount);
    std::vector<glm::mat4> ref;
    const float fractions[] = {0.0f, 0.13f, 0.37f, 0.5f, 0.71f, 0.93f, 0.999f};
    for (int s = 0; s < view.h->sequenceCount; ++s) {
        const int si = sequenceIndices[s];
        const float dur = static_cast<float>(view.sequences[s].durationMs);
        for (float fr : fractions) {
            const float t = dur * fr;
            fmt::evaluatePose(view, s, t, pose.data());
            poseBones(model, si, t, ref);
            size_t e = 0;
            double seqMax = 0.0;
            for (int b = 0; b < view.h->batchCount; ++b) {
                const fmt::ModelBatch& mb = view.batches[b];
                const bool dumpIt = dump && fr == 0.37f && (!dumpGeosets || dumpGeosets->count(view.geosets[b]));
                for (uint32_t k = 0; k < mb.indexCount; ++k) {
                    const fmt::SkinSubBatch& sb = view.subBatches[mb.indexStart + k];
                    const uint8_t* verts = view.vertices(sb);
                    const int stride = fmt::skinVertexStride(sb.weights);
                    std::vector<glm::vec3> ours(sb.vertexCount);
                    for (uint32_t i = 0; i < sb.vertexCount; ++i, ++e) {
                        ours[i] = geSkin(view, pose, sb, verts + static_cast<size_t>(i) * stride);
                        glm::vec3 want;
                        if (!skinVertex(bind[info.emittedSource[e]], ref, want, nullptr))
                            want = bind[info.emittedSource[e]].position;
                        const double err = glm::length(ours[i] - want);
                        r.sumSq += err * err;
                        ++r.samples;
                        seqMax = std::max(seqMax, err);
                        if (err > r.maxErr) {
                            r.maxErr = err;
                            char w[160];
                            std::snprintf(w, sizeof(w), "anim %u t=%.0f ms vertex %u: %.4f yd", view.sequences[s].animId,
                                          t, info.emittedSource[e], err);
                            r.worst = w;
                        }
                    }
                    if (dumpIt)
                        for (uint32_t i = 2; i < sb.vertexCount; ++i)
                            if (ours[i] != ours[i - 1] && ours[i] != ours[i - 2] && ours[i - 1] != ours[i - 2])
                                appendTri(*dump, ours[i - 2], ours[i - 1], ours[i], static_cast<uint16_t>(s * 2 + 1));
                }
                // Reference triangles of the same batch (source mesh, CPU skinning).
                if (dumpIt) {
                    const auto& tris = batches[b].tris;
                    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
                        glm::vec3 p[3];
                        for (int c = 0; c < 3; ++c)
                            if (!skinVertex(bind[tris[t + c]], ref, p[c], nullptr)) p[c] = bind[tris[t + c]].position;
                        appendTri(*dump, p[0], p[1], p[2], static_cast<uint16_t>(s * 2));
                    }
                }
            }
            if (g_verbose) std::printf("    anim %u t=%.0f: max %.4f\n", view.sequences[s].animId, t, seqMax);
        }
    }
    return r;
}

} // namespace wowee::pspconv
