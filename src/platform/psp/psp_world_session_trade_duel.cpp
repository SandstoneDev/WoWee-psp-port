// The trade, the duels and the requests the server asks the player to answer (summons, escort quests, shared quests)
// for the PSP world session (the packet layouts in include/platform/psp/psp_trade_duel_parse.hpp). The flow follows
// upstream WoWee (src/game/inventory_handler.cpp handleTradeStatus / acceptTradeRequest / cancelTrade,
// social_handler.cpp handleDuelRequested / handleSummonRequest, quest_handler.cpp handleQuestConfirmAccept) and what
// AzerothCore's handlers expect (Handlers/TradeHandler.cpp, DuelHandler.cpp, QuestHandler.cpp); the port's own rules
// where neither says:
//   - a trade request asks the player (the "TRADE" popup: YES = CMSG_BEGIN_TRADE, NO / time-out = CMSG_CANCEL_TRADE,
//     as upstream's acceptTradeRequest / declineTradeRequest); the server refuses the other cases itself (dead,
//     busy, ignored, too far, wrong faction - AzerothCore HandleInitiateTradeOpcode);
//   - our offer remembers the bag place and guid of each item we put up (shown locked in the bags); the server's
//     report of our side clears a slot it says is empty;
//   - CMSG_ACCEPT_TRADE has no body, BACK_TO_TRADE only clears both acceptances, TRADE_REJECTED closes the trade
//     (all upstream);
//   - the duel request's first guid is the duel flag (arbiter), the second the challenger; our own request asks
//     nothing (upstream); CMSG_DUEL_ACCEPTED / _CANCELLED carry the arbiter (upstream DuelAcceptPacket);
//   - the duel countdown is shown on the yellow info line, one number a second; duel opponents are hostile by the
//     public PLAYER_DUEL_ARBITER / PLAYER_DUEL_TEAM fields (AzerothCore Unit::GetReactionTo);
//   - summons: ConfirmSummon always answers (upstream PlayerCanTeleport is true; the server ignores an answer while
//     dead or in combat); the popup's accept is off in combat (public CONFIRM_SUMMON OnUpdate);
//   - MSG_QUEST_PUSH_RESULT -> the ERR_QUEST_PUSH_*_S line of the AzerothCore message; declining a quest a player
//     shared answers MSG_QUEST_PUSH_RESULT 3 (AzerothCore HandleQuestPushResult forwards it to the sharer);
//     QuestLogPushQuest only for a quest with QUEST_FLAGS_SHARABLE (AzerothCore Player::CanShareQuest).
// Where a line goes: the client-wide GameLine rule (red refusals, news in the chat), per key in psp_trade_duel_parse.hpp;
// the yellow info line carries the duel countdown only.
#include "platform/psp/psp_world_session.hpp"

#include "game/inventory_slots.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

namespace td = tradeduel;

namespace {

constexpr uint32_t kSpellDuel = 7266;               // Duel (upstream DuelProposedPacket casts it)
constexpr uint32_t kItemFlagBopTradeable = 0x100u;  // AzerothCore ITEM_FIELD_FLAG_BOP_TRADEABLE
constexpr uint32_t kItemFlagSoulbound = 0x1u;       // AzerothCore ITEM_FIELD_FLAG_SOULBOUND
constexpr uint32_t kUnitFlagInCombat = 0x80000u;    // AzerothCore UNIT_FLAG_IN_COMBAT (UnitAffectingCombat)
constexpr uint32_t kQuestFlagSharable = 0x8u;       // AzerothCore QuestDef.h QUEST_FLAGS_SHARABLE
constexpr int kMaxQuests = 25;                      // MAX_QUESTS (public QuestLogFrame.lua)
constexpr uint16_t kFieldDuelArbiter = 148;         // PLAYER_DUEL_ARBITER (AzerothCore UpdateFields.h), 2 fields
constexpr uint16_t kFieldDuelTeam = 156;            // PLAYER_DUEL_TEAM
constexpr uint32_t kTradeAskMs = 60000u;            // PSP: our unanswered request is forgotten after 60 s
constexpr uint32_t kNameWaitMs = 10000u;            // PSP: a named line waits for the name query 10 s at most

uint32_t readLe32(const std::vector<uint8_t>& d, size_t at) {
    return at + 4 <= d.size() ? static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
                                    (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24)
                              : 0u;
}

// A Lua format with %1$s / %2$s / %d (DUEL_WINNER_*), |3-N(x) cut to x.
std::string fmtPositional(const char* f, const std::string& a, const std::string& b) {
    std::string out;
    int next = 0;
    bool inDecl = false;  // inside |3-N( ... )
    for (const char* p = f; *p; ++p) {
        if (p[0] == '|' && p[1] == '3' && p[2] == '-' && p[3] >= '0' && p[3] <= '9' && p[4] == '(') {
            p += 4;  // keep what is inside the brackets
            inDecl = true;
            continue;
        }
        if (*p == ')' && inDecl) {
            inDecl = false;
            continue;
        }
        if (p[0] == '%' && p[1] >= '1' && p[1] <= '2' && p[2] == '$' && (p[3] == 's' || p[3] == 'd')) {
            out += p[1] == '1' ? a : b;
            p += 3;
            continue;
        }
        if (p[0] == '%' && (p[1] == 's' || p[1] == 'd')) {
            out += next++ == 0 ? a : b;
            ++p;
            continue;
        }
        out.push_back(*p);
    }
    return out;
}

}  // namespace

