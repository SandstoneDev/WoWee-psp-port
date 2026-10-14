#include "platform/psp/psp_world_session.hpp"

#include "core/application.hpp"  // shadowed on PSP (src/platform/psp/shadow)
#include "core/coordinates.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_public_safe.hpp"  // PSP
#include "platform/psp/psp_strings.hpp"

#include <zlib.h>

#include <openssl/rand.h>  // mbedTLS shim on PSP

#include <pspthreadman.h>

#include <cstdio>
#include <cstring>  // std::memcpy (SMSG_WEATHER)
#include <exception>

namespace wowee::platform::psp {

namespace {

game::ExpansionRegistry g_registry;
game::OpcodeTable g_opcodes;
game::UpdateFieldTable g_updateFields;
// PLAYER_VISIBLE_ITEM_1_ENTRYID, stride 2 (entry, enchant), 19 slots: upstream
// GameHandler visibleItemEntryBase_ / visibleItemStride_ for 3.3.5a.
constexpr uint16_t kVisibleItemBase = 283;
bool g_tablesReady = false;

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

// Per-state timeout: a silent server must end in a readable failure, not a
// spinner.
constexpr uint64_t kStepTimeoutUs = 15ull * 1000 * 1000;

} // namespace

bool WorldSession::initTables(const std::string& dataRoot, const std::string& expansionId) {
    if (g_tablesReady) return true;
    // Upstream loaders use std::filesystem and iostreams, which throw; an
    // escaped exception ends the process at boot with nothing in the log.
    try {
        return initTablesImpl(dataRoot, expansionId);
    } catch (const std::exception& e) {
        PSP_LOGE(tag::Net, "tables_exception what=%s", e.what());
    } catch (...) {
        PSP_LOGE(tag::Net, "tables_exception what=unknown");
    }
    return false;
}

bool WorldSession::initTablesImpl(const std::string& dataRoot, const std::string& expansionId) {
    PSP_LOGI(tag::Net, "tables_init root=%s", dataRoot.c_str());
    size_t n = g_registry.initialize(dataRoot);
    PSP_LOGI(tag::Net, "tables_registry profiles=%u", static_cast<unsigned>(n));
    if (!g_registry.setActive(expansionId)) {
        PSP_LOGE(tag::Net, "expansion_missing id=%s root=%s profiles=%u", expansionId.c_str(), dataRoot.c_str(),
                 static_cast<unsigned>(n));
        return false;
    }
    core::Application::getInstance().setExpansionRegistry(&g_registry);
    const std::string opcodesPath = g_registry.getActive()->dataPath + "/opcodes.json";
    uint64_t t0 = nowUs();
    if (!g_opcodes.loadFromJson(opcodesPath)) {
        PSP_LOGE(tag::Net, "opcodes_load_failed path=%s", opcodesPath.c_str());
        return false;
    }
    game::setActiveOpcodeTable(&g_opcodes);
    // M10a: update field indices (upstream application.cpp does the same).
    const std::string fieldsPath = g_registry.getActive()->dataPath + "/update_fields.json";
    if (g_updateFields.loadFromJson(fieldsPath)) {
        game::setActiveUpdateFieldTable(&g_updateFields);
    } else {
        PSP_LOGE(tag::Net, "update_fields_load_failed path=%s", fieldsPath.c_str());
    }
    PSP_LOGI(tag::Net, "tables expansion=%s profiles=%u opcodes_ms=%u", expansionId.c_str(),
             static_cast<unsigned>(n), static_cast<unsigned>((nowUs() - t0) / 1000u));
    g_tablesReady = true;
    return true;
}

WorldSession::WorldSession() = default;

WorldSession::~WorldSession() { close(); }

const char* WorldSession::stateName() const {
    switch (state_) {
        case State::Idle: return "idle";
        case State::Connecting: return "connecting";
        case State::WaitChallenge: return "wait_challenge";
        case State::WaitAuthResponse: return "wait_auth_response";
        case State::Ready: return "ready";
        case State::WaitCharList: return "wait_char_list";
        case State::WaitCharCreate: return "wait_char_create";
        case State::WaitCharDelete: return "wait_char_delete";
        case State::WaitWorld: return "wait_world";
        case State::InWorld: return "in_world";
        case State::Failed: return "failed";
    }
    return "?";
}

void WorldSession::setState(State s) {
    state_ = s;
    stateSinceUs_ = nowUs();
    PSP_LOGI(tag::Net, "world_state=%s", stateName());
    logFlush();  // rare event: keep the trace on the card
}

void WorldSession::fail(const std::string& reason) {
    failure_ = reason;
    PSP_LOGE(tag::Net, "world_failed state=%s reason=%s", stateName(), reason.c_str());
    setState(State::Failed);
}

bool WorldSession::begin(const std::string& host, uint16_t port, const std::string& account,
                         const std::vector<uint8_t>& sessionKey, uint32_t realmId) {
    if (!g_tablesReady) {
        fail(OWN_STR(SESSION_NO_OPCODES));
        return false;
    }
    close();
    account_ = account;
    sessionKey_ = sessionKey;
    realmId_ = realmId;
    failure_.clear();
    characters_.clear();
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&clientSeed_), sizeof(clientSeed_)) != 1) {
        clientSeed_ = static_cast<uint32_t>(nowUs());
    }

    socket_ = std::make_unique<network::WorldSocket>();
    socket_->setPacketCallback([this](const network::Packet& p) { onPacket(p); });
    setState(State::Connecting);
    uint64_t t0 = nowUs();
    bool ok = socket_->connect(host, port);
    PSP_LOGI(tag::Net, "world_connect host=%s port=%u ok=%d ms=%u", host.c_str(), static_cast<unsigned>(port),
             ok ? 1 : 0, static_cast<unsigned>((nowUs() - t0) / 1000u));
    if (!ok) {
        fail(OWN_FMT(SESSION_WORLD_NO_ANSWER, host));
        return false;
    }
    publicSafeOnConnect("realm", host.c_str());  // PSP: the realm's address decides the debug aids too
    wardenBegin();  // PSP: Warden state for this session (psp_warden.cpp)
    setState(State::WaitChallenge);
    return true;
}

