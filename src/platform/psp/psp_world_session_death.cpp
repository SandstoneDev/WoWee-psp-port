// death -> ghost -> corpse run -> resurrection for the PSP world session.
// GameHandler / CombatHandler / MovementHandler are not built for the PSP (see
// psp_world_session_combat.cpp), so the steps of this path are mirrored here from
// upstream with its packet builders unchanged:
//   - MovementHandler::handleTeleportAck (3.3.5a layout): MSG_MOVE_TELEPORT_ACK for us is
//     acknowledged at once (packed guid, counter, time) - until then the server drops
//     every movement packet we send - and the destination goes to the world screen,
//     which owns the player's position (takeTeleport). This is how the ghost reaches
//     the graveyard after CMSG_REPOP_REQUEST, and how a resurrection from another
//     player moves us to the caster. PSP: only teleports on this map (SMSG_NEW_WORLD -
//     another map - is logged, not followed: one terrain pak per map is loaded).
//   - EntityController::markPlayerDead (the corpse where we fell), onCreateCorpse (our
//     CORPSE object: its guid for CMSG_RECLAIM_CORPSE) and the PLAYER_FLAGS ghost edges
//     (entering ghost form without a corpse position asks MSG_CORPSE_QUERY - the
//     login-as-ghost case; leaving it forgets the corpse).
//   - GameHandler: MSG_CORPSE_QUERY (the travel map's coordinates), SMSG_CORPSE_RECLAIM_DELAY,
//     SMSG_DEATH_RELEASE_LOC (the spirit healer; map -1 withdraws it),
//     SMSG_RESURRECT_REQUEST (caster, name length + name, sickness, optional timer word),
//     SMSG_SPIRIT_HEALER_CONFIRM (-> the CONFIRM_XP_LOSS prompt).
//   - CombatHandler::canReclaimCorpse / reclaimCorpse / acceptResurrect / declineResurrect.
// The world screen draws the prompts (ui_world_death.cpp).
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"

#include <cmath>

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kPlayerFlagsGhost = 0x00000010;  // upstream PLAYER_FLAGS_GHOST
// PSP: AzerothCore's CORPSE_RECLAIM_RADIUS (39 yd, the original client's CORPSE_IN_RANGE);
// upstream asks 40, which offers a reclaim the server refuses in the last yard.
constexpr float kReclaimRadius = 39.0f;
constexpr uint16_t kCorpseFieldOwner = 6;  // CORPSE_FIELD_OWNER (uint64 at 6/7), upstream onCreateCorpse
}  // namespace

// --- Update blocks ------------------------------------------------------------------

// upstream EntityController::onCreateCorpse: our own corpse object gives the guid
// CMSG_RECLAIM_CORPSE names (and its position when the block carries one).
void WorldSession::deathUpdateBlock(const game::UpdateBlock& b) {
    using game::UpdateType;
    if (b.objectType != game::ObjectType::CORPSE) return;
    if (b.updateType != UpdateType::CREATE_OBJECT && b.updateType != UpdateType::CREATE_OBJECT2) return;
    auto lo = b.fields.find(kCorpseFieldOwner);
    auto hi = b.fields.find(static_cast<uint16_t>(kCorpseFieldOwner + 1));
    const uint32_t ownerLow = lo != b.fields.end() ? lo->second : 0u;
    const uint32_t ownerHigh = hi != b.fields.end() ? hi->second : 0u;
    const uint64_t owner = (static_cast<uint64_t>(ownerHigh) << 32) | ownerLow;
    if (playerGuid_ == 0 || (owner != playerGuid_ && ownerLow != static_cast<uint32_t>(playerGuid_))) return;
    corpseGuid_ = b.guid;
    if (b.hasMovement) {
        corpsePos_ = glm::vec3(b.x, b.y, b.z);
        corpseMapId_ = worldEntry_.mapId;
        corpseValid_ = true;
    }
    PSP_LOGI(tag::Net, "corpse_object guid=%08X pos=%d x=%.1f y=%.1f z=%.1f",
             static_cast<unsigned>(b.guid & 0xFFFFFFFFu), b.hasMovement ? 1 : 0, b.x, b.y, b.z);
}

