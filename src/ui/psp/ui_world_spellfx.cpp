// The world screen half of the spell effects
// (the effects themselves: src/rendering/psp/psp_spellfx.cpp).
//
// Upstream SpellHandler's visual triggers, over the session's SpellFxEvent queue
// (include/platform/psp/psp_spellfx_events.hpp), with client-authored effects only:
//   - SMSG_SPELL_START -> triggerCastVisual: the precast kit at the caster for the cast time
//     (profession spells - SkillLine category 11 / 9, isProfessionSpell - and the ranged auto
//     attack, isRangedWeaponAutoAttack, skipped);
//   - SMSG_SPELL_GO -> the cast kit at the caster, triggerImpactVisual at every hit target (same skips);
//   - cast cancelled / failed -> cancel only that caster's precast/channel visual;
//   - SMSG_PLAY_SPELL_VISUAL -> the cast kit at the caster; SMSG_PLAY_SPELL_IMPACT -> the impact kit.
// resolveUnitPosition = WorldScreen::unitPosition (the player's drawn feet, a remote player's drawn
// position, else the entity); resolveUnitRenderInstance = the player's or a remote player's
// CharacterModel (their "p_" attachment points, CharacterModel::attachmentPosition).
// PSP: creatures.pak bakes no attachment points except mount seats, so a creature caster is
// untracked - its effect stays at its position + upstream's height offset (what upstream does when
// getAttachmentTransform fails). Spell ids -> SpellVisualID through spells.pak (SpellRec::visual).
// wowee.cfg, read at the first world update: spellfx = 0 (visuals off; sounds/poses remain) | 1 own spells (default) (their impacts on the targets, server visuals on / by the player)
// | 2 every caster (upstream); spellfxkb (384), spellfxmax (4), spellfxsprites (96), spellfxpx (64).
#include "ui/psp/ui_world_screen.hpp"

#include "game/spell_classification.hpp"
#include "game/update_field_table.hpp"
#include "ui/psp/ui_world_sounds.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_spellfx_events.hpp"
#include "rendering/psp/psp_spellfx.hpp"
#include "rendering/psp/psp_simple_spellfx.hpp"
#include "rendering/psp/psp_world_effect_policy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

struct WorldScreen::SpellFxUi final : pp::SpellFxSink, rendering::psp::SpellFxHost {
    explicit SpellFxUi(WorldScreen& screen) : w(screen) {}
    ~SpellFxUi() {
        for(const auto& sound:castSounds)if(sound.tag)w.sounds_.cancelSoundTag(sound.tag);
        if (pp::spellFxSink() == this) pp::spellFxSink() = nullptr;
    }
    SpellFxUi(const SpellFxUi&) = delete;
    SpellFxUi& operator=(const SpellFxUi&) = delete;

