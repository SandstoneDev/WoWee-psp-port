// Glyphs, guild bank, channels: the glyph sockets, the guild bank and the chat
// channels as the 3.3.5a client reads and writes them. Pure: no PSP calls - the session files
// (psp_world_session_glyphs.cpp / _guildbank.cpp / _channels.cpp) use it, the host test
// (tools/psp/host_tests/glyphs_guildbank_host_test.sh) checks it against recorded packet bytes and a TrinityCore /
// AzerothCore 3.3.5 reader.
//
// Glyphs (the glyph Lua API, the cast, GameTooltip:SetGlyph):
//   - the player's descriptor: PLAYER_FIELD_GLYPH_SLOTS_1 (+0x1230 of the player fields = UNIT_END + 0x48C) six
//     GlyphSlot.dbc ids, PLAYER_FIELD_GLYPHS_1 (+0x1248) six GlyphProperties.dbc ids, PLAYER_GLYPHS_ENABLED (+0x1260)
//     a bit per socket;
//   - PlaceGlyphInSocket(i) on the pending glyph spell: the socket enabled (else
//     SPELL_FAILED_GLYPH_SOCKET_LOCKED 0xB1), an effect 74 (APPLY_GLYPH) whose MiscValue's GlyphProperties.TypeFlags
//     equals the socket's GlyphSlot.Type (else SPELL_FAILED_INVALID_GLYPH 0xAF), the glyph not in any socket yet (else
//     SPELL_FAILED_UNIQUE_GLYPH 0xB0); then the cast goes out as CMSG_USE_ITEM with glyph index = i - 1;
//   - RemoveGlyphFromSocket(i): CMSG_REMOVE_GLYPH (0x48A) u32 i - 1, when the socket has a slot id.
// Guild bank (GuildBankFrame):
//   SMSG_GUILD_BANK_LIST   u64 money, u8 tab, u32 withdrawals left, u8 full; full && tab 0: u8 tabs, per tab
//                                   string name (64), string icon (256); u8 slots, per slot u8 slot, u32 entry, entry
//                                   != 0: u32 flags, u32 random property, it != 0: u32 seed; u32 count, u32 enchant,
//                                   u8 charges, u8 gems, per gem u8 index, u32 enchant
//   MSG_GUILD_BANK_LOG_QUERY  u8 tab (6 = money), u8 count (<= 25), per entry u8 type, u64 guid, types 4, 5, 6,
//                                   8, 9: u32 amount, else u32 entry, u32 count, types 3 / 7: u8 other tab; u32 s ago
//   MSG_GUILD_BANK_MONEY_WITHDRAWN  u32 money left today (asked with an empty request)
//   MSG_QUERY_GUILD_BANK_TEXT  u8 tab, string text (asked with u8 tab)
//   CMSG_GUILD_BANKER_ACTIVATE  u64 banker, u8 full (always 1)
//   CMSG_GUILD_BANK_QUERY_TAB  u64 banker, u8 tab, u8 full (always 1)
//   CMSG_GUILD_BANK_SWAP_ITEMS      bank -> bank: u64 banker, u8 1, u8 tab, u8 slot, u32 entry (0), u8 src
//                                   tab, u8 src slot, u32 src entry (0), u8 0, u32 split; bag -> bank: u64, u8 0,
//                                   u8 tab, u8 slot (0xFF auto), u32 entry (0), u8 0, u8 bag, u8 bag slot, u8 0, u32
//                                   split; bank -> bag: ... u8 0, u8 bag, u8 bag slot, u8 1, u32 split;
//                                   AutoStoreGuildBankItem: u64, u8 0, u8 tab, u8 slot, u32 entry, u8 1, u32
//                                   count, u8 1, u32 0 (AzerothCore GuildBankSwapItems::Read; the split 0 = all)
//   CMSG_GUILD_BANK_BUY_TAB  u64 banker, u8 tab (= purchased tabs)
//   CMSG_GUILD_BANK_UPDATE_TAB  u64 banker, u8 tab, string name (<= 16 chars), string icon
//   CMSG_GUILD_BANK_DEPOSIT_MONEY / _WITHDRAW_MONEY  u64 banker, u32 copper
//   CMSG_SET_GUILD_BANK_TEXT  u8 tab, string text (<= 500 chars: AzerothCore MAX_GUILD_BANK_TAB_TEXT_LEN)
// Chat channels (ChannelFrame / ChatFrame):
//   SMSG_CHANNEL_NOTIFY    u8 notify, string channel, then by notify (channelNotify below)
//   SMSG_CHANNEL_LIST  u8 display, string channel, u8 channel flags, u32 count, count x {u64
//                                   guid, u8 member flags}
//   CMSG_JOIN_CHANNEL      u32 ChatChannels id (0 custom), u8 1, u8 0, string name, string password
//   CMSG_LEAVE_CHANNEL     u32 ChatChannels id, string name
//   the name of a ChatChannels channel: Name_lang formatted with the zone's AreaTable name, or
//   with AreaTable 3459's ("City") for a CITY_ONLY (0x20) channel in a zone with AreaTable flag 0x100.
#pragma once