// upstream EntityController (PLAYER_FLAGS): the ghost flag's edges. Called after every
// own update block (create too: logging in as a ghost carries the flag on the create).
void WorldSession::deathOwnFields() {
    const bool ghost = (selfField(game::fieldIndex(game::UF::PLAYER_FLAGS)) & kPlayerFlagsGhost) != 0;
    if (ghost == ghostSeen_) return;
    ghostSeen_ = ghost;
    if (ghost) {
        // upstream: "Player logged in as ghost" -> MSG_CORPSE_QUERY so the marker is right.
        if (!corpseValid_) queryCorpse();
        PSP_LOGI(tag::Net, "ghost_on corpse=%d", corpseValid_ ? 1 : 0);
    } else {
        // upstream: "Player resurrected (PLAYER_FLAGS ghost cleared)" - the corpse is gone.
        corpseValid_ = false;
        corpseMapId_ = 0;
        corpseGuid_ = 0;
        corpseReclaimAtMs_ = 0;
        graveyardValid_ = false;
        resOffer_.pending = false;
        lastRepopMs_ = 0;
        PSP_LOGI(tag::Net, "ghost_off");
    }
}

// --- Corpse ---------------------------------------------------------------------------

void WorldSession::markPlayerDead(const glm::vec3& pos) {
    corpsePos_ = pos;
    corpseMapId_ = worldEntry_.mapId;
    corpseValid_ = true;
    PSP_LOGI(tag::Net, "corpse_cached x=%.1f y=%.1f z=%.1f map=%u", pos.x, pos.y, pos.z,
             static_cast<unsigned>(corpseMapId_));
}

void WorldSession::queryCorpse() {
    if (!socket_) return;
    network::Packet cq(game::wireOpcode(game::Opcode::MSG_CORPSE_QUERY));
    socket_->send(cq);
}

// upstream CombatHandler::canReclaimCorpse (PSP: 39 yd, see kReclaimRadius; the corpse
// guid is not required - AzerothCore's HandleReclaimCorpseOpcode reads it and then uses
// the player's own corpse, so a corpse object that never reached us does not strand the
// ghost).
bool WorldSession::canReclaimCorpse(const glm::vec3& selfPos) const {
    if (!ghostSeen_ || !corpseKnown()) return false;
    const glm::vec3 d = selfPos - corpsePos_;
    return glm::dot(d, d) <= kReclaimRadius * kReclaimRadius;
}

float WorldSession::corpseReclaimDelaySec() const {
    if (corpseReclaimAtMs_ == 0) return 0.0f;
    const uint32_t now = logUptimeMs();
    if (static_cast<int32_t>(corpseReclaimAtMs_ - now) <= 0) return 0.0f;
    return static_cast<float>(corpseReclaimAtMs_ - now) / 1000.0f;
}

void WorldSession::reclaimCorpse(const glm::vec3& selfPos) {
    if (state_ != State::InWorld || !socket_ || !canReclaimCorpse(selfPos)) return;
    socket_->send(game::ReclaimCorpsePacket::build(corpseGuid_));
    PSP_LOGI(tag::Net, "reclaim_corpse guid=%08X", static_cast<unsigned>(corpseGuid_ & 0xFFFFFFFFu));
}

// --- Resurrection offers ----------------------------------------------------------------

// upstream CombatHandler::acceptResurrect.
void WorldSession::acceptResurrect() {
    if (state_ != State::InWorld || !socket_ || !resOffer_.pending) {
        PSP_LOGW(tag::Net, "resurrect_accept refused offer=%d", resOffer_.pending ? 1 : 0);
        return;
    }
    if (resOffer_.spiritHealer) {
        socket_->send(game::SpiritHealerActivatePacket::build(resOffer_.caster));
        PSP_LOGI(tag::Net, "spirit_healer_activate guid=%08X", static_cast<unsigned>(resOffer_.caster & 0xFFFFFFFFu));
    } else {
        socket_->send(game::ResurrectResponsePacket::build(resOffer_.caster, true));
        PSP_LOGI(tag::Net, "resurrect_response accept=1 guid=%08X", static_cast<unsigned>(resOffer_.caster & 0xFFFFFFFFu));
    }
    resOffer_.pending = false;
}

