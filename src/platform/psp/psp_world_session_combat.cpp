// target, melee combat, creature names and the player's death for
// the PSP world session. GameHandler/CombatHandler are not built for the PSP
// (they pull in rendering, audio and the addon layer), so the steps the PSP
// needs are mirrored here from upstream, with upstream's packet builders and
// parsers unchanged:
//   - CombatHandler::setTarget / clearTarget        -> setTarget (CMSG_SET_SELECTION)
//   - CombatHandler::startAutoAttack / stopAutoAttack / updateAutoAttack
//     (CMSG_ATTACKSWING / CMSG_ATTACKSTOP, 8 yd start gate, 5.75 yd melee
//     range, 40 yd leash, 0.5 s re-send after SMSG_ATTACKSTOP)
//   - CombatHandler::registerOpcodes: SMSG_ATTACKSTART/STOP, ATTACKSWING_*,
//     SMSG_CANCEL_COMBAT, SMSG_ATTACKERSTATEUPDATE (handleAttackerStateUpdate),
//     SMSG_HEALTH_UPDATE / SMSG_POWER_UPDATE (handleHealthUpdate / PowerUpdate)
//   - EntityController::queryCreatureInfo / handleCreatureQueryResponse
//     (CMSG_CREATURE_QUERY on a unit's creation, cache by entry)
//   - EntityController death tracking (health 0 / UNIT_DYNFLAG_DEAD /
//     PLAYER_FLAGS_GHOST, SMSG_FORCED_DEATH_UPDATE) and
//     CombatHandler::releaseSpirit (CMSG_REPOP_REQUEST + MSG_CORPSE_QUERY).
//   the rest of the death path (teleport, corpse, resurrection) is in
//   psp_world_session_death.cpp.
// The world screen (ui_world_target.cpp / ui_world_combat.cpp) draws it.
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kPlayerFlagsGhost = 0x00000010;  // upstream PLAYER_FLAGS_GHOST
constexpr size_t kMaxQueuedHits = 32;
}  // namespace

// --- Update blocks ------------------------------------------------------------

// Called first for every update block. The own player is not an entity here
// (the world screen moves it), so its fields are kept on their own: health,
// power, level and the death / ghost flags for the player frame. A unit's
// creation sends the creature query for its entry (upstream
// EntityController, "queryCreatureInfo(entry, guid)" on CREATE_OBJECT).
// Returns true when the block is fully handled (the own player's).
bool WorldSession::combatUpdateBlock(const game::UpdateBlock& b) {
    using game::UpdateType;
    deathUpdateBlock(b);  // our corpse object (psp_world_session_death.cpp)
    const bool create = b.updateType == UpdateType::CREATE_OBJECT || b.updateType == UpdateType::CREATE_OBJECT2;
    if (playerGuid_ != 0 && b.guid == playerGuid_) {
        if (create || b.updateType == UpdateType::VALUES) {
            const bool first = selfFields_.empty();
            const uint16_t fHealth = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
            // the skill slots (PLAYER_SKILL_INFO_START + 128 x 3) re-read when touched.
            const uint16_t fSkill0 = game::fieldIndex(game::UF::PLAYER_SKILL_INFO_START);
            for (const auto& [k, v] : b.fields) {
                selfFields_[k] = v;
                if (k == fHealth && v > 0) forcedDead_ = false;
                if (k >= fSkill0 && k < fSkill0 + 128 * 3) skillsDirty_ = true;
            }
            if (first && !selfFields_.empty())
                PSP_LOGI(tag::Net, "self_fields n=%u hp=%u/%u level=%u", static_cast<unsigned>(selfFields_.size()),
                         static_cast<unsigned>(selfField(fHealth)),
                         static_cast<unsigned>(selfField(game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH))),
                         static_cast<unsigned>(selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL))));
            deathOwnFields();  // the ghost flag's edges
        }
        return true;  // was: create -> ignored, values/movement -> no such entity
    }
    if (create && b.objectType == game::ObjectType::UNIT) {
        auto it = b.fields.find(game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY));
        if (it != b.fields.end() && it->second != 0) queryCreature(it->second, b.guid);
    }
    return false;
}

