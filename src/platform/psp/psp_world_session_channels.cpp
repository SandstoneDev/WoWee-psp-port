// The chat channels of the PSP world session (the packets in psp_glyph_bank_parse.hpp):
//   - the list: the joined channels in join order, numbered 1..10 by their place (upstream ChatHandler
//     joinedChannels_ / getChannelIndex; the chat frame's limit of 10, ChatFrame.lua). A channel is listed when the
//     realm says YOU_JOINED and dropped on YOU_LEFT (upstream handleChannelNotify); an 11th join is refused before
//     it is sent with ERR_TOO_MANY_CHAT_CHANNELS on the red error line;
//   - JoinChannel (JoinPermanentChannel / JoinChannelByName / the world entry): a ChatChannels Shortcut_lang
//     ("General") becomes the zone's channel - Name_lang formatted with the zone's name, or the city word for a
//     CITY_ONLY channel in a capital (AzerothCore Player::UpdateLocalChannels) - then CMSG_JOIN_CHANNEL (id, 0, 0,
//     name, password; upstream JoinChannelPacket). Every request is sent (upstream joinChannel); the realm answers
//     an existing membership itself;
//   - the world entry (once, when the zone's name is known): the ChatChannels rows of the auto-join set are joined
//     (upstream autoJoinDefaultChannels; the default set is the rows AzerothCore flags CHANNEL_DBC_FLAG_INITIAL -
//     General, Trade, LocalDefense, LookingForGroup). The set is wowee.cfg "autojoin_channels": leaving a standard
//     channel takes it out, joining one puts it back (the port's own rule; the PSP has no checkbox for it);
//   - LeaveChannelByName: a number or a name (a shortcut names the zone's channel of its row); the request is sent
//     (upstream leaveChannel);
//   - SMSG_CHANNEL_NOTIFY: the notices as CHAT_MSG_CHANNEL_NOTICE / _NOTICE_USER / _JOIN / _LEAVE. YOU_JOINED with
//     the ChatChannels id of a listed channel replaces that channel in its place and says YOU_CHANGED (AzerothCore
//     UpdateLocalChannels: a zone channel "already replaced at client"; Channel.h CHAT_YOU_CHANGED_NOTICE);
//     PLAYER_ALREADY_MEMBER about us lists a channel the list lacks (upstream); WRONG_PASSWORD asks for the password
//     (CHANNEL_PASSWORD_REQUEST), INVITE asks to join (CHANNEL_INVITE_REQUEST, one at a time - the realm drops the
//     invites of an ignored player: AzerothCore Channel::Invite); MODE_CHANGE -> SET / UNSET_MODERATOR / _VOICE;
//   - SMSG_CHANNEL_LIST: the roster of the channel ChannelFrame shows (the selected one);
//   - the lines: format(CHAT_<key>_NOTICE_BN or CHAT_<key>_NOTICE, number, "N. Name"[, player[, player 2]])
//     (ChatFrame.lua). Other players' joins / leaves are shown for a listed channel only (ChatFrame.lua); the
//     other notices are shown for any channel - one not listed is named without a number (upstream prints them all);
//   - names are compared ignoring case as the realm folds them (AzerothCore ChannelMgr: wstrToLower).
// PSP: one chat frame; no voice (SMSG_USERLIST_*, the voice flags are read past); /chatlist prints the names after
// the channel.
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::platform::psp {

namespace {

using game::LogicalOpcode;
constexpr size_t kMaxChannels = 10;      // the chat frame's channel limit (ChatFrame.lua)
constexpr size_t kMaxRoster = 500;       // the roster kept (PSP bound)
constexpr uint32_t kNameWaitMs = 3000;   // a notice waits this long for its players' names

// CHAT_<key>_NOTICE_BN / CHAT_<key>_NOTICE (GlobalStrings, the _BN one when it exists - ChatFrame_OnEvent;
// strings.pak of the running language).
struct NoticeText {
    const char* key;
    CS text;
};
constexpr NoticeText kNotices[] = {
    {"YOU_JOINED", CS::CHAT_YOU_JOINED_NOTICE_BN},
    {"YOU_CHANGED", CS::CHAT_YOU_CHANGED_NOTICE_BN},
    {"YOU_LEFT", CS::CHAT_YOU_LEFT_NOTICE_BN},
    {"WRONG_PASSWORD", CS::CHAT_WRONG_PASSWORD_NOTICE},
    {"NOT_MEMBER", CS::CHAT_NOT_MEMBER_NOTICE},
    {"NOT_MODERATOR", CS::CHAT_NOT_MODERATOR_NOTICE_BN},
    {"PASSWORD_CHANGED", CS::CHAT_PASSWORD_CHANGED_NOTICE_BN},
    {"OWNER_CHANGED", CS::CHAT_OWNER_CHANGED_NOTICE_BN},
    {"PLAYER_NOT_FOUND", CS::CHAT_PLAYER_NOT_FOUND_NOTICE_BN},
    {"NOT_OWNER", CS::CHAT_NOT_OWNER_NOTICE_BN},
    {"CHANNEL_OWNER", CS::CHAT_CHANNEL_OWNER_NOTICE_BN},
    {"SET_MODERATOR", CS::CHAT_SET_MODERATOR_NOTICE_BN},
    {"UNSET_MODERATOR", CS::CHAT_UNSET_MODERATOR_NOTICE_BN},
    {"SET_VOICE", CS::CHAT_SET_VOICE_NOTICE_BN},
    {"UNSET_VOICE", CS::CHAT_UNSET_VOICE_NOTICE_BN},
    {"ANNOUNCEMENTS_ON", CS::CHAT_ANNOUNCEMENTS_ON_NOTICE_BN},   // (sic: the _BN ones
    {"ANNOUNCEMENTS_OFF", CS::CHAT_ANNOUNCEMENTS_OFF_NOTICE_BN},   //  are swapped in ruRU)
    {"MODERATION_ON", CS::CHAT_MODERATION_ON_NOTICE_BN},
    {"MODERATION_OFF", CS::CHAT_MODERATION_OFF_NOTICE_BN},
    {"MUTED", CS::CHAT_MUTED_NOTICE_BN},
    {"PLAYER_KICKED", CS::CHAT_PLAYER_KICKED_NOTICE_BN},
    {"BANNED", CS::CHAT_BANNED_NOTICE},
    {"PLAYER_BANNED", CS::CHAT_PLAYER_BANNED_NOTICE_BN},
    {"PLAYER_UNBANNED", CS::CHAT_PLAYER_UNBANNED_NOTICE_BN},
    {"PLAYER_NOT_BANNED", CS::CHAT_PLAYER_NOT_BANNED_NOTICE_BN},
    {"PLAYER_ALREADY_MEMBER", CS::CHAT_PLAYER_ALREADY_MEMBER_NOTICE_BN},
    {"INVITE", CS::CHAT_INVITE_NOTICE},
    {"INVITE_WRONG_FACTION", CS::CHAT_INVITE_WRONG_FACTION_NOTICE},
    {"WRONG_FACTION", CS::CHAT_WRONG_FACTION_NOTICE},
    {"INVALID_NAME", CS::CHAT_INVALID_NAME_NOTICE},
    {"NOT_MODERATED", CS::CHAT_NOT_MODERATED_NOTICE},
    {"PLAYER_INVITED", CS::CHAT_PLAYER_INVITED_NOTICE_BN},
    {"PLAYER_INVITE_BANNED", CS::CHAT_PLAYER_INVITE_BANNED_NOTICE_BN},
    {"THROTTLED", CS::CHAT_THROTTLED_NOTICE_BN},
    {"NOT_IN_AREA", CS::CHAT_NOT_IN_AREA_NOTICE},
    {"VOICE_ON", CS::CHAT_VOICE_ON_NOTICE_BN},
    {"VOICE_OFF", CS::CHAT_VOICE_OFF_NOTICE_BN},
};

const char* noticeText(const std::string& key) {
    for (const NoticeText& n : kNotices)
        if (key == n.key) return cs(n.text);
    return nullptr;  // a key without a string here (NOT_IN_LFG): no line
}

// Lua's string.format for the %s / %d / %N$s / %N$d of these strings (a number for %s prints as Lua prints it).
std::string luaFormat(const char* f, const std::vector<std::string>& args) {
    std::string out;
    size_t next = 0;
    for (const char* p = f; *p; ++p) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        if (p[1] == '%') {
            out += '%';
            ++p;
            continue;
        }
        size_t idx = next;
        const char* q = p + 1;
        if (std::isdigit(static_cast<unsigned char>(*q)) && q[1] == '$') {
            idx = static_cast<size_t>(*q - '1');
            q += 2;
        } else {
            ++next;
        }
        if (*q == 's' || *q == 'd') {
            if (idx < args.size()) out += args[idx];
            p = q;
        } else {
            out += *p;
        }
    }
    return out;
}

// The chat shows a link's text: |Hchannel:..|h[x]|h -> [x]; |3-N(x) -> x (the declension of a number stays).
std::string stripLinks(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '|' && i + 1 < s.size()) {
            if (s[i + 1] == 'H') {
                const size_t e = s.find("|h", i + 2);
                if (e != std::string::npos) {
                    i = e + 1;
                    continue;
                }
            } else if (s[i + 1] == 'h') {
                ++i;
                continue;
            } else if (s[i + 1] == '3' && i + 3 < s.size() && s[i + 2] == '-') {
                const size_t open = s.find('(', i);
                const size_t close = open == std::string::npos ? open : s.find(')', open);
                if (close != std::string::npos) {
                    out += s.substr(open + 1, close - open - 1);
                    i = close;
                    continue;
                }
            }
        }
        out += s[i];
    }
    return out;
}

