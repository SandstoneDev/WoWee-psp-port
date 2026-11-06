// Skeletal animation evaluation for WSKN blobs (M9), shared by the PSP
// renderer (src/rendering/psp/psp_character.cpp) and the converter's
// --verify check (tools/psp/converter/psp_char_conv.cpp): the PC proves the
// exact math the PSP runs. Plain C++, no platform headers.
//
// Per frame: sample each bone's baked keys (linear, nlerp for rotations),
// build upstream's local matrix T(pivot) * T * R * S * T(-pivot) and compose
// it with the parent (parents come first). The result maps a bind-pose
// model-space point to the animated model space, like upstream
// CharacterRenderer::boneMatrices. geBoneMatrix() then folds the vertex
// dequantisation in, which is what sceGuBoneMatrix receives.
#pragma once

#include "platform/psp/psp_vmath.hpp"  // vrsq on the PSP (vfpumath), 1/sqrt elsewhere
#include "rendering/psp/psp_model_format.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace wowee::rendering::psp {

// Affine 3x4, column-major: m[0..2] x axis, m[3..5] y axis, m[6..8] z axis,
// m[9..11] translation.
struct BoneMatrix {
    float m[12];
};

// PSP: works on pose matrices (12 floats) or already folded GE matrices (16).
// Call after pose/crossfade evaluation; no per-vertex CPU skinning or allocation.
inline void skinBlendPaletteMatrix(const SkinPaletteBlend& blend, const float* matrices,
                                  uint32_t stride, float* out, uint32_t components) {
    for (uint32_t i = 0; i < components; ++i) out[i] = 0.0f;
    unsigned total = 0;
    for (unsigned i = 0; i < 4; ++i) total += blend.weights[i];
    if (!total) return;  // used zero-sum slots are rejected by SkinView
    const float inverse = 1.0f / static_cast<float>(total);
    for (unsigned k = 0; k < 4; ++k) {
        if (!blend.weights[k]) continue;
        const float factor = static_cast<float>(blend.weights[k]) * inverse;
        const float* matrix = matrices + static_cast<uint32_t>(blend.bones[k]) * stride;
        for (uint32_t i = 0; i < components; ++i) out[i] += matrix[i] * factor;
    }
}

// Checked view of a WSKN blob. init() validates every offset and key index,
// so a damaged pak entry is rejected instead of read out of bounds.
struct SkinView {
    const uint8_t* blob = nullptr;
    const SkinHeader* h = nullptr;
    const SkinBone* bones = nullptr;
    const AnimSequence* sequences = nullptr;
    const ModelBatch* batches = nullptr;
    const uint16_t* geosets = nullptr;
    const SkinSubBatch* subBatches = nullptr;
    const BoneTrack* tracks = nullptr;
    const int16_t* keys = nullptr;  // 4 per key
    const SkinPaletteBlend* virtualPalettes = nullptr;
    const SkinTimingSequence* timings = nullptr;

    const uint32_t* sequenceTimes(uint32_t sequence) const {
        return timings ? reinterpret_cast<const uint32_t*>(blob + timings[sequence].offset) : nullptr;
    }

    const SkinPaletteBlend* paletteBlend(const SkinSubBatch& sb, unsigned slot) const {
        if (!(sb.reserved & kSkinVirtualPalette)) return nullptr;
        return virtualPalettes + static_cast<size_t>(&sb - subBatches) * kSkinMaxPalette + slot;
    }

