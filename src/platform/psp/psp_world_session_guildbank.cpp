// The guild bank window's session half (the packets in psp_glyph_bank_parse.hpp; their layouts are AzerothCore's
// GuildPackets.cpp / Guild.cpp writers and readers):
//   - opening: CMSG_GUILD_BANKER_ACTIVATE; the first SMSG_GUILD_BANK_LIST after it opens the frame
//     (GUILDBANKFRAME_OPENED), every list updates the slots (GUILDBANKBAGSLOTS_CHANGED), tab 0's full list the tab
//     names / icons (GUILDBANK_UPDATE_TABS) - upstream inventory_handler.cpp handleGuildBankList. A list that comes
//     with no open request pending only updates the stored tab;
//   - every tab query asks for the full tab (upstream GuildBankerActivatePacket / queryGuildBankTab send 1);
//   - a new guild (or none) drops the old guild's bank data (the port's rule);
//   - GetGuildBankTabInfo: the rank's tab rights and stacks per day (AzerothCore GuildBankRights, the rank table
//     of the roster / MSG_GUILD_PERMISSIONS - the social code keeps it: guildRank), the last list's withdrawals left;
//   - item moves (CMSG_GUILD_BANK_SWAP_ITEMS): the entry field 0 and the split as given (upstream
//     GuildBankSwapItemsPacket; AzerothCore ignores the entry, a split of 0 moves / merges the whole stack). Refused
//     here, each with the red error line: a source that is not an inventory position (AzerothCore
//     HandleGuildBankSwapItems: Player::IsInventoryPos) and a move the rank's rights forbid (AzerothCore
//     Guild::_MoveItems: BankMoveItemData::HasStoreRights / HasWithdrawRights, not checked inside one tab). Bound,
//     conjured or timed items are left to the server (PlayerMoveItemData::InitItem, BankMoveItemData::CanStore);
//   - money: a deposit is capped at the player's money; a withdrawal is the server's to check; the money left to
//     withdraw today (MSG_GUILD_BANK_MONEY_WITHDRAWN) is asked when the frame opens and after each withdrawal;
//   - the log (MSG_GUILD_BANK_LOG_QUERY, <= 25 per tab, 6 = the money log) and the tab texts
//     (MSG_QUERY_GUILD_BANK_TEXT) are asked on every request (upstream queryGuildBankText); a text is kept up to 500
//     characters (AzerothCore MAX_GUILD_BANK_TAB_TEXT_LEN) and changes only by the server's answer;
//   - SMSG_GUILD_EVENT's bank events update only what they carry (upstream social_handler.cpp handleGuildEvent):
//     the money, a tab bought, a tab's name / icon, GUILDBANK_TEXT_CHANGED. AzerothCore sends the changed slots to
//     an open window by itself (Guild::_SendBankContentUpdate).
// PSP: the frame closes when the vault is out of reach (NPC_INTERACT_MAX_DISTANCE, as the bank / mailbox); no item
// lock is drawn while a move is out (the next list redraws the slots).
#include "platform/psp/psp_world_session.hpp"

#include "game/inventory_slots.hpp"
#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wowee::platform::psp {

namespace {

using game::LogicalOpcode;

constexpr size_t kBankTextChars = 500;  // AzerothCore Guild.cpp MAX_GUILD_BANK_TAB_TEXT_LEN

unsigned lo32gb(uint64_t g) { return static_cast<unsigned>(g & 0xFFFFFFFFu); }

}  // namespace

// Every refusal made here is the red error line (GameLine::Refused): 1 not an inventory position, 2 the rank's rights.
void WorldSession::guildBankError(uint32_t code) {
    gameLine(GameLine::Refused, cs(code == 1 ? CS::ERR_GUILD_BANK_EQUIPPED_ITEM : CS::ERR_GUILD_PERMISSIONS));
    if (++gbLogs_ <= 200) PSP_LOGI(tag::Net, "guildbank_refused code=%X", static_cast<unsigned>(code));
}

