// Trade and duels: the packets of the trade, the duels, the summons and the shared / escort quests - pure
// (network::Packet only), so the host test (tools/psp/host_tests/trade_duel_test.cpp) runs the same code as the PSP
// session (psp_world_session_trade_duel.cpp). Layouts as AzerothCore writes / reads them (Handlers/TradeHandler.cpp
// SendTradeStatus / SendUpdateTrade, Handlers/DuelHandler.cpp, Spells/SpellEffects.cpp EffectDuel, Player
// DuelComplete, Server/Packets/QuestPackets.cpp) and as upstream WoWee builds them (world_packets_social.cpp):
//
//   SMSG_TRADE_STATUS: u32 status; BEGIN_TRADE (1) u64 the asking player; OPEN_WINDOW (2) u32 trade id;
//                      CLOSE_WINDOW (12) u32 InventoryResult, u8 isTargetResult, u32 ItemLimitCategory;
//                      WRONG_REALM / NOT_ON_TAPLIST (22 / 23) u8 slot (0xFF = the money).
//   SMSG_TRADE_STATUS_EXTENDED: u8 side (0 = ours, 1 = the other's), u32 trade id, u32 + u32 (AzerothCore writes the
//                      slot count twice; unused), u32 gold, u32 the spell on the side's "will not be traded" item,
//                      then 73-byte slot records: u8 slot, u32 entry, display, count, wrapped, u64 gift creator, u32
//                      permanent enchant, 3 x u32 gems, u64 creator, u32 charges, suffix factor, random property,
//                      lock id, max durability, durability (upstream inventory_handler.cpp handleTradeStatusExtended).
//   CMSG_INITIATE_TRADE: u64 target. CMSG_BEGIN / BUSY / IGNORE / CANCEL / ACCEPT / UNACCEPT_TRADE: empty
//                      (upstream AcceptTradePacket has no body; AzerothCore reads none).
//   CMSG_SET_TRADE_ITEM: u8 trade slot, u8 bag, u8 slot. CMSG_CLEAR_TRADE_ITEM: u8 trade slot.
//   CMSG_SET_TRADE_GOLD: u32 copper.
//   SMSG_DUEL_REQUESTED: u64 duel flag (arbiter), u64 challenger (upstream social_handler.cpp handleDuelRequested).
//   CMSG_DUEL_ACCEPTED / _CANCELLED: u64 arbiter (upstream DuelAcceptPacket / DuelCancelPacket).
//   SMSG_DUEL_COUNTDOWN: u32 ms. SMSG_DUEL_COMPLETE: u8 started. SMSG_DUEL_WINNER: u8 (0 won, 1 fled), two names.
//   SMSG_SUMMON_REQUEST: u64 summoner, u32 zone, u32 ms to answer. CMSG_SUMMON_RESPONSE: u64 summoner, u8 yes.
//   SMSG_QUEST_CONFIRM_ACCEPT: u32 quest, title, u64 starter. CMSG_QUEST_CONFIRM_ACCEPT / PUSHQUESTTOPARTY: u32 quest.
//   MSG_QUEST_PUSH_RESULT: in u64 player, u8 message (QUEST_PARTY_MSG 0..10); out u64 player, u32 quest, u8 message.
#pragma once

#include "network/packet.hpp"
#include "platform/psp/psp_game_line.hpp"
#include "platform/psp/psp_strings.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace wowee::platform::psp::tradeduel {

