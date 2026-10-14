// Auction house: its packets and the port's rules - pure (network::Packet only), so the host test
// (tools/psp/host_tests/auction_proto_test.cpp) runs the same code as the PSP session (psp_world_session_auction.cpp).
//
// Sources:
//   - the packet layouts are AzerothCore's (src/server/game/Handlers/AuctionHouseHandler.cpp: SendAuctionHello,
//     SendAuctionCommandResult, SendAuctionBidderNotification, SendAuctionOwnerNotification, HandleAuctionSellItem;
//     AuctionHouse/AuctionHouseSearcher.cpp BuildAuctionInfo and the list results), the same ones upstream WoWee reads
//     (src/game/world_packets_economy.cpp);
//   - the sort columns are AzerothCore's AuctionSortOrder (AuctionHouseSearcher.h) under the names the auction
//     add-on's Lua uses for them (AuctionSort tables);
//   - the deposit is AzerothCore's AuctionHouseMgr::GetAuctionDeposit, the durations upstream's
//     include/game/auction_filters.hpp auctionDurationMinutes;
//   - the local comparator and the slot rows follow upstream (src/addons/lua_inventory_api.cpp auctionLess,
//     include/game/auction_filters.hpp auctionSlotsFor); the rest is the port's own simple rules, said where they are.
#pragma once

#include "game/auction_filters.hpp"
#include "network/packet.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::platform::psp::auction {

// GetNumAuctionItems / GetAuctionItemInfo "type": "list" 0, "bidder" 1, "owner" 2.
enum ListType : int { kList = 0, kBidder = 1, kOwner = 2, kListCount = 3 };

// One auction of a list.
struct Item {
    uint32_t houseId = 0;          // the house it came from (MSG_AUCTION_HELLO)
    uint32_t auctionId = 0;
    uint32_t itemEntry = 0;
    uint32_t enchantId = 0;        // the permanent enchantment (slot 0 of 7)
    uint32_t randomPropertyId = 0;
    uint32_t suffixFactor = 0;
    uint32_t count = 1;
    uint32_t charges = 0;
    uint64_t owner = 0;
    uint32_t minBid = 0;
    uint32_t minIncrement = 0;     // the "outbid" step
    uint32_t buyout = 0;
    uint32_t expireMs = 0;         // the session clock when it ends
    uint64_t bidder = 0;
    uint32_t bid = 0;              // the current bid
    // GetAuctionItemInfo's saleStatus (1 = sold, the money on its way). The servers the port targets list no sold
    // entries (AzerothCore answers CMSG_AUCTION_LIST_PENDING_SALES with none), so it stays 0.
    uint32_t saleStatus = 0;
};

// --- Sorting ---------------------------------------------------------------------------------------------------

// The columns - also the wire's sort mode (AzerothCore AuctionSortOrder: level 0 ... buyout 10).
enum Column : uint8_t {
    kColLevel = 0, kColQuality = 1, kColBuyoutThenBid = 2, kColDuration = 3, kColStatus = 4, kColName = 5,
    kColMinBidBuyout = 6, kColSeller = 7, kColBid = 8, kColQuantity = 9, kColBuyout = 10, kColumnCount = 11
};

inline const char* columnName(uint8_t c) {
    static const char* const kNames[kColumnCount] = {"level",  "quality", "buyoutthenbid", "duration",
                                                     "status", "name",    "minbidbuyout",  "seller",
                                                     "bid",    "quantity", "buyout"};
    return c < kColumnCount ? kNames[c] : nullptr;
}

inline bool columnByName(const char* name, uint8_t& out) {
    for (uint8_t c = 0; c < kColumnCount; ++c)
        if (std::strcmp(columnName(c), name) == 0) {
            out = c;
            return true;
        }
    return false;
}

// One list's ordering, the most significant key first. The add-on's Lua sets a table row by row
// (SortAuctionClearSort, then SortAuctionSetSort least significant first), so the key set last leads - as upstream
// reads GetAuctionSort (lua_inventory_api.cpp wireAuctionSort). Own rule: a column is kept once; setting it again
// moves it to the front.
struct SortTable {
    struct Key {
        uint8_t column = 0;
        bool reverse = false;
        bool used = false;
    };
    static constexpr int kSlots = kColumnCount;
    Key keys[kSlots];
    void clear() {
        for (Key& k : keys) k = Key{};
    }
    void set(uint8_t column, bool reverse) {
        if (column >= kColumnCount) return;
        int i = 0;
        while (i < kSlots - 1 && keys[i].used && keys[i].column != column) ++i;
        for (int k = i; k > 0; --k) keys[k] = keys[k - 1];
        keys[0] = Key{column, reverse, true};
    }
    int count() const {
        int n = 0;
        for (const Key& k : keys) n += k.used ? 1 : 0;
        return n;
    }
};

