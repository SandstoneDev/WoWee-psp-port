// friends, ignore, who and the guild for the PSP world session -
// upstream SocialHandler's friend / who / guild half (src/game/social_handler.cpp, the Lua API of
// src/addons/lua_social_api.cpp) without GameHandler; the packets as AzerothCore writes and reads them
// (psp_social_parse.hpp). The lines are GlobalStrings keys (the running language's strings.pak) instead of upstream's
// English texts: each result's key is the one named like AzerothCore's enum value (FriendsResult,
// GuildCommandError). Where a line shows is the port's GameLine rule (refusals red, news in the chat) and, where
// upstream shows a message in one place, that place: the friend results, ERR_IGNORE_NOT_FOUND and the guild events /
// invite / decline in the chat (upstream handleFriendStatus, removeIgnore, handleGuildEvent, handleGuildInvite).
//   - SMSG_CONTACT_LIST: the lists whose bit is in the mask are replaced (friends and ignore <= 50: AzerothCore
//     SOCIALMGR_FRIEND_LIMIT / _IGNORE_LIMIT; mutes - PSP: no voice, not kept), names from the name cache
//     (CMSG_NAME_QUERY when unknown). The port's order: friends online first, then by name; ignore by name;
//   - SMSG_FRIEND_STATUS: the result -> its line with the name of the name cache. A line whose name is not known
//     yet waits for its name query (the port's rule, as the channel notices), at most 10 s - then "Unknown";
//   - ShowFriends (CMSG_CONTACT_LIST 7, upstream), AddFriend / AddIgnore (sent as typed, upstream), RemoveFriend /
//     SetFriendNotes / DelIgnore of a name not in the list refused here (upstream; ERR_FRIEND_NOT_FOUND /
//     ERR_IGNORE_NOT_FOUND), AddOrDel*;
//   - SendWho (the port's parser, psp_social_parse.hpp parseWhoText); SMSG_WHO: chat lines while SetWhoToUI is off,
//     else WHO_LIST_UPDATE (the frame) - upstream handleWho; SortWho / SortGuildRoster: one key, ties by name, the
//     same key again reverses (upstream lua_social_api.cpp);
//   - the guild: SMSG_GUILD_ROSTER (sorted; with guildShowOffline off the online members come first and only they
//     are shown - upstream guildRosterView hides the offline ones), MSG_GUILD_PERMISSIONS (asked when the guild or
//     the rank field changes - world entry included), SMSG_GUILD_EVENT (upstream handleGuildEvent: the lines, a
//     sign-on / off line only for a real change, the roster asked again after a membership or rank event),
//     SMSG_GUILD_COMMAND_RESULT, SMSG_GUILD_INVITE (GUILD_INVITE_REQUEST + ERR_INVITED_TO_GUILD_SSS),
//     SMSG_GUILD_DECLINE, SMSG_GUILD_INFO, MSG_GUILD_EVENT_LOG_QUERY, the guild cache (SMSG_GUILD_QUERY_RESPONSE -
//     also the tabard emblems of others; a missing guild asked again after 5 s); GuildRoster's first call 15 s
//     after the UI loads (AnimTimerFrame.xml), the Can* rights of PLAYER_GUILDRANK.
//   - GuildRoster sends at most once per 5 s (the port's rule: a call inside that time is dropped; the session's
//     own roster request after a guild event waits for the 5 s instead).
#include "platform/psp/psp_world_session.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace wowee::platform::psp {

namespace {

using game::LogicalOpcode;

// The lines are the client's GlobalStrings (ERR_FRIEND_*, ERR_IGNORE_*, ERR_GUILD_* ...): CLIENT_STR, strings.pak of
// the running language.

constexpr size_t kGuildCacheMax = 32;      // PSP: guilds whose names / emblems are kept
constexpr uint32_t kNameWaitMs = 10000;    // PSP: a line whose name query never comes back
constexpr uint32_t kRosterGapMs = 5000;    // PSP: GuildRoster at most once per 5 s
constexpr uint32_t kGuildQueryRetryMs = 5000;  // PSP: a guild query without an answer is asked again after 5 s

// format() with Lua's positional (%1$s, %2$d) and plain %s / %d arguments, given as text; |3-N(x) (the ruRU
// declension of a player's name) becomes x, as the PSP chat writes names.
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

uint16_t playerField(int add) {
    const uint16_t end = game::fieldIndex(game::UF::UNIT_END);
    return end == 0xFFFF ? 0xFFFF : static_cast<uint16_t>(end + add);
}

}  // namespace

// --- Lines ---------------------------------------------------------------------------------------------------------

void WorldSession::socialLine(const std::string& text) { gameLine(GameLine::News, text); }

void WorldSession::socialError(const std::string& text) { gameLine(GameLine::Refused, text); }

std::string WorldSession::socialName(uint64_t guid) const {
    auto it = names_.find(guid);
    return it == names_.end() ? std::string() : it->second;
}

uint8_t WorldSession::socialGender(uint64_t guid) const {
    auto it = socialGenders_.find(guid);
    return it == socialGenders_.end() ? 0xFF : it->second;
}

void WorldSession::askName(uint64_t guid) {
    if (guid == 0 || names_.count(guid) || !socket_) return;
    socialNameAsked_.insert(guid);
    if (nameQueried_.insert(guid).second) socket_->send(game::NameQueryPacket::build(guid));
}

// A friend / ignore line with the name of the name cache: printed at once when the name is known, else when its
// query answers (socialUpdate).
void WorldSession::friendStatusLine(uint64_t guid, const char* fmt, int nameArgs, GameLine kind) {
    const std::string name = socialName(guid);
    if (name.empty() && guid != 0) {
        askName(guid);
        pendingLines_.push_back(PendingLine{guid, fmt, nameArgs, logUptimeMs() + kNameWaitMs, kind});
        return;
    }
    gameLine(kind, nameArgs >= 2 ? fmtArgs(fmt, {name, name}) : fmtArgs(fmt, {name}));
}

// --- Sorting -------------------------------------------------------------------------------------------------------

// The port's order: online first, then by name.
void WorldSession::sortFriends() {
    std::stable_sort(friends_.begin(), friends_.end(), [this](const FriendInfo& a, const FriendInfo& b) {
        if (a.online() != b.online()) return a.online();
        return social::compareNames(socialName(a.guid), socialName(b.guid)) < 0;
    });
}

void WorldSession::sortIgnores() {  // by name
    std::stable_sort(ignores_.begin(), ignores_.end(), [this](uint64_t a, uint64_t b) {
        return social::compareNames(socialName(a), socialName(b)) < 0;
    });
}