void WorldSession::close() {
    wardenEnd();  // PSP
    if (socket_) socket_->disconnect();
    socket_.reset();
    if (state_ != State::Failed) state_ = State::Idle;
}

void WorldSession::debugDropConnection() {
    PSP_LOGW(tag::Net, "world_debug_drop state=%s", stateName());
    if (socket_) socket_->disconnect();
}

// Against a GLUE_DISCONNECTED right after the loading screen: the world loads in one
// go and nothing goes to the server meanwhile, while AzerothCore closes an in-world socket that sent nothing
// for SocketTimeOutTimeActive (60 s). CMSG_PING does not count there (WorldSocket::HandlePing), CMSG_KEEP_ALIVE
// does (ResetTimeOutTime); the time-sync answers go only after the load. On a busy PC the emulator's load took
// minutes of real time. LoginScreen sends one per loading step and per 16 pak reads of the load: 6 bytes, no answer.
bool WorldSession::keepAlive() {
    if (!socket_ || !socket_->isConnected() || state_ != State::InWorld) return false;
    socket_->send(network::Packet(game::wireOpcode(game::Opcode::CMSG_KEEP_ALIVE)));
    return true;
}

void WorldSession::update() {
    if (!socket_) return;
    socket_->update();
    if (state_ != State::Failed && socket_ && !socket_->isConnected()) {
        fail(CLIENT_STR(GLUE_DISCONNECTED));
        return;
    }
    const bool waiting = state_ == State::WaitChallenge || state_ == State::WaitAuthResponse ||
                         state_ == State::WaitCharList || state_ == State::WaitCharCreate ||
                         state_ == State::WaitCharDelete ||
                         state_ == State::WaitWorld;
    // Keepalive: CMSG_PING every 30 s in the world, as GameHandler does
    // (game_handler.hpp kPingInterval); the server answers SMSG_PONG.
    if (state_ == State::InWorld && nowUs() - lastPingUs_ > 30ull * 1000 * 1000) {
        lastPingUs_ = nowUs();
        socket_->send(game::PingPacket::build(++pingSeq_, 50));
    }
    if (waiting && nowUs() - stateSinceUs_ > kStepTimeoutUs) fail(OWN_FMT(SESSION_TIMEOUT, stateName()));
    if (state_ == State::InWorld) chatUpdate();  // chat
    if (state_ == State::InWorld) inventoryUpdate();  // inventory (rebuild when dirty)
    if (state_ == State::InWorld) tutorialUpdate();  // tutorials (game points -> TUTORIAL_TRIGGER)
}

void WorldSession::requestCharacterList() {
    if (!socket_ || (state_ != State::Ready && state_ != State::WaitCharList)) return;
    socket_->send(game::CharEnumPacket::build());
    setState(State::WaitCharList);
}

void WorldSession::createCharacter(const game::CharCreateData& data) {
    if (!socket_ || state_ != State::Ready) return;
    lastCreateResult_ = -1;
    PSP_LOGI(tag::Net, "char_create name=%s race=%u class=%u gender=%u", data.name.c_str(),
             static_cast<unsigned>(data.race), static_cast<unsigned>(data.characterClass),
             static_cast<unsigned>(data.gender));
    socket_->send(game::CharCreatePacket::build(data));
    setState(State::WaitCharCreate);
}

void WorldSession::deleteCharacter(uint64_t characterGuid) {
    if (!socket_ || state_ != State::Ready) return;
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_CHAR_DELETE));
    packet.writeUInt64(characterGuid);
    socket_->send(packet);
    PSP_LOGI(tag::Net, "char_delete guid=%08X%08X", static_cast<unsigned>(characterGuid >> 32),
             static_cast<unsigned>(characterGuid));
    setState(State::WaitCharDelete);
}

// --- M10a: objects (upstream UpdateObjectParser / EntityManager / Entity
// interpolation; the EntityController is tied to GameHandler, so its few
// relevant steps are mirrored here) -----------------------------------------

// GCC 15 (-O2, MIPS) reports a false -Wmaybe-uninitialized inside the
// upstream Entity::setPosition (its optional spline) once inlined here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

void WorldSession::handleUpdateObject(network::Packet& packet) {
    game::UpdateObjectData data;
    if (!game::UpdateObjectParser::parse(packet, data)) return;
    for (uint64_t guid : data.outOfRangeGuids) removeEntity(guid);
    for (const auto& b : data.blocks) applyUpdateBlock(b);
    if ((updateBlocks_ += static_cast<uint32_t>(data.blocks.size())) <= 40 || data.blocks.size() > 20)
        PSP_LOGI(tag::Net, "update_object blocks=%u out_of_range=%u units=%u", static_cast<unsigned>(data.blocks.size()),
                 static_cast<unsigned>(data.outOfRangeGuids.size()),
                 static_cast<unsigned>(entities_.getEntities().size()));
}

