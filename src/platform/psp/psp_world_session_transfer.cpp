// transfers between maps, area triggers and the instance messages of the PSP
// world session (include/platform/psp/psp_world_session.hpp). GameHandler,
// MovementHandler and SocialHandler are not built for the PSP (see psp_world_session_combat.cpp), so their steps
// are mirrored here with upstream's packet layouts and order unchanged:
//  - GameHandler SMSG_TRANSFER_PENDING (upstream reads and drops it). PSP: the stage turns Pending and the
//    LoginScreen puts that map's loading screen over the world at once, as the original client does.
//  - MovementHandler::handleNewWorld: the map 0 near-origin fix (known area-trigger destinations), the same-map
//    resurrection (no reload), the clears of the old map (entities, attackers, auto-attack, the cast, world
//    states, area triggers), MSG_MOVE_WORLDPORT_ACK at once, MSG_MOVE_STOP + MSG_MOVE_HEARTBEAT at the new place,
//    an immediate ping - then the world entry: for another map the stage turns Ready and the LoginScreen
//    rebuilds the world (WorldScreen) there and calls commitTransfer(); on the same map the screen takes the
//    new place as a teleport (takeTeleport), as upstream's same-map world entry does.
//  - GameHandler SMSG_TRANSFER_ABORTED: map, reason (+ one byte for three reasons). PSP: GlobalStrings
//    TRANSFER_ABORT_* on the red error line (upstream: addUIError + addSystemChatMessage with English texts; the
//    port's game line rule, psp_game_line.hpp: a refused transfer is a Refused line). The other refusals below
//    (instance reset failed, corpse not in the instance, raid group errors) follow the same rule.
//  - MovementHandler::loadAreaTriggerDbc / checkAreaTriggers: AreaTrigger.dbc (glue.pak "at_<map>", only the
//    map the player is on), every 0.25 s with the swept path, the post-transfer suppression and cooldown, the
//    Deeprun Tram's return portals - CMSG_AREATRIGGER (instance portals, the tram, rest areas, explore points).
//  - SocialHandler / GameHandler instance packets: SMSG_RAID_INSTANCE_MESSAGE, SMSG_INSTANCE_RESET(_FAILED),
//    SMSG_INSTANCE_SAVE_CREATED, SMSG_CORPSE_NOT_IN_INSTANCE, SMSG_RAID_GROUP_ONLY (INSTANCE_BOOT),
//    SMSG_INSTANCE_LOCK_WARNING_QUERY (INSTANCE_LOCK), SMSG_INSTANCE_DIFFICULTY. PSP: the original client's ruRU
//    lines and AzerothCore's field order (upstream's English texts, its message types and the reset-failed field
//    order differ from what the server sends: InstanceResetWarningType 1..5, SendResetInstanceFailed(reason, map)).
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_strings.hpp"
#include "rendering/psp/psp_map_format.hpp"

#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

namespace {

namespace rp = wowee::rendering::psp;

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }

constexpr float kAreaTriggerCheckInterval = 0.25f;  // upstream protocol_constants AREA_TRIGGER_CHECK_INTERVAL
constexpr uint32_t kPendingTimeoutMs = 30000;       // PSP: a pending transfer the server never finished
constexpr uint32_t kDeeprunTram = 369;

// upstream game_utils.hpp isCorruptOriginPosition (server and canonical coordinates agree on it).
bool corruptOrigin(uint32_t mapId, float x, float y, float z) {
    auto near0 = [](float v) { return v < 10.0f && v > -10.0f; };
    return mapId == 0 && near0(x) && near0(y) && near0(z);
}

// upstream movement_handler.cpp kKnownAreaTriggerDestinations / findKnownReturnAreaTrigger (server coordinates).
struct KnownDest {
    uint32_t trigger, map;
    float x, y, z, o;
};
constexpr KnownDest kKnownDests[] = {
    {2166, 0, -4838.95f, -1318.46f, 501.868f, 1.42372f},   // tram -> the dwarf capital
    {2171, 0, -8364.57f, 535.981f, 91.7969f, 2.24619f},    // tram -> the human capital
    {2173, 369, 68.3006f, 2490.91f, -4.29647f, 3.12192f},  // the human capital -> tram
    {2175, 369, 69.2542f, 10.257f, -4.29664f, 3.09832f},   // the dwarf capital -> tram
};
const KnownDest* knownDest(uint32_t trigger, uint32_t map) {
    for (const KnownDest& d : kKnownDests)
        if (d.trigger == trigger && d.map == map) return &d;
    return nullptr;
}
const KnownDest* knownDestByTrigger(uint32_t trigger) {
    for (const KnownDest& d : kKnownDests)
        if (d.trigger == trigger) return &d;
    return nullptr;
}
uint32_t knownReturnTrigger(uint32_t trigger) {
    switch (trigger) {
        case 2166: return 2175;  // tram -> dwarf capital, return via that city's portal
        case 2171: return 2173;  // tram -> human capital, return via that city's portal
        case 2173: return 2171;  // human capital -> tram, exit on that city's side
        case 2175: return 2166;  // dwarf capital -> tram, exit on that city's side
        default: return 0;
    }
}

// GlobalStrings.lua (3.3.5a) through strings.pak - the lines the original client prints for
// these packets.
constexpr CS kAbortDifficulty[3] = {CS::TRANSFER_ABORT_DIFFICULTY1, CS::TRANSFER_ABORT_DIFFICULTY2,
                                    CS::TRANSFER_ABORT_DIFFICULTY3};