// Lower case as the realm folds channel names (AzerothCore Util.h wcharToLower, used by ChannelMgr's
// wstrToLower): A-Z, U+00C0..U+00D6, U+00D8..U+00DE, the even letters U+0100..U+012E, U+0401, U+0410..U+042F;
// UTF-8 of one or two bytes (the longer forms are kept as they are).
std::string lowerUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>(std::tolower(c));
        } else if (c >= 0xC2 && c <= 0xDF && i + 1 < s.size()) {
            int cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            if ((cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xDE) || (cp >= 0x410 && cp <= 0x42F)) cp += 0x20;
            else if (cp >= 0x100 && cp <= 0x12E && cp % 2 == 0) cp += 1;
            else if (cp == 0x401) cp = 0x451;
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool sameText(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lowerUtf8(a) == lowerUtf8(b);
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

}  // namespace

// The baked city word is not used: the port's own word (OWN CHANNEL_CITY_WORD) names the city channels.
void WorldSession::setChatChannelData(std::vector<gbk::ChatChannelRow> rows, std::string /*cityName*/) {
    chanRows_ = std::move(rows);
    chanCityName_ = OWN_STR(CHANNEL_CITY_WORD);
}

void WorldSession::setChannelZone(uint32_t zoneId, const std::string& zoneName, bool city) {
    chanZoneId_ = zoneId;
    chanZoneName_ = zoneName;
    chanZoneCity_ = city;
}

const WorldSession::ChatChannel* WorldSession::chatChannelByNumber(int number) const {
    for (const ChatChannel& c : channels_)
        if (c.number == number) return &c;
    return nullptr;
}

const WorldSession::ChatChannel* WorldSession::chatChannelByName(const std::string& name) const {
    for (const ChatChannel& c : channels_)
        if (sameText(c.name, name)) return &c;
    return nullptr;
}

int WorldSession::channelNumberOf(const std::string& name) const {
    const ChatChannel* c = chatChannelByName(name);
    return c ? c->number : 0;
}

std::string WorldSession::channelDisplay(const ChatChannel& c) const {
    return std::to_string(c.number) + ". " + c.name;
}

// A listed channel: by its ChatChannels id (one zone channel per row), else by name.
WorldSession::ChatChannel* WorldSession::channelSlotFor(uint32_t id, const std::string& name) {
    if (id != 0)
        for (ChatChannel& c : channels_)
            if (c.channelId == id) return &c;
    for (ChatChannel& c : channels_)
        if (sameText(c.name, name)) return &c;
    return nullptr;
}

void WorldSession::renumberChannels() {
    for (size_t i = 0; i < channels_.size(); ++i) channels_[i].number = static_cast<int>(i) + 1;
}

void WorldSession::channelChatLine(const std::string& text, game::ChatType type) {
    game::MessageChatData m;
    m.type = type;
    m.language = game::ChatLanguage::UNIVERSAL;
    m.message = text;
    addLocalChatMessage(m);
}

bool WorldSession::joinChannel(const std::string& nameIn, const std::string& password) {
    if (!socket_ || state_ != State::InWorld) return false;
    const std::string name = nameIn.substr(0, nameIn.find(' '));
    if (name.empty() || name.size() >= 128 || allDigits(name)) return false;
    std::string full = name;
    uint32_t id = 0;
    for (const gbk::ChatChannelRow& r : chanRows_) {
        if (!sameText(r.shortcut, name)) continue;
        full = gbk::channelFullName(r, chanZoneName_, chanZoneCity_, chanCityName_);
        if (full.empty()) return false;  // no zone name yet: the zone channel cannot be named
        id = r.id;
        break;
    }
    if (!channelSlotFor(id, full) && channels_.size() >= kMaxChannels) {
        addUiError(CLIENT_STR(ERR_TOO_MANY_CHAT_CHANNELS));
        return true;
    }
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_JOIN_CHANNEL);
    if (wire != 0xFFFF) {
        network::Packet p(wire);
        gbk::writeJoinChannel(p, id, full, id != 0 ? std::string() : password);
        socket_->send(p);
    }
    setAutoJoin(id, true);
    if (++chanLogs_ <= 200) PSP_LOGI(tag::Net, "channel_join id=%u name=%s", static_cast<unsigned>(id), full.c_str());
    return true;
}