    // animOnly: a "WSKA" blob (extra sequences, no vertices) whose skeleton
    // must match `bonesOf` (the model's WSKN).
    bool init(const void* data, uint32_t size, bool animOnly = false, uint16_t bonesOf = 0) {
        *this = SkinView{};
        if (!data || size < sizeof(SkinHeader)) return false;
        const auto* p = static_cast<const uint8_t*>(data);
        const auto* hh = reinterpret_cast<const SkinHeader*>(p);
        if (std::memcmp(hh->magic, animOnly ? "WSKA" : "WSKN", 4) != 0 ||
            (hh->version != kSkinFormatVersion && hh->version != kSkinFloatScaleVersion &&
             hh->version != kSkinPrecisionVersion && hh->version != kSkinTimedVersion))
            return false;
        if (hh->boneCount == 0 || hh->boneCount > 255 || (!animOnly && hh->batchCount == 0)) return false;
        if ((hh->flags & kSkinFloatPosition) && hh->version < kSkinFloatScaleVersion) return false;
        if ((hh->flags & kSkinHasVirtualPalette) && (hh->version < kSkinPrecisionVersion || animOnly)) return false;
        if ((hh->flags & (kSkinSlerpRotation | kSkinFloatRotation)) && hh->version != kSkinTimedVersion) return false;
        if (animOnly && (hh->boneCount != bonesOf || hh->batchCount != 0 || hh->subBatchCount != 0)) return false;
        auto fits = [&](uint32_t off, uint64_t bytes) {
            return (off & 3u) == 0 && off <= size && bytes <= static_cast<uint64_t>(size - off) &&
                !(hh->version == kSkinTimedVersion && bytes && off < sizeof(SkinHeader) + sizeof(SkinTimingExtension));
        };
        const uint32_t nb = hh->boneCount, ns = hh->sequenceCount;
        if (!fits(hh->boneOffset, uint64_t{nb} * sizeof(SkinBone)) ||
            !fits(hh->sequenceOffset, uint64_t{ns} * sizeof(AnimSequence)) ||
            !fits(hh->batchOffset, uint64_t{hh->batchCount} * sizeof(ModelBatch)) ||
            !fits(hh->geosetOffset, uint64_t{hh->batchCount} * 2u) ||
            !fits(hh->subBatchOffset, uint64_t{hh->subBatchCount} * sizeof(SkinSubBatch)) ||
            !fits(hh->trackOffset, uint64_t{ns} * nb * sizeof(BoneTrack)) ||
            !fits(hh->keyOffset, uint64_t{hh->keyCount} * 8u))
            return false;
        const auto* bn = reinterpret_cast<const SkinBone*>(p + hh->boneOffset);
        for (uint32_t i = 0; i < nb; ++i)
            if (bn[i].parent >= static_cast<int>(i) || bn[i].parent < -1) return false;
        const auto* sq = reinterpret_cast<const AnimSequence*>(p + hh->sequenceOffset);
        const auto* tr = reinterpret_cast<const BoneTrack*>(p + hh->trackOffset);
        const SkinTimingSequence* tm = nullptr;
        if (hh->version == kSkinTimedVersion) {
            if (size < sizeof(SkinHeader) + sizeof(SkinTimingExtension)) return false;
            const auto* ex = reinterpret_cast<const SkinTimingExtension*>(p + sizeof(SkinHeader));
            const uint32_t at = ex->timingOffset;
            if (ex->reserved || !fits(at, sizeof(SkinTimingHeader) + uint64_t{ns} * sizeof(SkinTimingSequence)))
                return false;
            const auto* th = reinterpret_cast<const SkinTimingHeader*>(p + at);
            if (std::memcmp(th->magic, "WTIM", 4) || th->version != 1 || th->sequenceCount != ns) return false;
            tm = reinterpret_cast<const SkinTimingSequence*>(p + at + sizeof(SkinTimingHeader));
        }
        for (uint32_t s = 0; s < ns; ++s) {
            if (sq[s].frameCount < 2) return false;
            if (tm) {
                if (tm[s].count != sq[s].frameCount || !fits(tm[s].offset, uint64_t{tm[s].count} * 4u)) return false;
                const auto* times = reinterpret_cast<const uint32_t*>(p + tm[s].offset);
                if (times[0] != 0 || times[tm[s].count - 1u] != sq[s].durationMs) return false;
                if (!sq[s].durationMs) {
                    if (tm[s].count != 2) return false;
                } else {
                    for (uint32_t k = 1; k < tm[s].count; ++k) if (times[k] <= times[k - 1u]) return false;
                }
            }
            for (uint32_t i = 0; i < nb; ++i) {
                const BoneTrack& t = tr[s * nb + i];
                const uint32_t refs[] = {t.rotation, t.translation, t.scale};
                for (int component = 0; component < 3; ++component) {
                    const uint32_t ref = refs[component];
                    if (ref == kTrackNone) continue;
                    const bool compactT = hh->version == kSkinTimedVersion && component == 1 &&
                        (ref & kTrackCompactTranslation) != 0;
                    const uint32_t first = ref & ~(kTrackAnimated | (compactT ? kTrackCompactTranslation : 0u));
                    uint32_t n = (ref & kTrackAnimated) ? sq[s].frameCount : 1u;
                    // PSP: scale has float xyz rather than three clipped int16s in v2.
                    const bool floatScale = (hh->version >= kSkinFloatScaleVersion && component == 2) ||
                        (hh->version == kSkinTimedVersion && component == 1 && !compactT);
                    const bool floatRotation = component == 0 && (hh->flags & kSkinFloatRotation);
                    if (floatScale || floatRotation) n *= 2u;
                    if (first > hh->keyCount || n > hh->keyCount - first) return false;
                    if (compactT) {
                        if (!(hh->translationScale > 0.0f) || !std::isfinite(hh->translationScale)) return false;
                        for (uint32_t k = 0; k < n; ++k) {
                            int16_t words[4];
                            std::memcpy(words, p + hh->keyOffset + (first + k) * 8u, sizeof(words));
                            const uint16_t code = static_cast<uint16_t>(words[3]);
                            if (code & 0x8000u) return false;
                            for (unsigned axis = 0; axis < 3; ++axis) {
                                const unsigned exponent = (code >> (axis * 5u)) & 31u;
                                const float step = hh->translationScale / static_cast<float>(uint32_t{1} << exponent);
                                if (!std::isfinite(words[axis] * step)) return false;
                            }
                        }
                    }
                    if (floatScale)
                        for (uint32_t k = 0; k < n; k += 2) {
                            float xyz[3];
                            std::memcpy(xyz, p + hh->keyOffset + (first + k) * 8u, sizeof(xyz));
                            for (float x : xyz) if (!std::isfinite(x)) return false;
                        }
                    if (floatRotation)
                        for (uint32_t k = 0; k < n; k += 2) {
                            float q[4];
                            std::memcpy(q, p + hh->keyOffset + (first + k) * 8u, sizeof(q));
                            float length2 = 0;
                            for (float c : q) {
                                if (!std::isfinite(c)) return false;
                                length2 += c * c;
                            }
                            if (!(length2 > 1e-12f) || !std::isfinite(length2)) return false;
                        }
                }
            }
        }
        const auto* sb = reinterpret_cast<const SkinSubBatch*>(p + hh->subBatchOffset);
        const SkinPaletteBlend* vp = nullptr;
        if (hh->flags & kSkinHasVirtualPalette) {
            const uint32_t at = hh->reserved[2];
            const uint64_t bytes = sizeof(SkinPaletteHeader) +
                uint64_t{hh->subBatchCount} * kSkinMaxPalette * sizeof(SkinPaletteBlend);
            if (!fits(at, bytes)) return false;
            const auto* ph = reinterpret_cast<const SkinPaletteHeader*>(p + at);
            if (std::memcmp(ph->magic, "WVPL", 4) || ph->version != 1 || ph->slots != kSkinMaxPalette ||
                ph->subBatchCount != hh->subBatchCount) return false;
            vp = reinterpret_cast<const SkinPaletteBlend*>(p + at + sizeof(SkinPaletteHeader));
        }
        for (uint32_t i = 0; i < hh->subBatchCount; ++i) {
            if ((sb[i].reserved & ~(kSkinWeights16 | kSkinVirtualPalette)) ||
                (sb[i].reserved && hh->version < kSkinPrecisionVersion) ||
                ((sb[i].reserved & kSkinVirtualPalette) && !vp)) return false;
            if (sb[i].weights < 1 || sb[i].weights > kSkinMaxPalette || (sb[i].vertexOffset & 15u) ||
                !fits(sb[i].vertexOffset, sb[i].vertexCount * static_cast<uint32_t>(
                    skinVertexStride(sb[i].weights, (hh->flags & kSkinFloatPosition) != 0, sb[i].reserved))))
                return false;
            for (int j = 0; j < sb[i].weights; ++j) {
                if (sb[i].reserved & kSkinVirtualPalette) {
                    const auto& blend = vp[i * kSkinMaxPalette + j];
                    unsigned sum = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (blend.weights[k] && blend.bones[k] >= nb) return false;
                        sum += blend.weights[k];
                    }
                    if (!sum) return false;
                } else if (sb[i].palette[j] >= nb) return false;
            }
        }
        const auto* bt = reinterpret_cast<const ModelBatch*>(p + hh->batchOffset);
        for (uint32_t i = 0; i < hh->batchCount; ++i)
            if (bt[i].indexStart > hh->subBatchCount || bt[i].indexCount > hh->subBatchCount - bt[i].indexStart)
                return false;
        blob = p;
        h = hh;
        bones = bn;
        sequences = sq;
        batches = bt;
        geosets = reinterpret_cast<const uint16_t*>(p + hh->geosetOffset);
        subBatches = sb;
        tracks = tr;
        keys = reinterpret_cast<const int16_t*>(p + hh->keyOffset);
        virtualPalettes = vp;
        timings = tm;
        return true;
    }

    // Upstream CharacterRenderer::playAnimation: variation 0 of the id,
    // else its first sequence; -1 when the model has none.
    int findSequence(uint32_t animId) const {
        int first = -1;
        for (int i = 0; h && i < h->sequenceCount; ++i) {
            if (sequences[i].animId != animId) continue;
            if (sequences[i].variation == 0) return i;
            if (first < 0) first = i;
        }
        return first;
    }

    const uint8_t* vertices(const SkinSubBatch& sb) const { return blob + sb.vertexOffset; }
};