    // The session's events of this frame (a small ring: the oldest go when it overflows).
    void push(const pp::SpellFxEvent& e) override {
        // Visual ownership filters below must not discard nearby spell audio.
        // Aura refresh/removal replaces an older queued event for the same aura.
        if (e.kind == pp::SpellFxEvent::kAura) for (int i=0;i<count;++i) {
            auto& queued = queue[(head+i)%kQueue];
            if (queued.kind==e.kind && queued.guid==e.guid && queued.spellId==e.spellId && queued.targets[0]==e.targets[0]) {
                queued=e; return;
            }
        }
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
    void impactSound(uint32_t visual, const glm::vec3& at) override {
        const auto* meta=fx.kit(visual);
        if (meta && meta->impactSound) w.sounds_.playEntry(meta->impactSound,&at);
    }
    struct CastSound { uint64_t caster=0;uint32_t tag=0; };
    std::array<CastSound,32> castSounds{};
    void stopCastSound(uint64_t caster) {
        for(auto& sound:castSounds)if(sound.tag && sound.caster==caster) {
            w.sounds_.cancelSoundTag(sound.tag);sound={};
        }
    }
    void updateCastSounds() {
        for(auto& sound:castSounds)if(sound.tag && castRemaining(sound.caster)<0) {
            w.sounds_.cancelSoundTag(sound.tag);sound={};
        }
    }
    void kitSound(uint32_t visual, uint64_t caster, bool start, bool channel=false) {
        // GO ends only this caster's charge-up; release/impact clips are one-shots.
        if(!channel)stopCastSound(caster);
        const auto* meta=fx.kit(visual);
        if (!meta) return;
        const uint32_t sound=channel ? meta->channelSound : start ? meta->precastSound : meta->castSound;
        if(!sound || sound==0xFFFFFFFFu)return;
        uint32_t tag=0;
        if(start || channel) {
            auto* slot=&castSounds[0];
            for(auto& entry:castSounds)if(!entry.tag){slot=&entry;break;}
            if(slot->tag)w.sounds_.cancelSoundTag(slot->tag);
            static uint32_t serial=0;
            tag=0x53000000u|((++serial)&0x00FFFFFFu);
            *slot={caster,tag};
        }
        // Sustain short charge-up/channel clips at their original pitch. GO,
        // cancel and cast completion stop this caster's tagged voice.
        glm::vec3 at; float height=0;bool played=false;
        if(caster==w.session_.playerGuid())played=w.sounds_.playEntry(sound,nullptr,tag,start || channel);
        else if(w.unitPosition(caster,at,height))played=w.sounds_.playEntry(sound,&at,tag,start || channel);
        if(!played && soundLogs++<12)PSP_LOGI(pp::tag::Sys,"spell_sound_failed visual=%u entry=%u phase=%s own=%d",
            static_cast<unsigned>(visual),static_cast<unsigned>(sound),channel?"channel":start?"precast":"cast",caster==w.session_.playerGuid()?1:0);
    }
    float castRemaining(uint64_t caster) override {
        if (caster == w.session_.playerGuid()) return w.session_.isCasting() ? w.session_.castTimeRemaining() : -1.0f;
        const auto* cast = w.session_.unitCast(caster);
        return cast && cast->casting ? cast->timeRemaining : -1.0f;
    }
    int32_t auraRemaining(uint64_t target, uint32_t spell, uint64_t caster) override {
        const auto* auras = w.session_.unitAuras(target);
        if (auras) for (const auto& aura : *auras)
            if (aura.spellId == spell && (aura.casterGuid ? aura.casterGuid : target) == caster)
                return aura.getRemainingMs(pp::WorldSession::auraNowMs());
        return 0;
    }
    bool auraAnchor(uint64_t guid, uint8_t attach, glm::vec3& out, float& scale) override {
        float height = 0;
        if (!w.unitPosition(guid, out, height)) return false;
        // Use the drawn NPC origin and baked height when the creature pack provides them.
        glm::vec3 npcOrigin; float npcHeight = 0;
        if (w.creatures_.attachPoint(guid, -1, npcOrigin, &npcHeight)) {
            out = npcOrigin;
            if (std::isfinite(npcHeight) && npcHeight > .1f) height = npcHeight;
        }
        scale = std::clamp(height / 2.0f, .25f, 6.0f);
        if (attach && attachmentWorld(guid, attach, out)) return true;
        if (attach && w.creatures_.attachPoint(guid, attach, out)) return true;
        // NPC packs have no chest/head/hand attachments: use recipient proportions.
        if (attach == rendering::psp::kFxAttachHead) out.z += height * .85f;
        else if (attach == rendering::psp::kFxAttachChest) out.z += height * .5f;
        else if (attach == rendering::psp::kFxAttachHand || attach == rendering::psp::kFxAttachLeftHand) out.z += height * .55f;
        return true;
    }
    bool unitFeet(uint64_t guid, glm::vec3& out) override {
        float height = 0;
        return w.unitPosition(guid, out, height);
    }
    bool targetWorld(uint64_t guid, glm::vec3& out) override {
        float height = 0;
        if (!w.unitPosition(guid, out, height)) return false;
        out.z += std::clamp(height * .5f, .8f, 2.5f);
        return true;
    }
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
    std::unique_ptr<rendering::psp::SpellFxRenderer> worldFire;
    uint32_t nextFireSyncMs=0;
    void syncWorldFire() {
        if(!w.settings_.values().particles){worldFire.reset();return;}
        const uint32_t now=pp::logUptimeMs();if(now<nextFireSyncMs)return;nextFireSyncMs=now+500;
        struct Fire{uint64_t target,caster;uint32_t spell,visual;int32_t remaining;float distance;};
        std::array<Fire,8> nearest{};size_t count=0;
        for(const auto& [guid,entity]:w.session_.entities().getEntities()){
            if(entity->getType()!=game::ObjectType::UNIT || !rendering::psp::isHallowsFireHelper(entity->getField(game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY))))continue;
            glm::vec3 at;float height=0;if(!w.unitPosition(guid,at,height))continue;
            const glm::vec3 d=at-glm::vec3(w.playerX_,w.playerY_,w.shownZ_);const float distance=glm::dot(d,d);
            if(distance>80.f*80.f)continue;
            const auto* auras=w.session_.unitAuras(guid);if(!auras)continue;
            for(const auto& aura:*auras){
                if(!rendering::psp::isHallowsFireAura(aura.spellId))continue;
                const int32_t remaining=aura.getRemainingMs(pp::WorldSession::auraNowMs());if(!remaining)continue;
                const auto* info=w.spellDb_.get(aura.spellId);if(!info || !info->rec.visual)continue;
                const Fire fire{guid,aura.casterGuid?aura.casterGuid:guid,aura.spellId,info->rec.visual,remaining,distance};
                size_t slot=count;
                if(count==nearest.size()){
                    slot=0;for(size_t i=1;i<count;++i)if(nearest[i].distance>nearest[slot].distance)slot=i;
                    if(distance>=nearest[slot].distance)break;
                }else ++count;
                nearest[slot]=fire;break; // one actual flame state per helper
            }
        }
        if(!count){worldFire.reset();return;}
        if(!worldFire){
            worldFire.reset(new(std::nothrow) rendering::psp::SpellFxRenderer);if(!worldFire)return;
            rendering::psp::SpellFxConfig cfg;cfg.cacheBytes=192u*1024u;cfg.maxActive=8;cfg.maxSprites=64;cfg.maxPx=64;
            if(!worldFire->open(cfg)){worldFire.reset();nextFireSyncMs=now+2000;return;}
        }
        std::array<uint64_t,8> targets{};for(size_t i=0;i<count;++i)targets[i]=nearest[i].target;
        worldFire->keepAuraTargets(targets.data(),count);
        for(size_t i=0;i<count;++i){const auto& fire=nearest[i];
            worldFire->aura(fire.spell,fire.visual,fire.caster,fire.target,fire.remaining,false,*this);
        }
    }
    static constexpr int kQueue = 64;
    pp::SpellFxEvent queue[kQueue];
    int head = 0, count = 0;
    uint32_t dropped = 0;
    uint32_t soundLogs = 0;
    uint32_t openedMs = 0;
    uint32_t lastLevel = 0;
    uint32_t nextAuraSyncMs = 0;
    std::unordered_map<uint64_t, uint32_t> remoteLevels;
    bool prefetched = false;

