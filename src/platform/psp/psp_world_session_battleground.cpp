// battlegrounds and world states for the PSP world session - upstream
// SocialHandler's battlefield half (handleBattlefieldStatus, handleBattlefieldList, joinBattlefield,
// acceptBattlefield / declineBattlefield, leaveBattlefield, requestPvpLog, handlePvpLogData,
// SMSG_BATTLEFIELD_PORT_DENIED / SMSG_GROUP_JOINED_BATTLEGROUND / SMSG_BATTLEGROUND_PLAYER_JOINED / _LEFT) and
// GameHandler's world states (SMSG_INIT_WORLD_STATES, SMSG_UPDATE_WORLD_STATE, SMSG_WORLD_STATE_UI_TIMER_UPDATE)
// without GameHandler. PSP - the 3.3.5a layouts as AzerothCore writes them where upstream differs:
//   - SMSG_BATTLEFIELD_LIST: battlemaster guid, from-where byte, type, two level bytes, the win / loss
//     rewards (+ the random battleground's), then the instances - upstream reads the pre-3.3 layout (no guid, no
//     rewards) and took the guid's low half for the type;
//   - SMSG_BATTLEFIELD_STATUS: a slot of 2+ is dropped; an empty 8-byte head (STATUS_NONE, 12 bytes in
//     all) clears the slot (upstream's parser refuses it and the queue stayed); status 3 reads the shutdown and
//     run times and the team byte (GetBattlefieldInstanceExpiration / RunTime);
//   - CMSG_BATTLEFIELD_PORT and CMSG_LEAVE_BATTLEFIELD carry the 8 bytes the status brought
//     (upstream: zeros, and an empty LEAVE that AzerothCore's read_skip rejects); accepting is sent as it is, as
//     upstream acceptBattlefield does (AzerothCore HandleBattleFieldPortOpcode refuses a fight itself and
//     resurrects a dead player on entering);
//   - MSG_PVP_LOG_DATA of an arena: both teams' rating blocks, then both names (upstream: five values and the name
//     per team);
//   - the lines: the status changes and SMSG_GROUP_JOINED_BATTLEGROUND by its result as upstream says them (the
//     screen writes them - the battleground names are in ui.pak), ERR_BG_PLAYER_JOINED_SS / _LEFT_S,
//     SMSG_BATTLEFIELD_PORT_DENIED as upstream (red line + chat);
//   - FrameXML's WorldStateAlwaysUpFrame chat filter (joins of the first 60 s said every 5 s as one line, leaves
//     dropped after the winner) and BattlefieldTimerFrame's shutdown lines run here (bgTick).
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

namespace {

// The lines: GlobalStrings (strings.pak) - ERR_BG_PLAYER_JOINED_SS / _LEFT_S (system chat);
// WorldStateAlwaysUpFrame_OnUpdate's summaries ERR_PLAYER_JOINED_BATTLE_D ...;
// BattlefieldTimerFrame_OnUpdate's BATTLEGROUND_COMPLETE_MESSAGE / ARENA_COMPLETE_MESSAGE / INSTANCE_SHUTDOWN_MESSAGE.

constexpr int kTimerThresholds[4] = {600, 300, 60, 15};  // BATTLEFIELD_TIMER_THRESHOLDS
constexpr uint32_t kTimerDelayMs = 3000;                  // BATTLEFIELD_TIMER_DELAY
constexpr uint32_t kFilterIntervalMs = 5000;              // WORLDSTATEALWAYSUPFRAME_DEFAULTINTERVAL
constexpr uint32_t kFilterFirstMs = 30000;                // TIMESINCELAST starts at -25: the first summary at 30 s
constexpr uint32_t kFilterRunMs = 60000;                  // WORLDSTATEALWAYSUPFRAME_TIMETORUN

unsigned lo32(uint64_t g) { return static_cast<unsigned>(g & 0xFFFFFFFFu); }

// SecondsToTime (UIParent.lua): abbreviated, two units at most, separated by TIME_UNIT_DELIMITER.
std::string secondsToTime(long seconds) {
    if (seconds < 0) seconds = 0;
    std::string t;
    int count = 0;
    auto add = [&](const char* f, long v) {
        if (!t.empty()) t += ' ';
        t += text::plural(text::textf(f, static_cast<int>(v)));  // enUS "%d |4Min:Min;"
        ++count;
    };
    if (seconds >= 86400) {
        add(CLIENT_STR(DAYS_ABBR), seconds / 86400);
        seconds %= 86400;
    }
    if (seconds >= 3600) {
        add(CLIENT_STR(HOURS_ABBR), seconds / 3600);
        seconds %= 3600;
    }
    if (count < 2 && seconds >= 60) {
        add(CLIENT_STR(MINUTES_ABBR), seconds / 60);
        seconds %= 60;
    }
    if (count < 2 && seconds > 0) add(CLIENT_STR(SECONDS_ABBR), seconds);
    return t;
}

// The patterns are the client's text of the running language (strings.pak): textf, not snprintf.
std::string fmtNum(const char* f, size_t n) { return text::plural(text::textf(f, static_cast<int>(n))); }

std::string fmtNumList(const char* f, size_t n, const std::string& list) {
    return text::plural(text::textf(f, static_cast<int>(n), list));
}

}  // namespace

