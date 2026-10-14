// the auction house for the PSP world session, without GameHandler. The packets are psp_auction_proto.hpp's and
// upstream's builders (world_packets_economy.cpp: the hello, the list query, the bid, the cancel, the owner and
// bidder lists). The rules and where they come from:
//   - opening (MSG_AUCTION_HELLO, AzerothCore SendAuctionHello): "enabled" 0 -> AUCTION_HOUSE_DISABLED, else
//     AUCTION_HOUSE_SHOW (upstream openAuctionHouse). Own rule: a newly opened house starts empty - the lists, their
//     totals and throttles and the sell slot cleared;
//   - closing: CloseAuctionHouse, or past NPC_INTERACT_MAX_DISTANCE from the auctioneer (upstream closeIfTooFar's
//     rule for the other NPC windows); a multi-stack sale stops with it - AzerothCore takes no sale without the
//     auctioneer in reach;
//   - the queries (upstream auctionSearch / handleAuctionListResult): a list's query holds the next one of that list
//     for 5 s, or until its answer and then the server's delay (AUCTION_SEARCH_DELAY). Own: the owner and bidder
//     lists use the same rule, each for itself, and a query asked for while its throttle runs goes once it allows;
//   - a list is announced as soon as it arrives and its unknown items are asked for (upstream); the owner and
//     bidder lists are sorted by their tables when they arrive and on SortAuctionApplySort (own, with upstream's
//     comparator); the browse list keeps the server's order (its sort block goes with the query);
//   - after a result (own): a sale -> the owner list asked again; a bid -> the browse list (upstream) and the bidder
//     list asked again; a cancel -> the entry leaves the lists and the owner list is asked again;
//   - the notifications (AzerothCore SendAuctionBidderNotification / SendAuctionOwnerNotification): a system chat
//     line with the item's name at once ("Item #n" while the item is unknown, as upstream), the list concerned asked
//     again. The auctions the outbid notices name go with the bidder query (AzerothCore HandleAuctionListBidderItems
//     lists them back, so the Bids tab can show them outbid) until the player bids on them again or wins them;
//   - selling: one CMSG_AUCTION_SELL_ITEM per stack, the next one after each answer (own); a stack bigger than the
//     sell item is made of several bag items of that entry (AzerothCore HandleAuctionSellItem takes them);
//   - the lines: GlobalStrings keys shown by the port's rule (psp_game_line.hpp: refusals red, news in the chat).
// PSP: item names without the random suffix (no ItemRandomProperties on the card, as the tooltips); the lists are
// bounded (kMaxBrowse / kMaxOwnerBidder).
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/inventory_slots.hpp"
#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wowee::platform::psp {

using game::LogicalOpcode;
namespace a = auction;

namespace {

constexpr size_t kMaxEvents = 32;
constexpr size_t kMaxBrowse = 64;        // the server sends 50 a page (NUM_AUCTION_ITEMS_PER_PAGE)
constexpr size_t kMaxOwnerBidder = 200;  // PSP: the owner / bidder lists are whole lists - bounded (~13 KB each)
constexpr size_t kMaxOutbid = 64;        // PSP: the outbid auctions remembered
constexpr uint32_t kQueryWaitMs = 5000;  // upstream auctionSearch: the wait until the server's figure arrives

// The GlobalStrings key of each line of the auction code.
CS lineText(a::Line l) {
    switch (l) {
        case a::kLineItemNotFound: return CS::ERR_ITEM_NOT_FOUND;
        case a::kLineNotEnoughMoney: return CS::ERR_NOT_ENOUGH_MONEY;
        case a::kLineBoundItem: return CS::ERR_AUCTION_BOUND_ITEM;
        case a::kLineConjuredItem: return CS::ERR_AUCTION_CONJURED_ITEM;
        case a::kLineBag: return CS::ERR_AUCTION_BAG;
        case a::kLineBidOwn: return CS::ERR_AUCTION_BID_OWN;
        case a::kLineBidIncrement: return CS::ERR_AUCTION_BID_INCREMENT;
        case a::kLineHigherBid: return CS::ERR_AUCTION_HIGHER_BID;
        case a::kLineMinBid: return CS::ERR_AUCTION_MIN_BID;
        case a::kLineRestricted: return CS::ERR_RESTRICTED_ACCOUNT;
        case a::kLineEnoughItems: return CS::ERR_AUCTION_ENOUGH_ITEMS;
        case a::kLineStarted: return CS::ERR_AUCTION_STARTED;
        case a::kLineRemoved: return CS::ERR_AUCTION_REMOVED;
        case a::kLineBidPlaced: return CS::ERR_AUCTION_BID_PLACED;
        case a::kLineOutbidS: return CS::ERR_AUCTION_OUTBID_S;
        case a::kLineWonS: return CS::ERR_AUCTION_WON_S;
        case a::kLineSoldS: return CS::ERR_AUCTION_SOLD_S;
        case a::kLineExpiredS: return CS::ERR_AUCTION_EXPIRED_S;
        case a::kLineRemovedS: return CS::ERR_AUCTION_REMOVED_S;
        default: return CS::ERR_AUCTION_DATABASE_ERROR;
    }
}

const char* listName(int list) { return list == a::kList ? "list" : list == a::kBidder ? "bidder" : "owner"; }

const char* listEvent(int list) {
    return list == a::kList ? "AUCTION_ITEM_LIST_UPDATE"
                            : list == a::kOwner ? "AUCTION_OWNED_LIST_UPDATE" : "AUCTION_BIDDER_LIST_UPDATE";
}

uint32_t notZero(uint32_t ms) { return ms == 0 ? 1u : ms; }

}  // namespace

