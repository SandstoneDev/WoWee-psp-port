// creature anim - NPC movement for the PSP world session.
//
// Upstream MovementHandler (src/game/movement_handler.cpp) is tied to
// GameHandler; its creature steps are repeated here with the upstream parsers
// unchanged:
//   - SMSG_MONSTER_MOVE: MonsterMoveParser::parse (3.3.5a layout, the 3.3.5a
//     PacketParsers::parseMonsterMove) + handleMonsterMove's entity update:
//     facing (moveType 4 angle / 3 target / else the travel direction, never
//     backwards over half a turn), Entity::startMoveAlongPath (start +
//     waypoints + destination, Catmull-Rom) or startMoveTo for a single
//     point, moveType 1 = stop in place, 4 = turn in place;
//   - SMSG_COMPRESSED_MOVES: handleCompressedMoves (zlib wrapper, sub-packets
//     "u8 size, u16 opcode, payload" with the payload-only-size fallback);
//     MSG_MOVE_* inside go to handleOtherMovement like the plain opcodes;
//   - SMSG_SPLINE_MOVE_SET_WALK_MODE / RUN_MODE / START_SWIM / STOP_SWIM /
//     SET_FLYING / UNSET_FLYING: upstream's synthesised move flags for the
//     unitMoveFlagsCallback (walking / swimming / flying per unit), which the
//     creature animation reads (Walk vs Run, Swim, Fly).
// Classic / 2.4.3 / Turtle branches (per-tick cap, spline RUNMODE bit, vanilla
// layout) and transports (SMSG_MONSTER_MOVE_TRANSPORT) are not ported: the
// PSP talks to a 3.3.5a server and has no TransportManager yet.
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "platform/psp/psp_log.hpp"
#include "game/spline_packet.hpp"      // walk/run (SplineFlagWotlk)
#include "platform/psp/psp_config.hpp"  // walk/run (splinewalk)
#include "platform/psp/psp_move_anim.hpp"  // PSP: npc-walk

#include <zlib.h>

#include <array>
#include <cmath>

// GCC 15 (-O2, MIPS) reports a false -Wmaybe-uninitialized inside the
// upstream Entity::setPosition / startMoveAlongPath once inlined (the same
// pragma as around M10a in psp_world_session.cpp).
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

namespace wowee::platform::psp {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr uint32_t kFlag(game::MovementFlags f) { return static_cast<uint32_t>(f); }
// PSP: npc-walk - WorldSession::splineGait_ bits.
constexpr uint8_t kGaitWalk = 1, kGaitFly = 2, kGaitFlags = 4;  // kGaitFlags: Walk/Run from the move flags

} // namespace

uint32_t WorldSession::unitMoveFlags(uint64_t guid) const {
    auto it = unitMoveFlags_.find(guid);
    return it == unitMoveFlags_.end() ? 0u : it->second;
}

// Upstream unitMoveFlagsCallback: only the bits in `mask` change.
void WorldSession::setUnitMoveFlags(uint64_t guid, uint32_t flags, uint32_t mask) {
    if (guid == 0 || guid == playerGuid_) return;
    uint32_t& f = unitMoveFlags_[guid];
    f = (f & ~mask) | (flags & mask);
    if (f == 0) unitMoveFlags_.erase(guid);
}