// TradeStatus (AzerothCore SharedDefines.h).
enum TradeStatus : uint32_t {
    kTradeBusy = 0, kTradeBegin = 1, kTradeOpenWindow = 2, kTradeCanceled = 3, kTradeAccept = 4, kTradeBusy2 = 5,
    kTradeNoTarget = 6, kTradeBackToTrade = 7, kTradeComplete = 8, kTradeRejected = 9, kTradeTargetTooFar = 10,
    kTradeWrongFaction = 11, kTradeCloseWindow = 12, kTrade13 = 13, kTradeIgnoreYou = 14, kTradeYouStunned = 15,
    kTradeTargetStunned = 16, kTradeYouDead = 17, kTradeTargetDead = 18, kTradeYouLogout = 19, kTradeTargetLogout = 20,
    kTradeTrialAccount = 21, kTradeWrongRealm = 22, kTradeNotOnTaplist = 23,
};
constexpr int kTradeSlots = 7;          // AzerothCore TRADE_SLOT_COUNT (the 7th: TRADE_SLOT_NONTRADED)
constexpr int kTradableSlots = 6;       // TRADE_SLOT_TRADED_COUNT
constexpr size_t kTradeSlotBytes = 73;  // 1 + 4 * 4 + 8 + 4 * 4 + 8 + 4 * 6
// InventoryResult values the trade lines name (AzerothCore Item.h).
constexpr uint32_t kEquipErrBagFull = 4;              // EQUIP_ERR_BAG_FULL
constexpr uint32_t kEquipErrCantCarryMore = 17;       // EQUIP_ERR_CANT_CARRY_MORE_OF_THIS

struct TradeStatusRead {
    uint32_t status = 0;
    uint64_t guid = 0;           // BEGIN_TRADE
    uint32_t tradeId = 0;        // OPEN_WINDOW
    uint32_t result = 0;         // CLOSE_WINDOW: InventoryResult
    bool isTarget = false;       // CLOSE_WINDOW
    uint32_t limitCategory = 0;  // CLOSE_WINDOW
    uint8_t slot = 0xFF;         // WRONG_REALM / NOT_ON_TAPLIST (0xFF: the money)
};

inline bool readTradeStatus(network::Packet& p, TradeStatusRead& out) {
    out = TradeStatusRead{};
    if (!p.hasRemaining(4)) return false;
    out.status = p.readUInt32();
    switch (out.status) {
        case kTradeBegin:
            if (!p.hasRemaining(8)) return false;
            out.guid = p.readUInt64();
            break;
        case kTradeOpenWindow:
            if (!p.hasRemaining(4)) return false;
            out.tradeId = p.readUInt32();
            break;
        case kTradeCloseWindow:
            if (!p.hasRemaining(9)) return false;
            out.result = p.readUInt32();
            out.isTarget = p.readUInt8() != 0;
            out.limitCategory = p.readUInt32();
            break;
        case kTradeWrongRealm:
        case kTradeNotOnTaplist:
            if (!p.hasRemaining(1)) return false;
            out.slot = p.readUInt8();
            break;
        default: break;
    }
    return true;
}

struct TradeSlotRecord {
    uint32_t entry = 0, displayId = 0, count = 0, wrapped = 0;
    uint64_t giftCreator = 0;
    uint32_t permEnchant = 0;
    uint32_t gems[3] = {};
    uint64_t creator = 0;
    uint32_t charges = 0, suffixFactor = 0;
    int32_t randomProperty = 0;
    uint32_t lockId = 0, maxDurability = 0, durability = 0;
};

struct TradeExtendedRead {
    uint8_t side = 0;  // 0 = our offer, 1 = the other player's
    uint32_t tradeId = 0, gold = 0, spell = 0;
    std::array<TradeSlotRecord, kTradeSlots> slots{};
    std::array<bool, kTradeSlots> present{};  // a record arrived for the slot
    int records = 0;
};

// The header (21 bytes) and the records while 73 bytes remain (a record of a slot past 6 is read and dropped).
inline bool readTradeExtended(network::Packet& p, TradeExtendedRead& out) {
    out = TradeExtendedRead{};
    if (!p.hasRemaining(21)) return false;
    out.side = p.readUInt8();
    out.tradeId = p.readUInt32();
    p.readUInt32();  // slot count (AzerothCore SendUpdateTrade)
    p.readUInt32();  // slot count again
    out.gold = p.readUInt32();
    out.spell = p.readUInt32();
    while (p.hasRemaining(kTradeSlotBytes)) {
        TradeSlotRecord r;
        const uint8_t slot = p.readUInt8();
        r.entry = p.readUInt32();
        r.displayId = p.readUInt32();
        r.count = p.readUInt32();
        r.wrapped = p.readUInt32();
        r.giftCreator = p.readUInt64();
        r.permEnchant = p.readUInt32();
        for (uint32_t& g : r.gems) g = p.readUInt32();
        r.creator = p.readUInt64();
        r.charges = p.readUInt32();
        r.suffixFactor = p.readUInt32();
        r.randomProperty = static_cast<int32_t>(p.readUInt32());
        r.lockId = p.readUInt32();
        r.maxDurability = p.readUInt32();
        r.durability = p.readUInt32();
        ++out.records;
        if (slot < kTradeSlots) {
            out.slots[slot] = r;
            out.present[slot] = true;
        }
    }
    return true;
}

