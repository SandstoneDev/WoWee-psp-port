// the group for the PSP world session - upstream SocialHandler's party half
// (src/game/social_handler.cpp) without GameHandler, with upstream's packet classes unchanged
// (GroupInvitePacket, GroupInviteResponseParser, GroupAcceptPacket, GroupDeclinePacket, GroupListParser,
// PartyCommandResultParser, GroupDeclineResponseParser, GroupUninvitePacket, GroupSetLeaderPacket,
// GroupDisbandPacket):
//   - handleGroupInvite (the canAccept refusal), handleGroupDecline, handleGroupList (online state seeded
//     from the roster), SMSG_GROUP_DESTROYED / SMSG_GROUP_CANCEL, handleGroupUninvite,
//     handlePartyCommandResult, handlePartyMemberStats (the update mask, member auras into the unit aura
//     cache as upstream's unitAurasCache), handleGroupSetLeader;
//   - inviteToGroup / acceptGroupInvite / declineGroupInvite / leaveGroup (local clear at once) /
//     uninvitePlayer / promoteToLeader / setLootMethod (CMSG_LOOT_METHOD: method, master only for
//     master loot, threshold).
// PSP:
//   - the lines are said where upstream says them, in the running language (GlobalStrings keys of the same meaning
//     from strings.pak, else the port's own strings), shown by the client-wide GameLine rule (psp_game_line.hpp):
//     a refused party action on the red error line, the news (an invite, a disband, a removal, joins / leaves, loot
//     settings, a new leader) in the chat - where upstream shows both, the rule picks one. The port's own
//     additions: one line per member who joins or leaves while we stay in the group (ERR_JOINED_GROUP_S / ERR_LEFT_GROUP_S, in a
//     raid ERR_RAID_MEMBER_ADDED_S / _REMOVED_S), each loot setting that changes while we are in a group, the
//     realm's OK for an invite (ERR_INVITE_PLAYER_S: AzerothCore HandleGroupInviteOpcode answers it with the name)
//     and our own leaving (ERR_LEFT_GROUP_YOU);
//   - SMSG_NAME_QUERY_RESPONSE is peeked (not consumed) for the members' race / gender / class: the party
//     frame's portrait while a member is out of range; the members are name-queried once.
#include "platform/psp/psp_world_session.hpp"

#include "game/game_utils.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <iterator>

