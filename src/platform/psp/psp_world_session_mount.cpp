// mounts - the session half.
//
// Upstream, ported without GameHandler (logic unchanged):
//   - EntityController::detectPlayerMountChange: the own
//     UNIT_FIELD_MOUNTDISPLAYID is the mount; a non-zero value that arrives
//     inside the dismount grace (MovementHandler::dismount: 1.5 s) is stale and
//     ignored, a zero ends the grace; on mounting, the mount aura is the spell
//     the player just cast if it carries SPELL_AURA_MOUNTED, else an own aura
//     that does (upstream: lastGroundCastSpellId_, then the indefinite aura scan).
//   - MovementHandler::dismount: local state cleared at once,
//     CMSG_CANCEL_MOUNT_AURA.
//   - MovementHandler::handleForceSpeedChange (+ buildForceAck): SMSG_FORCE_
//     RUN / RUN_BACK / WALK / SWIM / SWIM_BACK / FLIGHT / FLIGHT_BACK speed and
//     TURN / PITCH rate changes are applied and ACKed with our movement block.
//   - makeSplineSpeedHandler (SMSG_SPLINE_SET_RUN/RUN_BACK/SWIM_SPEED) and
//     handleMoveSetSpeed (MSG_MOVE_SET_*_SPEED): the own speeds.
//   - EntityController: the speeds of the own create / movement block
//     (MovementHandler::applyServerMovementSpeeds).
// PSP additions: other players' run speed (the same packets and their create
// blocks) for WorldSession::predictRemote - a mounted player is predicted at
// their mounted speed; upstream interpolates and does not need it.
// Not ported: taxi flights (psp_world_session_taxi.cpp), the Classic CMSG_CANCEL_AURA
// fallback, SMSG_DISMOUNT (the field says the same on a 3.3.5a server).
#include "platform/psp/psp_world_session.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_spell_db.hpp"

#include <pspkernel.h>

#include <cmath>

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kAuraMounted = 78;  // SPELL_AURA_MOUNTED
constexpr float kDismountGraceSeconds = 1.5f;  // upstream MovementHandler::dismount

bool validSpeed(float v) { return std::isfinite(v) && v > 0.01f && v < 200.0f; }
}  // namespace

float WorldSession::unitRunSpeed(uint64_t guid) const {
    auto it = unitRunSpeed_.find(guid);
    return it == unitRunSpeed_.end() ? 7.0f : it->second;
}

// PSP: npc-walk
float WorldSession::unitWalkSpeed(uint64_t guid) const {
    auto it = unitWalkSpeed_.find(guid);
    return it == unitWalkSpeed_.end() ? 2.5f : it->second;
}

void WorldSession::updateMount(float dtSec) {
    if (dismountGraceRemaining_ > 0.0f) dismountGraceRemaining_ -= dtSec;
}

void WorldSession::dismount() {
    if (state_ != State::InWorld || !socket_) return;
    if (taxiMountActive_ || onTaxiFlight_) return;  // the flight's mount is not ours to dismiss (upstream)
    dismountGraceRemaining_ = kDismountGraceSeconds;
    if (currentMountDisplayId_ != 0) {
        PSP_LOGI(tag::Net, "mount_dismount display=%u aura=%u", static_cast<unsigned>(currentMountDisplayId_),
                 static_cast<unsigned>(mountAuraSpellId_));
        currentMountDisplayId_ = 0;
        mountAuraSpellId_ = 0;
    }
    socket_->send(network::Packet(game::wireOpcode(game::Opcode::CMSG_CANCEL_MOUNT_AURA)));
}

