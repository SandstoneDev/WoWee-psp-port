// the raid for the PSP world session - upstream SocialHandler's raid half
// (src/game/social_handler.cpp: convertToRaid, setRaidSubgroup, swapRaidSubgroup, setGroupAssistant,
// setPartyAssignment, reportPvpAfk, setRaidMark / setRaidMarkLocally, the MSG_RAID_TARGET_UPDATE handler of
// game_handler_packets.cpp with its RaidTargetUpdateParser, initiateReadyCheck / respondToReadyCheck and the
// MSG_RAID_READY_CHECK* handlers) without GameHandler; the party half is psp_world_session_party.cpp.
// PSP:
//   - the raid's join / leave lines are said by handleGroupList (psp_world_session_party.cpp);
//   - the ready check (who may start it: a raid's leader / assistant or the party leader - ChatFrame.lua
//     /readycheck; the answers reach them only - AzerothCore Group::BroadcastReadyCheck): MSG_RAID_READY_CHECK
//     says ERR_RAID_LEADER_READY_CHECK_START_S to everyone (upstream: "X initiated a ready check!") and starts the
//     port's own 40 s timer; each answer says "X is ready" / RAID_MEMBER_NOT_READY where it arrives (upstream);
//     the initiator ends the check when everyone answered or the timer ran out (MSG_RAID_READY_CHECK_FINISHED,
//     which AzerothCore sends to the whole group); _FINISHED says upstream's "Ready check complete" counts where
//     the answers were tracked, READY_CHECK_FINISHED elsewhere; a new check simply restarts it (upstream);
//     SMSG_RAID_READY_CHECK_ERROR as upstream (red line + chat);
//   - SetRaidTarget: sent as it is (upstream setRaidMark; AzerothCore drops what it refuses), the marks set
//     locally when not in a group (upstream setRaidMarkLocally);
//   - ConvertToRaid: upstream's checks (in a group, the leader) with their red lines; the level rule is the
//     realm's (AzerothCore answers ERR_RAID_DISALLOWED_BY_LEVEL).
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cstdio>

namespace wowee::platform::psp {

namespace {

constexpr uint32_t kReadyCheckMs = 40000;     // the port's own ready check time (long enough on a PSP)
constexpr uint32_t kReadyIconsMs = 11500;     // ReadyCheck_Finish: 10 s shown + 1.5 s fade, then hidden
constexpr uint8_t kFlagAssistant = 0x01;      // MEMBER_FLAG_ASSISTANT

// The pattern is a text of the running language (strings.pak): textf, not snprintf.
std::string fmt1(const char* f, const std::string& a) { return text::textf(f, a); }

unsigned lo32(uint64_t g) { return static_cast<unsigned>(g & 0xFFFFFFFFu); }

}  // namespace

// --- Roster -----------------------------------------------------------------------------------

std::vector<WorldSession::RaidRosterEntry> WorldSession::raidRoster() const {
    std::vector<RaidRosterEntry> out;
    if (!isInRaid()) return out;
    const auto& pd = partyData_;
    auto entry = [&](uint64_t guid, const std::string& name, uint8_t sub, uint8_t flags, bool online, bool self) {
        RaidRosterEntry e;
        e.guid = guid;
        e.name = name;
        e.subGroup = sub;
        e.flags = flags;
        e.online = online;
        e.self = self;
        e.rank = guid == pd.leaderGuid ? 2 : (flags & kFlagAssistant) ? 1 : 0;
        e.master = pd.lootMethod == 2 && pd.looterGuid == guid;
        out.push_back(std::move(e));
    };
    out.reserve(pd.members.size() + 1);
    for (const auto& m : pd.members)
        if (m.guid != playerGuid_) entry(m.guid, m.name, m.subGroup, m.flags, m.isOnline != 0, false);
    const game::Character* me = activeCharacter();
    entry(playerGuid_, me ? me->name : std::string(), pd.subGroup, pd.flags, true, true);
    return out;
}

uint8_t WorldSession::raidRank(uint64_t guid) const {
    if (guid == 0 || !isInGroup()) return 0;
    if (guid == partyData_.leaderGuid) return 2;
    if (guid == playerGuid_) return (partyData_.flags & kFlagAssistant) ? 1 : 0;
    const game::GroupMember* m = partyMember(guid);
    return m && (m->flags & kFlagAssistant) ? 1 : 0;
}

bool WorldSession::isRaidOfficer() const { return isInRaid() && raidRank(playerGuid_) > 0; }

std::string WorldSession::raidName(uint64_t guid) const {
    std::string n = partyMemberName(guid);
    if (n.empty())
        for (const RaidKnown& k : raidKnown_)
            if (k.guid == guid) n = k.name;
    return n;
}

// The raid's names (raidName's fallback for the ready check's lines); handleGroupList calls this last.
void WorldSession::raidRosterUpdate() {
    if (!isInRaid()) {
        if (!raidKnown_.empty()) PSP_LOGI(tag::Net, "raid_left");
        raidKnown_.clear();
        return;
    }
    const bool wasEmpty = raidKnown_.empty();
    std::vector<RaidKnown> now;
    now.reserve(partyData_.members.size() + 1);
    for (const auto& m : partyData_.members)
        if (m.guid != playerGuid_) now.push_back(RaidKnown{m.guid, m.name});
    const game::Character* me = activeCharacter();
    now.push_back(RaidKnown{playerGuid_, me ? me->name : std::string()});
    raidKnown_ = std::move(now);
    if (++raidLogs_ <= 40)
        PSP_LOGI(tag::Net, "raid_roster members=%u joined=%d bg=%d", static_cast<unsigned>(raidKnown_.size()),
                 wasEmpty ? 1 : 0, isInBattlegroundGroup() ? 1 : 0);
}

// --- Senders (upstream SocialHandler) -------------------------------------------------------------

// upstream convertToRaid: in a group, its leader, not a raid yet (the last one silently: the button is hidden then).
void WorldSession::convertToRaid() {
    if (state_ != State::InWorld || !socket_) return;
    if (partyData_.members.empty()) {
        addUiError(CLIENT_STR(ERR_NOT_IN_GROUP));
        return;
    }
    if (!isGroupLeader()) {
        addUiError(CLIENT_STR(ERR_NOT_LEADER));
        return;
    }
    if (isInRaid()) return;
    socket_->send(game::GroupRaidConvertPacket::build());
    PSP_LOGI(tag::Net, "raid_convert_send");
}

void WorldSession::setRaidSubgroup(const std::string& name, uint8_t group) {
    if (state_ != State::InWorld || !socket_ || name.empty() || group < 1 || group > 8) return;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_GROUP_CHANGE_SUB_GROUP));
    p.writeString(name);
    p.writeUInt8(static_cast<uint8_t>(group - 1));
    socket_->send(p);
    PSP_LOGI(tag::Net, "raid_subgroup_send name=%s group=%u", name.c_str(), static_cast<unsigned>(group));
}

