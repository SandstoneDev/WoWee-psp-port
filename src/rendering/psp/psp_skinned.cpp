// Shared GE skinning for creatures/NPCs. Copied from CharacterModel
// (psp_character.cpp: playAnimation, startBlend, nextVariation, update,
// evaluatePose, drawPass's sub-batch loop), state moved into SkinAnimState.
#include "rendering/psp/psp_skinned.hpp"

#include "platform/psp/psp_prof.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <pspgu.h>

#include <cmath>

namespace wowee::rendering::psp {

namespace prof = wowee::platform::psp::prof;

namespace {

constexpr int skinnedType(int weights) {
    return GU_WEIGHTS(weights) | GU_WEIGHT_8BIT | GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT |
           GU_TRANSFORM_3D;
}

// CharacterModel::startBlend: the sequence playing now fades out over the
// new one's M2 blendTime (Stand 500 ms, most 150 ms; 150 when the pak has 0).
void startBlend(const SkinView& v, SkinAnimState& s, int newSeq) {
    s.blendFromSeq = -1;
    s.blendMs = 0.0f;
    if (s.sequence < 0 || s.sequence == newSeq) return;
    s.blendFromSeq = s.sequence;
    s.blendFromTimeMs = s.timeMs;
    s.blendElapsedMs = 0.0f;
    const uint16_t ms = v.sequences[newSeq].blendTimeMs;
    s.blendMs = ms ? static_cast<float>(ms) : 150.0f;
}

// CharacterModel::nextVariation: repeat the current variation replayMin..Max
// times, then pick the next one by frequency (probability x 0x7FFF).
bool nextVariation(const SkinView& v, SkinAnimState& s) {
    if (s.repeatsLeft > 0) {
        --s.repeatsLeft;
        return false;
    }
    const auto* seqs = v.sequences;
    int count = 0;
    for (int i = 0; i < v.h->sequenceCount; ++i)
        if (seqs[i].animId == s.animId) ++count;
    if (count < 2) return false;
    s.rng = s.rng * 1664525u + 1013904223u;
    int r = static_cast<int>((s.rng >> 8) % 0x7FFFu);
    int pick = -1;
    for (int i = 0; i < v.h->sequenceCount; ++i) {
        if (seqs[i].animId != s.animId || seqs[i].frequency <= 0) continue;
        if (r < seqs[i].frequency) {
            pick = i;
            break;
        }
        r -= seqs[i].frequency;
    }
    if (pick < 0) pick = v.findSequence(s.animId);
    if (pick < 0) return false;
    bool changed = false;
    if (pick != s.sequence) {
        startBlend(v, s, pick);
        s.sequence = pick;
        changed = true;
    }
    const auto& q = seqs[pick];
    if (q.replayMax > q.replayMin) {
        s.rng = s.rng * 1664525u + 1013904223u;
        s.repeatsLeft = static_cast<uint16_t>(q.replayMin + (s.rng >> 8) % (q.replayMax - q.replayMin + 1u));
    } else {
        s.repeatsLeft = q.replayMin;
    }
    return changed;
}

} // namespace

void skinPlay(const SkinView& v, SkinAnimState& s, uint32_t animId, bool loop, uint32_t resumeAnim) {
    if (!v.h || v.h->sequenceCount == 0) return;
    int seq = v.findSequence(animId);
    if (seq < 0) {
        seq = 0;
        animId = v.sequences[0].animId;
    }
    startBlend(v, s, seq);
    s.sequence = seq;
    s.animId = animId;
    s.timeMs = 0.0f;
    s.loop = loop;
    s.resumeAnim = loop ? 0u : resumeAnim;
    s.repeatsLeft = 0;
}

void skinHoldEnd(const SkinView& v, SkinAnimState& s) {
    if (!v.h || s.sequence < 0) return;
    s.timeMs = static_cast<float>(v.sequences[s.sequence].durationMs);
    s.blendFromSeq = -1;
}

bool skinUpdate(const SkinView& v, SkinAnimState& s, float dtSec) {
    if (!v.h || s.sequence < 0) return false;
    const float dtMs = dtSec * 1000.0f;
    if (!(dtMs > 0.0f)) return false;  // also NaN
    const float duration = static_cast<float>(v.sequences[s.sequence].durationMs);
    // A corpse (Death held on its last frame) does not change any more.
    if (!s.loop && s.animId == anim::DEATH && s.timeMs >= duration && s.blendFromSeq < 0) return false;
    if (s.blendFromSeq >= 0) {
        s.blendElapsedMs += dtMs;
        if (s.blendElapsedMs >= s.blendMs) {
            s.blendFromSeq = -1;
        } else {
            const float fd = static_cast<float>(v.sequences[s.blendFromSeq].durationMs);
            s.blendFromTimeMs += dtMs;
            if (fd > 0.0f && s.blendFromTimeMs >= fd) s.blendFromTimeMs = std::fmod(s.blendFromTimeMs, fd);
        }
    }
    s.timeMs += dtMs;
    if (duration > 0.0f && s.timeMs >= duration) {
        if (s.loop) {
            s.timeMs -= duration;
            while (s.timeMs >= duration) s.timeMs -= duration;
            nextVariation(v, s);
        } else if (s.animId != anim::DEATH) {
            skinPlay(v, s, s.resumeAnim ? s.resumeAnim : anim::STAND, true);
        } else {
            s.timeMs = duration;
        }
    }
    return true;
}

void skinPose(const SkinView& v, const SkinAnimState& s, BoneMatrix* scratch, ScePspFMatrix4* ge) {
    if (s.blendFromSeq >= 0 && s.blendMs > 0.0f) {
        evaluatePose(v, s.sequence, s.timeMs, scratch, &v, s.blendFromSeq, s.blendFromTimeMs,
                     s.blendElapsedMs / s.blendMs);
    } else {
        evaluatePose(v, s.sequence, s.timeMs, scratch);
    }
    for (uint32_t i = 0; i < v.h->boneCount; ++i) {
        float g[12];
        geBoneMatrix(scratch[i], *v.h, g);
        ScePspFMatrix4& m = ge[i];
        m.x = {g[0], g[1], g[2], 0.0f};
        m.y = {g[3], g[4], g[5], 0.0f};
        m.z = {g[6], g[7], g[8], 0.0f};
        m.w = {g[9], g[10], g[11], 1.0f};
    }
}

uint32_t SkinDrawer::drawBatch(const SkinView& v, const ModelBatch& b, const ScePspFMatrix4* ge) {
    uint32_t tris = 0;
    for (uint32_t k = 0; k < b.indexCount; ++k) {
        const SkinSubBatch& sb = v.subBatches[b.indexStart + k];
        if (sb.vertexCount < 3) continue;
        for (int j = 0; j < sb.weights; ++j) {
            if (slotBone_[j] == sb.palette[j]) continue;
            sceGuBoneMatrix(static_cast<unsigned>(j), &ge[sb.palette[j]]);
            slotBone_[j] = sb.palette[j];
            ++boneUploads;
        }
        prof::count(prof::kDraws, 1);
        prof::count(prof::kVertices, sb.vertexCount);
        prof::count(prof::kTriangles, sb.vertexCount - 2u);
        sceGuDrawArray(GU_TRIANGLE_STRIP, skinnedType(sb.weights), sb.vertexCount, nullptr, v.vertices(sb));
        tris += sb.vertexCount - 2u;
        ++draws;
    }
    return tris;
}

} // namespace wowee::rendering::psp
