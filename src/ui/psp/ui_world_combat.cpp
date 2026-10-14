// Melee combat, floating combat text, NPC names and the player's
// death on the world screen.
//
// Controls (free D-pad slots): L + D-pad right = auto-attack on/off
// (upstream CombatHandler::startAutoAttack / stopAutoAttack), D-pad right =
// next target and L + D-pad left = clear target (ui_world_target.cpp).
//
// Animation as upstream AnimationController + CharacterAnimator:
//   - setInCombat(isInCombat && alive) (GameScreen), setLowHealth(hp <= 20 %)
//     (playerHealthCallback);
//   - own swing (SMSG_ATTACKERSTATEUPDATE, attacker = us) = triggerMeleeSwing:
//     cancels an emote, 0.1 s cooldown, the swing timer = the attack
//     sequence's length clamped to 0.25..1 s (CombatFSM MELEE_SWING picks the
//     attack by the weapon loadout); PSP: the length is read from the model
//     the frame after the FSM started it (0.6 s until then) - CharacterModel
//     has no sequence-length query;
//   - hit reactions (hitReactionCallback): wound / crit / dodge / block
//     one-shots on the victim if the model has them (parry: none, upstream);
//   - other players' swings (npcSwingCallback): the first attack animation of
//     meleeAnimChain(Generic) the model has, once;
//   - deaths (npcDeathCallback / respawn): Death, held on its last frame;
//     RISE (or Stand) when alive again. NPC models are static on the PSP yet:
//     a dead NPC stays standing, its name grey.
// Floating combat text: upstream CombatUI::renderCombatText - world-anchored
// above the victim, rising 1.2 yd/s, fading over 2.5 s, same colours;
// PSP: the words of the running language (GlobalStrings COMBAT_TEXT_* - see below), crits in the title font.
// Death: Death held + grey screen; the death popups, the ghost's teleport,
// the corpse run and the resurrections are in ui_world_death.cpp (updateDeath).
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "game/inventory.hpp"
#include "game/spell_defines.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/melee_anim_chains.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace anim = wowee::rendering::anim;
using Life = pp::WorldSession::Life;
using CT = game::CombatTextEntry;