namespace skel_detail {

inline void key4(const int16_t* keys, uint32_t ref, uint32_t k0, float f, float* out) {
    const int16_t* a = keys + static_cast<size_t>(ref & ~kTrackAnimated) * 4u;
    if (!(ref & kTrackAnimated)) {
        for (int c = 0; c < 4; ++c) out[c] = a[c];
        return;
    }
    a += k0 * 4u;
    const int16_t* b = a + 4;
    for (int c = 0; c < 4; ++c) out[c] = a[c] + (b[c] - a[c]) * f;
}

} // namespace skel_detail

namespace skel_detail {

inline void keyFloat3(const int16_t* keys, uint32_t ref, uint32_t frame, float factor, float out[3]) {
    const int16_t* a = keys + static_cast<size_t>(ref & ~kTrackAnimated) * 4u;
    const bool animated = (ref & kTrackAnimated) != 0;
    if (animated) a += frame * 8u;
    float first[3], next[3];
    std::memcpy(first, a, sizeof(first));
    std::memcpy(next, animated ? a + 8 : a, sizeof(next));
    for (int c = 0; c < 3; ++c) out[c] = first[c] + (next[c] - first[c]) * factor;
}

// PSP: exact interpolation is paid only by clips that explicitly request it.
// Float keys also remove the int16 angular precision floor on long lever arms.
inline void keyRotation(const int16_t* keys, uint32_t ref, uint32_t frame, float factor,
                        bool floats, bool slerp, float out[4]) {
    const bool animated = (ref & kTrackAnimated) != 0;
    const unsigned stride = floats ? 8u : 4u;  // int16 words
    const int16_t* a = keys + static_cast<size_t>(ref & ~kTrackAnimated) * 4u;
    if (animated) a += frame * stride;
    const int16_t* b = animated ? a + stride : a;
    float qa[4], qb[4];
    if (floats) {
        std::memcpy(qa, a, sizeof(qa));std::memcpy(qb, b, sizeof(qb));
    } else {
        for (unsigned c = 0; c < 4; ++c) {qa[c] = a[c];qb[c] = b[c];}
    }
    if (!slerp) {
        for (unsigned c = 0; c < 4; ++c) out[c] = qa[c] + (qb[c] - qa[c]) * factor;
        return;
    }
    const auto unit = [](float q[4]) {
        const float length2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
        if (!(length2 > 1e-12f)) {q[0]=q[1]=q[2]=0;q[3]=1;return;}
        const float inverse = 1.0f / std::sqrt(length2);
        for (unsigned c = 0; c < 4; ++c) q[c] *= inverse;
    };
    unit(qa);unit(qb);
    float cosine = 0;
    for (unsigned c = 0; c < 4; ++c) cosine += qa[c] * qb[c];
    if (cosine < 0) {cosine = -cosine;for (float& c : qb) c = -c;}
    if (cosine > 1) cosine = 1;
    if (!animated || factor <= 0) {std::memcpy(out, qa, sizeof(qa));return;}
    if (factor >= 1) {std::memcpy(out, qb, sizeof(qb));return;}
    // Match the source sampler's epsilon, including very small rotations.
    if (cosine > 1.0f - std::numeric_limits<float>::epsilon()) {
        for (unsigned c = 0; c < 4; ++c) out[c] = qa[c] + (qb[c] - qa[c]) * factor;
    } else {
        const float angle = std::acos(cosine), denominator = std::sin(angle);
        const float first = std::sin((1.0f - factor) * angle) / denominator;
        const float next = std::sin(factor * angle) / denominator;
        for (unsigned c = 0; c < 4; ++c) out[c] = qa[c] * first + qb[c] * next;
    }
}

inline void keyCompactTranslation(const int16_t* keys, uint32_t ref, uint32_t frame,
                                  float factor, float baseScale, float out[3]) {
    const int16_t* a = keys + static_cast<size_t>(ref & ~(kTrackAnimated | kTrackCompactTranslation)) * 4u;
    const bool animated = (ref & kTrackAnimated) != 0;
    if (animated) a += frame * 4u;
    const int16_t* b = animated ? a + 4 : a;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto decode = [&](const int16_t* key) {
            const unsigned exponent = (static_cast<uint16_t>(key[3]) >> (axis * 5u)) & 31u;
            return key[axis] * (baseScale / static_cast<float>(uint32_t{1} << exponent));
        };
        const float first = decode(a), next = decode(b);
        out[axis] = first + (next - first) * factor;
    }
}