// A new guild (or none): the old guild's tabs, items, texts and logs go.
void WorldSession::guildBankReset() {
    gbTabsKnown_ = false;
    gbNumTabs_ = 0;
    gbTabs_.clear();
    gbItems_.clear();
    for (int t = 0; t < gbk::kBankTabs; ++t) gbText_[t].clear();
    for (auto& l : gbLog_) l.clear();
    gbMoney_ = 0;
    gbWithdrawMoney_ = -1;
    ++gbVersion_;
}

void WorldSession::guildBankAskWithdrawMoney() {  // MSG_GUILD_BANK_MONEY_WITHDRAWN (an empty request)
    if (!socket_ || state_ != State::InWorld || !isInGuild()) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::MSG_GUILD_BANK_MONEY_WITHDRAWN);
    if (wire != 0xFFFF) socket_->send(network::Packet(wire));
}

void WorldSession::openGuildBank(uint64_t banker) {
    if (!socket_ || state_ != State::InWorld || banker == 0) return;
    if (gbBanker_ == banker) return;  // already open at this vault
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANKER_ACTIVATE);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeBankerActivate(p, banker, true);
    socket_->send(p);
    gbPending_ = banker;
    PSP_LOGI(tag::Net, "guildbank_activate guid=%08X full=%d", lo32gb(banker), 1);
}

void WorldSession::closeGuildBank() {  // CloseGuildBankFrame
    if (gbBanker_ != 0) PSP_LOGI(tag::Net, "guildbank_close guid=%08X", lo32gb(gbBanker_));
    gbBanker_ = 0;
    gbPending_ = 0;
}

void WorldSession::queryGuildBankTab(uint8_t tab) {
    if (gbBanker_ == 0 || tab >= gbk::kBankTabs || !socket_) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_QUERY_TAB);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeBankQueryTab(p, gbBanker_, tab, true);
    socket_->send(p);
}

const gbk::BankItem* WorldSession::guildBankItem(uint8_t tab, uint8_t slot) const {
    if (tab >= gbItems_.size() || slot >= gbItems_[tab].size()) return nullptr;
    const gbk::BankItem& it = gbItems_[tab][slot];
    return it.entry != 0 ? &it : nullptr;
}

uint8_t WorldSession::guildBankNumTabs() const { return gbTabsKnown_ ? gbNumTabs_ : guildBankTabs(); }

std::string WorldSession::guildBankTabName(uint8_t tab) const {
    if (tab < gbTabs_.size() && !gbTabs_[tab].name.empty()) return gbTabs_[tab].name;
    return CLIENT_FMT(GUILDBANK_TAB_NUMBER, tab + 1);
}

std::string WorldSession::guildBankTabIcon(uint8_t tab) const {
    if (tab < gbTabs_.size() && !gbTabs_[tab].icon.empty()) return gbTabs_[tab].icon;
    return "INV_Misc_QuestionMark";
}

uint32_t WorldSession::guildBankTabFlags(uint8_t tab) const {  // the rank table's tab flags
    if (tab >= gbk::kBankTabs || !isInGuild()) return 0;
    return guildRank(guildRankIndex()).tabFlags[tab];
}

uint32_t WorldSession::guildBankTabWithdrawals(uint8_t tab) const {
    if (tab >= gbk::kBankTabs || !isInGuild()) return 0;
    return guildRank(guildRankIndex()).tabSlots[tab];
}

// CanWithdrawGuildBankMoney: money is left to withdraw today (-1 = no limit or not asked yet) - upstream
// lua_inventory_api.cpp CanWithdrawGuildBankMoney (the remaining withdraw money != 0).
bool WorldSession::canWithdrawGuildBankMoney() const { return isInGuild() && gbWithdrawMoney_ != 0; }

// CanGuildBankRepair: the rank's GR_RIGHT_WITHDRAW_REPAIR and money in the bank - upstream lua_inventory_api.cpp.
bool WorldSession::canGuildBankRepair() const { return guildCan(social::kRightRepair) && gbMoney_ != 0; }

uint32_t WorldSession::guildBankTabCost() const {
    const uint8_t n = guildBankNumTabs();
    return n < gbk::kBankTabs ? gbk::kBankTabCost[n] : 0u;
}