// --- Dispatch ----------------------------------------------------------------------------------------------------

bool WorldSession::handleAuctionPacket(LogicalOpcode op, network::Packet& packet) {
    switch (op) {
        case LogicalOpcode::MSG_AUCTION_HELLO: {
            a::Hello h;
            if (!a::readHello(packet, h)) return true;
            PSP_LOGI(tag::Net, "auction_hello guid=%08X house=%u enabled=%d", static_cast<unsigned>(h.guid & 0xFFFFFFFFu),
                     static_cast<unsigned>(h.houseId), h.enabled ? 1 : 0);
            if (!h.enabled) {
                pushAuctionEvent("AUCTION_HOUSE_DISABLED");
                return true;
            }
            auctioneerGuid_ = h.guid;
            auctionHouseId_ = h.houseId;
            for (int l = 0; l < a::kListCount; ++l) {
                auctionLists_[l].clear();
                auctionTotals_[l] = 0;
                auctionNextMs_[l] = 0;
                auctionWant_[l] = false;
                auctionOut_[l] = false;
            }
            auctionHasQuery_ = false;
            clearAuctionSellItem();
            ++auctionVersion_;
            pushAuctionEvent("AUCTION_HOUSE_SHOW");
            return true;
        }
        case LogicalOpcode::SMSG_AUCTION_LIST_RESULT: auctionListResult(a::kList, packet); return true;
        case LogicalOpcode::SMSG_AUCTION_OWNER_LIST_RESULT: auctionListResult(a::kOwner, packet); return true;
        case LogicalOpcode::SMSG_AUCTION_BIDDER_LIST_RESULT: auctionListResult(a::kBidder, packet); return true;
        case LogicalOpcode::SMSG_AUCTION_LIST_PENDING_SALES: {
            // Never asked for (upstream sends no CMSG_AUCTION_LIST_PENDING_SALES; AzerothCore answers it with none).
            const uint32_t n = packet.hasRemaining(4) ? packet.readUInt32() : 0;
            packet.skipAll();
            if (++auctionLogs_ <= 120) PSP_LOGI(tag::Net, "auction_pending sales=%u", static_cast<unsigned>(n));
            return true;
        }
        case LogicalOpcode::SMSG_AUCTION_COMMAND_RESULT: auctionCommandResult(packet); return true;
        case LogicalOpcode::SMSG_AUCTION_BIDDER_NOTIFICATION: auctionBidderNotification(packet); return true;
        case LogicalOpcode::SMSG_AUCTION_OWNER_NOTIFICATION: auctionOwnerNotification(packet); return true;
        case LogicalOpcode::SMSG_AUCTION_REMOVED_NOTIFICATION: auctionRemovedNotification(packet); return true;
        default: return false;
    }
}

void WorldSession::pushAuctionEvent(const char* name, int32_t argA, int32_t argB) {
    auctionEvents_.push_back(AuctionEvent{name, argA, argB});
    while (auctionEvents_.size() > kMaxEvents) auctionEvents_.pop_front();
}

bool WorldSession::takeAuctionEvent(AuctionEvent& out) {
    if (auctionEvents_.empty()) return false;
    out = auctionEvents_.front();
    auctionEvents_.pop_front();
    return true;
}

// --- Open / close ------------------------------------------------------------------------------------------------

