// Small asset-independent spell visuals. No game state or damage is simulated.
#pragma once
#include <glm/glm.hpp>
#include "rendering/psp/psp_spell_families.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace wowee::rendering::psp {
class SimpleSpellFx {
public:
    enum Phase : uint8_t { Empty, Missile, Impact, Aura, Cast, Beam, Area };
    struct Effect {
        Phase phase = Empty;
        SimpleSpell spell = SimpleSpell::None;
        bool heal = false, channeled = false;
        uint32_t id = 0;
        uint64_t target = 0, caster = 0;
        glm::vec3 pos{0}, end{0}, direction{1,0,0};
        float elapsed = 0, duration = 0;
    };
    static constexpr int capacity = 12;
    std::array<Effect, capacity> effects{};
    int limit = 4;
    bool launch(uint32_t id, const glm::vec3& from, const glm::vec3& to, uint64_t target, SimpleSpell tint = SimpleSpell::None) {
        const auto spell = tint == SimpleSpell::None ? simpleSpell(id) : tint;
        if (spell == SimpleSpell::None || (tint == SimpleSpell::None && spell == SimpleSpell::Corruption) || !target) return false;
        auto& e = slot();
        e = Effect{}; e.phase = Missile; e.spell = spell; e.id = id; e.target = target; e.pos = from; e.end = to;
        e.duration = 8.0f; // expiry if the target moves away or cannot be reached
        const auto delta = to - from;
        if (glm::dot(delta, delta) > 1e-6f) e.direction = glm::normalize(delta);
        return true;
    }
    void aura(uint32_t id, uint64_t caster, uint64_t target, int32_t durationMs, const glm::vec3& at,
              SimpleSpell tint = SimpleSpell::None, bool persistent = true) {
        if (!target) return;
        Effect* use = nullptr;
        for (auto& e : effects) if (e.phase == Aura && e.id == id && e.target == target && e.caster == caster) { use = &e; break; }
        if (!durationMs) { if (use) *use = Effect{}; return; }
        if (!use) use = &slot();
        *use = Effect{}; use->phase = Aura; use->spell = tint == SimpleSpell::None ? (simpleSpell(id)==SimpleSpell::None ? SimpleSpell::Arcane : simpleSpell(id)) : tint;
        use->id = id; use->caster = caster; use->target = target; use->pos = at;
        use->duration = durationMs < 0 ? 3600.0f : static_cast<float>(durationMs) / 1000.0f;
        if (!persistent) use->duration = std::min(use->duration, 6.0f);
    }
    void burst(uint32_t id, SimpleSpell tint, const glm::vec3& at, bool heal=false) {
        auto& e=slot(); e=Effect{}; e.phase=Impact; e.id=id; e.spell=tint; e.pos=at; e.duration=.65f; e.heal=heal;
    }
    void cancelCast(uint64_t caster) {
        for (auto& e:effects) if ((e.phase==Cast || e.phase==Beam || (e.phase==Area && e.channeled)) && e.caster==caster) e=Effect{};
    }
    void startCast(uint32_t id, SimpleSpell tint, uint64_t caster, const glm::vec3& at, uint32_t ms) {
        cancelCast(caster); auto& e=slot(); e=Effect{}; e.phase=Cast; e.id=id; e.spell=tint;
        e.caster=caster; e.pos=at; e.duration=std::clamp(ms/1000.0f,.2f,30.0f);
    }
    void channel(uint32_t id, SimpleSpell tint, uint64_t caster, uint64_t target,
                 const glm::vec3& from, const glm::vec3& to, float seconds) {
        cancelCast(caster); auto& e=slot(); e=Effect{}; e.phase=Beam; e.id=id; e.spell=tint;
        e.caster=caster; e.target=target; e.pos=from; e.end=to; e.duration=std::clamp(seconds,.1f,60.0f);
    }
    void area(uint32_t id, SimpleSpell tint, uint64_t caster, const glm::vec3& at, float seconds, bool channel=false) {
        auto& e=slot(); e=Effect{}; e.phase=Area; e.id=id; e.spell=tint;
        e.caster=caster; e.pos=at; e.duration=std::clamp(seconds,.5f,30.0f); e.channeled=channel;
    }
    void channelTime(uint64_t caster, uint32_t ms) {
        for (auto& e:effects) if ((e.phase==Beam || (e.phase==Area && e.channeled)) && e.caster==caster) {
            if (!ms) e=Effect{}; else e.duration=e.elapsed+ms/1000.0f;
        }
    }
    template<class Position> void advance(float dt, Position&& position) {
        dt = std::clamp(dt, 0.0f, 0.25f);
        for (auto& e : effects) {
            if (e.phase == Empty) continue;
            e.elapsed += dt;
            if (e.elapsed >= e.duration) { e = Effect{}; continue; }
            if ((e.phase==Cast || e.phase==Beam) && e.caster && !position(e.caster,e.pos)) { e=Effect{}; continue; }
            if (e.target && !position(e.target, e.end)) { e = Effect{}; continue; }
            if (e.phase == Aura) { e.pos = e.end; continue; }
            if (e.phase != Missile) continue;
            const glm::vec3 delta = e.end - e.pos;
            const float d = glm::length(delta), step = 24.0f * dt;
            if (d <= std::max(step, 0.05f)) {
                e.pos = e.end; e.phase = Impact; e.elapsed = 0; e.duration = 0.45f; e.target = 0;
            } else {
                e.direction = delta / d; e.pos += e.direction * step;
            }
        }
    }
    int active() const { return static_cast<int>(std::count_if(effects.begin(), effects.end(), [](const auto& e) { return e.phase != Empty; })); }
private:
    Effect& slot() {
        const int n = std::clamp(limit, 1, capacity);
        for (int i = 0; i < n; ++i) if (effects[i].phase == Empty) return effects[i];
        // Preserve long target auras where possible; evict an impact/old missile first.
        int oldest = 0; float score = -1;
        for (int i = 0; i < n; ++i) {
            const auto& e = effects[i];
            const float value = ((e.phase == Aura || e.phase == Area) ? 0.0f : 2.0f) + e.elapsed / std::max(e.duration, .01f);
            if (value > score) { score = value; oldest = i; }
        }
        return effects[oldest];
    }
};
struct TerrainCamera;
void drawSimpleSpellFx(const SimpleSpellFx& fx, const TerrainCamera& cam, int spriteBudget);
} // namespace wowee::rendering::psp