// PSP: npc-walk - Walk or Run of a unit on a server spline. Upstream reads no gait from a 3.3.5a spline (only the
// pre-3.3.5a RUNMODE bit) and so runs every patrol; an earlier PSP version took MoveSplineFlag 0x1000 for Walk and wrote it into the
// unit's WALKING flag - but AzerothCore never sends that bit (its 0x1000 is CanSwim), so every spline cleared WALKING,
// even one from SMSG_SPLINE_MOVE_SET_WALK_MODE (hardware log: "walk=0 sf=0 spd=2.5" for every walker). Now the gait
// is kept apart from the move flags (psp_move_anim.hpp: the spline's speed against the unit's walk / run speeds,
// as the 3.3.5a client must do it). wowee.cfg:
//   npcwalkspeed=1  gait from the spline speed (0 = upstream: the WALKING move flag only)
//   splinewalk=1    a spline with flag 0x1000 (MaNGOS-family Walkmode) walks too (0 = ignore the bit)
// A spline with MoveSplineFlag::Flying (0x2000) flies (unitSplineFlying): AzerothCore sets it for a unit with
// CAN_FLY / DISABLE_GRAVITY, whose create block has no FLYING flag (upstream reads only that one).
void WorldSession::noteSplineGait(const game::MonsterMoveData& d) {
    static const int bySpeed = configGetInt("npcwalkspeed", 1);
    static const int splineWalk = configGetInt("splinewalk", 1);
    auto e = entities_.getEntity(d.guid);
    if (!e || e->getType() != game::ObjectType::UNIT) return;
    if (!d.hasDest) {
        splineGait_.erase(d.guid);  // stopped / turned: the move flags again
        return;
    }
    float len2 = 0.0f, len3 = 0.0f, px = d.x, py = d.y, pz = d.z;
    auto leg = [&](float x, float y, float z) {
        const float dx = x - px, dy = y - py, dz = z - pz;
        len2 += std::sqrt(dx * dx + dy * dy);
        len3 += std::sqrt(dx * dx + dy * dy + dz * dz);
        px = x;
        py = y;
        pz = z;
    };
    for (const auto& wp : d.waypoints) leg(wp.x, wp.y, wp.z);
    leg(d.destX, d.destY, d.destZ);
    // The ground speed sets the animation rate (npcanimspeed).
    if (d.duration > 0) unitSplineSpeed_[d.guid] = len2 * 1000.0f / static_cast<float>(d.duration);
    else unitSplineSpeed_.erase(d.guid);
    const float speed = splinePathSpeed(len3, d.duration);
    // MoveSplineFlag::Flying: the server flies it (a unit with CAN_FLY / DISABLE_GRAVITY; "flying animation").
    const uint8_t fly = (d.splineFlags & game::SplineFlagWotlk::FLYING) ? kGaitFly : 0u;
    if (splineWalk && (d.splineFlags & game::SplineFlagWotlk::WALKMODE))
        splineGait_[d.guid] = static_cast<uint8_t>(kGaitWalk | fly);
    else if (bySpeed && speed > 0.0f)
        splineGait_[d.guid] = static_cast<uint8_t>(
            (splineSpeedIsWalk(speed, unitWalkSpeed(d.guid), unitRunSpeed(d.guid)) ? kGaitWalk : 0u) | fly);
    else if (fly) splineGait_[d.guid] = static_cast<uint8_t>(kGaitFly | kGaitFlags);  // gait from the move flags
    else splineGait_.erase(d.guid);  // no speed (a turn): the move flags decide
}

bool WorldSession::unitWalkAnim(uint64_t guid) const {
    auto it = splineGait_.find(guid);
    if (it != splineGait_.end() && !(it->second & kGaitFlags)) return (it->second & kGaitWalk) != 0;
    return (unitMoveFlags(guid) & kFlag(game::MovementFlags::WALKING)) != 0;
}

// A server-moved creature flies with CAN_FLY alone (AzerothCore Unit::SetCanFly on a creature in the air: no FLYING
// in its create block) or on a Flying spline; one a client moves (MSG_MOVE_*: a vehicle, a charmed unit) only with
// both bits (CAN_FLY alone is leave to fly, as MovementInfo::isPlayerFlying). Upstream reads FLYING only.
bool WorldSession::unitFlyAnim(uint64_t guid) const {
    const uint32_t f = unitMoveFlags(guid);
    const uint32_t canFly = kFlag(game::MovementFlags::CAN_FLY), flying = kFlag(game::MovementFlags::FLYING);
    if (motion_.count(guid)) return (f & (canFly | flying)) == (canFly | flying);
    if (f & (canFly | flying)) return true;
    auto it = splineGait_.find(guid);
    return it != splineGait_.end() && (it->second & kGaitFly) != 0;
}