#include "network/packet.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wowee::platform::psp::gbk {

// --- Text helpers ---------------------------------------------------------------------------------------------------

// The client's capped copy: at most maxChars UTF-8 characters.
inline std::string capUtf8(const std::string& s, size_t maxChars) {
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < maxChars) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + len > s.size()) break;
        i += len;
        ++chars;
    }
    return s.substr(0, i);
}

// --- Glyphs ---------------------------------------------------------------------------------------------------------

constexpr int kGlyphSockets = 6;
constexpr uint16_t kUnitEnd = 148;                                  // UNIT_END (update_fields.json)
constexpr uint16_t kFieldGlyphSlots = kUnitEnd + 0x48C;             // PLAYER_FIELD_GLYPH_SLOTS_1 (1312)
constexpr uint16_t kFieldGlyphs = kFieldGlyphSlots + 6;             // PLAYER_FIELD_GLYPHS_1 (1318)
constexpr uint16_t kFieldGlyphsEnabled = kFieldGlyphSlots + 12;     // PLAYER_GLYPHS_ENABLED (1324)
constexpr uint32_t kSpellEffectApplyGlyph = 74;
// Spell_C cast failures of the glyph socket check (SpellCastResult).
constexpr uint8_t kCastFailInvalidGlyph = 0xAF, kCastFailUniqueGlyph = 0xB0, kCastFailGlyphLocked = 0xB1;
constexpr uint32_t kOpRemoveGlyph = 0x48A;  // CMSG_REMOVE_GLYPH (Data/expansions/wotlk/opcodes.json)

struct GlyphSlotRow {   // GlyphSlot.dbc: ID, Type (0 major, 1 minor), Tooltip (the GLYPH_SLOT_TOOLTIP%d number)
    uint32_t id = 0, type = 0, order = 0;
};
struct GlyphPropRow {   // GlyphProperties.dbc: ID, SpellID, GlyphSlotFlags (= the slot type), SpellIconID -> rune
    uint32_t id = 0, spellId = 0, type = 0, rune = 0;  // rune: N of Interface\Spellbook\UI-Glyph-Rune-N (0 = Rune1)
};

// text.pak "glyph_data" (tools/psp/build_text_pak.py): u32 count + count x {id, type, order}; u32 count + count x {id,
// spell, type, rune}; u32 count + count x {spell, glyph properties id} (Spell.dbc effect 74's MiscValue).
struct GlyphData {
    std::vector<GlyphSlotRow> slots;
    std::vector<GlyphPropRow> props;
    std::vector<std::pair<uint32_t, uint32_t>> spellGlyph;  // sorted by spell
    const GlyphSlotRow* slot(uint32_t id) const {
        for (const auto& s : slots)
            if (s.id == id) return &s;
        return nullptr;
    }
    const GlyphPropRow* prop(uint32_t id) const {
        size_t lo = 0, hi = props.size();
        while (lo < hi) {
            const size_t m = (lo + hi) / 2;
            if (props[m].id < id) lo = m + 1;
            else hi = m;
        }
        return lo < props.size() && props[lo].id == id ? &props[lo] : nullptr;
    }
    uint32_t glyphOfSpell(uint32_t spellId) const {  // the APPLY_GLYPH effect's GlyphProperties id, 0 = none
        size_t lo = 0, hi = spellGlyph.size();
        while (lo < hi) {
            const size_t m = (lo + hi) / 2;
            if (spellGlyph[m].first < spellId) lo = m + 1;
            else hi = m;
        }
        return lo < spellGlyph.size() && spellGlyph[lo].first == spellId ? spellGlyph[lo].second : 0;
    }
};