// AzerothCore's BankMoveItemData::HasStoreRights / HasWithdrawRights for a move between `from` and `to`
// (-1 = the player's bags); inside one tab nothing is checked. False: refused, the line shown.
bool WorldSession::guildBankMoveAllowed(int from, int to, bool swap) {
    auto canStore = [&](int tab, int other) {
        return tab < 0 || tab == other || (guildBankTabFlags(static_cast<uint8_t>(tab)) & gbk::kTabDeposit) != 0;
    };
    auto canWithdraw = [&](int tab, int other) {
        return tab < 0 || tab == other || guildBankTabWithdrawals(static_cast<uint8_t>(tab)) != 0;
    };
    if (!canStore(to, from) || !canWithdraw(from, to) || (swap && !canStore(from, to))) {
        guildBankError(2);
        return false;
    }
    return true;
}

// a bag item into a bank slot (0xFF = wherever it fits: AzerothCore BankMoveItemData::CanStore picks the slot).
bool WorldSession::guildBankDeposit(uint8_t tab, uint8_t slot, uint8_t bag, uint8_t bagSlot, uint32_t split) {
    if (gbBanker_ == 0 || !socket_ || tab >= gbk::kBankTabs) return false;
    game::ItemDef def;
    uint64_t guid = 0;
    if (!itemAtWire(bag, bagSlot, def, guid) || guid == 0) return false;
    if (bag == game::slots::kNoContainer && bagSlot < 23) {  // worn items and bags: not an inventory position
        guildBankError(1);
        return false;
    }
    const bool swap = slot < gbk::kBankSlots && guildBankItem(tab, slot) != nullptr;
    if (!guildBankMoveAllowed(-1, tab, swap)) return false;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_SWAP_ITEMS);
    if (wire == 0xFFFF) return false;
    network::Packet p(wire);
    gbk::writeBankSwapWithBag(p, gbBanker_, tab, slot, 0, bag, bagSlot, false, split);
    socket_->send(p);
    PSP_LOGI(tag::Net, "guildbank_deposit tab=%u slot=%u bag=%u bagslot=%u entry=%u count=%u", tab, slot, bag, bagSlot,
             static_cast<unsigned>(def.itemId), static_cast<unsigned>(split));
    return true;
}

// a bank item into a bag slot (the bags' put-down with a guild bank item on the cursor).
bool WorldSession::guildBankWithdraw(uint8_t tab, uint8_t slot, uint8_t bag, uint8_t bagSlot, uint32_t split) {
    if (gbBanker_ == 0 || !socket_ || tab >= gbk::kBankTabs) return false;
    const gbk::BankItem* it = guildBankItem(tab, slot);
    if (!it) return false;
    if (!guildBankMoveAllowed(tab, -1, false)) return false;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_SWAP_ITEMS);
    if (wire == 0xFFFF) return false;
    network::Packet p(wire);
    gbk::writeBankSwapWithBag(p, gbBanker_, tab, slot, 0, bag, bagSlot, true, split);
    socket_->send(p);
    PSP_LOGI(tag::Net, "guildbank_withdraw tab=%u slot=%u bag=%u bagslot=%u entry=%u count=%u", tab, slot, bag, bagSlot,
             static_cast<unsigned>(it->entry), static_cast<unsigned>(split));
    return true;
}

// a bank item onto another bank slot (the server merges a same-item stack or swaps).
bool WorldSession::guildBankMove(uint8_t tab, uint8_t slot, uint8_t srcTab, uint8_t srcSlot, uint32_t split) {
    if (gbBanker_ == 0 || !socket_ || tab >= gbk::kBankTabs || srcTab >= gbk::kBankTabs) return false;
    const gbk::BankItem* src = guildBankItem(srcTab, srcSlot);
    if (!src) return false;
    if (!guildBankMoveAllowed(srcTab, tab, guildBankItem(tab, slot) != nullptr)) return false;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_SWAP_ITEMS);
    if (wire == 0xFFFF) return false;
    network::Packet p(wire);
    gbk::writeBankSwapBankBank(p, gbBanker_, tab, slot, 0, srcTab, srcSlot, 0, split);
    socket_->send(p);
    PSP_LOGI(tag::Net, "guildbank_move tab=%u slot=%u from=%u/%u entry=%u count=%u", tab, slot, srcTab, srcSlot,
             static_cast<unsigned>(src->entry), static_cast<unsigned>(split));
    return true;
}