uint32_t WorldSession::selfField(uint16_t index) const {
    auto it = selfFields_.find(index);
    return it == selfFields_.end() ? 0u : it->second;
}

WorldSession::Life WorldSession::life() const {
    if (selfFields_.empty()) return forcedDead_ ? Life::Dead : Life::Alive;
    if (selfField(game::fieldIndex(game::UF::PLAYER_FLAGS)) & kPlayerFlagsGhost) return Life::Ghost;
    const uint32_t hp = selfField(game::fieldIndex(game::UF::UNIT_FIELD_HEALTH));
    const uint32_t maxHp = selfField(game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH));
    const uint32_t dyn = selfField(game::fieldIndex(game::UF::UNIT_DYNAMIC_FLAGS));
    if (forcedDead_ || (maxHp > 0 && hp == 0) || (dyn & game::UNIT_DYNFLAG_DEAD)) return Life::Dead;
    return Life::Alive;
}

// --- Creature names -------------------------------------------------------------

void WorldSession::queryCreature(uint32_t entry, uint64_t guid) {
    if (creatureInfo_.count(entry) || !socket_) return;
    if (!creatureQueried_.insert(entry).second) return;
    socket_->send(game::CreatureQueryPacket::build(entry, guid));
}

const game::CreatureQueryResponseData* WorldSession::creatureInfo(uint32_t entry) const {
    auto it = creatureInfo_.find(entry);
    return it == creatureInfo_.end() ? nullptr : &it->second;
}

// --- Target -----------------------------------------------------------------------

// upstream CombatHandler::setTarget (CMSG_SET_SELECTION). PSP: clearing sends
// CMSG_SET_SELECTION with guid 0 as the original client does (upstream clearTarget
// only forgets it locally), so the server and the PC clients see no target.
void WorldSession::setTarget(uint64_t guid) {
    if (guid == targetGuid_) return;
    targetGuid_ = guid;
    if (state_ == State::InWorld && socket_) socket_->send(game::SetSelectionPacket::build(guid));
}

// upstream CombatHandler::autoTargetAttacker: an attacker becomes the target
// when there is none.
void WorldSession::autoTargetAttacker(uint64_t guid) {
    if (guid == 0 || guid == playerGuid_ || targetGuid_ != 0) return;
    if (!entities_.hasEntity(guid)) return;
    setTarget(guid);
}

// --- Auto-attack ------------------------------------------------------------------

void WorldSession::raiseAttackError(AttackError e) {
    // upstream raiseUiError behind autoAttackRangeWarnCooldown_ (1.25 s)
    if (rangeWarnCooldown_ > 0.0f) return;
    attackError_ = e;
    rangeWarnCooldown_ = 1.25f;
}

WorldSession::AttackError WorldSession::takeAttackError() {
    const AttackError e = attackError_;
    attackError_ = AttackError::None;
    return e;
}

void WorldSession::startAutoAttack(uint64_t guid, const glm::vec3& selfPos) {
    if (guid == playerGuid_ || guid == 0) return;
    // Client-side range gate (upstream: 8 yd melee, 40 with a ranged weapon -
    // the PSP has no inventory yet, so melee only).
    if (auto t = entities_.getEntity(guid)) {
        const glm::vec3 tp = core::coords::canonicalToServer(glm::vec3(t->getLatestX(), t->getLatestY(), t->getLatestZ()));
        const glm::vec3 d = selfPos - tp;
        if (std::sqrt(glm::dot(d, d)) > 8.0f) {
            raiseAttackError(AttackError::TooFar);
            return;
        }
    }
    // mounts - upstream startAutoAttack: a valid attack attempt dismounts.
    if (isMounted()) dismount();
    autoAttackRequested_ = true;
    autoAttackRetryPending_ = true;
    // Server-authoritative: autoAttacking_ turns on with SMSG_ATTACKSTART.
    autoAttacking_ = false;
    autoAttackTarget_ = guid;
    autoAttackOutOfRange_ = false;
    autoAttackOutOfRangeTime_ = 0.0f;
    autoAttackResendTimer_ = 0.0f;
    if (state_ == State::InWorld && socket_) socket_->send(game::AttackSwingPacket::build(guid));
    PSP_LOGI(tag::Net, "attack_swing on=1 guid=%08X", static_cast<unsigned>(guid & 0xFFFFFFFFu));
}