void WorldSession::auctionHello(uint64_t npcGuid) {
    if (state_ != State::InWorld || !socket_ || npcGuid == 0) return;
    socket_->send(game::AuctionHelloPacket::build(npcGuid));
    PSP_LOGI(tag::Net, "auction_hello_send guid=%08X", static_cast<unsigned>(npcGuid & 0xFFFFFFFFu));
}

void WorldSession::closeAuctionHouse() {
    if (auctioneerGuid_ == 0) return;
    pushAuctionEvent("AUCTION_HOUSE_CLOSED");
    if (auctionMulti_.active) auctionMultisellStop(auctionMulti_.stacks > 1);
    auctioneerGuid_ = 0;
    auctionHouseId_ = 0;
    for (int l = 0; l < a::kListCount; ++l) {
        std::vector<AuctionItem>().swap(auctionLists_[l]);  // freed while the house is closed
        auctionWant_[l] = false;
    }
    clearAuctionSellItem();
    ++auctionVersion_;
    PSP_LOGI(tag::Net, "auction_close");
}

void WorldSession::auctionUpdate(const glm::vec3& selfPos) {
    if (auctioneerGuid_ != 0) {
        if (auto npc = entities_.getEntity(auctioneerGuid_)) {
            const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(npc->getX(), npc->getY(), npc->getZ()));
            const float dx = selfPos.x - p.x, dy = selfPos.y - p.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > game::NPC_INTERACT_MAX_DISTANCE) {
                PSP_LOGI(tag::Net, "auction_far dist=%.1f", d);
                closeAuctionHouse();
            }
        }
    }
    // A query whose 5 s ran out without an answer no longer counts as out.
    for (int l = 0; l < a::kListCount; ++l)
        if (auctionOut_[l] && canSendAuctionQuery(l)) {
            PSP_LOGW(tag::Net, "auction_query_lost type=%s", listName(l));
            auctionOut_[l] = false;
        }
    auctionSendWanted();
}

// --- Queries -----------------------------------------------------------------------------------------------------

bool WorldSession::canSendAuctionQuery(int list) const {
    if (list < 0 || list >= a::kListCount) return false;
    return auctionNextMs_[list] == 0 || static_cast<int32_t>(logUptimeMs() - auctionNextMs_[list]) >= 0;
}

// The getAll sweep: never offered (upstream CanSendAuctionQuery answers no for it).
bool WorldSession::canSendAuctionMassQuery() const { return false; }

void WorldSession::queryAuctionItems(const std::string& name, uint8_t minLevel, uint8_t maxLevel, uint32_t invType,
                                     uint32_t itemClass, uint32_t subClass, int page, bool usable, uint32_t quality,
                                     bool getAll) {
    (void)getAll;  // upstream's builder writes getAll 0
    if (state_ != State::InWorld || !socket_ || auctioneerGuid_ == 0) return;
    AuctionQuery& q = auctionLastQuery_;
    q.name = name;
    q.minLevel = minLevel;
    q.maxLevel = maxLevel;
    q.invType = invType;
    q.itemClass = itemClass;
    q.subClass = subClass;
    q.page = std::max(0, page);
    q.usable = usable;
    q.quality = quality;
    auctionHasQuery_ = true;
    auctionWant_[a::kList] = true;
    auctionSendWanted();
}

void WorldSession::getOwnerAuctionItems() {
    if (state_ != State::InWorld || !socket_ || auctioneerGuid_ == 0) return;
    auctionWant_[a::kOwner] = true;
    auctionSendWanted();
}

void WorldSession::getBidderAuctionItems(int page) {
    (void)page;  // AzerothCore: "this list not have page control in client" - the whole list comes
    if (state_ != State::InWorld || !socket_ || auctioneerGuid_ == 0) return;
    auctionWant_[a::kBidder] = true;
    auctionSendWanted();
}