// What the comparator needs of the item cache and the name cache.
struct ItemFacts {
    bool known = false;
    int level = 0;          // RequiredLevel
    uint32_t quality = 1;
    std::string name;       // the item's name, or the "Item #n" a list row shows for an unknown one
};

// Upstream's comparator (lua_inventory_api.cpp auctionLess): every column ascending, an item not in the cache
// compared by what its row shows (its number, level 0, quality 1); "status" and "seller" by the guids. Own
// additions for the columns upstream has no rule for: "buyout" by the buyout, "buyoutthenbid" by the buyout then
// the bid. True when `a` goes before `b`.
template <typename Facts>
bool columnLess(const Item& a, const Item& b, uint8_t column, Facts facts) {
    auto bidOr = [](const Item& x) { return x.bid != 0 ? x.bid : x.minBid; };
    switch (column) {
        case kColQuantity: return a.count < b.count;
        case kColDuration: return static_cast<int32_t>(a.expireMs - b.expireMs) < 0;
        case kColBid: return bidOr(a) < bidOr(b);
        case kColMinBidBuyout: return a.buyout < b.buyout;
        case kColStatus: return a.bidder < b.bidder;
        case kColSeller: return a.owner < b.owner;
        case kColBuyout: return a.buyout < b.buyout;
        case kColBuyoutThenBid: return a.buyout != b.buyout ? a.buyout < b.buyout : bidOr(a) < bidOr(b);
        case kColName: return facts(a.itemEntry).name < facts(b.itemEntry).name;
        case kColLevel: return facts(a.itemEntry).level < facts(b.itemEntry).level;
        case kColQuality: return facts(a.itemEntry).quality < facts(b.itemEntry).quality;
        default: return false;
    }
}

// The whole table: the first key that tells them apart decides; a reversed key swaps the pair (upstream
// SortAuctionApplySort).
template <typename Facts>
bool itemLess(const Item& a, const Item& b, const SortTable& t, Facts facts) {
    for (const SortTable::Key& k : t.keys) {
        if (!k.used) continue;
        const bool ab = k.reverse ? columnLess(b, a, k.column, facts) : columnLess(a, b, k.column, facts);
        if (ab) return true;
        const bool ba = k.reverse ? columnLess(a, b, k.column, facts) : columnLess(b, a, k.column, facts);
        if (ba) return false;
    }
    return false;
}

// --- Prices and times ------------------------------------------------------------------------------------------

// StartAuction / CalculateAuctionDeposit: the duration dropdown's 1..3 -> minutes (upstream auctionDurationMinutes:
// 720 / 1440 / 2880, anything else twelve hours).
inline uint32_t runTimeMinutes(int runTime) {
    return game::auctionDurationMinutes(runTime > 0 ? static_cast<uint32_t>(runTime) : 0u);
}

// The deposit as AzerothCore charges it (AuctionHouseMgr::GetAuctionDeposit): the house's deposit per cent
// (AuctionHouse.dbc) of the vendor price x count, x3 per twelve hours, at least 100 copper (AH_MINIMUM_DEPOSIT).
inline uint32_t depositFor(uint32_t depositRate, uint32_t sellPrice, uint32_t count, uint32_t minutes) {
    const uint64_t periods = minutes / 720u;
    const uint64_t d = uint64_t{depositRate} * sellPrice * count * 3u * periods / 100u;
    return d < 100u ? 100u : static_cast<uint32_t>(std::min<uint64_t>(d, 0xFFFFFFFFu));
}

// GetAuctionItemTimeLeft for an unsold auction: 1 short (< 30 min), 2 medium (< 2 h), 3 long (< 12 h),
// 4 very long (upstream lua_inventory_api.cpp GetAuctionItemTimeLeft).
inline int timeLeftBucket(uint32_t msLeft) {
    static const uint32_t kLimits[3] = {1800000u, 7200000u, 43200000u};
    for (int i = 0; i < 3; ++i)
        if (msLeft < kLimits[i]) return i + 1;
    return 4;
}

constexpr uint32_t kMaximumBidPrice = 2000000000u;  // MAXIMUM_BID_PRICE (the auction add-on's Lua)