// --- Outgoing packets (wire opcode given: the PSP takes it from opcodes.json) ---
inline network::Packet buildEmpty(uint16_t op) { return network::Packet(op); }
inline network::Packet buildU32(uint16_t op, uint32_t v) {
    network::Packet p(op);
    p.writeUInt32(v);
    return p;
}
inline network::Packet buildGuid(uint16_t op, uint64_t guid) {
    network::Packet p(op);
    p.writeUInt64(guid);
    return p;
}
inline network::Packet buildSetTradeItem(uint16_t op, uint8_t tradeSlot, uint8_t bag, uint8_t slot) {
    network::Packet p(op);
    p.writeUInt8(tradeSlot);
    p.writeUInt8(bag);
    p.writeUInt8(slot);
    return p;
}
inline network::Packet buildClearTradeItem(uint16_t op, uint8_t tradeSlot) {
    network::Packet p(op);
    p.writeUInt8(tradeSlot);
    return p;
}
inline network::Packet buildSummonResponse(uint16_t op, uint64_t summoner, bool accept) {
    network::Packet p(op);
    p.writeUInt64(summoner);
    p.writeUInt8(accept ? 1 : 0);
    return p;
}
inline network::Packet buildQuestPushResult(uint16_t op, uint64_t guid, uint32_t questId, uint8_t msg) {
    network::Packet p(op);
    p.writeUInt64(guid);
    p.writeUInt32(questId);
    p.writeUInt8(msg);
    return p;
}
constexpr uint8_t kQuestPushDecline = 3;  // AzerothCore QuestDef.h QUEST_PARTY_MSG_DECLINE_QUEST

struct DuelRequestedRead {
    uint64_t arbiter = 0, challenger = 0;
};
inline bool readDuelRequested(network::Packet& p, DuelRequestedRead& out) {
    if (!p.hasRemaining(16)) return false;
    out.arbiter = p.readUInt64();
    out.challenger = p.readUInt64();
    return true;
}

struct DuelWinnerRead {
    bool retreat = false;  // DUEL_WINNER_RETREAT (else _KNOCKOUT)
    std::string first, second;
};
inline bool readDuelWinner(network::Packet& p, DuelWinnerRead& out) {
    if (!p.hasRemaining(1)) return false;
    out.retreat = p.readUInt8() != 0;
    out.first = p.hasRemaining(1) ? p.readString() : std::string();
    out.second = p.hasRemaining(1) ? p.readString() : std::string();
    return true;
}

struct SummonRequestRead {
    uint64_t summoner = 0;
    uint32_t zoneId = 0, timeoutMs = 0;
};
inline bool readSummonRequest(network::Packet& p, SummonRequestRead& out) {
    if (!p.hasRemaining(16)) return false;
    out.summoner = p.readUInt64();
    out.zoneId = p.readUInt32();
    out.timeoutMs = p.readUInt32();
    return true;
}

struct QuestConfirmRead {
    uint32_t questId = 0;
    std::string title;
    uint64_t starter = 0;
};
inline bool readQuestConfirmAccept(network::Packet& p, QuestConfirmRead& out) {
    if (!p.hasRemaining(4)) return false;
    out.questId = p.readUInt32();
    out.title = p.hasRemaining(1) ? p.readString() : std::string();
    out.starter = p.hasRemaining(8) ? p.readUInt64() : 0;
    return true;
}

struct QuestPushResultRead {
    uint64_t guid = 0;
    uint8_t msg = 0;
};
inline bool readQuestPushResult(network::Packet& p, QuestPushResultRead& out) {
    if (!p.hasRemaining(9)) return false;
    out.guid = p.readUInt64();
    out.msg = p.readUInt8();
    return true;
}