namespace {
template <typename T>
int cmp3(const T& a, const T& b) { return a < b ? -1 : b < a ? 1 : 0; }
}  // namespace

// SortGuildRoster's key (0 rank, 1 level, 2 name, 3 zone, 4 class, 5 group - none, 6 online, 7 note), ties by name,
// the whole order reversed for a descending sort - upstream lua_social_api.cpp guildRosterView. With guildShowOffline
// off the online members come first: the frame shows only them (upstream: the offline ones left out).
void WorldSession::sortRoster() {
    const social::WhoLookup& lk = whoLookup_;
    auto zoneKey = [&](const GuildMember& m) { return m.online() && lk.zoneName ? lk.zoneName(m.zone) : std::string(); };
    auto cmpKey = [&](const GuildMember& a, const GuildMember& b) -> int {
        switch (rosterSortKey_) {
            case 0: return cmp3(a.rank, b.rank);
            case 1: return cmp3(a.level, b.level);
            case 3: return social::compareNames(zoneKey(a), zoneKey(b));
            case 4: return cmp3(a.cls, b.cls);
            case 6:  // online first, then the shortest time offline (the "last online" column)
                if (a.online() != b.online()) return a.online() ? -1 : 1;
                return cmp3(a.offlineDays, b.offlineDays);
            case 7: return social::compareNames(a.note, b.note);
            default: return 0;
        }
    };
    auto& m = roster_.members;
    std::stable_sort(m.begin(), m.end(), [&](const GuildMember& a, const GuildMember& b) {
        const int c = cmpKey(a, b);
        return c != 0 ? c < 0 : social::compareNames(a.name, b.name) < 0;
    });
    if (rosterDesc_) std::reverse(m.begin(), m.end());
    if (!guildShowOffline_) std::stable_partition(m.begin(), m.end(), [](const GuildMember& x) { return x.online(); });
}

// SortWho's key (0 zone, 1 level, 2 class, 3 group - none, 4 name, 5 race, 6 guild), ties by name, reversed for a
// descending sort - upstream lua_social_api.cpp whoResultsView.
void WorldSession::sortWhoList() {
    const social::WhoLookup& lk = whoLookup_;
    auto cmpKey = [&](const WhoRec& a, const WhoRec& b) -> int {
        switch (whoSortKey_) {
            case 0:
                if (lk.zoneName) return social::compareNames(lk.zoneName(a.zone), lk.zoneName(b.zone));
                return cmp3(a.zone, b.zone);
            case 1: return cmp3(a.level, b.level);
            case 2: return cmp3(a.cls, b.cls);
            case 5: return cmp3(a.race, b.race);
            case 6: return social::compareNames(a.guild, b.guild);
            default: return 0;
        }
    };
    std::stable_sort(who_.begin(), who_.end(), [&](const WhoRec& a, const WhoRec& b) {
        const int c = cmpKey(a, b);
        return c != 0 ? c < 0 : social::compareNames(a.name, b.name) < 0;
    });
    if (whoDesc_) std::reverse(who_.begin(), who_.end());
}

// SortWho / SortGuildRoster: the same key again reverses the order, another key sorts by it ascending (upstream).
void WorldSession::sortWho(int key) {
    if (key < 0 || key > 6) return;
    whoDesc_ = whoSortKey_ == key ? !whoDesc_ : false;
    whoSortKey_ = static_cast<uint8_t>(key);
    sortWhoList();
    ++whoVersion_;
}

void WorldSession::sortGuildRoster(int key) {
    if (key < 0 || key > 7) return;
    rosterDesc_ = rosterSortKey_ == key ? !rosterDesc_ : false;
    rosterSortKey_ = static_cast<uint8_t>(key);
    sortRoster();
    ++guildVersion_;
}

void WorldSession::setGuildShowOffline(bool on) {
    guildShowOffline_ = on;
    configSet("guildshowoffline", on ? "1" : "0");  // PSP: the CVar kept in wowee.cfg
    configSave();
    sortRoster();
    ++guildVersion_;
}

// --- Friends / ignore ----------------------------------------------------------------------------------------------

const WorldSession::FriendInfo* WorldSession::friendByName(const std::string& name) const {
    for (const FriendInfo& f : friends_)
        if (!name.empty() && social::sameName(socialName(f.guid), name)) return &f;
    return nullptr;
}

bool WorldSession::isIgnored(const std::string& name) const {
    for (uint64_t g : ignores_)
        if (!name.empty() && social::sameName(socialName(g), name)) return true;
    return false;
}

void WorldSession::showFriends() {  // ShowFriends: CMSG_CONTACT_LIST with 7 (every list: upstream)
    if (state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_CONTACT_LIST));
    p.writeUInt32(social::kContactFriend | social::kContactIgnore | social::kContactMute);
    socket_->send(p);
}

void WorldSession::addFriend(const std::string& name, const std::string& note) {  // the server checks it
    if (state_ != State::InWorld || !socket_ || name.empty()) return;
    socket_->send(game::AddFriendPacket::build(name, note));
    if (++socialLogs_ <= 80) PSP_LOGI(tag::Net, "friend_add name=%s", name.c_str());
}

void WorldSession::removeFriend(const std::string& name) {
    if (state_ != State::InWorld || !socket_) return;
    const FriendInfo* f = friendByName(name);
    if (!f) {
        socialLine(CLIENT_STR(ERR_FRIEND_NOT_FOUND));
        return;
    }
    socket_->send(game::DelFriendPacket::build(f->guid));
    if (++socialLogs_ <= 80) PSP_LOGI(tag::Net, "friend_del name=%s", name.c_str());
}

void WorldSession::setFriendNotes(const std::string& name, const std::string& note) {
    if (state_ != State::InWorld || !socket_) return;
    for (FriendInfo& f : friends_) {
        if (name.empty() || !social::sameName(socialName(f.guid), name)) continue;
        f.note = social::capText(note, 511, false);  // written locally first, then FRIENDLIST_UPDATE
        ++friendsVersion_;
        socket_->send(game::SetContactNotesPacket::build(f.guid, f.note));
        return;
    }
    socialLine(CLIENT_STR(ERR_FRIEND_NOT_FOUND));
}

void WorldSession::addOrRemoveFriend(const std::string& name, const std::string& note) {
    if (friendByName(name)) removeFriend(name);
    else addFriend(name, note);
}