void WorldSession::auctionSendWanted() {
    if (state_ != State::InWorld || !socket_ || auctioneerGuid_ == 0) return;
    for (int l = 0; l < a::kListCount; ++l) {
        if (!auctionWant_[l] || !canSendAuctionQuery(l)) continue;
        if (l == a::kList && !auctionHasQuery_) {
            auctionWant_[l] = false;
            continue;
        }
        auctionWant_[l] = false;
        if (l == a::kList) {
            // The sort block: the list table's used keys, the most significant first.
            std::vector<game::AuctionSortKey> sort;
            for (const auto& k : auctionSorts_[a::kList].keys)
                if (k.used) sort.push_back(game::AuctionSortKey{k.column, k.reverse});
            const AuctionQuery& q = auctionLastQuery_;
            socket_->send(game::AuctionListItemsPacket::build(auctioneerGuid_, static_cast<uint32_t>(q.page * 50), q.name,
                                                              q.minLevel, q.maxLevel, q.invType, q.itemClass,
                                                              q.subClass, q.quality, q.usable ? 1 : 0, 0, sort));
            if (++auctionLogs_ <= 120)
                PSP_LOGI(tag::Net, "auction_query name_len=%u lv=%u-%u inv=%d class=%d sub=%d page=%d usable=%d q=%d sort=%u",
                         static_cast<unsigned>(q.name.size()), static_cast<unsigned>(q.minLevel),
                         static_cast<unsigned>(q.maxLevel), static_cast<int>(q.invType), static_cast<int>(q.itemClass),
                         static_cast<int>(q.subClass), q.page, q.usable ? 1 : 0, static_cast<int>(q.quality),
                         static_cast<unsigned>(sort.size()));
        } else if (l == a::kOwner) {
            socket_->send(game::AuctionListOwnerItemsPacket::build(auctioneerGuid_, 0));
            if (++auctionLogs_ <= 120) PSP_LOGI(tag::Net, "auction_owner_query");
        } else {
            socket_->send(game::AuctionListBidderItemsPacket::build(auctioneerGuid_, 0, auctionOutbid_));
            if (++auctionLogs_ <= 120)
                PSP_LOGI(tag::Net, "auction_bidder_query offset=0 outbid=%u", static_cast<unsigned>(auctionOutbid_.size()));
        }
        auctionOut_[l] = true;
        auctionNextMs_[l] = notZero(logUptimeMs() + kQueryWaitMs);
    }
}

// --- The lists -----------------------------------------------------------------------------------------------------

void WorldSession::auctionListResult(int list, network::Packet& packet) {
    std::vector<AuctionItem> fresh;
    a::ListTail tail;
    const bool ok = a::readList(packet, fresh, list == a::kList ? kMaxBrowse : kMaxOwnerBidder, logUptimeMs(),
                                auctionHouseId_, tail);
    packet.skipAll();
    auctionOut_[list] = false;
    if (tail.hasDelay) auctionNextMs_[list] = notZero(logUptimeMs() + tail.delayMs);
    if (!ok) {
        PSP_LOGW(tag::Net, "auction_list_bad list=%s", listName(list));
        return;
    }
    auctionLists_[list].swap(fresh);
    auctionTotals_[list] = tail.total;
    ++auctionVersion_;
    if (++auctionLogs_ <= 120)
        PSP_LOGI(tag::Net, "auction_list type=%s count=%u kept=%u total=%u delay=%u", listName(list),
                 static_cast<unsigned>(tail.count), static_cast<unsigned>(auctionLists_[list].size()),
                 static_cast<unsigned>(tail.total), static_cast<unsigned>(tail.delayMs));
    for (const AuctionItem& e : auctionLists_[list]) ensureItemInfo(e.itemEntry);
    auctionSortList(list);
    auctionAnnounce(list);
}

void WorldSession::auctionAnnounce(int list) { pushAuctionEvent(listEvent(list)); }

// The owner and bidder lists by their tables (the browse list is in the server's order).
void WorldSession::auctionSortList(int list) {
    if (list == a::kList) return;
    auto facts = [this](uint32_t entry) {
        a::ItemFacts f;
        const auto* info = getItemInfo(entry);
        if (!info || !info->valid) {
            f.name = psf(PS::ITEM_NUMBER, entry);
            return f;
        }
        f.known = true;
        f.level = static_cast<int>(info->requiredLevel);
        f.quality = info->quality;
        f.name = info->name;
        return f;
    };
    const a::SortTable& t = auctionSorts_[list];
    std::stable_sort(auctionLists_[list].begin(), auctionLists_[list].end(),
                     [&](const AuctionItem& x, const AuctionItem& y) { return a::itemLess(x, y, t, facts); });
}

void WorldSession::sortAuctionApplySort(int list) {
    if (list < 0 || list >= a::kListCount) return;
    auctionSortList(list);
    ++auctionVersion_;
    auctionAnnounce(list);
}

