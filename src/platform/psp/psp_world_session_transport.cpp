// the session's half of the transports - what the
// 3.3.5a client keeps of them:
//   - the GAMEOBJECT_TYPE_TRANSPORT (11) / _MO_TRANSPORT (15) objects: their create block's place and the movement
//     block's UPDATEFLAG_TRANSPORT path time minus the clock then (kept from the create
//     block on), GAMEOBJECT_LEVEL (a ship's period, a lift's pause), GAMEOBJECT_PARENTROTATION
//     (a lift's path rotation), the state (a lift with two rests);
//   - an MO_TRANSPORT's template: CMSG_GAMEOBJECT_QUERY, data0 taxiPathId, data1 moveSpeed, data2 accelRate,
//     data5 transportPhysics (read when the object is set up);
//   - the riders: MOVEMENTFLAG_ONTRANSPORT (0x200) in a unit's / player's movement block - the transport's guid and
//     the place in its frame (MovementInfo as the client writes it: guid, x y z o, time, seat, time2 with
//     MOVEMENTFLAG2_INTERPOLATED_MOVEMENT) - and SMSG_MONSTER_MOVE_TRANSPORT (an NPC walking on a deck);
//   - ours: the server's word (our create block on a transport, SMSG_NEW_WORLD's place while riding) for the world
//     screen, which owns the riding itself (ui_world_transports.cpp).
// Upstream's TransportManager guesses paths and speeds (pickFallbackMovingPath, a speed solved from the period);
// the PSP follows the client's own rules instead (psp_transport_path.cpp).
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_transport_parse.hpp"

#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::platform::psp {

namespace {
// 3.3.5a GAMEOBJECT_PARENTROTATION = OBJECT_END (6) + 4: four floats (the UF table has no entry for it).
constexpr uint16_t kParentRotation = 10;
constexpr uint8_t kTypeTransport = 11, kTypeMoTransport = 15;
}  // namespace

uint32_t WorldSession::transportClockMs() { return static_cast<uint32_t>(sceKernelGetSystemTimeWide() / 1000u); }

const WorldSession::TransportTemplate* WorldSession::transportTemplate(uint32_t entry, uint64_t guid) {
    auto it = transportTemplates_.find(entry);
    if (it != transportTemplates_.end() && it->second.known) return &it->second;
    if (entry && socket_ && state_ == State::InWorld && transportQueried_.insert(entry).second) {
        socket_->send(game::GameObjectQueryPacket::build(entry, guid));
        PSP_LOGI(tag::Net, "transport_query entry=%u", static_cast<unsigned>(entry));
    }
    return nullptr;
}

void WorldSession::setOwnTransport(uint64_t guid, const glm::vec3& local, float localO) {
    ownTransportGuid_ = guid;
    ownTransportLocal_ = local;
    ownTransportO_ = localO;
}

bool WorldSession::takeOwnTransportPlace(uint64_t& guid, glm::vec3& local, float& localO) {
    if (!ownPlacePending_) return false;
    ownPlacePending_ = false;
    guid = ownPlaceGuid_;
    local = ownPlaceLocal_;
    localO = ownPlaceO_;
    return true;
}