// --- Queues ---------------------------------------------------------------------------------------

const WorldSession::BgQueue& WorldSession::bgQueue(int slot) const {
    static const BgQueue kNone{};
    return slot >= 0 && slot < kBgQueueSlots ? bgQueues_[slot] : kNone;
}

bool WorldSession::takeBgEvent(BgEvent& out) {
    if (bgEvents_.empty()) return false;
    out = std::move(bgEvents_.front());
    bgEvents_.pop_front();
    return true;
}

bool WorldSession::bgPlayerLook(uint64_t guid, uint8_t& race, uint8_t& gender, uint8_t& cls) const {
    auto it = bgLooks_.find(guid);
    if (it == bgLooks_.end() || it->second.race == 0) return partyMemberLook(guid, race, gender, cls);
    race = it->second.race;
    gender = it->second.gender;
    cls = it->second.cls;
    return true;
}

void WorldSession::battlemasterHello(uint64_t guid) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_BATTLEMASTER_HELLO));
    p.writeUInt64(guid);
    socket_->send(p);
    PSP_LOGI(tag::Net, "bg_hello guid=%08X", lo32(guid));
}

// upstream joinBattlefield: CMSG_BATTLEMASTER_JOIN (the battlemaster, the type, the instance - 0 for the
// first available -, the group flag); the index is GetSelectedBattlefield's (0 = first available).
void WorldSession::joinBattlefield(int index, bool asGroup) {
    if (state_ != State::InWorld || !socket_ || bgList_.bgTypeId == 0) return;
    const uint32_t instance =
        index > 0 && static_cast<size_t>(index) <= bgList_.instances.size() ? bgList_.instances[index - 1] : 0;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_BATTLEMASTER_JOIN));
    p.writeUInt64(bgList_.battlemaster);
    p.writeUInt32(bgList_.bgTypeId);
    p.writeUInt32(instance);
    p.writeUInt8(asGroup ? 1 : 0);
    socket_->send(p);
    PSP_LOGI(tag::Net, "bg_join type=%u instance=%u group=%d", static_cast<unsigned>(bgList_.bgTypeId),
             static_cast<unsigned>(instance), asGroup ? 1 : 0);
}

// AcceptBattlefieldPort: the packet repeats the status' 8 bytes; no check of our own (upstream acceptBattlefield:
// the realm decides). upstream acceptBattlefield / declineBattlefield: the slot taken as entered / dropped at once.
void WorldSession::acceptBattlefieldPort(int slot, bool accept) {
    if (state_ != State::InWorld || !socket_ || slot < 0 || slot >= kBgQueueSlots) return;
    BgQueue& q = bgQueues_[slot];
    if (q.status == 0) return;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_BATTLEFIELD_PORT));
    for (uint8_t b : q.raw) p.writeUInt8(b);
    p.writeUInt8(accept ? 1 : 0);
    socket_->send(p);
    PSP_LOGI(tag::Net, "bg_port slot=%d type=%u accept=%d", slot, static_cast<unsigned>(q.bgTypeId), accept ? 1 : 0);
    if (accept) {
        q.status = 3;  // upstream acceptBattlefield
    } else {
        q = BgQueue{};  // upstream declineBattlefield
    }
    q.portExpireMs = 0;
    ++q.seq;
    bgStatusSlot_ = slot;
    ++bgStatusSeq_;
}