namespace wowee::platform::psp {

namespace {

// ERR_SET_LOOT_FREEFORALL .. _NBG by method (0 FFA, 1 round robin, 2 master, 3 group, 4 need before greed).
constexpr CS kErrSetLoot[5] = {CS::ERR_SET_LOOT_FREEFORALL, CS::ERR_SET_LOOT_ROUNDROBIN, CS::ERR_SET_LOOT_MASTER,
                               CS::ERR_SET_LOOT_GROUP, CS::ERR_SET_LOOT_NBG};
// ITEM_QUALITY0_DESC .. ITEM_QUALITY6_DESC.
constexpr CS kQualityDesc[7] = {CS::ITEM_QUALITY0_DESC, CS::ITEM_QUALITY1_DESC, CS::ITEM_QUALITY2_DESC,
                                CS::ITEM_QUALITY3_DESC, CS::ITEM_QUALITY4_DESC, CS::ITEM_QUALITY5_DESC,
                                CS::ITEM_QUALITY6_DESC};

// The pattern is a text of the running language (strings.pak): textf, not snprintf.
std::string fmt1(const char* f, const std::string& a) { return text::textf(f, a); }

std::string fmt2(const char* f, const std::string& a, const std::string& b) { return text::textf(f, a, b); }

unsigned lo32(uint64_t g) { return static_cast<unsigned>(g & 0xFFFFFFFFu); }

// format() with Lua's positional arguments (%1$s, %2$d) and plain %s / %d, the arguments given as text;
// |3-N(x) (the ruRU declension) becomes x as the PSP chat writes names.
std::string fmtArgs(const char* f, std::initializer_list<std::string> args) {
    const std::vector<std::string> a(args);
    std::string out;
    size_t next = 0;
    for (const char* p = f; *p; ++p) {
        if (*p == '%' && p[1] >= '1' && p[1] <= '9' && p[2] == '$' && (p[3] == 's' || p[3] == 'd')) {
            const size_t i = static_cast<size_t>(p[1] - '1');
            if (i < a.size()) out += a[i];
            p += 3;
        } else if (*p == '%' && (p[1] == 's' || p[1] == 'd')) {
            if (next < a.size()) out += a[next];
            ++next;
            ++p;
        } else {
            out.push_back(*p);
        }
    }
    for (size_t at; (at = out.find("|3-")) != std::string::npos;) {
        const size_t open = out.find('(', at), close = out.find(')', at);
        if (open == std::string::npos || close == std::string::npos || close < open) break;
        out = out.substr(0, at) + out.substr(open + 1, close - open - 1) + out.substr(close + 1);
    }
    return out;
}

}  // namespace

// --- Roster accessors --------------------------------------------------------------------------

const game::GroupMember* WorldSession::partyMember(uint64_t guid) const {
    if (guid == 0) return nullptr;
    for (const auto& m : partyData_.members)
        if (m.guid == guid) return &m;
    return nullptr;
}

// GetPartyMember(i): the members after the player, up to MAX_PARTY_MEMBERS (4). In a raid the party
// frames show the player's own subgroup (HIDE_PARTY_INTERFACE "0", the default).
const game::GroupMember* WorldSession::partyUnit(int index) const {
    if (index < 1 || index > 4) return nullptr;
    const bool raid = game::groupIsRaid(partyData_.groupType);
    int found = 0;
    for (const auto& m : partyData_.members) {
        if (m.guid == playerGuid_) continue;
        if (raid && m.subGroup != partyData_.subGroup) continue;
        if (++found == index) return &m;
    }
    return nullptr;
}

int WorldSession::partyUnitCount() const {
    int n = 0;
    while (n < 4 && partyUnit(n + 1)) ++n;
    return n;
}

bool WorldSession::partyMemberLook(uint64_t guid, uint8_t& race, uint8_t& gender, uint8_t& cls) const {
    auto it = partyLooks_.find(guid);
    if (it == partyLooks_.end() || it->second.race == 0) return false;
    race = it->second.race;
    gender = it->second.gender;
    cls = it->second.cls;
    return true;
}

std::string WorldSession::partyMemberName(uint64_t guid) const {
    if (guid == playerGuid_) {
        const game::Character* me = activeCharacter();
        return me ? me->name : std::string();
    }
    if (const game::GroupMember* m = partyMember(guid)) return m->name;
    if (const std::string* n = nameOf(guid)) return *n;
    return std::string();
}

void WorldSession::clearPartyData() {
    partyData_ = game::GroupListData{};
    ++partyVersion_;  // GROUP_ROSTER_UPDATE, PARTY_MEMBERS_CHANGED
    clearRaidMarks();  // the port's rule: the marks belong to the group (upstream: the server's group state)
}

// A refused party action: the red error line (GameLine::Refused).
void WorldSession::partyRefusal(const std::string& text) { gameLine(GameLine::Refused, text); }

// --- Senders (upstream SocialHandler) --------------------------------------------------------------

void WorldSession::inviteToGroup(const std::string& playerName) {
    if (state_ != State::InWorld || !socket_ || playerName.empty()) return;
    // Our own name is refused before sending (AzerothCore HandleGroupInviteOpcode drops a self-invite silently).
    if (const game::Character* me = activeCharacter(); me && me->name == playerName) {
        partyRefusal(CLIENT_STR(ERR_INVITE_SELF));
        return;
    }
    PSP_LOGI(tag::Net, "group_invite_send name=%s", playerName.c_str());
    socket_->send(game::GroupInvitePacket::build(playerName));
}

void WorldSession::acceptGroupInvite() {
    if (state_ != State::InWorld || !socket_) return;
    pendingGroupInvite_ = false;
    PSP_LOGI(tag::Net, "group_accept from=%s", pendingInviterName_.c_str());
    socket_->send(game::GroupAcceptPacket::build());
}

void WorldSession::declineGroupInvite() {
    if (state_ != State::InWorld || !socket_) return;
    pendingGroupInvite_ = false;
    PSP_LOGI(tag::Net, "group_decline from=%s", pendingInviterName_.c_str());
    socket_->send(game::GroupDeclinePacket::build());
}

// upstream leaveGroup: the packet, and the party cleared at once; the port says ERR_LEFT_GROUP_YOU (upstream's
// "You are no longer in a group." of the roster) here, since the roster that follows finds no group to leave.
void WorldSession::leaveGroup() {
    if (state_ != State::InWorld || !socket_) return;
    PSP_LOGI(tag::Net, "group_leave members=%u", static_cast<unsigned>(partyData_.members.size()));
    socket_->send(game::GroupDisbandPacket::build());
    const bool was = isInGroup();
    clearPartyData();
    if (was) addSystemChatMessage(CLIENT_STR(ERR_LEFT_GROUP_YOU));
}

void WorldSession::uninvitePlayer(const std::string& playerName) {
    if (state_ != State::InWorld || !socket_ || playerName.empty()) return;
    PSP_LOGI(tag::Net, "group_uninvite name=%s", playerName.c_str());
    socket_->send(game::GroupUninvitePacket::build(playerName));
    // No line here: the roster that drops him says ERR_LEFT_GROUP_S.
}

void WorldSession::promoteToLeader(uint64_t guid) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    PSP_LOGI(tag::Net, "group_set_leader guid=%08X", lo32(guid));
    socket_->send(game::GroupSetLeaderPacket::build(guid));
}

// upstream SocialHandler::setLootMethod: all three settings together, the master only for master loot.
void WorldSession::setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold) {
    if (state_ != State::InWorld || !socket_) return;
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_LOOT_METHOD));
    packet.writeUInt32(method);
    packet.writeUInt64(method == 2 ? masterGuid : 0);
    packet.writeUInt32(threshold);
    socket_->send(packet);
    PSP_LOGI(tag::Net, "loot_method_send method=%u threshold=%u master=%08X", static_cast<unsigned>(method),
             static_cast<unsigned>(threshold), lo32(method == 2 ? masterGuid : 0));
}

// --- Packets ---------------------------------------------------------------------------------------