constexpr CS kInstanceResetFailed[3] = {CS::INSTANCE_RESET_FAILED, CS::INSTANCE_RESET_FAILED_OFFLINE,
                                        CS::INSTANCE_RESET_FAILED_ZONING};
constexpr CS kRaidGroupErrors[4] = {CS::ERR_RAID_GROUP_LOWLEVEL, CS::ERR_RAID_GROUP_ONLY, CS::ERR_RAID_GROUP_FULL,
                                    CS::ERR_RAID_GROUP_REQUIREMENTS_UNMATCH};

// UIParent.lua SecondsToTime(seconds, noSeconds = nil, notAbbreviated = 1, maxCount = 2): D_DAYS, D_HOURS, D_MINUTES,
// D_SECONDS, TIME_UNIT_DELIMITER " ".
std::string secondsToTime(uint32_t seconds) {
    std::string out;
    int count = 0;
    auto add = [&](CS form, uint32_t v) {
        if (!out.empty()) out += ' ';
        out += text::plural(csf(form, static_cast<unsigned>(v)));
        ++count;
    };
    if (seconds >= 86400) {
        add(CS::D_DAYS, seconds / 86400);
        seconds %= 86400;
    }
    if (seconds >= 3600) {
        add(CS::D_HOURS, seconds / 3600);
        seconds %= 3600;
    }
    if (count < 2 && seconds >= 60) {
        add(CS::D_MINUTES, seconds / 60);
        seconds %= 60;
    }
    if (count < 2 && seconds > 0) add(CS::D_SECONDS, seconds);
    return out;
}

// A NUL-terminated string of the record's pool, bounded by the pool's end.
std::string poolString(const char* pool, uint32_t poolSize, uint32_t off) {
    if (off >= poolSize) return std::string();
    uint32_t end = off;
    while (end < poolSize && pool[end] != '\0') ++end;
    return std::string(pool + off, end - off);
}

// The maps looked up so far (a few per session: map entries, instance messages).
std::vector<WorldSession::MapInfo>& mapCache() {
    static std::vector<WorldSession::MapInfo> cache;
    return cache;
}

}  // namespace

// --- Map data -------------------------------------------------------------------------------------

// glue.pak "mapdirs" (tools/psp/build_glue_pak.py, include/rendering/psp/psp_map_format.hpp): one record per Map.dbc
// row; the pak is opened, the record read, both freed. Without it (an older glue.pak) the four continents, as the
// world screen's table had them before.
bool WorldSession::mapInfo(uint32_t mapId, MapInfo& out) {
    for (const MapInfo& m : mapCache())
        if (m.id == mapId) {
            out = m;
            return m.found;
        }
    MapInfo m;
    m.id = mapId;
    char path[256];
    fsPath(path, sizeof(path), "data/glue.pak");
    Pak pak;
    if (pak.open(path)) {
        if (const PakEntry* e = pak.find("mapdirs")) {
            if (void* blob = pak.load(e)) {
                const auto* b = static_cast<const uint8_t*>(blob);
                uint32_t n = 0;
                if (e->size >= 4) std::memcpy(&n, b, 4);
                const uint32_t poolAt = 4u + n * static_cast<uint32_t>(sizeof(rp::MapDirRecord));
                if (poolAt <= e->size) {
                    const char* pool = reinterpret_cast<const char*>(b + poolAt);
                    const uint32_t poolSize = e->size - poolAt;
                    for (uint32_t i = 0; i < n; ++i) {
                        rp::MapDirRecord r;
                        std::memcpy(&r, b + 4 + i * sizeof(rp::MapDirRecord), sizeof(r));
                        if (r.mapId != mapId) continue;
                        m.dir = poolString(pool, poolSize, r.dirOffset);
                        m.name = poolString(pool, poolSize, r.nameOffset);
                        m.instanceType = r.instanceType;
                        m.flags = r.flags;
                        m.expansion = r.expansion;
                        m.maxPlayers = r.maxPlayers;
                        m.found = !m.dir.empty();
                        break;
                    }
                }
                pakFree(blob);
            }
        }
        pak.close();
    }
    if (!m.found) {
        struct Builtin {
            uint32_t id;
            const char* dir;
            CS name;  // Map.dbc MapName_lang (strings.pak DBC_MAP_<id>)
        };
        static constexpr Builtin kBuiltin[] = {{0, "azeroth", CS::DBC_MAP_0},
                                               {1, "kalimdor", CS::DBC_MAP_1},
                                               {530, "expansion01", CS::DBC_MAP_530},
                                               {571, "northrend", CS::DBC_MAP_571}};
        for (const Builtin& c : kBuiltin)
            if (c.id == mapId) {
                m.dir = c.dir;
                m.name = cs(c.name);
                m.found = true;
            }
    }
    PSP_LOGI(tag::Fs, "map_info id=%u found=%d dir=%s type=%u flags=0x%02X", static_cast<unsigned>(mapId), m.found ? 1 : 0,
             m.dir.c_str(), static_cast<unsigned>(m.instanceType), static_cast<unsigned>(m.flags));
    auto& cache = mapCache();
    if (cache.size() >= 16) cache.erase(cache.begin());
    cache.push_back(m);
    out = m;
    return m.found;
}

std::string WorldSession::mapLabel(uint32_t mapId) const {
    MapInfo m;
    if (mapInfo(mapId, m) && !m.name.empty()) return m.name;
    return "#" + std::to_string(mapId);
}

// --- Packets ----------------------------------------------------------------------------------------