// First for every update block (before the combat / inventory hooks take the
// own one): the own mount field and speeds; other players' speeds.
void WorldSession::mountUpdateBlock(const game::UpdateBlock& b) {
    const bool self = playerGuid_ != 0 && b.guid == playerGuid_;
    if (b.hasMovement && validSpeed(b.runSpeed)) {
        if (self) {
            // applyServerMovementSpeeds
            auto apply = [](float v, float& dst) {
                if (validSpeed(v)) dst = v;
            };
            apply(b.walkSpeed, serverWalkSpeed_);
            apply(b.runSpeed, serverRunSpeed_);
            apply(b.runBackSpeed, serverRunBackSpeed_);
            apply(b.swimSpeed, serverSwimSpeed_);
            apply(b.swimBackSpeed, serverSwimBackSpeed_);
            apply(b.flightSpeed, serverFlightSpeed_);
            apply(b.flightBackSpeed, serverFlightBackSpeed_);
            apply(b.turnRate, serverTurnRate_);
            apply(b.pitchRate, serverPitchRate_);
            if (++mountLogs_ <= 20)
                PSP_LOGI(tag::Net, "speeds self walk=%.2f run=%.2f back=%.2f", static_cast<double>(serverWalkSpeed_),
                         static_cast<double>(serverRunSpeed_), static_cast<double>(serverRunBackSpeed_));
        } else if (b.objectType == game::ObjectType::PLAYER) {
            unitRunSpeed_[b.guid] = b.runSpeed;
            if (validSpeed(b.walkSpeed)) unitWalkSpeed_[b.guid] = b.walkSpeed;  // PSP: npc-walk
        } else if (b.objectType == game::ObjectType::UNIT || entities_.hasEntity(b.guid)) {
            // PSP: npc-walk - a creature's walk / run speeds: the gait of its splines (psp_move_anim.hpp).
            unitRunSpeed_[b.guid] = b.runSpeed;
            if (validSpeed(b.walkSpeed)) unitWalkSpeed_[b.guid] = b.walkSpeed;
        }
    }
    if (!self) return;
    const uint16_t fMount = game::fieldIndex(game::UF::UNIT_FIELD_MOUNTDISPLAYID);
    auto it = b.fields.find(fMount);
    if (it == b.fields.end()) return;
    const uint32_t now = it->second;
    if (taxiOwnMountField(now, b)) return;  // the field cleared mid-flight (detectPlayerMountChange)
    // detectPlayerMountChange: a dismount we asked for has not reached the field yet.
    if (now != 0 && dismountGraceRemaining_ > 0.0f) return;
    if (now == 0) dismountGraceRemaining_ = 0.0f;
    const uint32_t old = currentMountDisplayId_;
    currentMountDisplayId_ = now;
    if (old == 0 && now != 0) {
        // The mount aura: the spell we just cast if it mounts, else an own aura that does.
        mountAuraSpellId_ = 0;
        auto mounts = [&](uint32_t id) {
            const SpellInfo* s = id ? spellInfo(id) : nullptr;
            if (!s) return false;
            for (uint16_t a : s->rec.aura)
                if (a == kAuraMounted) return true;
            return false;
        };
        if (mounts(requestedSpellId_)) mountAuraSpellId_ = requestedSpellId_;
        for (const auto& a : playerAuras_)
            if (!mountAuraSpellId_ && !a.isEmpty() && mounts(a.spellId)) mountAuraSpellId_ = a.spellId;
    }
    if (now != old)
        PSP_LOGI(tag::Net, "mount_field display=%u old=%u aura=%u run=%.2f", static_cast<unsigned>(now),
                 static_cast<unsigned>(old), static_cast<unsigned>(mountAuraSpellId_),
                 static_cast<double>(serverRunSpeed_));
}

// upstream handleForceSpeedChange (3.3.5a branch) + buildForceAck.
void WorldSession::handleForceSpeedChange(network::Packet& packet, const char* name, game::Opcode ack,
                                          float* storage) {
    const uint64_t guid = packet.readPackedGuid();
    const uint32_t counter = packet.readUInt32();
    const size_t remaining = packet.getRemainingSize();
    if (remaining >= 8) packet.readUInt32();
    else if (remaining >= 5) packet.readUInt8();
    const float speed = packet.readFloat();
    if (guid != playerGuid_) {
        if (storage == &serverRunSpeed_ && validSpeed(speed)) unitRunSpeed_[guid] = speed;  // PSP: prediction
        return;
    }
    if (std::isnan(speed) || speed < 0.1f || speed > 100.0f) {
        PSP_LOGW(tag::Net, "speed_bad %s=%.2f", name, static_cast<double>(speed));
        return;
    }
    if (socket_) {
        network::Packet p(game::wireOpcode(ack));
        p.writePackedGuid(playerGuid_);
        p.writeUInt32(counter);
        game::MovementInfo wire = lastOwnMove_;  // server coordinates already (WorldScreen::sendMove)
        wire.time = static_cast<uint32_t>(sceKernelGetSystemTimeWide() / 1000u);
        game::MovementPacket::writeMovementPayload(p, wire);
        p.writeFloat(speed);
        socket_->send(p);
    }
    if (storage) *storage = speed;
    if (++mountLogs_ <= 40)
        PSP_LOGI(tag::Net, "speed_force %s=%.2f counter=%u", name, static_cast<double>(speed),
                 static_cast<unsigned>(counter));
}

// upstream makeSplineSpeedHandler: packed guid, float.
void WorldSession::handleSplineSpeed(network::Packet& packet, game::LogicalOpcode op) {
    using game::LogicalOpcode;
    if (!packet.hasRemaining(5)) return;
    const uint64_t guid = packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    const float speed = packet.readFloat();
    if (!validSpeed(speed)) return;
    if (guid != playerGuid_) {
        if (op == LogicalOpcode::SMSG_SPLINE_SET_RUN_SPEED) unitRunSpeed_[guid] = speed;  // PSP: prediction
        return;
    }
    if (op == LogicalOpcode::SMSG_SPLINE_SET_RUN_SPEED) serverRunSpeed_ = speed;
    else if (op == LogicalOpcode::SMSG_SPLINE_SET_RUN_BACK_SPEED) serverRunBackSpeed_ = speed;
    else if (op == LogicalOpcode::SMSG_SPLINE_SET_SWIM_SPEED) serverSwimSpeed_ = speed;
}

