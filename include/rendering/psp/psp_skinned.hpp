// GE hardware skinning of a WSKN blob for many instances (creature
// and NPC animation). The logic is a copy of CharacterModel's
// (src/rendering/psp/psp_character.cpp, M9/M9b), which stays
// untouched: one animation state per instance instead of per model, so the
// read-only WSKN blob of a creature model is shared by every unit showing it.
//
// - SkinAnimState: upstream CharacterRenderer::playAnimation / update
//   semantics (variation 0, a loop wraps by subtraction, a finished one-shot
//   plays its resume animation - upstream oneShotReturnAnim - or Stand, Death
//   holds its last frame) plus the two things the original client does and upstream
//   does not (PSP deviations, as for the player): idle variations by M2
//   frequency/replay (CharacterModel::nextVariation) and the M2 blendTime
//   cross-fade into a new sequence (CharacterModel::startBlend).
// - skinPose(): the bone matrices of one instance for the GE (psp_skeleton.hpp
//   evaluatePose + geBoneMatrix), dequantisation folded in.
// - SkinDrawer: one GU_TRIANGLE_STRIP per sub-batch with u8 weights; a GE bone
//   slot is re-sent only when it holds another bone of the SAME instance
//   (begin() per instance: another unit's matrices differ).
// sceGuBoneMatrix copies the 12 floats into the display list, so the caller
// may overwrite its matrices right after a draw (unlike vertices).
#pragma once

#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_skeleton.hpp"

#include <psptypes.h>

#include <cstdint>

namespace wowee::rendering::psp {

struct SkinAnimState {
    int sequence = -1;
    uint32_t animId = 0;
    float timeMs = 0.0f;
    bool loop = true;
    uint32_t resumeAnim = 0;   // one-shot end: loop this (0 = Stand); upstream oneShotReturnAnim
    uint16_t repeatsLeft = 0;  // variations: repeats left of the current one
    uint32_t rng = 0x2545F491u;
    // Cross-fade from the sequence that was playing (M2 blendTime).
    int blendFromSeq = -1;
    float blendFromTimeMs = 0.0f;
    float blendElapsedMs = 0.0f;
    float blendMs = 0.0f;
};

// Upstream CharacterRenderer::playAnimation: variation 0 of the id, else its
// first sequence, else the model's first sequence (animId then reads as that
// sequence's id, as upstream). Restarts at time 0 and cross-fades.
void skinPlay(const SkinView& v, SkinAnimState& s, uint32_t animId, bool loop, uint32_t resumeAnim = 0);
// Advances time and the fade; wraps loops (next idle variation), ends
// one-shots (resume / Stand; Death holds). Returns true when the pose changed.
bool skinUpdate(const SkinView& v, SkinAnimState& s, float dtSec);
// Jumps to the last frame of the current sequence, no fade (a corpse seen for
// the first time lies dead instead of falling again).
void skinHoldEnd(const SkinView& v, SkinAnimState& s);
inline bool skinHas(const SkinView& v, uint32_t animId) { return v.h && v.findSequence(animId) >= 0; }

// Bone matrices for the GE: pose (scratch: boneCount BoneMatrix) -> ge[boneCount].
void skinPose(const SkinView& v, const SkinAnimState& s, BoneMatrix* scratch, ScePspFMatrix4* ge);

class SkinDrawer {
public:
    // Per instance, before its first batch (its matrices are new to the GE).
    void begin() {
        for (int& b : slotBone_) b = -1;
    }
    // Every sub-batch of `b`; GE state (texture, blend, model matrix, UV
    // scale/offset) is the caller's. Returns the triangles drawn.
    uint32_t drawBatch(const SkinView& v, const ModelBatch& b, const ScePspFMatrix4* ge);
    uint32_t boneUploads = 0;  // reset by the caller (per frame)
    uint32_t draws = 0;

private:
    int slotBone_[kSkinMaxPalette] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

} // namespace wowee::rendering::psp