bool WorldSession::handleTransferPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_TRANSFER_PENDING:
            handleTransferPending(packet);
            return true;
        case LogicalOpcode::SMSG_NEW_WORLD:
            handleNewWorld(packet);
            return true;
        case LogicalOpcode::SMSG_TRANSFER_ABORTED:
            handleTransferAborted(packet);
            return true;
        case LogicalOpcode::MSG_MOVE_WORLDPORT_ACK:  // upstream: registerSkipHandler
            packet.skipAll();
            return true;
        case LogicalOpcode::SMSG_RAID_INSTANCE_MESSAGE: {
            // AzerothCore Player::SendInstanceResetWarning: type, map, difficulty, seconds left; RAID_INSTANCE_WELCOME
            // adds locked + extended. Types (InstanceResetWarningType): 1 hours, 2 minutes, 3 minutes (leave the zone),
            // 4 welcome, 5 expired. PSP: the original's lines (upstream reads 1 / 2 / 3 as minutes / saved / welcome).
            if (!packet.hasRemaining(12)) {
                packet.skipAll();
                return true;
            }
            const uint32_t type = packet.readUInt32();
            const uint32_t mapId = packet.readUInt32();
            packet.readUInt32();  // difficulty
            const uint32_t secs = packet.hasRemaining(4) ? packet.readUInt32() : 0u;
            const uint8_t locked = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            const uint8_t extended = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            packet.skipAll();
            const std::string name = mapLabel(mapId);
            std::string buf;
            switch (type) {
                case 1:  // RAID_INSTANCE_WARNING_HOURS (|4 plural markup)
                    buf = text::plural(CLIENT_FMT(RAID_INSTANCE_WARNING_HOURS, name, static_cast<int>(secs / 3600u)));
                    break;
                case 2:
                case 3: {
                    const int m = static_cast<int>(secs / 60u);
                    buf = text::plural(type == 2 ? CLIENT_FMT(RAID_INSTANCE_WARNING_MIN, name, m)
                                                 : CLIENT_FMT(RAID_INSTANCE_WARNING_MIN_SOON, name, m));
                    break;
                }
                case 4:  // UIParent RAID_INSTANCE_WELCOME (DEFAULT_CHAT_FRAME, ChatTypeInfo SYSTEM)
                    if (!locked) buf = CLIENT_FMT(RAID_INSTANCE_WELCOME, name, secondsToTime(secs));
                    else if (secs == 0) buf = CLIENT_FMT(RAID_INSTANCE_WELCOME_EXTENDED, name);
                    else if (!extended) buf = CLIENT_FMT(RAID_INSTANCE_WELCOME_LOCKED, name, secondsToTime(secs));
                    else buf = CLIENT_FMT(RAID_INSTANCE_WELCOME_LOCKED_EXTENDED, name, secondsToTime(secs));
                    break;
                case 5: buf = CLIENT_FMT(RAID_INSTANCE_EXPIRED, name); break;
                default: break;
            }
            if (!buf.empty()) addSystemChatMessage(buf);
            PSP_LOGI(tag::Net, "raid_instance_message type=%u map=%u secs=%u locked=%u", static_cast<unsigned>(type),
                     static_cast<unsigned>(mapId), static_cast<unsigned>(secs), static_cast<unsigned>(locked));
            return true;
        }
        case LogicalOpcode::SMSG_INSTANCE_RESET: {  // upstream SocialHandler: the map; INSTANCE_RESET_SUCCESS
            if (!packet.hasRemaining(4)) return true;
            const uint32_t mapId = packet.readUInt32();
            addSystemChatMessage(CLIENT_FMT(INSTANCE_RESET_SUCCESS, mapLabel(mapId)));
            PSP_LOGI(tag::Net, "instance_reset map=%u", static_cast<unsigned>(mapId));
            return true;
        }
        case LogicalOpcode::SMSG_INSTANCE_RESET_FAILED: {
            // AzerothCore Player::SendResetInstanceFailed: reason (0 players inside, 1 offline, 2 zoning), then the map
            // (upstream reads the map first). Upstream: addUIError + addSystemChatMessage; the port: a Refused line.
            if (!packet.hasRemaining(8)) {
                packet.skipAll();
                return true;
            }
            const uint32_t reason = packet.readUInt32();
            const uint32_t mapId = packet.readUInt32();
            gameLine(GameLine::Refused, csf(kInstanceResetFailed[std::min<uint32_t>(reason, 2u)], mapLabel(mapId)));
            PSP_LOGI(tag::Net, "instance_reset_failed map=%u reason=%u", static_cast<unsigned>(mapId),
                     static_cast<unsigned>(reason));
            return true;
        }
        case LogicalOpcode::SMSG_INSTANCE_SAVE_CREATED:  // upstream: a chat line; the original's INSTANCE_SAVED
            packet.skipAll();
            addSystemChatMessage(CLIENT_STR(INSTANCE_SAVED));
            return true;
        case LogicalOpcode::SMSG_CORPSE_NOT_IN_INSTANCE:  // upstream: addUIError + chat; the port: a Refused line
            gameLine(GameLine::Refused, CLIENT_STR(ERR_CORPSE_IS_NOT_IN_INSTANCE));
            return true;
        case LogicalOpcode::SMSG_RAID_GROUP_ONLY: {
            // upstream: the boot countdown (ms) + a reason; 0 stops it. The original: StaticPopup INSTANCE_BOOT with
            // INSTANCE_BOOT_TIMER (the world screen draws it from instanceBootSecondsLeft()). PSP: AzerothCore also
            // refuses a raid this way (SendRaidGroupOnlyMessage(RAID_GROUP_ERR_ONLY, 0): no timer, an error) - that
            // error is the original's ERR_RAID_GROUP_* line (upstream only stops the timer).
            uint32_t timerMs = 0, error = 0;
            if (packet.hasRemaining(8)) {
                timerMs = packet.readUInt32();
                error = packet.readUInt32();  // AzerothCore RaidGroupError: 1 low level, 2 only, 3 full, 4 requirements
            }
            packet.skipAll();
            instanceBootDeadlineMs_ = timerMs ? logUptimeMs() + timerMs : 0u;
            if (!timerMs && error >= 1 && error <= 4) gameLine(GameLine::Refused, cs(kRaidGroupErrors[error - 1]));
            PSP_LOGI(tag::Net, "instance_boot ms=%u error=%u", static_cast<unsigned>(timerMs), static_cast<unsigned>(error));
            return true;
        }
        case LogicalOpcode::SMSG_INSTANCE_LOCK_WARNING_QUERY: {
            // upstream SocialHandler: nine bytes - ms to decide, the completed encounters, previously saved. The
            // original: StaticPopup INSTANCE_LOCK (ACCEPT / INSTANCE_LEAVE) -> RespondInstanceLock.
            if (!packet.hasRemaining(9)) {
                packet.skipAll();
                return true;
            }
            const uint32_t ms = packet.readUInt32();
            instanceLockMask_ = packet.readUInt32();
            instanceLockSaved_ = packet.readUInt8() != 0;
            instanceLockDeadlineMs_ = logUptimeMs() + std::max<uint32_t>(ms, 1u);
            PSP_LOGI(tag::Net, "instance_lock ms=%u mask=0x%X saved=%d", static_cast<unsigned>(ms),
                     static_cast<unsigned>(instanceLockMask_), instanceLockSaved_ ? 1 : 0);
            return true;
        }
        case LogicalOpcode::SMSG_INSTANCE_DIFFICULTY:  // upstream handleInstanceDifficulty: the difficulty (+ dynamic)
            if (packet.hasRemaining(4)) instanceDifficulty_ = packet.readUInt32();
            packet.skipAll();
            return true;
        case LogicalOpcode::SMSG_UPDATE_LAST_INSTANCE:  // upstream: skipped
        case LogicalOpcode::SMSG_UPDATE_INSTANCE_OWNERSHIP:
            packet.skipAll();
            return true;
        default:
            return false;
    }
}