void WorldSession::swapRaidSubgroup(const std::string& a, const std::string& b) {
    if (state_ != State::InWorld || !socket_ || a.empty() || b.empty()) return;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_GROUP_SWAP_SUB_GROUP));
    p.writeString(a);
    p.writeString(b);
    socket_->send(p);
    PSP_LOGI(tag::Net, "raid_swap_send a=%s b=%s", a.c_str(), b.c_str());
}

void WorldSession::setGroupAssistant(uint64_t guid, bool apply) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_GROUP_ASSISTANT_LEADER));
    p.writeUInt64(guid);
    p.writeUInt8(apply ? 1 : 0);
    socket_->send(p);
    PSP_LOGI(tag::Net, "raid_assistant_send guid=%08X apply=%d", lo32(guid), apply ? 1 : 0);
}

void WorldSession::setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    network::Packet p(game::wireOpcode(game::Opcode::MSG_PARTY_ASSIGNMENT));
    p.writeUInt8(assignment);
    p.writeUInt8(apply ? 1 : 0);
    p.writeUInt64(guid);
    socket_->send(p);
    PSP_LOGI(tag::Net, "raid_assignment_send what=%u guid=%08X apply=%d", static_cast<unsigned>(assignment), lo32(guid),
             apply ? 1 : 0);
}

void WorldSession::reportPvpAfk(uint64_t guid) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    network::Packet p(game::wireOpcode(game::Opcode::CMSG_REPORT_PVP_AFK));
    p.writeUInt64(guid);
    socket_->send(p);
}

// --- Raid marks ---------------------------------------------------------------------------------------

uint8_t WorldSession::raidMarkOf(uint64_t guid) const {
    if (guid == 0) return kNoRaidMark;
    for (uint8_t i = 0; i < 8; ++i)
        if (raidMarks_[i] == guid) return i;
    return kNoRaidMark;
}

bool WorldSession::anyRaidMark() const {
    for (uint64_t g : raidMarks_)
        if (g != 0) return true;
    return false;
}

void WorldSession::clearRaidMarks() {
    if (!anyRaidMark()) return;
    raidMarks_.fill(0);
    ++raidMarksVersion_;
}