void WorldSession::addIgnore(const std::string& name) {  // the server checks it (upstream addIgnore)
    if (state_ != State::InWorld || !socket_ || name.empty()) return;
    socket_->send(game::AddIgnorePacket::build(name));
    if (++socialLogs_ <= 80) PSP_LOGI(tag::Net, "ignore_add name=%s", name.c_str());
}

void WorldSession::delIgnore(const std::string& name) {
    if (state_ != State::InWorld || !socket_) return;
    for (uint64_t g : ignores_) {
        if (name.empty() || !social::sameName(socialName(g), name)) continue;
        socket_->send(game::DelIgnorePacket::build(g));
        if (++socialLogs_ <= 80) PSP_LOGI(tag::Net, "ignore_del name=%s", name.c_str());
        return;
    }
    socialLine(CLIENT_STR(ERR_IGNORE_NOT_FOUND));
}

void WorldSession::addOrDelIgnore(const std::string& name) {
    if (isIgnored(name)) delIgnore(name);
    else addIgnore(name);
}

// FriendsFrame_SetOnlineStatus: SendChatMessage("", "AFK" / "DND") - CMSG_MESSAGECHAT with an empty message.
void WorldSession::sendAwayFlag(bool dnd, const std::string& text) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::MessageChatPacket::build(dnd ? game::ChatType::DND : game::ChatType::AFK,
                                                 game::ChatLanguage::UNIVERSAL, text, std::string()));
}

// --- Who -----------------------------------------------------------------------------------------------------------

void WorldSession::sendWho(const std::string& text) {  // SendWho (no throttle; parseWhoText's rules)
    if (state_ != State::InWorld || !socket_) return;
    const social::WhoQuery q = social::parseWhoText(text, whoLookup_);
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_WHO));
    social::writeWho(p, q);
    socket_->send(p);
    if (++socialLogs_ <= 80)
        PSP_LOGI(tag::Net, "who_send lvl=%u-%u name=%s guild=%s race=%08X class=%08X zones=%u strings=%u",
                 static_cast<unsigned>(q.minLevel), static_cast<unsigned>(q.maxLevel), q.name.c_str(), q.guild.c_str(),
                 static_cast<unsigned>(q.raceMask), static_cast<unsigned>(q.classMask),
                 static_cast<unsigned>(q.zones.size()), static_cast<unsigned>(q.strings.size()));
}

bool WorldSession::takeWhoChat(std::vector<WhoRec>& out, uint32_t& count) {
    if (!whoChatPending_) return false;
    whoChatPending_ = false;
    out = std::move(whoChat_);
    whoChat_.clear();
    count = whoChatCount_;
    return true;
}

// --- Guild ---------------------------------------------------------------------------------------------------------

uint32_t WorldSession::guildId() const {
    const uint16_t f = playerField(3);
    return f == 0xFFFF ? 0u : selfField(f);
}

uint32_t WorldSession::guildRankIndex() const {
    const uint16_t f = playerField(4);
    return f == 0xFFFF ? 0u : selfField(f);
}

uint32_t WorldSession::guildRights() const {
    const uint32_t r = guildRankIndex();
    return r < social::kGuildRanks ? roster_.ranks[r].rights : 0u;
}

const WorldSession::GuildRank& WorldSession::guildRank(uint32_t rank) const {
    static const GuildRank kNone{};
    return rank < social::kGuildRanks ? roster_.ranks[rank] : kNone;
}

uint32_t WorldSession::guildNumRanks() {  // GuildControlGetNumRanks: the guild query's count
    if (const social::GuildQuery* g = ownGuild()) {
        if (g->rankCount > 0) return std::min<uint32_t>(g->rankCount, social::kGuildRanks);
        uint32_t n = 0;
        while (n < social::kGuildRanks && !g->ranks[n].empty()) ++n;
        return n;
    }
    return std::min<uint32_t>(roster_.rankCount, social::kGuildRanks);
}

bool WorldSession::guildRosterSend() {  // CMSG_GUILD_ROSTER unless one went out in the last kRosterGapMs
    if (state_ != State::InWorld || !socket_ || !isInGuild()) return false;
    const uint32_t now = logUptimeMs();
    if (rosterSentMs_ != 0 && now - rosterSentMs_ < kRosterGapMs) return false;
    rosterSentMs_ = now ? now : 1u;
    rosterWanted_ = false;
    socket_->send(game::GuildRosterPacket::build());
    return true;
}

void WorldSession::guildRosterRequest() { guildRosterSend(); }  // GuildRoster: dropped inside the 5 s

void WorldSession::guildCommandByName(LogicalOpcode op, const std::string& name) {  // an empty name: nothing
    if (state_ != State::InWorld || !socket_ || name.empty()) return;
    network::Packet p(game::wireOpcode(op));
    p.writeString(name);
    socket_->send(p);
    if (++socialLogs_ <= 80) PSP_LOGI(tag::Net, "guild_cmd op=%u name=%s", static_cast<unsigned>(op), name.c_str());
}

void WorldSession::guildInvite(const std::string& name) { guildCommandByName(LogicalOpcode::CMSG_GUILD_INVITE, name); }
void WorldSession::guildUninvite(const std::string& name) { guildCommandByName(LogicalOpcode::CMSG_GUILD_REMOVE, name); }
void WorldSession::guildPromote(const std::string& name) { guildCommandByName(LogicalOpcode::CMSG_GUILD_PROMOTE, name); }
void WorldSession::guildDemote(const std::string& name) { guildCommandByName(LogicalOpcode::CMSG_GUILD_DEMOTE, name); }
void WorldSession::guildSetLeader(const std::string& name) { guildCommandByName(LogicalOpcode::CMSG_GUILD_LEADER, name); }

// The text limits are the frames' (staticpopup.lua: MOTD 128, notes 31, rank names 15; GuildInfoEditBox 500); the
// MOTD and the notes are one line. The server skips an unchanged text itself (AzerothCore Guild.cpp HandleSetMOTD).
void WorldSession::guildSetMotd(const std::string& text) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GuildMotdPacket::build(social::capText(text, social::kMotdChars, true)));
}

void WorldSession::guildSetInfoText(const std::string& text) {  // 500 characters, no local update
    if (state_ != State::InWorld || !socket_) return;
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_GUILD_INFO_TEXT));
    p.writeString(social::capText(text, social::kInfoChars, false));
    socket_->send(p);
}

void WorldSession::guildSetPublicNote(const std::string& name, const std::string& note) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GuildSetPublicNotePacket::build(name, social::capText(note, social::kNoteChars, true)));
}