// GameHandler: SMSG_TRANSFER_PENDING = the map (+ the transport's entry and map when on one).
void WorldSession::handleTransferPending(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    const uint32_t mapId = packet.readUInt32();
    transferTransportEntry_ = 0;
    transferTransportMap_ = 0;
    if (packet.hasRemaining(8)) {
        transferTransportEntry_ = packet.readUInt32();
        transferTransportMap_ = packet.readUInt32();
    }
    packet.skipAll();
    if (transferStage_ == TransferStage::Ready) return;  // the last one is not built yet: it shows its own screen
    transferStage_ = TransferStage::Pending;
    transferMapId_ = mapId;
    transferPendingMs_ = logUptimeMs();
    PSP_LOGI(tag::Net, "transfer_pending map=%u from=%u transport=%u/%u", static_cast<unsigned>(mapId),
             static_cast<unsigned>(worldEntry_.mapId), static_cast<unsigned>(transferTransportEntry_),
             static_cast<unsigned>(transferTransportMap_));
    logFlush();
}

// GameHandler SMSG_TRANSFER_ABORTED: the map, a reason, and one more byte for exactly three reasons (the expansion
// level, the difficulty, the unique message). AzerothCore TransferAbortReason.
void WorldSession::handleTransferAborted(network::Packet& packet) {
    if (!packet.hasRemaining(4)) {
        packet.skipAll();
        return;
    }
    const uint32_t mapId = packet.readUInt32();
    const uint8_t reason = packet.hasRemaining(1) ? packet.readUInt8() : 0;
    const uint8_t arg = packet.hasRemaining(1) ? packet.readUInt8() : 0;
    packet.skipAll();
    std::string text;
    switch (reason) {
        case 0x02: text = CLIENT_STR(TRANSFER_ABORT_MAX_PLAYERS); break;
        case 0x03:
        case 0x0C:
        case 0x0D:
        case 0x0E: text = CLIENT_STR(TRANSFER_ABORT_NOT_FOUND); break;
        case 0x04: text = CLIENT_STR(TRANSFER_ABORT_TOO_MANY_INSTANCES); break;
        case 0x06: text = CLIENT_STR(TRANSFER_ABORT_ZONE_IN_COMBAT); break;
        case 0x07:
            text = arg >= 2 ? CLIENT_STR(TRANSFER_ABORT_INSUF_EXPAN_LVL2) : CLIENT_STR(TRANSFER_ABORT_INSUF_EXPAN_LVL1);
            break;
        case 0x08: text = csf(kAbortDifficulty[std::min<uint32_t>(arg, 2u)], mapLabel(mapId)); break;
        case 0x09: text = CLIENT_STR(TRANSFER_ABORT_UNIQUE_MESSAGE1); break;
        case 0x0A: text = CLIENT_STR(TRANSFER_ABORT_TOO_MANY_REALM_INSTANCES); break;
        case 0x0B: text = CLIENT_STR(TRANSFER_ABORT_NEED_GROUP); break;
        case 0x0F: text = CLIENT_STR(TRANSFER_ABORT_REALM_ONLY); break;
        case 0x10: text = CLIENT_STR(TRANSFER_ABORT_MAP_NOT_ALLOWED); break;
        default: text = OWN_STR(TRANSFER_ABORTED); break;  // PSP: TRANSFER_ABORT_ERROR / unknown - no line in GlobalStrings
    }
    gameLine(GameLine::Refused, text);
    if (transferStage_ == TransferStage::Pending) {
        transferStage_ = TransferStage::None;  // the LoginScreen drops the loading screen: back in the world
        transferTransportEntry_ = 0;
    }
    PSP_LOGW(tag::Net, "transfer_aborted map=%u reason=0x%02X arg=%u", static_cast<unsigned>(mapId),
             static_cast<unsigned>(reason), static_cast<unsigned>(arg));
    logFlush();
}