// SetRaidTarget - upstream setRaidMark for the packet, setRaidMarkLocally without a group.
void WorldSession::setRaidMark(uint64_t guid, uint8_t icon) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    uint8_t slot = icon;
    uint64_t target = guid;
    if (icon >= 8) {  // clear: the unit's own icon with an empty guid
        slot = raidMarkOf(guid);
        target = 0;
        if (slot >= 8) return;
    }
    if (isInGroup()) {
        network::Packet p(game::wireOpcode(game::Opcode::MSG_RAID_TARGET_UPDATE));
        p.writeUInt8(slot);
        p.writeUInt64(target);
        socket_->send(p);
    } else {
        // upstream setRaidMarkLocally: one icon per unit, unique.
        for (uint64_t& g : raidMarks_)
            if (g == guid) g = 0;
        raidMarks_[slot] = target;
        ++raidMarksVersion_;
    }
    PSP_LOGI(tag::Net, "raid_mark_send guid=%08X icon=%u group=%d", lo32(guid), static_cast<unsigned>(icon),
             isInGroup() ? 1 : 0);
}

// --- Ready check ---------------------------------------------------------------------------------------

WorldSession::ReadyState WorldSession::readyCheckState(uint64_t guid) const {
    for (const ReadyEntry& r : readyStates_)
        if (r.guid == guid) return r.state;
    return ReadyState::None;
}

uint32_t WorldSession::readyCheckTimeLeftMs() const {
    const uint32_t now = logUptimeMs();
    return readyCheckEndMs_ && static_cast<int32_t>(readyCheckEndMs_ - now) > 0 ? readyCheckEndMs_ - now : 0;
}

// DoReadyCheck: a raid's leader / assistant or the party leader (ChatFrame.lua /readycheck).
void WorldSession::doReadyCheck() {
    if (state_ != State::InWorld || !socket_ || !isInGroup()) return;
    if (isInRaid() ? !isRaidOfficer() : !isGroupLeader()) return;
    socket_->send(game::ReadyCheckPacket::build());
    PSP_LOGI(tag::Net, "ready_check_send");
}

// ConfirmReadyCheck (upstream respondToReadyCheck without its English line: MSG_RAID_READY_CHECK + the answer).
void WorldSession::confirmReadyCheck(bool ready) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::ReadyCheckConfirmPacket::build(ready));
    PSP_LOGI(tag::Net, "ready_check_answer ready=%d", ready ? 1 : 0);
}

// the initiator, the timer, the tracked states (the initiator counted ready), then ShowReadyCheck.
void WorldSession::handleReadyCheck(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;
    const uint64_t initiator = packet.readUInt64();
    const uint32_t now = logUptimeMs();
    readyCheckInitiator_ = initiator;
    readyCheckEndMs_ = (now + kReadyCheckMs) | 1u;
    readyCheckFinishedMs_ = 0;
    readyStates_.clear();
    // The answers come to the raid's leader / assistants and the party leader only: they keep the states.
    const bool tracks = isInRaid() ? isRaidOfficer() : isGroupLeader();
    if (tracks) {
        auto add = [&](uint64_t g) {
            readyStates_.push_back(ReadyEntry{g, g == initiator ? ReadyState::Ready : ReadyState::Waiting});
        };
        for (const auto& m : partyData_.members)
            if (m.guid != playerGuid_) add(m.guid);
        add(playerGuid_);
    }
    readyCheckInitiatorName_ = raidName(initiator);
    if (readyCheckInitiatorName_.empty())
        if (const std::string* n = nameOf(initiator)) readyCheckInitiatorName_ = *n;
    ++readyCheckSeq_;
    ++readyCheckVersion_;
    if (initiator == playerGuid_ && readyCheckInitiatorName_.empty())
        if (const game::Character* me = activeCharacter()) readyCheckInitiatorName_ = me->name;
    if (!readyCheckInitiatorName_.empty())  // upstream: "X initiated a ready check!" to everyone
        addSystemChatMessage(fmt1(CLIENT_STR(ERR_RAID_LEADER_READY_CHECK_START_S), readyCheckInitiatorName_));
    PSP_LOGI(tag::Net, "ready_check from=%08X me=%d tracks=%d n=%u", lo32(initiator), initiator == playerGuid_ ? 1 : 0,
             tracks ? 1 : 0, static_cast<unsigned>(readyStates_.size()));
}

// the answer, said where it arrives (upstream: "X is Ready / Not Ready."); all answered = the end at once.
void WorldSession::handleReadyCheckConfirm(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;
    const uint64_t guid = packet.readUInt64();
    const bool ready = packet.hasRemaining(1) && packet.readUInt8() != 0;
    bool any = false, waiting = false;
    for (ReadyEntry& r : readyStates_) {
        if (r.guid == guid) {
            r.state = ready ? ReadyState::Ready : ReadyState::NotReady;
            any = true;
        }
        waiting = waiting || r.state == ReadyState::Waiting;
    }
    PSP_LOGI(tag::Net, "ready_check_confirm guid=%08X ready=%d tracked=%d", lo32(guid), ready ? 1 : 0, any ? 1 : 0);
    if (!any) return;
    ++readyCheckVersion_;
    if (guid != playerGuid_) {
        const std::string n = raidName(guid);
        if (!n.empty()) addSystemChatMessage(ready ? OWN_FMT(READY_CHECK_MEMBER_READY, n)
                                                   : fmt1(CLIENT_STR(RAID_MEMBER_NOT_READY), n));
    }
    if (!waiting && readyCheckEndMs_) {  // everyone answered - the timer ends now
        readyCheckEndMs_ = (logUptimeMs() - 1u) | 1u;
        processReadyCheck();
    }
}