bool WorldSession::guildBankAutoStore(uint8_t tab, uint8_t slot) {  // the right click on a bank item
    if (gbBanker_ == 0 || !socket_ || tab >= gbk::kBankTabs) return false;
    const gbk::BankItem* it = guildBankItem(tab, slot);
    if (!it) return false;
    if (!guildBankMoveAllowed(tab, -1, false)) return false;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_SWAP_ITEMS);
    if (wire == 0xFFFF) return false;
    network::Packet p(wire);
    gbk::writeBankAutoStore(p, gbBanker_, tab, slot, 0, 0);  // AzerothCore reads no count for the auto store
    socket_->send(p);
    PSP_LOGI(tag::Net, "guildbank_autostore tab=%u slot=%u entry=%u", tab, slot, static_cast<unsigned>(it->entry));
    return true;
}

void WorldSession::guildBankDepositMoney(uint32_t copper) {  // no more than the player has
    if (gbBanker_ == 0 || !socket_) return;
    copper = static_cast<uint32_t>(std::min<uint64_t>(copper, getMoneyCopper()));
    if (copper == 0) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_DEPOSIT_MONEY);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeBankMoney(p, gbBanker_, copper);
    socket_->send(p);
    PSP_LOGI(tag::Net, "guildbank_money_deposit copper=%u", static_cast<unsigned>(copper));
}

void WorldSession::guildBankWithdrawMoney(uint32_t copper) {
    if (gbBanker_ == 0 || !socket_ || copper == 0) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_WITHDRAW_MONEY);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeBankMoney(p, gbBanker_, copper);
    socket_->send(p);
    guildBankAskWithdrawMoney();  // the money left today, after the server took this one
    PSP_LOGI(tag::Net, "guildbank_money_withdraw copper=%u", static_cast<unsigned>(copper));
}

// BuyGuildBankTab: the guild leader, and the player's and the bank's money cover the price (the frame's own
// condition: the guild bank UI add-on's GetMoney() + GetGuildBankMoney() >= tabCost); otherwise nothing is sent.
void WorldSession::guildBankBuyTab() {
    if (gbBanker_ == 0 || !socket_ || !isGuildLeader()) return;
    const uint8_t n = guildBankNumTabs();
    if (n >= gbk::kBankTabs || getMoneyCopper() + gbMoney_ < gbk::kBankTabCost[n]) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_BUY_TAB);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeBankBuyTab(p, gbBanker_, n);
    socket_->send(p);
    PSP_LOGI(tag::Net, "guildbank_buy_tab tab=%u cost=%u", n, static_cast<unsigned>(gbk::kBankTabCost[n]));
}

void WorldSession::guildBankSetTabInfo(uint8_t tab, const std::string& name, const std::string& icon) {
    if (gbBanker_ == 0 || !socket_ || tab >= guildBankNumTabs()) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_GUILD_BANK_UPDATE_TAB);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeBankUpdateTab(p, gbBanker_, tab, name, icon);
    socket_->send(p);
    PSP_LOGI(tag::Net, "guildbank_tab_info tab=%u icon=%s", tab, icon.c_str());
}

void WorldSession::guildBankQueryLog(uint8_t tab) {
    if (gbBanker_ == 0 || !socket_ || tab > gbk::kBankMoneyTab) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::MSG_GUILD_BANK_LOG_QUERY);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    p.writeUInt8(tab);
    socket_->send(p);
}

const std::vector<gbk::BankLogEntry>& WorldSession::guildBankLog(uint8_t tab) const {
    static const std::vector<gbk::BankLogEntry> kNone;
    return tab <= gbk::kBankMoneyTab ? gbLog_[tab] : kNone;
}

void WorldSession::guildBankQueryText(uint8_t tab) {  // asked every time (upstream queryGuildBankText)
    if (tab >= gbk::kBankTabs || !socket_ || state_ != State::InWorld) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::MSG_QUERY_GUILD_BANK_TEXT);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    p.writeUInt8(tab);
    socket_->send(p);
}