bool WorldSession::handlePartyPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_GROUP_INVITE:
            handleGroupInvite(packet);
            return true;
        case LogicalOpcode::SMSG_GROUP_DECLINE: {
            game::GroupDeclineData data;
            if (game::GroupDeclineResponseParser::parse(packet, data))
                addSystemChatMessage(fmt1(CLIENT_STR(ERR_DECLINE_GROUP_S), data.playerName));
            return true;
        }
        case LogicalOpcode::SMSG_GROUP_LIST:
            handleGroupList(packet);
            return true;
        case LogicalOpcode::SMSG_GROUP_DESTROYED: {
            // upstream: members cleared + "Your party has been disbanded." (news: the chat, GameLine).
            const bool wasInGroup = isInGroup();
            clearPartyData();
            gameLine(GameLine::News, CLIENT_STR(ERR_GROUP_DISBANDED));
            PSP_LOGI(tag::Net, "group_destroyed was=%d", wasInGroup ? 1 : 0);
            return true;
        }
        case LogicalOpcode::SMSG_GROUP_CANCEL:
            // PARTY_INVITE_CANCEL: the PARTY_INVITE popup hidden (upstream: "Group invite cancelled.").
            if (pendingGroupInvite_) groupInviteCancel_ = true;
            pendingGroupInvite_ = false;
            PSP_LOGI(tag::Net, "group_invite_cancel from=%s", pendingInviterName_.c_str());
            return true;
        case LogicalOpcode::SMSG_GROUP_UNINVITE:
            // upstream handleGroupUninvite: the party cleared, ERR_UNINVITE_YOU (news: the chat, GameLine).
            clearPartyData();
            gameLine(GameLine::News, CLIENT_STR(ERR_UNINVITE_YOU));
            PSP_LOGI(tag::Net, "group_uninvited");
            return true;
        case LogicalOpcode::SMSG_PARTY_COMMAND_RESULT:
            handlePartyCommandResult(packet);
            return true;
        case LogicalOpcode::SMSG_PARTY_MEMBER_STATS:
            handlePartyMemberStats(packet, false);
            return true;
        case LogicalOpcode::SMSG_PARTY_MEMBER_STATS_FULL:
            handlePartyMemberStats(packet, true);
            return true;
        case LogicalOpcode::SMSG_GROUP_SET_LEADER:
            handleGroupSetLeader(packet);
            return true;
        // Group loot rolls (upstream InventoryHandler; before the loot file, which only logged them).
        case LogicalOpcode::SMSG_LOOT_START_ROLL:
            handleLootStartRoll(packet);
            return true;
        case LogicalOpcode::SMSG_LOOT_ROLL:
            handleLootRollVote(packet);
            return true;
        case LogicalOpcode::SMSG_LOOT_ROLL_WON:
            handleLootRollWon(packet);
            return true;
        case LogicalOpcode::SMSG_LOOT_ALL_PASSED:
            handleLootAllPassed(packet);
            return true;
        case LogicalOpcode::SMSG_NAME_QUERY_RESPONSE: {
            // PSP: peeked on a copy - the main switch still takes the name (psp_world_session.cpp).
            if (partyNameQueried_.empty()) return false;
            network::Packet copy = packet;
            game::NameQueryResponseData d;
            if (game::NameQueryResponseParser::parse(copy, d) && d.isValid() && partyNameQueried_.count(d.guid))
                partyLooks_[d.guid] = PartyLook{d.race, d.gender, d.classId};
            return false;
        }
        default:
            return false;
    }
}

// upstream handleGroupInvite: an offer says ERR_INVITED_TO_GROUP_SS in the chat and on the error line
// (+ PARTY_INVITE_REQUEST -> the popup); canAccept 0 says ERR_INVITED_ALREADY_IN_GROUP_SS in the chat.
void WorldSession::handleGroupInvite(network::Packet& packet) {
    game::GroupInviteResponseData data;
    const bool hasCanAccept = !game::isPreWotlk();
    if (!game::GroupInviteResponseParser::parse(packet, data, hasCanAccept)) return;
    PSP_LOGI(tag::Net, "group_invite from=%s can_accept=%u", data.inviterName.c_str(),
             static_cast<unsigned>(data.canAccept));
    if (!data.canAccept) {
        addSystemChatMessage(fmt2(CLIENT_STR(ERR_INVITED_ALREADY_IN_GROUP_SS), data.inviterName, data.inviterName));
        return;
    }
    pendingGroupInvite_ = true;
    groupInviteCancel_ = false;
    pendingInviterName_ = data.inviterName;
    ++groupInviteSeq_;  // PARTY_INVITE_REQUEST
    const std::string line = fmt2(CLIENT_STR(ERR_INVITED_TO_GROUP_SS), data.inviterName, data.inviterName);
    gameLine(GameLine::News, line);  // an invite is news (upstream shows it on both lines)
}