void WorldSession::leaveChannel(const std::string& nameOrNumber) {
    if (!socket_ || state_ != State::InWorld) return;
    std::string name = nameOrNumber.substr(0, nameOrNumber.find(' '));
    if (allDigits(name)) {  // a number names its channel
        const ChatChannel* c = chatChannelByNumber(std::atoi(name.c_str()));
        if (!c) return;
        name = c->name;
    }
    if (name.empty()) return;
    uint32_t id = 0;
    for (const gbk::ChatChannelRow& r : chanRows_)
        if (sameText(r.shortcut, name) || sameText(r.name, name)) {
            const std::string full = gbk::channelFullName(r, chanZoneName_, chanZoneCity_, chanCityName_);
            if (!full.empty()) name = full;
            id = r.id;
            break;
        }
    if (const ChatChannel* c = channelSlotFor(id, name)) {
        name = c->name;
        id = c->channelId;
    }
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_LEAVE_CHANNEL);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeLeaveChannel(p, id, name);
    socket_->send(p);
    setAutoJoin(id, false);
    PSP_LOGI(tag::Net, "channel_leave id=%u name=%s", static_cast<unsigned>(id), name.c_str());
}

// The auto-join set (wowee.cfg "autojoin_channels", a bit per ChatChannels id 1..32): the port's own rule - a
// standard channel the player leaves is not joined at the next world entry until he joins it again.
void WorldSession::setAutoJoin(uint32_t channelId, bool on) {
    if (!chanMaskRead_ || channelId == 0 || channelId > 32) return;
    const uint32_t bit = 1u << (channelId - 1);
    const uint32_t mask = on ? (autoJoinMask_ | bit) : (autoJoinMask_ & ~bit);
    if (mask == autoJoinMask_) return;
    autoJoinMask_ = mask;
    char v[16];
    std::snprintf(v, sizeof(v), "%u", static_cast<unsigned>(autoJoinMask_));
    configSet("autojoin_channels", v);
    configSave();
}