const std::string& WorldSession::guildBankText(uint8_t tab) const {
    static const std::string kNone;
    return tab < gbk::kBankTabs ? gbText_[tab] : kNone;
}

// SetGuildBankText: the server stores it and tells the guild (GUILDBANK_TEXT_CHANGED); the text shown changes
// with the answer to the query that event asks.
void WorldSession::guildBankSetText(uint8_t tab, const std::string& text) {
    if (gbBanker_ == 0 || !socket_ || tab >= guildBankNumTabs()) return;
    const uint16_t wire = game::wireOpcode(LogicalOpcode::CMSG_SET_GUILD_BANK_TEXT);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    gbk::writeBankSetText(p, tab, text);
    socket_->send(p);
    PSP_LOGI(tag::Net, "guildbank_set_text tab=%u len=%u", tab, static_cast<unsigned>(text.size()));
}

std::string WorldSession::guildBankName(uint64_t guid) {
    if (guid == 0) return std::string();
    if (const std::string* n = nameOf(guid)) return *n;
    askName(guid);
    return std::string();
}

void WorldSession::guildBankUpdate(const glm::vec3& selfPos) {
    // A new guild (or none): the bank's state is the old guild's.
    const uint32_t gid = guildId();
    if (gid != gbGuildSeen_) {
        gbGuildSeen_ = gid;
        if (gbBanker_ != 0) closeGuildBank();
        guildBankReset();
    }
    if (gbBanker_ == 0) return;
    const GameObjectInfo* g = findGameObject(gbBanker_);
    float dist = 0.0f;
    if (g) {
        const float dx = selfPos.x - g->pos.x, dy = selfPos.y - g->pos.y;
        dist = std::sqrt(dx * dx + dy * dy);
    }
    if (!g || dist > game::NPC_INTERACT_MAX_DISTANCE) {
        PSP_LOGI(tag::Net, "guildbank_far dist=%.1f gone=%d", static_cast<double>(dist), g ? 0 : 1);
        closeGuildBank();
    }
}

