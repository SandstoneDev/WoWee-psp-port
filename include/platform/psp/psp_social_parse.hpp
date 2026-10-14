// Social: the friends / ignore / who / guild packets read and written as the 3.3.5 servers (AzerothCore
// SocialMgr.cpp, MiscHandler.cpp, Guild.cpp / GuildPackets.cpp) write and read them, and the port's rules around
// them - pure (network::Packet only), so the host test (tools/psp/host_tests/social_test.cpp) runs the same code as
// the PSP session (psp_world_session_social.cpp).
//
//   SMSG_CONTACT_LIST   u32 listMask, u32 count; per contact u64 guid, u32 flags, string note; flags & 1:
//                                u8 status, status != 0: u32 area, u32 level, u32 class (a 16-byte trailer: not read)
//   SMSG_FRIEND_STATUS  u8 result, u64 guid; 2 ONLINE: u8 status, u32 area, level, class; 6 ADDED_ONLINE:
//                                string note + the same; 7 ADDED_OFFLINE: string note
//   SMSG_WHO            u32 displayCount, u32 total; per entry string name, string guild, u32 level, class,
//                                race, u8 gender, u32 zone (the first 50 kept)
//   SMSG_GUILD_ROSTER   u32 members, string MOTD (128 chars, one line), string info (500 chars),
//                                u32 ranks; per rank u32 rights, u32 money per day, 6 x {u32 tab flags, u32 tab slots};
//                                per member u64 guid, u8 flags (!= 0 online; 2 AFK, 4 DND), string name, u32 rank,
//                                u8 level, u8 class, u8 gender, u32 zone, offline only: float days, string note,
//                                string officer note (31 chars each, one line)
//   MSG_GUILD_PERMISSIONS  u32 rank, u32 rights, u32 money per day, u8 purchased tabs, 6 x {flags, slots}
//   MSG_GUILD_EVENT_LOG_QUERY  u8 count (<= 100); per entry u8 type, u64 guid1, u64 guid2 (not for types
//                                2 / 6), u8 rank (types 3 / 4), u32 seconds ago
//   SMSG_GUILD_QUERY_RESPONSE  u32 id, string name, 10 x string rank, u32 emblem style, emblem colour, border
//                                style, border colour, background colour, u32 rank count
//   SMSG_GUILD_EVENT    u8 type, u8 count, the strings (one line each), u64 guid for types 3, 4, 12, 13
//   SMSG_GUILD_COMMAND_RESULT  u32 command, string name, u32 result
//   SMSG_GUILD_INVITE   string inviter, string guild;  SMSG_GUILD_DECLINE  string name
//   SMSG_GUILD_INFO     string guild, u32 packed date, u32 members, u32 accounts
//   CMSG_WHO (SendWho)  u32 min, u32 max, string name, string guild, u32 race mask, u32 class mask, u32 zone
//                                count + zone ids, u32 string count (<= 4) + strings
#pragma once

#include "network/packet.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace wowee::platform::psp::social {

// --- Text helpers --------------------------------------------------------------------------------------------------

// A capped copy: at most maxChars UTF-8 characters; a one-line text (oneLine) has its CR / LF turned into spaces
// (the port's rule for the MOTD, notes and event strings - a chat line or a roster row is one line).
inline std::string capText(const std::string& s, size_t maxChars, bool oneLine) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < s.size() && chars < maxChars;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + len > s.size()) len = s.size() - i;
        if (oneLine && (c == '\n' || c == '\r')) out.push_back(' ');
        else out.append(s, i, len);
        i += len;
        ++chars;
    }
    return out;
}

