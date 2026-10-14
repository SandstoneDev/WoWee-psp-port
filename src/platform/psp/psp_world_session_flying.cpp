// The session half of flight and swimming.
//
// The server's movement-flag toggles for our mover (upstream MovementHandler::handleForceMoveFlagChange's table):
//   SMSG_MOVE_SET_CAN_FLY / _UNSET_CAN_FLY -> CMSG_MOVE_SET_CAN_FLY_ACK, WATER_WALK / LAND_WALK -> CMSG_MOVE_WATER_WALK_ACK,
//   FEATHER_FALL / NORMAL_FALL -> CMSG_MOVE_FEATHER_FALL_ACK, SET_HOVER / UNSET_HOVER -> CMSG_MOVE_HOVER_ACK,
//   GRAVITY_DISABLE / _ENABLE -> CMSG_MOVE_GRAVITY_DISABLE_ACK / _ENABLE_ACK,
//   SET / UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY -> its ACK (no flag changes).
// As upstream, the flag alone is set / cleared and the ACK carries the movement block with it; AzerothCore's
// HandleMoveFlagChangeOpcode reads the guid, the counter, the block and (not for the gravity ACKs) a u32 isApplied.
// PSP: the ACK is sent at once from the last movement block sent (lastOwnMove_, like the speed ACKs of the mount code);
// the world screen sees the new flags next frame (ownGrantedMoveFlags) - a flier whose CAN_FLY went falls there.
// Also: SMSG_SPLINE_SET_FLIGHT_SPEED / _FLIGHT_BACK / _SWIM_BACK / _WALK_SPEED (upstream makeSplineSpeedHandler),
// other players' flight and swim speeds (their FORCE / MSG_MOVE_SET / spline speed packets and create blocks: peeked,
// they go on to the mount code's handlers), other players' climb and flag packets (MSG_MOVE_START_ASCEND, _STOP_ASCEND,
// _START_DESCEND, _UPDATE_CAN_FLY, _GRAVITY_CHNG, _UPDATE_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY - upstream's MSG_MOVE_*
// relay list) and the prediction of a flier / swimmer between its packets (own rule, flightPredict).
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_flight_rules.hpp"
#include "platform/psp/psp_log.hpp"

#include <pspkernel.h>

#include <cmath>