void WorldSession::stopAutoAttack() {
    if (!autoAttacking_ && !autoAttackRequested_) return;
    autoAttackRequested_ = false;
    autoAttacking_ = false;
    autoAttackRetryPending_ = false;
    autoAttackTarget_ = 0;
    autoAttackOutOfRange_ = false;
    autoAttackOutOfRangeTime_ = 0.0f;
    autoAttackResendTimer_ = 0.0f;
    if (state_ == State::InWorld && socket_) socket_->send(game::AttackStopPacket::build());
    PSP_LOGI(tag::Net, "attack_swing on=0");
}

// upstream CombatHandler::updateAutoAttack (3.3.5a branch: melee range 5.75,
// re-send every 0.5 s) + the death checks EntityController makes when a
// unit's health reaches 0 (stop attacking it, forget it as an attacker).
void WorldSession::updateAutoAttack(float dtSec, const glm::vec3& selfPos) {
    if (rangeWarnCooldown_ > 0.0f) rangeWarnCooldown_ = std::max(0.0f, rangeWarnCooldown_ - dtSec);
    const uint16_t fHealth = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
    const uint16_t fMaxHealth = game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH);
    const uint16_t fDyn = game::fieldIndex(game::UF::UNIT_DYNAMIC_FLAGS);
    auto isDead = [&](const game::Entity& e) {
        return game::isUnitCorpseState(e.getField(fHealth), e.getField(fMaxHealth), e.getField(fDyn) & game::UNIT_DYNFLAG_DEAD);
    };
    for (auto it = hostileAttackers_.begin(); it != hostileAttackers_.end();) {
        auto e = entities_.getEntity(*it);
        it = (!e || isDead(*e)) ? hostileAttackers_.erase(it) : std::next(it);
    }
    if (autoAttackRequested_ && autoAttackTarget_ != 0) {
        auto t = entities_.getEntity(autoAttackTarget_);
        if (t && isDead(*t)) {
            stopAutoAttack();
        } else if (t) {
            const glm::vec3 tp =
                core::coords::canonicalToServer(glm::vec3(t->getLatestX(), t->getLatestY(), t->getLatestZ()));
            const glm::vec3 d = selfPos - tp;
            const float dist = std::sqrt(d.x * d.x + d.y * d.y);
            const float dist3d = std::sqrt(glm::dot(d, d));
            if (dist > 40.0f) {
                stopAutoAttack();
                PSP_LOGI(tag::Net, "attack_leash dist=%.1f", dist);
            } else if (state_ == State::InWorld && socket_) {
                bool allowResync = true;
                if (dist3d > 5.75f) {
                    autoAttackOutOfRange_ = true;
                    autoAttackOutOfRangeTime_ += dtSec;
                    raiseAttackError(AttackError::NotInRange);
                    if (autoAttackOutOfRangeTime_ > 2.0f && dist3d > 9.0f) {
                        stopAutoAttack();
                        attackError_ = AttackError::OutOfRange;
                        allowResync = false;
                    }
                } else {
                    autoAttackOutOfRange_ = false;
                    autoAttackOutOfRangeTime_ = 0.0f;
                }
                if (allowResync) {
                    autoAttackResendTimer_ += dtSec;
                    if (!autoAttacking_ && !autoAttackOutOfRange_ && autoAttackRetryPending_ &&
                        autoAttackResendTimer_ >= 0.5f) {
                        autoAttackResendTimer_ = 0.0f;
                        autoAttackRetryPending_ = false;
                        socket_->send(game::AttackSwingPacket::build(autoAttackTarget_));
                    }
                }
            }
        }
    }
    // Keep active melee attackers facing the player (upstream: canonical
    // atan2(-dy, dx) toward the player's position).
    if (!hostileAttackers_.empty()) {
        const glm::vec3 me = core::coords::serverToCanonical(selfPos);
        for (uint64_t g : hostileAttackers_) {
            auto a = entities_.getEntity(g);
            if (!a) continue;
            const float dx = me.x - a->getX(), dy = me.y - a->getY();
            if (std::fabs(dx) < 0.01f && std::fabs(dy) < 0.01f) continue;
            a->setOrientation(std::atan2(-dy, dx));
        }
    }
}