void WorldSession::listChannel(const std::string& nameOrNumber) {  // ListChannelByName: CMSG_CHANNEL_LIST
    if (!socket_ || state_ != State::InWorld) return;
    std::string name = nameOrNumber.substr(0, nameOrNumber.find(' '));
    if (allDigits(name)) {
        const ChatChannel* c = chatChannelByNumber(std::atoi(name.c_str()));
        if (!c) return;
        name = c->name;
    }
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_CHANNEL_LIST);
    if (wire == 0xFFFF || name.empty()) return;
    network::Packet p(wire);
    p.writeString(name);
    socket_->send(p);
    chanListShow_ = name;
}

void WorldSession::channelMemberCommand(LogicalOpcode op, const std::string& channel, const std::string& player) {
    if (!socket_ || state_ != State::InWorld || channel.empty() || player.empty()) return;
    const uint16_t wire = game::wireOpcode(op);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    p.writeString(channel);
    p.writeString(player);
    socket_->send(p);
    if (++chanLogs_ <= 200) PSP_LOGI(tag::Net, "channel_cmd op=%X channel=%s", static_cast<unsigned>(wire), channel.c_str());
}

void WorldSession::channelSimpleCommand(LogicalOpcode op, const std::string& channel, const std::string& arg) {
    if (!socket_ || state_ != State::InWorld || channel.empty()) return;
    const uint16_t wire = game::wireOpcode(op);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    p.writeString(channel);
    if (op == LogicalOpcode::CMSG_CHANNEL_PASSWORD) p.writeString(arg);
    socket_->send(p);
    if (++chanLogs_ <= 200) PSP_LOGI(tag::Net, "channel_cmd op=%X channel=%s", static_cast<unsigned>(wire), channel.c_str());
}