// --- The lines (GlobalStrings keys; texts from strings.pak, psp_strings.hpp) ---
// Where each shows is the client-wide rule of psp_game_line.hpp (GameLine): a refusal or a failure of what the player
// tried -> the red error line, news -> the system chat; upstream's place where it shows the message one way only
// (inventory_handler.cpp handleTradeStatus: busy, too far, logging out, cancelled / rejected, complete in the chat,
// ignoring you red; social_handler.cpp handleDuelComplete: the duel cancelled in the chat).
struct LineText {
    const char* key;
    CS text;
    GameLine line;
};
constexpr GameLine kRef = GameLine::Refused, kNews = GameLine::News;
constexpr LineText kLineTexts[] = {
    {"ERR_PLAYER_BUSY_S", CS::ERR_PLAYER_BUSY_S, kNews},
    {"ERR_PLAYER_DEAD", CS::ERR_PLAYER_DEAD, kRef},
    {"ERR_GENERIC_NO_TARGET", CS::ERR_GENERIC_NO_TARGET, kRef},
    {"ERR_INITIATE_TRADE_S", CS::ERR_INITIATE_TRADE_S, kNews},
    {"ERR_TRADE_TARGET_DEAD", CS::ERR_TRADE_TARGET_DEAD, kRef},
    {"ERR_TRADE_TOO_FAR", CS::ERR_TRADE_TOO_FAR, kNews},
    {"ERR_TRADE_CANCELLED", CS::ERR_TRADE_CANCELLED, kNews},
    {"ERR_TRADE_COMPLETE", CS::ERR_TRADE_COMPLETE, kNews},
    {"ERR_TRADE_BAG_FULL", CS::ERR_TRADE_BAG_FULL, kRef},
    {"ERR_TRADE_TARGET_BAG_FULL", CS::ERR_TRADE_TARGET_BAG_FULL, kRef},
    {"ERR_TRADE_MAX_COUNT_EXCEEDED", CS::ERR_TRADE_MAX_COUNT_EXCEEDED, kRef},
    {"ERR_TRADE_TARGET_MAX_COUNT_EXCEEDED", CS::ERR_TRADE_TARGET_MAX_COUNT_EXCEEDED, kRef},
    {"ERR_ALREADY_TRADING", CS::ERR_ALREADY_TRADING, kRef},
    {"ERR_PLAYER_WRONG_FACTION", CS::ERR_PLAYER_WRONG_FACTION, kRef},
    {"ERR_IGNORING_YOU_S", CS::ERR_IGNORING_YOU_S, kRef},
    {"ERR_GENERIC_STUNNED", CS::ERR_GENERIC_STUNNED, kRef},
    {"ERR_TARGET_STUNNED", CS::ERR_TARGET_STUNNED, kRef},
    {"ERR_LOGGING_OUT", CS::ERR_LOGGING_OUT, kNews},
    {"ERR_TARGET_LOGGING_OUT", CS::ERR_TARGET_LOGGING_OUT, kNews},
    {"ERR_RESTRICTED_ACCOUNT", CS::ERR_RESTRICTED_ACCOUNT, kRef},
    {"ERR_TRADE_WRONG_REALM", CS::ERR_TRADE_WRONG_REALM, kRef},
    {"ERR_TRADE_NOT_ON_TAPLIST", CS::ERR_TRADE_NOT_ON_TAPLIST, kRef},
    {"ERR_TRADE_BOUND_ITEM", CS::ERR_TRADE_BOUND_ITEM, kRef},
    {"ERR_TRADE_BAG", CS::ERR_TRADE_BAG, kRef},
    {"ERR_TRADE_EQUIPPED_BAG", CS::ERR_TRADE_EQUIPPED_BAG, kRef},
    {"ERR_DUEL_CANCELLED", CS::ERR_DUEL_CANCELLED, kNews},
    {"ERR_QUEST_PUSH_SUCCESS_S", CS::ERR_QUEST_PUSH_SUCCESS_S, kNews},
    {"ERR_QUEST_PUSH_INVALID_S", CS::ERR_QUEST_PUSH_INVALID_S, kRef},
    {"ERR_QUEST_PUSH_ACCEPTED_S", CS::ERR_QUEST_PUSH_ACCEPTED_S, kNews},
    {"ERR_QUEST_PUSH_DECLINED_S", CS::ERR_QUEST_PUSH_DECLINED_S, kRef},
    {"ERR_QUEST_PUSH_BUSY_S", CS::ERR_QUEST_PUSH_BUSY_S, kRef},
    {"ERR_QUEST_PUSH_LOG_FULL_S", CS::ERR_QUEST_PUSH_LOG_FULL_S, kRef},
    {"ERR_QUEST_PUSH_ONQUEST_S", CS::ERR_QUEST_PUSH_ONQUEST_S, kRef},
    {"ERR_QUEST_PUSH_ALREADY_DONE_S", CS::ERR_QUEST_PUSH_ALREADY_DONE_S, kRef},
    {"ERR_QUEST_PUSH_NOT_DAILY_S", CS::ERR_QUEST_PUSH_NOT_DAILY_S, kRef},
    {"ERR_QUEST_PUSH_TIMER_EXPIRED_S", CS::ERR_QUEST_PUSH_TIMER_EXPIRED_S, kRef},
    {"ERR_QUEST_PUSH_NOT_IN_PARTY_S", CS::ERR_QUEST_PUSH_NOT_IN_PARTY_S, kRef},
};

