// The game objects
// in sight - doors, chests, herbs, ore, mailboxes, signs, chairs, braziers... The session kept only the
// mailboxes; here every GAMEOBJECT create block (position, orientation) and its fields
// (upstream EntitySpawner::queueGameObjectSpawn: entry, display, orientation, scale; GAMEOBJECT_BYTES_1 state and
// type; GAMEOBJECT_DYNAMIC), forgotten on SMSG_DESTROY_OBJECT / out of range. The world screen draws their models
// (ui_world_gameobjects.cpp, data/gameobjects.pak).
// Also the fields the client reads to use an object - flags,
// faction, level, the creator, the art kit / anim progress bytes - the state change (the model's transition, as
// OnStateChanged sees old -> new) and the object cache query the client sends for a new entry
// (CMSG_GAMEOBJECT_QUERY, psp_world_session_gouse.cpp).
#include "platform/psp/psp_world_session.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"

#include <algorithm>
#include <cstring>

namespace wowee::platform::psp {

namespace {
// 3.3.5a (12340) GAMEOBJECT descriptor indices the upstream field table does not name
// (the 3.3.5a descriptor layout: CREATED_BY 6-7, DISPLAYID 8, FLAGS 9,
// PARENTROTATION 10-13, DYNAMIC 14, FACTION 15, LEVEL 16, BYTES_1 17).
constexpr uint16_t kGoFieldCreatedBy = 6, kGoFieldFlags = 9, kGoFieldFaction = 15;
}  // namespace

void WorldSession::gameObjectUpdateBlock(const game::UpdateBlock& b) {
    using game::UpdateType;
    const bool create = b.updateType == UpdateType::CREATE_OBJECT || b.updateType == UpdateType::CREATE_OBJECT2;
    // a VALUES block carries no object type - it is ours when its guid is a known object (the
    // state changes of doors, chests, lifts come that way; they were once dropped here).
    if (create && b.objectType != game::ObjectType::GAMEOBJECT) return;
    auto it = std::find_if(gameObjects_.begin(), gameObjects_.end(), [&](const GameObjectInfo& g) { return g.guid == b.guid; });
    if (create) {
        if (!b.hasMovement) return;  // no place to stand
        if (it == gameObjects_.end()) {
            gameObjects_.push_back(GameObjectInfo{});
            it = gameObjects_.end() - 1;
            it->guid = b.guid;
        }
        it->pos = glm::vec3(b.x, b.y, b.z);
        it->orientation = b.orientation;
        it->prevState = 0xFF;  // a create shows the state as it is (no transition)
        it->seenMs = logUptimeMs();
    } else if (it == gameObjects_.end() || b.updateType != UpdateType::VALUES) {
        return;
    }
    auto field = [&](game::UF f, uint32_t& out) {
        auto fv = b.fields.find(game::fieldIndex(f));
        if (fv == b.fields.end()) return false;
        out = fv->second;
        return true;
    };
    auto raw = [&](uint16_t index, uint32_t& out) {
        auto fv = b.fields.find(index);
        if (fv == b.fields.end()) return false;
        out = fv->second;
        return true;
    };
    uint32_t v = 0;
    if (field(game::UF::OBJECT_FIELD_ENTRY, v)) it->entry = v;
    if (field(game::UF::GAMEOBJECT_DISPLAYID, v)) it->displayId = v;
    if (field(game::UF::OBJECT_FIELD_SCALE_X, v)) {
        float f = 1.0f;
        std::memcpy(&f, &v, sizeof(f));
        it->scale = f > 0.01f && f < 100.0f ? f : 1.0f;
    }
    if (field(game::UF::GAMEOBJECT_BYTES_1, v)) {
        const uint8_t state = static_cast<uint8_t>(v & 0xFFu);
        // a VALUES update that moves the state is the transition the model plays
        // (READY -> ACTIVE opens, ACTIVE -> READY closes, -> ACTIVE_ALTERNATIVE destroys).
        if (!create && state != it->state) {
            it->prevState = it->state;
            ++it->stateSeq;
        }
        it->state = state;
        it->type = static_cast<uint8_t>((v >> 8) & 0xFFu);
        it->artKit = static_cast<uint8_t>((v >> 16) & 0xFFu);
        it->animProgress = static_cast<uint8_t>((v >> 24) & 0xFFu);
    }
    if (field(game::UF::GAMEOBJECT_DYNAMIC, v)) it->dynamic = v;
    // flags, faction, level, creator.
    if (raw(kGoFieldFlags, v)) it->flags = v;
    if (raw(kGoFieldFaction, v)) it->faction = v;
    if (field(game::UF::GAMEOBJECT_LEVEL, v)) it->level = v;
    {
        uint32_t lo = 0, hi = 0;
        const bool gotLo = raw(kGoFieldCreatedBy, lo), gotHi = raw(kGoFieldCreatedBy + 1, hi);
        if (gotLo || gotHi) {
            if (!gotLo) lo = static_cast<uint32_t>(it->createdBy & 0xFFFFFFFFu);
            if (!gotHi) hi = static_cast<uint32_t>(it->createdBy >> 32);
            it->createdBy = (static_cast<uint64_t>(hi) << 32) | lo;
        }
    }
    ++goVersion_;
    // the client asks its object cache for a new entry (the name, the type's data).
    if (create && it->entry != 0) (void)goTemplate(it->entry);
    if (create && ++goLogs_ <= 60)
        PSP_LOGI(tag::Net, "gameobject_seen guid=%08X entry=%u display=%u type=%u x=%.1f y=%.1f n=%u",
                 static_cast<unsigned>(b.guid & 0xFFFFFFFFu), static_cast<unsigned>(it->entry),
                 static_cast<unsigned>(it->displayId), static_cast<unsigned>(it->type), b.x, b.y,
                 static_cast<unsigned>(gameObjects_.size()));
}

void WorldSession::gameObjectForget(uint64_t guid) {
    const size_t before = gameObjects_.size();
    gameObjects_.erase(std::remove_if(gameObjects_.begin(), gameObjects_.end(),
                                      [guid](const GameObjectInfo& g) { return g.guid == guid; }),
                       gameObjects_.end());
    if (gameObjects_.size() != before) ++goVersion_;
}

}  // namespace wowee::platform::psp