void WorldSession::applyUpdateBlock(const game::UpdateBlock& b) {
    using game::UpdateType;
    transferSelfBlock(b);  // our world place after a transfer on a ship / zeppelin
    transportUpdateBlock(b);  // transport objects (11 / 15), riders, our place on a transport
    classBarsUpdateBlock(b);  // the units' vehicle kits, our seat, the riders' seats
    taxiUpdateBlock(b);  // UNIT_FLAG_TAXI_FLIGHT in our create block (logged in mid-flight)
    bankMailUpdateBlock(b);  // the mailboxes in sight (game objects, kept apart from the entities)
    gameObjectUpdateBlock(b);  // every game object in sight (their models)
    mountUpdateBlock(b);  // mounts (own mount field + speeds, other players' run speed)
    flyingUpdateBlock(b);  // other players' flight / swim speeds, our create block's flags
    npcUpdateBlock(b);  // npc (quest-log fields, questgiver status query on create)
    if (inventoryUpdateBlock(b)) return;  // inventory (item/bag objects, own slot fields)
    if (combatUpdateBlock(b)) return;  // combat (own player's fields, creature names)
    const bool create = b.updateType == UpdateType::CREATE_OBJECT || b.updateType == UpdateType::CREATE_OBJECT2;
    if (create) {
        // Units and players only for now (M10a/b); the own player is ours.
        if (b.guid == playerGuid_) return;
        std::shared_ptr<game::Entity> e;
        if (b.objectType == game::ObjectType::PLAYER) e = std::make_shared<game::Player>(b.guid);
        else if (b.objectType == game::ObjectType::UNIT) e = std::make_shared<game::Unit>(b.guid);
        else return;
        if (b.hasMovement) {
            const glm::vec3 c = core::coords::serverToCanonical(glm::vec3(b.x, b.y, b.z));
            e->setPosition(c.x, c.y, c.z, core::coords::serverToCanonicalYaw(b.orientation));
        }
        for (const auto& [k, v] : b.fields) e->setField(k, v);
        entities_.addEntity(b.guid, e);
        if (b.hasMovement && b.moveFlags) setUnitMoveFlags(b.guid, b.moveFlags, ~0u);  // creature anim
        ++entityVersion_;
        if (b.objectType == game::ObjectType::PLAYER) {
            queryVisibleItems(*e);
            auto n = names_.find(b.guid);
            if (n != names_.end()) {
                static_cast<game::Player*>(e.get())->setName(n->second);
            } else if (nameQueried_.insert(b.guid).second && socket_) {
                socket_->send(game::NameQueryPacket::build(b.guid));
            }
        }
        return;
    }
    auto e = entities_.getEntity(b.guid);
    if (!e) return;
    if (b.updateType == UpdateType::VALUES) {
        const uint16_t look[] = {game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0),
                                 game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID),
                                 game::fieldIndex(game::UF::PLAYER_BYTES), game::fieldIndex(game::UF::PLAYER_BYTES_2)};
        bool items = false;
        // Other players' look (and the re-dress on SHOW_HELM / SHOW_CLOAK): a visible item's enchantment (PLAYER_VISIBLE_ITEM_n_ENCHANTMENT, its glow) and the
        // "show helm / cloak" flags (PLAYER_FLAGS_HIDE_HELM 0x400 / _HIDE_CLOAK 0x800) change the look too - the
        // world screen re-dresses on the version (WorldScreen::syncRemotePlayers).
        bool equipLook = false;
        const uint16_t fPlayerFlags = game::fieldIndex(game::UF::PLAYER_FLAGS);
        for (const auto& [k, v] : b.fields) {
            for (uint16_t idx : look)
                if (k == idx && e->getField(k) != v) ++entityVersion_;
            if (k >= kVisibleItemBase && k < kVisibleItemBase + 19 * 2 && (k - kVisibleItemBase) % 2 == 0 &&
                e->getField(k) != v)
                items = true;
            if (k > kVisibleItemBase && k < kVisibleItemBase + 19 * 2 && (k - kVisibleItemBase) % 2 == 1 &&
                e->getField(k) != v)
                equipLook = true;
            if (k == fPlayerFlags && ((e->getField(k) ^ v) & 0xC00u)) equipLook = true;
            e->setField(k, v);
        }
        if (items && e->getType() == game::ObjectType::PLAYER) {
            queryVisibleItems(*e);
            ++entityVersion_;
        }
        if (equipLook && !items && e->getType() == game::ObjectType::PLAYER) ++entityVersion_;
    } else if (b.updateType == UpdateType::MOVEMENT && b.hasMovement) {
        const glm::vec3 c = core::coords::serverToCanonical(glm::vec3(b.x, b.y, b.z));
        e->setPosition(c.x, c.y, c.z, core::coords::serverToCanonicalYaw(b.orientation));
    }
}

void WorldSession::queryVisibleItems(const game::Entity& e) {
    if (!socket_) return;
    for (int s = 0; s < 19; ++s) {
        const uint32_t entry = e.getField(static_cast<uint16_t>(kVisibleItemBase + s * 2));
        if (entry == 0 || itemInfo_.count(entry) || !itemQueried_.insert(entry).second) continue;
        // upstream queryItemInfo: some cores reject guid 0, send ours.
        socket_->send(game::ItemQueryPacket::build(entry, playerGuid_));
    }
}

bool WorldSession::equipmentOf(uint64_t guid, uint32_t (&displayIds)[19], uint8_t (&invTypes)[19],
                               int* unresolved) const {
    if (unresolved) *unresolved = 0;
    auto e = entities_.getEntity(guid);
    if (!e) return false;
    int entries = 0, resolved = 0;
    for (int s = 0; s < 19; ++s) {
        displayIds[s] = 0;
        invTypes[s] = 0;
        const uint32_t entry = e->getField(static_cast<uint16_t>(kVisibleItemBase + s * 2));
        if (!entry) continue;
        ++entries;
        auto it = itemInfo_.find(entry);
        if (it == itemInfo_.end()) {
            if (unresolved) ++*unresolved;
            continue;
        }
        displayIds[s] = it->second.displayId;
        invTypes[s] = it->second.invType;
        ++resolved;
    }
    return entries == 0 || resolved > 0;
}