inline bool readGlyphData(const uint8_t* b, size_t n, GlyphData& out) {
    out = GlyphData{};
    size_t o = 0;
    auto u32 = [&](uint32_t& v) {
        if (o + 4 > n) return false;
        std::memcpy(&v, b + o, 4);
        o += 4;
        return true;
    };
    uint32_t c = 0;
    if (!u32(c) || c > 64) return false;
    out.slots.resize(c);
    for (auto& s : out.slots)
        if (!u32(s.id) || !u32(s.type) || !u32(s.order)) return false;
    if (!u32(c) || c > 4096) return false;
    out.props.resize(c);
    for (auto& p : out.props)
        if (!u32(p.id) || !u32(p.spellId) || !u32(p.type) || !u32(p.rune)) return false;
    if (!u32(c) || c > 8192) return false;
    out.spellGlyph.resize(c);
    for (auto& m : out.spellGlyph)
        if (!u32(m.first) || !u32(m.second)) return false;
    return true;
}

// GetGlyphSocketInfo of the active talent group, from the player's fields.
struct GlyphSocket {
    bool enabled = false;
    uint32_t slotId = 0;      // GlyphSlot id (0: none)
    int type = 1;             // 1 major, 2 minor (GlyphSlot.Type + 1; without a slot row: 1)
    uint32_t glyph = 0;       // GlyphProperties id (0: empty)
    uint32_t spell = 0;       // its spell
    uint32_t rune = 0;        // its icon
    uint32_t order = 0;       // GlyphSlot.Tooltip (GLYPH_SLOT_TOOLTIP%d)
};
inline GlyphSocket glyphSocket(const GlyphData& d, int socket, uint32_t slotId, uint32_t glyph, uint32_t enabledMask) {
    GlyphSocket s;
    s.enabled = (enabledMask >> socket) & 1u;
    s.slotId = slotId;
    if (const GlyphSlotRow* r = d.slot(slotId)) {
        s.type = static_cast<int>(r->type) + 1;
        s.order = r->order;
    }
    s.glyph = glyph;
    if (glyph != 0)
        if (const GlyphPropRow* p = d.prop(glyph)) {
            s.spell = p->spellId;
            s.rune = p->rune;
        }
    return s;
}

// GlyphMatchesSocket: can the glyph spell (its APPLY_GLYPH properties id) go into the socket?
// 0 = yes, else the SpellCastResult the client shows. glyphsNow = PLAYER_FIELD_GLYPHS_1..6.
inline uint8_t glyphPlaceCheck(const GlyphData& d, uint32_t glyphProp, uint32_t socketSlotId, bool socketEnabled,
                               const uint32_t glyphsNow[kGlyphSockets]) {
    if (!socketEnabled) return kCastFailGlyphLocked;
    const GlyphPropRow* p = d.prop(glyphProp);
    const GlyphSlotRow* s = d.slot(socketSlotId);
    if (!p || !s || p->type != s->type) return kCastFailInvalidGlyph;
    for (int i = 0; i < kGlyphSockets; ++i)
        if (glyphsNow[i] == glyphProp) return kCastFailUniqueGlyph;
    return 0;
}
inline bool glyphMatchesSocket(const GlyphData& d, uint32_t glyphProp, uint32_t socketSlotId) {
    const GlyphPropRow* p = d.prop(glyphProp);
    const GlyphSlotRow* s = d.slot(socketSlotId);
    return p && s && p->type == s->type;
}