// Local rotation / translation / scale of every bone of one sequence at one
// time (keys of the frame pair around it, linear between them).
struct Sampler {
    const SkinView* v = nullptr;
    const BoneTrack* tr = nullptr;
    uint32_t k0 = 0;
    float f = 0.0f;

    bool init(const SkinView& view, int seq, float timeMs) {
        v = nullptr;
        if (!view.h || seq < 0 || seq >= view.h->sequenceCount) return false;
        const AnimSequence& s = view.sequences[seq];
        const uint32_t last = s.frameCount - 1u;
        if (const uint32_t* times = view.sequenceTimes(static_cast<uint32_t>(seq))) {
            float time = timeMs;
            if (!(time > 0.0f)) time = 0.0f;  // also NaN
            if (time > s.durationMs) time = static_cast<float>(s.durationMs);
            uint32_t lo = 0, hi = last;
            while (lo + 1u < hi) {
                const uint32_t mid = lo + (hi - lo) / 2u;
                if (times[mid] <= time) lo = mid;
                else hi = mid;
            }
            k0 = lo;
            const uint32_t span = times[lo + 1u] - times[lo];
            f = span ? (time - static_cast<float>(times[lo])) / static_cast<float>(span) : 0.0f;
        } else {
            float x = s.durationMs > 0 ? timeMs * static_cast<float>(last) / static_cast<float>(s.durationMs) : 0.0f;
            if (!(x > 0.0f)) x = 0.0f;
            if (x > static_cast<float>(last)) x = static_cast<float>(last);
            k0 = static_cast<uint32_t>(x);
            if (k0 >= last) k0 = last - 1u;
            f = x - static_cast<float>(k0);
        }
        tr = view.tracks + static_cast<size_t>(seq) * view.h->boneCount;
        v = &view;
        return true;
    }