// Item.dbc first, the item query for what it lacks (psp_world_session.hpp).
bool WorldSession::equipmentOf(uint64_t guid, uint32_t (&displayIds)[19], uint8_t (&invTypes)[19], int* unresolved,
                               ItemDbcLookup dbc, void* user) const {
    if (!dbc) return equipmentOf(guid, displayIds, invTypes, unresolved);
    if (unresolved) *unresolved = 0;
    auto e = entities_.getEntity(guid);
    if (!e) return false;
    int entries = 0, resolved = 0;
    for (int s = 0; s < 19; ++s) {
        displayIds[s] = 0;
        invTypes[s] = 0;
        const uint32_t entry = e->getField(static_cast<uint16_t>(kVisibleItemBase + s * 2));
        if (!entry) continue;
        ++entries;
        uint32_t displayId = 0;
        uint8_t invType = 0;
        if (dbc(user, entry, displayId, invType)) {
            displayIds[s] = displayId;
            invTypes[s] = invType;
            ++resolved;
            continue;
        }
        auto it = itemInfo_.find(entry);
        if (it == itemInfo_.end()) {
            if (unresolved) ++*unresolved;
            continue;
        }
        displayIds[s] = it->second.displayId;
        invTypes[s] = it->second.invType;
        ++resolved;
    }
    return entries == 0 || resolved > 0;
}

void WorldSession::removeEntity(uint64_t guid) {
    inventoryForget(guid);  // inventory (items are not entities here)
    bankMailForget(guid);   // a mailbox out of sight
    gameObjectForget(guid);
    transportForget(guid);
    flyingForget(guid);
    classBarsForget(guid);   // a totem, a vehicle, a rider gone
    if (!entities_.hasEntity(guid)) return;
    entities_.removeEntity(guid);
    nameQueried_.erase(guid);
    moveTimeMs_.erase(guid);
    moveIntervalMs_.erase(guid);
    jumpHints_.erase(guid);
    walking_.erase(guid);
    motion_.erase(guid);
    unitMoveFlags_.erase(guid);  // creature anim
    unitSplineSpeed_.erase(guid);  // walk/run
    unitRunSpeed_.erase(guid);  // mounts
    unitWalkSpeed_.erase(guid);  // PSP: npc-walk
    splineGait_.erase(guid);     // PSP: npc-walk
    ++entityVersion_;
}

// upstream MovementHandler::handleOtherPlayerMovement, + the ONTRANSPORT block (the client's
// MovementInfo order: transport guid, offset x y z o, time, seat, time2) and the rider it makes.
void WorldSession::handleOtherMovement(network::Packet& packet, bool stop, bool jump) {
    const uint64_t guid = packet.readPackedGuid();
    if (guid == 0 || guid == playerGuid_) return;
    game::MovementInfo info{};
    info.flags = packet.readUInt32();
    info.flags2 = packet.readUInt16();
    info.time = packet.readUInt32();
    info.x = packet.readFloat();
    info.y = packet.readFloat();
    info.z = packet.readFloat();
    info.orientation = packet.readFloat();
    // The rest of the 3.3.5 movement block (MovementPacket::writeMovementPayload
    // order), for the prediction.
    using MF = game::MovementFlags;
    const bool onTransport = (info.flags & static_cast<uint32_t>(MF::ONTRANSPORT)) != 0;
    if (onTransport) transportReadMoveBlock(packet, info);  // the rest follows it
    {
        if ((info.flags & (static_cast<uint32_t>(MF::SWIMMING) | static_cast<uint32_t>(MF::FLYING))) ||
            (info.flags2 & 0x0020))
            info.pitch = packet.readFloat();
        info.fallTime = packet.readUInt32();
        if (info.flags & static_cast<uint32_t>(MF::FALLING)) {
            info.jumpVelocity = packet.readFloat();
            info.jumpSinAngle = packet.readFloat();
            info.jumpCosAngle = packet.readFloat();
            info.jumpXYSpeed = packet.readFloat();
        }
    }
    auto e = entities_.getEntity(guid);
    if (!e) return;
    if (onTransport) {
        motion_.erase(guid);
    } else {
        RemoteMotion& m = motion_[guid];
        const int64_t offset = static_cast<int64_t>(nowUs() / 1000u) - static_cast<int64_t>(info.time);
        const bool first = m.info.time == 0 && m.clockOffsetMs == 0;
        // Smallest latency seen = the offset; +2 ms per packet lets it follow
        // a clock that drifts the other way.
        m.clockOffsetMs = first ? offset : std::min(m.clockOffsetMs + 2, offset);
        m.info = info;
    }
    uint32_t durationMs = 120;
    auto prev = moveTimeMs_.find(guid);
    if (prev != moveTimeMs_.end()) {
        const uint32_t raw = info.time - prev->second;
        if (raw >= 20 && raw <= 2000) {
            float& smoothed = moveIntervalMs_[guid];
            if (smoothed < 1.0f) smoothed = static_cast<float>(raw);
            smoothed = 0.7f * smoothed + 0.3f * static_cast<float>(raw);
            durationMs = static_cast<uint32_t>(std::max(60.0f, std::min(500.0f, smoothed)));
        }
    }
    moveTimeMs_[guid] = info.time;
    transportOtherMove(guid, info, stop ? 0u : durationMs);  // on a transport or off it
    const glm::vec3 c = core::coords::serverToCanonical(glm::vec3(info.x, info.y, info.z));
    e->startMoveTo(c.x, c.y, c.z, core::coords::serverToCanonicalYaw(info.orientation),
                   stop ? 0.0f : durationMs / 1000.0f);
    if (jump) jumpHints_.insert(guid);
    if (info.flags & static_cast<uint32_t>(game::MovementFlags::WALKING)) walking_.insert(guid);
    else walking_.erase(guid);
    // PSP: npc-walk - upstream handleOtherPlayerMovement hands every MSG_MOVE_* mover's flags to
    // unitMoveFlagsCallback (a charmed creature, a vehicle walks / swims / flies by them); a spline gait is over.
    setUnitMoveFlags(guid, info.flags, ~0u);
    splineGait_.erase(guid);
}