// Name order: standard Russian alphabetical order, CLDR collation "ru" (common/collation/ru.xml: the root order with
// [reorder Cyrl]). The port's simple form of it, for names and tags:
//   - case is ignored (Unicode simple case mapping: a-z, U+0430..U+044F and U+0450..U+045F to their capitals);
//   - primary order: ASCII non-letters (by code point), then the Cyrillic letters A..YA (Cyrillic first, [reorder
//     Cyrl]), then Latin A..Z, then everything else by code point (other Cyrillic letters after YA);
//   - U+0401 / U+0451 (IO) is primary-equal to U+0415 (IE) - CLDR root: IE + diaeresis - and differs only at the
//     secondary level: of two names equal at the primary level, the one with IE first sorts first.
// foldWeight is the primary weight of one code point (equal weights = the same letter for search and tags).
inline uint32_t foldWeight(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') cp -= 32;
    else if (cp >= 0x430 && cp <= 0x44F) cp -= 0x20;
    else if (cp >= 0x450 && cp <= 0x45F) cp -= 0x50;
    if (cp == 0x401) cp = 0x415;                                  // IO = IE at the primary level
    if (cp < 0x80 && !(cp >= 'A' && cp <= 'Z')) return cp;        // ASCII non-letters first
    if (cp >= 0x410 && cp <= 0x42F) return 0x1000u + (cp - 0x410);  // Cyrillic A..YA
    if (cp >= 'A' && cp <= 'Z') return 0x2000u + (cp - 'A');      // then Latin
    return 0x10000u + cp;                                         // everything else by code point
}
// The secondary weight: 1 for IO (U+0401 / U+0451), 0 for every other code point.
inline uint32_t secondaryWeight(uint32_t cp) { return (cp == 0x401 || cp == 0x451) ? 1u : 0u; }