void WorldSession::declineChannelInvite(const std::string& channel) {  // DeclineInvite
    if (!socket_ || state_ != State::InWorld) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_DECLINE_CHANNEL_INVITE);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    p.writeString(channel);
    socket_->send(p);
}

bool WorldSession::sendChannelMessage(int number, const std::string& text) {
    const ChatChannel* c = chatChannelByNumber(number);
    if (!c || text.empty()) return false;
    sendChatMessage(game::ChatType::CHANNEL, text, c->name);
    return true;
}

void WorldSession::requestChannelRoster(const std::string& channel) {  // SetSelectedDisplayChannel -> 0x3D2
    rosterChannel_ = channel;
    roster2_.clear();
    rosterChannelFlags_ = 0;
    ++rosterVersion_;
    if (!socket_ || state_ != State::InWorld || channel.empty()) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_CHANNEL_DISPLAY_LIST);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    p.writeString(channel);
    socket_->send(p);
}

uint8_t WorldSession::ownChannelFlags() const {
    for (const gbk::ChannelMember& m : roster2_)
        if (m.guid == playerGuid_) return m.flags;
    return 0;
}

std::string WorldSession::channelMemberName(uint64_t guid) {
    if (guid == 0) return std::string();
    if (const std::string* n = nameOf(guid)) return *n;
    askName(guid);
    return std::string();
}

bool WorldSession::takeChannelInvite(std::string& channel, std::string& inviter) {
    if (!chanInviteNew_) return false;
    chanInviteNew_ = false;
    channel = chanInvite_;
    inviter = chanInviter_;
    return true;
}

bool WorldSession::takeChannelPasswordRequest(std::string& channel) {
    if (!chanPasswordNew_) return false;
    chanPasswordNew_ = false;
    channel = chanPasswordAsk_;
    return true;
}

// The notice as ChatFrame_OnEvent prints it, once the names of its players are known; false while one is asked.
bool WorldSession::channelNoticeReady(const gbk::ChannelNotify& n, const std::string& key) {
    std::string p1 = n.name, p2;
    if (n.guid != 0) {
        const std::string* a = nameOf(n.guid);
        if (!a) {
            askName(n.guid);
            return false;
        }
        p1 = *a;
    }
    if (n.guid2 != 0) {
        const std::string* b = nameOf(n.guid2);
        if (!b) {
            askName(n.guid2);
            return false;
        }
        p2 = *b;
    }
    const ChatChannel* c = chatChannelByName(n.channel);
    if (!c && n.channelId != 0)
        for (const ChatChannel& x : channels_)
            if (x.channelId == n.channelId) c = &x;
    const bool invite = key == "INVITE";
    if (!c && (n.type == 0x00 || n.type == 0x01)) return true;  // others' joins / leaves: listed channels only
    if (invite) c = nullptr;  // an invitation names the channel as the packet does
    const std::string num = c ? std::to_string(c->number) : std::string("0");
    const std::string arg4 = c ? channelDisplay(*c) : n.channel;  // not listed: the name alone
    std::string text;
    if (n.type == 0x00 || n.type == 0x01) {  // CHAT_CHANNEL_JOIN_GET / CHAT_CHANNEL_LEAVE_GET after "[N. Name] "
        text = "[" + arg4 + "] " + (n.type == 0x00 ? CLIENT_FMT(CHAT_CHANNEL_JOIN_GET, "[" + p1 + "]")
                                                  : CLIENT_FMT(CHAT_CHANNEL_LEAVE_GET, "[" + p1 + "]"));
        channelChatLine(text, n.type == 0x00 ? game::ChatType::CHANNEL_JOIN : game::ChatType::CHANNEL_LEAVE);
        return true;
    }
    const char* f = noticeText(key);
    if (!f) return true;
    if (invite) text = luaFormat(f, {n.channel, p1});                                // format(G, arg4, arg2)
    else if (n.event == gbk::ChanEvent::NoticeUser && n.guid2 != 0) text = luaFormat(f, {num, arg4, p1, p2});
    else if (n.event == gbk::ChanEvent::NoticeUser) text = luaFormat(f, {num, arg4, p1});
    else text = luaFormat(f, {num, arg4});
    // The colour: CHANNEL_NOTICE (the chat draws it in the channel's colour); INVITE keeps CHANNEL_NOTICE_USER's.
    channelChatLine(stripLinks(text), invite ? game::ChatType::CHANNEL_NOTICE_USER : game::ChatType::CHANNEL_NOTICE);
    return true;
}