void WorldSession::handleNpcMove(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    using MF = game::MovementFlags;
    const uint32_t walk = kFlag(MF::WALKING), swim = kFlag(MF::SWIMMING);
    const uint32_t fly = kFlag(MF::CAN_FLY) | kFlag(MF::FLYING);
    auto synth = [&](uint32_t flags, uint32_t mask) {
        if (!packet.hasRemaining(1)) return;
        setUnitMoveFlags(packet.readPackedGuid(), flags, mask);
    };
    switch (op) {
        case LogicalOpcode::SMSG_MONSTER_MOVE: handleMonsterMove(packet); break;
        case LogicalOpcode::SMSG_COMPRESSED_MOVES: handleCompressedMoves(packet); break;
        case LogicalOpcode::SMSG_SPLINE_MOVE_SET_WALK_MODE: synth(walk, walk); break;
        // Run mode says nothing about swimming or flying (upstream).
        case LogicalOpcode::SMSG_SPLINE_MOVE_SET_RUN_MODE: synth(0u, walk); break;
        case LogicalOpcode::SMSG_SPLINE_MOVE_START_SWIM: synth(swim, swim); break;
        case LogicalOpcode::SMSG_SPLINE_MOVE_STOP_SWIM: synth(0u, swim); break;
        case LogicalOpcode::SMSG_SPLINE_MOVE_SET_FLYING: synth(fly, fly); break;
        case LogicalOpcode::SMSG_SPLINE_MOVE_UNSET_FLYING: synth(0u, fly); break;
        default: break;
    }
}