// The lines the auction code shows, by their GlobalStrings key. The refusals come first, the news after
// kLineStarted: the session shows the first kind as the red error line and the second in the chat (GameLine).
enum Line : uint8_t {
    kLineNone = 0,
    kLineItemNotFound,      // ERR_ITEM_NOT_FOUND
    kLineNotEnoughMoney,    // ERR_NOT_ENOUGH_MONEY
    kLineBoundItem,         // ERR_AUCTION_BOUND_ITEM
    kLineConjuredItem,      // ERR_AUCTION_CONJURED_ITEM
    kLineBag,               // ERR_AUCTION_BAG
    kLineDatabase,          // ERR_AUCTION_DATABASE_ERROR
    kLineBidOwn,            // ERR_AUCTION_BID_OWN
    kLineBidIncrement,      // ERR_AUCTION_BID_INCREMENT
    kLineHigherBid,         // ERR_AUCTION_HIGHER_BID
    kLineMinBid,            // ERR_AUCTION_MIN_BID
    kLineRestricted,        // ERR_RESTRICTED_ACCOUNT
    kLineEnoughItems,       // ERR_AUCTION_ENOUGH_ITEMS
    kLineStarted,           // ERR_AUCTION_STARTED
    kLineRemoved,           // ERR_AUCTION_REMOVED
    kLineBidPlaced,         // ERR_AUCTION_BID_PLACED
    kLineOutbidS,           // ERR_AUCTION_OUTBID_S
    kLineWonS,              // ERR_AUCTION_WON_S
    kLineSoldS,             // ERR_AUCTION_SOLD_S
    kLineExpiredS,          // ERR_AUCTION_EXPIRED_S
    kLineRemovedS,          // ERR_AUCTION_REMOVED_S
};
inline bool lineIsNews(Line l) { return l >= kLineStarted; }

// The port's checks before CMSG_AUCTION_PLACE_BID. AzerothCore (HandleAuctionPlaceBid) drops a bid that is too low
// or that the player cannot pay without an answer, so the port says why itself (own rule, the key named by the
// reason): a bid past the buyout is the buyout (upstream PlaceAuctionBid); below the opening bid of an auction
// nobody bid on -> ERR_AUCTION_MIN_BID; below the current bid + the increment (unless it is the buyout) ->
// ERR_AUCTION_BID_INCREMENT; more than the player has (AzerothCore asks for the whole bid) -> ERR_NOT_ENOUGH_MONEY.
// `bid` comes back as it is to be sent.
inline Line checkBid(const Item& a, uint32_t& bid, uint64_t money) {
    if (a.buyout != 0 && bid > a.buyout) bid = a.buyout;
    const bool buyout = a.buyout != 0 && bid == a.buyout;
    if (!buyout) {
        if (a.bid == 0 && bid < a.minBid) return kLineMinBid;
        if (a.bid != 0 && bid < a.bid + a.minIncrement) return kLineBidIncrement;
    }
    if (bid == 0) return kLineMinBid;
    if (bid > money) return kLineNotEnoughMoney;
    return kLineNone;
}

// --- The wire --------------------------------------------------------------------------------------------------

// CMSG_AUCTION_SELL_ITEM (AzerothCore HandleAuctionSellItem): the auctioneer, the item count (at most
// MAX_AUCTION_ITEMS), per item its guid and how many of it, the minimum bid, the buyout, the time in minutes.
struct SellPiece {
    uint64_t guid = 0;
    uint32_t count = 0;
};
constexpr size_t kMaxSellPieces = 160;  // MAX_AUCTION_ITEMS

inline void writeSellItem(network::Packet& p, uint64_t auctioneer, const std::vector<SellPiece>& pieces, uint32_t bid,
                          uint32_t buyout, uint32_t minutes) {
    p.writeUInt64(auctioneer);
    p.writeUInt32(static_cast<uint32_t>(pieces.size()));
    for (const SellPiece& s : pieces) {
        p.writeUInt64(s.guid);
        p.writeUInt32(s.count);
    }
    p.writeUInt32(bid);
    p.writeUInt32(buyout);
    p.writeUInt32(minutes);
}

// One stack of a sale made of several bag items (own rule): the items are used in the order they were collected
// (the backpack, then the bags, slot by slot), whole ones first; the one used in part keeps its rest for the next
// stack. False: not enough left.
inline bool takeStack(std::vector<SellPiece>& sources, uint32_t stackSize, std::vector<SellPiece>& out) {
    out.clear();
    if (stackSize == 0) return false;
    uint32_t need = stackSize;
    size_t used = 0;
    while (need > 0 && used < sources.size() && out.size() < kMaxSellPieces) {
        SellPiece& s = sources[used];
        const uint32_t take = std::min(need, s.count);
        out.push_back(SellPiece{s.guid, take});
        s.count -= take;
        need -= take;
        if (s.count == 0) ++used;
    }
    if (need > 0) return false;
    sources.erase(sources.begin(), sources.begin() + static_cast<long>(used));
    return true;
}