bool WorldSession::handleChannelPacket(LogicalOpcode op, network::Packet& packet) {
    switch (op) {
        case LogicalOpcode::SMSG_CHANNEL_NOTIFY: {
            gbk::ChannelNotify n;
            if (!gbk::readChannelNotify(packet, n)) return true;
            if (++chanLogs_ <= 300)
                PSP_LOGI(tag::Net, "channel_notify type=%u channel=%s id=%u", static_cast<unsigned>(n.type),
                         n.channel.c_str(), static_cast<unsigned>(n.channelId));
            std::string key = n.key;
            ChatChannel* c = channelSlotFor(n.channelId, n.channel);
            bool drop = false;
            switch (n.type) {
                case 0x00:
                case 0x01:
                    if (!c) return true;  // an unlisted channel's join / leave is not shown
                    break;
                case 0x02:  // YOU_JOINED: listed at the end, or a zone channel replaced in its place (YOU_CHANGED)
                    if (c && n.channelId != 0 && !sameText(c->name, n.channel)) key = "YOU_CHANGED";
                    if (!c) {
                        if (channels_.size() >= kMaxChannels) {
                            PSP_LOGW(tag::Net, "channel_over_limit name=%s", n.channel.c_str());
                            break;
                        }
                        ChatChannel nc;
                        nc.channelId = n.channelId;
                        channels_.push_back(nc);
                        renumberChannels();
                        c = &channels_.back();
                    }
                    c->name = n.channel;
                    c->flags = n.flags;
                    ++chanVersion_;
                    break;
                case 0x03:  // YOU_LEFT: the channel dropped after its line
                    if (c) {
                        drop = true;
                        if (sameText(c->name, rosterChannel_)) {
                            rosterChannel_.clear();
                            roster2_.clear();
                            ++rosterVersion_;
                        }
                    }
                    break;
                case 0x04:  // WRONG_PASSWORD: CHANNEL_PASSWORD_REQUEST
                    chanPasswordAsk_ = n.channel;
                    chanPasswordNew_ = true;
                    break;
                case 0x0C: {  // MODE_CHANGE: the flags in the shown roster, the lines
                    for (gbk::ChannelMember& m : roster2_)
                        if (m.guid == n.guid && (m.guid == playerGuid_ || sameText(n.channel, rosterChannel_)))
                            m.flags = n.newFlags;
                    ++rosterVersion_;
                    for (const char* k : gbk::modeChangeKeys(n.flags, n.newFlags))
                        chanPending_.push_back(ChanPendingLine{n, k, logUptimeMs() + kNameWaitMs});
                    return true;
                }
                case 0x17:  // PLAYER_ALREADY_MEMBER about us: a membership the list lacks (upstream)
                    if (!c && n.guid == playerGuid_ && channels_.size() < kMaxChannels) {
                        ChatChannel nc;
                        nc.name = n.channel;
                        channels_.push_back(nc);
                        renumberChannels();
                        ++chanVersion_;
                    }
                    break;
                case 0x18: {  // INVITE: one at a time (StaticPopup CHAT_CHANNEL_INVITE), the inviter's name first
                    if (!chanInvite_.empty() && logUptimeMs() - chanInviteMs_ < 60000u) return true;
                    chanInvite_ = n.channel;
                    chanInviterGuid_ = n.guid;
                    chanInviter_.clear();
                    chanInviteMs_ = logUptimeMs();
                    break;
                }
                default: break;
            }
            chanPending_.push_back(ChanPendingLine{n, key, logUptimeMs() + kNameWaitMs});
            channelUpdate();  // a line without names goes out now (the channel is still listed)
            if (drop) {
                const std::string gone = c->name;
                channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                               [&](const ChatChannel& x) { return sameText(x.name, gone); }),
                                channels_.end());
                renumberChannels();
                ++chanVersion_;
            }
            return true;
        }
        case LogicalOpcode::SMSG_CHANNEL_LIST: {
            gbk::ChannelList l;
            if (!gbk::readChannelList(packet, l, kMaxRoster)) return true;
            for (const gbk::ChannelMember& m : l.members) askName(m.guid);
            if (!rosterChannel_.empty() && sameText(l.channel, rosterChannel_)) {  // the shown channel
                roster2_ = l.members;
                rosterChannelFlags_ = l.channelFlags;
                ++rosterVersion_;
            }
            if (ChatChannel* c = channelSlotFor(0, l.channel)) c->members = static_cast<uint32_t>(l.members.size());
            if (!chanListShow_.empty() && sameText(l.channel, chanListShow_)) {
                // PSP: "[N. Name] " + the names (the owner '*', the moderators '@'), after the names are known.
                chanListShow_.clear();
                std::string names;
                for (const gbk::ChannelMember& m : l.members) {
                    const std::string* nm = nameOf(m.guid);
                    if (!names.empty()) names += ", ";
                    names += (m.flags & gbk::kMemberOwner) ? "*" : (m.flags & gbk::kMemberModerator) ? "@" : "";
                    names += nm ? *nm : std::string("?");
                }
                const ChatChannel* c = chatChannelByName(l.channel);
                const std::string head = c ? "[" + channelDisplay(*c) + "] " : "[" + l.channel + "] ";
                channelChatLine(head + names, game::ChatType::CHANNEL_LIST);
            }
            PSP_LOGI(tag::Net, "channel_list channel=%s n=%u", l.channel.c_str(), static_cast<unsigned>(l.members.size()));
            return true;
        }
        case LogicalOpcode::SMSG_CHANNEL_MEMBER_COUNT:  // read and ignored (upstream)
        case LogicalOpcode::SMSG_USERLIST_ADD:          // the voice roster (PSP: no voice)
        case LogicalOpcode::SMSG_USERLIST_REMOVE:
        case LogicalOpcode::SMSG_USERLIST_UPDATE:
            return true;
        case LogicalOpcode::SMSG_LOGIN_VERIFY_WORLD:  // peek: the world entry - the joins go out when the zone is known
            return false;
        default: return false;
    }
}

