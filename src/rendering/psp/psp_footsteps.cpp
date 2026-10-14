// Sounds - footfall ($FSD) accessors of the skinned models, for
// src/ui/psp/ui_world_sounds.cpp (upstream FootstepDriver reads
// CharacterRenderer::getAnimationState + getFootstepEventTimes the same way).
// Read-only: nothing here changes an animation.
#include "rendering/psp/psp_character.hpp"
#include "rendering/psp/psp_creatures.hpp"

#include <cmath>

namespace wowee::rendering::psp {

bool CharacterModel::footstepClock(uint32_t& animId, float& timeMs, float& durationMs, const uint32_t*& events,
                                   uint32_t& count) const {
    events = nullptr;
    count = 0;
    if (!getAnimationState(animId, timeMs, durationMs)) return false;
    // The blob size is not kept here: the table is checked against itself
    // (psp_model_format.hpp skinFootsteps).
    events = skinFootsteps(animView().h, 0xFFFFFFFFu, static_cast<uint32_t>(sequence_), count);
    return true;
}

uint32_t CreatureRenderer::footstepProbes(float x, float y, float z, float maxDist, FootstepProbe* out,
                                          uint32_t max) const {
    uint32_t n = 0;
    const float max2 = maxDist * maxDist;
    for (const Unit& un : units_) {
        if (n >= max) break;
        const CreatureUnit& u = un.u;
        const float dx = u.x - x, dy = u.y - y, dz = u.z - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (!(d2 <= max2)) continue;
        auto it = anims_.find(u.guid);
        if (it == anims_.end()) continue;
        const AnimUnit& a = it->second;
        const Skin* skp = skinOf(a.skin);  // skins resident on demand (a map now)
        if (!skp) continue;
        const Skin& sk = *skp;
        if (!sk.blob || !sk.view.h || a.s.sequence < 0 || a.s.sequence >= static_cast<int>(sk.view.h->sequenceCount))
            continue;
        FootstepProbe& p = out[n++];
        p.guid = u.guid;
        p.displayId = u.displayId;
        p.x = u.x;
        p.y = u.y;
        p.z = u.z;
        p.dist = std::sqrt(d2);
        p.animId = a.s.animId;
        p.timeMs = a.s.timeMs;
        p.durationMs = static_cast<float>(sk.view.sequences[a.s.sequence].durationMs);
        p.events = skinFootsteps(sk.view.h, sk.bytes, static_cast<uint32_t>(a.s.sequence), p.eventCount);
        p.moving = u.moving && !u.swimming && !u.flying && !u.dead && !a.dead;
    }
    return n;
}

} // namespace wowee::rendering::psp