// --- Lines --------------------------------------------------------------------------------------------------------

void WorldSession::tdLine(const char* key, const std::string& name) {
    const td::LineText* e = td::lineText(key);
    if (!e) return;
    const std::string text = td::formatName(cs(e->text), name);
    gameLine(e->line, text);
    if (++tdLogs_ <= 120)
        PSP_LOGI(tag::Net, "td_line %s kind=%s", key, e->line == GameLine::Refused ? "red" : "chat");
}

// The line now when the player's name is known, else once the name query is answered (kNameWaitMs at most).
void WorldSession::tdLineNamed(const char* key, uint64_t guid) {
    if (const std::string n = tdNameOf(guid); !n.empty() || guid == 0) {
        tdLine(key, n);
        return;
    }
    if (socket_ && nameQueried_.insert(guid).second) socket_->send(game::NameQueryPacket::build(guid));
    if (tdAwait_.size() < 16) tdAwait_.push_back(TdAwait{key, guid, logUptimeMs()});
}

std::string WorldSession::tdNameOf(uint64_t guid) const {
    if (const std::string* n = nameOf(guid)) return *n;
    if (const game::GroupMember* m = partyMember(guid); m && !m->name.empty()) return m->name;
    return std::string();
}

// The name for a popup: known now, else the query is sent (the popup shows it once the answer is in).
std::string WorldSession::tdNameOrQuery(uint64_t guid) {
    std::string n = tdNameOf(guid);
    if (n.empty() && guid != 0 && socket_ && nameQueried_.insert(guid).second)
        socket_->send(game::NameQueryPacket::build(guid));
    return n;
}

bool WorldSession::takeTradeDuelInfo(std::string& out) {
    if (tdInfo_.empty()) return false;
    out = std::move(tdInfo_.front());
    tdInfo_.pop_front();
    return true;
}