bool WorldSession::predictRemote(uint64_t guid, glm::vec3& pos, float& yaw, bool& moving, bool& falling) const {
    auto it = motion_.find(guid);
    if (it == motion_.end()) return false;
    using MF = game::MovementFlags;
    const game::MovementInfo& m = it->second.info;
    const int64_t senderNow = static_cast<int64_t>(nowUs() / 1000u) - it->second.clockOffsetMs;
    // At most 1 s past the packet: heartbeats come every 0.5 s while moving.
    const float dt = std::clamp(static_cast<float>(senderNow - static_cast<int64_t>(m.time)) / 1000.0f, 0.0f, 1.0f);
    const uint32_t dirBits = static_cast<uint32_t>(MF::FORWARD) | static_cast<uint32_t>(MF::BACKWARD) |
                             static_cast<uint32_t>(MF::STRAFE_LEFT) | static_cast<uint32_t>(MF::STRAFE_RIGHT);
    falling = (m.flags & static_cast<uint32_t>(MF::FALLING)) != 0;
    moving = (m.flags & dirBits) != 0;
    pos = glm::vec3(m.x, m.y, m.z);
    // Keyboard turning: the client's turn rate pi rad/s; the move below follows the
    // mid-way facing (close to the arc).
    float turn = 0.0f;
    if (m.flags & static_cast<uint32_t>(MF::TURN_LEFT)) turn += 3.14159265f;
    if (m.flags & static_cast<uint32_t>(MF::TURN_RIGHT)) turn -= 3.14159265f;
    yaw = m.orientation + turn * dt;
    const float moveYaw = m.orientation + turn * dt * 0.5f;
    if (flightPredict(guid, m, dt, moveYaw, pos, moving)) return true;  // a flier / swimmer in 3D
    if (falling) {
        // Jump: the horizontal speed and direction fixed at take-off, z
        // ballistic (the client's gravity 19.29) from the packet's fall time.
        pos.x += m.jumpCosAngle * m.jumpXYSpeed * dt;
        pos.y += m.jumpSinAngle * m.jumpXYSpeed * dt;
        const float t0 = m.fallTime / 1000.0f;
        const float vz = std::fabs(m.jumpVelocity) - 19.29f * t0;
        pos.z += vz * dt - 0.5f * 19.29f * dt * dt;
        return true;
    }
    if (!moving) return true;
    float fx = 0.0f, fy = 0.0f;  // x = forward, y = left (server: orientation CCW from +x)
    if (m.flags & static_cast<uint32_t>(MF::FORWARD)) fx += 1.0f;
    if (m.flags & static_cast<uint32_t>(MF::BACKWARD)) fx -= 1.0f;
    if (m.flags & static_cast<uint32_t>(MF::STRAFE_LEFT)) fy += 1.0f;
    if (m.flags & static_cast<uint32_t>(MF::STRAFE_RIGHT)) fy -= 1.0f;
    const float len = std::sqrt(fx * fx + fy * fy);
    if (len < 1e-3f) {
        moving = false;
        return true;
    }
    // The client's default speeds: run 7, backpedal 4.5, walk 2.5.
    const bool walking = (m.flags & static_cast<uint32_t>(MF::WALKING)) != 0;
    // mounts - the run speed the server gave that player (a mount), 7 when unknown.
    // PSP: npc-walk - and its walk speed (2.5 when unknown).
    const float speed = walking ? unitWalkSpeed(guid) : (fx < 0.0f && fy == 0.0f ? 4.5f : unitRunSpeed(guid));
    const float c = std::cos(moveYaw), s = std::sin(moveYaw);
    const float dx = (fx * c - fy * s) / len, dy = (fx * s + fy * c) / len;
    pos.x += dx * speed * dt;
    pos.y += dy * speed * dt;
    return true;
}

void WorldSession::updateEntities(float dtSec) {
    for (const auto& [guid, e] : entities_.getEntities()) e->updateMovement(dtSec);
}

#pragma GCC diagnostic pop

const std::string* WorldSession::nameOf(uint64_t guid) const {
    auto it = names_.find(guid);
    return it == names_.end() ? nullptr : &it->second;
}

void WorldSession::sendMovement(game::Opcode opcode, game::MovementInfo info) {
    if (state_ != State::InWorld || !socket_) return;
    // after the worldport ACK the old world (still on screen until the LoginScreen rebuilds it)
    // must not tell the server the old map's place.
    if (transferStage_ == TransferStage::Ready) return;
    // the unit we move (a vehicle we drive, a possessed unit) - its guid; a passenger sends nothing
    // (AzerothCore takes a packet without ONTRANSPORT off the vehicle), nor do we while the server moves us.
    uint64_t moveGuid = playerGuid_;
    if (!movementGuid(moveGuid)) return;
    info.time = static_cast<uint32_t>(nowUs() / 1000u);
    lastOwnMove_ = info;  // mounts - the movement block of the speed-change ACKs
    socket_->send(game::MovementPacket::build(opcode, info, moveGuid));
}

void WorldSession::sendTextEmote(uint32_t textEmoteId) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::TextEmotePacket::build(textEmoteId, 0));
}

void WorldSession::sendStandState(uint8_t state) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::StandStateChangePacket::build(state));
}

// CMSG_SET_SHEATHED - the server's HandleSetSheathedOpcode reads one u32
// (TrinityCore / AzerothCore: SetSheath, values >= MAX_SHEATH_STATE refused).
void WorldSession::sendSetSheathed(uint8_t state) {
    if (state_ != State::InWorld || !socket_ || state > 2) return;
    if (transferStage_ == TransferStage::Ready) return;  // the old world, as sendMovement
    network::Packet packet(game::wireOpcode(game::LogicalOpcode::CMSG_SET_SHEATHED));
    packet.writeUInt32(state);
    socket_->send(packet);
}

void WorldSession::enterWorld(uint64_t characterGuid) {
    if (!socket_ || state_ != State::Ready) return;
    playerGuid_ = characterGuid;
    socket_->send(game::PlayerLoginPacket::build(characterGuid));
    setState(State::WaitWorld);
}