inline void writeRemoveGlyph(network::Packet& p, uint32_t socket) { p.writeUInt32(socket); }

// --- Guild bank -----------------------------------------------------------------------------------------------------

constexpr int kBankTabs = 6;               // MAX_GUILDBANK_TABS
constexpr int kBankSlots = 98;             // MAX_GUILDBANK_SLOTS_PER_TAB
constexpr int kBankLogMax = 25;            // the log's entries per tab
constexpr uint8_t kBankMoneyTab = 6;       // QueryGuildBankLog(MAX_GUILDBANK_TABS + 1)
// GetGuildBankTabCost: the next tab's price by the tabs bought.
constexpr uint32_t kBankTabCost[kBankTabs] = {1000000u, 2500000u, 5000000u, 10000000u, 25000000u, 50000000u};
// The guild bank tab rights (GetGuildBankTabInfo: flags & 1 isViewable, & 2 canDeposit, & 4 update text).
enum : uint32_t { kTabView = 0x1, kTabDeposit = 0x2, kTabUpdateText = 0x4 };

struct BankItem {
    uint32_t entry = 0, flags = 0, randomProp = 0, seed = 0, count = 0, enchant = 0;
    uint8_t charges = 0;
    uint32_t gems[3] = {};
};
struct BankTabInfo {
    std::string name, icon;
};
struct BankList {
    uint64_t money = 0;
    uint8_t tab = 0;
    uint32_t withdrawalsLeft = 0;  // the u32 after the tab (GetGuildBankTabInfo's remainingWithdrawals)
    bool full = false;
    bool hasTabs = false;          // full && tab 0: the tab names came (GUILDBANK_UPDATE_TABS)
    std::vector<BankTabInfo> tabs;
    std::vector<std::pair<uint8_t, BankItem>> items;
};

inline bool readBankList(network::Packet& p, BankList& out) {
    out = BankList{};
    if (!p.hasRemaining(8 + 1 + 4 + 1)) return false;
    out.money = p.readUInt64();
    out.tab = p.readUInt8();
    out.withdrawalsLeft = p.readUInt32();
    out.full = p.readUInt8() != 0;
    if (out.full && out.tab == 0) {
        if (!p.hasRemaining(1)) return false;
        const uint8_t n = p.readUInt8();
        out.hasTabs = true;
        for (uint8_t i = 0; i < n && i < kBankTabs; ++i) {
            BankTabInfo t;
            t.name = capUtf8(p.readString(), 16);
            t.icon = p.readString();
            out.tabs.push_back(std::move(t));
        }
    }
    if (!p.hasRemaining(1)) return false;
    const uint8_t n = p.readUInt8();
    for (uint8_t i = 0; i < n; ++i) {
        if (!p.hasRemaining(5)) return false;
        const uint8_t slot = p.readUInt8();
        BankItem it;
        it.entry = p.readUInt32();
        if (it.entry != 0) {
            if (!p.hasRemaining(8)) return false;
            it.flags = p.readUInt32();
            it.randomProp = p.readUInt32();
            if (it.randomProp != 0) {
                if (!p.hasRemaining(4)) return false;
                it.seed = p.readUInt32();
            }
            if (!p.hasRemaining(4 + 4 + 1 + 1)) return false;
            it.count = p.readUInt32();
            it.enchant = p.readUInt32();
            it.charges = p.readUInt8();
            const uint8_t gems = p.readUInt8();
            for (uint8_t g = 0; g < gems; ++g) {
                if (!p.hasRemaining(5)) return false;
                const uint8_t idx = p.readUInt8();
                const uint32_t e = p.readUInt32();
                if (idx < 3) it.gems[idx] = e;
            }
        }
        if (slot < kBankSlots) out.items.emplace_back(slot, it);
    }
    return true;
}