// upstream handleGroupList (the roster replaced, the online state seeded from it) and its lines: joining a
// group says who is in it, leaving it ERR_LEFT_GROUP_YOU ("You are no longer in a group."), a changed loot
// method its ERR_SET_LOOT_* line. The port's own lines while we stay in the group: one per member who came or
// went (ERR_JOINED_GROUP_S / ERR_LEFT_GROUP_S; in a raid ERR_RAID_MEMBER_ADDED_S / _REMOVED_S through the
// battleground chat filter), ERR_RAID_YOU_JOINED when the group becomes a raid, the master looter and threshold
// lines when they change.
void WorldSession::handleGroupList(network::Packet& packet) {
    const bool hasRoles = game::isActiveExpansion("wotlk");
    const bool hasBattleGroupFlag = game::isActiveExpansion("tbc");
    const game::GroupListData prev = partyData_;
    const bool wasInGroup = !prev.isEmpty();
    const bool wasRaid = wasInGroup && game::groupIsRaid(prev.groupType);
    partyData_ = game::GroupListData{};
    ++partyVersion_;
    if (!game::GroupListParser::parse(packet, partyData_, hasRoles, hasBattleGroupFlag)) {
        PSP_LOGW(tag::Net, "group_list parse failed");
        return;
    }
    for (auto& m : partyData_.members)
        if (m.isOnline) m.onlineStatus |= kMemberOnline;
    // Stats a previous roster had for the same member stay until the next SMSG_PARTY_MEMBER_STATS (PSP: the
    // roster carries none; upstream reset them - the frames went empty between the two packets).
    for (auto& m : partyData_.members)
        for (const auto& o : prev.members)
            if (o.guid == m.guid && o.hasPartyStats) {
                const uint16_t online = m.onlineStatus;
                const uint8_t isOnline = m.isOnline, flags = m.flags, sub = m.subGroup, roles = m.roles;
                const std::string name = m.name;
                m = o;
                m.name = name;
                m.isOnline = isOnline;
                m.flags = flags;
                m.subGroup = sub;
                m.roles = roles;
                m.onlineStatus = static_cast<uint16_t>((o.onlineStatus & ~kMemberOnline) | (online & kMemberOnline));
                break;
            }
    const bool nowInGroup = !partyData_.isEmpty();
    const bool raid = game::groupIsRaid(partyData_.groupType);
    auto wasMember = [&](uint64_t guid) {
        for (const auto& o : prev.members)
            if (o.guid == guid) return true;
        return false;
    };
    bool joined = false;
    std::string names;
    for (const auto& m : partyData_.members) {
        if (m.guid == playerGuid_) continue;
        if (!names.empty()) names += ", ";
        names += m.name.empty() ? std::string("?") : m.name;
        if (wasInGroup && !wasMember(m.guid)) {
            joined = true;
            if (!m.name.empty()) {
                if (raid) raidSystemLine(fmt1(CLIENT_STR(ERR_RAID_MEMBER_ADDED_S), m.name), true, m.name);
                else addSystemChatMessage(fmt1(CLIENT_STR(ERR_JOINED_GROUP_S), m.name));
            }
        }
        // PSP: the roster's names feed the name cache (upstream's chat takes names from the party roster),
        // and each member is name-queried once for its race / gender (the portrait out of range).
        names_.emplace(m.guid, m.name);
        if (partyNameQueried_.insert(m.guid).second && socket_) socket_->send(game::NameQueryPacket::build(m.guid));
    }
    if (nowInGroup && !wasInGroup) {  // upstream: "You joined a group with: ..."
        joined = true;
        addSystemChatMessage(raid ? std::string(CLIENT_STR(ERR_RAID_YOU_JOINED)) : OWN_FMT(GROUP_JOINED_WITH, names));
        // Ask for the group's raid marks (AzerothCore HandleRaidTargetUpdateOpcode: 0xFF = the full list back).
        if (socket_ && state_ == State::InWorld) {
            network::Packet p(game::wireOpcode(game::Opcode::MSG_RAID_TARGET_UPDATE));
            p.writeUInt8(0xFF);
            socket_->send(p);
        }
    } else if (nowInGroup && raid && !wasRaid) {
        addSystemChatMessage(CLIENT_STR(ERR_RAID_YOU_JOINED));
    } else if (!nowInGroup && wasInGroup) {  // upstream: "You are no longer in a group."
        clearRaidMarks();
        addSystemChatMessage(CLIENT_STR(ERR_LEFT_GROUP_YOU));
    }
    if (joined) ++partyJoinSoundSeq_;  // igPlayerInviteAccept
    if (wasInGroup && nowInGroup)
        for (const auto& o : prev.members) {
            if (o.guid == playerGuid_ || partyMember(o.guid) || o.name.empty()) continue;
            if (raid) raidSystemLine(fmt1(CLIENT_STR(ERR_RAID_MEMBER_REMOVED_S), o.name), false, o.name);
            else addSystemChatMessage(fmt1(CLIENT_STR(ERR_LEFT_GROUP_S), o.name));
        }
    if (partyData_.memberCount > 0) partyLootLines(wasInGroup);
    // Looks of members no longer in the group are dropped (the map stays the size of the roster).
    for (auto it = partyLooks_.begin(); it != partyLooks_.end();)
        it = partyMember(it->first) ? std::next(it) : partyLooks_.erase(it);
    for (auto it = partyNameQueried_.begin(); it != partyNameQueried_.end();)
        it = partyMember(*it) ? std::next(it) : partyNameQueried_.erase(it);
    if (++partyLogs_ <= 60)
        PSP_LOGI(tag::Net, "group_list members=%u type=0x%X leader=%08X loot=%u thr=%u looter=%08X was=%d now=%d",
                 static_cast<unsigned>(partyData_.memberCount), static_cast<unsigned>(partyData_.groupType),
                 lo32(partyData_.leaderGuid), static_cast<unsigned>(partyData_.lootMethod),
                 static_cast<unsigned>(partyData_.lootThreshold), lo32(partyData_.looterGuid), wasInGroup ? 1 : 0,
                 nowInGroup ? 1 : 0);
    raidRosterUpdate();  // the raid lines and list
}