// upstream handleMoveSetSpeed: packed guid, movement block, float (the last 4 bytes).
void WorldSession::handleMoveSetSpeed(network::Packet& packet, game::LogicalOpcode op) {
    using game::LogicalOpcode;
    const uint64_t guid = packet.readPackedGuid();
    const size_t remaining = packet.getRemainingSize();
    if (remaining < 4) return;
    if (remaining > 4) packet.setReadPos(packet.getSize() - 4);
    const float speed = packet.readFloat();
    if (!validSpeed(speed)) return;
    if (guid != playerGuid_) {
        if (op == LogicalOpcode::MSG_MOVE_SET_RUN_SPEED) unitRunSpeed_[guid] = speed;  // PSP: prediction
        if (op == LogicalOpcode::MSG_MOVE_SET_WALK_SPEED) unitWalkSpeed_[guid] = speed;  // PSP: npc-walk
        return;
    }
    switch (op) {
        case LogicalOpcode::MSG_MOVE_SET_RUN_SPEED: serverRunSpeed_ = speed; break;
        case LogicalOpcode::MSG_MOVE_SET_RUN_BACK_SPEED: serverRunBackSpeed_ = speed; break;
        case LogicalOpcode::MSG_MOVE_SET_WALK_SPEED: serverWalkSpeed_ = speed; break;
        case LogicalOpcode::MSG_MOVE_SET_SWIM_SPEED: serverSwimSpeed_ = speed; break;
        case LogicalOpcode::MSG_MOVE_SET_SWIM_BACK_SPEED: serverSwimBackSpeed_ = speed; break;
        case LogicalOpcode::MSG_MOVE_SET_FLIGHT_SPEED: serverFlightSpeed_ = speed; break;
        case LogicalOpcode::MSG_MOVE_SET_FLIGHT_BACK_SPEED: serverFlightBackSpeed_ = speed; break;
        default: break;
    }
}

bool WorldSession::handleMountPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    using game::Opcode;
    if (state_ != State::InWorld) return false;
    switch (op) {
        case LogicalOpcode::SMSG_FORCE_RUN_SPEED_CHANGE:
            handleForceSpeedChange(packet, "run", Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK, &serverRunSpeed_);
            return true;
        case LogicalOpcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE:
            handleForceSpeedChange(packet, "run_back", Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK,
                                   &serverRunBackSpeed_);
            return true;
        case LogicalOpcode::SMSG_FORCE_WALK_SPEED_CHANGE:
            handleForceSpeedChange(packet, "walk", Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK, &serverWalkSpeed_);
            return true;
        case LogicalOpcode::SMSG_FORCE_SWIM_SPEED_CHANGE:
            handleForceSpeedChange(packet, "swim", Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK, &serverSwimSpeed_);
            return true;
        case LogicalOpcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE:
            handleForceSpeedChange(packet, "swim_back", Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK,
                                   &serverSwimBackSpeed_);
            return true;
        case LogicalOpcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE:
            handleForceSpeedChange(packet, "flight", Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK,
                                   &serverFlightSpeed_);
            return true;
        case LogicalOpcode::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE:
            handleForceSpeedChange(packet, "flight_back", Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK,
                                   &serverFlightBackSpeed_);
            return true;
        case LogicalOpcode::SMSG_FORCE_TURN_RATE_CHANGE:
            handleForceSpeedChange(packet, "turn", Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK, &serverTurnRate_);
            return true;
        case LogicalOpcode::SMSG_FORCE_PITCH_RATE_CHANGE:
            handleForceSpeedChange(packet, "pitch", Opcode::CMSG_FORCE_PITCH_RATE_CHANGE_ACK, &serverPitchRate_);
            return true;
        case LogicalOpcode::SMSG_SPLINE_SET_RUN_SPEED:
        case LogicalOpcode::SMSG_SPLINE_SET_RUN_BACK_SPEED:
        case LogicalOpcode::SMSG_SPLINE_SET_SWIM_SPEED:
            handleSplineSpeed(packet, op);
            return true;
        case LogicalOpcode::MSG_MOVE_SET_RUN_SPEED:
        case LogicalOpcode::MSG_MOVE_SET_RUN_BACK_SPEED:
        case LogicalOpcode::MSG_MOVE_SET_WALK_SPEED:
        case LogicalOpcode::MSG_MOVE_SET_SWIM_SPEED:
        case LogicalOpcode::MSG_MOVE_SET_SWIM_BACK_SPEED:
        case LogicalOpcode::MSG_MOVE_SET_FLIGHT_SPEED:
        case LogicalOpcode::MSG_MOVE_SET_FLIGHT_BACK_SPEED:
            handleMoveSetSpeed(packet, op);
            return true;
        default:
            return false;
    }
}

} // namespace wowee::platform::psp