// upstream MovementHandler::handleMonsterMove, 3.3.5a path.
void WorldSession::handleMonsterMove(network::Packet& packet) {
    game::MonsterMoveData data;
    if (!game::MonsterMoveParser::parse(packet, data)) {
        // Upstream retries a wrapped sub-packet ("u8 size, u16 opcode,
        // payload") before giving up.
        const auto& raw = packet.getData();
        bool ok = false;
        if (raw.size() >= 3 && raw[0] >= 2 && static_cast<size_t>(raw[0]) + 1 == raw.size()) {
            std::vector<uint8_t> stripped(raw.begin() + 3, raw.begin() + 3 + (raw[0] - 2));
            network::Packet wrapped(packet.getOpcode(), stripped);
            data = game::MonsterMoveData{};
            ok = game::MonsterMoveParser::parse(wrapped, data);
        }
        if (!ok) {
            if (++monsterMoveFails_ <= 10)
                PSP_LOGW(tag::Net, "monster_move_bad n=%u", static_cast<unsigned>(monsterMoveFails_));
            return;
        }
    }
    if (playerGuid_ != 0 && data.guid == playerGuid_) {  // our own spline (the flight)
        taxiOwnMonsterMove(data);
        return;
    }
    auto entity = entities_.getEntity(data.guid);
    if (!entity) return;
    ++monsterMoves_;

    noteSplineGait(data);  // PSP: npc-walk - Walk or Run, the animation rate

    if (data.hasDest) {
        const glm::vec3 destCanonical = core::coords::serverToCanonical(glm::vec3(data.destX, data.destY, data.destZ));
        float orientation = entity->getOrientation();
        if (data.moveType == 4) {
            orientation = core::coords::serverToCanonicalYaw(data.facingAngle);
        } else if (data.moveType == 3) {
            auto target = entities_.getEntity(data.facingTarget);
            if (target) {
                const float dx = target->getX() - entity->getX();
                const float dy = target->getY() - entity->getY();
                if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) orientation = std::atan2(-dy, dx);
            }
        } else {
            const float dx = destCanonical.x - entity->getX();
            const float dy = destCanonical.y - entity->getY();
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) orientation = std::atan2(-dy, dx);
        }
        if (data.moveType != 3) {
            const glm::vec3 startCanonical = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
            const float travelDx = destCanonical.x - startCanonical.x;
            const float travelDy = destCanonical.y - startCanonical.y;
            const float travelLen = std::sqrt(travelDx * travelDx + travelDy * travelDy);
            if (travelLen > 0.5f) {
                const float travelAngle = std::atan2(-travelDy, travelDx);
                float diff = orientation - travelAngle;
                while (diff > kPi) diff -= 2.0f * kPi;
                while (diff < -kPi) diff += 2.0f * kPi;
                if (std::abs(diff) > kPi * 0.5f) orientation = travelAngle;
            }
        }
        if (!data.waypoints.empty()) {
            const glm::vec3 startCanonical = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
            std::vector<std::array<float, 3>> path;
            path.reserve(data.waypoints.size() + 2);
            path.push_back({startCanonical.x, startCanonical.y, startCanonical.z});
            for (const auto& wp : data.waypoints) {
                const glm::vec3 c = core::coords::serverToCanonical(glm::vec3(wp.x, wp.y, wp.z));
                path.push_back({c.x, c.y, c.z});
            }
            path.push_back({destCanonical.x, destCanonical.y, destCanonical.z});
            entity->startMoveAlongPath(path, orientation, data.duration / 1000.0f);
        } else {
            entity->startMoveTo(destCanonical.x, destCanonical.y, destCanonical.z, orientation,
                                data.duration / 1000.0f);
        }
    } else if (data.moveType == 1) {
        const glm::vec3 c = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
        entity->setPosition(c.x, c.y, c.z, entity->getOrientation());
    } else if (data.moveType == 4) {
        const glm::vec3 c = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
        entity->setPosition(c.x, c.y, c.z, core::coords::serverToCanonicalYaw(data.facingAngle));
    } else if (data.moveType == 3 && data.facingTarget != 0) {
        auto target = entities_.getEntity(data.facingTarget);
        if (target) {
            const float dx = target->getX() - entity->getX();
            const float dy = target->getY() - entity->getY();
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) entity->setOrientation(std::atan2(-dy, dx));
        }
    }
    // Network path, not the frame: a bounded count of lines, then every 500th.
    // PSP: npc-walk - walk=<gait drawn>/<WALKING move flag>, sf=<spline flags>, spd=<spline>/<unit walk speed>.
    if (++monsterMoveLogs_ <= 30 || monsterMoves_ % 500 == 0)
        PSP_LOGI(tag::Net, "monster_move guid=%08x t=%u pts=%u ms=%u walk=%d/%d n=%u sf=%x spd=%.1f/%.1f",
                 static_cast<unsigned>(data.guid & 0xFFFFFFFFu), static_cast<unsigned>(data.moveType),
                 static_cast<unsigned>(data.waypoints.size() + (data.hasDest ? 1u : 0u)),
                 static_cast<unsigned>(data.duration), unitWalkAnim(data.guid) ? 1 : 0,
                 (unitMoveFlags(data.guid) & kFlag(game::MovementFlags::WALKING)) ? 1 : 0,
                 static_cast<unsigned>(monsterMoves_), static_cast<unsigned>(data.splineFlags),  // walk/run
                 static_cast<double>(unitSplineSpeed(data.guid)), static_cast<double>(unitWalkSpeed(data.guid)));
}

