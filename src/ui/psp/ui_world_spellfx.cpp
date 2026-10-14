// The world screen half of the spell effects
// (the effects themselves: src/rendering/psp/psp_spellfx.cpp).
//
// Upstream SpellHandler's visual triggers, over the session's SpellFxEvent queue
// (include/platform/psp/psp_spellfx_events.hpp), logic unchanged:
//   - SMSG_SPELL_START -> triggerCastVisual: the precast kit at the caster for the cast time
//     (profession spells - SkillLine category 11 / 9, isProfessionSpell - and the ranged auto
//     attack, isRangedWeaponAutoAttack, skipped);
//   - SMSG_SPELL_GO -> the cast kit at the caster, triggerImpactVisual at every hit target (same skips);
//   - the own cast cancelled / failed (CAST_FAILED, SPELL_FAILURE) -> cancelAllPrecastVisuals;
//   - SMSG_PLAY_SPELL_VISUAL -> the cast kit at the caster; SMSG_PLAY_SPELL_IMPACT -> the impact kit.
// resolveUnitPosition = WorldScreen::unitPosition (the player's drawn feet, a remote player's drawn
// position, else the entity); resolveUnitRenderInstance = the player's or a remote player's
// CharacterModel (their "p_" attachment points, CharacterModel::attachmentPosition).
// PSP: creatures.pak bakes no attachment points except mount seats, so a creature caster is
// untracked - its effect stays at its position + upstream's height offset (what upstream does when
// getAttachmentTransform fails). Spell ids -> SpellVisualID through spells.pak (SpellRec::visual).
// wowee.cfg, read at the first world update: spellfx = 0 (default: off, nothing allocated, no
// events queued) | 1 own spells (their impacts on the targets, server visuals on / by the player)
// | 2 every caster (upstream); spellfxkb (384), spellfxmax (4), spellfxsprites (96), spellfxpx (64).
#include "ui/psp/ui_world_screen.hpp"

#include "game/spell_classification.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_spellfx_events.hpp"
#include "rendering/psp/psp_spellfx.hpp"

#include <algorithm>
#include <cmath>
#include <new>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

struct WorldScreen::SpellFxUi final : pp::SpellFxSink, rendering::psp::SpellFxHost {
    explicit SpellFxUi(WorldScreen& screen) : w(screen) {}
    ~SpellFxUi() {
        if (pp::spellFxSink() == this) pp::spellFxSink() = nullptr;
    }
    SpellFxUi(const SpellFxUi&) = delete;
    SpellFxUi& operator=(const SpellFxUi&) = delete;

    // The session's events of this frame (a small ring: the oldest go when it overflows).
    void push(const pp::SpellFxEvent& e) override {
        if (count == kQueue) {
            head = (head + 1) % kQueue;
            --count;
            if (++dropped <= 5) PSP_LOGW(pp::tag::Gfx, "spellfx_queue_full dropped=%u", static_cast<unsigned>(dropped));
        }
        queue[(head + count) % kQueue] = e;
        ++count;
    }
    bool pop(pp::SpellFxEvent& e) {
        if (!count) return false;
        e = queue[head];
        head = (head + 1) % kQueue;
        --count;
        return true;
    }

    // upstream CharacterRenderer::getAttachmentTransform for the caster's instance: the point on this
    // frame's pose, turned by the facing the model is drawn with, at the drawn position.
    bool attachmentWorld(uint64_t guid, uint8_t id, glm::vec3& out) override {
        const rendering::psp::CharacterModel* model = nullptr;
        glm::vec3 at(0.0f);
        float yaw = 0.0f;
        if (guid == w.session_.playerGuid()) {
            if (!w.player_.loaded()) return false;
            model = &w.player_;
            at = (w.mountActive_ && w.mountSeatFound_) ? w.mountSeatSmoothed_ : glm::vec3(w.playerX_, w.playerY_, w.shownZ_);
            yaw = w.facing_;
        } else {
            auto r = w.remotes_.find(guid);
            if (r == w.remotes_.end() || !r->second.model || !r->second.hasCache) return false;
            model = r->second.model.get();
            at = r->second.mountReady ? r->second.pos + glm::vec3(0.0f, 0.0f, r->second.riderHeight) : r->second.pos;
            yaw = r->second.yaw;
        }
        float p[3];
        if (!model->attachmentPosition(id, p)) return false;
        const float c = std::cos(yaw), s = std::sin(yaw);
        out = at + glm::vec3(c * p[0] - s * p[1], s * p[0] + c * p[1], p[2]);
        return true;
    }

    WorldScreen& w;
    rendering::psp::SpellFxRenderer fx;
    static constexpr int kQueue = 16;
    pp::SpellFxEvent queue[kQueue];
    int head = 0, count = 0;
    uint32_t dropped = 0;
    uint32_t openedMs = 0;
    bool prefetched = false;
};

void WorldScreen::destroySpellFxUi(SpellFxUi* p) { delete p; }

namespace {

// upstream GameHandler::isProfessionSpell: the spell's skill line is a profession (11) or a
// secondary skill (9). An older spells.pak without "skills": never.
bool professionSpell(pp::SpellDb& db, const pp::SpellInfo* s) {
    if (!s || !s->rec.skillLine) return false;
    const auto* line = db.skillLine(s->rec.skillLine);
    return line && (line->category == 11 || line->category == 9);
}

}  // namespace