// MSG_AUCTION_HELLO (AzerothCore SendAuctionHello): the auctioneer, the house (AuctionHouse.dbc), enabled.
struct Hello {
    uint64_t guid = 0;
    uint32_t houseId = 0;
    bool enabled = false;
};

inline bool readHello(network::Packet& p, Hello& out) {
    if (!p.hasRemaining(13)) return false;
    out.guid = p.readUInt64();
    out.houseId = p.readUInt32();
    out.enabled = p.readUInt8() != 0;
    return true;
}

// A list entry (AzerothCore BuildAuctionInfo, 148 bytes): the id, the entry, 7 x (enchantment id, duration,
// charges), the random property, the suffix factor, the count, the charges, a flags word, the owner, the minimum
// bid, the increment, the buyout, the time left in ms, the bidder, the bid.
inline bool readListEntry(network::Packet& p, Item& e, uint32_t nowMs, uint32_t houseId) {
    if (!p.hasRemaining(148)) return false;
    e = Item{};
    e.houseId = houseId;
    e.auctionId = p.readUInt32();
    e.itemEntry = p.readUInt32();
    for (int s = 0; s < 7; ++s) {
        const uint32_t id = p.readUInt32();
        p.readUInt32();  // duration
        p.readUInt32();  // charges
        if (s == 0) e.enchantId = id;
    }
    e.randomPropertyId = p.readUInt32();
    e.suffixFactor = p.readUInt32();
    e.count = p.readUInt32();
    e.charges = p.readUInt32();
    p.readUInt32();  // flags (AzerothCore writes 0)
    e.owner = p.readUInt64();
    e.minBid = p.readUInt32();
    e.minIncrement = p.readUInt32();
    e.buyout = p.readUInt32();
    e.expireMs = nowMs + p.readUInt32();
    e.bidder = p.readUInt64();
    e.bid = p.readUInt32();
    return true;
}

// A whole list result: the count, the entries (PSP: at most `maxKeep` kept, the rest read past), the total, the
// next query's delay in ms (AUCTION_SEARCH_DELAY).
struct ListTail {
    uint32_t count = 0, total = 0, delayMs = 0;
    bool hasDelay = false;
};

inline bool readList(network::Packet& p, std::vector<Item>& out, size_t maxKeep, uint32_t nowMs, uint32_t houseId,
                     ListTail& tail) {
    tail = ListTail{};
    if (!p.hasRemaining(4)) return false;
    tail.count = p.readUInt32();
    Item e;
    for (uint32_t i = 0; i < tail.count; ++i) {
        if (!readListEntry(p, e, nowMs, houseId)) return false;
        if (out.size() < maxKeep) out.push_back(e);
    }
    if (p.hasRemaining(4)) tail.total = p.readUInt32();
    if (p.hasRemaining(4)) {
        tail.delayMs = p.readUInt32();
        tail.hasDelay = true;
    }
    return true;
}

// SMSG_AUCTION_COMMAND_RESULT (AzerothCore SendAuctionCommandResult, AuctionAction / AuctionError): the auction,
// the action, the error; with error 0 after a cancel or a bid a u32 follows (AzerothCore writes 0; not used).
enum CommandAction : uint32_t { kActSell = 0, kActCancel = 1, kActBid = 2 };
enum CommandError : uint32_t {
    kCmdOk = 0, kCmdInventory = 1, kCmdDatabase = 2, kCmdNotEnoughMoney = 3, kCmdItemNotFound = 4,
    kCmdHigherBid = 5, kCmdBidIncrement = 7, kCmdBidOwn = 10, kCmdRestrictedAccount = 13
};
struct CommandResult {
    uint32_t auctionId = 0, action = 0, error = 0;
};

inline bool readCommandResult(network::Packet& p, CommandResult& out) {
    out = CommandResult{};
    if (!p.hasRemaining(12)) return false;
    out.auctionId = p.readUInt32();
    out.action = p.readUInt32();
    out.error = p.readUInt32();
    return true;
}