// upstream handleNewWorld's clears, before the worldport ACK: every unit / player of the old map despawns (the
// server sends the new map's after the ACK), attackers, auto-attack and the cast stop, world states go.
// PSP: also the windows of the old map's NPCs and objects (loot, gossip, quests, vendor, trainer, taxi, bank,
// mailbox, auction house, stable, ItemTextFrame) - local only, the server dropped those
// interactions with the teleport; the world screen that showed them is rebuilt (upstream's frames close on their
// own events).
void WorldSession::transferResetWorld() {
    std::vector<uint64_t> guids;
    guids.reserve(entities_.getEntities().size());
    for (const auto& [guid, e] : entities_.getEntities()) guids.push_back(guid);
    for (uint64_t g : guids) removeEntity(g);
    unitAuras_.clear();      // upstream unitAurasCache clear
    unitCastStates_.clear(); // upstream clearUnitCastStates
    hostileAttackers_.clear();
    stopAutoAttack();
    clearOwnCast();          // upstream resetCastState
    emotes_.clear();
    worldStateZone_ = 0;
    teleportPending_ = false;
    resOffer_.pending = false;  // upstream resurrectPending / resurrectRequestPending
    targetGuid_ = 0;            // the target is one of the despawned units
    currentMountDisplayId_ = 0; // upstream currentMountDisplayId = 0 (+ mountCallback(0)); the field comes again
    dismountGraceRemaining_ = 0.0f;
    if (lootWindowOpen_) handleLootReleaseResponse();
    localLootState_.clear();
    lootable_.clear();
    closeGossip();
    questDetailsOpen_ = questRequestItemsOpen_ = questOfferRewardOpen_ = false;
    closeVendor();
    npcQuestStatus_.clear();
    closeTrainer();
    forceClearTaxi("new_world");  // upstream: onTaxiFlight_ / taxiMountActive_ / taxiClientActive_ ... = false
    closeTaxi();
    releaseTaxiData();            // the next taxi window reads the new map's nodes
    closeBank();
    closeMailbox();
    mailboxes_.clear();
    // the auction house, the stable and ItemTextFrame close too - their own range checks
    // (auctionUpdate / petsUpdate / gameObjectUseUpdate) need the NPC or the object, and the despawn above took it,
    // so they stayed open on the new map (the auction's sell slot goes with closeAuctionHouse; cleared again in case
    // the house was already shut).
    closeAuctionHouse();
    clearAuctionSellItem();
    stableClose();
    closeItemText();
    transportResetWorld();  // the old map's transports and riders
    flyingResetWorld();     // the toggles come again for the new map
    classBarsResetWorld();  // the totems, the vehicle, the mover of the old map
    // the old map's game objects (they are not entities - the despawn above missed them): drawn,
    // highlighted and used (CMSG_GAMEOBJ_USE) on the new map otherwise. As transportResetWorld.
    gameObjects_.clear();
    ++goVersion_;
    instanceBootDeadlineMs_ = 0;
    instanceLockDeadlineMs_ = 0;
    ++entityVersion_;
}

// upstream: MSG_MOVE_STOP + MSG_MOVE_HEARTBEAT right after the ACK, at the new place (movementInfo was set first,
// no movement flags) - so the server does not keep the old coordinates.
void WorldSession::transferSendArrival(const glm::vec3& pos, float orientation) {
    if (!socket_) return;
    if (transportArrival(pos, orientation)) return;  // riding - the place is on the transport
    game::MovementInfo mi;
    mi.flags = 0;
    mi.x = pos.x;
    mi.y = pos.y;
    mi.z = pos.z;
    mi.orientation = orientation;
    mi.time = static_cast<uint32_t>(nowUs() / 1000u);
    lastOwnMove_ = mi;
    socket_->send(game::MovementPacket::build(game::Opcode::MSG_MOVE_STOP, mi, playerGuid_));
    socket_->send(game::MovementPacket::build(game::Opcode::MSG_MOVE_HEARTBEAT, mi, playerGuid_));
}