namespace {

constexpr size_t kMaxCombatText = 24;

uint32_t rgba(float r, float g, float b, float a) {
    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

const char* attackErrorText(pp::WorldSession::AttackError e) {
    using E = pp::WorldSession::AttackError;
    switch (e) {
        case E::TooFar:
        case E::NotInRange: return OWN_STR(ATTACK_TOO_FAR);
        case E::OutOfRange: return OWN_STR(ATTACK_STOPPED_FAR);
        case E::BadFacing: return OWN_STR(ATTACK_BAD_FACING);
        case E::NotStanding: return OWN_STR(ATTACK_NOT_STANDING);
        case E::CantAttack: return OWN_STR(ATTACK_CANT);
        default: return nullptr;
    }
}

}  // namespace

// Weapon loadout for CombatFSM (upstream GameScreen: setEquippedWeaponType
// from the main/off hand). PSP: from SMSG_CHAR_ENUM (no inventory yet), so
// the item subclass (polearm/staff/fist/dagger) is unknown - one-hand /
// two-hand / off-hand / shield only.
void WorldScreen::applyCombatLoadout() {
    rendering::WeaponLoadout lo;
    for (const auto& c : session_.characters()) {
        if (c.name != name_) continue;
        const uint8_t mh = c.equipment.size() > 15 && !c.equipment[15].isEmpty() ? c.equipment[15].inventoryType : 0;
        const uint8_t oh = c.equipment.size() > 16 && !c.equipment[16].isEmpty() ? c.equipment[16].inventoryType : 0;
        lo.inventoryType = mh;
        lo.hasOffHand = oh != 0 && game::isOffHandWeaponInventoryType(oh);
        lo.hasShield = oh == game::InvType::SHIELD;
    }
    animator_.setEquippedWeaponType(lo);
}

// Dead: no movement, no jump, no sitting (the camera still turns).
const pp::PadState& WorldScreen::combatPad(const pp::PadState& pad) {
    if (session_.life() != Life::Dead) return pad;
    constexpr uint32_t kBlocked = PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS | PSP_CTRL_SQUARE | PSP_CTRL_CIRCLE |
                                  PSP_CTRL_START | PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER;
    deadPad_ = pad;
    deadPad_.held &= ~kBlocked;
    deadPad_.pressed &= ~kBlocked;
    deadPad_.released &= ~kBlocked;
    return deadPad_;
}

// Upstream: the dead player's model plays Death once and holds its last
// frame; nothing else animates it until it is alive again.
bool WorldScreen::combatAnimOverride() {
    if (session_.life() != Life::Dead) return false;
    uint32_t id = 0;
    float t = 0.0f, dur = 0.0f;
    if (player_.hasAnimation(anim::DEATH) && (!player_.getAnimationState(id, t, dur) || id != anim::DEATH)) {
        player_.playAnimation(anim::DEATH, false);
        lastAnimRequest_ = anim::DEATH;
        lastAnimLoop_ = false;
    }
    return true;
}

// upstream AnimationController::updateMeleeTimers + the FrameInput fields
// it fills for CharacterAnimator.
void WorldScreen::combatFrameInput(rendering::CharacterAnimator::FrameInput& in, float dtSec) {
    if (meleeSwingCooldown_ > 0.0f) meleeSwingCooldown_ = std::max(0.0f, meleeSwingCooldown_ - dtSec);
    if (meleeSwingTimer_ > 0.0f) meleeSwingTimer_ = std::max(0.0f, meleeSwingTimer_ - dtSec);
    if (meleeDurPending_ && meleeSwingTimer_ > 0.0f && in.haveAnimState &&
        animator_.getCombat().getState() == rendering::CombatFSM::State::MELEE_SWING &&
        in.currentAnimId == lastAnimRequest_ && in.currentAnimDuration > 1.0f) {
        const float dur = std::clamp(in.currentAnimDuration / 1000.0f, 0.25f, 1.0f);
        meleeSwingTimer_ = std::max(0.0f, dur - in.currentAnimTime / 1000.0f);
        meleeDurPending_ = false;
    }
    in.meleeSwingTimer = meleeSwingTimer_;
}

// upstream AnimationController::triggerMeleeSwing.
void WorldScreen::triggerMeleeSwing() {
    if (!player_.animated() || meleeSwingCooldown_ > 0.0f) return;
    if (animator_.getActivity().isEmoteActive()) animator_.cancelEmote();
    meleeSwingCooldown_ = 0.1f;
    meleeSwingTimer_ = 0.6f;  // until the sequence's own length is seen (combatFrameInput)
    meleeDurPending_ = true;
}

void WorldScreen::toggleAutoAttack() {
    const glm::vec3 self(playerX_, playerY_, playerZ_);
    if (session_.autoAttackRequested()) {
        session_.stopAutoAttack();
        return;
    }
    uint64_t t = session_.target();
    // (Square used to snap the character around when the player only wanted to
    // hit): the turn is only for a target within reach of a swing. A far or
    // missing target: the nearest enemy in front of the character (60 deg,
    // in reach) becomes the target - hit what you face; nothing there: the
    // far target is attacked without turning (the server answers "too far"),
    // no target at all - the error.
    auto planarTo = [&](uint64_t g, float& dist, float& ang) {
        glm::vec3 p;
        float hh = 0.0f;
        if (!unitPosition(g, p, hh)) return false;
        const float dx = p.x - playerX_, dy = p.y - playerY_;
        dist = std::sqrt(dx * dx + dy * dy);
        float a = std::atan2(dy, dx) - facing_;
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        ang = std::fabs(a);
        return true;
    };
    constexpr float kSwingReach = 5.0f + 1.5f + 1.5f;  // ATTACK_DISTANCE + both reaches (approx.)
    auto aliveHostile = [&](uint64_t g) {
        UnitView u;
        return g != 0 && g != session_.playerGuid() && unitView(g, u) && !u.dead &&
               (session_.isHostileUnit(g) || session_.isHostileAttacker(g));
    };
    float tDist = 1e9f, tAng = 0.0f;
    const bool tNear = t != 0 && t != session_.playerGuid() && planarTo(t, tDist, tAng) && tDist <= kSwingReach;
    if (!tNear) {
        uint64_t best = 0;
        float bestD = kSwingReach;
        for (const auto& [g, ent] : session_.entities().getEntities()) {
            (void)ent;
            float d = 0.0f, a = 0.0f;
            if (!aliveHostile(g) || !planarTo(g, d, a) || a > 1.0472f) continue;  // 60 deg either side
            if (d < bestD) {
                bestD = d;
                best = g;
            }
        }
        if (best != 0) {
            session_.setTarget(best);
            t = best;
        }
    }
    if (t == 0 || t == session_.playerGuid()) {
        combatError_ = CLIENT_STR(SPELL_FAILED_BAD_IMPLICIT_TARGETS);
        combatErrorUntilMs_ = pp::logUptimeMs() + 2000u;
        return;
    }
    float dist = 1e9f, ang = 0.0f;
    const bool inReach = planarTo(t, dist, ang) && dist <= kSwingReach;
    // upstream isAutoFaceTarget (a debug option there, off by default): turn
    // to the target once, on the command. PSP: on by default (wowee.cfg
    // autoface) - the character faces where it last walked, with no mouse
    // to aim it; the server refuses swings at a unit behind. Only
    // within reach.
    glm::vec3 tp;
    float h = 0.0f;
    if (autoFace_ && inReach && unitPosition(t, tp, h)) {
        const float dx = tp.x - playerX_, dy = tp.y - playerY_;
        if (std::fabs(dx) > 0.01f || std::fabs(dy) > 0.01f) {
            facing_ = std::atan2(dy, dx);
            sendMove(game::Opcode::MSG_MOVE_SET_FACING, moving_);
        }
    }
    session_.startAutoAttack(t, self);
}

void WorldScreen::addCombatText(uint8_t type, int32_t amount, bool outgoing, uint64_t dst) {
    if (!combatTextOn_) return;
    if (combatText_.size() >= kMaxCombatText) combatText_.erase(combatText_.begin());
    CombatText c;
    c.type = type;
    c.amount = amount;
    c.outgoing = outgoing;
    c.dst = dst;
    // upstream: random horizontal stagger in -1..1 (a cheap LCG here).
    static uint32_t seed = 0x1234567u;
    seed = seed * 1664525u + 1013904223u;
    c.xSeed = static_cast<float>((seed >> 8) & 0xFFFF) / 32767.5f - 1.0f;
    combatText_.push_back(c);
}

// upstream CombatHandler::handleAttackerStateUpdate (text + reactions) and
// the swing / hit-reaction callbacks the Application wires to the renderer.
void WorldScreen::processMeleeHits() {
    pp::WorldSession::MeleeHit h;
    const uint64_t me = session_.playerGuid();
    while (session_.takeMeleeHit(h)) {
        const bool mine = h.attacker == me;
        const bool atMe = h.victim == me;
        const bool miss = (h.hitInfo & 0x0010) != 0;
        const bool crit = (h.hitInfo & 0x0200) != 0;
        if ((mine || atMe) && ++meleeLogs_ <= 60)
            PSP_LOGI(pp::tag::Net, "melee_hit src=%08X dst=%08X dmg=%d flags=0x%X state=%u",
                     static_cast<unsigned>(h.attacker & 0xFFFFFFFFu), static_cast<unsigned>(h.victim & 0xFFFFFFFFu),
                     static_cast<int>(h.damage), static_cast<unsigned>(h.hitInfo),
                     static_cast<unsigned>(h.victimState));
        if (mine) {
            triggerMeleeSwing();
        } else {
            auto r = remotes_.find(h.attacker);
            if (r != remotes_.end() && r->second.model) {
                for (uint32_t id : anim::meleeAnimChain(anim::MeleeChain::Generic)) {
                    if (!r->second.model->hasAnimation(id)) continue;
                    r->second.model->playAnimation(id, false);
                    r->second.anim = id;
                    break;
                }
            } else if (r == remotes_.end()) {
                creatures_.playAttack(h.attacker);  // upstream npcSwingCallback
            }
        }
        if (!mine && !atMe) continue;  // upstream: not our combat
        onMeleeHitSound(mine, atMe, h.hitInfo, h.victimState);  // sounds (upstream combat sound block)
        if (miss) {
            addCombatText(CT::MISS, 0, mine, h.victim);
        } else if (h.victimState == 1) {
            addCombatText(CT::DODGE, 0, mine, h.victim);
        } else if (h.victimState == 2) {
            addCombatText(CT::PARRY, 0, mine, h.victim);
        } else if (h.victimState == 4) {
            if (h.damage > 0) addCombatText(CT::MELEE_DAMAGE, h.damage, mine, h.victim);
            addCombatText(CT::BLOCK, static_cast<int32_t>(h.blocked), mine, h.victim);
        } else if (h.victimState == 5) {
            addCombatText(CT::EVADE, 0, mine, h.victim);
        } else if (h.victimState == 6) {
            addCombatText(CT::IMMUNE, 0, mine, h.victim);
        } else if (h.victimState == 7) {
            addCombatText(CT::DEFLECT, 0, mine, h.victim);
        } else {
            uint8_t type = CT::MELEE_DAMAGE;
            if (crit) type = CT::CRIT_DAMAGE;
            else if (h.hitInfo & 0x1000) type = CT::CRUSHING;
            else if (h.hitInfo & 0x0800) type = CT::GLANCING;
            addCombatText(type, h.damage, mine, h.victim);
            if (h.absorbed) addCombatText(CT::ABSORB, static_cast<int32_t>(h.absorbed), mine, h.victim);
            if (h.resisted) addCombatText(CT::RESIST, static_cast<int32_t>(h.resisted), mine, h.victim);
        }
        if (miss) continue;
        // Hit reaction on the victim.
        uint32_t react = crit ? anim::COMBAT_CRITICAL : anim::COMBAT_WOUND;
        if (h.victimState == 1) react = anim::DODGE;
        else if (h.victimState == 2) react = 0;  // parry: upstream leaves it
        else if (h.victimState == 4) react = anim::BLOCK;
        if (react == 0) continue;
        if (atMe) {
            if (player_.hasAnimation(react)) animator_.triggerHitReaction(react);
        } else {
            auto r = remotes_.find(h.victim);
            if (r != remotes_.end() && r->second.model && r->second.model->hasAnimation(react)) {
                r->second.model->playAnimation(react, false);
                r->second.anim = react;
            } else if (r == remotes_.end()) {
                creatures_.playWound(h.victim, react);  // upstream hitReactionCallback (NPC)
            }
        }
    }
}

// Other players: Death when their health reaches 0 (held), RISE / Stand
// when they live again (upstream npcDeathCallback / npcRespawnCallback).
void WorldScreen::syncRemoteDeaths() {
    const uint16_t fHealth = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
    const uint16_t fMaxHealth = game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH);
    const uint16_t fDyn = game::fieldIndex(game::UF::UNIT_DYNAMIC_FLAGS);
    for (auto& [guid, r] : remotes_) {
        if (!r.model) continue;
        auto e = session_.entities().getEntity(guid);
        if (!e) continue;
        const bool dead = game::isUnitCorpseState(e->getField(fHealth), e->getField(fMaxHealth),
                                                  e->getField(fDyn) & game::UNIT_DYNFLAG_DEAD);
        auto it = std::find(deadRemotes_.begin(), deadRemotes_.end(), guid);
        const bool shown = it != deadRemotes_.end();
        if (dead && !shown) {
            deadRemotes_.push_back(guid);
            if (r.model->hasAnimation(anim::DEATH)) {
                r.model->playAnimation(anim::DEATH, false);
                r.anim = anim::DEATH;
            }
            PSP_LOGI(pp::tag::Gfx, "remote_dead guid=%08X", static_cast<unsigned>(guid & 0xFFFFFFFFu));
        } else if (!dead && shown) {
            deadRemotes_.erase(it);
            const bool rise = r.model->hasAnimation(anim::RISE);
            r.model->playAnimation(rise ? anim::RISE : anim::STAND, !rise);
            r.anim = rise ? anim::RISE : anim::STAND;
        }
    }
    deadRemotes_.erase(std::remove_if(deadRemotes_.begin(), deadRemotes_.end(),
                                      [&](uint64_t g) { return remotes_.find(g) == remotes_.end(); }),
                       deadRemotes_.end());
}