// The line of a command result: an OK by its action, an error by the GlobalStrings key AzerothCore's AuctionError
// names (anything else -> ERR_AUCTION_DATABASE_ERROR).
inline Line commandLine(const CommandResult& r) {
    if (r.error == kCmdOk)
        return r.action == kActSell ? kLineStarted : r.action == kActCancel ? kLineRemoved : kLineBidPlaced;
    switch (r.error) {
        case kCmdNotEnoughMoney: return kLineNotEnoughMoney;
        case kCmdItemNotFound: return kLineItemNotFound;
        case kCmdHigherBid: return kLineHigherBid;
        case kCmdBidIncrement: return kLineBidIncrement;
        case kCmdBidOwn: return kLineBidOwn;
        case kCmdRestrictedAccount: return kLineRestricted;
        default: return kLineDatabase;
    }
}

// SMSG_AUCTION_BIDDER_NOTIFICATION (AzerothCore SendAuctionBidderNotification): the house, the auction, the bidder,
// the bid (0: the auction was won - SendAuctionWonMail), the increment, the item, its random property.
struct BidderNotification {
    uint32_t houseId = 0, auctionId = 0;
    uint64_t bidder = 0;
    uint32_t bid = 0, outbid = 0, itemEntry = 0, randomPropertyId = 0;
};

inline bool readBidderNotification(network::Packet& p, BidderNotification& out) {
    out = BidderNotification{};
    if (!p.hasRemaining(32)) return false;
    out.houseId = p.readUInt32();
    out.auctionId = p.readUInt32();
    out.bidder = p.readUInt64();
    out.bid = p.readUInt32();
    out.outbid = p.readUInt32();
    out.itemEntry = p.readUInt32();
    out.randomPropertyId = p.readUInt32();
    return true;
}

// SMSG_AUCTION_OWNER_NOTIFICATION (AzerothCore SendAuctionOwnerNotification): the auction, the bid (0: it expired,
// else it sold), three fields AzerothCore writes as 0, the item, a field written 0, a float written 0.
struct OwnerNotification {
    uint32_t auctionId = 0, bid = 0, itemEntry = 0;
};

inline bool readOwnerNotification(network::Packet& p, OwnerNotification& out) {
    out = OwnerNotification{};
    if (!p.hasRemaining(24)) return false;
    out.auctionId = p.readUInt32();
    out.bid = p.readUInt32();
    p.readUInt32();
    p.readUInt64();
    out.itemEntry = p.readUInt32();
    return true;
}

// SMSG_AUCTION_REMOVED_NOTIFICATION (upstream inventory_handler.cpp): the auction, the item, its random property.
struct RemovedNotification {
    uint32_t auctionId = 0, itemEntry = 0, randomPropertyId = 0;
};

inline bool readRemovedNotification(network::Packet& p, RemovedNotification& out) {
    out = RemovedNotification{};
    if (!p.hasRemaining(12)) return false;
    out.auctionId = p.readUInt32();
    out.itemEntry = p.readUInt32();
    out.randomPropertyId = p.readUInt32();
    return true;
}

// --- The search filters ----------------------------------------------------------------------------------------

// GetAuctionItemClasses: upstream's twelve classes in its order (include/game/auction_filters.hpp kAuctionClasses,
// row 0 "All" left out).
inline uint32_t classIdAt(int index0) {
    return index0 >= 0 && index0 + 1 < game::kNumAuctionClasses ? game::kAuctionClasses[index0 + 1].classId
                                                                  : game::kAuctionAny;
}

// GetAuctionInvTypes: the slot rows under a weapon or armour subclass - upstream's lists (auction_filters.hpp
// auctionSlotsFor), found by the subclass id (the PSP lists the subclasses in ItemSubClass.dbc order, upstream in
// its own); a subclass upstream does not name gets the class's "any" list. Fills the inventory types in row order;
// returns how many (0: the class has no slot rows).
inline int slotRows(uint32_t classId, uint32_t subId, uint32_t (&out)[game::kNumAuctionSlots]) {
    int subCount = 0;
    const game::AuctionSubFilter* subs = game::auctionSubsFor(classId, subCount);
    if (!subs) return 0;
    int subIndex = 0;
    for (int i = 1; i < subCount; ++i)
        if (subs[i].subId == subId) subIndex = i;
    int n = 0;
    const uint8_t* slots = game::auctionSlotsFor(classId, subIndex, n);
    int k = 0;
    for (int i = 0; slots && i < n && k < game::kNumAuctionSlots; ++i)
        if (slots[i] < game::kNumAuctionSlots) out[k++] = game::kAuctionSlots[slots[i]].invType;
    return k;
}

}  // namespace wowee::platform::psp::auction