struct BankLogEntry {
    uint8_t type = 0;      // 1 deposit, 2 withdraw, 3 / 7 move, 4 deposit money, 5 withdraw money, 6 repair, 8 withdraw
                           // for a tab, 9 buy a tab
    uint64_t guid = 0;
    uint32_t entry = 0, count = 0, amount = 0;
    uint8_t otherTab = 0;  // types 3 / 7
    uint32_t secondsAgo = 0;
    bool money() const { return type == 4 || type == 5 || type == 6 || type == 8 || type == 9; }
};

inline bool readBankLog(network::Packet& p, uint8_t& tab, std::vector<BankLogEntry>& out) {
    out.clear();
    if (!p.hasRemaining(2)) return false;
    tab = p.readUInt8();
    const uint8_t n = p.readUInt8();
    for (uint8_t i = 0; i < n; ++i) {
        BankLogEntry e;
        if (!p.hasRemaining(9)) return false;
        e.type = p.readUInt8();
        e.guid = p.readUInt64();
        if (e.money()) {
            if (!p.hasRemaining(4)) return false;
            e.amount = p.readUInt32();
        } else {
            if (!p.hasRemaining(8)) return false;
            e.entry = p.readUInt32();
            e.count = p.readUInt32();
            if (e.type == 3 || e.type == 7) {
                if (!p.hasRemaining(1)) return false;
                e.otherTab = p.readUInt8();
            }
        }
        if (!p.hasRemaining(4)) return false;
        e.secondsAgo = p.readUInt32();
        if (out.size() < static_cast<size_t>(kBankLogMax)) out.push_back(e);
    }
    return true;
}

inline void writeBankerActivate(network::Packet& p, uint64_t banker, bool full) {
    p.writeUInt64(banker);
    p.writeUInt8(full ? 1 : 0);
}
inline void writeBankQueryTab(network::Packet& p, uint64_t banker, uint8_t tab, bool full) {
    p.writeUInt64(banker);
    p.writeUInt8(tab);
    p.writeUInt8(full ? 1 : 0);
}
// bank -> bank
inline void writeBankSwapBankBank(network::Packet& p, uint64_t banker, uint8_t tab, uint8_t slot, uint32_t entryThere,
                                  uint8_t srcTab, uint8_t srcSlot, uint32_t srcEntry, uint32_t count) {
    p.writeUInt64(banker);
    p.writeUInt8(1);
    p.writeUInt8(tab);
    p.writeUInt8(slot);
    p.writeUInt32(entryThere);
    p.writeUInt8(srcTab);
    p.writeUInt8(srcSlot);
    p.writeUInt32(srcEntry);
    p.writeUInt8(0);
    p.writeUInt32(count);
}
// bag <-> bank (toChar 0 / toChar 1)
inline void writeBankSwapWithBag(network::Packet& p, uint64_t banker, uint8_t tab, uint8_t slot, uint32_t entryThere,
                                 uint8_t bag, uint8_t bagSlot, bool toChar, uint32_t count) {
    p.writeUInt64(banker);
    p.writeUInt8(0);
    p.writeUInt8(tab);
    p.writeUInt8(slot);
    p.writeUInt32(entryThere);
    p.writeUInt8(0);
    p.writeUInt8(bag);
    p.writeUInt8(bagSlot);
    p.writeUInt8(toChar ? 1 : 0);
    p.writeUInt32(count);
}
// AutoStoreGuildBankItem
inline void writeBankAutoStore(network::Packet& p, uint64_t banker, uint8_t tab, uint8_t slot, uint32_t entry,
                               uint32_t stackCount) {
    p.writeUInt64(banker);
    p.writeUInt8(0);
    p.writeUInt8(tab);
    p.writeUInt8(slot);
    p.writeUInt32(entry);
    p.writeUInt8(1);
    p.writeUInt32(stackCount);
    p.writeUInt8(1);
    p.writeUInt32(0);
}
inline void writeBankBuyTab(network::Packet& p, uint64_t banker, uint8_t tab) {
    p.writeUInt64(banker);
    p.writeUInt8(tab);
}
inline void writeBankUpdateTab(network::Packet& p, uint64_t banker, uint8_t tab, const std::string& name,
                               const std::string& icon) {
    p.writeUInt64(banker);
    p.writeUInt8(tab);
    p.writeString(capUtf8(name, 16));
    p.writeString(icon);
}
inline void writeBankMoney(network::Packet& p, uint64_t banker, uint32_t copper) {
    p.writeUInt64(banker);
    p.writeUInt32(copper);
}
inline void writeBankSetText(network::Packet& p, uint8_t tab, const std::string& text) {
    p.writeUInt8(tab);
    p.writeString(capUtf8(text, 500));
}