// Every frame: the world entry's joins (once, when the zone's name is known), the notices whose names came.
void WorldSession::channelUpdate() {
    if (!chanMaskRead_ && !chanRows_.empty()) {
        chanMaskRead_ = true;
        uint32_t initial = 0;  // the default: the CHANNEL_DBC_FLAG_INITIAL rows (ids up to 32)
        for (const gbk::ChatChannelRow& r : chanRows_)
            if (r.id >= 1 && r.id <= 32 && (r.flags & gbk::kChanInitial)) initial |= 1u << (r.id - 1);
        const std::string v = configGet("autojoin_channels", "");
        autoJoinMask_ = v.empty() ? initial : static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    }
    if (!chanWorldJoinDone_ && chanMaskRead_ && state_ == State::InWorld && !chanZoneName_.empty() && socket_) {
        chanWorldJoinDone_ = true;
        int n = 0;
        for (const gbk::ChatChannelRow& r : chanRows_)
            if (r.id >= 1 && r.id <= 32 && (autoJoinMask_ & (1u << (r.id - 1))) && joinChannel(r.shortcut)) ++n;
        PSP_LOGI(tag::Net, "channel_world_join zone=%u mask=%X n=%d", static_cast<unsigned>(chanZoneId_),
                 static_cast<unsigned>(autoJoinMask_), n);
    }
    if (!chanInvite_.empty() && chanInviter_.empty() && chanInviterGuid_ != 0) {  // CHANNEL_INVITE_REQUEST
        if (const std::string* nm = nameOf(chanInviterGuid_)) {
            chanInviter_ = *nm;
            chanInviteNew_ = true;
            chanInviterGuid_ = 0;
        }
    }
    if (!chanInvite_.empty() && chanInviterGuid_ == 0 && !chanInviteNew_ && logUptimeMs() - chanInviteMs_ > 60000u)
        chanInvite_.clear();  // the popup's 60 s
    const uint32_t now = logUptimeMs();
    for (size_t i = 0; i < chanPending_.size();) {
        if (channelNoticeReady(chanPending_[i].n, chanPending_[i].key) || now > chanPending_[i].deadlineMs) {
            chanPending_.erase(chanPending_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    if (chanPending_.size() > 32) chanPending_.erase(chanPending_.begin());
}

}  // namespace wowee::platform::psp