// An auction leaves the browse, owner and bidder lists (their totals one less).
void WorldSession::auctionRemove(uint32_t auctionId) {
    for (int l = 0; l < a::kListCount; ++l) {
        auto& v = auctionLists_[l];
        auto it = std::find_if(v.begin(), v.end(), [auctionId](const AuctionItem& e) { return e.auctionId == auctionId; });
        if (it == v.end()) continue;
        v.erase(it);
        if (auctionTotals_[l] > 0) --auctionTotals_[l];
        ++auctionVersion_;
        auctionAnnounce(l);
    }
}

const std::string* WorldSession::auctionNameOf(uint64_t guid) {
    if (guid == 0) return nullptr;
    if (const std::string* n = nameOf(guid)) return n;
    if (socket_ && nameQueried_.insert(guid).second) socket_->send(game::NameQueryPacket::build(guid));
    return nullptr;
}

// --- Bids and cancels --------------------------------------------------------------------------------------------

void WorldSession::placeAuctionBid(int list, int index, uint32_t bid) {
    if (state_ != State::InWorld || !socket_ || auctioneerGuid_ == 0) return;
    if (list < 0 || list >= a::kListCount || index < 0 || index >= static_cast<int>(auctionLists_[list].size())) return;
    const AuctionItem e = auctionLists_[list][static_cast<size_t>(index)];
    const a::Line refusal = a::checkBid(e, bid, getMoneyCopper());
    if (refusal != a::kLineNone) {
        auctionGameError(refusal);
        return;
    }
    socket_->send(game::AuctionPlaceBidPacket::build(auctioneerGuid_, e.auctionId, bid));
    PSP_LOGI(tag::Net, "auction_bid list=%s id=%u bid=%u buyout=%u", listName(list), static_cast<unsigned>(e.auctionId),
             static_cast<unsigned>(bid), static_cast<unsigned>(e.buyout));
}

bool WorldSession::canCancelAuction(int index) const {  // the add-on's cancel button: an entry not sold
    const auto& v = auctionLists_[a::kOwner];
    return auctioneerGuid_ != 0 && index >= 0 && index < static_cast<int>(v.size()) &&
           v[static_cast<size_t>(index)].saleStatus == 0;
}

void WorldSession::cancelAuction(int index) {
    if (state_ != State::InWorld || !socket_ || !canCancelAuction(index)) return;
    const AuctionItem& e = auctionLists_[a::kOwner][static_cast<size_t>(index)];
    // The fee the cancel popup shows: floor(bid x 0.05) (the auction add-on's Lua). Own rule: refused here when the
    // player has less (AzerothCore drops the cancel without an answer then).
    if (e.bid != 0 && getMoneyCopper() < e.bid / 20u) {
        auctionGameError(a::kLineNotEnoughMoney);
        return;
    }
    socket_->send(game::AuctionRemoveItemPacket::build(auctioneerGuid_, e.auctionId));
    PSP_LOGI(tag::Net, "auction_cancel id=%u bid=%u", static_cast<unsigned>(e.auctionId), static_cast<unsigned>(e.bid));
}

// --- The sell slot -----------------------------------------------------------------------------------------------

// What the PSP knows of AzerothCore's sale checks (HandleAuctionSellItem / Item::CanBeTraded), each refused here with
// the GlobalStrings key named by the reason (own rule; the server answers ERR_AUCTION_DATABASE_ERROR for all of
// them): a bag in a bag slot -> ERR_AUCTION_BAG, a soulbound item -> ERR_AUCTION_BOUND_ITEM, a conjured one ->
// ERR_AUCTION_CONJURED_ITEM, nothing there -> ERR_ITEM_NOT_FOUND. The rest (an item with a duration, ...) the
// server refuses.
a::Line WorldSession::canAuctionItem(uint8_t bag, uint8_t slot) const {
    game::ItemDef item;
    if (!readWireSlot(bag, slot, item) || item.itemId == 0) {
        if (bag == game::slots::kNoContainer && slot >= game::slots::kWornBagFirst &&
            slot < game::slots::kWornBagFirst + game::slots::kWornBagCount &&
            !getInventory()
                 .getEquipSlot(static_cast<game::EquipSlot>(game::Inventory::FIRST_BAG_EQUIP_SLOT + slot -
                                                            game::slots::kWornBagFirst))
                 .empty())
            return a::kLineBag;
        return a::kLineItemNotFound;
    }
    if (item.soulbound) return a::kLineBoundItem;
    const auto* info = getItemInfo(item.itemId);
    if (info && info->valid && (info->itemFlags & 0x2u)) return a::kLineConjuredItem;  // ITEM_FLAG_CONJURED
    return a::kLineNone;
}

