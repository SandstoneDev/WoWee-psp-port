// Spell effects: spell visual events from WorldSession to the spell effects
// (WorldScreen::spellFxEvents, src/ui/psp/ui_world_spellfx.cpp -> src/rendering/psp/psp_spellfx.cpp).
// What upstream's SpellHandler hands SpellVisualSystem: SMSG_SPELL_START (triggerCastVisual),
// SMSG_SPELL_GO (the cast kit at the caster, triggerImpactVisual per hit target), the own cast
// cancelled / failed (cancelAllPrecastVisuals), SMSG_PLAY_SPELL_VISUAL (handlePlaySpellVisual) and
// SMSG_PLAY_SPELL_IMPACT. Nothing is queued - and nothing allocated - while no sink is registered:
// wowee.cfg spellfx=0 (the default) never registers one.
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

struct SpellFxEvent {
    enum Kind : uint8_t { kStart, kGo, kCancel, kPlayVisual, kPlayImpact };
    static constexpr int kMaxTargets = 4;  // PSP: SPELL_GO hit targets kept (the effect cap is 4 anyway)
    uint8_t kind = kStart;
    uint8_t targetCount = 0;
    uint32_t spellId = 0;   // kStart / kGo
    uint32_t visualId = 0;  // kPlayVisual / kPlayImpact
    uint32_t castMs = 0;    // kStart: SPELL_START cast time
    uint64_t guid = 0;      // the caster (kStart / kGo / kCancel / kPlayVisual), the target (kPlayImpact)
    uint64_t targets[kMaxTargets] = {};
};

class SpellFxSink {
public:
    virtual void push(const SpellFxEvent& e) = 0;

protected:
    ~SpellFxSink() = default;
};

inline SpellFxSink*& spellFxSink() {
    static SpellFxSink* sink = nullptr;
    return sink;
}
inline bool spellFxListening() { return spellFxSink() != nullptr; }
inline void spellFxPush(const SpellFxEvent& e) {
    if (SpellFxSink* s = spellFxSink()) s->push(e);
}

// One-line hooks for WorldSession (psp_world_session_spells.cpp); free when nobody listens.
inline void spellFxStart(uint64_t caster, uint32_t spellId, uint32_t castMs) {
    if (!spellFxListening()) return;
    SpellFxEvent e;
    e.kind = SpellFxEvent::kStart;
    e.guid = caster;
    e.spellId = spellId;
    e.castMs = castMs;
    spellFxPush(e);
}
template <typename Guids>
inline void spellFxGo(uint64_t caster, uint32_t spellId, const Guids& hitTargets) {
    if (!spellFxListening()) return;
    SpellFxEvent e;
    e.kind = SpellFxEvent::kGo;
    e.guid = caster;
    e.spellId = spellId;
    for (uint64_t t : hitTargets) {
        if (e.targetCount >= SpellFxEvent::kMaxTargets) break;
        if (t) e.targets[e.targetCount++] = t;
    }
    spellFxPush(e);
}
inline void spellFxCancel(uint64_t caster) {
    if (!spellFxListening()) return;
    SpellFxEvent e;
    e.kind = SpellFxEvent::kCancel;
    e.guid = caster;
    spellFxPush(e);
}
// SMSG_PLAY_SPELL_VISUAL (impact = false: guid is the caster) / SMSG_PLAY_SPELL_IMPACT (the target).
inline void spellFxVisual(bool impact, uint64_t guid, uint32_t visualId) {
    if (!spellFxListening() || visualId == 0) return;
    SpellFxEvent e;
    e.kind = impact ? SpellFxEvent::kPlayImpact : SpellFxEvent::kPlayVisual;
    e.guid = guid;
    e.visualId = visualId;
    spellFxPush(e);
}

} // namespace wowee::platform::psp