// The loot settings' lines (upstream: the method line when it changes in a group; the port adds the master
// looter - master loot only - and the threshold the same way). Joining a group takes the settings silently.
void WorldSession::partyLootLines(bool announce) {
    const uint8_t method = partyData_.lootMethod;
    const uint64_t master = partyData_.looterGuid;
    const uint8_t threshold = partyData_.lootThreshold;
    if (!announce) {
        shownLootMethod_ = method;
        shownLooterGuid_ = master;
        shownLootThreshold_ = threshold;
        return;
    }
    if (method != shownLootMethod_) {
        shownLootMethod_ = method;
        if (method < 5) addSystemChatMessage(cs(kErrSetLoot[method]));
    }
    if (master != shownLooterGuid_) {
        shownLooterGuid_ = master;
        if (master != 0 && method == 2) {
            const std::string name = partyMemberName(master);
            if (!name.empty()) addSystemChatMessage(fmt1(CLIENT_STR(ERR_NEW_LOOT_MASTER_S), name));
        }
    }
    if (threshold != shownLootThreshold_) {
        shownLootThreshold_ = threshold;
        if (threshold < 7)
            addSystemChatMessage(fmt1(CLIENT_STR(ERR_SET_LOOT_THRESHOLD_S), cs(kQualityDesc[threshold])));
    }
}

// upstream handlePartyCommandResult: an error on the red line and in the chat, by AzerothCore's PartyResult (the
// names are the GlobalStrings keys; 14 is the swap failure for a swap, else the invite in combat - its comment);
// the codes without a key here say upstream's "Party command failed.". OK says nothing (upstream) but for an
// invite, which names the invited player (ERR_INVITE_PLAYER_S).
void WorldSession::handlePartyCommandResult(network::Packet& packet) {
    game::PartyCommandResultData data;
    if (!game::PartyCommandResultParser::parse(packet, data)) return;
    const uint32_t cmd = static_cast<uint32_t>(data.command), res = static_cast<uint32_t>(data.result);
    PSP_LOGI(tag::Net, "party_result cmd=%u res=%u name=%s", static_cast<unsigned>(cmd), static_cast<unsigned>(res),
             data.name.c_str());
    if (res == 0) {
        if (data.command == game::PartyCommand::INVITE && !data.name.empty())
            addSystemChatMessage(fmt1(CLIENT_STR(ERR_INVITE_PLAYER_S), data.name));
        return;
    }
    const std::string& n = data.name;
    switch (res) {
        case 1: partyRefusal(fmt1(CLIENT_STR(ERR_BAD_PLAYER_NAME_S), n)); break;
        case 2: partyRefusal(fmt1(CLIENT_STR(ERR_TARGET_NOT_IN_GROUP_S), n)); break;
        case 3: partyRefusal(fmt1(CLIENT_STR(ERR_TARGET_NOT_IN_INSTANCE_S), n)); break;
        case 4: partyRefusal(CLIENT_STR(ERR_GROUP_FULL)); break;
        case 5: partyRefusal(fmt1(CLIENT_STR(ERR_ALREADY_IN_GROUP_S), n)); break;
        case 6: partyRefusal(CLIENT_STR(ERR_NOT_IN_GROUP)); break;
        case 7: partyRefusal(CLIENT_STR(ERR_NOT_LEADER)); break;
        case 8: partyRefusal(CLIENT_STR(ERR_PLAYER_WRONG_FACTION)); break;
        case 9: partyRefusal(fmt1(CLIENT_STR(ERR_IGNORING_YOU_S), n)); break;
        case 12: partyRefusal(CLIENT_STR(ERR_LFG_PENDING)); break;
        case 13: partyRefusal(CLIENT_STR(ERR_INVITE_RESTRICTED)); break;
        case 14:
            partyRefusal(data.command == game::PartyCommand::SWAP ? CLIENT_STR(ERR_GROUP_SWAP_FAILED)
                                                                  : CLIENT_STR(ERR_INVITE_IN_COMBAT));
            break;
        case 15: partyRefusal(CLIENT_STR(ERR_INVITE_UNKNOWN_REALM)); break;
        case 16: partyRefusal(CLIENT_STR(ERR_INVITE_NO_PARTY_SERVER)); break;
        case 17: partyRefusal(CLIENT_STR(ERR_INVITE_PARTY_BUSY)); break;
        case 18: partyRefusal(CLIENT_STR(ERR_PARTY_TARGET_AMBIGUOUS)); break;
        case 19: partyRefusal(fmt1(CLIENT_STR(ERR_PARTY_LFG_INVITE_RAID_LOCKED), n)); break;
        case 20: partyRefusal(CLIENT_STR(ERR_PARTY_LFG_BOOT_LIMIT)); break;
        case 22: partyRefusal(CLIENT_STR(ERR_PARTY_LFG_BOOT_IN_PROGRESS)); break;
        case 23: partyRefusal(CLIENT_STR(ERR_PARTY_LFG_BOOT_TOO_FEW_PLAYERS)); break;
        case 25: partyRefusal(OWN_STR(RAID_DISALLOWED_BY_LEVEL)); break;  // ERR_RAID_DISALLOWED_BY_LEVEL
        case 26: partyRefusal(CLIENT_STR(ERR_PARTY_LFG_BOOT_IN_COMBAT)); break;
        case 28: partyRefusal(CLIENT_STR(ERR_PARTY_LFG_BOOT_DUNGEON_COMPLETE)); break;
        case 29: partyRefusal(CLIENT_STR(ERR_PARTY_LFG_BOOT_LOOT_ROLLS)); break;
        case 30: partyRefusal(CLIENT_STR(ERR_PARTY_LFG_TELEPORT_IN_COMBAT)); break;
        default: partyRefusal(OWN_STR(PARTY_COMMAND_FAILED)); break;  // upstream's default line
    }
}