void WorldSession::transportUpdateBlock(const game::UpdateBlock& b) {
    using game::UpdateType;
    const bool create = b.updateType == UpdateType::CREATE_OBJECT || b.updateType == UpdateType::CREATE_OBJECT2;
    // Riders: a unit / player whose movement block says ONTRANSPORT (upstream's parser read the guid and offset).
    // (A MOVEMENT block carries no object type: a unit / player is one the entities hold, or us.)
    const bool unitLike = b.objectType == game::ObjectType::UNIT || b.objectType == game::ObjectType::PLAYER ||
                          (b.updateType == UpdateType::MOVEMENT && (b.guid == playerGuid_ || entities_.hasEntity(b.guid)));
    if (unitLike && b.hasMovement && (create || b.updateType == UpdateType::MOVEMENT)) {
        const glm::vec3 local(b.transportX, b.transportY, b.transportZ);
        if (b.guid == playerGuid_) {
            if (create && b.onTransport && b.transportGuid) {
                ownPlacePending_ = true;
                ownPlaceGuid_ = b.transportGuid;
                ownPlaceLocal_ = local;
                ownPlaceO_ = cleanTransportAngle(b.transportO);
                PSP_LOGI(tag::Net, "transport_own_place guid=%08X x=%.2f y=%.2f z=%.2f o=%.2f (create block)",
                         static_cast<unsigned>(b.transportGuid & 0xFFFFFFFFu), local.x, local.y, local.z, b.transportO);
            }
            return;
        }
        if (b.onTransport && b.transportGuid) {
            TransportRider& r = transportRiders_[b.guid];
            r.transport = b.transportGuid;
            r.local = r.from = local;
            r.localO = cleanTransportAngle(b.transportO);
            r.moveMs = 0;
            r.spline = false;
            r.moveFlags = b.moveFlags;
            if (++riderLogs_ <= 30)
                PSP_LOGI(tag::Net, "transport_rider guid=%08X on=%08X x=%.2f y=%.2f z=%.2f block", static_cast<unsigned>(b.guid),
                         static_cast<unsigned>(b.transportGuid & 0xFFFFFFFFu), local.x, local.y, local.z);
        } else {
            transportRiders_.erase(b.guid);
        }
        return;
    }
    auto field = [&](uint16_t idx, uint32_t& out) {
        auto fv = b.fields.find(idx);
        if (fv == b.fields.end()) return false;
        out = fv->second;
        return true;
    };
    auto applyFields = [&](TransportObject& t) {
        uint32_t v = 0;
        if (field(game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY), v)) t.entry = v;
        if (field(game::fieldIndex(game::UF::GAMEOBJECT_DISPLAYID), v)) t.displayId = v;
        if (field(game::fieldIndex(game::UF::GAMEOBJECT_LEVEL), v)) t.level = v;
        if (field(game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X), v)) {
            float f = 1.0f;
            std::memcpy(&f, &v, sizeof(f));
            t.scale = f > 0.01f && f < 100.0f ? f : 1.0f;
        }
        for (int i = 0; i < 4; ++i)
            if (field(static_cast<uint16_t>(kParentRotation + i), v)) {
                float f = 0.0f;
                std::memcpy(&f, &v, sizeof(f));
                t.parentRot[i] = std::isfinite(f) ? f : (i == 3 ? 1.0f : 0.0f);
            }
        if (field(game::fieldIndex(game::UF::GAMEOBJECT_BYTES_1), v)) {
            const uint8_t state = static_cast<uint8_t>(v & 0xFFu);
            t.type = static_cast<uint8_t>((v >> 8) & 0xFFu);
            if (state != t.state) {
                t.prevState = t.state;
                t.state = state;
                ++t.stateSerial;
                t.stateChangeMs = transportClockMs();
            }
        }
    };
    if (create) {
        if (b.objectType != game::ObjectType::GAMEOBJECT || !b.hasMovement) return;
        uint32_t bytes1 = 0;
        const bool haveType = field(game::fieldIndex(game::UF::GAMEOBJECT_BYTES_1), bytes1);
        const uint8_t type = static_cast<uint8_t>((bytes1 >> 8) & 0xFFu);
        if (!(haveType && (type == kTypeTransport || type == kTypeMoTransport)) && !b.hasGoTransportTime) return;
        TransportObject& t = transportObjects_[b.guid];
        t = TransportObject{};
        t.guid = b.guid;
        t.pos = glm::vec3(b.x, b.y, b.z);
        t.orientation = cleanTransportAngle(b.orientation);  // finite, wrapped
        applyFields(t);
        t.state = static_cast<uint8_t>(bytes1 & 0xFFu);
        t.prevState = t.state;
        t.stateSerial = 0;
        if (b.hasGoTransportTime) {
            // The u32 is the path progress (ms) when the server built the block, and the server advances it by the
            // elapsed ms (AzerothCore Object::BuildMovementUpdate / MotionTransport::Update). Port rule: continue it
            // with our own clock from the moment the block arrived - path time = transportClockMs() + this offset
            // (u32 wrap-around arithmetic; the network delay is not corrected).
            t.timeOffsetMs = b.goTransportTime - transportClockMs();
            t.hasTime = true;
        }
        ++transportVersion_;
        if (++transportLogs_ <= 40)
            PSP_LOGI(tag::Net, "transport_seen guid=%08X%08X entry=%u display=%u type=%u level=%u state=%u time=%u "
                     "x=%.1f y=%.1f z=%.1f o=%.2f rot=%.2f,%.2f,%.2f,%.2f",
                     static_cast<unsigned>(b.guid >> 32), static_cast<unsigned>(b.guid & 0xFFFFFFFFu),
                     static_cast<unsigned>(t.entry), static_cast<unsigned>(t.displayId), static_cast<unsigned>(t.type),
                     static_cast<unsigned>(t.level), static_cast<unsigned>(t.state),
                     static_cast<unsigned>(b.goTransportTime), b.x, b.y, b.z, b.orientation, t.parentRot.x,
                     t.parentRot.y, t.parentRot.z, t.parentRot.w);
        if (t.type == kTypeMoTransport) transportTemplate(t.entry, t.guid);
        return;
    }
    auto it = transportObjects_.find(b.guid);
    if (it == transportObjects_.end()) return;
    if (b.updateType == UpdateType::VALUES) {
        const uint32_t serial = it->second.stateSerial;
        applyFields(it->second);
        ++transportVersion_;
        if (it->second.stateSerial != serial && ++transportLogs_ <= 80)
            PSP_LOGI(tag::Net, "transport_state guid=%08X entry=%u %u->%u level=%u", static_cast<unsigned>(b.guid),
                     static_cast<unsigned>(it->second.entry), static_cast<unsigned>(it->second.prevState),
                     static_cast<unsigned>(it->second.state), static_cast<unsigned>(it->second.level));
    } else if (b.updateType == UpdateType::MOVEMENT && b.hasMovement) {
        it->second.pos = glm::vec3(b.x, b.y, b.z);
        it->second.orientation = cleanTransportAngle(b.orientation);
        ++transportVersion_;
    }
}