void WorldSession::setAuctionSellItem(uint8_t bag, uint8_t slot) {
    game::ItemDef item;
    uint64_t guid = 0;
    if (readWireSlot(bag, slot, item) && item.itemId != 0) {
        if (bag == game::slots::kNoContainer) guid = getBackpackItemGuid(slot - game::slots::kBackpackFirst);
        else guid = getBagItemGuid(bag - game::slots::kWornBagFirst, slot);
    }
    auctionSellGuid_ = guid;
    auctionSellBag_ = bag;
    auctionSellSlot_ = slot;
    pushAuctionEvent("NEW_AUCTION_UPDATE");
    PSP_LOGI(tag::Net, "auction_sell_slot bag=%u slot=%u entry=%u count=%u", static_cast<unsigned>(bag),
             static_cast<unsigned>(slot), static_cast<unsigned>(item.itemId), static_cast<unsigned>(item.stackCount));
}

void WorldSession::clearAuctionSellItem() {
    if (auctionSellGuid_ == 0) return;
    auctionSellGuid_ = 0;
    auctionSellBag_ = auctionSellSlot_ = 0;
    pushAuctionEvent("NEW_AUCTION_UPDATE");
}

bool WorldSession::auctionSellItem(uint8_t& bag, uint8_t& slot) const {
    if (auctionSellGuid_ == 0) return false;
    bag = auctionSellBag_;
    slot = auctionSellSlot_;
    return true;
}

bool WorldSession::auctionSellItemDef(game::ItemDef& out) const {
    if (auctionSellGuid_ == 0 || !readWireSlot(auctionSellBag_, auctionSellSlot_, out) || out.itemId == 0) return false;
    const uint64_t now = auctionSellBag_ == game::slots::kNoContainer
                             ? getBackpackItemGuid(auctionSellSlot_ - game::slots::kBackpackFirst)
                             : getBagItemGuid(auctionSellBag_ - game::slots::kWornBagFirst, auctionSellSlot_);
    return now == auctionSellGuid_;  // the item moved (the server swapped it): the slot holds something else
}

// the auctionable items of the sell item's entry in the backpack and the worn bags.
uint32_t WorldSession::auctionSellTotalCount() const {
    game::ItemDef sell;
    if (!auctionSellItemDef(sell)) return 0;
    const auto& inv = getInventory();
    uint32_t total = 0;
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        const auto& s = inv.getBackpackSlot(i);
        if (!s.empty() && s.item.itemId == sell.itemId &&
            canAuctionItem(game::slots::kNoContainer, static_cast<uint8_t>(game::slots::kBackpackFirst + i)) == a::kLineNone)
            total += s.item.stackCount;
    }
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b)
        for (int s = 0; s < inv.getBagSize(b); ++s) {
            const auto& it = inv.getBagSlot(b, s);
            if (!it.empty() && it.item.itemId == sell.itemId &&
                canAuctionItem(static_cast<uint8_t>(game::slots::kWornBagFirst + b), static_cast<uint8_t>(s)) == a::kLineNone)
                total += it.item.stackCount;
        }
    return total;
}

// Own rule: the item in the sell slot is drawn locked in its bag, as an item on a letter or on the trade table.
bool WorldSession::auctionItemLocked(uint8_t bag, uint8_t slot) const {
    return auctionSellGuid_ != 0 && auctionSellBag_ == bag && auctionSellSlot_ == slot;
}