// GetGuildBankTransaction / GetGuildBankMoneyTransaction: the age in whole hours split into
// years (8760 h), months (720 h), days (24 h), hours - RecentTimeDate's input.
inline void bankLogAge(uint32_t secondsAgo, int& years, int& months, int& days, int& hours) {
    int h = static_cast<int>(secondsAgo / 3600u);
    years = h / 8760;
    h %= 8760;
    months = h / 720;
    h %= 720;
    days = h / 24;
    hours = h % 24;
}

// --- Chat channels --------------------------------------------------------------------------------------------------

// ChatChannels.dbc flags (CHANNEL_DBC_FLAG_*, AzerothCore Channel.h).
enum : uint32_t {
    kChanInitial = 0x1, kChanZoneDep = 0x2, kChanGlobal = 0x4, kChanTrade = 0x8, kChanCityOnly = 0x10,
    kChanCityOnly2 = 0x20, kChanDefense = 0x10000, kChanGuildReq = 0x20000, kChanLfg = 0x40000,
};
// Member flags (SMSG_CHANNEL_LIST / MODE_CHANGE; GetChannelRosterInfo).
enum : uint8_t { kMemberOwner = 0x1, kMemberModerator = 0x2, kMemberVoiced = 0x4, kMemberMuted = 0x8 };

struct ChatChannelRow {   // ChatChannels.dbc (ruRU strings)
    uint32_t id = 0, flags = 0, faction = 0;
    std::string name;      // Name_lang: "General - %s"
    std::string shortcut;  // Shortcut_lang: "General"
};

// The full name of a ChatChannels channel in a zone: Name_lang formatted with the zone's AreaTable name, or with
// the city word for a CITY_ONLY channel in a capital (AzerothCore Player::UpdateLocalChannels). Empty while the
// zone has no name (the zone channel cannot be named yet).
inline std::string channelFullName(const ChatChannelRow& c, const std::string& zone, bool zoneIsCity,
                                   const std::string& cityName) {
    const std::string& place = (c.flags & kChanCityOnly) && zoneIsCity && !cityName.empty() ? cityName : zone;
    if (place.empty()) return std::string();
    const size_t pos = c.name.find("%s");
    if (pos == std::string::npos) return c.name;  // a global channel ("LocalDefense - global")
    return c.name.substr(0, pos) + place + c.name.substr(pos + 2);
}

inline void writeJoinChannel(network::Packet& p, uint32_t channelId, const std::string& name,
                             const std::string& password) {
    p.writeUInt32(channelId);
    p.writeUInt8(0);  // AzerothCore reads two unknown bytes; upstream JoinChannelPacket::build sends 0, 0
    p.writeUInt8(0);
    p.writeString(name);
    p.writeString(password);
}
inline void writeLeaveChannel(network::Packet& p, uint32_t channelId, const std::string& name) {
    p.writeUInt32(channelId);
    p.writeString(name);
}

// SMSG_CHANNEL_NOTIFY (AzerothCore Channel::Make* / Channel.h notify enum): the event (CHAT_MSG_CHANNEL_JOIN / _LEAVE / _NOTICE / _NOTICE_USER), the
// GlobalStrings key part (CHAT_<key>_NOTICE), the guids / names it carries.
enum class ChanEvent : uint8_t { None, Join, Leave, Notice, NoticeUser };
struct ChannelNotify {
    uint8_t type = 0xFF;
    std::string channel;
    ChanEvent event = ChanEvent::None;
    const char* key = "";        // "YOU_JOINED", "PLAYER_KICKED", ... ("" for MODE_CHANGE: the caller builds them)
    uint64_t guid = 0, guid2 = 0;  // arg2's / arg5's player
    std::string name;            // a name the packet carries (arg2): PLAYER_NOT_FOUND, CHANNEL_OWNER, ...
    uint8_t flags = 0;           // YOU_JOINED: the channel flags; MODE_CHANGE: old
    uint8_t newFlags = 0;        // MODE_CHANGE: new
    uint32_t channelId = 0;      // YOU_JOINED / YOU_LEFT: the ChatChannels id
    uint32_t extra = 0;          // YOU_JOINED: the second u32
    bool constant = false;       // YOU_LEFT: the u8 (AzerothCore MakeYouLeft: IsConstant)
};