// LeaveBattlefield: the active slot's 8 bytes (zeros without one).
void WorldSession::leaveBattlefield() {
    if (state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_LEAVE_BATTLEFIELD));
    const BgQueue& q = bgQueue(activeBgSlot_);
    for (uint8_t b : q.raw) p.writeUInt8(b);
    socket_->send(p);
    PSP_LOGI(tag::Net, "bg_leave slot=%d type=%u", activeBgSlot_, static_cast<unsigned>(q.bgTypeId));
}

void WorldSession::requestPvpLog() {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(network::Packet(game::wireOpcode(game::Opcode::MSG_PVP_LOG_DATA)));
}

// --- World states -------------------------------------------------------------------------------------

uint32_t WorldSession::worldState(uint32_t key, bool* found) const {
    auto it = worldStates_.find(key);
    if (found) *found = it != worldStates_.end();
    return it != worldStates_.end() ? it->second : 0;
}

bool WorldSession::worldStateServerTime(uint32_t& unixTime, uint32_t& receivedMs) const {
    if (!wsServerTimeMs_) return false;
    unixTime = wsServerTime_;
    receivedMs = wsServerTimeMs_;
    return true;
}

void WorldSession::requestWorldStateTimer() {
    if (state_ != State::InWorld || !socket_) return;
    const uint32_t now = logUptimeMs();
    if (wsTimerAskMs_ && now - wsTimerAskMs_ < 10000u) return;
    wsTimerAskMs_ = now | 1u;
    socket_->send(network::Packet(game::wireOpcode(game::Opcode::CMSG_WORLD_STATE_UI_TIMER_UPDATE)));
}

// upstream SMSG_INIT_WORLD_STATES (map, zone, area, the pairs - the states replaced); on a copy: the zone still
// reaches onPacket's switch (worldStateZone_).
void WorldSession::handleInitWorldStates(network::Packet packet) {
    if (!packet.hasRemaining(10)) return;
    wsMap_ = packet.readUInt32();
    packet.readUInt32();  // zone (onPacket)
    if (packet.getRemainingSize() >= 6) wsArea_ = packet.readUInt32();
    uint16_t count = packet.readUInt16();
    const size_t available = packet.getRemainingSize();
    if (available < static_cast<size_t>(count) * 8u) {
        if (available % 8u != 0) return;  // upstream: tolerant of a short count, not of a torn pair
        count = static_cast<uint16_t>(available / 8u);
    }
    worldStates_.clear();
    worldStates_.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t key = packet.readUInt32();
        worldStates_[key] = packet.readUInt32();
    }
    ++wsVersion_;
    if (++bgLogs_ <= 80)
        PSP_LOGI(tag::Net, "world_states map=%u area=%u n=%u", static_cast<unsigned>(wsMap_), static_cast<unsigned>(wsArea_),
                 static_cast<unsigned>(count));
}

// --- Battleground packets ------------------------------------------------------------------------------