void WorldScreen::spellFxEvents() {
    if (spellFxMode_ < 0) {
        spellFxMode_ = std::clamp(pp::configGetInt("spellfx", 0), 0, 2);
        if (spellFxMode_ == 0) {
            PSP_LOGI(pp::tag::Gfx, "spellfx off (wowee.cfg spellfx=0)");
            return;
        }
        rendering::psp::SpellFxConfig cfg;
        cfg.cacheBytes = static_cast<uint32_t>(std::clamp(pp::configGetInt("spellfxkb", 384), 64, 4096)) * 1024u;
        cfg.maxActive = pp::configGetInt("spellfxmax", 4);
        cfg.maxSprites = pp::configGetInt("spellfxsprites", 96);
        cfg.maxPx = pp::configGetInt("spellfxpx", 64);
        auto* ui = new (std::nothrow) SpellFxUi(*this);
        if (!ui || !ui->fx.open(cfg)) {
            delete ui;
            spellFxMode_ = 0;
            return;
        }
        spellFx_ = std::unique_ptr<SpellFxUi, void (*)(SpellFxUi*)>(ui, &WorldScreen::destroySpellFxUi);
        pp::spellFxSink() = ui;
        ui->openedMs = pp::logUptimeMs();
        PSP_LOGI(pp::tag::Gfx, "spellfx mode=%d (%s)", spellFxMode_, spellFxMode_ == 1 ? "own spells" : "every caster");
    }
    if (!spellFx_) return;
    auto& fx = spellFx_->fx;
    const uint64_t me = session_.playerGuid();
    const bool everyone = spellFxMode_ == 2;
    // resolveUnitRenderInstance: casters whose bones can be followed.
    auto tracked = [&](uint64_t guid) -> uint64_t {
        if (guid == me) return player_.loaded() ? guid : 0;
        auto r = remotes_.find(guid);
        return (r != remotes_.end() && r->second.model) ? guid : 0;
    };
    // The spell's visual, or 0 when upstream shows none (profession, ranged auto attack, no visual).
    auto visualOf = [&](uint32_t spellId) -> uint32_t {
        if (game::spellclass::isRangedWeaponAutoAttack(spellId)) return 0;
        const pp::SpellInfo* s = spellDb_.get(spellId);
        if (!s || professionSpell(spellDb_, s)) return 0;
        return s->rec.visual;
    };
    // PSP: read the main bar's effects ahead once, a few seconds into the world (one per frame).
    if (!spellFx_->prefetched && pp::logUptimeMs() - spellFx_->openedMs > 3000u) {
        const auto& bar = session_.actionBar();
        const int base = mainBarBase();
        uint32_t n = 0;
        bool any = false;
        for (int i = 0; i < 12 && base + i < pp::WorldSession::kActionBarSlots; ++i) {
            const auto& slot = bar[static_cast<size_t>(base + i)];
            if (slot.type != game::ActionBarSlot::SPELL || !slot.id) continue;
            any = true;
            if (const uint32_t v = visualOf(slot.id)) {
                fx.prefetch(v);
                ++n;
            }
        }
        if (any) {
            spellFx_->prefetched = true;
            PSP_LOGI(pp::tag::Gfx, "spellfx_prefetch visuals=%u (main bar from slot %d)", static_cast<unsigned>(n), base);
        }
    }
    pp::SpellFxEvent e;
    while (spellFx_->pop(e)) {
        glm::vec3 pos;
        float height = 0.0f;
        switch (e.kind) {
            case pp::SpellFxEvent::kStart: {
                if (!everyone && e.guid != me) break;
                const uint32_t visual = visualOf(e.spellId);
                if (visual && unitPosition(e.guid, pos, height))
                    fx.playPrecast(visual, pos, e.castMs, tracked(e.guid), e.guid == me, *spellFx_);
                break;
            }
            case pp::SpellFxEvent::kGo: {
                if (!everyone && e.guid != me) break;
                const uint32_t visual = visualOf(e.spellId);
                if (!visual) break;
                if (unitPosition(e.guid, pos, height))
                    fx.play(visual, pos, false, tracked(e.guid), e.guid == me, *spellFx_);
                for (int i = 0; i < e.targetCount; ++i)
                    if (unitPosition(e.targets[i], pos, height)) fx.play(visual, pos, true, 0, e.guid == me, *spellFx_);
                break;
            }
            case pp::SpellFxEvent::kCancel:
                fx.cancelAllPrecast();
                break;
            case pp::SpellFxEvent::kPlayVisual:
                if (!everyone && e.guid != me) break;
                if (unitPosition(e.guid, pos, height))
                    fx.play(e.visualId, pos, false, tracked(e.guid), e.guid == me, *spellFx_);
                break;
            case pp::SpellFxEvent::kPlayImpact:
                if (!everyone && e.guid != me) break;
                if (unitPosition(e.guid, pos, height)) fx.play(e.visualId, pos, true, 0, e.guid == me, *spellFx_);
                break;
            default:
                break;
        }
    }
}

void WorldScreen::drawSpellFx(const rendering::psp::TerrainCamera& cam) {
    if (!spellFx_) return;
    spellFx_->fx.frame(cam, lastDt_, *spellFx_);
}

} // namespace wowee::ui::psp