inline bool readChannelNotify(network::Packet& p, ChannelNotify& n) {
    n = ChannelNotify{};
    if (!p.hasRemaining(1)) return false;
    n.type = p.readUInt8();
    n.channel = p.readString();
    auto guid = [&](uint64_t& g) {
        if (!p.hasRemaining(8)) return false;
        g = p.readUInt64();
        return true;
    };
    switch (n.type) {
        case 0x00: n.event = ChanEvent::Join; return guid(n.guid);
        case 0x01: n.event = ChanEvent::Leave; return guid(n.guid);
        case 0x02:  // YOU_JOINED (the caller shows YOU_CHANGED when it replaces a listed zone channel)
            n.event = ChanEvent::Notice;
            n.key = "YOU_JOINED";
            if (!p.hasRemaining(9)) return false;
            n.flags = p.readUInt8();
            n.channelId = p.readUInt32();
            n.extra = p.readUInt32();
            return true;
        case 0x03:  // YOU_LEFT
            n.event = ChanEvent::Notice;
            n.key = "YOU_LEFT";
            if (!p.hasRemaining(5)) return false;
            n.channelId = p.readUInt32();
            n.constant = p.readUInt8() != 0;
            return true;
        case 0x04: n.event = ChanEvent::Notice; n.key = "WRONG_PASSWORD"; return true;
        case 0x05: n.event = ChanEvent::Notice; n.key = "NOT_MEMBER"; return true;
        case 0x06: n.event = ChanEvent::Notice; n.key = "NOT_MODERATOR"; return true;
        case 0x07: n.event = ChanEvent::NoticeUser; n.key = "PASSWORD_CHANGED"; return guid(n.guid);
        case 0x08: n.event = ChanEvent::NoticeUser; n.key = "OWNER_CHANGED"; return guid(n.guid);
        case 0x09: n.event = ChanEvent::NoticeUser; n.key = "PLAYER_NOT_FOUND"; n.name = p.readString(); return true;
        case 0x0A: n.event = ChanEvent::Notice; n.key = "NOT_OWNER"; return true;
        case 0x0B: n.event = ChanEvent::NoticeUser; n.key = "CHANNEL_OWNER"; n.name = p.readString(); return true;
        case 0x0C:  // MODE_CHANGE -> SET / UNSET_MODERATOR, _VOICE, _SPEAK
            n.event = ChanEvent::NoticeUser;
            if (!guid(n.guid) || !p.hasRemaining(2)) return false;
            n.flags = p.readUInt8();
            n.newFlags = p.readUInt8();
            return true;
        case 0x0D: n.event = ChanEvent::NoticeUser; n.key = "ANNOUNCEMENTS_ON"; return guid(n.guid);
        case 0x0E: n.event = ChanEvent::NoticeUser; n.key = "ANNOUNCEMENTS_OFF"; return guid(n.guid);
        case 0x0F: n.event = ChanEvent::NoticeUser; n.key = "MODERATION_ON"; return guid(n.guid);
        case 0x10: n.event = ChanEvent::NoticeUser; n.key = "MODERATION_OFF"; return guid(n.guid);
        case 0x11: n.event = ChanEvent::Notice; n.key = "MUTED"; return true;
        case 0x12: n.event = ChanEvent::NoticeUser; n.key = "PLAYER_KICKED"; return guid(n.guid) && guid(n.guid2);
        case 0x13: n.event = ChanEvent::Notice; n.key = "BANNED"; return true;
        case 0x14: n.event = ChanEvent::NoticeUser; n.key = "PLAYER_BANNED"; return guid(n.guid) && guid(n.guid2);
        case 0x15: n.event = ChanEvent::NoticeUser; n.key = "PLAYER_UNBANNED"; return guid(n.guid) && guid(n.guid2);
        case 0x16: n.event = ChanEvent::NoticeUser; n.key = "PLAYER_NOT_BANNED"; n.name = p.readString(); return true;
        case 0x17: n.event = ChanEvent::NoticeUser; n.key = "PLAYER_ALREADY_MEMBER"; return guid(n.guid);
        case 0x18: n.event = ChanEvent::NoticeUser; n.key = "INVITE"; return guid(n.guid);
        case 0x19: n.event = ChanEvent::NoticeUser; n.key = "INVITE_WRONG_FACTION"; return true;
        case 0x1A: n.event = ChanEvent::NoticeUser; n.key = "WRONG_FACTION"; return true;
        case 0x1B: n.event = ChanEvent::NoticeUser; n.key = "INVALID_NAME"; return true;
        case 0x1C: n.event = ChanEvent::NoticeUser; n.key = "NOT_MODERATED"; return true;
        case 0x1D: n.event = ChanEvent::NoticeUser; n.key = "PLAYER_INVITED"; n.name = p.readString(); return true;
        case 0x1E: n.event = ChanEvent::NoticeUser; n.key = "PLAYER_INVITE_BANNED"; n.name = p.readString(); return true;
        case 0x1F: n.event = ChanEvent::Notice; n.key = "THROTTLED"; return true;
        case 0x20: n.event = ChanEvent::Notice; n.key = "NOT_IN_AREA"; return true;
        case 0x21: n.event = ChanEvent::Notice; n.key = "NOT_IN_LFG"; return true;
        case 0x22: n.event = ChanEvent::NoticeUser; n.key = "VOICE_ON"; return guid(n.guid);
        case 0x23: n.event = ChanEvent::NoticeUser; n.key = "VOICE_OFF"; return guid(n.guid);
        default: return true;  // a type not in AzerothCore's enum: nothing shown
    }
}