    void sample(uint32_t i, float q[4], float tv[3], float sv[3]) const {
        const BoneTrack& t = tr[i];
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        tv[0] = tv[1] = tv[2] = 0.0f;
        sv[0] = sv[1] = sv[2] = 1.0f;
        float k[4];
        if (t.rotation != kTrackNone) {
            const bool exact = (v->h->flags & (kSkinSlerpRotation | kSkinFloatRotation)) != 0;
            if (exact) keyRotation(v->keys, t.rotation, k0, f,
                                  (v->h->flags & kSkinFloatRotation) != 0,
                                  (v->h->flags & kSkinSlerpRotation) != 0, k);
            else key4(v->keys, t.rotation, k0, f, k);
            const float len2 = k[0] * k[0] + k[1] * k[1] + k[2] * k[2] + k[3] * k[3];
            if (len2 > 1e-12f) {
                const float inv = exact ? 1.0f / std::sqrt(len2) : platform::psp::vmath::rsqrt(len2);
                for (int c = 0; c < 4; ++c) q[c] = k[c] * inv;
            }
        }
        if (t.translation != kTrackNone) {
            if (v->h->version == kSkinTimedVersion && (t.translation & kTrackCompactTranslation))
                keyCompactTranslation(v->keys, t.translation, k0, f, v->h->translationScale, tv);
            else if (v->h->version == kSkinTimedVersion) keyFloat3(v->keys, t.translation, k0, f, tv);
            else {
                key4(v->keys, t.translation, k0, f, k);
                for (int c = 0; c < 3; ++c) tv[c] = k[c] * v->h->translationScale;
            }
        }
        if (t.scale != kTrackNone) {
            if (v->h->version >= kSkinFloatScaleVersion) {
                keyFloat3(v->keys, t.scale, k0, f, sv);
            } else {
                key4(v->keys, t.scale, k0, f, k);
                for (int c = 0; c < 3; ++c) sv[c] = k[c] * (1.0f / kSkinScaleOne);
            }
        }
    }
};