void WorldSession::guildSetOfficerNote(const std::string& name, const std::string& note) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GuildSetOfficerNotePacket::build(name, social::capText(note, social::kNoteChars, true)));
}

void WorldSession::guildLeave() {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GuildLeavePacket::build());
}

void WorldSession::guildDisband() {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GuildDisbandPacket::build());
}

void WorldSession::guildInfoRequest() {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GuildInfoPacket::build());
}

// GuildControlSaveRank: rank, rights, name (15 characters), gold per day x 10000, 6 x {tab flags, slots}.
void WorldSession::guildSaveRank(uint32_t rank, uint32_t rights, const std::string& name, uint32_t goldPerDay,
                                 const uint32_t tabFlags[6], const uint32_t tabSlots[6]) {
    if (state_ != State::InWorld || !socket_) return;
    if (rank >= guildNumRanks()) return;
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_GUILD_RANK));
    p.writeUInt32(rank);
    p.writeUInt32(rights);
    p.writeString(social::capText(name, social::kRankNameChars, false));
    const uint64_t copper = static_cast<uint64_t>(std::min<uint32_t>(goldPerDay, 1000000000u)) * 10000u;
    p.writeUInt32(static_cast<uint32_t>(std::min<uint64_t>(copper, 0xFFFFFFFFu)));
    for (int t = 0; t < 6; ++t) {
        p.writeUInt32(tabFlags ? tabFlags[t] : 0u);
        p.writeUInt32(tabSlots ? tabSlots[t] : 0u);
    }
    socket_->send(p);
    PSP_LOGI(tag::Net, "guild_rank_save rank=%u rights=%08X gold=%u", static_cast<unsigned>(rank),
             static_cast<unsigned>(rights), static_cast<unsigned>(goldPerDay));
}

void WorldSession::guildAddRank(const std::string& name) {  // below 10 ranks
    if (state_ != State::InWorld || !socket_ || guildNumRanks() >= social::kGuildRanks) return;
    socket_->send(game::GuildAddRankPacket::build(social::capText(name, social::kRankNameChars, false)));
}

void WorldSession::guildDelRank() {  // above 5 ranks
    if (state_ != State::InWorld || !socket_ || guildNumRanks() <= 5) return;
    socket_->send(game::GuildDelRankPacket::build());
}

void WorldSession::guildEventLogQuery() {  // QueryGuildEventLog
    if (state_ != State::InWorld || !socket_ || !isInGuild()) return;
    socket_->send(network::Packet(game::wireOpcode(LogicalOpcode::MSG_GUILD_EVENT_LOG_QUERY)));
}

void WorldSession::acceptGuild() {  // AcceptGuild
    guildInvitePending_ = false;  // answered
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GuildAcceptPacket::build());
    PSP_LOGI(tag::Net, "guild_accept");
}

void WorldSession::declineGuild() {  // DeclineGuild
    guildInvitePending_ = false;  // answered (a timeout, START)
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::GuildDeclineInvitationPacket::build());
    PSP_LOGI(tag::Net, "guild_decline");
}

void WorldSession::sendGuildPermissionsQuery() {  // MSG_GUILD_PERMISSIONS (empty)
    if (state_ != State::InWorld || !socket_ || !isInGuild()) return;
    socket_->send(network::Packet(game::wireOpcode(LogicalOpcode::MSG_GUILD_PERMISSIONS)));
}

// The guild cache: a miss asks CMSG_GUILD_QUERY, again after kGuildQueryRetryMs while it is still wanted (the
// port's rule; an answer without a name is not kept and waits the same time).
const social::GuildQuery* WorldSession::guildQuery(uint32_t id) {
    if (id == 0) return nullptr;
    for (GuildCacheEntry& e : guildCache_)
        if (e.q.id == id) {
            e.lastUse = ++guildCacheTick_;
            return &e.q;
        }
    if (!socket_) return nullptr;
    const uint32_t now = logUptimeMs();
    auto it = guildAskedMs_.find(id);
    if (it == guildAskedMs_.end() || now - it->second > kGuildQueryRetryMs) {
        guildAskedMs_[id] = now;
        socket_->send(game::GuildQueryPacket::build(id));
        if (++socialLogs_ <= 80) PSP_LOGI(tag::Net, "guild_query id=%u", static_cast<unsigned>(id));
    }
    return nullptr;
}

// --- Per frame -----------------------------------------------------------------------------------------------------

void WorldSession::socialUpdate() {
    if (state_ != State::InWorld) {
        worldEnterMs_ = 0;
        loginRosterDone_ = false;
        return;
    }
    const uint32_t now = logUptimeMs();
    if (worldEnterMs_ == 0) {
        worldEnterMs_ = now ? now : 1u;
        static bool cfgRead = false;
        if (!cfgRead) {  // PSP: the CVars guildShowOffline / guildMemberNotify kept in wowee.cfg
            cfgRead = true;
            guildShowOffline_ = configGetInt("guildshowoffline", 1) != 0;
            guildNotify_ = configGetInt("guildnotify", 0) != 0;
        }
    }
    // The guild id / rank fields: PLAYER_GUILD_UPDATE, the permissions asked again (the port's rule: on every change
    // of the two, the first one after entering the world included).
    if (selfKnown()) {
        const uint32_t gid = guildId(), rank = guildRankIndex();
        if (gid != lastGuildId_ || rank != lastGuildRank_) {
            if (gid != lastGuildId_) {
                if (gid == 0) {  // left / removed / disbanded: the guild's data goes
                    roster_ = social::Roster{};
                    rosterKnown_ = false;
                    eventLog_.clear();
                    guildTabs_ = 0;
                }
                if (gid != 0) rosterWanted_ = true;
            }
            lastGuildId_ = gid;
            lastGuildRank_ = rank;
            ++guildVersion_;
            sendGuildPermissionsQuery();
            if (gid != 0) guildQuery(gid);
            PSP_LOGI(tag::Net, "guild_self id=%u rank=%u", static_cast<unsigned>(gid), static_cast<unsigned>(rank));
        }
        // AnimTimerFrame: GuildRoster() once, 15 s after the UI loaded, when in a guild.
        if (!loginRosterDone_ && now - worldEnterMs_ > 15000u) {
            loginRosterDone_ = true;
            if (isInGuild()) rosterWanted_ = true;
        }
        if (rosterWanted_) guildRosterSend();  // the session's own request: waits for the 5 s
    }
    // Lines waiting for a name, FRIENDLIST / IGNORELIST_UPDATE once the names are in.
    for (auto it = pendingLines_.begin(); it != pendingLines_.end();) {
        std::string name = socialName(it->guid);
        const bool late = now >= it->deadlineMs;
        if (name.empty() && !late) {
            ++it;
            continue;
        }
        if (name.empty()) name = CLIENT_STR(UNKNOWN);  // PSP: no answer in time (or a failed query)
        gameLine(it->kind, it->nameArgs >= 2 ? fmtArgs(it->fmt, {name, name}) : fmtArgs(it->fmt, {name}));
        it = pendingLines_.erase(it);
    }
    if (namesPending_) {
        bool missing = false;
        for (const FriendInfo& f : friends_) missing = missing || socialName(f.guid).empty();
        for (uint64_t g : ignores_) missing = missing || socialName(g).empty();
        if (!missing || now - worldEnterMs_ > 600000u) {
            namesPending_ = false;
            sortFriends();
            sortIgnores();
            ++friendsVersion_;
            ++ignoresVersion_;
        }
    }
}