// The world screen calls it every frame: the duel countdown (one number a second on the info line), the lines
// that wait for a name, the summon's time-out (the popup's OnCancel = CancelSummon).
void WorldSession::tradeDuelTick() {
    const uint32_t now = logUptimeMs();
    while (duelCountdown_ > 0 && static_cast<int32_t>(now - duelCountdownNextMs_) >= 0) {
        tdInfo_.push_back(csf(CS::DUEL_COUNTDOWN, static_cast<unsigned>(duelCountdown_)));
        --duelCountdown_;
        duelCountdownNextMs_ += 1000u;
    }
    for (size_t i = 0; i < tdAwait_.size();) {
        const TdAwait& a = tdAwait_[i];
        const std::string name = tdNameOf(a.guid);
        if (!name.empty() || now - a.atMs > kNameWaitMs) {
            tdLine(a.key, name);
            tdAwait_.erase(tdAwait_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (summon_.pending && static_cast<int32_t>(now - summon_.expiresMs) >= 0) cancelSummon();
}

// --- Packets ------------------------------------------------------------------------------------------------------

bool WorldSession::handleTradeDuelPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_TRADE_STATUS: {
            td::TradeStatusRead s;
            if (td::readTradeStatus(packet, s)) tradeStatus(s);
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_TRADE_STATUS_EXTENDED: {
            td::TradeExtendedRead x;
            if (td::readTradeExtended(packet, x)) tradeExtended(x);
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_DUEL_REQUESTED: {
            td::DuelRequestedRead d;
            if (!td::readDuelRequested(packet, d)) return true;
            duel_.arbiter = d.arbiter;
            const bool own = d.challenger == playerGuid_;
            if (!own) {  // upstream: the challenger keeps the flag and is not asked; everybody else gets the popup
                duel_.challenger = d.challenger;
                duel_.challengerName = tdNameOrQuery(d.challenger);
                duel_.requested = true;
                ++duel_.requestSeq;
            }
            PSP_LOGI(tag::Net, "duel_requested arbiter=%llx challenger=%llx self=%d",
                     static_cast<unsigned long long>(d.arbiter), static_cast<unsigned long long>(d.challenger),
                     own ? 1 : 0);
            return true;
        }
        case LogicalOpcode::SMSG_DUEL_OUTOFBOUNDS:  // DUEL_OUTOFBOUNDS
            duel_.outOfBounds = true;
            ++duel_.boundsSeq;
            PSP_LOGI(tag::Net, "duel_out_of_bounds");
            return true;
        case LogicalOpcode::SMSG_DUEL_INBOUNDS:  // DUEL_INBOUNDS
            duel_.outOfBounds = false;
            PSP_LOGI(tag::Net, "duel_in_bounds");
            return true;
        case LogicalOpcode::SMSG_DUEL_COUNTDOWN: {
            if (!packet.hasRemaining(4)) return true;
            duelCountdown_ = packet.readUInt32() / 1000u;
            duelCountdownNextMs_ = logUptimeMs();  // the first number now
            PSP_LOGI(tag::Net, "duel_countdown s=%u", static_cast<unsigned>(duelCountdown_));
            return true;
        }
        case LogicalOpcode::SMSG_DUEL_COMPLETE: {
            const bool started = packet.hasRemaining(1) && packet.readUInt8() != 0;
            if (!started) tdLine("ERR_DUEL_CANCELLED", std::string());  // upstream handleDuelComplete
            duel_.arbiter = 0;
            duel_.requested = false;  // DUEL_FINISHED: the DUEL_REQUESTED / DUEL_OUTOFBOUNDS popups hide
            duel_.outOfBounds = false;
            ++duel_.finishedSeq;
            duelCountdown_ = 0;
            duel_.challenger = 0;
            PSP_LOGI(tag::Net, "duel_complete started=%d", started ? 1 : 0);
            return true;
        }
        case LogicalOpcode::SMSG_DUEL_WINNER: {
            td::DuelWinnerRead w;
            if (!td::readDuelWinner(packet, w)) return true;
            // DUEL_WINNER_KNOCKOUT / _RETREAT (positional %1$s / %2$s) with the two names in the packet's order, in
            // the chat (upstream handleDuelWinner).
            addSystemChatMessage(fmtPositional(w.retreat ? cs(CS::DUEL_WINNER_RETREAT) : cs(CS::DUEL_WINNER_KNOCKOUT),
                                               w.first, w.second));
            PSP_LOGI(tag::Net, "duel_winner retreat=%d", w.retreat ? 1 : 0);
            return true;
        }
        case LogicalOpcode::SMSG_SUMMON_REQUEST: {
            td::SummonRequestRead r;
            if (!td::readSummonRequest(packet, r)) return true;
            summon_.summoner = r.summoner;
            summon_.zoneId = r.zoneId;
            summon_.expiresMs = logUptimeMs() + r.timeoutMs;
            summon_.pending = true;
            summon_.name = tdNameOrQuery(r.summoner);
            ++summon_.seq;  // CONFIRM_SUMMON
            PSP_LOGI(tag::Net, "summon_request zone=%u ms=%u", static_cast<unsigned>(r.zoneId),
                     static_cast<unsigned>(r.timeoutMs));
            return true;
        }
        case LogicalOpcode::SMSG_SUMMON_CANCEL:  // CANCEL_SUMMON (upstream game_handler_packets.cpp)
            summon_.pending = false;
            ++summon_.seq;
            PSP_LOGI(tag::Net, "summon_cancel");
            return true;
        case LogicalOpcode::SMSG_QUEST_CONFIRM_ACCEPT: {
            td::QuestConfirmRead q;
            if (!td::readQuestConfirmAccept(packet, q)) return true;
            questConfirm_.questId = q.questId;
            questConfirm_.title = q.title;
            questConfirm_.starter = q.starter;
            questConfirm_.name = tdNameOrQuery(q.starter);
            questConfirm_.pending = true;  // QUEST_ACCEPT_CONFIRM at once (upstream handleQuestConfirmAccept)
            ++questConfirm_.seq;
            PSP_LOGI(tag::Net, "quest_confirm quest=%u", static_cast<unsigned>(q.questId));
            return true;
        }
        case LogicalOpcode::MSG_QUEST_PUSH_RESULT: {
            td::QuestPushResultRead r;
            if (!td::readQuestPushResult(packet, r)) return true;
            if (const char* key = td::questPushKey(r.msg)) tdLineNamed(key, r.guid);
            PSP_LOGI(tag::Net, "quest_push_result msg=%u", static_cast<unsigned>(r.msg));
            return true;
        }
        case LogicalOpcode::SMSG_QUEST_QUERY_RESPONSE: {
            // Peeked (psp_world_session_npc.cpp reads it): the quest's flags - the u32 after the 20 u32 fields
            // AzerothCore's GossipDef.cpp SendQuestQueryResponse writes first - for GetQuestLogPushable.
            const auto& d = packet.getData();
            if (d.size() >= 84 && questShareFlags_.size() < 512) questShareFlags_[readLe32(d, 0)] = readLe32(d, 80);
            return false;
        }
        default:
            return false;
    }
}

// --- Trade --------------------------------------------------------------------------------------------------------

// The guid of a bag item by its wire place (bag 255 = the backpack / worn slots, 19..22 = the worn bags).
uint64_t WorldSession::wireItemGuid(uint8_t bag, uint8_t slot) const {
    if (bag == game::slots::kNoContainer) {
        if (slot >= game::slots::kBackpackFirst && slot < game::slots::kBackpackFirst + 16)
            return backpackSlotGuids_[slot - game::slots::kBackpackFirst];
        return slot < equipSlotGuids_.size() ? equipSlotGuids_[slot] : 0;
    }
    if (bag < game::slots::kWornBagFirst || bag >= game::slots::kWornBagFirst + game::slots::kWornBagCount) return 0;
    const uint64_t bagGuid = equipSlotGuids_[bag];
    auto it = containerContents_.find(bagGuid);
    if (it == containerContents_.end() || slot >= it->second.numSlots) return 0;
    return it->second.slotGuids[slot];
}

uint32_t WorldSession::itemInstanceFlags(uint64_t itemGuid) const {
    auto it = onlineItems_.find(itemGuid);
    return it == onlineItems_.end() ? 0u : it->second.flags;
}

void WorldSession::sendTradeEmpty(game::LogicalOpcode op) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(td::buildEmpty(game::wireOpcode(op)));
}

// Our offer forgotten (the trade is over): the items unlock.
void WorldSession::tradeUnlockAll() {
    for (TradeMine& m : tradeMine_) m = TradeMine{};
}

// The window shuts (TRADE_CLOSED). cancelServer: also CMSG_CANCEL_TRADE - the port's rule for a close the server did
// not end itself (our close, CLOSE_WINDOW: AzerothCore keeps its trade data after a failed accept), so a new trade
// can start (AzerothCore HandleCancelTradeOpcode -> TradeCancel; harmless when there is none).
void WorldSession::tradeCloseWindow(bool completed, bool cancelServer) {
    if (tradeWindow_) {
        tradeWindow_ = false;
        tradeCompleted_ = completed;
        ++tradeSeq_;
        PSP_LOGI(tag::Net, "trade_closed completed=%d", completed ? 1 : 0);
    }
    for (auto& side : tradeView_) side = {};
    tradeGold_[0] = tradeGold_[1] = 0;
    tradeSpell_[0] = tradeSpell_[1] = 0;
    tradeSelfAccepted_ = tradePeerAccepted_ = false;
    tradeUnlockAll();
    if (tradeBindAsk_) {
        tradeBindAsk_ = false;
        ++tradeBindSeq_;
    }
    if (cancelServer) sendTradeEmpty(game::LogicalOpcode::CMSG_CANCEL_TRADE);
}

void WorldSession::tradeForgetRequest() {
    tradeAsking_ = false;
    tradeRequestGuid_ = 0;
    if (tradeAsked_) {
        tradeAsked_ = false;
        ++tradeAskSeq_;  // the TRADE popup hides
    }
}

void WorldSession::tradeStatus(const td::TradeStatusRead& s) {
    using game::LogicalOpcode;
    if (++tdLogs_ <= 200)
        PSP_LOGI(tag::Net, "trade_status %u result=%u", static_cast<unsigned>(s.status), static_cast<unsigned>(s.result));
    const td::TradeStatusLine line = td::tradeStatusLine(s.status, s.result, s.isTarget);
    switch (s.status) {
        case td::kTradeBegin:  // somebody asks: TRADE_REQUEST -> the TRADE popup (upstream handleTradeStatus case 1)
            tradeAsking_ = false;
            tradeRequestGuid_ = s.guid;
            tradeAskerName_ = tdNameOrQuery(s.guid);
            tradeAsked_ = true;
            tradeAskMs_ = logUptimeMs();
            ++tradeAskSeq_;
            PSP_LOGI(tag::Net, "trade_request from=%llx", static_cast<unsigned long long>(s.guid));
            break;
        case td::kTradeOpenWindow:  // TRADE_SHOW with the request's player
            tradeId_ = s.tradeId;
            tradePeer_ = tradeRequestGuid_;
            for (auto& side : tradeView_) side = {};
            tradeGold_[0] = tradeGold_[1] = 0;
            tradeSpell_[0] = tradeSpell_[1] = 0;
            tradeSelfAccepted_ = tradePeerAccepted_ = false;
            tradeUnlockAll();
            tradeWindow_ = true;
            tradeCompleted_ = false;
            tradeBindAsk_ = false;
            if (tradeAsked_) {
                tradeAsked_ = false;
                ++tradeAskSeq_;
            }
            ++tradeSeq_;
            PSP_LOGI(tag::Net, "trade_show peer=%llx id=%u", static_cast<unsigned long long>(tradePeer_),
                     static_cast<unsigned>(tradeId_));
            break;
        case td::kTradeCanceled:  // the server ended it: closed, ERR_TRADE_CANCELLED
        case td::kTradeRejected:  // upstream: closes the trade as a cancel
            tradeCloseWindow(false, false);
            tdLine(line.key, std::string());
            break;
        case td::kTradeAccept: tradePeerAccepted_ = true; ++tradeAcceptSeq_; break;
        case td::kTradeBackToTrade:  // upstream: both acceptances cleared (the server already took ours back)
            tradeSelfAccepted_ = tradePeerAccepted_ = false;
            ++tradeAcceptSeq_;
            break;
        case td::kTradeComplete:
            tradeCloseWindow(true, false);
            tdLine(line.key, std::string());
            break;
        case td::kTradeCloseWindow:
            if (line.key) tdLine(line.key, std::string());
            else if (line.genericResult) addUiError(inventoryErrorString(static_cast<uint8_t>(s.result)));
            tradeCloseWindow(false, true);
            break;
        case td::kTradeWrongRealm:
        case td::kTradeNotOnTaplist:
            tdLine(line.key, std::string());
            tradeClearLocal(s.slot);
            break;
        case td::kTradeBusy:
        case td::kTradeBusy2:
        case td::kTradeIgnoreYou:  // named by the player we asked
            tdLineNamed(line.key, tradeAsking_ ? tradeRequestGuid_ : 0);
            break;
        default:
            if (line.key) tdLine(line.key, std::string());
            break;
    }
    if (td::tradeStatusEndsRequest(s.status)) tradeForgetRequest();
}

// Our slot (or 0xFF: our gold) forgotten locally.
void WorldSession::tradeClearLocal(uint8_t slot) {
    if (slot == 0xFF) {
        tradeGold_[0] = 0;
        return;
    }
    if (slot < td::kTradeSlots) tradeMine_[slot] = TradeMine{};
}

void WorldSession::tradeExtended(const td::TradeExtendedRead& x) {
    if (!tradeWindow_) return;
    const int side = x.side ? 1 : 0;
    tradeGold_[side] = x.gold;
    tradeSpell_[side] = x.spell;
    for (int i = 0; i < td::kTradeSlots; ++i) {
        const td::TradeSlotRecord& r = x.slots[static_cast<size_t>(i)];
        const bool has = x.present[static_cast<size_t>(i)] && r.entry != 0;
        // Our own offer: a slot the server reports empty is no longer ours on the table.
        if (side == 0 && !has) tradeClearLocal(static_cast<uint8_t>(i));
        tradeView_[static_cast<size_t>(side)][static_cast<size_t>(i)] = has ? r : td::TradeSlotRecord{};
        if (has && side == 1) npcQueryItem(r.entry);  // the name / icon / quality of what is offered
    }
    ++tradeUpdateSeq_;
    if (++tdLogs_ <= 200)
        PSP_LOGI(tag::Net, "trade_update side=%d gold=%u spell=%u records=%d", side, static_cast<unsigned>(x.gold),
                 static_cast<unsigned>(x.spell), x.records);
}

// InitiateTrade: CMSG_INITIATE_TRADE for another player; the server answers every refusal with a status
// (AzerothCore HandleInitiateTradeOpcode). PSP: one request at a time (ERR_ALREADY_TRADING while one is open or
// pending), ERR_INITIATE_TRADE_S in the chat once it is sent.
void WorldSession::initiateTrade(uint64_t guid) {
    if (state_ != State::InWorld || !socket_ || guid == 0 || guid == playerGuid_) return;
    auto e = entities_.getEntity(guid);
    if (!e || e->getType() != game::ObjectType::PLAYER) return;
    const uint32_t now = logUptimeMs();
    if (tradeAsking_ && now - tradeAskMs_ > kTradeAskMs) tradeForgetRequest();  // never answered
    if (tradeWindow_ || tradeRequestGuid_ != 0) {
        tdLine("ERR_ALREADY_TRADING", std::string());
        return;
    }
    socket_->send(td::buildGuid(game::wireOpcode(game::LogicalOpcode::CMSG_INITIATE_TRADE), guid));
    tradeRequestGuid_ = guid;
    tradeAsking_ = true;
    tradeAskMs_ = now;
    tdLineNamed("ERR_INITIATE_TRADE_S", guid);
    PSP_LOGI(tag::Net, "trade_initiate guid=%llx", static_cast<unsigned long long>(guid));
}

// The TRADE popup's answers (public StaticPopup TRADE: OnAccept = BeginTrade, OnCancel = CancelTrade), as upstream's
// acceptTradeRequest / declineTradeRequest.
void WorldSession::tradeAnswerRequest(bool accept) {
    if (!tradeAsked_) return;
    tradeAsked_ = false;
    ++tradeAskSeq_;
    sendTradeEmpty(accept ? game::LogicalOpcode::CMSG_BEGIN_TRADE : game::LogicalOpcode::CMSG_CANCEL_TRADE);
    if (!accept) tradeRequestGuid_ = 0;
    PSP_LOGI(tag::Net, "trade_request_answer accept=%d", accept ? 1 : 0);
}

const std::string& WorldSession::tradeAskerName() {
    if (tradeAskerName_.empty()) tradeAskerName_ = tdNameOf(tradeRequestGuid_);
    return tradeAskerName_;
}

const td::TradeSlotRecord& WorldSession::tradeSlot(int side, int slot) const {
    static const td::TradeSlotRecord kNone{};
    if (side < 0 || side > 1 || slot < 0 || slot >= td::kTradeSlots) return kNone;
    return tradeView_[static_cast<size_t>(side)][static_cast<size_t>(slot)];
}

const WorldSession::TradeMine& WorldSession::tradeMine(int slot) const {
    static const TradeMine kNone{};
    return slot >= 0 && slot < td::kTradeSlots ? tradeMine_[static_cast<size_t>(slot)] : kNone;
}

// A bag item we put on the table is shown locked until it leaves the table (the port's rule).
bool WorldSession::tradeItemLocked(uint8_t bag, uint8_t slot) const {
    if (!tradeWindow_) return false;
    const uint64_t guid = wireItemGuid(bag, slot);
    if (guid == 0) return false;
    for (const TradeMine& m : tradeMine_)
        if (m.item == guid) return true;
    return false;
}

// GetTradePlayerItemInfo: our slot's item - at the place we put it up from, else found by its guid in the backpack
// and the worn bags.
bool WorldSession::tradeMineItem(int slot, game::ItemDef& out, uint8_t* bagOut, uint8_t* slotOut) const {
    if (slot < 0 || slot >= td::kTradeSlots) return false;
    const TradeMine& m = tradeMine_[static_cast<size_t>(slot)];
    if (m.item == 0) return false;
    auto at = [&](uint8_t bag, uint8_t s) {
        if (!readWireSlot(bag, s, out) || out.itemId == 0) return false;
        if (bagOut) *bagOut = bag;
        if (slotOut) *slotOut = s;
        return true;
    };
    if (wireItemGuid(m.bag, m.slot) == m.item) return at(m.bag, m.slot);
    for (int i = 0; i < 16; ++i)
        if (backpackSlotGuids_[static_cast<size_t>(i)] == m.item)
            return at(game::slots::kNoContainer, static_cast<uint8_t>(game::slots::kBackpackFirst + i));
    for (int b = 0; b < game::slots::kWornBagCount; ++b) {
        const uint8_t bag = static_cast<uint8_t>(game::slots::kWornBagFirst + b);
        auto it = containerContents_.find(equipSlotGuids_[bag]);
        if (it == containerContents_.end()) continue;
        for (uint32_t i = 0; i < it->second.numSlots && i < it->second.slotGuids.size(); ++i)
            if (it->second.slotGuids[i] == m.item) return at(bag, static_cast<uint8_t>(i));
    }
    return false;
}

// Before an item goes on one of the traded slots 1..6, what AzerothCore's HandleSetTradeItemOpcode would refuse
// (Item::CanBeTraded; the 7th "will not be traded" slot is exempt) is refused here with a line - the port's order:
// a bag in a bag slot (ERR_TRADE_EQUIPPED_BAG), a bag with items in it (ERR_TRADE_BAG), a soulbound item that is
// not ITEM_FIELD_FLAG_BOP_TRADEABLE (ERR_TRADE_BOUND_ITEM). nullptr = it may go.
const char* WorldSession::tradeItemRefusal(uint8_t bag, uint8_t slot, int tradeSlot, const game::ItemDef& item,
                                           uint64_t itemGuid) const {
    if (tradeSlot >= td::kTradableSlots) return nullptr;
    if (item.bagSlots > 0) {
        if (bag == game::slots::kNoContainer && slot >= game::slots::kWornBagFirst &&
            slot < game::slots::kWornBagFirst + game::slots::kWornBagCount)
            return "ERR_TRADE_EQUIPPED_BAG";
        auto it = containerContents_.find(itemGuid);
        if (it != containerContents_.end())
            for (uint32_t i = 0; i < it->second.numSlots && i < it->second.slotGuids.size(); ++i)
                if (it->second.slotGuids[i] != 0) return "ERR_TRADE_BAG";
    }
    const uint32_t flags = itemInstanceFlags(itemGuid);
    const bool soulbound = item.soulbound || (flags & kItemFlagSoulbound);
    if (soulbound && !(flags & kItemFlagBopTradeable)) return "ERR_TRADE_BOUND_ITEM";
    return nullptr;
}

// An item on the table: CMSG_SET_TRADE_ITEM, the item remembered (locked in the bags). The 7th slot raises
// TRADE_POTENTIAL_BIND_ENCHANT for an item still BOP_TRADEABLE (an enchant on it would bind it - the port's rule).
bool WorldSession::tradePlace(int tradeSlot, uint8_t bag, uint8_t slot, uint64_t itemGuid) {
    if (state_ != State::InWorld || !socket_) return false;
    socket_->send(td::buildSetTradeItem(game::wireOpcode(game::LogicalOpcode::CMSG_SET_TRADE_ITEM),
                                        static_cast<uint8_t>(tradeSlot), bag, slot));
    tradeMine_[static_cast<size_t>(tradeSlot)] = TradeMine{itemGuid, bag, slot};
    if (tradeSlot == td::kTradableSlots) {
        tradeBindAsk_ = (itemInstanceFlags(itemGuid) & kItemFlagBopTradeable) != 0;
        ++tradeBindSeq_;
    }
    ++tradeUpdateSeq_;
    PSP_LOGI(tag::Net, "trade_set slot=%d bag=%u bslot=%u", tradeSlot, static_cast<unsigned>(bag),
             static_cast<unsigned>(slot));
    return true;
}

// ClickTradeButton(i) with an item on the cursor.
bool WorldSession::tradeSetItem(int tradeSlot, uint8_t bag, uint8_t slot) {
    if (!tradeWindow_ || tradeSlot < 0 || tradeSlot >= td::kTradeSlots) return false;
    game::ItemDef item;
    const uint64_t guid = wireItemGuid(bag, slot);
    if (guid == 0 || !readWireSlot(bag, slot, item) || item.itemId == 0) return false;
    if (const char* refusal = tradeItemRefusal(bag, slot, tradeSlot, item, guid)) {
        tdLine(refusal, std::string());
        return false;
    }
    if (tradeMine_[static_cast<size_t>(tradeSlot)].item == guid) return true;  // already there
    for (int i = 0; i < td::kTradeSlots; ++i)  // the item off any other slot it was on (it moves)
        if (i != tradeSlot && tradeMine_[static_cast<size_t>(i)].item == guid) tradeMine_[static_cast<size_t>(i)] = TradeMine{};
    return tradePlace(tradeSlot, bag, slot, guid);
}

// UseContainerItem while trading: the first free slot of 1..6 (public TradeFrame.lua TradeFrame_GetAvailableSlot);
// none free: nothing. The 7th slot only takes an item put there by hand.
bool WorldSession::tradeAddItem(uint8_t bag, uint8_t slot) {
    if (!tradeWindow_) return false;
    const uint64_t guid = wireItemGuid(bag, slot);
    if (guid == 0) return false;
    for (const TradeMine& m : tradeMine_)
        if (m.item == guid) return true;  // on the table already
    for (int i = 0; i < td::kTradableSlots; ++i)
        if (tradeMine_[static_cast<size_t>(i)].item == 0) return tradeSetItem(i, bag, slot);
    return true;
}

// ClickTradeButton(i, true) / a click on a filled slot with nothing held: CMSG_CLEAR_TRADE_ITEM, the item unlocked.
void WorldSession::tradeClearItem(int tradeSlot) {
    if (!tradeWindow_ || tradeSlot < 0 || tradeSlot >= td::kTradeSlots) return;
    if (tradeMine_[static_cast<size_t>(tradeSlot)].item == 0) return;
    if (state_ == State::InWorld && socket_)
        socket_->send(td::buildClearTradeItem(game::wireOpcode(game::LogicalOpcode::CMSG_CLEAR_TRADE_ITEM),
                                              static_cast<uint8_t>(tradeSlot)));
    tradeMine_[static_cast<size_t>(tradeSlot)] = TradeMine{};
    if (tradeSlot == td::kTradableSlots) {  // TRADE_POTENTIAL_BIND_ENCHANT(false)
        tradeBindAsk_ = false;
        ++tradeBindSeq_;
    }
    ++tradeUpdateSeq_;
    PSP_LOGI(tag::Net, "trade_clear slot=%d", tradeSlot);
}

// SetTradeMoney (the money box refuses more than we have before it gets here - public TradeFrame_UpdateMoney).
bool WorldSession::tradeSetMoney(uint32_t copper) {
    if (!tradeWindow_ || state_ != State::InWorld || !socket_) return false;
    socket_->send(td::buildU32(game::wireOpcode(game::LogicalOpcode::CMSG_SET_TRADE_GOLD), copper));
    tradeGold_[0] = copper;
    ++tradeUpdateSeq_;
    PSP_LOGI(tag::Net, "trade_gold copper=%u", static_cast<unsigned>(copper));
    return true;
}

// AcceptTrade: CMSG_ACCEPT_TRADE (no body - upstream acceptTrade), TRADE_ACCEPT_UPDATE.
void WorldSession::tradeAccept() {
    if (!tradeWindow_ || tradeSelfAccepted_ || state_ != State::InWorld || !socket_) return;
    tradeSelfAccepted_ = true;
    ++tradeAcceptSeq_;
    sendTradeEmpty(game::LogicalOpcode::CMSG_ACCEPT_TRADE);
    PSP_LOGI(tag::Net, "trade_accept");
}

// CancelTradeAccept (upstream unacceptTrade).
void WorldSession::tradeCancelAccept() {
    if (!tradeWindow_ || !tradeSelfAccepted_) return;
    tradeSelfAccepted_ = false;
    ++tradeAcceptSeq_;
    sendTradeEmpty(game::LogicalOpcode::CMSG_UNACCEPT_TRADE);
    PSP_LOGI(tag::Net, "trade_unaccept");
}

// CloseTrade: TRADE_CLOSED, CMSG_CANCEL_TRADE (upstream cancelTrade; the server answers TRADE_CANCELED).
void WorldSession::tradeClose() { tradeCloseWindow(false, true); }

// --- Duels --------------------------------------------------------------------------------------------------------

// StartDuel(unit): the Duel spell on the unit.
void WorldSession::startDuel(uint64_t guid) {
    if (guid == 0 || guid == playerGuid_) return;
    auto e = entities_.getEntity(guid);
    if (!e || e->getType() != game::ObjectType::PLAYER) return;
    castSpell(kSpellDuel, guid, false);
    PSP_LOGI(tag::Net, "duel_start target=%llx", static_cast<unsigned long long>(guid));
}

void WorldSession::sendDuelAnswer(bool accept) {
    if (state_ != State::InWorld || !socket_) return;
    const auto op = accept ? game::LogicalOpcode::CMSG_DUEL_ACCEPTED : game::LogicalOpcode::CMSG_DUEL_CANCELLED;
    socket_->send(td::buildGuid(game::wireOpcode(op), duel_.arbiter));
}

void WorldSession::acceptDuel() {  // AcceptDuel
    sendDuelAnswer(true);
    duel_.requested = false;
    PSP_LOGI(tag::Net, "duel_accept");
}

void WorldSession::cancelDuel() {  // CancelDuel: declining the request, or forfeiting the duel
    sendDuelAnswer(false);
    duel_.requested = false;
    PSP_LOGI(tag::Net, "duel_cancel");
}

// The unit is our duel opponent while the duel runs (tradeduel::duelHostile on the public duel fields).
bool WorldSession::duelOpponent(uint64_t guid) const {
    if (guid == 0 || guid == playerGuid_) return false;
    const uint32_t myTeam = selfField(kFieldDuelTeam);
    if (myTeam == 0) return false;  // not in a duel (the common case: one lookup - isHostileUnit asks for every unit)
    auto e = entities_.getEntity(guid);
    if (!e || e->getType() != game::ObjectType::PLAYER) return false;
    constexpr uint16_t kArbiterHigh = kFieldDuelArbiter + 1;
    const uint64_t mine = (static_cast<uint64_t>(selfField(kArbiterHigh)) << 32) | selfField(kFieldDuelArbiter);
    const uint64_t theirs = (static_cast<uint64_t>(e->getField(kArbiterHigh)) << 32) | e->getField(kFieldDuelArbiter);
    return td::duelHostile(mine, myTeam, theirs, e->getField(kFieldDuelTeam));
}

// --- Summons ------------------------------------------------------------------------------------------------------

int WorldSession::summonTimeLeftSec() const {  // GetSummonConfirmTimeLeft (from the packet's time to answer)
    if (!summon_.pending) return 0;
    const int32_t left = static_cast<int32_t>(summon_.expiresMs - logUptimeMs());
    return left > 0 ? left / 1000 : 0;
}

const std::string& WorldSession::summonerName() {  // GetSummonConfirmSummoner
    if (summon_.name.empty()) summon_.name = tdNameOf(summon_.summoner);
    return summon_.name;
}

// CONFIRM_SUMMON's OnUpdate: ACCEPT off while UnitAffectingCombat("player") or not PlayerCanTeleport() - the latter
// is always true here (upstream lua_social_api.cpp PlayerCanTeleport).
bool WorldSession::summonCanAccept() const {
    return !(selfField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS)) & kUnitFlagInCombat);
}