// Bone i's matrix from its local q/t/s and its parent's (already in out).
inline void compose(const SkinBone& bone, const float q[4], const float tv[3], const float sv[3], BoneMatrix* out,
                    uint32_t i) {
    // glm::mat3_cast(quat(w, x, y, z)), columns scaled (R * S).
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    const float xy = qx * qy, xz = qx * qz, yz = qy * qz, wx = qw * qx, wy = qw * qy, wz = qw * qz;
    float l[12];
    l[0] = (1.0f - 2.0f * (yy + zz)) * sv[0];
    l[1] = 2.0f * (xy + wz) * sv[0];
    l[2] = 2.0f * (xz - wy) * sv[0];
    l[3] = 2.0f * (xy - wz) * sv[1];
    l[4] = (1.0f - 2.0f * (xx + zz)) * sv[1];
    l[5] = 2.0f * (yz + wx) * sv[1];
    l[6] = 2.0f * (xz + wy) * sv[2];
    l[7] = 2.0f * (yz - wx) * sv[2];
    l[8] = (1.0f - 2.0f * (xx + yy)) * sv[2];
    // Translation: pivot + T - RS * pivot.
    const float* pv = bone.pivot;
    for (int r = 0; r < 3; ++r) l[9 + r] = pv[r] + tv[r] - (l[r] * pv[0] + l[3 + r] * pv[1] + l[6 + r] * pv[2]);
    float* o = out[i].m;
    if (bone.parent < 0) {
        std::memcpy(o, l, sizeof(l));
        return;
    }
    const float* pm = out[bone.parent].m;
    for (int c = 0; c < 4; ++c) {
        const float lx = l[c * 3], ly = l[c * 3 + 1], lz = l[c * 3 + 2];
        for (int r = 0; r < 3; ++r) o[c * 3 + r] = pm[r] * lx + pm[3 + r] * ly + pm[6 + r] * lz;
    }
    for (int r = 0; r < 3; ++r) o[9 + r] += pm[9 + r];
}

} // namespace skel_detail