// The lines a MODE_CHANGE makes (old -> new member flags): the moderator line, then the voice line (AzerothCore
// Channel.h MEMBER_FLAG_MODERATOR 0x02 / MEMBER_FLAG_VOICED 0x04; the other bits make no line).
inline std::vector<const char*> modeChangeKeys(uint8_t oldFlags, uint8_t newFlags) {
    std::vector<const char*> out;
    if (!(oldFlags & 0x02) && (newFlags & 0x02)) out.push_back("SET_MODERATOR");
    else if ((oldFlags & 0x02) && !(newFlags & 0x02)) out.push_back("UNSET_MODERATOR");
    if (!(oldFlags & 0x04) && (newFlags & 0x04)) out.push_back("SET_VOICE");
    else if ((oldFlags & 0x04) && !(newFlags & 0x04)) out.push_back("UNSET_VOICE");
    return out;
}

struct ChannelMember {
    uint64_t guid = 0;
    uint8_t flags = 0;
};
struct ChannelList {
    bool display = false;
    std::string channel;
    uint8_t channelFlags = 0;
    std::vector<ChannelMember> members;
};
inline bool readChannelList(network::Packet& p, ChannelList& out, size_t maxMembers) {
    out = ChannelList{};
    if (!p.hasRemaining(1)) return false;
    out.display = p.readUInt8() != 0;
    out.channel = p.readString();
    if (!p.hasRemaining(5)) return false;
    out.channelFlags = p.readUInt8();
    const uint32_t n = p.readUInt32();
    for (uint32_t i = 0; i < n; ++i) {
        if (!p.hasRemaining(9)) return false;
        ChannelMember m;
        m.guid = p.readUInt64();
        m.flags = p.readUInt8();
        if (out.members.size() < maxMembers) out.members.push_back(m);
    }
    return true;
}

}  // namespace wowee::platform::psp::gbk