bool WorldSession::takeMeleeHit(MeleeHit& out) {
    if (meleeHits_.empty()) return false;
    out = meleeHits_.front();
    meleeHits_.erase(meleeHits_.begin());
    return true;
}

// --- Death --------------------------------------------------------------------------

// upstream CombatHandler::releaseSpirit: at most once a second while pending.
void WorldSession::releaseSpirit() {
    if (state_ != State::InWorld || !socket_) return;
    const uint32_t now = logUptimeMs();
    if (lastRepopMs_ != 0 && now - lastRepopMs_ < 1000u) return;
    lastRepopMs_ = now ? now : 1u;
    socket_->send(game::RepopRequestPacket::build());
    network::Packet cq(game::wireOpcode(game::Opcode::MSG_CORPSE_QUERY));
    socket_->send(cq);
    PSP_LOGI(tag::Net, "repop_request");
}

// --- Packets --------------------------------------------------------------------------

bool WorldSession::handleCombatPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    if (op == LogicalOpcode::SMSG_CREATURE_QUERY_RESPONSE) {
        game::CreatureQueryResponseData d;
        if (game::CreatureQueryResponseParser::parse(packet, d) && d.entry != 0) {
            creatureQueried_.insert(d.entry);
            if (d.isValid()) {
                ++creatureInfoVersion_;
                if (++combatLogs_ <= 60)
                    PSP_LOGI(tag::Net, "creature_query entry=%u name=%s rank=%u", static_cast<unsigned>(d.entry),
                             d.name.c_str(), static_cast<unsigned>(d.rank));
                creatureInfo_[d.entry] = std::move(d);
            }
        }
        return true;
    }
    if (state_ != State::InWorld) return false;
    if (handleDeathPacket(op, packet)) return true;  // ghost, corpse, resurrection, teleport
    switch (op) {
        case LogicalOpcode::SMSG_ATTACKSTART: {
            game::AttackStartData d;
            if (!game::AttackStartParser::parse(packet, d)) return true;
            if (d.attackerGuid == playerGuid_) {
                autoAttackRequested_ = true;
                autoAttacking_ = true;
                autoAttackRetryPending_ = false;
                autoAttackTarget_ = d.victimGuid;
            } else if (d.victimGuid == playerGuid_ && d.attackerGuid != 0) {
                hostileAttackers_.insert(d.attackerGuid);
                autoTargetAttacker(d.attackerGuid);
            }
            if ((d.attackerGuid == playerGuid_ || d.victimGuid == playerGuid_) && ++combatLogs_ <= 60)
                PSP_LOGI(tag::Net, "attack_start src=%08X dst=%08X", static_cast<unsigned>(d.attackerGuid & 0xFFFFFFFFu),
                         static_cast<unsigned>(d.victimGuid & 0xFFFFFFFFu));
            return true;
        }
        case LogicalOpcode::SMSG_ATTACKSTOP: {
            game::AttackStopData d;
            if (!game::AttackStopParser::parse(packet, d)) return true;
            // Keep the intent; the server-confirmed state waits for ATTACKSTART.
            if (d.attackerGuid == playerGuid_) {
                autoAttacking_ = false;
                autoAttackRetryPending_ = autoAttackRequested_;
                autoAttackResendTimer_ = 0.0f;
            } else if (d.victimGuid == playerGuid_) {
                hostileAttackers_.erase(d.attackerGuid);
            }
            return true;
        }
        case LogicalOpcode::SMSG_ATTACKSWING_NOTINRANGE:
            autoAttackOutOfRange_ = true;
            raiseAttackError(AttackError::NotInRange);
            return true;
        case LogicalOpcode::SMSG_ATTACKSWING_BADFACING:
            // Reported, not corrected (upstream: the facing is the player's).
            raiseAttackError(AttackError::BadFacing);
            return true;
        case LogicalOpcode::SMSG_ATTACKSWING_NOTSTANDING:
            autoAttackOutOfRange_ = false;
            autoAttackOutOfRangeTime_ = 0.0f;
            raiseAttackError(AttackError::NotStanding);
            return true;
        case LogicalOpcode::SMSG_ATTACKSWING_CANT_ATTACK:
            stopAutoAttack();
            raiseAttackError(AttackError::CantAttack);
            return true;
        case LogicalOpcode::SMSG_ATTACKSWING_DEADTARGET:
            autoAttacking_ = false;
            autoAttackTarget_ = 0;
            return true;
        case LogicalOpcode::SMSG_CANCEL_COMBAT:
            autoAttacking_ = false;
            autoAttackTarget_ = 0;
            autoAttackRequested_ = false;
            autoAttackRetryPending_ = false;
            hostileAttackers_.clear();
            return true;
        case LogicalOpcode::SMSG_ATTACKERSTATEUPDATE: {
            game::AttackerStateUpdateData d;
            if (!game::AttackerStateUpdateParser::parse(packet, d)) return true;
            // PSP: upstream drops swings between two other units here ("not our
            // combat"); the PSP keeps them so another player's model swings
            // (the screen shows numbers only for the player's own fights).
            if (d.targetGuid == playerGuid_ && d.attackerGuid != 0) {
                hostileAttackers_.insert(d.attackerGuid);
                autoTargetAttacker(d.attackerGuid);
            }
            MeleeHit h;
            h.attacker = d.attackerGuid;
            h.victim = d.targetGuid;
            h.damage = d.totalDamage;
            h.hitInfo = d.hitInfo;
            h.victimState = d.victimState;
            h.blocked = d.blocked;
            for (const auto& s : d.subDamages) {
                h.absorbed += s.absorbed;
                h.resisted += s.resisted;
            }
            if (meleeHits_.size() >= kMaxQueuedHits) meleeHits_.erase(meleeHits_.begin());
            meleeHits_.push_back(h);
            return true;
        }
        case LogicalOpcode::SMSG_HEALTH_UPDATE: {
            // upstream handleHealthUpdate (3.3.5a: packed guid, uint32 health)
            if (!packet.hasRemaining(2)) return true;
            const uint64_t guid = packet.readPackedGuid();
            if (!packet.hasRemaining(4)) return true;
            const uint32_t hp = packet.readUInt32();
            const uint16_t f = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
            if (guid == playerGuid_) {
                selfFields_[f] = hp;
                if (hp > 0) forcedDead_ = false;
            } else if (auto e = entities_.getEntity(guid)) {
                e->setField(f, hp);
            }
            return true;
        }
        case LogicalOpcode::SMSG_POWER_UPDATE: {
            // upstream handlePowerUpdate (packed guid, uint8 power type, uint32 value)
            if (!packet.hasRemaining(2)) return true;
            const uint64_t guid = packet.readPackedGuid();
            if (!packet.hasRemaining(5)) return true;
            const uint8_t type = packet.readUInt8();
            const uint32_t value = packet.readUInt32();
            if (type >= 7) return true;
            const uint16_t f = static_cast<uint16_t>(game::fieldIndex(game::UF::UNIT_FIELD_POWER1) + type);
            if (guid == playerGuid_) {
                selfFields_[f] = value;
            } else if (auto e = entities_.getEntity(guid)) {
                e->setField(f, value);
            }
            return true;
        }
        case LogicalOpcode::SMSG_FORCED_DEATH_UPDATE:
            forcedDead_ = true;
            PSP_LOGI(tag::Net, "forced_death");
            return true;
        default:
            return false;
    }
}

}  // namespace wowee::platform::psp