namespace wowee::platform::psp {

namespace {
bool validFlightSpeed(float v) { return std::isfinite(v) && v > 0.01f && v < 200.0f; }
}  // namespace

// The server's toggle for our mover: packed guid, u32 counter (upstream handleForceMoveFlagChange).
void WorldSession::moveToggle(network::Packet& packet, int toggle, bool on, game::Opcode ack, const char* name) {
    if (!packet.hasFullPackedGuid()) return;
    const uint64_t guid = packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    const uint32_t counter = packet.readUInt32();
    if (guid != playerGuid_) return;  // the server sends these to the mover only
    const auto t = static_cast<flight::Toggle>(toggle);
    game::MovementInfo mi = lastOwnMove_;  // server coordinates (WorldScreen::sendMove)
    flight::applyToggle(mi, t, on);
    const uint32_t flag = flight::toggleFlag(t);
    ownGranted_ = on ? (ownGranted_ | flag) : (ownGranted_ & ~flag);
    ++moveToggleSeq_;
    if (socket_) {
        mi.time = static_cast<uint32_t>(sceKernelGetSystemTimeWide() / 1000u);
        socket_->send(flight::buildMoveAck(game::wireOpcode(ack), playerGuid_, counter, mi, flight::ackHasApplied(t), on));
        lastOwnMove_ = mi;  // the next ACK starts from what the server now knows
    }
    if (++flyLogs_ <= 60)
        PSP_LOGI(tag::Net, "move_toggle %s=%d counter=%u flags=%08X granted=%08X", name, on ? 1 : 0,
                 static_cast<unsigned>(counter), static_cast<unsigned>(mi.flags), static_cast<unsigned>(ownGranted_));
}

// upstream makeSplineSpeedHandler (packed guid, float) for the speeds the mount code's handler leaves out.
void WorldSession::flightSplineSpeed(network::Packet& packet, game::LogicalOpcode op) {
    using game::LogicalOpcode;
    if (!packet.hasFullPackedGuid()) return;
    const uint64_t guid = packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    const float speed = packet.readFloat();
    if (!validFlightSpeed(speed)) return;
    if (guid != playerGuid_) {
        if (op == LogicalOpcode::SMSG_SPLINE_SET_FLIGHT_SPEED) unitFlightSpeed_[guid] = speed;  // PSP: prediction
        if (op == LogicalOpcode::SMSG_SPLINE_SET_WALK_SPEED) unitWalkSpeed_[guid] = speed;  // PSP: npc-walk
        return;
    }
    switch (op) {
        case LogicalOpcode::SMSG_SPLINE_SET_FLIGHT_SPEED: serverFlightSpeed_ = speed; break;
        case LogicalOpcode::SMSG_SPLINE_SET_FLIGHT_BACK_SPEED: serverFlightBackSpeed_ = speed; break;
        case LogicalOpcode::SMSG_SPLINE_SET_SWIM_BACK_SPEED: serverSwimBackSpeed_ = speed; break;
        case LogicalOpcode::SMSG_SPLINE_SET_WALK_SPEED: serverWalkSpeed_ = speed; break;
        default: break;
    }
}

// Another player's flight / swim speed out of a speed packet that goes on to the mount code's handler (read position
// restored): SMSG_FORCE_*_SPEED_CHANGE (packed guid, u32 counter, [u8], float - the last 4 bytes), MSG_MOVE_SET_*_SPEED
// and SMSG_SPLINE_SET_SWIM_SPEED (the float last).
void WorldSession::peekFlightSpeed(network::Packet& packet, game::LogicalOpcode op) {
    using game::LogicalOpcode;
    const size_t at = packet.getReadPos();
    if (packet.hasFullPackedGuid()) {
        const uint64_t guid = packet.readPackedGuid();
        if (guid != playerGuid_ && guid != 0 && packet.getRemainingSize() >= 4) {
            packet.setReadPos(packet.getSize() - 4);
            const float speed = packet.readFloat();
            if (validFlightSpeed(speed)) {
                if (op == LogicalOpcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE || op == LogicalOpcode::MSG_MOVE_SET_FLIGHT_SPEED)
                    unitFlightSpeed_[guid] = speed;
                else
                    unitSwimSpeed_[guid] = speed;
            }
        }
    }
    packet.setReadPos(at);
}

bool WorldSession::handleFlyingPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    using game::Opcode;
    using T = flight::Toggle;
    if (state_ != State::InWorld) return false;
    switch (op) {
        case LogicalOpcode::SMSG_MOVE_SET_CAN_FLY:
            moveToggle(packet, static_cast<int>(T::CanFly), true, Opcode::CMSG_MOVE_SET_CAN_FLY_ACK, "can_fly");
            return true;
        case LogicalOpcode::SMSG_MOVE_UNSET_CAN_FLY:
            moveToggle(packet, static_cast<int>(T::CanFly), false, Opcode::CMSG_MOVE_SET_CAN_FLY_ACK, "can_fly");
            return true;
        case LogicalOpcode::SMSG_MOVE_WATER_WALK:
            moveToggle(packet, static_cast<int>(T::WaterWalk), true, Opcode::CMSG_MOVE_WATER_WALK_ACK, "water_walk");
            return true;
        case LogicalOpcode::SMSG_MOVE_LAND_WALK:
            moveToggle(packet, static_cast<int>(T::WaterWalk), false, Opcode::CMSG_MOVE_WATER_WALK_ACK, "water_walk");
            return true;
        case LogicalOpcode::SMSG_MOVE_FEATHER_FALL:
            moveToggle(packet, static_cast<int>(T::FeatherFall), true, Opcode::CMSG_MOVE_FEATHER_FALL_ACK, "feather_fall");
            return true;
        case LogicalOpcode::SMSG_MOVE_NORMAL_FALL:
            moveToggle(packet, static_cast<int>(T::FeatherFall), false, Opcode::CMSG_MOVE_FEATHER_FALL_ACK, "feather_fall");
            return true;
        case LogicalOpcode::SMSG_MOVE_SET_HOVER:
            moveToggle(packet, static_cast<int>(T::Hover), true, Opcode::CMSG_MOVE_HOVER_ACK, "hover");
            return true;
        case LogicalOpcode::SMSG_MOVE_UNSET_HOVER:
            moveToggle(packet, static_cast<int>(T::Hover), false, Opcode::CMSG_MOVE_HOVER_ACK, "hover");
            return true;
        case LogicalOpcode::SMSG_MOVE_GRAVITY_DISABLE:
            moveToggle(packet, static_cast<int>(T::GravityOff), true, Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK, "gravity_off");
            return true;
        case LogicalOpcode::SMSG_MOVE_GRAVITY_ENABLE:
            moveToggle(packet, static_cast<int>(T::GravityOff), false, Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK, "gravity_off");
            return true;
        case LogicalOpcode::SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY:
            moveToggle(packet, static_cast<int>(T::CanSwimToFly), true,
                       Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, "swim_to_fly");
            return true;
        case LogicalOpcode::SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY:
            moveToggle(packet, static_cast<int>(T::CanSwimToFly), false,
                       Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, "swim_to_fly");
            return true;
        case LogicalOpcode::SMSG_SPLINE_SET_FLIGHT_SPEED:
        case LogicalOpcode::SMSG_SPLINE_SET_FLIGHT_BACK_SPEED:
        case LogicalOpcode::SMSG_SPLINE_SET_SWIM_BACK_SPEED:
        case LogicalOpcode::SMSG_SPLINE_SET_WALK_SPEED:
            flightSplineSpeed(packet, op);
            return true;
        case LogicalOpcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE:
        case LogicalOpcode::SMSG_FORCE_SWIM_SPEED_CHANGE:
        case LogicalOpcode::MSG_MOVE_SET_FLIGHT_SPEED:
        case LogicalOpcode::MSG_MOVE_SET_SWIM_SPEED:
        case LogicalOpcode::SMSG_SPLINE_SET_SWIM_SPEED:
            peekFlightSpeed(packet, op);
            return false;  // goes on (the mount code: the own speeds, the ACK)
        case LogicalOpcode::MSG_MOVE_START_ASCEND:
        case LogicalOpcode::MSG_MOVE_STOP_ASCEND:
        case LogicalOpcode::MSG_MOVE_START_DESCEND:
        case LogicalOpcode::MSG_MOVE_UPDATE_CAN_FLY:
        case LogicalOpcode::MSG_MOVE_GRAVITY_CHNG:
        case LogicalOpcode::MSG_MOVE_UPDATE_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY:
            handleOtherMovement(packet, false);
            return true;
        default:
            return false;
    }
}