inline const LineText* lineText(const char* key) {
    for (const LineText& e : kLineTexts)
        if (std::strcmp(e.key, key) == 0) return &e;
    return nullptr;
}

// MSG_QUEST_PUSH_RESULT's message -> its line: AzerothCore QuestDef.h QUEST_PARTY_MSG_SHARING_QUEST (0) ..
// QUEST_PARTY_MSG_NOT_IN_PARTY (10), each matched to the GlobalStrings ERR_QUEST_PUSH_*_S key of the same name.
// Any other message: no line.
inline const char* questPushKey(uint8_t msg) {
    static const char* const keys[11] = {
        "ERR_QUEST_PUSH_SUCCESS_S", "ERR_QUEST_PUSH_INVALID_S", "ERR_QUEST_PUSH_ACCEPTED_S",
        "ERR_QUEST_PUSH_DECLINED_S", "ERR_QUEST_PUSH_BUSY_S", "ERR_QUEST_PUSH_LOG_FULL_S",
        "ERR_QUEST_PUSH_ONQUEST_S", "ERR_QUEST_PUSH_ALREADY_DONE_S", "ERR_QUEST_PUSH_NOT_DAILY_S",
        "ERR_QUEST_PUSH_TIMER_EXPIRED_S", "ERR_QUEST_PUSH_NOT_IN_PARTY_S"};
    return msg < 11 ? keys[msg] : nullptr;
}