void WorldScreen::updateCombat(const pp::PadState& pad, float dtSec) {
    PROF_SCOPE("combat");
    if (!combatInit_) initCombat();
    // Life: dead -> Death + grey screen + the DEATH popup; the ghost's teleport, the corpse
    // and the resurrections (ui_world_death.cpp - before the movement).
    updateDeath(pad, dtSec);
    const uint32_t nowMs = pp::logUptimeMs();
    const glm::vec3 self(playerX_, playerY_, playerZ_);
    const Life life = session_.life();

    // Controls (the game pad: idle while the cursor is up).
    const bool lHeld = (pad.held & PSP_CTRL_LTRIGGER) != 0;
    const bool rHeld = (pad.held & PSP_CTRL_RTRIGGER) != 0;
    if (life != Life::Dead && !rHeld) {
        if (!lHeld && (pad.pressed & PSP_CTRL_RIGHT)) tabTarget();
        if (lHeld && (pad.pressed & PSP_CTRL_LEFT)) clearTarget();
        if (lHeld && (pad.pressed & PSP_CTRL_RIGHT)) toggleAutoAttack();
    }
    // The target left the world (despawned / out of range): the original client
    // drops it; the server hears an empty selection.
    if (session_.target() != 0 && session_.target() != session_.playerGuid() &&
        !session_.entities().hasEntity(session_.target()))
        clearTarget();
    if (session_.target() != shownTarget_) {
        shownTarget_ = session_.target();
        UnitView u;
        const bool known = unitView(shownTarget_, u);
        if (++combatLogs_ <= 60)
            PSP_LOGI(pp::tag::Gfx, "target_set guid=%08X type=%s name=%s", static_cast<unsigned>(shownTarget_ & 0xFFFFFFFFu),
                     shownTarget_ == 0 ? "none" : !known ? "?" : u.player ? "player" : "unit",
                     known ? u.name.c_str() : "");
        chat_.setTarget(shownTarget_, known ? u.name : std::string());  // /emote at the target
        playUiSound(shownTarget_ ? UiSound::TargetSelect : UiSound::TargetDeselect);
    }

    session_.updateAutoAttack(dtSec, self);
    const auto err = session_.takeAttackError();
    if (const char* text = attackErrorText(err)) {
        combatError_ = text;
        combatErrorUntilMs_ = nowMs + 2000u;
        if (++combatLogs_ <= 60) PSP_LOGI(pp::tag::Net, "attack_error kind=%u", static_cast<unsigned>(err));
    }
    processMeleeHits();
    syncRemoteDeaths();
    for (auto& c : combatText_) c.age += dtSec;
    combatText_.erase(std::remove_if(combatText_.begin(), combatText_.end(),
                                     [](const CombatText& c) { return c.age >= CT::LIFETIME; }),
                      combatText_.end());

    // Animator flags (upstream GameScreen / playerHealthCallback).
    animator_.setInCombat(session_.inCombat() && life == Life::Alive);
    const uint32_t hp = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_HEALTH));
    const uint32_t maxHp = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH));
    animator_.setLowHealth(maxHp > 0 && hp > 0 && hp * 5 <= maxHp);
    updateSheath(dtSec);  // weapons sheathed out of combat (ui_world_sheath.cpp)

    // A hardware sample every 10 s while fighting (not per frame).
    if ((session_.inCombat() || session_.autoAttackRequested()) && nowMs >= nextCombatLogMs_) {
        nextCombatLogMs_ = nowMs + 10000u;
        uint32_t id = 0;
        float t = 0.0f, dur = 0.0f;
        player_.getAnimationState(id, t, dur);
        PSP_LOGI(pp::tag::Gfx, "combat_frame attacking=%d target=%08X hp=%u/%u anim=%u fsm=%d texts=%u",
                 session_.autoAttacking() ? 1 : 0, static_cast<unsigned>(session_.target() & 0xFFFFFFFFu),
                 static_cast<unsigned>(hp), static_cast<unsigned>(maxHp), static_cast<unsigned>(id),
                 static_cast<int>(animator_.getCombat().getState()), static_cast<unsigned>(combatText_.size()));
    }
}