void WorldSession::confirmSummon() {  // ConfirmSummon (upstream sendSummonResponse)
    if (!summon_.pending || state_ != State::InWorld || !socket_) return;
    socket_->send(td::buildSummonResponse(game::wireOpcode(game::LogicalOpcode::CMSG_SUMMON_RESPONSE),
                                          summon_.summoner, true));
    summon_.pending = false;
    PSP_LOGI(tag::Net, "summon_accept");
}

void WorldSession::cancelSummon() {  // CancelSummon
    if (!summon_.pending) return;
    summon_.pending = false;
    if (state_ == State::InWorld && socket_)
        socket_->send(td::buildSummonResponse(game::wireOpcode(game::LogicalOpcode::CMSG_SUMMON_RESPONSE),
                                              summon_.summoner, false));
    PSP_LOGI(tag::Net, "summon_decline");
}

// --- Quests: share / escort -----------------------------------------------------------------------------------------

bool WorldSession::questPushable(uint32_t questId) const {  // GetQuestLogPushable
    auto it = questShareFlags_.find(questId);
    return it != questShareFlags_.end() && (it->second & kQuestFlagSharable) != 0;
}

void WorldSession::pushQuestToParty(uint32_t questId) {  // QuestLogPushQuest
    if (state_ != State::InWorld || !socket_ || !questPushable(questId) || !isInGroup()) return;
    socket_->send(td::buildU32(game::wireOpcode(game::LogicalOpcode::CMSG_PUSHQUESTTOPARTY), questId));
    PSP_LOGI(tag::Net, "quest_push quest=%u", static_cast<unsigned>(questId));
}