inline uint32_t nextCodePoint(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    size_t len = 1;
    if (c >= 0xF0) cp = c & 0x07, len = 4;
    else if (c >= 0xE0) cp = c & 0x0F, len = 3;
    else if (c >= 0xC0) cp = c & 0x1F, len = 2;
    for (size_t k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += len;
    return cp;
}

// Names compared in the order above: <0, 0, >0. Primary weights first (a shorter name that is a prefix sorts
// first), then the first secondary difference (IE before IO); case alone never makes two names differ.
inline int compareNames(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    int secondary = 0;
    while (i < a.size() && j < b.size()) {
        const uint32_t ca = nextCodePoint(a, i), cb = nextCodePoint(b, j);
        const uint32_t x = foldWeight(ca), y = foldWeight(cb);
        if (x != y) return x < y ? -1 : 1;
        if (secondary == 0) {
            const uint32_t sa = secondaryWeight(ca), sb = secondaryWeight(cb);
            if (sa != sb) secondary = sa < sb ? -1 : 1;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return secondary;
}

inline bool sameName(const std::string& a, const std::string& b) { return compareNames(a, b) == 0; }

// Substring match at the primary level - case ignored, IO matches IE (the who tags' zone / race / class search).
inline bool containsName(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    std::vector<uint32_t> h, n;
    for (size_t i = 0; i < hay.size();) h.push_back(foldWeight(nextCodePoint(hay, i)));
    for (size_t i = 0; i < needle.size();) n.push_back(foldWeight(nextCodePoint(needle, i)));
    return std::search(h.begin(), h.end(), n.begin(), n.end()) != h.end();
}

// `hay` begins with `needle`, ignoring case (the who tags' zone / race / class search - the port's own rule).
inline bool beginsWithName(const std::string& hay, const std::string& needle) {
    size_t i = 0, j = 0;
    while (j < needle.size()) {
        if (i >= hay.size()) return false;
        if (foldWeight(nextCodePoint(hay, i)) != foldWeight(nextCodePoint(needle, j))) return false;
    }
    return true;
}

// --- SMSG_CONTACT_LIST -----------------------------------------------------------------------------------------------

constexpr uint32_t kContactFriend = 0x1, kContactIgnore = 0x2, kContactMute = 0x4;
constexpr size_t kMaxFriends = 50, kMaxIgnores = 50;  // AzerothCore SocialMgr.h SOCIALMGR_FRIEND_LIMIT / _IGNORE_LIMIT

struct ContactRec {
    uint64_t guid = 0;
    uint32_t flags = 0;
    std::string note;
    uint8_t status = 0;  // FriendStatus: 0 offline, bit 1 online, 2 AFK, 4 DND, 8 RAF
    uint32_t area = 0, level = 0, cls = 0;
};

struct ContactList {
    uint32_t mask = 0;
    uint32_t count = 0;
    std::vector<ContactRec> contacts;
};

inline bool readContactList(network::Packet& p, ContactList& out) {
    out = ContactList{};
    if (!p.hasRemaining(8)) return false;
    out.mask = p.readUInt32();
    out.count = p.readUInt32();
    for (uint32_t i = 0; i < out.count; ++i) {
        if (!p.hasRemaining(12)) return false;
        ContactRec c;
        c.guid = p.readUInt64();
        c.flags = p.readUInt32();
        c.note = p.readString();
        if (c.flags & kContactFriend) {
            if (!p.hasRemaining(1)) return false;
            c.status = p.readUInt8();
            if (c.status != 0) {
                if (!p.hasRemaining(12)) return false;
                c.area = p.readUInt32();
                c.level = p.readUInt32();
                c.cls = p.readUInt32();
            }
        }
        if (out.contacts.size() < kMaxFriends + 2 * kMaxIgnores) out.contacts.push_back(std::move(c));
    }
    return true;
}

// --- SMSG_FRIEND_STATUS ----------------------------------------------------------------------------------------------

enum FriendResult : uint8_t {
    kFriendDbError = 0, kFriendListFull = 1, kFriendOnline = 2, kFriendOffline = 3, kFriendNotFound = 4,
    kFriendRemoved = 5, kFriendAddedOnline = 6, kFriendAddedOffline = 7, kFriendAlready = 8, kFriendSelf = 9,
    kFriendEnemy = 0x0A, kIgnoreFull = 0x0B, kIgnoreSelf = 0x0C, kIgnoreNotFound = 0x0D, kIgnoreAlready = 0x0E,
    kIgnoreAdded = 0x0F, kIgnoreRemoved = 0x10, kIgnoreAmbiguous = 0x11, kMuteFull = 0x12, kMuteSelf = 0x13,
    kMuteNotFound = 0x14, kMuteAlready = 0x15, kMuteAdded = 0x16, kMuteRemoved = 0x17, kMuteAmbiguous = 0x18,
};

struct FriendStatus {
    uint8_t result = 0;
    uint64_t guid = 0;
    std::string note;
    bool hasNote = false, hasInfo = false;
    uint8_t status = 0;
    uint32_t area = 0, level = 0, cls = 0;
};

// AzerothCore SocialMgr.cpp SendFriendStatus: the note for the two ADDED results, the status / area / level / class
// for ONLINE and ADDED_ONLINE; nothing more for the rest.
inline bool readFriendStatus(network::Packet& p, FriendStatus& out) {
    out = FriendStatus{};
    if (!p.hasRemaining(9)) return false;
    out.result = p.readUInt8();
    out.guid = p.readUInt64();
    if (out.result == kFriendAddedOnline || out.result == kFriendAddedOffline) {
        out.note = p.readString();
        out.hasNote = true;
    }
    if (out.result == kFriendOnline || out.result == kFriendAddedOnline) {
        if (!p.hasRemaining(13)) return false;
        out.status = p.readUInt8();
        out.area = p.readUInt32();
        out.level = p.readUInt32();
        out.cls = p.readUInt32();
        out.hasInfo = true;
    }
    return true;
}

// --- SMSG_WHO / CMSG_WHO -------------------------------------------------------------------------------------------

constexpr size_t kMaxWho = 50;  // friendsframe.lua MAX_WHOS_FROM_SERVER

struct WhoRec {
    std::string name, guild;
    uint32_t level = 0, cls = 0, race = 0, zone = 0;
    uint8_t gender = 0;
};

struct WhoList {
    uint32_t display = 0, total = 0;
    std::vector<WhoRec> list;
};

inline bool readWho(network::Packet& p, WhoList& out) {
    out = WhoList{};
    if (!p.hasRemaining(8)) return false;
    out.display = p.readUInt32();
    out.total = p.readUInt32();
    for (uint32_t i = 0; i < out.display; ++i) {
        WhoRec w;
        if (!p.hasRemaining(1)) return false;
        w.name = capText(p.readString(), 64, false);  // the port's memory bounds
        w.guild = capText(p.readString(), 128, false);
        if (!p.hasRemaining(17)) return false;
        w.level = p.readUInt32();
        w.cls = p.readUInt32();
        w.race = p.readUInt32();
        w.gender = p.readUInt8();
        w.zone = p.readUInt32();
        if (out.list.size() < kMaxWho) out.list.push_back(std::move(w));
    }
    return true;
}

struct WhoQuery {
    uint32_t minLevel = 0, maxLevel = 100;
    std::string name, guild;
    uint32_t raceMask = 0xFFFFFFFFu, classMask = 0xFFFFFFFFu;
    std::vector<uint32_t> zones;      // <= 10
    std::vector<std::string> strings; // <= 4
};

// The data SendWho's tags search (AreaTable zones without a parent, ChrRaces / ChrClasses names) and the tags.
struct WhoLookup {
    std::function<void(const std::string& text, std::vector<uint32_t>& zones)> zones;  // every zone beginning with text
    std::function<uint32_t(const std::string& text)> raceMask;                         // OR of 1 << race id
    std::function<uint32_t(const std::string& text)> classMask;                        // OR of 1 << class id
    std::function<std::string(uint32_t zone)> zoneName;                               // the zone sort key (by name)
    // GlobalStrings WHO_TAG_NAME / _GUILD / _ZONE / _RACE / _CLASS. The PSP sets the running language's from
    // strings.pak (WorldScreen::socialSetWhoLookup); these defaults are only the pure host test's fixture
    // (tools/psp/host_tests/social_parse_test.cpp), never shown.
    std::string tagName = "и-", tagGuild = "g-", tagZone = "з-", tagRace = "р-", tagClass = "к-";
};

// A level word: "N" (exactly N), "N-M", "N-" (N and above), "-M" (up to M). False: not one.
inline bool parseWhoLevels(const std::string& w, uint32_t& lo, uint32_t& hi) {
    const size_t dash = w.find('-');
    const std::string a = dash == std::string::npos ? w : w.substr(0, dash);
    const std::string b = dash == std::string::npos ? std::string() : w.substr(dash + 1);
    auto digits = [](const std::string& s) {
        return !s.empty() && s.size() <= 3 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if ((!a.empty() && !digits(a)) || (!b.empty() && !digits(b)) || (a.empty() && b.empty())) return false;
    if (dash == std::string::npos) {
        lo = hi = static_cast<uint32_t>(std::atoi(a.c_str()));
        return true;
    }
    lo = a.empty() ? 0u : static_cast<uint32_t>(std::atoi(a.c_str()));
    hi = b.empty() ? 100u : static_cast<uint32_t>(std::atoi(b.c_str()));
    return true;
}

// SendWho's text, the port's own reading of it: words split at spaces, "double quotes" keep spaces inside one word.
//   - a level word sets the level range (parseWhoLevels);
//   - a word that starts with a who tag (any case) sets that field: the name, the guild, the zones / races /
//     classes whose name starts with the rest (none: a filter that finds nobody);
//   - every other word is a free search word; at most 4 of them and 10 zones are sent (AzerothCore
//     HandleWhoOpcode reads no more).
inline WhoQuery parseWhoText(const std::string& text, const WhoLookup& lookup) {
    WhoQuery q;
    std::vector<std::string> words(1);
    bool quoted = false;
    for (char c : text) {
        if (c == '"') quoted = !quoted;
        else if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) words.emplace_back();
        else words.back() += c;
    }
    auto tagged = [](const std::string& w, const std::string& tag, std::string& rest) {  // w = tag + rest
        if (tag.empty() || !beginsWithName(w, tag)) return false;
        size_t i = 0;
        for (size_t j = 0; j < tag.size();) {
            nextCodePoint(tag, j);
            nextCodePoint(w, i);
        }
        rest = w.substr(i);
        return true;
    };
    bool zonesSet = false;
    for (const std::string& w : words) {
        if (w.empty()) continue;
        std::string rest;
        uint32_t lo = 0, hi = 0;
        if (tagged(w, lookup.tagName, rest)) {
            q.name = rest;
        } else if (tagged(w, lookup.tagGuild, rest)) {
            q.guild = rest;
        } else if (tagged(w, lookup.tagZone, rest)) {
            if (!zonesSet) q.zones.clear();
            zonesSet = true;
            std::vector<uint32_t> found;
            if (lookup.zones) lookup.zones(rest, found);
            for (uint32_t z : found)
                if (q.zones.size() < 10) q.zones.push_back(z);
            if (q.zones.empty()) q.zones.push_back(0);  // no zone of that name: zone 0, which finds nobody
        } else if (tagged(w, lookup.tagRace, rest)) {
            q.raceMask = lookup.raceMask ? lookup.raceMask(rest) : 0u;
        } else if (tagged(w, lookup.tagClass, rest)) {
            q.classMask = lookup.classMask ? lookup.classMask(rest) : 0u;
        } else if (parseWhoLevels(w, lo, hi)) {
            q.minLevel = lo;
            q.maxLevel = hi;
        } else if (q.strings.size() < 4) {
            q.strings.push_back(w);
        }
    }
    return q;
}

inline void writeWho(network::Packet& p, const WhoQuery& q) {
    p.writeUInt32(q.minLevel);
    p.writeUInt32(q.maxLevel);
    p.writeString(q.name);
    p.writeString(q.guild);
    p.writeUInt32(q.raceMask);
    p.writeUInt32(q.classMask);
    p.writeUInt32(static_cast<uint32_t>(q.zones.size()));
    for (uint32_t z : q.zones) p.writeUInt32(z);
    p.writeUInt32(static_cast<uint32_t>(q.strings.size()));
    for (const std::string& s : q.strings) p.writeString(s);
}

// --- Guild ---------------------------------------------------------------------------------------------------------

constexpr size_t kGuildRanks = 10;        // GUILD_RANKS_MAX
constexpr size_t kMaxRoster = 500;        // PSP: members kept (online ones first)
constexpr size_t kMaxEventLog = 100;
constexpr size_t kMotdChars = 128, kInfoChars = 500, kNoteChars = 31, kRankNameChars = 15;

// Rank rights (AzerothCore Guild.h GuildRankRights: the Can* functions, the rank-flag table in GuildControl
// checkbox order).
enum : uint32_t {
    kRightGChatListen = 0x1, kRightGChatSpeak = 0x2, kRightOffChatListen = 0x4, kRightOffChatSpeak = 0x8,
    kRightInvite = 0x10, kRightRemove = 0x20, kRightPromote = 0x80, kRightDemote = 0x100, kRightSetMotd = 0x1000,
    kRightEditPublicNote = 0x2000, kRightViewOfficerNote = 0x4000, kRightEditOfficerNote = 0x8000,
    kRightModifyInfo = 0x10000, kRightWithdrawGoldLock = 0x20000, kRightRepair = 0x40000, kRightWithdrawGold = 0x80000,
    kRightCreateEvent = 0x100000,
};
// GuildControlPopupFrameCheckbox1..17 -> the right (upstream lua_social_api.cpp kGuildRankFlagBits: checkbox 14 has
// no right - 0 leaves the rank's bit as it is).
constexpr uint32_t kRankFlagBit[17] = {0x1,    0x2,    0x4,    0x8,     0x80,    0x100,   0x10,    0x20,    0x1000,
                                       0x2000, 0x4000, 0x8000, 0x10000, 0x0,     0x40000, 0x80000, 0x100000};

struct RosterRank {
    uint32_t rights = 0, moneyPerDay = 0;
    uint32_t tabFlags[6] = {}, tabSlots[6] = {};
};

struct RosterMember {
    uint64_t guid = 0;
    uint8_t flags = 0;  // != 0 online; 2 AFK, 4 DND
    std::string name;
    uint32_t rank = 0;
    uint8_t level = 0, cls = 0, gender = 0;
    uint32_t zone = 0;
    float offlineDays = 0.0f;
    std::string note, officerNote;
    bool online() const { return flags != 0; }
};

struct Roster {
    uint32_t memberCount = 0, online = 0;
    std::string motd, info;
    uint32_t rankCount = 0;
    RosterRank ranks[kGuildRanks];
    std::vector<RosterMember> members;  // <= maxMembers: the online members first when there are more
};

inline bool readRoster(network::Packet& p, Roster& out, size_t maxMembers = kMaxRoster) {
    out = Roster{};
    if (!p.hasRemaining(4)) return false;
    out.memberCount = p.readUInt32();
    out.motd = capText(p.readString(), kMotdChars, true);
    out.info = capText(p.readString(), kInfoChars, false);
    if (!p.hasRemaining(4)) return false;
    out.rankCount = p.readUInt32();
    for (uint32_t r = 0; r < out.rankCount; ++r) {
        if (!p.hasRemaining(8 + 48)) return false;
        RosterRank rk;
        rk.rights = p.readUInt32();
        rk.moneyPerDay = p.readUInt32();
        for (int t = 0; t < 6; ++t) {
            rk.tabFlags[t] = p.readUInt32();
            rk.tabSlots[t] = p.readUInt32();
        }
        if (r < kGuildRanks) out.ranks[r] = rk;
    }
    size_t offlineKept = 0;
    for (uint32_t i = 0; i < out.memberCount; ++i) {
        if (!p.hasRemaining(9)) return false;
        RosterMember m;
        m.guid = p.readUInt64();
        m.flags = p.readUInt8();
        m.name = capText(p.readString(), 64, false);
        if (!p.hasRemaining(11)) return false;
        m.rank = p.readUInt32();
        m.level = p.readUInt8();
        m.cls = p.readUInt8();
        m.gender = p.readUInt8();
        m.zone = p.readUInt32();
        if (!m.online()) {
            if (!p.hasRemaining(4)) return false;
            m.offlineDays = p.readFloat();
        }
        m.note = capText(p.readString(), kNoteChars, true);
        m.officerNote = capText(p.readString(), kNoteChars, true);
        if (m.online()) ++out.online;
        // PSP: bounded - online members always (up to the cap, replacing an offline one), offline ones while room.
        if (out.members.size() < maxMembers) {
            if (!m.online()) ++offlineKept;
            out.members.push_back(std::move(m));
        } else if (m.online() && offlineKept > 0) {
            for (auto it = out.members.rbegin(); it != out.members.rend(); ++it)
                if (!it->online()) {
                    *it = std::move(m);
                    --offlineKept;
                    break;
                }
        }
    }
    return true;
}

struct GuildPermissions {
    uint32_t rank = 0, rights = 0, moneyPerDay = 0;
    uint8_t purchasedTabs = 0;
    uint32_t tabFlags[6] = {}, tabSlots[6] = {};
};

inline bool readGuildPermissions(network::Packet& p, GuildPermissions& out) {
    out = GuildPermissions{};
    if (!p.hasRemaining(13 + 48)) return false;
    out.rank = p.readUInt32();
    out.rights = p.readUInt32();
    out.moneyPerDay = p.readUInt32();
    out.purchasedTabs = p.readUInt8();
    for (int t = 0; t < 6; ++t) {
        out.tabFlags[t] = p.readUInt32();
        out.tabSlots[t] = p.readUInt32();
    }
    return true;
}

struct EventLogEntry {
    uint8_t type = 0;  // 1 invite, 2 join, 3 promote, 4 demote, 5 remove, 6 quit
    uint64_t guid1 = 0, guid2 = 0;
    uint8_t rank = 0;
    uint32_t secondsAgo = 0;
};

inline bool readEventLog(network::Packet& p, std::vector<EventLogEntry>& out) {
    out.clear();
    if (!p.hasRemaining(1)) return false;
    const uint8_t count = p.readUInt8();
    for (uint8_t i = 0; i < count; ++i) {
        EventLogEntry e;
        if (!p.hasRemaining(9)) return false;
        e.type = p.readUInt8();
        e.guid1 = p.readUInt64();
        if (e.type != 2 && e.type != 6) {
            if (!p.hasRemaining(8)) return false;
            e.guid2 = p.readUInt64();
        }
        if (e.type == 3 || e.type == 4) {
            if (!p.hasRemaining(1)) return false;
            e.rank = p.readUInt8();
        }
        if (!p.hasRemaining(4)) return false;
        e.secondsAgo = p.readUInt32();
        if (out.size() < kMaxEventLog) out.push_back(e);
    }
    return true;
}

struct GuildQuery {
    uint32_t id = 0;
    std::string name;
    std::string ranks[kGuildRanks];
    int32_t emblem[5] = {-1, -1, -1, -1, -1};  // style, colour, border style, border colour, background
    uint32_t rankCount = 0;
};

inline bool readGuildQuery(network::Packet& p, GuildQuery& out) {
    out = GuildQuery{};
    if (!p.hasRemaining(4)) return false;
    out.id = p.readUInt32();
    out.name = capText(p.readString(), 128, false);
    for (auto& r : out.ranks) r = capText(p.readString(), 64, false);
    if (p.hasRemaining(20))
        for (int32_t& e : out.emblem) e = static_cast<int32_t>(p.readUInt32());
    if (p.hasRemaining(4)) out.rankCount = p.readUInt32();
    return true;
}

// SMSG_GUILD_EVENT types.
enum GuildEventType : uint8_t {
    kGePromotion = 0, kGeDemotion = 1, kGeMotd = 2, kGeJoined = 3, kGeLeft = 4, kGeRemoved = 5, kGeLeaderIs = 6,
    kGeLeaderChanged = 7, kGeDisbanded = 8, kGeTabardChanged = 9, kGeRankUpdated = 10, kGeRankDeleted = 11,
    kGeSignedOn = 12, kGeSignedOff = 13, kGeBankBagSlots = 14, kGeBankTabPurchased = 15, kGeBankTabUpdated = 16,
    kGeBankMoneySet = 17, kGeBankTabAndMoney = 18, kGeBankTextChanged = 19,
};

struct GuildEvent {
    uint8_t type = 0, count = 0;
    std::string strings[3];
    uint64_t guid = 0;
};

inline bool readGuildEvent(network::Packet& p, GuildEvent& out) {
    out = GuildEvent{};
    if (!p.hasRemaining(2)) return false;
    out.type = p.readUInt8();
    out.count = p.readUInt8();
    for (uint8_t i = 0; i < out.count; ++i) {
        std::string s = capText(p.readString(), 1024, true);
        if (i < 3) out.strings[i] = std::move(s);
    }
    if ((out.type == kGeJoined || out.type == kGeLeft || out.type == kGeSignedOn || out.type == kGeSignedOff) &&
        p.hasRemaining(8))
        out.guid = p.readUInt64();
    return true;
}

struct GuildCommandResult {
    uint32_t command = 0, result = 0;
    std::string name;
};

inline bool readGuildCommandResult(network::Packet& p, GuildCommandResult& out) {
    out = GuildCommandResult{};
    if (!p.hasRemaining(4)) return false;
    out.command = p.readUInt32();
    out.name = p.readString();
    if (!p.hasRemaining(4)) return false;
    out.result = p.readUInt32();
    return true;
}

struct GuildInfo {
    std::string name;
    uint32_t packedDate = 0, members = 0, accounts = 0;
};

inline bool readGuildInfo(network::Packet& p, GuildInfo& out) {
    out = GuildInfo{};
    out.name = p.readString();
    if (!p.hasRemaining(12)) return false;
    out.packedDate = p.readUInt32();
    out.members = p.readUInt32();
    out.accounts = p.readUInt32();
    return true;
}

// The packed date (AzerothCore ByteBuffer::AppendPackedTime): bits 0-5 minute, 6-10 hour, 11-13 weekday, 14-19 day - 1, 20-23 month - 1,
// 24-28 year - 2000.
inline void unpackDate(uint32_t d, int& year, int& month, int& day) {
    day = static_cast<int>((d >> 14) & 0x3F) + 1;
    month = static_cast<int>((d >> 20) & 0x0F) + 1;
    year = static_cast<int>((d >> 24) & 0x1F) + 2000;
}

// GetGuildRosterLastOnline: the float days since logout split 365 / 30 / 1 days and hours (upstream
// lua_social_api.cpp GetGuildRosterLastOnline).
inline void lastOnline(float days, int& years, int& months, int& d, int& hours) {
    if (days < 0.0f) days = 0.0f;
    years = static_cast<int>(days / 365.0f);
    float r = days - 365.0f * static_cast<float>(years);
    months = static_cast<int>(r / 30.0f);
    const float r2 = r - 30.0f * static_cast<float>(months);
    d = static_cast<int>(r2);
    hours = static_cast<int>((r2 - static_cast<float>(d)) * 24.0f);
}

}  // namespace wowee::platform::psp::social