void WorldSession::handleNewWorld(network::Packet& packet) {
    if (!packet.hasRemaining(20)) {
        PSP_LOGW(tag::Net, "new_world short size=%u", static_cast<unsigned>(packet.getSize()));
        return;
    }
    const uint32_t mapId = packet.readUInt32();
    float x = packet.readFloat(), y = packet.readFloat(), z = packet.readFloat();
    float o = packet.readFloat();
    // The map the client stands on: a transfer not built yet counts (two in one update).
    const uint32_t currentMap = transferStage_ == TransferStage::Ready ? transferEntry_.mapId : worldEntry_.mapId;
    // upstream: the area trigger that sent us, its known destination (the Deeprun Tram) and return portal.
    const uint32_t trigger = lastAreaTriggerId_;
    lastAreaTriggerId_ = 0;
    postTransferReturnTrigger_ = knownReturnTrigger(trigger);
    postTransferReturnSawNear_ = false;
    const bool havePending = pendingTriggerDestValid_ && pendingTriggerDestMap_ == mapId;
    const glm::vec3 pendingPos = pendingTriggerDestPos_;
    const float pendingO = pendingTriggerDestO_;
    pendingTriggerDestValid_ = false;
    const KnownDest* known = knownDest(trigger, mapId);
    if (havePending) o = pendingO;  // upstream: the armed fallback's orientation is taken
    else if (known) o = known->o;
    if (corruptOrigin(mapId, x, y, z)) {
        // upstream: "Correcting bad SMSG_NEW_WORLD near-origin destination" from the area trigger's destination.
        if (havePending) {
            x = pendingPos.x;
            y = pendingPos.y;
            z = pendingPos.z;
        } else if (known) {
            x = known->x;
            y = known->y;
            z = known->z;
        }
        PSP_LOGW(tag::Net, "new_world near_origin trigger=%u fixed=%d", static_cast<unsigned>(trigger),
                 (havePending || known) ? 1 : 0);
    }
    const bool onTransport = transferTransportEntry_ != 0;
    const bool sameMap = mapId == currentMap;
    // upstream: "same-map resurrection - skipping world reload". PSP: the ghost stands for upstream's
    // resurrectPending_ (set when a resurrection was accepted).
    const bool resurrection = sameMap && ghostSeen_;
    if (!resurrection) transferResetWorld();
    else {
        resOffer_.pending = false;
        hostileAttackers_.clear();
        stopAutoAttack();
        clearOwnCast();
    }
    armAreaTriggers(mapId);
    if (socket_) {
        socket_->send(network::Packet(game::wireOpcode(game::Opcode::MSG_MOVE_WORLDPORT_ACK)));
        transferSendArrival(glm::vec3(x, y, z), o);
        if (!resurrection) {  // upstream: "World transfer keepalive: sending immediate ping"
            lastPingUs_ = nowUs();
            socket_->send(game::PingPacket::build(++pingSeq_, 50));
        }
    }
    ++transfersDone_;
    PSP_LOGI(tag::Net, "new_world map=%u from=%u x=%.1f y=%.1f z=%.1f o=%.2f same=%d res=%d transport=%d trigger=%u",
             static_cast<unsigned>(mapId), static_cast<unsigned>(currentMap), x, y, z, o, sameMap ? 1 : 0,
             resurrection ? 1 : 0, onTransport ? 1 : 0, static_cast<unsigned>(trigger));
    logFlush();
    if (sameMap && transferStage_ == TransferStage::Ready) {
        // A second one onto the map not built yet: the world will be built at this place.
        transferEntry_ = game::LoginVerifyWorldData{mapId, x, y, z, o};
        teleportPending_ = false;
        transferAwaitSelf_ = false;
        return;
    }
    if (sameMap) {
        // upstream WorldEntryCallbackHandler: the same map is not reloaded - the player is put there (the world screen
        // takes it as a teleport: takeTeleport -> applyTeleport, the streamer follows).
        teleportPending_ = true;
        teleportPos_ = glm::vec3(x, y, z);
        teleportO_ = o;
        if (transferStage_ == TransferStage::Pending) transferStage_ = TransferStage::None;
        transferTransportEntry_ = 0;
        return;
    }
    // Another map: the world is rebuilt there (LoginScreen), then commitTransfer().
    transferEntry_ = game::LoginVerifyWorldData{mapId, x, y, z, o};
    transferMapId_ = mapId;
    transferStage_ = TransferStage::Ready;
    // PSP: on a ship / zeppelin the place above is transport-local (upstream composes it through the transport once
    // that is known on the new map; the PSP has none) - our own create block, right after the ACK, has the world one.
    transferAwaitSelf_ = onTransport;
}

// PSP: our create block on the new map after a transfer on a transport: its movement block's place is the world one
// (server coordinates, as the other units' blocks).
void WorldSession::transferSelfBlock(const game::UpdateBlock& b) {
    if (!transferAwaitSelf_ || b.guid != playerGuid_ || !b.hasMovement) return;
    if (b.updateType != game::UpdateType::CREATE_OBJECT && b.updateType != game::UpdateType::CREATE_OBJECT2) return;
    transferEntry_.x = b.x;
    transferEntry_.y = b.y;
    transferEntry_.z = b.z;
    transferEntry_.orientation = b.orientation;
    transferAwaitSelf_ = false;
    PSP_LOGI(tag::Net, "transfer_self_place x=%.1f y=%.1f z=%.1f on_transport=%d", b.x, b.y, b.z, b.onTransport ? 1 : 0);
}

void WorldSession::commitTransfer() {
    if (transferStage_ != TransferStage::Ready) return;
    PSP_LOGI(tag::Net, "transfer_commit map=%u->%u", static_cast<unsigned>(worldEntry_.mapId),
             static_cast<unsigned>(transferEntry_.mapId));
    worldEntry_ = transferEntry_;
    transferStage_ = TransferStage::None;
    transferTransportEntry_ = 0;
    transferTransportMap_ = 0;
    transferAwaitSelf_ = false;
    armAreaTriggers(worldEntry_.mapId);
}

void WorldSession::dropPendingTransfer(const char* why) {
    if (transferStage_ != TransferStage::Pending) return;
    PSP_LOGW(tag::Net, "transfer_pending_dropped map=%u why=%s", static_cast<unsigned>(transferMapId_), why);
    transferStage_ = TransferStage::None;
    transferTransportEntry_ = 0;
}

// --- Area triggers ------------------------------------------------------------------------------------