    // Real baked kits that must play later (a shot arriving) or repeatedly (channels, lasting
    // ground areas). Fixed size, filled by the spell events, run from drawSpellFx.
    struct Pending {
        float wait = 0, interval = 0, left = 0;  // s: until the next play / repeat period / time still to run
        uint32_t visual = 0, spellId = 0;
        uint64_t caster = 0, target = 0;
        glm::vec3 at{0.0f};                      // used when fixed (a ground destination)
        bool used = false, fixed = false, own = false;
    };
    static constexpr int kPending = 16;
    Pending pending[kPending];
    bool schedule(Pending p) {
        for (auto& slot : pending) {
            if (!slot.used) { p.used = true; slot = p; return true; }
        }
        return false;
    }
    void dropCaster(uint64_t caster) {
        for (auto& p : pending) if (p.used && p.caster == caster) p = Pending{};
    }
    void channelTime(uint64_t caster, uint32_t ms) {
        for (auto& p : pending) {
            if (!p.used || p.caster != caster || p.interval <= 0) continue;
            if (!ms) p = Pending{}; else p.left = ms / 1000.0f;
        }
    }
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

uint32_t WorldScreen::spellKitAnimation(uint32_t spell, int phase) {
    const auto* info=spellDb_.get(spell);
    const auto* meta=spellFx_ && info ? spellFx_->fx.kit(info->rec.visual) : nullptr;
    if (!meta) return 0xFFFFFFFFu;
    if(phase==6)return 1; // metadata is present, including authored no-animation values
    if(phase>=3)return spellFx_->fx.kitStart(info->rec.visual,phase-3);
    return phase==0 ? meta->precastAnim : phase==1 ? meta->castAnim : meta->channelAnim;
}

void WorldScreen::configureSpellFx(int mode, bool fancy) {
    mode = std::clamp(mode, 0, 2);
    if (spellFxMode_ == mode && spellFxReal_ == static_cast<int>(fancy)) return;
    spellFx_.reset(); // unregister the old event sink before replacing the renderer
    spellFxMode_ = -1;
    spellFxReal_ = fancy ? 1 : 0;
}

void WorldScreen::spellFxEvents() {
    if (spellFxMode_ < 0) {
        spellFxMode_ = std::clamp(settings_.values().spellfx, 0, 2);
        rendering::psp::SpellFxConfig cfg;
        cfg.cacheBytes = static_cast<uint32_t>(std::clamp(pp::configGetInt("spellfxkb", 384), 64, 4096)) * 1024u;
        cfg.maxActive = pp::configGetInt("spellfxmax", 4);
        cfg.maxSprites = pp::configGetInt("spellfxsprites", 96);
        cfg.maxPx = pp::configGetInt("spellfxpx", 64);
        auto* ui = new (std::nothrow) SpellFxUi(*this);
        if (!ui) { spellFxMode_ = 0; return; }
        ui->fx.loadKits(); // sounds and authored poses remain independent of visual quality
        if(!settings_.values().spellfxreal)cfg.maxSprites=std::min(cfg.maxSprites,32);
        if(spellFxMode_ && !ui->fx.open(cfg))
            PSP_LOGW(pp::tag::Gfx,"spellfx: original visuals unavailable; rebuild spellfx.pak (no synthetic fallback)");
        spellFx_ = std::unique_ptr<SpellFxUi, void (*)(SpellFxUi*)>(ui, &WorldScreen::destroySpellFxUi);
        pp::spellFxSink() = ui;
        ui->openedMs = pp::logUptimeMs();
        PSP_LOGI(pp::tag::Gfx, "spellfx mode=%d (%s)", spellFxMode_, spellFxMode_ == 1 ? "own spells" : "every caster");
    }
    if (!spellFx_) return;
    // A descriptor/heap shortage on entry must not silence spells for the
    // entire login. Loaded metadata is cached; transient failures back off.
    spellFx_->fx.loadKits();
    spellFx_->syncWorldFire(); // environment effects are independent of own-spells/basic/off filters
    spellFx_->updateCastSounds();
    auto& fx = spellFx_->fx;
    const uint64_t me = session_.playerGuid();
    const bool everyone = spellFxMode_ == 2;
    const bool visualsEnabled=spellFxMode_!=0;
    const bool fancy=settings_.values().spellfxreal!=0;
    const uint64_t pet = session_.petGuid();
    const uint32_t level = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    if (spellFxMode_ && level && spellFx_->lastLevel && level > spellFx_->lastLevel && settings_.values().levelupfx) {
        playUiSound(UiSound::LevelUp);
        fx.auraImpact(rendering::psp::kFxLevelUpVisual, me, true, *spellFx_);
    }
    if (level) spellFx_->lastLevel = level;
    if (visualsEnabled && everyone && settings_.values().levelupfx) {
        for (const auto& remote : remotes_) {
            const auto entity = session_.entities().getEntity(remote.first);
            if (!entity) continue;
            const uint32_t now = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
            auto& previous = spellFx_->remoteLevels[remote.first];
            if (now && previous && now > previous)
                fx.auraImpact(rendering::psp::kFxLevelUpVisual, remote.first, false, *spellFx_);
            if (now) previous = now;
        }
        for (auto it = spellFx_->remoteLevels.begin(); it != spellFx_->remoteLevels.end();)
            if (remotes_.find(it->first) == remotes_.end()) it = spellFx_->remoteLevels.erase(it); else ++it;
    }
    auto ours = [&](uint64_t guid) { return guid == me || (pet && guid == pet); };
    // The spell's visual, or 0 when upstream shows none (profession, ranged auto attack, no visual).
    auto visualOf = [&](uint32_t spellId) -> uint32_t {
        if (game::spellclass::isRangedWeaponAutoAttack(spellId)) return 0;
        const pp::SpellInfo* s = spellDb_.get(spellId);
        if (!s || professionSpell(spellDb_, s)) return 0;
        return s->rec.visual;
    };
    // Every spell uses its baked metadata; named ranks are only old-data fallbacks.
    auto styleOf = [&](uint32_t id) {
        const auto* info = spellDb_.get(id);
        const uint8_t effects[3] = {}; const uint16_t auras[3] = {};
        return info ? rendering::psp::simpleSpellStyle(id, info->rec.schoolMask, info->rec.castMs,
                          info->rec.attributesEx, info->rec.effect, info->rec.aura)
                    : rendering::psp::simpleSpellStyle(id, 0, 0, 0, effects, auras);
    };
    auto weaponStrike = [&](uint32_t id) {
        const auto* info = spellDb_.get(id);
        if (!info || info->rec.schoolMask != 1) return false;
        for (uint16_t aura : info->rec.aura) if (aura) return false;
        for (uint8_t effect : info->rec.effect)
            if (effect == 17 || effect == 31 || effect == 58 || effect == 121) return true;
        return false;
    };
    auto auraOnly = [&](uint32_t id) {
        const auto* info = spellDb_.get(id);
        if (!info) return false;
        bool any = false;
        for (uint8_t effect : info->rec.effect) {
            if (!effect) continue;
            // WotLK APPLY_AURA and persistent area-aura variants.
            if (effect != 6 && effect != 27 && effect != 35 && effect != 65 && effect != 119 && effect != 128 && effect != 129 && effect != 143) return false;
            any = true;
        }
        return any;
    };
    // Recover state kits for auras received before the sink was registered, or while full.
    // Application flashes are event-only; synchronizing must never replay them.
    if (visualsEnabled && pp::logUptimeMs() >= spellFx_->nextAuraSyncMs) {
        spellFx_->nextAuraSyncMs = pp::logUptimeMs() + 500;
        auto sync = [&](uint64_t target) {
            if (!fx.hasRoom()) return;
            const auto* list = session_.unitAuras(target);
            if (!list) return;
            glm::vec3 at; float height = 0;
            if (!unitPosition(target, at, height) || glm::length(at - glm::vec3(playerX_,playerY_,shownZ_)) > 120.0f) return;
            for (const auto& aura : *list) {
                if (!fx.hasRoom()) break;
                const uint64_t caster = aura.casterGuid ? aura.casterGuid : target;
                if (!aura.spellId || (!everyone && !ours(caster))) continue;
                const int32_t left = aura.getRemainingMs(pp::WorldSession::auraNowMs());
                const auto* info = left ? spellDb_.get(aura.spellId) : nullptr;
                if (info && !weaponStrike(aura.spellId)) fx.aura(aura.spellId, info->rec.visual, caster, target, left, ours(caster), *spellFx_);
            }
        };
        sync(me);
        for (const auto& entry : session_.entities().getEntities()) {
            if (!fx.hasRoom()) break;
            if (entry.first != me) sync(entry.first);
        }
    }
    // PSP: read the main bar's effects ahead once, a few seconds into the world (one per frame).
    if (visualsEnabled && !spellFx_->prefetched && pp::logUptimeMs() - spellFx_->openedMs > 3000u) {
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
    while(spellFx_->pop(e)) {
        const auto* info=spellDb_.get(e.spellId);
        const uint32_t visual=e.kind==pp::SpellFxEvent::kPlayVisual || e.kind==pp::SpellFxEvent::kPlayImpact
            ? e.visualId : info ? info->rec.visual : e.visualId;
        // Audio lifecycle is independent of ownership filters and Basic/Fancy/Off.
        if(e.kind==pp::SpellFxEvent::kStart)spellFx_->kitSound(visual,e.guid,true);
        if(e.kind==pp::SpellFxEvent::kGo) {
            spellFx_->kitSound(visual,e.guid,false);
            if(info && styleOf(e.spellId).channel)spellFx_->kitSound(visual,e.guid,false,true);
        }
        if(e.kind==pp::SpellFxEvent::kCancel || (e.kind==pp::SpellFxEvent::kChannel && !e.castMs))spellFx_->stopCastSound(e.guid);
        if(e.kind==pp::SpellFxEvent::kPlayVisual)spellFx_->kitSound(visual,e.guid,false);
        if(e.kind==pp::SpellFxEvent::kChannel)spellFx_->channelTime(e.guid,e.castMs);
        if(e.kind==pp::SpellFxEvent::kCancel) {fx.cancelPrecast(e.guid);spellFx_->dropCaster(e.guid);}
        if(e.kind==pp::SpellFxEvent::kGo) {fx.cancelPrecast(e.guid);spellFx_->dropCaster(e.guid);}
        const bool draw=visualsEnabled && (everyone || ours(e.guid));
        glm::vec3 pos;float height=0;
        switch(e.kind) {
            case pp::SpellFxEvent::kStart:
                if(draw && fancy && !professionSpell(spellDb_,info) && unitPosition(e.guid,pos,height))
                    fx.playPrecast(visual,pos,e.castMs,e.guid,ours(e.guid),*spellFx_);
                break;
            case pp::SpellFxEvent::kGo: {
                const auto style=styleOf(e.spellId);
                const bool mine=ours(e.guid);
                glm::vec3 origin;float casterHeight=0;
                const bool haveCaster=unitPosition(e.guid,origin,casterHeight);
                const bool missile=fx.hasMissile(visual);
                const float duration=info && info->rec.durationMs>0 ? info->rec.durationMs/1000.f : 0.f;
                if(draw && fancy && haveCaster && !professionSpell(spellDb_,info))fx.play(visual,origin,false,e.guid,mine,*spellFx_);
                if(e.hasDestination) {
                    const glm::vec3 dest(e.destination[0],e.destination[1],e.destination[2]);
                    spellFx_->impactSound(visual,dest);
                    if(draw)fx.play(visual,dest,true,0,mine,*spellFx_);
                }
                for(int i=0;i<e.targetCount;++i) {
                    if(!unitPosition(e.targets[i],pos,height))continue;
                    if(!missile)spellFx_->impactSound(visual,pos);
                    if(!draw || professionSpell(spellDb_,info))continue;
                    if(missile && haveCaster) {
                        glm::vec3 hand=origin+glm::vec3(0,0,casterHeight*.55f);
                        spellFx_->attachmentWorld(e.guid,rendering::psp::kFxAttachHand,hand);
                        fx.launch(visual,hand,pos+glm::vec3(0,0,height*.5f),e.targets[i],mine);
                    } else if(!auraOnly(e.spellId) || style.channel) {
                        fx.play(visual,pos,true,0,mine,*spellFx_);
                        if(style.channel && duration>1.5f) {
                            SpellFxUi::Pending p;p.wait=p.interval=1.5f;p.left=duration-1.5f;
                            p.visual=visual;p.spellId=e.spellId;p.caster=e.guid;p.target=e.targets[i];p.own=mine;
                            spellFx_->schedule(p);
                        }
                    }
                }
                break;
            }
            case pp::SpellFxEvent::kAura:
                if(draw && e.targetCount && !weaponStrike(e.spellId)) {
                    const bool state=fx.aura(e.spellId,visual,e.guid,e.targets[0],e.auraMs,ours(e.guid),*spellFx_);
                    if(!e.auraMs) {if(state)fx.auraImpact(visual,e.targets[0],ours(e.guid),*spellFx_,2);}
                    else if(auraOnly(e.spellId))fx.auraImpact(visual,e.targets[0],ours(e.guid),*spellFx_);
                }
                break;
            case pp::SpellFxEvent::kPlayVisual:
                if(draw && unitPosition(e.guid,pos,height))fx.play(visual,pos,false,e.guid,ours(e.guid),*spellFx_);
                break;
            case pp::SpellFxEvent::kPlayImpact:
                if(unitPosition(e.guid,pos,height)) {
                    spellFx_->impactSound(visual,pos);
                    if(draw)fx.play(visual,pos,true,0,ours(e.guid),*spellFx_);
                }
                break;
            default:break;
        }
    }
}

void WorldScreen::drawSpellFx(const rendering::psp::TerrainCamera& cam) {
    if (!spellFx_) return;
    if(spellFx_->worldFire)spellFx_->worldFire->frame(cam,lastDt_,*spellFx_);
    if(spellFxMode_ == 0)return;
    // Real kits that were waiting for a shot to land or for the next channel / ground-area pulse.
    {
        const float dt = std::clamp(lastDt_, 0.0f, 0.25f);
        for (auto& p : spellFx_->pending) {
            if (!p.used) continue;
            p.wait -= dt;
            if (p.wait > 0.0f) continue;
            glm::vec3 at = p.at;
            float height = 0.0f;
            const bool ok = p.fixed || unitPosition(p.target, at, height);
            if(ok)spellFx_->fx.play(p.visual,at,true,0,p.own,*spellFx_);
            if (ok && p.interval > 0.0f && (p.left -= p.interval) > 0.0f) p.wait = p.interval;
            else p = SpellFxUi::Pending{};
        }
    }
    spellFx_->fx.performanceScale=performanceSpells_;
    spellFx_->fx.frame(cam, lastDt_, *spellFx_);
}

} // namespace wowee::ui::psp