void WorldSession::startAuction(uint32_t minBid, uint32_t buyout, int runTime, uint32_t stackSize, uint32_t numStacks) {
    if (state_ != State::InWorld || !socket_ || auctioneerGuid_ == 0 || stackSize == 0) return;
    game::ItemDef sell;
    if (!auctionSellItemDef(sell)) return;
    const uint32_t minutes = a::runTimeMinutes(runTime);
    if (numStacks == 0) numStacks = 1;
    if (stackSize <= sell.stackCount && numStacks == 1) {
        network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_AUCTION_SELL_ITEM));
        a::writeSellItem(p, auctioneerGuid_, {a::SellPiece{auctionSellGuid_, stackSize}}, minBid, buyout, minutes);
        socket_->send(p);
        PSP_LOGI(tag::Net, "auction_sell entry=%u count=%u bid=%u buyout=%u min=%u", static_cast<unsigned>(sell.itemId),
                 static_cast<unsigned>(stackSize), static_cast<unsigned>(minBid), static_cast<unsigned>(buyout),
                 static_cast<unsigned>(minutes));
        clearAuctionSellItem();
        return;
    }
    // The auctionable bag items of this entry, the backpack first, then the bags, slot by slot - until there are
    // numStacks x stackSize of them.
    const uint32_t need = numStacks * stackSize;
    std::vector<a::SellPiece> sources;
    uint32_t total = 0;
    const auto& inv = getInventory();
    auto take = [&](uint8_t bag, uint8_t slot, const game::ItemDef& it, uint64_t guid) {
        if (total >= need || it.itemId != sell.itemId || guid == 0 || canAuctionItem(bag, slot) != a::kLineNone) return;
        total += it.stackCount;
        sources.push_back(a::SellPiece{guid, it.stackCount});
    };
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        const auto& s = inv.getBackpackSlot(i);
        if (!s.empty())
            take(game::slots::kNoContainer, static_cast<uint8_t>(game::slots::kBackpackFirst + i), s.item, getBackpackItemGuid(i));
    }
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b)
        for (int s = 0; s < inv.getBagSize(b); ++s) {
            const auto& it = inv.getBagSlot(b, s);
            if (!it.empty())
                take(static_cast<uint8_t>(game::slots::kWornBagFirst + b), static_cast<uint8_t>(s), it.item, getBagItemGuid(b, s));
        }
    if (total < need) {  // own rule: the key named by the reason
        auctionGameError(a::kLineEnoughItems);
        return;
    }
    auctionMulti_ = AuctionMultisell{};
    auctionMulti_.active = true;
    auctionMulti_.auctioneer = auctioneerGuid_;
    auctionMulti_.minBid = minBid;
    auctionMulti_.buyout = buyout;
    auctionMulti_.minutes = minutes;
    auctionMulti_.stackSize = stackSize;
    auctionMulti_.stacks = numStacks;
    auctionMulti_.left = numStacks;
    auctionMulti_.sources = std::move(sources);
    PSP_LOGI(tag::Net, "auction_multisell entry=%u stack=%u stacks=%u sources=%u", static_cast<unsigned>(sell.itemId),
             static_cast<unsigned>(stackSize), static_cast<unsigned>(numStacks),
             static_cast<unsigned>(auctionMulti_.sources.size()));
    if (numStacks > 1) pushAuctionEvent("AUCTION_MULTISELL_START", static_cast<int32_t>(numStacks));
    auctionMultisellNext();
}

// The next stack of the sale; with none left the sale is over and the sell slot empty.
void WorldSession::auctionMultisellNext() {
    AuctionMultisell& m = auctionMulti_;
    if (m.left == 0) {
        m = AuctionMultisell{};
        clearAuctionSellItem();
        return;
    }
    --m.left;
    std::vector<a::SellPiece> pieces;
    if (!a::takeStack(m.sources, m.stackSize, pieces)) {
        auctionMultisellStop(m.stacks > 1);
        return;
    }
    if (!socket_) return;
    network::Packet p(game::wireOpcode(LogicalOpcode::CMSG_AUCTION_SELL_ITEM));
    a::writeSellItem(p, m.auctioneer, pieces, m.minBid, m.buyout, m.minutes);
    socket_->send(p);
    if (++auctionLogs_ <= 120)
        PSP_LOGI(tag::Net, "auction_sell_stack left=%u pieces=%u stack=%u", static_cast<unsigned>(m.left),
                 static_cast<unsigned>(pieces.size()), static_cast<unsigned>(m.stackSize));
}

void WorldSession::auctionMultisellStop(bool failureEvent) {
    auctionMulti_ = AuctionMultisell{};
    clearAuctionSellItem();
    if (failureEvent) pushAuctionEvent("AUCTION_MULTISELL_FAILURE");
}

void WorldSession::cancelSell() {
    if (!auctionMulti_.active) return;
    PSP_LOGI(tag::Net, "auction_cancel_sell left=%u", static_cast<unsigned>(auctionMulti_.left));
    auctionMultisellStop(auctionMulti_.stacks > 1);
}

// --- Results and notifications -----------------------------------------------------------------------------------

void WorldSession::auctionGameError(a::Line line, const std::string& itemName) {
    if (line == a::kLineNone) return;
    std::string text = cs(lineText(line));
    if (const size_t at = text.find("%s"); at != std::string::npos) text.replace(at, 2, itemName);
    gameLine(a::lineIsNews(line) ? GameLine::News : GameLine::Refused, text);
}