// First for every update block (with mountUpdateBlock): other players' flight / swim speeds; our own create block's
// movement flags - a login (or a transfer) in the air or in the water (TrinityCore / AzerothCore follow it with the
// SMSG_MOVE_SET_CAN_FLY of the aura, SendInitialPacketsAfterAddToMap).
void WorldSession::flyingUpdateBlock(const game::UpdateBlock& b) {
    if (!b.hasMovement || b.guid == 0) return;
    if (b.guid == playerGuid_) {
        const bool create =
            b.updateType == game::UpdateType::CREATE_OBJECT || b.updateType == game::UpdateType::CREATE_OBJECT2;
        if (!create) return;
        ownCreateFlags_ = b.moveFlags;
        ownGranted_ = (ownGranted_ & ~flight::kServerGranted) | (b.moveFlags & flight::kServerGranted);
        ++moveToggleSeq_;
        if (++flyLogs_ <= 60)
            PSP_LOGI(tag::Net, "move_self_create flags=%08X", static_cast<unsigned>(b.moveFlags));
        return;
    }
    if (b.objectType != game::ObjectType::PLAYER) return;
    if (validFlightSpeed(b.flightSpeed)) unitFlightSpeed_[b.guid] = b.flightSpeed;
    if (validFlightSpeed(b.swimSpeed)) unitSwimSpeed_[b.guid] = b.swimSpeed;
}

void WorldSession::flyingForget(uint64_t guid) {
    unitFlightSpeed_.erase(guid);
    unitSwimSpeed_.erase(guid);
}

void WorldSession::flyingResetWorld() {
    // A new world (SMSG_NEW_WORLD / a login): the server sends the toggles again for the new map
    // (SendInitialPacketsAfterAddToMap), the create block says the rest.
    ownGranted_ = 0;
    ownCreateFlags_ = 0;
    ++moveToggleSeq_;
}

uint32_t WorldSession::remoteMoveFlags(uint64_t guid) const {
    auto it = motion_.find(guid);
    if (it != motion_.end()) return it->second.info.flags;
    return unitMoveFlags(guid);
}

// predictRemote, a flier / swimmer. Own rule: it moves level along its facing at its flight / swim speed (the same
// direction rule as on foot: forward / back, a strafe square to the facing), its height stays at the packet's until the
// next packet (a heartbeat every half second) says otherwise. False for a unit on foot (the rest of predictRemote).
bool WorldSession::flightPredict(uint64_t guid, const game::MovementInfo& m, float dt, float moveYaw, glm::vec3& pos,
                                 bool& moving) const {
    if (!flight::swimOrFly(m.flags)) return false;
    flight::Speeds s;
    auto fl = unitFlightSpeed_.find(guid);
    auto sw = unitSwimSpeed_.find(guid);
    if (fl != unitFlightSpeed_.end()) s.flight = fl->second;
    if (sw != unitSwimSpeed_.end()) s.swim = sw->second;
    moving = (m.flags & (flight::kDirections | flight::kClimbs)) != 0;
    pos = glm::vec3(m.x, m.y, m.z);
    if (!(m.flags & flight::kDirections)) return true;
    // Level: the pitch is left out of the step.
    const flight::Vec3 d = flight::moveDirection(m.flags & ~(flight::kSwimming | flight::kFlying), moveYaw, 0.0f);
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1e-4f) return true;
    const float speed = flight::moveSpeed(m.flags, s);
    pos.x += d.x * (speed * dt / len);
    pos.y += d.y * (speed * dt / len);
    return true;
}

}  // namespace wowee::platform::psp