// SMSG_BATTLEFIELD_STATUS (upstream BattlefieldStatusPacket for the queued / invited heads).
void WorldSession::handleBattlefieldStatus(network::Packet& packet) {
    if (!packet.hasRemaining(12)) return;
    const uint32_t slotId = packet.readUInt32();
    if (slotId >= static_cast<uint32_t>(kBgQueueSlots)) {
        packet.skipAll();
        return;
    }
    const int slot = static_cast<int>(slotId);
    BgQueue& q = bgQueues_[slot];
    const uint32_t prevStatus = q.status;
    uint8_t raw[8];
    for (uint8_t& b : raw) b = packet.readUInt8();
    const uint32_t now = logUptimeMs();
    const uint32_t seq = q.seq + 1;
    bool zero = true;
    for (uint8_t b : raw) zero = zero && b == 0;
    if (zero) {  // STATUS_NONE: the slot cleared (and the active battlefield with it)
        q = BgQueue{};
        if (activeBgSlot_ == slot) {
            activeBgSlot_ = -1;
            bgExpireMs_ = bgStartMs_ = 0;
            bgFilterStop();
        }
    } else {
        if (!packet.hasRemaining(11)) return;
        std::memcpy(q.raw, raw, 8);
        q.arenaType = raw[0] & 0x7F;
        q.bgTypeId = static_cast<uint32_t>(raw[2]) | (static_cast<uint32_t>(raw[3]) << 8) |
                     (static_cast<uint32_t>(raw[4]) << 16) | (static_cast<uint32_t>(raw[5]) << 24);
        q.minLevel = packet.readUInt8();
        q.maxLevel = packet.readUInt8();
        q.instanceId = packet.readUInt32();
        q.rated = packet.readUInt8() != 0;
        q.status = packet.readUInt32();
        q.portExpireMs = 0;
        if (q.status == 2 && packet.hasRemaining(16)) {
            q.mapId = packet.readUInt32();
            packet.readUInt64();
            const uint32_t timeout = packet.readUInt32();
            q.portExpireMs = timeout ? (now + timeout) | 1u : 0;
        }
        if (q.status == 3 && packet.hasRemaining(21)) {
            q.mapId = packet.readUInt32();
            bgActiveMap_ = q.mapId;
            packet.readUInt64();
            const uint32_t time1 = packet.readUInt32();  // to the auto leave (0 at the start, 120000 after the end)
            const uint32_t time2 = packet.readUInt32();  // since the start
            bgTeam_ = packet.readUInt8();
            bgExpireMs_ = time1 ? (now + time1) | 1u : 0;
            bgStartMs_ = (now - time2) | 1u;
            activeBgSlot_ = slot;
            bgTimerThreshold_ = 0;  // BattlefieldFrame_UpdateStatus: index 1, previous mod 0
            bgTimerPrevMod_ = 0;
            if (prevStatus != 3) bgFilterStart();  // PLAYER_ENTERING_BATTLEGROUND
        } else if (activeBgSlot_ == slot) {
            activeBgSlot_ = -1;
            bgExpireMs_ = bgStartMs_ = 0;
            bgFilterStop();
        }
        if (q.status == 1 && packet.hasRemaining(8)) {
            q.avgWaitMs = packet.readUInt32();
            q.queuedSinceMs = (now - packet.readUInt32()) | 1u;
        } else {
            q.avgWaitMs = 0;
            q.queuedSinceMs = 0;
        }
    }
    q.seq = seq;
    bgStatusSlot_ = slot;
    ++bgStatusSeq_;
    if (++bgLogs_ <= 80 || q.status != prevStatus)
        PSP_LOGI(tag::Net, "bg_status slot=%d status=%u type=%u arena=%u instance=%u map=%u expire_ms=%u", slot,
                 static_cast<unsigned>(q.status), static_cast<unsigned>(q.bgTypeId), static_cast<unsigned>(q.arenaType),
                 static_cast<unsigned>(q.instanceId), static_cast<unsigned>(q.mapId),
                 static_cast<unsigned>(q.portExpireMs ? q.portExpireMs - now : 0));
}

// the battlemaster's list (BATTLEFIELDS_SHOW for a battlemaster, fromWhere 0).
void WorldSession::handleBattlefieldList(network::Packet& packet) {
    if (!packet.hasRemaining(8 + 1 + 4 + 2 + 1 + 12 + 1)) return;
    BgList l;
    l.battlemaster = packet.readUInt64();
    l.fromWhere = packet.readUInt8();
    l.bgTypeId = packet.readUInt32();
    l.minLevel = packet.readUInt8();
    l.maxLevel = packet.readUInt8();
    l.hasWin = packet.readUInt8() != 0;
    l.winHonor = packet.readUInt32();
    l.winArena = packet.readUInt32();
    l.lossHonor = packet.readUInt32();
    l.isRandom = packet.readUInt8() != 0;
    if (l.isRandom) {
        if (!packet.hasRemaining(13)) return;
        l.hasWinRandom = packet.readUInt8() != 0;
        l.winHonorRandom = packet.readUInt32();
        l.winArenaRandom = packet.readUInt32();
        l.lossHonorRandom = packet.readUInt32();
    }
    if (!packet.hasRemaining(4)) return;
    const uint32_t count = std::min<uint32_t>(packet.readUInt32(), 256u);
    l.instances.reserve(count);
    for (uint32_t i = 0; i < count && packet.hasRemaining(4); ++i) l.instances.push_back(packet.readUInt32());
    bgList_ = std::move(l);
    ++bgListSeq_;
    PSP_LOGI(tag::Net, "bg_list type=%u from=%u instances=%u random=%d win=%u loss=%u",
             static_cast<unsigned>(bgList_.bgTypeId), static_cast<unsigned>(bgList_.fromWhere),
             static_cast<unsigned>(bgList_.instances.size()), bgList_.isRandom ? 1 : 0,
             static_cast<unsigned>(bgList_.winHonor), static_cast<unsigned>(bgList_.lossHonor));
}