// upstream MovementHandler::handleCompressedMoves.
void WorldSession::handleCompressedMoves(network::Packet& packet) {
    std::vector<uint8_t> inflated;
    const std::vector<uint8_t>* dataPtr = &packet.getData();
    const auto& raw = packet.getData();
    if (raw.size() >= 6 && raw[4] == 0x78 && (raw[5] == 0x01 || raw[5] == 0x9C || raw[5] == 0xDA || raw[5] == 0x5E)) {
        const uint32_t size = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8) |
                              (static_cast<uint32_t>(raw[2]) << 16) | (static_cast<uint32_t>(raw[3]) << 24);
        if (size == 0 || size > 65536) return;
        inflated.resize(size);
        uLongf len = size;
        if (uncompress(inflated.data(), &len, raw.data() + 4, raw.size() - 4) != Z_OK) return;
        inflated.resize(len);
        dataPtr = &inflated;
    }
    const std::vector<uint8_t>& data = *dataPtr;
    const uint16_t monsterMove = game::wireOpcode(game::LogicalOpcode::SMSG_MONSTER_MOVE);
    const game::OpcodeTable* table = game::getActiveOpcodeTable();
    struct Sub {
        uint16_t opcode;
        size_t start, len;
    };
    // payloadOnly: the size byte counts the payload alone (upstream fallback).
    auto decode = [&](bool payloadOnly, std::vector<Sub>& out, size_t& recognized) {
        out.clear();
        recognized = 0;
        size_t pos = 0;
        while (pos < data.size()) {
            const uint8_t sz = data[pos];
            if (sz == 0) return true;
            if (!payloadOnly && sz < 2) return false;
            const size_t payload = payloadOnly ? sz : static_cast<size_t>(sz) - 2;
            if (pos + 3 + payload > data.size()) return false;
            const uint16_t op = static_cast<uint16_t>(data[pos + 1] | (data[pos + 2] << 8));
            out.push_back({op, pos + 3, payload});
            if (op == monsterMove || (table && table->fromWire(op))) ++recognized;
            pos += 3 + payload;
        }
        return true;
    };
    std::vector<Sub> subs, alt;
    size_t rec = 0, altRec = 0;
    bool ok = decode(false, subs, rec);
    const bool altOk = decode(true, alt, altRec);
    if (altOk && (!ok || altRec > rec)) {
        subs.swap(alt);
        ok = true;
    }
    if (!ok) return;
    for (const Sub& s : subs) {
        network::Packet sub(s.opcode, std::vector<uint8_t>(data.begin() + s.start, data.begin() + s.start + s.len));
        auto logical = table ? table->fromWire(s.opcode) : std::nullopt;
        if (!logical) continue;
        using game::LogicalOpcode;
        switch (*logical) {
            case LogicalOpcode::SMSG_MONSTER_MOVE: handleMonsterMove(sub); break;
            case LogicalOpcode::SMSG_MONSTER_MOVE_TRANSPORT: handleMonsterMoveTransport(sub); break;
            case LogicalOpcode::MSG_MOVE_STOP:
            case LogicalOpcode::MSG_MOVE_STOP_STRAFE:
            case LogicalOpcode::MSG_MOVE_STOP_TURN:
            case LogicalOpcode::MSG_MOVE_STOP_SWIM:
            case LogicalOpcode::MSG_MOVE_FALL_LAND: handleOtherMovement(sub, true); break;
            case LogicalOpcode::MSG_MOVE_JUMP: handleOtherMovement(sub, false, true); break;
            case LogicalOpcode::MSG_MOVE_START_FORWARD:
            case LogicalOpcode::MSG_MOVE_START_BACKWARD:
            case LogicalOpcode::MSG_MOVE_START_STRAFE_LEFT:
            case LogicalOpcode::MSG_MOVE_START_STRAFE_RIGHT:
            case LogicalOpcode::MSG_MOVE_START_TURN_LEFT:
            case LogicalOpcode::MSG_MOVE_START_TURN_RIGHT:
            case LogicalOpcode::MSG_MOVE_SET_FACING:
            case LogicalOpcode::MSG_MOVE_HEARTBEAT:
            case LogicalOpcode::MSG_MOVE_START_SWIM:
            case LogicalOpcode::MSG_MOVE_SET_WALK_MODE:
            case LogicalOpcode::MSG_MOVE_SET_RUN_MODE: handleOtherMovement(sub, false); break;
            default: break;
        }
    }
}

} // namespace wowee::platform::psp