// --- HUD under the windows -------------------------------------------------------

// NPC names over the heads (upstream draws names above units), nearest first,
// within npcnamedist and the NPC view distance. With dbc.pak the
// colour is upstream's nameplate reaction colour (reactionColor, ui_world_hud.cpp);
// without it (PSP) what is known - red: attacking us, grey: dead, green: an
// NPC with services (UNIT_NPC_FLAGS), yellow: the rest.
void WorldScreen::drawNpcNames(UiRenderer& ui) {
    if (!npcNamesOn_ || !creaturesOn_ || !creatures_.isOpen()) return;
    constexpr size_t kMaxNames = 12;
    const float maxDist = std::min(npcNameDist_, creatures_.viewDistance);
    struct Near {
        uint64_t guid;
        float distSq;
    };
    Near nearest[kMaxNames];
    size_t count = 0;
    for (const auto& [guid, e] : session_.entities().getEntities()) {
        if (e->getType() != game::ObjectType::UNIT) continue;
        const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
        const glm::vec3 d(p.x - playerX_, p.y - playerY_, p.z - playerZ_);
        const float distSq = glm::dot(d, d);
        if (distSq > maxDist * maxDist && guid != session_.target()) continue;
        if (count < kMaxNames) {
            nearest[count++] = {guid, distSq};
        } else {
            size_t far = 0;
            for (size_t i = 1; i < count; ++i)
                if (nearest[i].distSq > nearest[far].distSq) far = i;
            if (distSq < nearest[far].distSq) nearest[far] = {guid, distSq};
        }
    }
    const uint16_t fNpcFlags = game::fieldIndex(game::UF::UNIT_NPC_FLAGS);
    for (size_t i = 0; i < count; ++i) {
        const uint64_t guid = nearest[i].guid;
        UnitView u;
        glm::vec3 pos;
        float h = 0.0f, sx = 0.0f, sy = 0.0f;
        if (!unitView(guid, u) || !unitPosition(guid, pos, h)) continue;
        if (!projectHud(pos + glm::vec3(0, 0, h + 0.4f), sx, sy)) continue;
        auto e = session_.entities().getEntity(guid);
        uint32_t col = 0xFF00FFFFu;  // yellow
        if (u.dead) col = kColGrey;
        else if (factionsOk_) col = reactionColor(guid);  // factions (upstream nameplate colours)
        else if (session_.isHostileAttacker(guid)) col = kColRed;
        else if (e && e->getField(fNpcFlags) != 0) col = kColGreen;
        const bool isTarget = guid == session_.target();
        ui.textShadow(kit_.font(kFontSmall), sx, sy - 10, u.name.c_str(), isTarget ? kColWhite : col, Align::Center);
    }
}