// upstream handlePvpLogData (the battleground / arena rows as AzerothCore writes them); PSP: the players' names,
// races and classes from their name queries (GetBattlefieldScore reads the name cache).
void WorldSession::handlePvpLogData(network::Packet& packet) {
    auto remaining = [&]() { return packet.getRemainingSize(); };
    if (remaining() < 1) return;
    game::BgScoreboardData sb;
    sb.isArena = packet.readUInt8() != 0;
    if (sb.isArena) {
        // PSP: the 3.3.5a layout (AzerothCore / TrinityCore BuildPvPLogDataPacket): both teams' rating blocks (old /
        // lost, new / won, matchmaker rating), then both names - upstream read five values and the name per team.
        for (auto& team : sb.arenaTeams) {
            if (remaining() < 12) return;
            team.ratingChange = packet.readUInt32();
            team.newRating = packet.readUInt32();
            packet.readUInt32();  // matchmaker rating (GetBattlefieldTeamInfo's teamSkill)
        }
        for (auto& team : sb.arenaTeams) team.teamName = remaining() > 0 ? packet.readString() : "";
    }
    if (remaining() < 1) return;
    sb.hasWinner = packet.readUInt8() != 0;
    if (sb.hasWinner) {
        if (remaining() < 1) return;
        sb.winner = packet.readUInt8();
    }
    if (remaining() < 4) return;
    const uint32_t playerCount = std::min<uint32_t>(packet.readUInt32(), 200u);
    sb.players.reserve(playerCount);
    for (uint32_t i = 0; i < playerCount && remaining() >= 12; ++i) {
        game::BgPlayerScore ps;
        ps.guid = packet.readUInt64();
        ps.killingBlows = packet.readUInt32();
        if (sb.isArena) {
            if (remaining() < 1) {
                sb.players.push_back(std::move(ps));
                break;
            }
            ps.team = packet.readUInt8();
            ps.hasTeam = true;
        } else {
            if (remaining() < 12) {
                sb.players.push_back(std::move(ps));
                break;
            }
            ps.honorableKills = packet.readUInt32();
            ps.deaths = packet.readUInt32();
            ps.bonusHonor = packet.readUInt32();
        }
        if (remaining() < 8) {
            sb.players.push_back(std::move(ps));
            break;
        }
        ps.damageDone = packet.readUInt32();
        ps.healingDone = packet.readUInt32();
        if (remaining() >= 4) {
            const uint32_t statCount = std::min<uint32_t>(packet.readUInt32(), 8u);
            for (uint32_t st = 0; st < statCount && remaining() >= 4; ++st)
                ps.bgStats.emplace_back(std::string(), packet.readUInt32());
        }
        if (const std::string* n = nameOf(ps.guid)) ps.name = *n;
        // PSP: one name query per player (his name, race, class - the score's name, class icon and faction).
        if (ps.guid != 0 && bgNameQueried_.insert(ps.guid).second && socket_ && !bgLooks_.count(ps.guid))
            socket_->send(game::NameQueryPacket::build(ps.guid));
        sb.players.push_back(std::move(ps));
    }
    bgScore_ = std::move(sb);
    ++bgScoreVersion_;
    if (++bgLogs_ <= 80)
        PSP_LOGI(tag::Net, "bg_score players=%u arena=%d winner=%d", static_cast<unsigned>(bgScore_.players.size()),
                 bgScore_.isArena ? 1 : 0, bgScore_.hasWinner ? static_cast<int>(bgScore_.winner) : -1);
}