void WorldSession::auctionItemLine(a::Line line, uint32_t itemEntry) {
    const auto* info = getItemInfo(itemEntry);
    if (!info || !info->valid) ensureItemInfo(itemEntry);
    auctionGameError(line, info && info->valid && !info->name.empty() ? info->name : psf(PS::ITEM_NUMBER, itemEntry));
}

void WorldSession::auctionCommandResult(network::Packet& packet) {
    a::CommandResult r;
    if (!a::readCommandResult(packet, r)) return;
    packet.skipAll();
    PSP_LOGI(tag::Net, "auction_result id=%u action=%u err=%u", static_cast<unsigned>(r.auctionId),
             static_cast<unsigned>(r.action), static_cast<unsigned>(r.error));
    if (r.action == a::kActSell && auctionMulti_.active) {
        if (r.error != a::kCmdOk) {
            auctionMultisellStop(auctionMulti_.stacks > 1);
        } else {
            if (auctionMulti_.stacks > 1)
                pushAuctionEvent("AUCTION_MULTISELL_UPDATE", static_cast<int32_t>(auctionMulti_.stacks - auctionMulti_.left),
                                 static_cast<int32_t>(auctionMulti_.stacks));
            auctionMultisellNext();
        }
    }
    auctionGameError(a::commandLine(r));
    if (auctioneerGuid_ == 0) return;
    if (r.error == a::kCmdOk) {
        if (r.action == a::kActSell) {
            auctionWant_[a::kOwner] = true;
        } else if (r.action == a::kActBid) {
            auctionOutbid_.erase(std::remove(auctionOutbid_.begin(), auctionOutbid_.end(), r.auctionId), auctionOutbid_.end());
            auctionWant_[a::kBidder] = true;
            auctionWant_[a::kList] = auctionHasQuery_;
        } else if (r.action == a::kActCancel) {
            auctionRemove(r.auctionId);
            auctionWant_[a::kOwner] = true;
        }
    } else if (r.action == a::kActBid && r.error == a::kCmdHigherBid) {
        auctionWant_[a::kList] = auctionHasQuery_;  // the list shows the higher bid
    }
    auctionSendWanted();
}

void WorldSession::auctionBidderNotification(network::Packet& packet) {
    a::BidderNotification n;
    if (!a::readBidderNotification(packet, n)) return;
    packet.skipAll();
    PSP_LOGI(tag::Net, "auction_bidder_note house=%u id=%u bid=%u item=%u", static_cast<unsigned>(n.houseId),
             static_cast<unsigned>(n.auctionId), static_cast<unsigned>(n.bid), static_cast<unsigned>(n.itemEntry));
    auctionOutbid_.erase(std::remove(auctionOutbid_.begin(), auctionOutbid_.end(), n.auctionId), auctionOutbid_.end());
    if (n.bid == 0) {  // won
        auctionItemLine(a::kLineWonS, n.itemEntry);
        auctionRemove(n.auctionId);
    } else {           // outbid
        auctionItemLine(a::kLineOutbidS, n.itemEntry);
        if (auctionOutbid_.size() >= kMaxOutbid) auctionOutbid_.erase(auctionOutbid_.begin());
        auctionOutbid_.push_back(n.auctionId);
    }
    if (auctioneerGuid_ != 0) {
        auctionWant_[a::kBidder] = true;
        auctionSendWanted();
    }
}

void WorldSession::auctionOwnerNotification(network::Packet& packet) {
    a::OwnerNotification n;
    if (!a::readOwnerNotification(packet, n)) return;
    packet.skipAll();
    PSP_LOGI(tag::Net, "auction_owner_note id=%u bid=%u item=%u", static_cast<unsigned>(n.auctionId),
             static_cast<unsigned>(n.bid), static_cast<unsigned>(n.itemEntry));
    auctionItemLine(n.bid == 0 ? a::kLineExpiredS : a::kLineSoldS, n.itemEntry);
    auctionRemove(n.auctionId);
    if (auctioneerGuid_ != 0) {
        auctionWant_[a::kOwner] = true;
        auctionSendWanted();
    }
}

void WorldSession::auctionRemovedNotification(network::Packet& packet) {
    a::RemovedNotification n;
    if (!a::readRemovedNotification(packet, n)) return;
    packet.skipAll();
    PSP_LOGI(tag::Net, "auction_removed_note id=%u item=%u", static_cast<unsigned>(n.auctionId),
             static_cast<unsigned>(n.itemEntry));
    auctionItemLine(a::kLineRemovedS, n.itemEntry);
    auctionRemove(n.auctionId);
}

}  // namespace wowee::platform::psp