void WorldSession::onPacket(const network::Packet& packetIn) {
    network::Packet packet = packetIn;  // parsers take a mutable packet
    const game::OpcodeTable* table = game::getActiveOpcodeTable();
    auto logical = table ? table->fromWire(packet.getOpcode()) : std::nullopt;
    if (!logical) return;

    using game::LogicalOpcode;
    // PSP: Warden first and in every state - from SMSG_AUTH_RESPONSE on, AzerothCore's MODULE_USE can come
    // before the character list; a packet not decrypted would leave its RC4 stream out of step for the session.
    if (*logical == LogicalOpcode::SMSG_WARDEN_DATA) {
        handleWardenData(packet);
        return;
    }
    // SMSG_TRANSFER_PENDING / SMSG_NEW_WORLD / SMSG_TRANSFER_ABORTED and the instance packets
    // (psp_world_session_transfer.cpp) - before the death handler, which only logged the transfers.
    if (handleTransferPacket(*logical, packet)) return;
    // totems, vehicles (our seat from SMSG_MONSTER_MOVE_TRANSPORT onto a unit, our TransportExit
    // spline), the mover (SMSG_CLIENT_CONTROL_UPDATE) and its speed changes - before the transports, the NPC moves and
    // the mount code (psp_world_session_classbars.cpp); other movers' packets go on
    if (handleClassBarsPacket(*logical, packet)) return;
    // SMSG_MONSTER_MOVE_TRANSPORT; peeks SMSG_GAMEOBJECT_QUERY_RESPONSE (a transport's template)
    if (handleTransportPacket(*logical, packet)) return;
    if (handleTooltipPacket(*logical, packet)) return;  // item extras (peek), proficiency, bind point
    // SMSG_ITEM_COOLDOWN, SMSG_STANDSTATE_UPDATE; peeks SPELL_GO / COOLDOWN_EVENT / CLEAR_COOLDOWN /
    // INITIAL_SPELLS / LOGIN_VERIFY_WORLD (the item cooldowns) before the spells see them (psp_world_session_items.cpp)
    if (handleItemUsePacket(*logical, packet)) return;
    if (handlePartyPacket(*logical, packet)) return;  // group packets (psp_world_session_party.cpp)
    // the guild bank's packets, the chat channels (psp_world_session_guildbank.cpp /
    // _channels.cpp); peeks SMSG_GUILD_EVENT (the bank's events) and SMSG_LOGIN_VERIFY_WORLD - they go on
    if (handleGuildBankPacket(*logical, packet) || handleChannelPacket(*logical, packet)) return;
    // friends / ignore / who / guild (psp_world_session_social.cpp); peeks SMSG_NAME_QUERY_RESPONSE
    if (handleSocialPacket(*logical, packet)) return;
    // the pet, its bar, talents, the stable (psp_world_session_pets.cpp); peeks SMSG_SPELL_COOLDOWN /
    // SMSG_COOLDOWN_EVENT / SMSG_TALENTS_INFO / SMSG_TALENTS_INVOLUNTARILY_RESET (takes only the pet's) and
    // SMSG_ATTACKSTART / STOP, SMSG_CLEAR_COOLDOWN (they go on) - before the combat, trainer and
    // spell handlers
    if (handlePetPacket(*logical, packet)) return;
    // raid marks, ready checks, battlegrounds, world states (psp_world_session_raid.cpp /
    // _battleground.cpp); peeks SMSG_INIT_WORLD_STATES and SMSG_NAME_QUERY_RESPONSE (they go on below)
    if (handleRaidBgPacket(*logical, packet)) return;
    // the trade, the duels, summons, escort / shared quests (psp_world_session_trade_duel.cpp);
    // peeks SMSG_QUEST_QUERY_RESPONSE (the quest's share flag) - it goes on to the NPC handler
    if (handleTradeDuelPacket(*logical, packet)) return;
    if (handleChatPacket(*logical, packet)) return;  // chat (psp_world_session_chat.cpp)
    if (handleCombatPacket(*logical, packet)) return;  // combat
    if (handleTrainerPacket(*logical, packet)) return;  // trainers + talents (watches LEARNED_SPELL first)
    if (handleSpellPacket(*logical, packet)) return;   // spells (psp_world_session_spells.cpp)
    if (handleLootPacket(*logical, packet)) return;  // loot (psp_world_session_loot.cpp)
    if (handleNpcPacket(*logical, packet)) return;  // npc (psp_world_session_npc.cpp)
    if (handleFactionPacket(*logical, packet)) return;  // factions (psp_world_session_factions.cpp)
    if (handleInventoryPacket(*logical, packet)) return;  // inventory (+ item query cache)
    // the Dungeon Finder, the achievements, the calendar (the calendar peeks the pending invite
    // notices and lets them go on to the minimap's handler)
    if (handleLfgPacket(*logical, packet)) return;
    if (handleAchievementPacket(*logical, packet)) return;
    if (handleCalendarPacket(*logical, packet)) return;
    // mail / calendar notices; peeks the realm's date of the time packets (they go on below)
    if (handleMinimapPacket(*logical, packet)) return;
    if (handleTimePacket(*logical, packet)) return;  // lighting (realm clock)
    if (handleTutorialPacket(*logical, packet)) return;  // tutorials (psp_world_session_tutorial.cpp)
    if (handleFlyingPacket(*logical, packet)) return;  // flag toggles + ACKs, climb packets (peeks speeds)
    if (handleMountPacket(*logical, packet)) return;  // mounts (speed changes, psp_world_session_mount.cpp)
    if (handleTaxiPacket(*logical, packet)) return;  // flight paths (psp_world_session_taxi.cpp)
    if (handleBankMailPacket(*logical, packet)) return;  // bank, mailbox (psp_world_session_mail.cpp)
    // the object cache, page texts, custom / despawn anims, fishing, the barber shop, the stand
    // state (psp_world_session_gouse.cpp)
    if (handleGameObjectPacket(*logical, packet)) return;
    if (handleAuctionPacket(*logical, packet)) return;  // the auction house (psp_world_session_auction.cpp)
    if (handleMoviePacket(*logical, packet)) return;  // movies, cinematics (psp_world_session_movie.cpp)
    if (handleInspectPacket(*logical, packet)) return;  // the inspect (psp_world_session_inspect.cpp)
    switch (*logical) {
        case LogicalOpcode::SMSG_AUTH_CHALLENGE: {
            game::AuthChallengeData challenge{};
            if (!game::AuthChallengeParser::parse(packet, challenge)) {
                fail(OWN_FMT(SESSION_BAD_PACKET, "SMSG_AUTH_CHALLENGE"));
                return;
            }
            socket_->send(game::AuthSessionPacket::build(12340, account_, clientSeed_, sessionKey_,
                                                         challenge.serverSeed, realmId_));
            // AzerothCore encrypts SMSG_AUTH_RESPONSE: RC4 goes on right after
            // CMSG_AUTH_SESSION, exactly as GameHandler::sendAuthSession does.
            socket_->initEncryption(sessionKey_, 12340);
            setState(State::WaitAuthResponse);
            break;
        }
        case LogicalOpcode::SMSG_AUTH_RESPONSE: {
            game::AuthResponseData response{};
            if (!game::AuthResponseParser::parse(packet, response)) {
                fail(OWN_FMT(SESSION_BAD_PACKET, "SMSG_AUTH_RESPONSE"));
                return;
            }
            if (!response.isSuccess()) {
                fail(OWN_FMT(SESSION_WORLD_REFUSED, game::getAuthResultString(response.result)));
                return;
            }
            setState(State::Ready);
            requestCharacterList();
            break;
        }
        case LogicalOpcode::SMSG_CHAR_ENUM: {
            game::CharEnumResponse response;
            uint64_t t0 = nowUs();
            if (!game::CharEnumParser::parse(packet, response)) {
                fail(OWN_FMT(SESSION_BAD_PACKET, "SMSG_CHAR_ENUM"));
                return;
            }
            characters_ = std::move(response.characters);
            charListFresh_ = true;
            PSP_LOGI(tag::Net, "char_enum count=%u parse_us=%u", static_cast<unsigned>(characters_.size()),
                     static_cast<unsigned>(nowUs() - t0));
            for (const auto& c : characters_) {
                PSP_LOGI(tag::Net, "char name=%s level=%u race=%u class=%u map=%u zone=%u", c.name.c_str(),
                         c.level, static_cast<unsigned>(c.race), static_cast<unsigned>(c.characterClass),
                         static_cast<unsigned>(c.mapId), static_cast<unsigned>(c.zoneId));
            }
            setState(State::Ready);
            break;
        }
        case LogicalOpcode::SMSG_CHAR_CREATE: {
            game::CharCreateResponseData response{};
            if (!game::CharCreateResponseParser::parse(packet, response)) {
                fail(OWN_FMT(SESSION_BAD_PACKET, "SMSG_CHAR_CREATE"));
                return;
            }
            lastCreateResult_ = static_cast<int>(response.result);
            PSP_LOGI(tag::Net, "char_create_result=0x%02X", static_cast<unsigned>(response.result));
            setState(State::Ready);
            requestCharacterList();
            break;
        }
        case LogicalOpcode::SMSG_CHAR_DELETE: {
            // upstream game_handler_packets.cpp: one result byte, then the list again.
            lastDeleteResult_ = packet.hasRemaining(1) ? static_cast<int>(packet.readUInt8()) : -1;
            ++deleteSeq_;
            PSP_LOGI(tag::Net, "char_delete_result=0x%02X", static_cast<unsigned>(lastDeleteResult_ & 0xFF));
            setState(State::Ready);
            requestCharacterList();
            break;
        }
        case LogicalOpcode::SMSG_LOGIN_VERIFY_WORLD: {
            if (!game::LoginVerifyWorldParser::parse(packet, worldEntry_)) {
                fail(OWN_FMT(SESSION_BAD_PACKET, "SMSG_LOGIN_VERIFY_WORLD"));
                return;
            }
            PSP_LOGI(tag::Net, "login_verify_world map=%u x=%.1f y=%.1f z=%.1f o=%.2f",
                     static_cast<unsigned>(worldEntry_.mapId), worldEntry_.x, worldEntry_.y, worldEntry_.z,
                     worldEntry_.orientation);
            // As GameHandler::handleLoginVerifyWorld: we are the active mover.
            socket_->send(game::SetActiveMoverPacket::build(playerGuid_));
            lastPingUs_ = nowUs();
            socket_->send(game::PingPacket::build(++pingSeq_, 50));
            setState(State::InWorld);
            break;
        }
        case LogicalOpcode::SMSG_CHARACTER_LOGIN_FAILED:
            fail(OWN_STR(SESSION_CHAR_LOGIN_REFUSED));
            break;
        case LogicalOpcode::SMSG_TIME_SYNC_REQ: {
            // Built inline upstream too (game_handler_packets.cpp): counter + client ms.
            const uint32_t counter = packet.readUInt32();
            network::Packet resp(game::wireOpcode(LogicalOpcode::CMSG_TIME_SYNC_RESP));
            resp.writeUInt32(counter);
            resp.writeUInt32(static_cast<uint32_t>(nowUs() / 1000u));
            socket_->send(resp);
            if (++timeSyncs_ <= 2) PSP_LOGI(tag::Net, "time_sync counter=%u", static_cast<unsigned>(counter));
            break;
        }
        // --- M10a: objects around the player -----------------------------
        case LogicalOpcode::SMSG_UPDATE_OBJECT:
            if (state_ == State::InWorld || state_ == State::WaitWorld) handleUpdateObject(packet);
            break;
        case LogicalOpcode::SMSG_COMPRESSED_UPDATE_OBJECT: {
            // upstream EntityController::handleCompressedUpdateObject
            if (packet.getSize() < 4) break;
            const uint32_t size = packet.readUInt32();
            if (size == 0 || size > 5u * 1024u * 1024u) break;
            std::vector<uint8_t> out(size);
            uLongf outLen = size;
            if (uncompress(out.data(), &outLen, packet.getData().data() + packet.getReadPos(),
                           packet.getRemainingSize()) != Z_OK)
                break;
            out.resize(outLen);
            network::Packet inflated(game::wireOpcode(LogicalOpcode::SMSG_UPDATE_OBJECT), out);
            handleUpdateObject(inflated);
            break;
        }
        case LogicalOpcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE: {  // taken by handleInventoryPacket
            game::ItemQueryResponseData d;
            if (game::ItemQueryResponseParser::parse(packet, d) && d.entry != 0) {
                itemInfo_[d.entry] = ItemInfo{d.displayInfoId, static_cast<uint8_t>(d.inventoryType)};
                ++entityVersion_;  // looks may change
            }
            break;
        }
        case LogicalOpcode::SMSG_DESTROY_OBJECT: {
            game::DestroyObjectData d;
            if (game::DestroyObjectParser::parse(packet, d)) removeEntity(d.guid);
            break;
        }
        case LogicalOpcode::SMSG_NAME_QUERY_RESPONSE: {
            game::NameQueryResponseData d;
            if (game::NameQueryResponseParser::parse(packet, d) && d.isValid()) {
                names_[d.guid] = d.name;
                if (auto e = entities_.getEntity(d.guid))
                    if (auto* u = dynamic_cast<game::Unit*>(e.get())) u->setName(d.name);
            }
            break;
        }
        case LogicalOpcode::MSG_MOVE_STOP:
        case LogicalOpcode::MSG_MOVE_STOP_STRAFE:
        case LogicalOpcode::MSG_MOVE_STOP_TURN:
        case LogicalOpcode::MSG_MOVE_STOP_SWIM:
        case LogicalOpcode::MSG_MOVE_FALL_LAND:
            if (state_ == State::InWorld) handleOtherMovement(packet, true);
            break;
        case LogicalOpcode::MSG_MOVE_JUMP:
            if (state_ == State::InWorld) handleOtherMovement(packet, false, true);
            break;
        case LogicalOpcode::SMSG_INIT_WORLD_STATES:
            // uint32 map, uint32 zone, uint32 area (3.3.5), then the states.
            if (state_ == State::InWorld && packet.hasRemaining(8)) {
                packet.readUInt32();
                const uint32_t zone = packet.readUInt32();
                if (zone != worldStateZone_) PSP_LOGI(tag::Net, "world_state_zone zone=%u", static_cast<unsigned>(zone));
                worldStateZone_ = zone;
            }
            break;
        case LogicalOpcode::SMSG_WEATHER:
            // upstream's read - uint32 type, float intensity (+ uint8 isAbrupt on 2.4.3 / 3.3.5a).
            // PSP: a non-finite intensity reads as 0 (a NaN compare traps on the Allegrex FPU).
            if (packet.hasRemaining(8)) {
                weatherType_ = packet.readUInt32();
                float intensity = packet.readFloat();
                uint32_t bits;
                std::memcpy(&bits, &intensity, sizeof(bits));
                if ((bits & 0x7F800000u) == 0x7F800000u) intensity = 0.0f;
                weatherIntensity_ = intensity;
                weatherAbrupt_ = packet.hasRemaining(1) && packet.readUInt8() != 0;
                ++weatherVersion_;
                if (++weatherLogs_ <= 40)
                    PSP_LOGI(tag::Net, "weather type=%u intensity=%.3f abrupt=%d", static_cast<unsigned>(weatherType_),
                             static_cast<double>(weatherIntensity_), weatherAbrupt_ ? 1 : 0);
            }
            break;
        case LogicalOpcode::SMSG_EMOTE:
            // upstream ChatHandler: uint32 emote id, uint64 source guid.
            if (state_ == State::InWorld && packet.hasRemaining(12)) {
                const uint32_t emoteId = packet.readUInt32();
                const uint64_t source = packet.readUInt64();
                if (source != 0 && emotes_.size() < 32) emotes_.emplace_back(source, emoteId);
            }
            break;
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
        case LogicalOpcode::MSG_MOVE_SET_RUN_MODE:
        case LogicalOpcode::MSG_MOVE_START_PITCH_UP:
        case LogicalOpcode::MSG_MOVE_START_PITCH_DOWN:
        case LogicalOpcode::MSG_MOVE_STOP_PITCH:
        case LogicalOpcode::MSG_MOVE_SET_PITCH:
        case LogicalOpcode::MSG_MOVE_HOVER:
        case LogicalOpcode::MSG_MOVE_FEATHER_FALL:
        case LogicalOpcode::MSG_MOVE_WATER_WALK:
        case LogicalOpcode::MSG_MOVE_ROOT:
        case LogicalOpcode::MSG_MOVE_UNROOT:
            if (state_ == State::InWorld) handleOtherMovement(packet, false);
            break;
        // creature anim - NPC movement (psp_world_session_move.cpp).
        case LogicalOpcode::SMSG_MONSTER_MOVE:
        case LogicalOpcode::SMSG_COMPRESSED_MOVES:
        case LogicalOpcode::SMSG_SPLINE_MOVE_SET_WALK_MODE:
        case LogicalOpcode::SMSG_SPLINE_MOVE_SET_RUN_MODE:
        case LogicalOpcode::SMSG_SPLINE_MOVE_START_SWIM:
        case LogicalOpcode::SMSG_SPLINE_MOVE_STOP_SWIM:
        case LogicalOpcode::SMSG_SPLINE_MOVE_SET_FLYING:
        case LogicalOpcode::SMSG_SPLINE_MOVE_UNSET_FLYING:
            if (state_ == State::InWorld) handleNpcMove(*logical, packet);
            break;
        default:
            break;  // SMSG_ADDON_INFO, SMSG_CLIENTCACHE_VERSION...: not needed yet (Warden: handleWardenData above)
    }
}

} // namespace wowee::platform::psp