void WorldSession::transportForget(uint64_t guid) {
    if (transportObjects_.erase(guid)) ++transportVersion_;
    transportRiders_.erase(guid);
}

void WorldSession::transportResetWorld() {
    transportObjects_.clear();
    transportRiders_.clear();
    ++transportVersion_;
}

// MovementInfo's ONTRANSPORT block, as the client writes it and upstream's writeMovementPayload sends it.
void WorldSession::transportReadMoveBlock(network::Packet& packet, game::MovementInfo& info) {
    readTransportMoveBlock(packet, info);  // psp_transport_parse.hpp (host-tested)
}

namespace {
glm::vec3 riderLocalNow(const WorldSession::TransportRider& r, uint32_t now) {
    if (!r.moveMs) return r.local;
    const float f = std::clamp(static_cast<float>(now - r.moveStartMs) / static_cast<float>(r.moveMs), 0.0f, 1.0f);
    return r.from + (r.local - r.from) * f;
}
}  // namespace

void WorldSession::transportOtherMove(uint64_t guid, const game::MovementInfo& info, uint32_t durationMs) {
    if (!(info.flags & static_cast<uint32_t>(game::MovementFlags::ONTRANSPORT)) || !info.transportGuid) {
        transportRiders_.erase(guid);
        return;
    }
    const uint32_t now = transportClockMs();
    const glm::vec3 local(info.transportX, info.transportY, info.transportZ);
    auto it = transportRiders_.find(guid);
    const bool same = it != transportRiders_.end() && it->second.transport == info.transportGuid;
    TransportRider& r = transportRiders_[guid];
    r.from = same ? riderLocalNow(r, now) : local;
    r.transport = info.transportGuid;
    r.local = local;
    r.localO = cleanTransportAngle(info.transportO);
    r.moveStartMs = now;
    r.moveMs = durationMs;
    r.moveFlags = info.flags;
    r.spline = false;
    if (!same && ++riderLogs_ <= 30)
        PSP_LOGI(tag::Net, "transport_rider guid=%08X on=%08X x=%.2f y=%.2f z=%.2f move", static_cast<unsigned>(guid),
                 static_cast<unsigned>(info.transportGuid & 0xFFFFFFFFu), local.x, local.y, local.z);
}