// upstream handlePartyMemberStats, unchanged but for where the member auras go (the session's unit aura
// cache, as upstream's unitAurasCache) and the pre-3.3.5a dispel type (spells.pak instead of the DBC).
void WorldSession::handlePartyMemberStats(network::Packet& packet, bool isFull) {
    auto remaining = [&]() { return packet.getRemainingSize(); };
    const bool isWotLK = game::isActiveExpansion("wotlk");

    if (isFull) {
        if (remaining() < 1) return;
        packet.readUInt8();
    }

    const bool pmsTbc = game::isActiveExpansion("tbc");
    if (remaining() < (pmsTbc ? 8u : 1u)) return;
    const uint64_t memberGuid = pmsTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (remaining() < 4) return;
    const uint32_t updateFlags = packet.readUInt32();

    game::GroupMember* member = nullptr;
    for (auto& m : partyData_.members) {
        if (m.guid == memberGuid) {
            member = &m;
            break;
        }
    }
    if (!member) {
        packet.skipAll();
        return;
    }

    const bool wasOnline = (member->onlineStatus & kMemberOnline) != 0;
    if (updateFlags & 0x0001) {
        if (remaining() >= 2) {
            member->onlineStatus = packet.readUInt16();
            member->isOnline = (member->onlineStatus & kMemberOnline) ? 1 : 0;
        }
    }
    if (updateFlags & 0x0002) {
        if (isWotLK) { if (remaining() >= 4) member->curHealth = packet.readUInt32(); }
        else { if (remaining() >= 2) member->curHealth = packet.readUInt16(); }
    }
    if (updateFlags & 0x0004) {
        if (isWotLK) { if (remaining() >= 4) member->maxHealth = packet.readUInt32(); }
        else { if (remaining() >= 2) member->maxHealth = packet.readUInt16(); }
    }
    if (updateFlags & 0x0008) { if (remaining() >= 1) member->powerType = packet.readUInt8(); }
    if (updateFlags & 0x0010) { if (remaining() >= 2) member->curPower = packet.readUInt16(); }
    if (updateFlags & 0x0020) { if (remaining() >= 2) member->maxPower = packet.readUInt16(); }
    if (updateFlags & 0x0040) { if (remaining() >= 2) member->level = packet.readUInt16(); }
    if (updateFlags & 0x0080) { if (remaining() >= 2) member->zoneId = packet.readUInt16(); }
    if (updateFlags & 0x0100) {
        if (remaining() >= 4) {
            member->posX = static_cast<int16_t>(packet.readUInt16());
            member->posY = static_cast<int16_t>(packet.readUInt16());
        }
    }
    if (updateFlags & 0x0200) {
        if (remaining() >= 8) {
            const uint64_t auraMask = packet.readUInt64();
            std::vector<game::AuraSlot> newAuras;
            for (int i = 0; i < 64; ++i) {
                if (auraMask & (uint64_t(1) << i)) {
                    game::AuraSlot a;
                    a.level = static_cast<uint8_t>(i);
                    if (isWotLK) {
                        if (remaining() < 5) break;
                        a.spellId = packet.readUInt32();
                        a.flags = packet.readUInt8();
                    } else {
                        if (remaining() < 2) break;
                        a.spellId = packet.readUInt16();
                        const SpellInfo* si = spellInfo(a.spellId);  // PSP: spells.pak (upstream getSpellDispelType)
                        if (si && si->rec.dispel > 0) a.flags = 0x80;
                    }
                    if (a.spellId != 0) newAuras.push_back(a);
                }
            }
            if (memberGuid != 0 && memberGuid != playerGuid_ && memberGuid != targetGuid_)
                unitAuras_[memberGuid] = std::move(newAuras);
        }
    }
    // Skip pet fields and vehicle seat
    if (updateFlags & 0x0400) { if (remaining() >= 8) packet.readUInt64(); }
    if (updateFlags & 0x0800) { if (remaining() > 0) packet.readString(); }
    if (updateFlags & 0x1000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x2000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (updateFlags & 0x4000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (updateFlags & 0x8000) { if (remaining() >= 1) packet.readUInt8(); }
    if (updateFlags & 0x10000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x20000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x40000) {
        if (remaining() >= 8) {
            const uint64_t petAuraMask = packet.readUInt64();
            for (int i = 0; i < 64; ++i) {
                if (petAuraMask & (uint64_t(1) << i)) {
                    if (isWotLK) {
                        if (remaining() < 5) break;
                        packet.readUInt32();
                        packet.readUInt8();
                    } else {
                        if (remaining() < 2) break;
                        packet.readUInt16();
                    }
                }
            }
        }
    }
    if (isWotLK && (updateFlags & 0x80000)) { if (remaining() >= 4) packet.readUInt32(); }

    member->hasPartyStats = true;
    ++partyVersion_;  // UNIT_HEALTH / UNIT_POWER / UNIT_AURA / PARTY_MEMBER_ENABLE / _DISABLE
    if (++partyStatLogs_ <= 40 || ((member->onlineStatus & kMemberOnline) != 0) != wasOnline)
        PSP_LOGI(tag::Net, "party_stats guid=%08X full=%d mask=0x%X hp=%u/%u pw=%u/%u t=%u lvl=%u online=%u pos=%d,%d",
                 lo32(memberGuid), isFull ? 1 : 0, static_cast<unsigned>(updateFlags),
                 static_cast<unsigned>(member->curHealth), static_cast<unsigned>(member->maxHealth),
                 static_cast<unsigned>(member->curPower), static_cast<unsigned>(member->maxPower),
                 static_cast<unsigned>(member->powerType), static_cast<unsigned>(member->level),
                 static_cast<unsigned>(member->onlineStatus), static_cast<int>(member->posX),
                 static_cast<int>(member->posY));
}

// upstream handleGroupSetLeader (the leader found by name) and its chat line: ERR_NEW_LEADER_YOU for us,
// ERR_NEW_LEADER_S otherwise.
void WorldSession::handleGroupSetLeader(network::Packet& packet) {
    if (!packet.hasData()) return;
    const std::string leaderName = packet.readString();
    for (const auto& m : partyData_.members) {
        if (m.name == leaderName) {
            partyData_.leaderGuid = m.guid;
            break;
        }
    }
    const game::Character* me = activeCharacter();
    const bool isMe = me && me->name == leaderName;
    if (isMe) partyData_.leaderGuid = playerGuid_;
    ++partyVersion_;  // PARTY_LEADER_CHANGED
    if (!leaderName.empty())
        addSystemChatMessage(isMe ? std::string(CLIENT_STR(ERR_NEW_LEADER_YOU))
                                  : fmt1(CLIENT_STR(ERR_NEW_LEADER_S), leaderName));
    PSP_LOGI(tag::Net, "group_set_leader name=%s me=%d", leaderName.c_str(), isMe ? 1 : 0);
}

// --- Group loot rolls (upstream InventoryHandler) -------------------------------------------------

std::string WorldSession::lootRollItemLink(uint32_t itemId) {
    // PSP: the link's text (the chat writes no link markup): [Name] from the item cache.
    const game::ItemQueryResponseData* info = getItemInfo(itemId);
    if (info && !info->name.empty()) return "[" + info->name + "]";
    ensureItemInfo(itemId);
    return "[" + std::to_string(itemId) + "]";
}

// upstream handleLootRoll / handleLootRollWon / SMSG_LOOT_ALL_PASSED: the roll lines are system chat lines.
void WorldSession::lootRollLine(const std::string& text) { addSystemChatMessage(text); }

// upstream SMSG_LOOT_START_ROLL: objectGuid, mapId (3.3.5a), slot, item, suffix, property, count, countdown,
// vote mask (the count only when the packet holds it). No "Loot roll started for" line: GroupLootFrame shows the
// roll (UIParent.lua START_LOOT_ROLL).
void WorldSession::handleLootStartRoll(network::Packet& packet) {
    const bool hasMapId = game::isActiveExpansion("wotlk");
    const size_t baseSz = hasMapId ? 33 : 29;
    if (packet.getRemainingSize() < baseSz) return;
    const bool hasItemCount = packet.getRemainingSize() >= baseSz + 4;
    LootRoll r;
    r.objectGuid = packet.readUInt64();
    if (hasMapId) packet.readUInt32();
    r.slot = packet.readUInt32();
    r.itemId = packet.readUInt32();
    packet.readUInt32();  // random suffix
    packet.readUInt32();  // random property
    if (hasItemCount) r.count = packet.readUInt32();
    r.countdownMs = packet.readUInt32();
    r.voteMask = packet.readUInt8();
    r.startedMs = logUptimeMs();
    ensureItemInfo(r.itemId);
    // upstream beginLootRoll: one entry per corpse and slot; a repeat keeps its id.
    for (auto& o : lootRolls_)
        if (o.objectGuid == r.objectGuid && o.slot == r.slot) {
            r.rollId = o.rollId;
            o = r;
            return;
        }
    r.rollId = nextLootRollId_++;
    lootRolls_.push_back(r);
    PSP_LOGI(tag::Net, "loot_roll_start id=%d item=%u slot=%u count=%u ms=%u mask=0x%X", r.rollId,
             static_cast<unsigned>(r.itemId), static_cast<unsigned>(r.slot), static_cast<unsigned>(r.count),
             static_cast<unsigned>(r.countdownMs), static_cast<unsigned>(r.voteMask));
}

void WorldSession::endLootRoll(uint64_t objectGuid, uint32_t slot) {
    for (auto it = lootRolls_.begin(); it != lootRolls_.end(); ++it)
        if (it->objectGuid == objectGuid && it->slot == slot) {
            lootRolls_.erase(it);  // CANCEL_LOOT_ROLL
            return;
        }
}

void WorldSession::dropExpiredLootRolls() {
    const uint32_t now = logUptimeMs();
    for (auto it = lootRolls_.begin(); it != lootRolls_.end();) {
        if (now - it->startedMs > it->countdownMs + 5000u) {
            PSP_LOGI(tag::Net, "loot_roll_expired id=%d", it->rollId);
            it = lootRolls_.erase(it);
        } else {
            ++it;
        }
    }
}

// upstream sendLootRoll: CMSG_LOOT_ROLL (guid, slot, vote), the roll's window closed at once.
void WorldSession::rollOnLoot(int rollId, uint8_t rollType) {
    for (const LootRoll& r : lootRolls_) {
        if (r.rollId != rollId) continue;
        if (state_ == State::InWorld && socket_) {
            network::Packet pkt(game::wireOpcode(game::Opcode::CMSG_LOOT_ROLL));
            pkt.writeUInt64(r.objectGuid);
            pkt.writeUInt32(r.slot);
            pkt.writeUInt8(rollType);
            socket_->send(pkt);
        }
        PSP_LOGI(tag::Net, "loot_roll_send id=%d vote=%u", rollId, static_cast<unsigned>(rollType));
        endLootRoll(r.objectGuid, r.slot);
        return;
    }
}

// upstream handleLootRoll (objectGuid, slot, player, item, suffix, property, roll number, vote, auto pass). The
// lines by AzerothCore Group::SendLootRoll / CountRollVote: a number of 128+ is a choice - greed (2),
// disenchant (3), else passed (automatically when autoPass); below 128 vote 0 is the need choice
// and 1 / 2 / 3 the rolls themselves (LOOT_ROLL_ROLLED_*); _SELF lines for the player.
void WorldSession::handleLootRollVote(network::Packet& packet) {
    if (!packet.hasRemaining(35)) return;
    packet.readUInt64();  // object
    packet.readUInt32();  // slot
    const uint64_t player = packet.readUInt64();
    const uint32_t itemId = packet.readUInt32();
    packet.readUInt32();
    packet.readUInt32();
    const uint8_t rollNumber = packet.readUInt8();
    const uint8_t rollType = packet.readUInt8();
    const uint8_t autoPass = packet.readUInt8();
    const bool self = player == playerGuid_;
    std::string name = partyMemberName(player);
    if (name.empty()) name = "?";
    const std::string item = lootRollItemLink(itemId);
    std::string line;
    if (rollNumber >= 128) {
        if (rollType == 2)
            line = self ? fmtArgs(CLIENT_STR(LOOT_ROLL_GREED_SELF), {item})
                        : fmtArgs(CLIENT_STR(LOOT_ROLL_GREED), {name, item});
        else if (rollType == 3)
            line = self ? fmtArgs(CLIENT_STR(LOOT_ROLL_DISENCHANT_SELF), {item})
                        : fmtArgs(CLIENT_STR(LOOT_ROLL_DISENCHANT), {name, item});
        else if (self)
            line = fmtArgs(autoPass ? CLIENT_STR(LOOT_ROLL_PASSED_SELF_AUTO) : CLIENT_STR(LOOT_ROLL_PASSED_SELF),
                           {item});
        else line = fmtArgs(autoPass ? CLIENT_STR(LOOT_ROLL_PASSED_AUTO) : CLIENT_STR(LOOT_ROLL_PASSED), {name, item});
    } else if (rollType == 0) {
        line = self ? fmtArgs(CLIENT_STR(LOOT_ROLL_NEED_SELF), {item})
                    : fmtArgs(CLIENT_STR(LOOT_ROLL_NEED), {name, item});
    } else {
        const char* f = rollType == 2   ? CLIENT_STR(LOOT_ROLL_ROLLED_GREED)
                        : rollType == 3 ? CLIENT_STR(LOOT_ROLL_ROLLED_DE)
                                        : CLIENT_STR(LOOT_ROLL_ROLLED_NEED);
        line = fmtArgs(f, {std::to_string(rollNumber), item, name});
    }
    lootRollLine(line);
}

// upstream handleLootRollWon (objectGuid, slot, item, suffix, property, winner, number, vote): the window
// closes; PSP: LOOT_ROLL_YOU_WON / LOOT_ROLL_WON.
void WorldSession::handleLootRollWon(network::Packet& packet) {
    if (!packet.hasRemaining(34)) return;
    const uint64_t guid = packet.readUInt64();
    const uint32_t slot = packet.readUInt32();
    const uint32_t itemId = packet.readUInt32();
    packet.readUInt32();
    packet.readUInt32();
    const uint64_t winner = packet.readUInt64();
    packet.readUInt8();  // roll number
    packet.readUInt8();  // vote
    const std::string item = lootRollItemLink(itemId);
    if (winner == playerGuid_) lootRollLine(fmtArgs(CLIENT_STR(LOOT_ROLL_YOU_WON), {item}));
    else lootRollLine(fmtArgs(CLIENT_STR(LOOT_ROLL_WON), {partyMemberName(winner), item}));
    endLootRoll(guid, slot);
}

// upstream SMSG_LOOT_ALL_PASSED (objectGuid, slot, item, suffix, property): LOOT_ROLL_ALL_PASSED.
void WorldSession::handleLootAllPassed(network::Packet& packet) {
    if (!packet.hasRemaining(24)) return;
    const uint64_t guid = packet.readUInt64();
    const uint32_t slot = packet.readUInt32();
    const uint32_t itemId = packet.readUInt32();
    packet.readUInt32();
    packet.readUInt32();
    lootRollLine(fmtArgs(CLIENT_STR(LOOT_ROLL_ALL_PASSED), {lootRollItemLink(itemId)}));
    endLootRoll(guid, slot);
}

}  // namespace wowee::platform::psp