// Declining a quest another player shared: MSG_QUEST_PUSH_RESULT 3 (QUEST_PARTY_MSG_DECLINE_QUEST), which
// AzerothCore's HandleQuestPushResult passes on to the sharer. The port sends it whenever the quest's giver is a player.
void WorldSession::questPushDecline() {
    if (!questDetailsOpen_ || state_ != State::InWorld || !socket_) return;
    const uint64_t giver = currentQuestDetails_.npcGuid;
    auto e = giver ? entities_.getEntity(giver) : nullptr;
    if (!e || e->getType() != game::ObjectType::PLAYER) return;
    socket_->send(td::buildQuestPushResult(game::wireOpcode(game::LogicalOpcode::MSG_QUEST_PUSH_RESULT), giver,
                                           currentQuestDetails_.questId, td::kQuestPushDecline));
    PSP_LOGI(tag::Net, "quest_push_decline quest=%u", static_cast<unsigned>(currentQuestDetails_.questId));
}

bool WorldSession::questLogFull() const { return static_cast<int>(questLog_.size()) >= kMaxQuests; }

const std::string& WorldSession::questConfirmName() {
    if (questConfirm_.name.empty()) questConfirm_.name = tdNameOf(questConfirm_.starter);
    return questConfirm_.name;
}

void WorldSession::confirmAcceptQuest() {  // ConfirmAcceptQuest
    if (questConfirm_.questId == 0 || state_ != State::InWorld || !socket_) return;
    socket_->send(td::buildU32(game::wireOpcode(game::LogicalOpcode::CMSG_QUEST_CONFIRM_ACCEPT), questConfirm_.questId));
    questConfirm_.pending = false;
    PSP_LOGI(tag::Net, "quest_confirm_accept quest=%u", static_cast<unsigned>(questConfirm_.questId));
}

void WorldSession::dismissQuestConfirm() { questConfirm_.pending = false; }

}  // namespace wowee::platform::psp