// SMSG_MONSTER_MOVE_TRANSPORT (TrinityCore / AzerothCore MoveSplineInit::Launch on a transport): the mover's packed
// guid, the transport's packed guid, the seat (int8), then SMSG_MONSTER_MOVE's body in the transport's frame.
// PSP: the walk is eased straight to the spline's end over its duration (the waypoints are not followed).
void WorldSession::handleMonsterMoveTransport(network::Packet& packet) {
    // both guids whole (psp_transport_parse.hpp, host-tested; was: one byte checked each).
    uint64_t mover = 0, transport = 0;
    int8_t seat = -1;
    if (!readMonsterMoveTransportHead(packet, mover, transport, seat)) return;
    // a mover we hold no entity of makes no rider (upstream handleMonsterMoveTransport: no
    // entity -> return) - nothing would draw it or forget it (removeEntity -> transportForget never comes).
    if (mover == 0 || mover == playerGuid_ || transport == 0 || !entities_.hasEntity(mover)) return;
    network::Packet body(game::wireOpcode(game::LogicalOpcode::SMSG_MONSTER_MOVE));
    body.writePackedGuid(mover);
    const auto& raw = packet.getData();
    if (packet.getReadPos() < raw.size())
        body.writeBytes(raw.data() + packet.getReadPos(), raw.size() - packet.getReadPos());
    game::MonsterMoveData d;
    if (!game::MonsterMoveParser::parse(body, d)) return;
    noteSplineGait(d);  // PSP: npc-walk - a deck walker walks too (its spline in the transport's frame)
    const uint32_t now = transportClockMs();
    auto it = transportRiders_.find(mover);
    const bool same = it != transportRiders_.end() && it->second.transport == transport;
    TransportRider& r = transportRiders_[mover];
    const glm::vec3 start(d.x, d.y, d.z);
    r.from = same ? riderLocalNow(r, now) : start;
    r.transport = transport;
    r.moveStartMs = now;
    if (d.hasDest) {
        r.local = glm::vec3(d.destX, d.destY, d.destZ);
        r.moveMs = d.duration;
        r.spline = d.duration > 0;
        if (d.moveType == 4) {
            r.localO = cleanTransportAngle(d.facingAngle);
        } else {
            const glm::vec3 dv = r.local - r.from;
            if (std::fabs(dv.x) > 0.01f || std::fabs(dv.y) > 0.01f)
                r.localO = cleanTransportAngle(std::atan2(dv.y, dv.x));
        }
    } else {
        r.local = r.from = start;
        r.moveMs = 0;
        r.spline = false;
        if (d.moveType == 4) r.localO = cleanTransportAngle(d.facingAngle);
    }
    if (++riderLogs_ <= 30)
        PSP_LOGI(tag::Net, "monster_move_transport guid=%08X on=%08X type=%u ms=%u x=%.2f y=%.2f z=%.2f",
                 static_cast<unsigned>(mover), static_cast<unsigned>(transport & 0xFFFFFFFFu),
                 static_cast<unsigned>(d.moveType), static_cast<unsigned>(d.duration), r.local.x, r.local.y, r.local.z);
}

// SMSG_NEW_WORLD while riding a transport to another map (SMSG_TRANSFER_PENDING named a transport): the place is
// the place on the transport (upstream beginPlayerTransportWorldTransfer). A MSG_MOVE_STOP without ONTRANSPORT there
// (map-transfer's arrival, upstream's sendMovement after the ACK) would take us off the ship on the server
// (AzerothCore HandleMovementOpcodes: a passenger whose packet lacks the flag is removed) - the world screen tells
// the server with its first movement packet on the transport instead.
bool WorldSession::transportArrival(const glm::vec3& pos, float orientation) {
    if (!transferOnTransport() || ownTransportGuid_ == 0) {
        ownTransportGuid_ = 0;
        return false;
    }
    ownPlacePending_ = true;
    ownPlaceGuid_ = ownTransportGuid_;
    ownPlaceLocal_ = pos;
    ownPlaceO_ = cleanTransportAngle(orientation);
    PSP_LOGI(tag::Net, "transport_own_place guid=%08X x=%.2f y=%.2f z=%.2f o=%.2f (new world)",
             static_cast<unsigned>(ownTransportGuid_ & 0xFFFFFFFFu), pos.x, pos.y, pos.z, orientation);
    return true;
}

bool WorldSession::handleTransportPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_MONSTER_MOVE_TRANSPORT:
            if (state_ == State::InWorld) handleMonsterMoveTransport(packet);
            return true;
        case LogicalOpcode::SMSG_GAMEOBJECT_QUERY_RESPONSE: {
            // A peek (other readers of the answer - the objects' names - see it too).
            network::Packet copy = packet;
            game::GameObjectQueryResponseData d;
            if (!game::GameObjectQueryResponseParser::parse(copy, d) || !d.entry || !transportQueried_.count(d.entry))
                return false;
            TransportTemplate& t = transportTemplates_[d.entry];
            t.type = d.type;
            t.taxiPathId = d.data[0];
            t.moveSpeed = d.data[1];
            t.accelRate = d.data[2];
            t.physics = d.data[5];
            t.known = d.hasData;
            ++transportVersion_;
            PSP_LOGI(tag::Net, "transport_template entry=%u type=%u path=%u speed=%u accel=%u physics=%u known=%d",
                     static_cast<unsigned>(d.entry), static_cast<unsigned>(t.type), static_cast<unsigned>(t.taxiPathId),
                     static_cast<unsigned>(t.moveSpeed), static_cast<unsigned>(t.accelRate),
                     static_cast<unsigned>(t.physics), t.known ? 1 : 0);
            return false;
        }
        default:
            return false;
    }
}

}  // namespace wowee::platform::psp