// upstream CombatHandler::declineResurrect.
void WorldSession::declineResurrect() {
    if (state_ != State::InWorld || !socket_ || !resOffer_.pending) return;
    socket_->send(game::ResurrectResponsePacket::build(resOffer_.caster, false));
    PSP_LOGI(tag::Net, "resurrect_response accept=0 guid=%08X", static_cast<unsigned>(resOffer_.caster & 0xFFFFFFFFu));
    resOffer_.pending = false;
}

// --- Teleport ---------------------------------------------------------------------------

bool WorldSession::takeTeleport(glm::vec3& pos, float& orientation) {
    if (!teleportPending_) return false;
    teleportPending_ = false;
    pos = teleportPos_;
    orientation = teleportO_;
    return true;
}

// --- Packets ----------------------------------------------------------------------------

bool WorldSession::handleDeathPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::MSG_MOVE_TELEPORT_ACK: {
            // upstream handleTeleportAck, 3.3.5a: packed guid, counter, flags, flags2 (u16),
            // time, x, y, z, orientation.
            if (!packet.hasRemaining(1)) return true;
            const uint64_t guid = packet.readPackedGuid();
            if (!packet.hasRemaining(4 + 4 + 2 + 4 + 16)) return true;
            const uint32_t counter = packet.readUInt32();
            packet.readUInt32();  // moveFlags
            packet.readUInt16();  // moveFlags2
            const uint32_t moveTime = packet.readUInt32();
            const float x = packet.readFloat(), y = packet.readFloat(), z = packet.readFloat();
            const float o = packet.readFloat();
            if (guid != playerGuid_) {
                // upstream: a remote entity is put there.
                if (auto e = entities_.getEntity(guid)) {
                    const glm::vec3 c = core::coords::serverToCanonical(glm::vec3(x, y, z));
                    e->setPosition(c.x, c.y, c.z, core::coords::serverToCanonicalYaw(o));
                }
                return true;
            }
            // upstream: a destination at the map origin is a server misfire - acknowledged
            // (the server ignores our movement until it is), the position not taken.
            const bool originMisfire = worldEntry_.mapId == 0 && std::fabs(x) < 50.0f && std::fabs(y) < 50.0f;
            if (!originMisfire) {
                teleportPending_ = true;
                teleportPos_ = glm::vec3(x, y, z);
                teleportO_ = o;
            }
            network::Packet ack(game::wireOpcode(game::Opcode::MSG_MOVE_TELEPORT_ACK));
            ack.writePackedGuid(playerGuid_);
            ack.writeUInt32(counter);
            ack.writeUInt32(moveTime);
            socket_->send(ack);
            PSP_LOGI(tag::Net, "teleport_ack x=%.1f y=%.1f z=%.1f o=%.2f counter=%u misfire=%d life=%u", x, y, z, o,
                     static_cast<unsigned>(counter), originMisfire ? 1 : 0, static_cast<unsigned>(life()));
            logFlush();
            return true;
        }
        case LogicalOpcode::SMSG_NEW_WORLD:
        case LogicalOpcode::SMSG_TRANSFER_PENDING:
            // PSP: another map is not followed (one terrain pak per map); the owner sees why.
            if (++deathLogs_ <= 20)
                PSP_LOGW(tag::Net, "world_transfer op=%s (another map: not supported on the PSP)",
                         op == LogicalOpcode::SMSG_NEW_WORLD ? "new_world" : "transfer_pending");
            return true;
        case LogicalOpcode::MSG_CORPSE_QUERY: {
            // upstream: found(1) + travel map(4) + x, y, z + corpse map(4) [+ unk(4)]. The
            // coordinates belong to the travel map (a dungeon corpse: its entrance).
            if (!packet.hasRemaining(1)) return true;
            const uint8_t found = packet.readUInt8();
            if (!found || !packet.hasRemaining(20)) {
                PSP_LOGI(tag::Net, "corpse_query found=0");
                return true;
            }
            const uint32_t travelMap = packet.readUInt32();
            const float cx = packet.readFloat(), cy = packet.readFloat(), cz = packet.readFloat();
            const uint32_t corpseMap = packet.readUInt32();
            corpsePos_ = glm::vec3(cx, cy, cz);
            corpseMapId_ = travelMap;
            corpseValid_ = true;
            PSP_LOGI(tag::Net, "corpse_query map=%u x=%.1f y=%.1f z=%.1f corpse_map=%u", static_cast<unsigned>(travelMap),
                     cx, cy, cz, static_cast<unsigned>(corpseMap));
            return true;
        }
        case LogicalOpcode::SMSG_CORPSE_RECLAIM_DELAY: {
            if (!packet.hasRemaining(4)) return true;
            const uint32_t delayMs = packet.readUInt32();
            const uint32_t now = logUptimeMs();
            corpseReclaimAtMs_ = (now + delayMs) ? now + delayMs : 1u;
            PSP_LOGI(tag::Net, "corpse_reclaim_delay ms=%u", static_cast<unsigned>(delayMs));
            return true;
        }
        case LogicalOpcode::SMSG_DEATH_RELEASE_LOC: {
            if (!packet.hasRemaining(16)) return true;
            const uint32_t map = packet.readUInt32();
            const float x = packet.readFloat(), y = packet.readFloat(), z = packet.readFloat();
            if (map == 0xFFFFFFFFu) {  // ResurrectPlayer takes the marker down
                graveyardValid_ = false;
            } else {
                graveyardValid_ = true;
                graveyardMapId_ = map;
                graveyardPos_ = glm::vec3(x, y, z);
            }
            PSP_LOGI(tag::Net, "death_release_loc map=%d x=%.1f y=%.1f z=%.1f", static_cast<int>(map), x, y, z);
            return true;
        }
        case LogicalOpcode::SMSG_SPIRIT_HEALER_CONFIRM: {
            if (!packet.hasRemaining(8)) return true;
            const uint64_t npc = packet.readUInt64();
            if (!npc) return true;
            resOffer_.caster = npc;
            resOffer_.name.clear();
            resOffer_.spiritHealer = true;
            resOffer_.sickness = false;
            resOffer_.hasTimer = false;
            resOffer_.pending = true;
            ++resOffer_.seq;
            PSP_LOGI(tag::Net, "spirit_healer_confirm guid=%08X", static_cast<unsigned>(npc & 0xFFFFFFFFu));
            return true;
        }
        case LogicalOpcode::SMSG_RESURRECT_REQUEST: {
            // upstream (Spell::SendResurrectRequest): caster guid, name length, name + 0,
            // a second 0, sickness (1 from a creature), uint32 0 only when the spell waives
            // the reclaim delay.
            if (!packet.hasRemaining(12)) {
                packet.skipAll();
                return true;
            }
            const uint64_t caster = packet.readUInt64();
            packet.readUInt32();  // name length
            std::string name = packet.readString();
            if (packet.hasRemaining(1)) packet.readUInt8();
            const bool sickness = packet.hasRemaining(1) && packet.readUInt8() != 0;
            const bool hasTimer = !packet.hasRemaining(4);
            packet.skipAll();
            if (!caster) return true;
            if (name.empty()) {
                if (const std::string* n = nameOf(caster)) name = *n;
            }
            resOffer_.caster = caster;
            resOffer_.name = name;
            resOffer_.spiritHealer = false;
            resOffer_.sickness = sickness;
            resOffer_.hasTimer = hasTimer;
            resOffer_.pending = true;
            ++resOffer_.seq;
            PSP_LOGI(tag::Net, "resurrect_request guid=%08X name=%s sickness=%d timer=%d",
                     static_cast<unsigned>(caster & 0xFFFFFFFFu), name.c_str(), sickness ? 1 : 0, hasTimer ? 1 : 0);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace wowee::platform::psp