// --- Packets -------------------------------------------------------------------------------------------------------

bool WorldSession::handleSocialPacket(LogicalOpcode op, network::Packet& packet) {
    switch (op) {
        case LogicalOpcode::SMSG_NAME_QUERY_RESPONSE: {
            // PSP: peeked on a copy - the gender of a contact / who / guild name (the class name for the gender).
            if (socialNameAsked_.empty() && friends_.empty()) return false;
            network::Packet copy = packet;
            game::NameQueryResponseData d;
            if (!game::NameQueryResponseParser::parse(copy, d)) return false;
            if (d.isValid()) {
                socialGenders_[d.guid] = d.gender;
                if (socialGenders_.size() > 512) socialGenders_.clear();
                if (socialNameAsked_.erase(d.guid)) namesPending_ = true;
            } else if (d.found != 0 && socialNameAsked_.erase(d.guid)) {
                // The query failed: the lines waiting for that name go out now ("Unknown"); the entry stays.
                for (PendingLine& l : pendingLines_)
                    if (l.guid == d.guid) l.deadlineMs = 0;
                PSP_LOGW(tag::Net, "social_name_failed guid=%llX", static_cast<unsigned long long>(d.guid));
            }
            return false;
        }
        case LogicalOpcode::SMSG_CONTACT_LIST: {
            social::ContactList cl;
            if (!social::readContactList(packet, cl)) {
                PSP_LOGW(tag::Net, "contact_list_bad size=%u", static_cast<unsigned>(packet.getSize()));
                return true;
            }
            if (cl.mask & social::kContactFriend) friends_.clear();
            if (cl.mask & social::kContactIgnore) ignores_.clear();
            for (const social::ContactRec& c : cl.contacts) {
                if ((c.flags & social::kContactFriend) && (cl.mask & social::kContactFriend) &&
                    friends_.size() < social::kMaxFriends) {
                    FriendInfo f;
                    f.guid = c.guid;
                    f.note = c.note;
                    f.status = c.status;
                    f.area = c.area;
                    f.level = c.level;
                    f.cls = c.cls;
                    friends_.push_back(std::move(f));
                    askName(c.guid);
                }
                if ((c.flags & cl.mask & social::kContactIgnore) && ignores_.size() < social::kMaxIgnores) {
                    ignores_.push_back(c.guid);
                    askName(c.guid);
                }
            }
            sortFriends();
            sortIgnores();
            ++friendsVersion_;
            ++ignoresVersion_;
            namesPending_ = true;
            PSP_LOGI(tag::Net, "contact_list mask=%u count=%u friends=%u ignores=%u", static_cast<unsigned>(cl.mask),
                     static_cast<unsigned>(cl.count), static_cast<unsigned>(friends_.size()),
                     static_cast<unsigned>(ignores_.size()));
            return true;
        }
        case LogicalOpcode::SMSG_FRIEND_STATUS: {
            social::FriendStatus fs;
            if (!social::readFriendStatus(packet, fs)) return true;
            if (++socialLogs_ <= 120)
                PSP_LOGI(tag::Net, "friend_status result=%u guid=%llX", static_cast<unsigned>(fs.result),
                         static_cast<unsigned long long>(fs.guid));
            auto find = [&]() -> FriendInfo* {
                for (FriendInfo& f : friends_)
                    if (f.guid == fs.guid) return &f;
                return nullptr;
            };
            auto addSlot = [&]() -> FriendInfo* {  // the entry, added when missing
                if (FriendInfo* f = find()) return f;
                if (friends_.size() >= social::kMaxFriends) return nullptr;
                FriendInfo f;
                f.guid = fs.guid;
                friends_.push_back(std::move(f));
                askName(fs.guid);
                return &friends_.back();
            };
            switch (fs.result) {  // AzerothCore FriendsResult -> the key named like it
                case social::kFriendDbError: socialLine(CLIENT_STR(ERR_FRIEND_ERROR)); break;
                case social::kFriendNotFound: socialLine(CLIENT_STR(ERR_FRIEND_NOT_FOUND)); break;
                case social::kFriendListFull: socialLine(CLIENT_STR(ERR_FRIEND_LIST_FULL)); break;
                case social::kFriendOnline:
                    if (FriendInfo* f = find()) {
                        f->status = fs.status ? fs.status : 1;
                        f->area = fs.area;
                        f->level = fs.level;
                        f->cls = fs.cls;
                    }
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_FRIEND_ONLINE_SS), 2);  // + sound FRIENDJOINGAME
                    ++friendSoundSeq_;
                    break;
                case social::kFriendOffline:
                    if (FriendInfo* f = find()) f->status = 0;
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_FRIEND_OFFLINE_S), 1);
                    break;
                case social::kFriendRemoved: {
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_FRIEND_REMOVED_S), 1);
                    friends_.erase(std::remove_if(friends_.begin(), friends_.end(),
                                                  [&](const FriendInfo& x) { return x.guid == fs.guid; }),
                                   friends_.end());
                    break;
                }
                case social::kFriendAddedOnline:
                case social::kFriendAddedOffline:
                    if (FriendInfo* f = addSlot()) {
                        f->note = fs.note;
                        f->status = fs.result == social::kFriendAddedOnline ? (fs.status ? fs.status : 1) : 0;
                        if (fs.hasInfo) {
                            f->area = fs.area;
                            f->level = fs.level;
                            f->cls = fs.cls;
                        }
                    }
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_FRIEND_ADDED_S), 1);
                    break;
                case social::kFriendAlready:
                    addSlot();
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_FRIEND_ALREADY_S), 1);
                    break;
                case social::kFriendSelf: socialLine(CLIENT_STR(ERR_FRIEND_SELF)); break;
                case social::kFriendEnemy: socialLine(CLIENT_STR(ERR_FRIEND_WRONG_FACTION)); break;
                case social::kIgnoreFull: socialError(CLIENT_STR(ERR_IGNORE_FULL)); break;
                case social::kIgnoreSelf: socialError(CLIENT_STR(ERR_IGNORE_SELF)); break;
                case social::kIgnoreNotFound: socialLine(CLIENT_STR(ERR_IGNORE_NOT_FOUND)); break;
                case social::kIgnoreAlready:
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_IGNORE_ALREADY_S), 1, GameLine::Refused);
                    break;
                case social::kIgnoreAdded:
                    if (std::find(ignores_.begin(), ignores_.end(), fs.guid) == ignores_.end() &&
                        ignores_.size() < social::kMaxIgnores) {
                        ignores_.push_back(fs.guid);
                        askName(fs.guid);
                    }
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_IGNORE_ADDED_S), 1);
                    ++ignoresVersion_;
                    namesPending_ = true;
                    break;
                case social::kIgnoreRemoved:
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_IGNORE_REMOVED_S), 1);
                    ignores_.erase(std::remove(ignores_.begin(), ignores_.end(), fs.guid), ignores_.end());
                    ++ignoresVersion_;
                    break;
                case social::kIgnoreAmbiguous: socialError(CLIENT_STR(ERR_IGNORE_AMBIGUOUS)); break;
                case social::kMuteFull: socialError(CLIENT_STR(ERR_VOICE_IGNORE_FULL)); break;
                case social::kMuteSelf: socialError(CLIENT_STR(ERR_VOICE_IGNORE_SELF)); break;
                case social::kMuteNotFound: socialError(CLIENT_STR(ERR_VOICE_IGNORE_NOT_FOUND)); break;
                case social::kMuteAlready:
                    friendStatusLine(fs.guid, CLIENT_STR(ERR_VOICE_IGNORE_ALREADY_S), 1, GameLine::Refused);
                    break;
                case social::kMuteAdded: friendStatusLine(fs.guid, CLIENT_STR(ERR_VOICE_IGNORE_ADDED_S), 1); break;
                case social::kMuteRemoved: friendStatusLine(fs.guid, CLIENT_STR(ERR_VOICE_IGNORE_REMOVED_S), 1); break;
                case social::kMuteAmbiguous: socialError(CLIENT_STR(ERR_VOICE_IGNORE_AMBIGUOUS)); break;
                default: break;  // FRIEND_UNK1..3 / FRIEND_UNKNOWN: no message (AzerothCore SocialMgr.h)
            }
            sortFriends();
            ++friendsVersion_;
            return true;
        }
        case LogicalOpcode::SMSG_WHO: {
            social::WhoList wl;
            if (!social::readWho(packet, wl)) return true;
            who_ = std::move(wl.list);
            whoTotal_ = wl.total;
            // upstream handleWho: with SetWhoToUI off every result goes to the chat (the list keeps them for the Who
            // tab); with it on, WHO_LIST_UPDATE.
            if (!whoToUi_) {
                whoChat_ = who_;
                whoChatCount_ = wl.display;
                whoChatPending_ = true;
            } else {
                ++whoVersion_;
            }
            sortWhoList();
            if (++socialLogs_ <= 80)
                PSP_LOGI(tag::Net, "who_result display=%u total=%u kept=%u ui=%d", static_cast<unsigned>(wl.display),
                         static_cast<unsigned>(wl.total), static_cast<unsigned>(who_.size()),
                         whoChatPending_ ? 0 : 1);
            return true;
        }
        case LogicalOpcode::SMSG_GUILD_ROSTER: {
            social::Roster r;
            if (!social::readRoster(packet, r, social::kMaxRoster)) {
                PSP_LOGW(tag::Net, "guild_roster_bad size=%u", static_cast<unsigned>(packet.getSize()));
                return true;
            }
            roster_ = std::move(r);
            rosterKnown_ = true;
            sortRoster();
            ++guildVersion_;  // GUILD_ROSTER_UPDATE
            uint32_t bytes = 0;
            for (const GuildMember& m : roster_.members)
                bytes += static_cast<uint32_t>(sizeof(m) + m.name.capacity() + m.note.capacity() + m.officerNote.capacity());
            PSP_LOGI(tag::Net, "guild_roster members=%u kept=%u online=%u ranks=%u kb=%u",
                     static_cast<unsigned>(roster_.memberCount), static_cast<unsigned>(roster_.members.size()),
                     static_cast<unsigned>(roster_.online), static_cast<unsigned>(roster_.rankCount),
                     static_cast<unsigned>(bytes / 1024u));
            return true;
        }
        case LogicalOpcode::MSG_GUILD_PERMISSIONS: {
            social::GuildPermissions gp;
            if (!social::readGuildPermissions(packet, gp)) return true;
            if (gp.rank < social::kGuildRanks) {
                social::RosterRank& rk = roster_.ranks[gp.rank];
                rk.rights = gp.rights;
                rk.moneyPerDay = gp.moneyPerDay;
                for (int t = 0; t < 6; ++t) {
                    rk.tabFlags[t] = gp.tabFlags[t];
                    rk.tabSlots[t] = gp.tabSlots[t];
                }
            }
            guildTabs_ = gp.purchasedTabs;
            ++guildVersion_;
            PSP_LOGI(tag::Net, "guild_permissions rank=%u rights=%08X tabs=%u", static_cast<unsigned>(gp.rank),
                     static_cast<unsigned>(gp.rights), static_cast<unsigned>(gp.purchasedTabs));
            return true;
        }
        case LogicalOpcode::MSG_GUILD_EVENT_LOG_QUERY: {
            std::vector<social::EventLogEntry> log;
            if (!social::readEventLog(packet, log)) return true;
            eventLog_ = std::move(log);
            eventLogAtMs_ = logUptimeMs();
            for (const social::EventLogEntry& e : eventLog_) {
                askName(e.guid1);
                if (e.guid2) askName(e.guid2);
            }
            ++eventLogVersion_;
            PSP_LOGI(tag::Net, "guild_event_log n=%u", static_cast<unsigned>(eventLog_.size()));
            return true;
        }
        case LogicalOpcode::SMSG_GUILD_QUERY_RESPONSE: {
            social::GuildQuery q;
            if (!social::readGuildQuery(packet, q)) return true;
            guildAskedMs_.erase(q.id);
            if (q.id == 0 || q.name.empty()) return true;  // a negative entry
            GuildCacheEntry* slot = nullptr;
            for (GuildCacheEntry& e : guildCache_)
                if (e.q.id == q.id) slot = &e;
            if (!slot) {
                if (guildCache_.size() >= kGuildCacheMax) {
                    auto old = std::min_element(guildCache_.begin(), guildCache_.end(),
                                                [](const GuildCacheEntry& a, const GuildCacheEntry& b) {
                                                    return a.lastUse < b.lastUse;
                                                });
                    guildCache_.erase(old);
                }
                guildCache_.push_back(GuildCacheEntry{});
                slot = &guildCache_.back();
            }
            slot->q = std::move(q);
            slot->lastUse = ++guildCacheTick_;
            ++guildCacheVersion_;
            if (slot->q.id == guildId()) ++guildVersion_;
            if (++socialLogs_ <= 80)
                PSP_LOGI(tag::Net, "guild_query_answer id=%u name=%s ranks=%u emblem=%d,%d,%d,%d,%d",
                         static_cast<unsigned>(slot->q.id), slot->q.name.c_str(), static_cast<unsigned>(slot->q.rankCount),
                         static_cast<int>(slot->q.emblem[0]), static_cast<int>(slot->q.emblem[1]),
                         static_cast<int>(slot->q.emblem[2]), static_cast<int>(slot->q.emblem[3]),
                         static_cast<int>(slot->q.emblem[4]));
            return true;
        }
        case LogicalOpcode::SMSG_GUILD_EVENT: {
            social::GuildEvent ev;
            if (!social::readGuildEvent(packet, ev)) return true;
            const std::string& s1 = ev.strings[0];
            const std::string& s2 = ev.strings[1];
            const std::string& s3 = ev.strings[2];
            // upstream handleGuildEvent: a membership or rank event asks the roster again, a sign-on / off redraws it.
            bool askRoster = false, rosterUpdate = false;
            switch (ev.type) {
                case social::kGePromotion: socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_PROMOTE_SSS), {s1, s2, s3})); break;
                case social::kGeDemotion: socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_DEMOTE_SSS), {s1, s2, s3})); break;
                case social::kGeMotd:
                    // GUILD_MOTD(text): no C line; ChatFrame prints GUILD_MOTD_TEMPLATE in the guild colour when the
                    // MOTD is not empty.
                    roster_.motd = social::capText(s1, social::kMotdChars, true);
                    if (!roster_.motd.empty()) {
                        game::MessageChatData msg;
                        msg.type = game::ChatType::GUILD;  // no sender: the chat writes it as it is (the guild colour)
                        msg.language = game::ChatLanguage::UNIVERSAL;
                        msg.message = fmtArgs(CLIENT_STR(GUILD_MOTD_TEMPLATE), {roster_.motd});
                        addLocalChatMessage(msg);
                    }
                    break;
                case social::kGeJoined: socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_JOIN_S), {s1})); break;
                case social::kGeLeft: socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_LEAVE_S), {s1})); break;
                case social::kGeRemoved: socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_REMOVE_SS), {s1, s2})); break;
                case social::kGeLeaderIs: socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_LEADER_IS_S), {s1})); break;
                case social::kGeLeaderChanged: socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_LEADER_CHANGED_SS), {s1, s2})); break;
                case social::kGeDisbanded: socialLine(CLIENT_STR(ERR_GUILD_DISBANDED)); break;
                case social::kGeRankUpdated: {  // silent: rank[atoi(s1)].name = s2, numRanks = atoi(s3)
                    const uint32_t gid = guildId();  // (AzerothCore Guild::HandleSetRankInfo / HandleAddNewRank)
                    for (GuildCacheEntry& e : guildCache_) {
                        if (e.q.id != gid) continue;
                        const int r = std::atoi(s1.c_str());
                        if (r >= 0 && r < static_cast<int>(social::kGuildRanks))
                            e.q.ranks[r] = social::capText(s2, social::kRankNameChars, false);
                        if (ev.count >= 3) e.q.rankCount = static_cast<uint32_t>(std::atoi(s3.c_str()));
                        ++guildCacheVersion_;
                    }
                    break;
                }
                case social::kGeRankDeleted: {  // silent: numRanks = atoi(s1) (AzerothCore Guild::HandleRemoveRank)
                    const uint32_t gid = guildId();
                    for (GuildCacheEntry& e : guildCache_)
                        if (e.q.id == gid) {
                            e.q.rankCount = static_cast<uint32_t>(std::atoi(s1.c_str()));
                            ++guildCacheVersion_;
                        }
                    break;
                }
                case social::kGeSignedOn:
                case social::kGeSignedOff: {
                    // A line only for a real change of the member's state (upstream guildMemberOnlineTransition:
                    // a member not in the roster yet counts as one) and with guildMemberNotify on.
                    const bool on = ev.type == social::kGeSignedOn;
                    bool changed = true;
                    for (GuildMember& m : roster_.members) {
                        if (!social::sameName(m.name, s1)) continue;
                        changed = m.online() != on;
                        m.flags = on ? 1 : 0;
                        break;
                    }
                    if (guildNotify_ && changed && !s1.empty()) {
                        if (on) {
                            socialLine(fmtArgs(CLIENT_STR(ERR_FRIEND_ONLINE_SS), {s1, s1}));
                            ++friendSoundSeq_;
                        } else {
                            socialLine(fmtArgs(CLIENT_STR(ERR_FRIEND_OFFLINE_S), {s1}));
                        }
                    }
                    rosterUpdate = true;
                    break;
                }
                case social::kGeBankTabPurchased:  // the bank's events: the window's (psp_world_session_guildbank.cpp)
                    if (guildTabs_ < 6) ++guildTabs_;
                    break;
                default: break;  // the other bank events, the tabard (below), unknown types: no line (upstream)
            }
            switch (ev.type) {  // upstream's clientWillRequest: these change the roster's members or ranks
                case social::kGePromotion: case social::kGeDemotion: case social::kGeJoined: case social::kGeLeft:
                case social::kGeRemoved: case social::kGeLeaderChanged: case social::kGeRankUpdated:
                case social::kGeRankDeleted:
                    askRoster = rosterKnown_;
                    rosterUpdate = true;
                    break;
                default: break;
            }
            if (ev.type == social::kGeTabardChanged) {  // upstream: the guild query again (the emblem is in it)
                guildAskedMs_.erase(guildId());
                guildCache_.erase(std::remove_if(guildCache_.begin(), guildCache_.end(),
                                                 [&](const GuildCacheEntry& e) { return e.q.id == guildId(); }),
                                  guildCache_.end());
                guildQuery(guildId());
                ++guildCacheVersion_;
            }
            if (askRoster) rosterWanted_ = true;
            if (rosterUpdate) ++guildVersion_;
            if (++socialLogs_ <= 120)
                PSP_LOGI(tag::Net, "guild_event type=%u n=%u", static_cast<unsigned>(ev.type),
                         static_cast<unsigned>(ev.count));
            return true;
        }
        case LogicalOpcode::SMSG_GUILD_COMMAND_RESULT: {
            social::GuildCommandResult r;
            if (!social::readGuildCommandResult(packet, r)) return true;
            PSP_LOGI(tag::Net, "guild_command cmd=%u result=%u name=%s", static_cast<unsigned>(r.command),
                     static_cast<unsigned>(r.result), r.name.c_str());
            // AzerothCore Guild.h GuildCommandError -> the key of the same name (every one a chat line); success: the
            // GUILD_COMMAND_CREATE / _INVITE / _QUIT / _FOUNDER line; ERR_GUILD_UNK1 (20) "forces roster update".
            const std::string& n = r.name;
            switch (r.result) {
                case 0:
                    if (r.command == 0) socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_CREATE_S), {n}));
                    else if (r.command == 1) socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_INVITE_S), {n}));
                    else if (r.command == 3) socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_QUIT_S), {n}));
                    else if (r.command == 14) socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_FOUNDER_S), {n}));
                    break;
                case 1: socialError(CLIENT_STR(ERR_GUILD_INTERNAL)); break;
                case 2: socialError(CLIENT_STR(ERR_ALREADY_IN_GUILD)); break;
                case 3: socialError(fmtArgs(CLIENT_STR(ERR_ALREADY_IN_GUILD_S), {n})); break;
                case 4: socialError(CLIENT_STR(ERR_INVITED_TO_GUILD)); break;
                case 5: socialError(fmtArgs(CLIENT_STR(ERR_ALREADY_INVITED_TO_GUILD_S), {n})); break;
                case 6: socialError(CLIENT_STR(ERR_NAME_INVALID)); break;  // ERR_GUILD_NAME_INVALID: the name key
                case 7: socialError(fmtArgs(CLIENT_STR(ERR_GUILD_NAME_EXISTS_S), {n})); break;
                // 8 is both ERR_GUILD_LEADER_LEAVE (AzerothCore sends it for GUILD_COMMAND_QUIT) and ERR_GUILD_PERMISSIONS.
                case 8: socialError(r.command == 3 ? CLIENT_STR(ERR_GUILD_LEADER_LEAVE) : CLIENT_STR(ERR_GUILD_PERMISSIONS)); break;
                case 9: socialError(CLIENT_STR(ERR_GUILD_PLAYER_NOT_IN_GUILD)); break;
                case 10: socialError(fmtArgs(CLIENT_STR(ERR_GUILD_PLAYER_NOT_IN_GUILD_S), {n})); break;
                case 11: socialError(fmtArgs(CLIENT_STR(ERR_GUILD_PLAYER_NOT_FOUND_S), {n})); break;
                case 12: socialError(CLIENT_STR(ERR_GUILD_NOT_ALLIED)); break;
                case 13: socialError(fmtArgs(CLIENT_STR(ERR_GUILD_RANK_TOO_HIGH_S), {n})); break;
                case 14: socialError(fmtArgs(CLIENT_STR(ERR_GUILD_RANK_TOO_LOW_S), {n})); break;
                case 17: socialError(CLIENT_STR(ERR_GUILD_RANKS_LOCKED)); break;
                case 18: socialError(CLIENT_STR(ERR_GUILD_RANK_IN_USE)); break;
                case 19: socialError(fmtArgs(CLIENT_STR(ERR_IGNORING_YOU_S), {n})); break;  // ERR_GUILD_IGNORING_YOU_S
                case 20:
                    rosterWanted_ = true;
                    ++guildVersion_;
                    break;
                case 25: socialError(CLIENT_STR(ERR_GUILD_WITHDRAW_LIMIT)); break;
                case 26: socialError(CLIENT_STR(ERR_GUILD_NOT_ENOUGH_MONEY)); break;
                case 28: socialError(CLIENT_STR(ERR_GUILD_BANK_FULL)); break;
                case 29: socialError(CLIENT_STR(ERR_ITEM_NOT_FOUND)); break;  // ERR_GUILD_ITEM_NOT_FOUND
                default: break;
            }
            return true;
        }
        case LogicalOpcode::SMSG_GUILD_INVITE: {
            const std::string inviter = packet.readString();
            const std::string guild = packet.readString();
            // GUILD_INVITE_REQUEST(inviter, guild) -> StaticPopup GUILD_INVITE; ERR_INVITED_TO_GUILD_SSS(inviter,
            // inviter, guild) - the ruRU string's plain %s puts the inviter first and the guild in the link (sound
            // LEVELUP).
            guildInviter_ = inviter;
            guildInviteGuild_ = guild;
            ++guildInviteSeq_;
            guildInvitePending_ = true;  // until AcceptGuild / DeclineGuild
            socialLine(fmtArgs(CLIENT_STR(ERR_INVITED_TO_GUILD_SSS), {inviter, inviter, guild}));
            PSP_LOGI(tag::Net, "guild_invite from=%s guild=%s", inviter.c_str(), guild.c_str());
            return true;
        }
        case LogicalOpcode::SMSG_GUILD_DECLINE: {
            socialLine(fmtArgs(CLIENT_STR(ERR_GUILD_DECLINE_S), {packet.readString()}));
            return true;
        }
        case LogicalOpcode::SMSG_GUILD_INFO: {
            social::GuildInfo gi;
            if (!social::readGuildInfo(packet, gi)) return true;
            int y = 0, mo = 0, d = 0;
            social::unpackDate(gi.packedDate, y, mo, d);
            socialLine(fmtArgs(CLIENT_STR(GUILD_NAME_TEMPLATE), {gi.name}));
            socialLine(fmtArgs(CLIENT_STR(GUILD_INFO_TEMPLATE), {std::to_string(mo), std::to_string(d), std::to_string(y),
                                                    std::to_string(gi.members), std::to_string(gi.accounts)}));
            return true;
        }
        default:
            return false;
    }
}

}  // namespace wowee::platform::psp