void WorldScreen::drawCombatUnder(UiRenderer& ui) {
    PROF_SCOPE("combat.hud");
    const Life life = session_.life();
    const Rect full{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)};
    // PSP: the GE cannot desaturate the frame; a grey veil stands in for
    // The client's grey death screen (a pale blue one for the ghost).
    if (life == Life::Dead) ui.fill(full, 0x90585858u);
    else if (life == Life::Ghost) ui.fill(full, 0x40FFC080u);
    drawNpcNames(ui);
    // (A mailbox was hard to find:) game objects have no model on the PSP yet - the mailboxes
    // in sight get their name where the model would stand (GameObject name MINIMAP_TRACKING_MAILBOX, 40 yd).
    for (size_t i = 0; i < session_.mailboxCount(); ++i) {
        const glm::vec3 p = session_.mailboxPos(i);
        const glm::vec3 d(p.x - playerX_, p.y - playerY_, p.z - playerZ_);
        float sx = 0.0f, sy = 0.0f;
        if (glm::dot(d, d) > 40.0f * 40.0f || !projectHud(p + glm::vec3(0, 0, 1.6f), sx, sy)) continue;
        ui.textShadow(kit_.font(kFontSmall), sx, sy - 10, CLIENT_STR(MINIMAP_TRACKING_MAILBOX), kColWhite, Align::Center);
    }
    drawPlayerPlates(ui);  // layers - names, then the combat text over them
    // Target marker: a gold bar over the target's head (players' names are
    // drawn by the name plates, NPCs' by drawNpcNames).
    glm::vec3 pos;
    float h = 0.0f, sx = 0.0f, sy = 0.0f;
    const uint64_t t = session_.target();
    if (t != 0 && t != session_.playerGuid() && unitPosition(t, pos, h) &&
        projectHud(pos + glm::vec3(0, 0, h + 0.4f), sx, sy)) {
        const bool swinging = session_.autoAttacking() && session_.autoAttackTarget() == t;
        ui.fill(Rect{sx - 12, sy - 14, 24, 2}, swinging ? 0xFF0000FFu : 0xFF00D1FFu);
    }
    // a block / absorb / resist entry is a part of a hit when a damage or
    // heal entry of the same hit stands beside it (added together for the same victim: the same age);
    // alone it is the whole hit (the word).
    auto partOfHit = [this](const CombatText& e) {
        for (const auto& o : combatText_) {
            if (&o == &e || o.dst != e.dst || o.age != e.age || o.amount <= 0) continue;
            switch (o.type) {
                case CT::MELEE_DAMAGE:
                case CT::CRIT_DAMAGE:
                case CT::GLANCING:
                case CT::CRUSHING:
                case CT::SPELL_DAMAGE:
                case CT::PERIODIC_DAMAGE:
                case CT::HEAL:
                case CT::CRIT_HEAL:
                case CT::PERIODIC_HEAL: return true;
                default: break;
            }
        }
        return false;
    };
    // Floating combat text (upstream CombatUI::renderCombatText).
    for (const auto& c : combatText_) {
        float bx = 0.0f, by = 0.0f;
        if (!unitPosition(c.dst, pos, h)) continue;
        if (!projectHud(pos + glm::vec3(0, 0, 2.5f + c.age * 1.2f), bx, by)) continue;
        const float a = 1.0f - c.age / CT::LIFETIME;
        char text[48];
        // A trailer of the client as the line's text (enUS starts it with a space).
        auto trailer = [&text](const std::string& t) {
            std::snprintf(text, sizeof(text), "%s", t.c_str() + std::min(t.find_first_not_of(' '), t.size()));
        };
        uint32_t col = rgba(1, 1, 1, a);
        const bool out = c.outgoing;
        switch (c.type) {
            case CT::MELEE_DAMAGE:
                std::snprintf(text, sizeof(text), "-%d", static_cast<int>(c.amount));
                col = out ? rgba(1.0f, 1.0f, 0.3f, a) : rgba(1.0f, 0.3f, 0.3f, a);
                break;
            case CT::CRIT_DAMAGE:
                std::snprintf(text, sizeof(text), "-%d!", static_cast<int>(c.amount));
                col = out ? rgba(1.0f, 0.8f, 0.0f, a) : rgba(1.0f, 0.5f, 0.0f, a);
                break;
            case CT::GLANCING:
                std::snprintf(text, sizeof(text), "~%d", static_cast<int>(c.amount));
                col = out ? rgba(0.75f, 0.75f, 0.5f, a) : rgba(0.75f, 0.35f, 0.35f, a);
                break;
            case CT::CRUSHING:
                std::snprintf(text, sizeof(text), "%d!", static_cast<int>(c.amount));
                col = out ? rgba(1.0f, 0.55f, 0.1f, a) : rgba(1.0f, 0.15f, 0.15f, a);
                break;
            // The port's choice for the words above the unit: the names the combat text add-on gives the same
            // events (the combat text add-on's CombatText_OnEvent, GlobalStrings COMBAT_TEXT_*), in the running
            // language: MISS, DODGE, PARRY, EVADE, DEFLECT, REFLECT, BLOCK, IMMUNE, ABSORB, RESIST.
            // The part of a hit that was blocked / absorbed / resisted (a damage or heal line of the same
            // hit beside it): the combat text add-on's trailers BLOCK_TRAILER, ABSORB_TRAILER, RESIST_TRAILER
            // (ruRU's RESIST_TRAILER reads as ABSORB_TRAILER; enUS' trailers start with a space - dropped).
            case CT::MISS:
                std::snprintf(text, sizeof(text), "%s", CLIENT_STR(COMBAT_TEXT_MISS));
                col = rgba(0.7f, 0.7f, 0.7f, a);
                break;
            case CT::DODGE:
            case CT::PARRY:
            case CT::EVADE:
            case CT::DEFLECT:
                std::snprintf(text, sizeof(text), "%s",
                              c.type == CT::DODGE   ? CLIENT_STR(COMBAT_TEXT_DODGE)
                              : c.type == CT::PARRY ? CLIENT_STR(COMBAT_TEXT_PARRY)
                              : c.type == CT::EVADE ? CLIENT_STR(COMBAT_TEXT_EVADE)
                                                    : CLIENT_STR(COMBAT_TEXT_DEFLECT));
                col = out ? rgba(0.6f, 0.6f, 0.6f, a) : rgba(0.4f, 0.9f, 1.0f, a);
                break;
            case CT::BLOCK:
                if (c.amount > 0 && partOfHit(c)) trailer(CLIENT_FMT(BLOCK_TRAILER, static_cast<int>(c.amount)));
                else std::snprintf(text, sizeof(text), "%s", CLIENT_STR(COMBAT_TEXT_BLOCK));
                col = out ? rgba(0.6f, 0.6f, 0.6f, a) : rgba(0.4f, 0.9f, 1.0f, a);
                break;
            case CT::IMMUNE:
                std::snprintf(text, sizeof(text), "%s", CLIENT_STR(COMBAT_TEXT_IMMUNE));
                col = rgba(0.9f, 0.9f, 0.9f, a);
                break;
            case CT::ABSORB:
                if (c.amount > 0 && partOfHit(c)) trailer(CLIENT_FMT(ABSORB_TRAILER, static_cast<int>(c.amount)));
                else std::snprintf(text, sizeof(text), "%s", CLIENT_STR(COMBAT_TEXT_ABSORB));
                col = rgba(0.5f, 0.8f, 1.0f, a);
                break;
            case CT::RESIST:
                if (c.amount > 0 && partOfHit(c)) trailer(CLIENT_FMT(RESIST_TRAILER, static_cast<int>(c.amount)));
                else std::snprintf(text, sizeof(text), "%s", CLIENT_STR(COMBAT_TEXT_RESIST));
                col = rgba(0.7f, 0.7f, 0.7f, a);
                break;
            // spells (upstream CombatUI::renderCombatText colours)
            case CT::SPELL_DAMAGE:
                std::snprintf(text, sizeof(text), "-%d", static_cast<int>(c.amount));
                col = out ? rgba(1.0f, 1.0f, 0.3f, a) : rgba(1.0f, 0.3f, 0.3f, a);
                break;
            case CT::PERIODIC_DAMAGE:
                std::snprintf(text, sizeof(text), "-%d", static_cast<int>(c.amount));
                col = out ? rgba(1.0f, 0.9f, 0.3f, a) : rgba(1.0f, 0.4f, 0.4f, a);
                break;
            case CT::HEAL:
            case CT::PERIODIC_HEAL:
                std::snprintf(text, sizeof(text), "+%d", static_cast<int>(c.amount));
                col = rgba(0.3f, 1.0f, 0.3f, a);
                break;
            case CT::CRIT_HEAL:
                std::snprintf(text, sizeof(text), "+%d!", static_cast<int>(c.amount));
                col = rgba(0.3f, 1.0f, 0.3f, a);
                break;
            case CT::ENERGIZE:
                std::snprintf(text, sizeof(text), "+%d", static_cast<int>(c.amount));
                col = rgba(0.3f, 0.6f, 1.0f, a);
                break;
            case CT::REFLECT:
                std::snprintf(text, sizeof(text), "%s", CLIENT_STR(COMBAT_TEXT_REFLECT));
                col = rgba(0.9f, 0.9f, 0.9f, a);
                break;
            default:
                std::snprintf(text, sizeof(text), "%d", static_cast<int>(c.amount));
                break;
        }
        const bool crit = c.type == CT::CRIT_DAMAGE;
        ui.textShadow(kit_.font(crit ? kFontTitle : kFontNormal), bx + c.xSeed * 20.0f, by, text, col, Align::Center);
    }
    // layers - the refused-attack line moved to the HUD lines
    // (drawHudLines, over the frames and chat); the controls hint is gone
    // (no hints on screen).
}

// --- The death popups (window stack, ui_world_windows.cpp) ---------------------------
// StaticPopup DEATH / RECOVER_CORPSE / RESURRECT / XP_LOSS (ui_world_death.cpp);
// was the PSP's own death window.

bool WorldScreen::drawCombatWindow(UiRenderer& ui, uint8_t w) { return drawDeathWindow(ui, w); }

bool WorldScreen::combatWindowAction(uint32_t act) { return deathWindowAction(act); }

}  // namespace wowee::ui::psp