// upstream handleNewWorld / handleTeleportAck / handleLoginVerifyWorld: the triggers the player stands in after the
// move are only marked (not fired), and none fires for 10 s (1.5 s on the Deeprun Tram: its exits are close).
void WorldSession::armAreaTriggers(uint32_t mapId) {
    const bool tram = mapId == kDeeprunTram;
    activeAreaTriggers_.clear();
    areaTriggerCheckTimer_ = tram ? -1.0f : -5.0f;
    areaTriggerSuppressFirst_ = true;
    areaTriggerCooldown_ = tram ? 1.5f : 10.0f;
    lastTriggerCheckValid_ = false;
}

// upstream loadAreaTriggerDbc, PSP: only the rows of the map the player is on (glue.pak "at_<map>").
bool WorldSession::loadAreaTriggers(uint32_t mapId) {
    areaTriggerMap_ = mapId;
    std::vector<AreaTrigger>().swap(areaTriggers_);
    nearMissLogged_.clear();
    char path[256];
    fsPath(path, sizeof(path), "data/glue.pak");
    Pak pak;
    if (!pak.open(path)) return false;
    char name[24];
    std::snprintf(name, sizeof(name), "at_%u", static_cast<unsigned>(mapId));
    const PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob) {
        const uint32_t n = e->size / static_cast<uint32_t>(sizeof(rp::AreaTriggerRecord));
        areaTriggers_.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            rp::AreaTriggerRecord r;
            std::memcpy(&r, static_cast<const uint8_t*>(blob) + i * sizeof(r), sizeof(r));
            if (r.mapId != mapId) continue;
            AreaTrigger at;
            at.id = r.id;
            at.x = r.x;
            at.y = r.y;
            at.z = r.z;
            at.radius = r.radius;
            at.boxLength = r.boxLength;
            at.boxWidth = r.boxWidth;
            at.boxHeight = r.boxHeight;
            at.boxYaw = r.boxYaw;
            areaTriggers_.push_back(at);
        }
        pakFree(blob);
    }
    pak.close();
    PSP_LOGI(tag::Net, "area_triggers map=%u n=%u kb=%u", static_cast<unsigned>(mapId),
             static_cast<unsigned>(areaTriggers_.size()),
             static_cast<unsigned>((areaTriggers_.size() * sizeof(AreaTrigger) + 1023u) / 1024u));
    return !areaTriggers_.empty();
}