// The initiator's end of the check (the port's own rule: everyone answered or the timer ran out):
// MSG_RAID_READY_CHECK_FINISHED, which the server sends to the whole group.
void WorldSession::processReadyCheck() {
    if (readyCheckInitiator_ != playerGuid_ || !readyCheckEndMs_) return;
    if (static_cast<int32_t>(logUptimeMs() - readyCheckEndMs_) < 0) return;
    readyCheckEndMs_ = 0;
    int waiting = 0;
    for (const ReadyEntry& r : readyStates_)
        if (r.state == ReadyState::Waiting) ++waiting;
    if (socket_ && state_ == State::InWorld)
        socket_->send(network::Packet(game::wireOpcode(game::Opcode::MSG_RAID_READY_CHECK_FINISHED)));
    PSP_LOGI(tag::Net, "ready_check_end afk=%d all_ready=%d", waiting > 0 ? 1 : 0, waiting == 0 ? 1 : 0);
}

void WorldSession::raidTick() {
    processReadyCheck();
    if (readyCheckFinishedMs_ && logUptimeMs() - readyCheckFinishedMs_ > kReadyIconsMs) {
        readyCheckFinishedMs_ = 0;
        readyStates_.clear();
        ++readyCheckVersion_;
    }
}

// --- Packets ---------------------------------------------------------------------------------------

bool WorldSession::handleRaidBgPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::MSG_RAID_TARGET_UPDATE: {
            // upstream: RaidTargetUpdateParser; the full list is authoritative.
            game::RaidTargetUpdateData rtu;
            if (!game::RaidTargetUpdateParser::parse(packet, rtu)) return true;
            if (rtu.fullList) raidMarks_.fill(0);
            for (const auto& [icon, guid] : rtu.marks) {
                if (icon >= 8) continue;
                if (guid != 0)
                    for (uint64_t& g : raidMarks_)
                        if (g == guid) g = 0;  // one icon per unit (the server moves it)
                raidMarks_[icon] = guid;
            }
            ++raidMarksVersion_;
            if (++raidLogs_ <= 60)
                PSP_LOGI(tag::Net, "raid_marks full=%d n=%u", rtu.fullList ? 1 : 0, static_cast<unsigned>(rtu.marks.size()));
            return true;
        }
        case LogicalOpcode::MSG_RAID_READY_CHECK:
            handleReadyCheck(packet);
            return true;
        case LogicalOpcode::MSG_RAID_READY_CHECK_CONFIRM:
            handleReadyCheckConfirm(packet);
            return true;
        case LogicalOpcode::MSG_RAID_READY_CHECK_FINISHED: {
            // READY_CHECK_FINISHED, the timer and the initiator cleared; the icons of the
            // unanswered turn "away" (ReadyCheck_Finish) and go after 10 + 1.5 s. The line: upstream's counts
            // where the answers were tracked, READY_CHECK_FINISHED elsewhere.
            int ready = 0, notReady = 0;
            for (ReadyEntry& r : readyStates_) {
                if (r.state == ReadyState::Ready) ++ready;
                else if (r.state != ReadyState::None) ++notReady;
                if (r.state == ReadyState::Waiting) r.state = ReadyState::Afk;
            }
            if (!readyStates_.empty()) addSystemChatMessage(OWN_FMT(READY_CHECK_COMPLETE, ready, notReady));
            else if (readyCheckInitiator_ != 0) addSystemChatMessage(CLIENT_STR(READY_CHECK_FINISHED));
            readyCheckEndMs_ = 0;
            readyCheckInitiator_ = 0;
            readyCheckFinishedMs_ = logUptimeMs() | 1u;
            ++readyCheckFinishSeq_;
            ++readyCheckVersion_;
            PSP_LOGI(tag::Net, "ready_check_finished");
            return true;
        }
        case LogicalOpcode::SMSG_RAID_READY_CHECK_ERROR:  // a failure: the red line (GameLine; upstream shows both)
            packet.skipAll();
            gameLine(GameLine::Refused, OWN_STR(READY_CHECK_FAILED));
            return true;
        default:
            break;
    }
    return handleBattlegroundPacket(op, packet);
}

}  // namespace wowee::platform::psp