bool WorldSession::handleGuildBankPacket(LogicalOpcode op, network::Packet& packet) {
    switch (op) {
        case LogicalOpcode::SMSG_GUILD_BANK_LIST: {
            gbk::BankList l;
            if (!gbk::readBankList(packet, l)) return true;
            gbMoney_ = l.money;
            gbWithdrawalsLeft_ = l.withdrawalsLeft;
            if (l.tab >= gbk::kBankTabs) return true;
            if (gbItems_.size() < gbk::kBankTabs) gbItems_.resize(gbk::kBankTabs);
            auto& slots = gbItems_[l.tab];
            if (slots.size() != static_cast<size_t>(gbk::kBankSlots)) slots.assign(gbk::kBankSlots, gbk::BankItem{});
            if (l.hasTabs) {  // GUILDBANK_UPDATE_TABS
                gbTabs_ = l.tabs;
                gbNumTabs_ = static_cast<uint8_t>(std::min<size_t>(l.tabs.size(), gbk::kBankTabs));
                gbTabsKnown_ = true;
            }
            for (const auto& [slot, it] : l.items) {
                slots[slot] = it;
                if (it.entry != 0 && !getItemInfo(it.entry)) queryItemInfo(it.entry, 0);  // the item cache
            }
            if (gbBanker_ == 0 && gbPending_ != 0) {  // GUILDBANKFRAME_OPENED
                gbBanker_ = gbPending_;
                gbPending_ = 0;
                gbTab_ = 0;
                ++gbOpenSeq_;
                guildBankAskWithdrawMoney();
                PSP_LOGI(tag::Net, "guildbank_open guid=%08X tabs=%u money=%llu", lo32gb(gbBanker_),
                         static_cast<unsigned>(guildBankNumTabs()), static_cast<unsigned long long>(gbMoney_));
            }
            ++gbVersion_;  // GUILDBANKBAGSLOTS_CHANGED
            if (++gbLogs_ <= 200)
                PSP_LOGI(tag::Net, "guildbank_list tab=%u full=%d items=%u left=%u", l.tab, l.full ? 1 : 0,
                         static_cast<unsigned>(l.items.size()), static_cast<unsigned>(l.withdrawalsLeft));
            return true;
        }
        case LogicalOpcode::MSG_GUILD_BANK_LOG_QUERY: {
            uint8_t tab = 0;
            std::vector<gbk::BankLogEntry> log;
            if (!gbk::readBankLog(packet, tab, log)) return true;
            if (tab > gbk::kBankMoneyTab) return true;
            for (const auto& e : log) askName(e.guid);
            for (const auto& e : log)
                if (e.entry != 0 && !getItemInfo(e.entry)) queryItemInfo(e.entry, 0);
            gbLog_[tab] = std::move(log);
            gbLogAtMs_[tab] = logUptimeMs();
            ++gbLogVersion_;  // GUILDBANKLOG_UPDATE
            PSP_LOGI(tag::Net, "guildbank_log tab=%u n=%u", tab, static_cast<unsigned>(gbLog_[tab].size()));
            return true;
        }
        case LogicalOpcode::MSG_GUILD_BANK_MONEY_WITHDRAWN: {
            if (!packet.hasRemaining(4)) return true;
            gbWithdrawMoney_ = static_cast<int32_t>(packet.readUInt32());
            ++gbVersion_;  // GUILDBANK_UPDATE_WITHDRAWMONEY
            return true;
        }
        case LogicalOpcode::MSG_QUERY_GUILD_BANK_TEXT: {
            if (!packet.hasRemaining(1)) return true;
            const uint8_t tab = packet.readUInt8();
            const std::string text = packet.readString();
            if (tab < gbk::kBankTabs) gbText_[tab] = gbk::capUtf8(text, kBankTextChars);
            ++gbTextSeq_;  // GUILDBANK_UPDATE_TEXT
            return true;
        }
        case LogicalOpcode::SMSG_GUILD_EVENT: {  // peek: the bank's events; social takes the packet on
            network::Packet copy = packet;
            social::GuildEvent ev;
            if (!social::readGuildEvent(copy, ev)) return false;
            switch (ev.type) {
                case social::kGeBankBagSlots: ++gbVersion_; break;  // GUILDBANKBAGSLOTS_CHANGED
                case social::kGeBankTabPurchased:
                    if (gbTabsKnown_ && gbNumTabs_ < gbk::kBankTabs) ++gbNumTabs_;
                    if (gbTabs_.size() < gbNumTabs_) gbTabs_.resize(gbNumTabs_);
                    ++gbVersion_;  // GUILDBANK_UPDATE_TABS
                    break;
                case social::kGeBankTabUpdated: {  // tab, name, icon (AzerothCore Guild::HandleSetBankTabInfo)
                    const int t = std::atoi(ev.strings[0].c_str());
                    if (t >= 0 && t < gbk::kBankTabs) {
                        if (gbTabs_.size() <= static_cast<size_t>(t)) gbTabs_.resize(static_cast<size_t>(t) + 1);
                        gbTabs_[t].name = gbk::capUtf8(ev.strings[1], 16);
                        gbTabs_[t].icon = ev.strings[2];
                        ++gbVersion_;
                    }
                    break;
                }
                case social::kGeBankMoneySet:     // the money as hex digits (AzerothCore ByteArrayToHexStr)
                case social::kGeBankTabAndMoney:  // upstream: both set the bank's money from the first string
                    if (!ev.strings[0].empty()) gbMoney_ = std::strtoull(ev.strings[0].c_str(), nullptr, 16);
                    ++gbVersion_;  // GUILDBANK_UPDATE_MONEY
                    break;
                case social::kGeBankTextChanged: {  // GUILDBANK_TEXT_CHANGED(tab + 1)
                    const long t = std::atol(ev.strings[0].c_str());
                    if (t >= 0 && t < gbk::kBankTabs) {
                        gbTextChangedTab_ = static_cast<uint8_t>(t);
                        ++gbTextChangedSeq_;
                    }
                    break;
                }
                default: break;
            }
            return false;
        }
        case LogicalOpcode::SMSG_LOGIN_VERIFY_WORLD:  // peek: a new world - the frame closes (the vault is gone)
            if (gbBanker_ != 0 || gbPending_ != 0) closeGuildBank();
            return false;
        default: return false;
    }
}

}  // namespace wowee::platform::psp