// The line of a trade status - the port's own pairing of AzerothCore's TradeStatus names with the GlobalStrings keys
// of the same meaning. nullptr: no line. needsName: the line names the player we asked. CLOSE_WINDOW: a full bag /
// "can't carry more" names whose bags it is (AzerothCore HandleAcceptTradeOpcode sends isTargetResult = 1 to the
// player whose bags cannot take the items, 0 to the other); any other non-zero result -> genericResult (the port's
// inventory error line of that result).
struct TradeStatusLine {
    const char* key = nullptr;
    bool needsName = false;
    bool genericResult = false;
};
inline TradeStatusLine tradeStatusLine(uint32_t status, uint32_t result = 0, bool isTarget = false) {
    TradeStatusLine l;
    switch (status) {
        case kTradeBusy:
        case kTradeBusy2: l.key = "ERR_PLAYER_BUSY_S"; l.needsName = true; break;
        case kTradeCanceled:
        case kTradeRejected: l.key = "ERR_TRADE_CANCELLED"; break;
        case kTradeNoTarget: l.key = "ERR_GENERIC_NO_TARGET"; break;
        case kTradeComplete: l.key = "ERR_TRADE_COMPLETE"; break;
        case kTradeTargetTooFar: l.key = "ERR_TRADE_TOO_FAR"; break;
        case kTradeWrongFaction: l.key = "ERR_PLAYER_WRONG_FACTION"; break;
        case kTradeIgnoreYou: l.key = "ERR_IGNORING_YOU_S"; l.needsName = true; break;
        case kTradeYouStunned: l.key = "ERR_GENERIC_STUNNED"; break;
        case kTradeTargetStunned: l.key = "ERR_TARGET_STUNNED"; break;
        case kTradeYouDead: l.key = "ERR_PLAYER_DEAD"; break;
        case kTradeTargetDead: l.key = "ERR_TRADE_TARGET_DEAD"; break;
        case kTradeYouLogout: l.key = "ERR_LOGGING_OUT"; break;
        case kTradeTargetLogout: l.key = "ERR_TARGET_LOGGING_OUT"; break;
        case kTradeTrialAccount: l.key = "ERR_RESTRICTED_ACCOUNT"; break;
        case kTradeWrongRealm: l.key = "ERR_TRADE_WRONG_REALM"; break;
        case kTradeNotOnTaplist: l.key = "ERR_TRADE_NOT_ON_TAPLIST"; break;
        case kTradeCloseWindow:
            if (result == kEquipErrBagFull) l.key = isTarget ? "ERR_TRADE_BAG_FULL" : "ERR_TRADE_TARGET_BAG_FULL";
            else if (result == kEquipErrCantCarryMore)
                l.key = isTarget ? "ERR_TRADE_MAX_COUNT_EXCEEDED" : "ERR_TRADE_TARGET_MAX_COUNT_EXCEEDED";
            else l.genericResult = result != 0;
            break;
        default: break;  // BEGIN / OPEN_WINDOW / ACCEPT / BACK_TO_TRADE / 13: no line
    }
    return l;
}

// The port's own rule for our pending request (InitiateTrade): it ends with every status except the ones of a
// trade in progress (BEGIN_TRADE - somebody asks us -, OPEN_WINDOW, ACCEPT, BACK_TO_TRADE and the slot statuses
// WRONG_REALM / NOT_ON_TAPLIST).
inline bool tradeStatusEndsRequest(uint32_t status) {
    switch (status) {
        case kTradeBegin: case kTradeOpenWindow: case kTradeAccept: case kTradeBackToTrade:
        case kTradeWrongRealm: case kTradeNotOnTaplist:
            return false;
        default:
            return true;
    }
}

// Duel opponents are hostile to each other while the duel runs (AzerothCore Unit::GetReactionTo: "duel - always
// hostile to opponent"). What the PSP sees of it are the public player fields: both players under the same
// PLAYER_DUEL_ARBITER (non-zero), each with a PLAYER_DUEL_TEAM (set when the duel starts, cleared when it ends) and
// the two teams different -> true. Otherwise the faction rules decide.
inline bool duelHostile(uint64_t arbiterA, uint32_t teamA, uint64_t arbiterB, uint32_t teamB) {
    return arbiterA != 0 && arbiterA == arbiterB && teamA != 0 && teamB != 0 && teamA != teamB;
}

// A GlobalStrings format with one %s (or none), |3-N(x) cut to x.
inline std::string formatName(const char* f, const std::string& name) {
    std::string out;
    bool used = false;
    for (const char* p = f; *p; ++p) {
        if (p[0] == '|' && p[1] == '3' && p[2] == '-' && p[3] >= '0' && p[3] <= '9' && p[4] == '(') {
            p += 4;  // the "(": what is inside stays, the ")" goes
            const char* close = p + 1;
            while (*close && *close != ')') ++close;
            for (const char* q = p + 1; q < close; ++q) {
                if (q[0] == '%' && q[1] == 's' && !used) {
                    out += name;
                    used = true;
                    ++q;
                } else {
                    out.push_back(*q);
                }
            }
            p = *close ? close : close - 1;
            continue;
        }
        if (p[0] == '%' && p[1] == 's' && !used) {
            out += name;
            used = true;
            ++p;
            continue;
        }
        out.push_back(*p);
    }
    return out;
}

}  // namespace wowee::platform::psp::tradeduel