// upstream GameHandler::update (the cooldown, the 0.25 s timer) + MovementHandler::checkAreaTriggers.
void WorldSession::updateAreaTriggers(float dtSec, const glm::vec3& selfPos) {
    if (transferStage_ == TransferStage::Pending && logUptimeMs() - transferPendingMs_ > kPendingTimeoutMs)
        dropPendingTransfer("timeout");
    if (state_ != State::InWorld || !socket_ || transferStage_ == TransferStage::Ready) return;
    if (areaTriggerCooldown_ > 0.0f) areaTriggerCooldown_ -= dtSec;
    areaTriggerCheckTimer_ += dtSec;
    if (areaTriggerCheckTimer_ < kAreaTriggerCheckInterval) return;
    areaTriggerCheckTimer_ = 0.0f;
    // upstream: not during a flight.
    if (onTaxiFlight_ || taxiClientActive_) {
        lastTriggerCheckValid_ = false;
        return;
    }
    const uint32_t mapId = worldEntry_.mapId;
    if (areaTriggerMap_ != mapId) loadAreaTriggers(mapId);
    if (areaTriggers_.empty()) return;
    const float px = selfPos.x, py = selfPos.y, pz = selfPos.z;
    if (corruptOrigin(mapId, px, py, pz)) {
        if (++areaTriggerLogs_ <= 10) PSP_LOGW(tag::Net, "area_trigger_skip near_origin map=%u", static_cast<unsigned>(mapId));
        return;
    }
    // Swept-path samples between the last check and now (a mount crosses a small portal box between two checks).
    glm::vec3 samples[10];
    int sampleCount = 0;
    const glm::vec3 cur(px, py, pz);
    if (lastTriggerCheckValid_ && lastTriggerCheckMap_ == mapId) {
        const glm::vec3 d = cur - lastTriggerCheckPos_;
        const float movedSq = glm::dot(d, d);
        if (movedSq > 1.0f && movedSq < 50.0f * 50.0f) {
            const int steps = std::min(8, static_cast<int>(std::sqrt(movedSq)));
            for (int s = 1; s <= steps; ++s)
                samples[sampleCount++] = lastTriggerCheckPos_ + d * (static_cast<float>(s) / static_cast<float>(steps + 1));
        } else if (movedSq >= 50.0f * 50.0f) {
            // PSP: a same-map teleport (upstream arms in handleTeleportAck; here the place jumps).
            armAreaTriggers(mapId);
            areaTriggerCheckTimer_ = 0.0f;
        }
    }
    samples[sampleCount++] = cur;
    lastTriggerCheckPos_ = cur;
    lastTriggerCheckMap_ = mapId;
    lastTriggerCheckValid_ = true;
    const bool cooldownActive = areaTriggerCooldown_ > 0.0f;
    const bool suppressFirst = areaTriggerSuppressFirst_;
    areaTriggerSuppressFirst_ = false;
    for (const AreaTrigger& at : areaTriggers_) {
        auto inside = [&at](const glm::vec3& q) {
            if (at.radius > 0.0f) {
                const glm::vec3 d = q - glm::vec3(at.x, at.y, at.z);
                return glm::dot(d, d) <= at.radius * at.radius;
            }
            if (at.boxLength > 0.0f || at.boxWidth > 0.0f || at.boxHeight > 0.0f) {
                // upstream: the box axes are in server space; rotate the offset into the box by -yaw.
                const float dx = q.x - at.x, dy = q.y - at.y, dz = q.z - at.z;
                const float c = std::cos(-at.boxYaw), s = std::sin(-at.boxYaw);
                const float lx = dx * c - dy * s, ly = dx * s + dy * c;
                return std::fabs(lx) <= at.boxLength * 0.5f && std::fabs(ly) <= at.boxWidth * 0.5f &&
                       std::fabs(dz) <= at.boxHeight * 0.5f;
            }
            return false;
        };
        bool in = false;
        for (int s = 0; s < sampleCount && !in; ++s) in = inside(samples[s]);
        if (!in && !nearMissLogged_.count(at.id)) {
            // upstream: "AreaTrigger near miss" once per trigger (a trigger that does not fire looks like none).
            const float dx = px - at.x, dy = py - at.y;
            const float flat = std::sqrt(dx * dx + dy * dy);
            const float extent = at.radius > 0.0f ? at.radius : std::max(at.boxLength, at.boxWidth) * 0.5f;
            if (extent > 0.0f && flat <= extent + 5.0f) {
                nearMissLogged_.insert(at.id);
                if (++areaTriggerLogs_ <= 60)
                    PSP_LOGI(tag::Net, "area_trigger_near id=%u flat=%.1f dz=%.1f at=%.1f,%.1f,%.1f", static_cast<unsigned>(at.id),
                             flat, pz - at.z, at.x, at.y, at.z);
            }
        }
        if (in) {
            if (activeAreaTriggers_.count(at.id)) continue;
            const bool suppress = suppressFirst || cooldownActive || postTransferReturnTrigger_ == at.id;
            if (suppressFirst) {
                // upstream: spawned inside a portal - armed, it fires only after leaving and re-entering.
                if (postTransferReturnTrigger_ == at.id) postTransferReturnSawNear_ = true;
                activeAreaTriggers_.insert(at.id);
            } else if (postTransferReturnTrigger_ == at.id) {
                postTransferReturnSawNear_ = true;
                activeAreaTriggers_.insert(at.id);
            } else if (!cooldownActive) {
                activeAreaTriggers_.insert(at.id);
            }  // cooldown: not marked - it fires once the grace ends, if the player is still inside
            if (suppress) {
                if (++areaTriggerLogs_ <= 60)
                    PSP_LOGI(tag::Net, "area_trigger_suppressed id=%u first=%d cooldown=%.1f", static_cast<unsigned>(at.id),
                             suppressFirst ? 1 : 0, static_cast<double>(areaTriggerCooldown_));
                continue;
            }
            network::Packet pkt(game::wireOpcode(game::Opcode::CMSG_AREATRIGGER));
            pkt.writeUInt32(at.id);
            socket_->send(pkt);
            lastAreaTriggerId_ = at.id;
            if (const KnownDest* d = knownDestByTrigger(at.id)) {
                pendingTriggerDestValid_ = true;
                pendingTriggerDestMap_ = d->map;
                pendingTriggerDestPos_ = glm::vec3(d->x, d->y, d->z);
                pendingTriggerDestO_ = d->o;
            }
            PSP_LOGI(tag::Net, "area_trigger id=%u map=%u x=%.1f y=%.1f z=%.1f", static_cast<unsigned>(at.id),
                     static_cast<unsigned>(mapId), px, py, pz);
            logFlush();
            continue;
        }
        if (postTransferReturnTrigger_ == at.id) {
            // upstream: keep the paired return portal armed until the player is well clear of it.
            const glm::vec3 d = cur - glm::vec3(at.x, at.y, at.z);
            const float distSq = glm::dot(d, d);
            const float extent =
                std::max({at.radius, at.boxLength * 0.5f, at.boxWidth * 0.5f, at.boxHeight * 0.5f, 12.0f});
            const float clear = extent + 8.0f;
            if (distSq <= clear * clear) {
                postTransferReturnSawNear_ = true;
                activeAreaTriggers_.insert(at.id);
                continue;
            }
            if (!postTransferReturnSawNear_ && distSq > 1000.0f * 1000.0f && areaTriggerCooldown_ > 0.0f) {
                activeAreaTriggers_.insert(at.id);  // the destination's place has not arrived yet
                continue;
            }
            postTransferReturnTrigger_ = 0;
            postTransferReturnSawNear_ = false;
        }
        activeAreaTriggers_.erase(at.id);  // left it: may fire again on re-entry
    }
}

// --- Instance popups ------------------------------------------------------------------------------------

float WorldSession::instanceBootSecondsLeft() const {
    if (!instanceBootDeadlineMs_) return 0.0f;
    const int32_t left = static_cast<int32_t>(instanceBootDeadlineMs_ - logUptimeMs());
    return left > 0 ? static_cast<float>(left) / 1000.0f : 0.0f;
}

float WorldSession::instanceLockSecondsLeft() const {
    if (!instanceLockDeadlineMs_) return 0.0f;
    const int32_t left = static_cast<int32_t>(instanceLockDeadlineMs_ - logUptimeMs());
    return left > 0 ? static_cast<float>(left) / 1000.0f : 0.0f;
}

// RespondInstanceLock: CMSG_INSTANCE_LOCK_RESPONSE (uint8 accept; AzerothCore HandleInstanceLockResponse binds or
// sends the player back to the graveyard).
void WorldSession::respondInstanceLock(bool accept) {
    if (!instanceLockDeadlineMs_) return;
    instanceLockDeadlineMs_ = 0;
    if (state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_INSTANCE_LOCK_RESPONSE));
    p.writeUInt8(accept ? 1 : 0);
    socket_->send(p);
    PSP_LOGI(tag::Net, "instance_lock_response accept=%d", accept ? 1 : 0);
}

}  // namespace wowee::platform::psp