// the result (or a BattlemasterList id) - the screen picks the line; the timed-out player's name.
void WorldSession::handleGroupJoinedBattleground(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    BgEvent e;
    e.result = static_cast<int32_t>(packet.readUInt32());
    if ((e.result == -11 || e.result == -12) && packet.hasRemaining(8)) {
        const uint64_t guid = packet.readUInt64();
        if (const std::string* n = nameOf(guid)) e.name = *n;
        if (e.name.empty()) e.result = -12;  // a name not in the cache = ERR_BATTLEGROUND_JOIN_FAILED
        else e.result = -11;
    }
    PSP_LOGI(tag::Net, "bg_group_joined result=%d", static_cast<int>(e.result));
    if (e.result == -1) return;  // ERR_BATTLEGROUND_NONE: nothing said
    bgEvents_.push_back(std::move(e));
}

// ERR_BG_PLAYER_JOINED_SS / ERR_BG_PLAYER_LEFT_S with the name (through the BG filter).
void WorldSession::handleBattlegroundPlayer(network::Packet& packet, bool joined) {
    if (!packet.hasRemaining(8)) return;
    const uint64_t guid = packet.readUInt64();
    std::string name;
    if (const std::string* n = nameOf(guid)) name = *n;
    if (name.empty()) {
        if (bgNameQueried_.insert(guid).second && socket_) socket_->send(game::NameQueryPacket::build(guid));
        return;  // a name not known yet: no line (upstream says only a cached name), the name is asked for
    }
    raidSystemLine(joined ? CLIENT_FMT(ERR_BG_PLAYER_JOINED_SS, name, name) : CLIENT_FMT(ERR_BG_PLAYER_LEFT_S, name),
                   joined, name);
}

// --- The battleground chat filter (WorldStateAlwaysUpFrame_FilterChatMsgSystem / _OnUpdate) ----------------

void WorldSession::bgFilterStart() {
    bgFilterOn_ = true;
    bgFilterStartMs_ = logUptimeMs() | 1u;
    bgFilterLastMs_ = bgFilterStartMs_;
    bgAdded_.clear();
    bgSubtracted_.clear();
}

void WorldSession::bgFilterStop() {
    bgFilterOn_ = false;
    bgAdded_.clear();
    bgSubtracted_.clear();
}

// A raid / battleground join or leave line: said, gathered (the first 60 s) or dropped (after the winner).
void WorldSession::raidSystemLine(const std::string& text, bool joined, const std::string& name) {
    if (bgFilterOn_) {
        if (bgScore_.hasWinner) {
            if (!joined) return;  // FILTERED_BG_CHAT_SUBTRACT after the battle
        } else if (logUptimeMs() - bgFilterStartMs_ < kFilterRunMs) {
            std::vector<std::string>& list = joined ? bgAdded_ : bgSubtracted_;
            if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
            return;
        }
    }
    addSystemChatMessage(text);
}