// Bone matrices of sequence `seq` at `timeMs` (0 .. duration) into
// out[boneCount]. With `from` (a sequence of the same skeleton that was
// playing) and weight w < 1, the local transforms are cross-faded first -
// the 3.3.5a client's blend into a new animation (M2 blendTime): rotations
// nlerp (shortest arc), translation and scale linear.
inline void evaluatePose(const SkinView& v, int seq, float timeMs, BoneMatrix* out,
                         const SkinView* from = nullptr, int fromSeq = -1, float fromTimeMs = 0.0f,
                         float w = 1.0f) {
    const uint32_t nb = v.h->boneCount;
    skel_detail::Sampler cur;
    if (!cur.init(v, seq, timeMs)) {
        for (uint32_t i = 0; i < nb; ++i) {
            std::memset(out[i].m, 0, sizeof(out[i].m));
            out[i].m[0] = out[i].m[4] = out[i].m[8] = 1.0f;
        }
        return;
    }
    skel_detail::Sampler prev;
    const bool blend = from && w < 1.0f && from->h && from->h->boneCount == nb && prev.init(*from, fromSeq, fromTimeMs);
    const bool exactBlend = blend && ((v.h->flags | from->h->flags) &
                                     (kSkinSlerpRotation | kSkinFloatRotation));
    if (!(w > 0.0f)) w = 0.0f;
    for (uint32_t i = 0; i < nb; ++i) {
        float q[4], tv[3], sv[3];
        cur.sample(i, q, tv, sv);
        if (blend) {
            float pq[4], pt[3], ps[3];
            prev.sample(i, pq, pt, ps);
            const float dot = q[0] * pq[0] + q[1] * pq[1] + q[2] * pq[2] + q[3] * pq[3];
            const float wp = (dot < 0.0f ? -1.0f : 1.0f) * (1.0f - w);
            float len2 = 0.0f;
            for (int c = 0; c < 4; ++c) {
                q[c] = q[c] * w + pq[c] * wp;
                len2 += q[c] * q[c];
            }
            if (len2 > 1e-12f) {
                const float inv = exactBlend ? 1.0f / std::sqrt(len2) : platform::psp::vmath::rsqrt(len2);
                for (float& c : q) c *= inv;
            } else {
                q[0] = q[1] = q[2] = 0.0f;
                q[3] = 1.0f;
            }
            for (int c = 0; c < 3; ++c) {
                tv[c] = pt[c] + (tv[c] - pt[c]) * w;
                sv[c] = ps[c] + (sv[c] - ps[c]) * w;
            }
        }
        skel_detail::compose(v.bones[i], q, tv, sv, out, i);
    }
}

// bone * T(center) * S(half): the GE's bone matrix for the s16 vertices
// (the GE reads s16 positions as value / 32768).
inline void geBoneMatrix(const BoneMatrix& b, const SkinHeader& h, float out[12]) {
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) out[c * 3 + r] = b.m[c * 3 + r] * h.half[c];
    for (int r = 0; r < 3; ++r)
        out[9 + r] = b.m[r] * h.center[0] + b.m[3 + r] * h.center[1] + b.m[6 + r] * h.center[2] + b.m[9 + r];
}

} // namespace wowee::rendering::psp
