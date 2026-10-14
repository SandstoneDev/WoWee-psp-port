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

namespace wowee::rendering::psp {

// Affine 3x4, column-major: m[0..2] x axis, m[3..5] y axis, m[6..8] z axis,
// m[9..11] translation.
struct BoneMatrix {
    float m[12];
};

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

    // animOnly: a "WSKA" blob (extra sequences, no vertices) whose skeleton
    // must match `bonesOf` (the model's WSKN).
    bool init(const void* data, uint32_t size, bool animOnly = false, uint16_t bonesOf = 0) {
        *this = SkinView{};
        if (!data || size < sizeof(SkinHeader)) return false;
        const auto* p = static_cast<const uint8_t*>(data);
        const auto* hh = reinterpret_cast<const SkinHeader*>(p);
        if (std::memcmp(hh->magic, animOnly ? "WSKA" : "WSKN", 4) != 0 || hh->version != kSkinFormatVersion)
            return false;
        if (hh->boneCount == 0 || hh->boneCount > 255 || (!animOnly && hh->batchCount == 0)) return false;
        if (animOnly && (hh->boneCount != bonesOf || hh->batchCount != 0 || hh->subBatchCount != 0)) return false;
        auto fits = [&](uint32_t off, uint64_t bytes) {
            return (off & 3u) == 0 && off <= size && bytes <= static_cast<uint64_t>(size - off);
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
        for (uint32_t s = 0; s < ns; ++s) {
            if (sq[s].frameCount < 2) return false;
            for (uint32_t i = 0; i < nb; ++i) {
                const BoneTrack& t = tr[s * nb + i];
                for (uint32_t ref : {t.rotation, t.translation, t.scale}) {
                    if (ref == kTrackNone) continue;
                    const uint32_t first = ref & ~kTrackAnimated;
                    const uint32_t n = (ref & kTrackAnimated) ? sq[s].frameCount : 1u;
                    if (first > hh->keyCount || n > hh->keyCount - first) return false;
                }
            }
        }
        const auto* sb = reinterpret_cast<const SkinSubBatch*>(p + hh->subBatchOffset);
        for (uint32_t i = 0; i < hh->subBatchCount; ++i) {
            if (sb[i].weights < 1 || sb[i].weights > kSkinMaxPalette || (sb[i].vertexOffset & 15u) ||
                !fits(sb[i].vertexOffset, sb[i].vertexCount * static_cast<uint32_t>(skinVertexStride(sb[i].weights))))
                return false;
            for (int j = 0; j < sb[i].weights; ++j)
                if (sb[i].palette[j] >= nb) return false;
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
        float x = s.durationMs > 0 ? timeMs * static_cast<float>(last) / static_cast<float>(s.durationMs) : 0.0f;
        if (!(x > 0.0f)) x = 0.0f;  // also NaN
        if (x > static_cast<float>(last)) x = static_cast<float>(last);
        k0 = static_cast<uint32_t>(x);
        if (k0 >= last) k0 = last - 1u;
        f = x - static_cast<float>(k0);
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
            key4(v->keys, t.rotation, k0, f, k);
            const float len2 = k[0] * k[0] + k[1] * k[1] + k[2] * k[2] + k[3] * k[3];
            if (len2 > 1e-12f) {
                const float inv = platform::psp::vmath::rsqrt(len2);
                for (int c = 0; c < 4; ++c) q[c] = k[c] * inv;
            }
        }
        if (t.translation != kTrackNone) {
            key4(v->keys, t.translation, k0, f, k);
            for (int c = 0; c < 3; ++c) tv[c] = k[c] * v->h->translationScale;
        }
        if (t.scale != kTrackNone) {
            key4(v->keys, t.scale, k0, f, k);
            for (int c = 0; c < 3; ++c) sv[c] = k[c] * (1.0f / kSkinScaleOne);
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
                const float inv = platform::psp::vmath::rsqrt(len2);
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