void WorldSession::bgTick() {
    const uint32_t now = logUptimeMs();
    if (bgFilterOn_ && bgFilterStartMs_) {
        const uint32_t sinceStart = now - bgFilterStartMs_;
        const bool due = sinceStart >= kFilterFirstMs && now - bgFilterLastMs_ >= kFilterIntervalMs;
        const bool last = sinceStart >= kFilterRunMs;
        if (due || last) {
            auto say = [&](std::vector<std::string>& list, bool joined) {
                if (list.empty()) return;
                std::string names;
                for (const std::string& n : list) names += (names.empty() ? "" : ", ") + n;  // PLAYER_LIST_DELIMITER
                const size_t n = list.size();
                if (n > 1 && n <= 3)
                    addSystemChatMessage(fmtNumList(
                        joined ? CLIENT_STR(ERR_PLAYERLIST_JOINED_BATTLE) : CLIENT_STR(ERR_PLAYERLIST_LEFT_BATTLE), n,
                        names));
                else if (n > 3)
                    addSystemChatMessage(fmtNum(
                        joined ? CLIENT_STR(ERR_PLAYERS_JOINED_BATTLE_D) : CLIENT_STR(ERR_PLAYERS_LEFT_BATTLE_D), n));
                else
                    addSystemChatMessage(joined ? CLIENT_FMT(ERR_PLAYER_JOINED_BATTLE_D, names)
                                                : CLIENT_FMT(ERR_PLAYER_LEFT_BATTLE_D, names));
                list.clear();
            };
            say(bgSubtracted_, false);
            say(bgAdded_, true);
            bgFilterLastMs_ = now;
            if (last) bgFilterStartMs_ = 0;  // OnUpdate off (the filter of the winner stays)
        }
    }
    // BattlefieldTimerFrame_OnUpdate: the battle closing (a winner) or emptying - at 600 / 300 / 60 / 15 s.
    if (activeBgSlot_ >= 0 && bgExpireMs_ && now - bgTimerCheckMs_ >= kTimerDelayMs) {
        bgTimerCheckMs_ = now;
        const int32_t leftMs = static_cast<int32_t>(bgExpireMs_ - now);
        const double left = leftMs > 0 ? leftMs / 1000.0 : 0.0;
        if (left > 0.0) {
            const int threshold = kTimerThresholds[bgTimerThreshold_];
            if (left < threshold && bgTimerThreshold_ != 3) {
                ++bgTimerThreshold_;
            } else {
                const int64_t mod = static_cast<int64_t>(std::floor(left / threshold));
                if (bgTimerPrevMod_ != mod) {
                    const long at = static_cast<long>(std::ceil(left / threshold) * threshold);
                    const char* f = !bgScore_.hasWinner ? CLIENT_STR(INSTANCE_SHUTDOWN_MESSAGE)
                                    : bgScore_.isArena  ? CLIENT_STR(ARENA_COMPLETE_MESSAGE)
                                                        : CLIENT_STR(BATTLEGROUND_COMPLETE_MESSAGE);
                    addSystemChatMessage(text::textf(f, secondsToTime(at)));
                    bgTimerPrevMod_ = mod;
                }
            }
        }
    }
}

bool WorldSession::handleBattlegroundPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_BATTLEFIELD_STATUS:
            handleBattlefieldStatus(packet);
            return true;
        case LogicalOpcode::SMSG_BATTLEFIELD_LIST:
            handleBattlefieldList(packet);
            return true;
        case LogicalOpcode::MSG_PVP_LOG_DATA:
            handlePvpLogData(packet);
            return true;
        case LogicalOpcode::SMSG_GROUP_JOINED_BATTLEGROUND:
            handleGroupJoinedBattleground(packet);
            return true;
        case LogicalOpcode::SMSG_BATTLEGROUND_PLAYER_JOINED:
            handleBattlegroundPlayer(packet, true);
            return true;
        case LogicalOpcode::SMSG_BATTLEGROUND_PLAYER_LEFT:
            handleBattlegroundPlayer(packet, false);
            return true;
        case LogicalOpcode::SMSG_BATTLEFIELD_PORT_DENIED:  // a refusal: the red line (GameLine; upstream shows both)
            gameLine(GameLine::Refused, OWN_STR(BG_PORT_DENIED));
            return true;
        case LogicalOpcode::MSG_BATTLEGROUND_PLAYER_POSITIONS:  // the flag carriers of the battlefield minimap: none
            packet.skipAll();
            return true;
        case LogicalOpcode::SMSG_INIT_WORLD_STATES:
            if (state_ == State::InWorld) handleInitWorldStates(packet);
            return false;  // onPacket's switch takes the zone
        case LogicalOpcode::SMSG_UPDATE_WORLD_STATE: {
            if (!packet.hasRemaining(8)) return true;
            const uint32_t key = packet.readUInt32();
            worldStates_[key] = packet.readUInt32();
            ++wsVersion_;
            return true;
        }
        case LogicalOpcode::SMSG_WORLD_STATE_UI_TIMER_UPDATE:
            if (packet.hasRemaining(4)) {
                wsServerTime_ = packet.readUInt32();
                wsServerTimeMs_ = logUptimeMs() | 1u;
            }
            return true;
        case LogicalOpcode::SMSG_NAME_QUERY_RESPONSE: {
            // PSP: peeked on a copy (the main switch keeps the name): the score players' race / gender / class.
            if (bgNameQueried_.empty()) return false;
            network::Packet copy = packet;
            game::NameQueryResponseData d;
            if (game::NameQueryResponseParser::parse(copy, d) && d.isValid() && bgNameQueried_.count(d.guid))
                bgLooks_[d.guid] = PartyLook{d.race, d.gender, d.classId};
            return false;
        }
        default:
            return false;
    }
}

}  // namespace wowee::platform::psp
