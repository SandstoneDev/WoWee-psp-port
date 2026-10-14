// The auction house of the original - AuctionFrame of the client's auction add-on (its .xml / .lua and its
// templates, ruRU 3.3.5a) on its own art (ui.pak auc_*), over the session's auction half
// (psp_world_session_auction.cpp).
//
//   - AUCTION_HOUSE_SHOW -> AuctionFrame_Show (the cursor comes up; GetBidderAuctionItems / GetOwnerAuctionItems),
//     AuctionFrame_OnShow (the Browse tab, BROWSE_SEARCH_TEXT, AuctionWindowOpen); AUCTION_HOUSE_CLOSED -> the frame
//     hides (OnHide: AuctionWindowClose, StaticPopup_Hide BUYOUT_AUCTION / CANCEL_AUCTION, CloseAuctionHouse);
//     AUCTION_HOUSE_DISABLED -> StaticPopup AUCTION_HOUSE_DISABLED;
//   - the frame (832 x 447 units at 0.55), the tab's six textures with its static widgets (build_ui_pak.py
//     auction_frames), the portrait, the title, the close button, the three tabs (AuctionTabTemplate), the money;
//   - BROWSE (AuctionFrameBrowse): NAME (BrowseName), LEVEL_RANGE (BrowseMinLevel / MaxLevel),
//     RARITY (BrowseDropDown: ALL, ITEM_QUALITY0..5_DESC), USABLE_ITEMS (IsUsableCheckButton), DISPLAY_ON_CHARACTER
//     (ShowOnPlayerCheckButton = CVar auctionDisplayOnCharacter: a row clicked goes on the dressing room), SEARCH
//     (enabled by CanSendAuctionQuery; SEARCHING_FOR_ITEMS with its dots while it runs), RESET; the category list
//     (GetAuctionItemClasses / GetAuctionItemSubClasses / GetAuctionInvTypes: ItemClass / ItemSubClass.dbc ruRU,
//     upstream's twelve classes and slot rows, 15 rows, the selected ones open, FilterBg / FilterLines);
//     the five sort columns (AuctionFrame_OnClickSortColumn with the AuctionSort tables: the list asks again); eight
//     rows (BrowseButtonTemplate: icon (red when it cannot be used), count, name in the quality colour, level (red
//     above ours), AUCTION_TIME_LEFT1..4, the seller, the bid or minimum bid, YOUR_BID, BUYOUT_COST + the buyout),
//     FauxScrollFrame and the pages of 50 (BrowsePrevPageButton / NextPage, NUMBER_OF_RESULTS_TEMPLATE); BID
//     (BrowseBidPrice, the required bid set on select) -> BID_AUCTION, BUYOUT -> BUYOUT_AUCTION, CLOSE;
//   - BIDS (AuctionFrameBid): six sort columns (SortAuctionApplySort: sorted here), nine rows (the buyout, the
//     status HIGH_BIDDER green / OUTBID red, the bid), BID (PlaceAuctionBid at once), BUYOUT;
//   - AUCTIONS (AuctionFrameAuctions): AUCTION_TITLE with our name, the sell slot (ClickAuctionSellItemButton - the
//     item on the cursor, or the one in it back on the cursor; NEW_AUCTION_UPDATE: name, count, the start price
//     max(100, 1.5 x the vendor price) or LAST_ITEM_*), AUCTION_STACK_SIZE / AUCTION_NUM_STACKS + MAXIMUM (stacks),
//     AUCTION_PRICE (PriceDropDown: AUCTION_PRICE_PER_ITEM / _PER_STACK), STARTING_PRICE / BUYOUT_PRICE (OPTIONAL)
//     (MoneyInputFrames; AUCTION_BUYOUT_ERROR), AUCTION_DURATION (AUCTION_DURATION_ONE..THREE), DEPOSIT_COLON
//     (CalculateAuctionDeposit), CREATE_AUCTION (AuctionsFrameAuctions_ValidateAuction, StartAuction); nine rows (sold
//     ones grey AUCTION_ITEM_SOLD with AUCTION_ITEM_TIME_UNTIL_DELIVERY and AUCTION_ITEM_INCOMING_AMOUNT),
//     CANCEL_AUCTION -> the CANCEL_AUCTION popup (the 5 % of a bid);
//     AuctionProgressFrame for several stacks (AUCTION_MULTISELL_*: the bar, AUCTION_CREATING (n/N), the icon, the
//     cancel);
//   - GameTooltip:SetAuctionItem of the focused row (+ AUCTION_TOOLTIP_BID_PREFIX / _BUYOUT_PREFIX per unit for a
//     stack), the sell slot's item (SetAuctionSellItem) or AUCTION_ITEM_TEXT, DISPLAY_ON_CHAR_TOOLTIP.
// PSP (marked PSP:):
//   - Cross = the left click, Circle on a bag item with AUCTIONS shown = into the sell slot (UseContainerItem with
//     SetAuctionsTabShowing), Square on the sell slot = the drag; the text boxes and money boxes take the system
//     keyboard (OSK); START closes a dropdown list / a popup, the frame closes with its close buttons;
//   - the fonts are the PSP's (10 / 8 px): long names and cells scroll (textMarquee) instead of wrapping; titles in
//     the 10 px font (the 13 px one does not fit the 18-unit bands);
//   - lists follow the focus (the first / last row scrolls one), the scroll bars' arrows are buttons;
//   - the item names have no random suffix (no ItemRandomProperties on the card - as the tooltips);
//   - no AuctionDressUpFrame (the dressing room of the PSP takes the item), no closing-time tooltips per cell,
//     no Shift / Ctrl clicks (the tooltip's L = compare and L + Triangle = try on work on the rows);
//   - AuctionProgressFrame is drawn over the frame's bottom (the PSP screen has no room under it).
// Memory: the tab's frame (T8 512 x 256, ~130 KB) + the small parts (~90 KB) + auc_data (~3 KB) only while the frame
// is open (log auction_art kb=); the lists are the session's (bounded there).
#include "ui/psp/ui_world_screen.hpp"

#include "game/inventory_slots.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace a = wowee::platform::psp::auction;

namespace {

// AuctionFrame units -> px (tools/psp/build_ui_pak.py AUC_S): 832 x 447 at 0.55 = 458 x 246, centred.
constexpr float kS = 0.55f, kOx = 11.0f, kOy = 2.0f;
constexpr float kFrameW = 832.0f, kFrameH = 447.0f;
float PX(float u) { return kOx + u * kS; }
float PY(float u) { return kOy + u * kS; }
Rect R(float x, float y, float w, float h) { return Rect{PX(x), PY(y), w * kS, h * kS}; }

constexpr int kBrowseRows = 8, kBidRows = 9, kAuctionRows = 9, kFilterRows = 15;  // NUM_*_TO_DISPLAY
constexpr int kPerPage = 50;                    // NUM_AUCTION_ITEMS_PER_PAGE
constexpr float kRowU = 37.0f;                  // AUCTIONS_BUTTON_HEIGHT
constexpr float kFilterU = 20.0f;               // BROWSE_FILTER_HEIGHT
constexpr float kDropBrowseX = 230.0f + 86.0f - 5.0f;  // BrowseDropDown (build_ui_pak.py AUC_LEVEL_TEXT_W)
constexpr int kOskOwner = 0x4143;               // "AC": the auction frame's keyboard (psp_osk oskBeginFor)
constexpr uint32_t kPriceUnit = 1, kPriceStack = 2;  // PRICE_TYPE_UNIT / _STACK

constexpr uint32_t kColRedFont = 0xFF1A1AFFu;    // RED_FONT_COLOR (1, 0.1, 0.1)
constexpr uint32_t kColGreenFont = 0xFF1AFF1Au;  // GREEN_FONT_COLOR (0.1, 1, 0.1)
constexpr uint32_t kColGrayFont = 0xFF808080u;   // GRAY_FONT_COLOR (0.5, 0.5, 0.5)
constexpr uint32_t kTintRed = 0xFF1A1AFFu;       // SetVertexColor(1.0, 0.1, 0.1)
constexpr uint32_t kTintGray = 0xFF808080u;      // SetVertexColor(0.5, 0.5, 0.5)

// Widget ids (0x8200-0x83FF).
enum : uint32_t {
    kIdClose = 0x8200, kIdTab1, kIdTab2, kIdTab3,
    kIdBrowseName = 0x8210, kIdMinLevel, kIdMaxLevel, kIdQualityDD, kIdUsable, kIdShowOnPlayer, kIdSearch, kIdReset,
    kIdPrev, kIdNext, kIdBrowseBidG, kIdBrowseBidS, kIdBrowseBidC, kIdBrowseBid, kIdBrowseBuyout, kIdBrowseClose,
    kIdBrowseUp, kIdBrowseDown, kIdFilterUp, kIdFilterDown,
    kIdSortList0 = 0x8230,   // 5
    kIdFilter0 = 0x8240,     // 15
    kIdBrowseRow0 = 0x8260,  // 8
    kIdSortBid0 = 0x8280,    // 6
    kIdBidRow0 = 0x8290,     // 9
    kIdBidUp = 0x82A0, kIdBidDown, kIdBidG, kIdBidS, kIdBidC, kIdBidBid, kIdBidBuyout, kIdBidClose,
    kIdSortOwn0 = 0x82C0,    // 4
    kIdOwnRow0 = 0x82D0,     // 9
    kIdOwnUp = 0x82E0, kIdOwnDown, kIdSellSlot, kIdStackSize, kIdStackMax, kIdNumStacks, kIdNumStacksMax, kIdPriceDD,
    kIdStartG, kIdStartS, kIdStartC, kIdBuyoutG, kIdBuyoutS, kIdBuyoutC, kIdDurationDD, kIdCreate, kIdCancelAuction,
    kIdAuctionsClose, kIdBuyoutError,
    kIdProgressCancel = 0x8300,
    kIdPopupAccept = 0x8310, kIdPopupCancel, kIdDisabledOk,
    kIdDdItem0 = 0x8320,     // up to 16
};

enum : uint8_t { kPopupNone = 0, kPopupBuyout, kPopupBid, kPopupCancel };   // StaticPopupDialogs of the AH
enum : uint8_t { kDdNone = 0, kDdQuality, kDdPrice, kDdDuration };

// AuctionSort (the auction add-on's Lua): the presets AuctionFrame_SetSort sets row by row.
struct SortRow {
    const char* column;
    bool reverse;
};
struct SortPreset {
    const char* table;
    const char* column;
    SortRow rows[8];
    int n;
};
const SortPreset kPresets[] = {
    {"owner", "status", {{"quantity", true}, {"bid", false}, {"name", false}, {"level", true}, {"quality", false}, {"duration", false}, {"status", false}}, 7},
    {"owner", "bid", {{"quantity", true}, {"name", false}, {"level", true}, {"quality", false}, {"duration", false}, {"status", false}, {"bid", false}}, 7},
    {"owner", "quality", {{"bid", false}, {"quantity", true}, {"minbidbuyout", false}, {"name", false}, {"level", true}, {"quality", false}}, 6},
    {"owner", "duration", {{"quantity", true}, {"bid", false}, {"name", false}, {"level", true}, {"quality", false}, {"status", false}, {"duration", false}}, 7},
    {"bidder", "quality", {{"bid", false}, {"quantity", true}, {"minbidbuyout", false}, {"name", false}, {"level", true}, {"quality", false}}, 6},
    {"bidder", "level", {{"minbidbuyout", true}, {"status", true}, {"bid", true}, {"duration", true}, {"quantity", false}, {"name", true}, {"quality", true}, {"level", false}}, 8},
    {"bidder", "buyout", {{"quantity", true}, {"name", false}, {"level", true}, {"quality", false}, {"status", false}, {"bid", false}, {"duration", false}, {"buyout", false}}, 8},
    {"bidder", "status", {{"quantity", true}, {"name", false}, {"level", true}, {"quality", false}, {"minbidbuyout", false}, {"bid", false}, {"duration", false}, {"status", false}}, 8},
    {"bidder", "bid", {{"quantity", true}, {"name", false}, {"level", true}, {"quality", false}, {"minbidbuyout", false}, {"status", false}, {"duration", false}, {"bid", false}}, 8},
    {"bidder", "duration", {{"quantity", true}, {"name", false}, {"level", true}, {"quality", false}, {"minbidbuyout", false}, {"status", false}, {"bid", false}, {"duration", false}}, 8},
    {"list", "level", {{"duration", true}, {"bid", true}, {"quantity", false}, {"minbidbuyout", true}, {"name", true}, {"quality", true}, {"level", false}}, 7},
    {"list", "duration", {{"bid", false}, {"quantity", true}, {"minbidbuyout", false}, {"name", false}, {"level", true}, {"quality", false}, {"duration", false}}, 7},
    {"list", "seller", {{"duration", false}, {"bid", false}, {"quantity", true}, {"minbidbuyout", false}, {"name", false}, {"level", true}, {"quality", false}, {"seller", false}}, 8},
    {"list", "bid", {{"duration", false}, {"quantity", true}, {"name", false}, {"level", true}, {"quality", false}, {"bid", false}}, 6},
    {"list", "quality", {{"duration", false}, {"bid", false}, {"quantity", true}, {"minbidbuyout", false}, {"name", false}, {"level", true}, {"quality", false}}, 7},
};

const char* tableName(int list) { return list == a::kList ? "list" : list == a::kBidder ? "bidder" : "owner"; }

// ITEM_QUALITY_COLORS (0xAABBGGRR).
uint32_t qualityColor(uint32_t q) {
    switch (q) {
        case 0: return 0xFF9D9D9Du;
        case 2: return 0xFF00FF1Eu;
        case 3: return 0xFFDD7000u;
        case 4: return 0xFFEE35A3u;
        case 5: return 0xFF0080FFu;
        case 6:
        case 7: return 0xFF80CCE6u;
        default: return 0xFFFFFFFFu;
    }
}

// ITEM_QUALITY0..5_DESC.
const char* qualityDesc(int q) {
    static constexpr pp::CS k[6] = {pp::CS::ITEM_QUALITY0_DESC, pp::CS::ITEM_QUALITY1_DESC, pp::CS::ITEM_QUALITY2_DESC,
                                    pp::CS::ITEM_QUALITY3_DESC, pp::CS::ITEM_QUALITY4_DESC, pp::CS::ITEM_QUALITY5_DESC};
    return pp::cs(k[std::clamp(q, 0, 5)]);
}
// AUCTION_TIME_LEFT1..4.
const char* timeLeftText(int t) {
    static constexpr pp::CS k[4] = {pp::CS::AUCTION_TIME_LEFT1, pp::CS::AUCTION_TIME_LEFT2, pp::CS::AUCTION_TIME_LEFT3,
                                    pp::CS::AUCTION_TIME_LEFT4};
    return pp::cs(k[std::clamp(t, 1, 4) - 1]);
}
// AUCTION_DURATION_ONE / _TWO / _THREE.
const char* durationText(int d) {
    static constexpr pp::CS k[3] = {pp::CS::AUCTION_DURATION_ONE, pp::CS::AUCTION_DURATION_TWO,
                                    pp::CS::AUCTION_DURATION_THREE};
    return pp::cs(k[std::clamp(d, 1, 3) - 1]);
}
// A slot row's text: the GlobalStrings INVTYPE_* token upstream's kAuctionSlots names for that inventory type
// (include/game/auction_filters.hpp).
const char* slotText(uint32_t invType) {
    switch (invType) {
        case 1: return pp::cs(pp::CS::INVTYPE_HEAD);
        case 2: return pp::cs(pp::CS::INVTYPE_NECK);
        case 3: return pp::cs(pp::CS::INVTYPE_SHOULDER);
        case 5: return pp::cs(pp::CS::INVTYPE_CHEST);
        case 6: return pp::cs(pp::CS::INVTYPE_WAIST);
        case 7: return pp::cs(pp::CS::INVTYPE_LEGS);
        case 8: return pp::cs(pp::CS::INVTYPE_FEET);
        case 9: return pp::cs(pp::CS::INVTYPE_WRIST);
        case 10: return pp::cs(pp::CS::INVTYPE_HAND);
        case 11: return pp::cs(pp::CS::INVTYPE_FINGER);
        case 12: return pp::cs(pp::CS::INVTYPE_TRINKET);
        case 13: return pp::cs(pp::CS::INVTYPE_WEAPON);
        case 14: return pp::cs(pp::CS::INVTYPE_SHIELD);
        case 16: return pp::cs(pp::CS::INVTYPE_CLOAK);
        case 17: return pp::cs(pp::CS::INVTYPE_2HWEAPON);
        case 21: return pp::cs(pp::CS::INVTYPE_WEAPONMAINHAND);
        case 22: return pp::cs(pp::CS::INVTYPE_WEAPONOFFHAND);
        case 23: return pp::cs(pp::CS::INVTYPE_HOLDABLE);
        case 26: return pp::cs(pp::CS::INVTYPE_RANGED);
        case 28: return pp::cs(pp::CS::INVTYPE_RELIC);
        default: return "";
    }
}

void loadTex(pp::Pak& pak, const char* name, pp::Texture& t) {
    if (t.pixels) return;
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
}

// SecondsToTime (UIParent.lua), two units at most: DAYS_ABBR / HOURS_ABBR / MINUTES_ABBR / SECONDS_ABBR.
std::string secondsToTime(int seconds) {
    std::string out;
    int count = 0;
    auto add = [&](pp::CS fmt, int v) {
        if (!out.empty()) out += ' ';
        out += pp::text::plural(pp::csf(fmt, v));
        ++count;
    };
    if (seconds >= 86400) {
        add(pp::CS::DAYS_ABBR, seconds / 86400);
        seconds %= 86400;
    }
    if (seconds >= 3600) {
        add(pp::CS::HOURS_ABBR, seconds / 3600);
        seconds %= 3600;
    }
    if (count < 2 && seconds >= 60) {
        add(pp::CS::MINUTES_ABBR, seconds / 60);
        seconds %= 60;
    }
    if (count < 2 && seconds > 0) add(pp::CS::SECONDS_ABBR, seconds);
    return out;
}

uint32_t digitsOf(const std::string& s, uint32_t max) {
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') continue;
        v = v * 10u + static_cast<uint32_t>(c - '0');
        if (v > max) v = max;
    }
    return v;
}

}  // namespace

// --- The state ---------------------------------------------------------------------------------------------------

struct WorldScreen::AuctionUi {
    // Art (ui.pak; all optional).
    pp::Texture frame;  // auc_browse / auc_bid / auc_auctions - the shown tab's
    int frameTab = 0;
    pp::Texture colTab, arrow, nameFr, rowHl, tabHl, filterBg, filterLines, input, scroll, post, alert, alertGear;
    pp::Texture cbUp, cbHl, cbCheck, ddBtn, ddBtnDn, ddHl, ddBg, ddCheck, prev, prevDis, next, nextDis, castBorder,
        bar, slotHl;
    bool smallTried = false;
    bool disabledArtTried = false;  // AUCTION_HOUSE_DISABLED's icon asked for (drawAuctionWindow)
    // auc_data: the classes in GetAuctionItemClasses' order, their shown subclasses, the houses' deposit rates.
    struct Sub {
        uint32_t id = 0;
        std::string name;
    };
    struct Cls {
        uint32_t id = 0;
        std::string name;
        std::vector<Sub> subs;
    };
    std::vector<Cls> classes;
    std::vector<std::pair<uint32_t, uint32_t>> deposit;  // house -> DepositRate
    bool dataTried = false;

    // AuctionFrame.
    bool shown = false, guard = false, raisedCursor = false;
    int tab = 1;
    int type = a::kList;  // AuctionFrame.type
    bool gotBids = false, gotAuctions = false;
    int sel[a::kListCount] = {};  // GetSelectedAuctionItem (1-based in the list; 0 none)
    uint32_t buyoutPrice = 0;     // AuctionFrame.buyoutPrice
    // Browse.
    int page = 0, browseOffset = 0, filterOffset = 0;
    bool searching = false, searchedOnce = false;
    int dotCount = 0;
    uint32_t dotMs = 0;
    bool prevEnabled = false, nextEnabled = false;
    bool hasPrevParams = false;
    struct Params {
        std::string name, minL, maxL;
        int invType = 0, cls = 0, sub = 0;
        bool usable = false;
        int quality = -1;
    } prevParams;
    uint32_t browseBid = 0;  // BrowseBidPrice
    bool browseBidEnabled = false, browseBuyoutEnabled = false;
    // Bids.
    int bidOffset = 0;
    uint32_t bidBid = 0;     // BidBidPrice
    bool bidBidEnabled = false, bidBuyoutEnabled = false;
    // Auctions.
    int aucOffset = 0;
    uint32_t cancelPrice = 0;  // AuctionFrameAuctions.cancelPrice
    bool cancelEnabled = false;
    // The sell item (AuctionsItemButton after NEW_AUCTION_UPDATE).
    bool sellItem = false;
    uint32_t sellEntry = 0, sellDisplay = 0, sellQuality = 1, sellCount = 0, sellPrice = 0, sellUnit = 0,
             sellStack = 0, sellTotal = 0;
    std::string sellName;
    bool sellCanUse = true;
    uint32_t stackSize = 0, numStacks = 0;  // AuctionsStackSizeEntry / AuctionsNumStacksEntry (0 = empty)
    uint32_t startPrice = 0, buyoutSell = 0;  // StartPrice / BuyoutPrice
    bool createEnabled = false, buyoutError = false;
    uint32_t depositCopper = 0;
    // AuctionProgressFrame / AuctionsBlockFrame.
    bool progressShown = false, blockShown = false, fadeOut = false;
    int progressDone = 0, progressTotal = 0;
    uint32_t progressDisplay = 0;  // AuctionProgressBarIcon (the sell item's at MULTISELL_START)
    bool openedBags = false;       // PSP: the backpack came with the AUCTIONS tab
    float progressAlpha = 1.0f;
    // Popups, the dropdown list, the keyboard.
    uint8_t popup = kPopupNone;
    uint8_t dropDown = kDdNone;
    Rect ddBox{};              // the box the list hangs under (screen coordinates)
    uint32_t oskField = 0;
    // Tooltips of this frame (after all the windows).
    const char* tipText = nullptr;  // a one-line GameTooltip:SetText
    uint32_t tipColor = 0xFFFFFFFFu;
    Rect tipRect{};
    uint32_t lastVersion = ~0u;
    uint32_t logs = 0;

    void releaseArt() {
        for (pp::Texture* t : {&frame, &colTab, &arrow, &nameFr, &rowHl, &tabHl, &filterBg, &filterLines, &input, &scroll,
                               &post, &alert, &alertGear, &cbUp, &cbHl, &cbCheck, &ddBtn, &ddBtnDn, &ddHl, &ddBg,
                               &ddCheck, &prev, &prevDis, &next, &nextDis, &castBorder, &bar, &slotHl})
            if (t->pixels) pp::textureRelease(*t);
        frameTab = 0;
        smallTried = false;
        disabledArtTried = false;
    }
    uint32_t artBytes() const {
        uint32_t n = 0;
        for (const pp::Texture* t : {&frame, &colTab, &arrow, &nameFr, &rowHl, &tabHl, &filterBg, &filterLines, &input,
                                     &scroll, &post, &alert, &alertGear, &cbUp, &cbHl, &cbCheck, &ddBtn, &ddBtnDn, &ddHl,
                                     &ddBg, &ddCheck, &prev, &prevDis, &next, &nextDis, &castBorder, &bar, &slotHl})
            if (t->pixels) n += pp::textureBytes(*t);
        return n;
    }
    uint32_t depositRate(uint32_t house) const {
        for (const auto& d : deposit)
            if (d.first == house) return d.second;
        return 0;
    }
    ~AuctionUi() { releaseArt(); }
};

void WorldScreen::destroyAuctionUi(AuctionUi* p) { delete p; }

WorldScreen::AuctionUi& WorldScreen::auctionUi() {
    if (!auc_) auc_ = std::unique_ptr<AuctionUi, void (*)(AuctionUi*)>(new AuctionUi(), &destroyAuctionUi);
    return *auc_;
}

bool WorldScreen::auctionItemLocked(uint8_t bag, uint8_t slot) const {
    return session_.isAuctionHouseOpen() && session_.auctionItemLocked(bag, slot);
}

bool WorldScreen::auctionKeepsPopupArt() const {
    return windowOpen(kWinAuctionPopup) || windowOpen(kWinAuctionDisabled);
}

// --- Events (the session -> the frame) ---------------------------------------------------------------------------

void WorldScreen::updateAuction() {
    PROF_SCOPE("auction");
    aucTipBid_ = aucTipBuyout_ = 0;
    aucTipRow_ = false;
    session_.auctionUpdate(glm::vec3(playerX_, playerY_, playerZ_));
    if (!auc_ && !session_.isAuctionHouseOpen()) {
        pp::WorldSession::AuctionEvent ev;
        while (session_.takeAuctionEvent(ev)) {
            if (std::strcmp(ev.name, "AUCTION_HOUSE_DISABLED") == 0) {
                AuctionUi& d = auctionUi();
                openWindow(kWinAuctionDisabled);  // StaticPopup_Show("AUCTION_HOUSE_DISABLED")
                winFocus_.setFocus(kIdDisabledOk);
                if (!uiMode_) {  // the cursor comes up for the dialog (its button)
                    setUiMode(true);
                    d.raisedCursor = true;
                    suppress_ = ~0u;
                }
                break;
            }
        }
        if (!auc_) return;
    }
    AuctionUi& u = auctionUi();
    // The default sorts (AuctionFrameBrowse_OnLoad / Bid_OnLoad / Auctions_OnLoad, once: the tables stay).
    auto setSort = [&](int list, const char* column, bool opposite) {
        for (const SortPreset& p : kPresets) {
            if (std::strcmp(p.table, tableName(list)) != 0 || std::strcmp(p.column, column) != 0) continue;
            session_.sortAuctionClearSort(list);
            for (int i = 0; i < p.n; ++i) {
                uint8_t c = 0;
                if (a::columnByName(p.rows[i].column, c))
                    session_.sortAuctionSetSort(list, c, opposite ? !p.rows[i].reverse : p.rows[i].reverse);
            }
            return;
        }
    };
    if (session_.auctionSort(a::kList).count() == 0) setSort(a::kList, "quality", false);
    if (session_.auctionSort(a::kBidder).count() == 0) setSort(a::kBidder, "duration", false);
    if (session_.auctionSort(a::kOwner).count() == 0) setSort(a::kOwner, "duration", false);

    pp::WorldSession::AuctionEvent ev;
    while (session_.takeAuctionEvent(ev)) {
        const std::string n = ev.name;
        if (n == "AUCTION_HOUSE_SHOW") {
            // AuctionFrame_Show.
            if (!u.shown) {
                u.shown = true;
                u.guard = true;
                if (!uiMode_) {
                    setUiMode(true);
                    u.raisedCursor = true;
                    suppress_ = ~0u;
                }
                openWindow(kWinAuction);
                u.page = 0;
                u.browseOffset = 0;
                u.bidOffset = 0;
                u.aucOffset = 0;
                session_.getBidderAuctionItems(0);
                session_.getOwnerAuctionItems();
                u.prevEnabled = u.nextEnabled = false;
                // AuctionFrame_OnShow: the Browse tab, the portrait, BROWSE_SEARCH_TEXT, the sound.
                u.gotAuctions = u.gotBids = false;
                u.tab = 1;
                u.type = a::kList;
                u.searching = false;
                u.searchedOnce = false;
                for (int& s : u.sel) s = 0;
                playUiSound(UiSound::AuctionHouseOpen);
                winFocus_.setFocus(kIdBrowseName);
                PSP_LOGI(pp::tag::Ctrl, "auction_open house=%u", static_cast<unsigned>(session_.auctionHouseId()));
            }
        } else if (n == "AUCTION_HOUSE_CLOSED") {
            closeWindow(kWinAuction);  // AuctionFrame_Hide: OnHide below (syncing the window)
        } else if (n == "AUCTION_HOUSE_DISABLED") {
            openWindow(kWinAuctionDisabled);
            winFocus_.setFocus(kIdDisabledOk);
            if (!uiMode_) {
                setUiMode(true);
                u.raisedCursor = true;
                suppress_ = ~0u;
            }
        } else if (n == "AUCTION_ITEM_LIST_UPDATE") {
            // AuctionFrameBrowse_OnEvent: the list, not searching any more, BROWSE_NO_RESULTS.
            u.searching = false;
            u.searchedOnce = true;
            auctionRefresh(a::kList);
        } else if (n == "AUCTION_BIDDER_LIST_UPDATE") {
            auctionRefresh(a::kBidder);
        } else if (n == "NEW_AUCTION_UPDATE") {
            // AuctionSellItemButton_OnEvent.
            game::ItemDef it;
            const bool has = session_.auctionSellItemDef(it);
            const auto* info = has ? session_.getItemInfo(it.itemId) : nullptr;
            u.sellItem = has;
            u.sellEntry = has ? it.itemId : 0;
            u.sellDisplay = has ? it.displayInfoId : 0;
            u.sellName = has ? it.name : std::string();
            u.sellQuality = has ? static_cast<uint32_t>(it.quality) : 1u;
            u.sellCount = has ? it.stackCount : 0;
            u.sellUnit = info && info->valid ? info->sellPrice : (has ? it.sellPrice : 0);
            u.sellPrice = u.sellUnit * u.sellCount;
            u.sellStack = info && info->valid ? static_cast<uint32_t>(std::max<int32_t>(1, info->maxStack)) : (has ? it.maxStack : 0);
            u.sellTotal = has ? session_.auctionSellTotalCount() : 0;
            u.sellCanUse = true;
            if (u.sellTotal == 1) aucKeep_.priceType = static_cast<int>(kPriceUnit);  // UIDropDownMenu_SetSelectedValue
            u.stackSize = u.sellCount;
            u.numStacks = 1;
            if (has && u.sellName == aucKeep_.lastItem && u.sellCount == aucKeep_.lastCount) {
                u.startPrice = aucKeep_.lastStartBid;
                u.buyoutSell = aucKeep_.lastBuyout;
            } else {
                const uint32_t base = aucKeep_.priceType == static_cast<int>(kPriceUnit) && u.sellStack > 0 ? u.sellUnit
                                                                                                             : u.sellPrice;
                u.startPrice = std::max<uint32_t>(100u, static_cast<uint32_t>(std::floor(base * 1.5)));
                u.buyoutSell = 0;
                if (has) {
                    aucKeep_.lastItem = u.sellName;
                    aucKeep_.lastCount = u.sellCount;
                    aucKeep_.lastStartBid = u.startPrice;
                    aucKeep_.lastBuyout = u.buyoutSell;
                }
            }
        } else if (n == "AUCTION_MULTISELL_START") {
            u.createEnabled = false;
            u.blockShown = true;
            u.progressShown = true;
            u.fadeOut = false;
            u.progressAlpha = 1.0f;
            u.progressDone = 0;
            u.progressTotal = ev.a;
            u.progressDisplay = u.sellDisplay;
        } else if (n == "AUCTION_MULTISELL_UPDATE") {
            u.progressDone = ev.a;
            u.progressTotal = ev.b;
            if (ev.a == ev.b) {
                u.blockShown = false;
                u.fadeOut = true;
            }
        } else if (n == "AUCTION_MULTISELL_FAILURE") {
            u.blockShown = false;
            u.progressShown = false;
        }
    }
    // PSP: the sell item moved out of its bag slot (the PSP's bags do not lock it): the slot lets it go.
    if (u.sellItem) {
        game::ItemDef it;
        if (!session_.auctionSellItemDef(it)) session_.clearAuctionSellItem();
    }
    // The frame taken off the stack (its close buttons, START...): AuctionFrame OnHide.
    if (u.shown && !windowOpen(kWinAuction)) {
        u.shown = false;
        playUiSound(UiSound::AuctionHouseClose);
        if (u.popup == kPopupBuyout || u.popup == kPopupCancel) {  // StaticPopup_Hide("BUYOUT_AUCTION" / "CANCEL_AUCTION")
            u.popup = kPopupNone;
            closeWindow(kWinAuctionPopup);
        }
        closeWindow(kWinAuctionDropDown);
        u.dropDown = kDdNone;
        u.progressShown = u.blockShown = false;
        if (u.openedBags && windowOpen(kWinBags)) closeWindow(kWinBags);
        u.openedBags = false;
        session_.closeAuctionHouse();  // SetAuctionsTabShowing(false), CloseAuctionHouse
        const uint32_t kb = u.artBytes() / 1024u;
        u.releaseArt();
        PSP_LOGI(pp::tag::Ctrl, "auction_close kb=%u", static_cast<unsigned>(kb));
    }
    // The keyboard's text for its box.
    {
        bool accepted = false;
        std::string text;
        if (pp::oskFinishedFor(kOskOwner, accepted, text)) {
            PSP_LOGI(pp::tag::Ctrl, "auction_osk field=%X ok=%d len=%u", static_cast<unsigned>(u.oskField),
                     accepted ? 1 : 0, static_cast<unsigned>(text.size()));
            if (accepted && u.shown) {
                auto money = [&](uint32_t& copper, int part) {
                    uint32_t g = copper / 10000u, s = (copper / 100u) % 100u, c = copper % 100u;
                    if (part == 0) g = digitsOf(text, 999999);
                    else if (part == 1) s = digitsOf(text, 99);
                    else c = digitsOf(text, 99);
                    // PSP: Lua keeps the sum as a double; 999999 g does not fit 32 bits - held at the top instead
                    // (above MAXIMUM_BID_PRICE, so the checks refuse it as they would there).
                    const uint64_t sum = uint64_t{g} * 10000u + s * 100u + c;
                    copper = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(sum);
                };
                switch (u.oskField) {
                    case kIdBrowseName: aucKeep_.name = text; break;  // the keyboard's 63 letters
                    case kIdMinLevel: aucKeep_.minLevel = std::to_string(digitsOf(text, 99)); break;
                    case kIdMaxLevel: aucKeep_.maxLevel = std::to_string(digitsOf(text, 99)); break;
                    case kIdBrowseBidG: money(u.browseBid, 0); break;
                    case kIdBrowseBidS: money(u.browseBid, 1); break;
                    case kIdBrowseBidC: money(u.browseBid, 2); break;
                    case kIdBidG: money(u.bidBid, 0); break;
                    case kIdBidS: money(u.bidBid, 1); break;
                    case kIdBidC: money(u.bidBid, 2); break;
                    case kIdStartG: money(u.startPrice, 0); break;
                    case kIdStartS: money(u.startPrice, 1); break;
                    case kIdStartC: money(u.startPrice, 2); break;
                    case kIdBuyoutG: money(u.buyoutSell, 0); break;
                    case kIdBuyoutS: money(u.buyoutSell, 1); break;
                    case kIdBuyoutC: money(u.buyoutSell, 2); break;
                    case kIdStackSize: u.stackSize = digitsOf(text, 9999); break;
                    case kIdNumStacks: u.numStacks = digitsOf(text, 9999); break;
                    default: break;
                }
                if (aucKeep_.minLevel == "0") aucKeep_.minLevel.clear();
                if (aucKeep_.maxLevel == "0") aucKeep_.maxLevel.clear();
            }
            u.oskField = 0;
        }
    }
    // The cursor came up for the frame: down again once it and every other window are gone.
    if (u.raisedCursor) {
        if (!uiMode_) {
            u.raisedCursor = false;
        } else if (!u.shown && windows_.empty() && !settings_.isOpen()) {
            setUiMode(false);
            u.raisedCursor = false;
            suppress_ = ~0u;
        }
    }
    // Nothing of it open: the state goes (the art already went with the frame).
    if (!u.shown && !session_.isAuctionHouseOpen() && !windowOpen(kWinAuctionPopup) &&
        !windowOpen(kWinAuctionDisabled) && !u.raisedCursor) {
        auc_.reset();
    }
}

// --- Drawing helpers ---------------------------------------------------------------------------------------------

namespace {

// The money of a SmallMoneyFrame in a font (gold / silver / copper and their coins); right-aligned at x when
// `right`. Returns the width. MoneyFrame_Update with collapse: "STATIC" (the rows, the deposit) shows the coins above
// zero and nothing for 0; `backpack` = "PLAYER" (showSmallerCoins "Backpack"): the lower coins under the highest, 0 c.
float coins(UiRenderer& ui, const Font& f, const pp::Texture& money, float x, float y, uint32_t copper, bool right,
            uint32_t col, uint32_t alpha = 0xFF, bool backpack = false) {
    const uint32_t parts[3] = {copper / 10000u, (copper / 100u) % 100u, copper % 100u};
    char txt[3][12] = {};
    const float icon = static_cast<float>(std::min(10, f.lineHeight()));
    float w = 0.0f;
    for (int p = 0; p < 3; ++p) {
        const bool lower = backpack && ((p >= 1 && parts[0] > 0) || (p == 2 && parts[1] > 0) || (p == 2 && copper == 0));
        if (parts[p] == 0 && !lower) continue;
        std::snprintf(txt[p], sizeof(txt[p]), "%u", static_cast<unsigned>(parts[p]));
        w += static_cast<float>(f.measure(txt[p])) + 1.0f + icon + 2.0f;
    }
    float cx = right ? x - w : x;
    const uint32_t c = (col & 0x00FFFFFFu) | (alpha << 24);
    for (int p = 0; p < 3; ++p) {
        if (!txt[p][0]) continue;
        ui.textShadow(f, cx, y, txt[p], c);
        cx += static_cast<float>(f.measure(txt[p])) + 1.0f;
        if (money.pixels) {
            const float uw = money.uw * 0.25f;
            ui.image(money, Rect{cx, y + (static_cast<float>(f.lineHeight()) - icon) * 0.5f, icon, icon},
                     SrcRect{uw * static_cast<float>(p), 0, uw, static_cast<float>(money.uh)}, 0x00FFFFFFu | (alpha << 24));
        }
        cx += icon + 2.0f;
    }
    return w;
}

// Three slices of a texture across r: caps of `capSrc` texels left / right drawn `capPx` wide.
void slices(UiRenderer& ui, const pp::Texture& t, const Rect& r, float capSrcL, float capSrcR, float capPxL,
            float capPxR, uint32_t col = 0xFFFFFFFFu, Blend blend = Blend::Alpha) {
    if (!t.pixels) return;
    const float tw = t.uw, th = t.uh;
    ui.image(t, Rect{r.x, r.y, capPxL, r.h}, SrcRect{0, 0, capSrcL, th}, col, blend);
    ui.image(t, Rect{r.x + capPxL, r.y, std::max(0.0f, r.w - capPxL - capPxR), r.h},
             SrcRect{capSrcL, 0, tw - capSrcL - capSrcR, th}, col, blend);
    ui.image(t, Rect{r.x + r.w - capPxR, r.y, capPxR, r.h}, SrcRect{tw - capSrcR, 0, capSrcR, th}, col, blend);
}

}  // namespace

// --- The frame ---------------------------------------------------------------------------------------------------

bool WorldScreen::drawAuctionWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinAuction && w != kWinAuctionPopup && w != kWinAuctionDropDown && w != kWinAuctionDisabled) return false;
    AuctionUi& u = auctionUi();
    const uint32_t fid = winFocus_.focused();
    if (fid >= 0x8200 && fid <= 0x83FF && winFocus_.isPressed(fid)) u.guard = false;
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight()), th = static_cast<float>(ft.lineHeight());
    loadNpcArt(false);  // the panel buttons, the close button, the coins, the scroll arrows

    if (w == kWinAuctionDisabled) {  // StaticPopup AUCTION_HOUSE_DISABLED (OKAY, showAlertGear)
        // its icon loaded here - the frame's art (below) is loaded only with AuctionFrame, which this
        // popup comes without (was: no icon). Once per AuctionUi art (releaseArt clears the flag).
        if (!u.disabledArtTried) {
            u.disabledArtTried = true;
            char path[256];
            pp::fsPath(path, sizeof(path), "data/ui.pak");
            pp::Pak pak;
            if (pak.open(path)) {
                loadTex(pak, "auc_alertgear", u.alertGear);           // UI-Dialog-Icon-AlertOther
                if (!u.alertGear.pixels) loadTex(pak, "mail_alert", u.alert);  // UI-Dialog-Icon-AlertNew (fallback)
                pak.close();
            }
            PSP_LOGI(pp::tag::Gfx, "auction_disabled_art gear=%d alert=%d kb=%u", u.alertGear.pixels ? 1 : 0,
                     u.alert.pixels ? 1 : 0, static_cast<unsigned>(u.artBytes() / 1024u));
        }
        StaticPopup p;
        p.text = CLIENT_STR(ERR_AUCTION_HOUSE_DISABLED);
        p.button1 = CLIENT_STR(OKAY);
        p.button1Id = kIdDisabledOk;
        p.focusable = focusable;
        p.alert = u.alertGear.pixels ? &u.alertGear : (u.alert.pixels ? &u.alert : nullptr);
        drawStaticPopup(ui, p);
        return true;
    }
    if (w == kWinAuctionPopup) {  // BUYOUT_AUCTION / BID_AUCTION / CANCEL_AUCTION (showAlert, the money frame)
        StaticPopup p;
        p.button1 = CLIENT_STR(ACCEPT);
        p.button1Id = kIdPopupAccept;
        p.button2 = CLIENT_STR(CANCEL);
        p.button2Id = kIdPopupCancel;
        p.hasMoneyFrame = true;
        p.focusable = focusable;
        p.alert = u.alert.pixels ? &u.alert : nullptr;
        if (u.popup == kPopupBuyout) {
            p.text = CLIENT_STR(BUYOUT_AUCTION_CONFIRMATION);
            p.money = u.buyoutPrice;
        } else if (u.popup == kPopupBid) {
            p.text = CLIENT_STR(BID_AUCTION_CONFIRMATION);
            p.money = u.browseBid;
        } else {
            p.text = u.cancelPrice > 0 ? CLIENT_STR(CANCEL_AUCTION_CONFIRMATION_MONEY) : CLIENT_STR(CANCEL_AUCTION_CONFIRMATION);
            p.money = u.cancelPrice;
        }
        drawStaticPopup(ui, p);
        return true;
    }
    if (w == kWinAuctionDropDown) {  // DropDownList1 under the open box
        std::vector<std::string> items;
        int checked = -1;
        if (u.dropDown == kDdQuality) {  // BrowseDropDown_Initialize: ALL (-1), ITEM_QUALITY0..5_DESC
            items.push_back(CLIENT_STR(ALL));
            for (int q = 0; q < 6; ++q) items.push_back(qualityDesc(q));
            checked = aucKeep_.quality + 1;
        } else if (u.dropDown == kDdPrice) {  // PriceDropDown_Initialize
            items = {CLIENT_STR(AUCTION_PRICE_PER_ITEM), CLIENT_STR(AUCTION_PRICE_PER_STACK)};
            checked = aucKeep_.priceType - 1;
        } else if (u.dropDown == kDdDuration) {  // DurationDropDown_Initialize
            for (int d = 1; d <= 3; ++d) items.push_back(durationText(d));
            checked = aucKeep_.duration - 1;
        } else {
            closeWindow(kWinAuctionDropDown);
            return true;
        }
        constexpr float kRowH = 12.0f, kPad = 7.0f, kCheckW = 12.0f;
        float tw = 0.0f;
        for (const auto& s : items) tw = std::max(tw, static_cast<float>(fs.measure(s.c_str())));
        Rect r{u.ddBox.x, u.ddBox.y + u.ddBox.h, tw + kCheckW + 6.0f + 2.0f * kPad,
               kRowH * static_cast<float>(items.size()) + 2.0f * kPad};
        r.x = std::clamp(r.x, 2.0f, static_cast<float>(pp::kScreenW) - 2.0f - r.w);
        if (r.y + r.h > pp::kScreenH - 2.0f) r.y = u.ddBox.y - r.h;  // PSP: above the box when it does not fit under
        if (u.ddBg.pixels) ui.tile(u.ddBg, Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xFFFFFFFFu);
        else ui.fill(Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xE0000000u);
        if (kit_.has(kTexDlgBorder)) ui.nineSlice(kit_.tex(kTexDlgBorder), r, 0.75f);
        for (size_t k = 0; k < items.size(); ++k) {
            const Rect ir{r.x + kPad, r.y + kPad + static_cast<float>(k) * kRowH, r.w - 2 * kPad, kRowH};
            const bool f = focusable && winFocus_.addStop(kIdDdItem0 + static_cast<uint32_t>(k), ir);
            if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, ir, 0xFFFFFFFFu, Blend::Add);
            if (static_cast<int>(k) == checked) {
                if (u.ddCheck.pixels) ui.image(u.ddCheck, Rect{ir.x, ir.y + 1.0f, 10.0f, 10.0f});
                else ui.textShadow(fs, ir.x + 1.0f, ir.y, "*", kColGold);
            }
            ui.textShadow(fs, ir.x + kCheckW, ir.y + (kRowH - lh) * 0.5f, items[k].c_str(), kColWhite);
        }
        return true;
    }

    // --- AuctionFrame ---
    PROF_SCOPE("auction.frame");
    // A dropdown list / a popup taken off the stack by START (StaticPopup's hideOnEscape, CloseDropDownMenus).
    if (u.dropDown != kDdNone && !windowOpen(kWinAuctionDropDown)) u.dropDown = kDdNone;
    if (u.popup != kPopupNone && !windowOpen(kWinAuctionPopup)) u.popup = kPopupNone;
    // The art: the shown tab's frame (one at a time), the small parts, the data.
    if (u.frameTab != u.tab || !u.smallTried) {
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            if (u.frameTab != u.tab) {
                if (u.frame.pixels) pp::textureRelease(u.frame);
                loadTex(pak, u.tab == 1 ? "auc_browse" : u.tab == 2 ? "auc_bid" : "auc_auctions", u.frame);
            }
            if (!u.smallTried) {
                loadTex(pak, "auc_coltab", u.colTab);
                loadTex(pak, "auc_arrow", u.arrow);
                loadTex(pak, "auc_namefr", u.nameFr);
                loadTex(pak, "auc_rowhl", u.rowHl);
                loadTex(pak, "auc_tabhl", u.tabHl);
                loadTex(pak, "auc_filterbg", u.filterBg);
                loadTex(pak, "auc_flines", u.filterLines);
                loadTex(pak, "auc_input", u.input);
                loadTex(pak, "auc_scroll", u.scroll);
                loadTex(pak, "auc_post", u.post);
                loadTex(pak, "mail_alert", u.alert);
                loadTex(pak, "cb_up", u.cbUp);
                loadTex(pak, "cb_hl", u.cbHl);
                loadTex(pak, "ql_check", u.cbCheck);
                loadTex(pak, "wm_dd_btn", u.ddBtn);
                loadTex(pak, "wm_dd_btn_dn", u.ddBtnDn);
                loadTex(pak, "wm_dd_hl", u.ddHl);
                loadTex(pak, "wm_dd_bg", u.ddBg);
                loadTex(pak, "wm_dd_check", u.ddCheck);
                loadTex(pak, "sb_prev", u.prev);
                loadTex(pak, "sb_prev_dis", u.prevDis);
                loadTex(pak, "sb_next", u.next);
                loadTex(pak, "sb_next_dis", u.nextDis);
                loadTex(pak, "cast_border_s", u.castBorder);
                loadTex(pak, "uf_bar", u.bar);
                loadTex(pak, "ab_hl", u.slotHl);
            }
            if (!u.dataTried) {  // auc_data: "H\thouse\trate\tcut", "C\tclass\tname", "S\tsub\tslots\tname"
                u.dataTried = true;
                const pp::PakEntry* e = pak.find("auc_data");
                if (char* raw = e ? static_cast<char*>(pak.load(e)) : nullptr) {
                    const std::string all(raw, e->size);
                    pp::pakFree(raw);
                    size_t at = 0;
                    while (at < all.size()) {
                        const size_t nl = all.find('\n', at);
                        const std::string line = all.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                        at = nl == std::string::npos ? all.size() : nl + 1;
                        if (line.size() < 3 || line[1] != '\t') continue;
                        const char* f1 = line.c_str() + 2;
                        const char* t2 = std::strchr(f1, '\t');
                        if (!t2) continue;
                        if (line[0] == 'H') {
                            u.deposit.emplace_back(static_cast<uint32_t>(std::atoi(f1)), static_cast<uint32_t>(std::atoi(t2 + 1)));
                        } else if (line[0] == 'C') {
                            AuctionUi::Cls c;
                            c.id = static_cast<uint32_t>(std::atoi(f1));
                            c.name = t2 + 1;
                            u.classes.push_back(std::move(c));
                        } else if (line[0] == 'S' && !u.classes.empty()) {
                            const char* t3 = std::strchr(t2 + 1, '\t');
                            if (!t3) continue;
                            AuctionUi::Sub s;
                            s.id = static_cast<uint32_t>(std::atoi(f1));
                            // the second field (the DBC's slot-filter flag) is not used: the slot rows are upstream's
                            s.name = t3 + 1;
                            u.classes.back().subs.push_back(std::move(s));
                        }
                    }
                }
            }
            if (!u.alertGear.pixels) {  // UI-Dialog-Icon-AlertOther (AUCTION_HOUSE_DISABLED); optional
                const pp::PakEntry* e = pak.find("auc_alertgear");
                void* blob = e ? pak.load(e) : nullptr;
                if (blob && !pp::textureFromBlob(blob, e->size, u.alertGear, true)) pp::pakFree(blob);
            }
            pak.close();
        }
        if (!tabArtTried_) {  // CharacterFrameTabButtonTemplate (drawFrameTab's art)
            tabArtTried_ = true;
            char p2[256];
            pp::fsPath(p2, sizeof(p2), "data/ui.pak");
            pp::Pak pk;
            if (pk.open(p2)) {
                loadTex(pk, "tab_active", tabArt_[0]);
                loadTex(pk, "tab_inactive", tabArt_[1]);
                pk.close();
            }
        }
        u.frameTab = u.tab;
        u.smallTried = true;
        PSP_LOGI(pp::tag::Gfx, "auction_art tab=%d frame=%d classes=%u houses=%u kb=%u", u.tab, u.frame.pixels ? 1 : 0,
                 static_cast<unsigned>(u.classes.size()), static_cast<unsigned>(u.deposit.size()),
                 static_cast<unsigned>(u.artBytes() / 1024u));
    }

    const bool live = focusable && !windowOpen(kWinAuctionPopup);
    const uint64_t me = session_.playerGuid();
    const uint64_t money = session_.getMoneyCopper();
    const uint32_t playerLevel = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    auto seen = [&](uint32_t id) {
        if (winFocus_.isPressed(id)) u.guard = false;
    };
    auto stop = [&](uint32_t id, const Rect& r, bool enabled = true) {
        const bool f = live && enabled && winFocus_.addStop(id, r);
        if (f) seen(id);
        return f;
    };
    auto screenRect = [&](const Rect& r) { return Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h}; };
    auto setTip = [&](const char* text, uint32_t col, const Rect& r) {
        u.tipText = text;
        u.tipColor = col;
        u.tipRect = screenRect(r);
    };
    // GetAuctionItemInfo's canUse: AzerothCore's "usable" search filter (AuctionHouseSearcher.cpp
    // AuctionHouseUsablePlayerInfo::PlayerCanUseItem) - the item's weapon / armour skill (the proficiency masks
    // here), the class and race masks, the required skill and rank, the level, a recipe whose spell is known
    // already. PSP: the required spell is not in the item cache (not checked).
    auto canUse = [&](const game::ItemQueryResponseData& info) {
        // A recipe taught through spell 483 / 55884: the second spell is the one learnt.
        if ((info.spells[0].spellId == 483 || info.spells[0].spellId == 55884) && info.spells[1].spellId != 0 &&
            session_.hasKnownSpell(info.spells[1].spellId))
            return false;
        if (info.requiredLevel > playerLevel) return false;
        const uint32_t b0 = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        const uint32_t race = b0 & 0xFFu, cls = (b0 >> 8) & 0xFFu;
        if (race >= 1 && race <= 32 && info.allowableRace != 0 && !(info.allowableRace & (1u << (race - 1)))) return false;
        if (cls >= 1 && cls <= 32 && info.allowableClass != 0 && !(info.allowableClass & (1u << (cls - 1)))) return false;
        const uint32_t prof = info.itemClass == 2 ? session_.weaponProficiency()
                            : info.itemClass == 4 ? session_.armorProficiency() : 0u;
        if (prof != 0 && info.subClass < 32 && !(prof & (1u << info.subClass))) return false;
        if (info.requiredSkill != 0) {
            bool ok = false;
            for (const auto& s : session_.playerSkills())
                if (s.skillId == info.requiredSkill && s.value >= info.requiredSkillRank) ok = true;
            if (!ok) return false;
        }
        return true;
    };
    struct Info {  // GetAuctionItemInfo
        std::string name;
        uint32_t display = 0, quality = 1, count = 1;
        bool canUse = true;
        int level = 1;
        uint32_t minBid = 0, minIncrement = 0, buyout = 0, bid = 0;
        bool highBidder = false;
        const std::string* highBidderName = nullptr;  // the owner list: the bidder's name
        const std::string* owner = nullptr;
        uint32_t saleStatus = 0;
        bool known = false;
    };
    auto infoOf = [&](int list, const a::Item& e) {
        Info i;
        const auto* qi = session_.getItemInfo(e.itemEntry);
        if (qi && qi->valid) {
            i.known = true;
            i.name = qi->name;
            i.display = qi->displayInfoId;
            i.quality = qi->quality;
            i.canUse = canUse(*qi);
            i.level = std::max<int>(1, static_cast<int>(qi->requiredLevel));
        } else {
            session_.ensureItemInfo(e.itemEntry);
        }
        i.count = e.count;
        i.minBid = e.minBid;
        i.minIncrement = e.minIncrement;
        i.buyout = e.buyout;
        i.bid = e.bid;
        if (list == a::kOwner && e.owner == me) i.highBidderName = session_.auctionNameOf(e.bidder);
        else i.highBidder = e.bidder == me && me != 0;
        i.owner = session_.auctionNameOf(e.owner);
        i.saleStatus = e.saleStatus;
        return i;
    };
    const uint32_t now = pp::logUptimeMs();
    auto timeLeft = [&](const a::Item& e) {  // GetAuctionItemTimeLeft (1..4, a sold one: seconds)
        const int32_t left = static_cast<int32_t>(e.expireMs - now);
        if (e.saleStatus == 1) return static_cast<int>(std::max<int32_t>(0, left / 1000));
        return left <= 0 ? 1 : a::timeLeftBucket(static_cast<uint32_t>(left));
    };
    // The text of a cell: centred when it fits, else a ticker (PSP).
    auto cell = [&](const Font& f, const Rect& r, const char* text, uint32_t col, Align al = Align::Center) {
        if (!text || !text[0]) return;
        const float tw = static_cast<float>(f.measure(text));
        const float ty = r.y + (r.h - static_cast<float>(f.lineHeight())) * 0.5f;
        if (tw <= r.w) {
            const float tx = al == Align::Center ? r.x + r.w * 0.5f : al == Align::Right ? r.x + r.w : r.x;
            ui.textShadow(f, tx, ty, text, col, al);
        } else {
            ui.textMarquee(f, Rect{r.x, ty, r.w, static_cast<float>(f.lineHeight())}, text, col);
        }
    };
    auto panelButton = [&](uint32_t id, const Rect& r, const char* label, bool enabled) {
        drawPanelButton(ui, id, r, label, enabled, live);
        if (live && enabled && winFocus_.isPressed(id)) u.guard = false;
    };
    // InputBoxTemplate / MoneyInputFrame value boxes (their borders are baked): the text, the focus.
    auto textBox = [&](uint32_t id, const Rect& r, const std::string& text, bool enabled = true) {
        const Rect hit{r.x - 3 * kS, r.y - 2 * kS, r.w + 3 * kS, r.h + 4 * kS};
        const bool f = stop(id, hit, enabled);
        ui.pushClip(hit);
        ui.textShadow(fs, r.x, r.y + (r.h - lh) * 0.5f, text.c_str(), enabled ? kColWhite : kColGrayFont);
        ui.popClip();
        if (f) ui.fill(hit, 0x30FFFFFFu);
    };
    auto moneyInput = [&](uint32_t idG, float x, float y, uint32_t copper, bool enabled = true) {
        char v[16];
        std::snprintf(v, sizeof(v), "%u", static_cast<unsigned>(copper / 10000u));
        textBox(idG, R(x, y, 58, 20), copper / 10000u ? v : "", enabled);
        std::snprintf(v, sizeof(v), "%u", static_cast<unsigned>((copper / 100u) % 100u));
        textBox(idG + 1, R(x + 84, y, 26, 20), (copper / 100u) % 100u ? v : "", enabled);
        std::snprintf(v, sizeof(v), "%u", static_cast<unsigned>(copper % 100u));
        textBox(idG + 2, R(x + 130, y, 26, 20), copper % 100u ? v : "", enabled);
    };
    auto checkBox = [&](uint32_t id, const Rect& r, bool checked, const char* label) {  // UICheckButtonTemplate
        const bool f = stop(id, Rect{r.x, r.y, r.w + static_cast<float>(ft.measure(label)) + 4.0f, r.h});
        if (u.cbUp.pixels) ui.image(u.cbUp, r);
        else ui.fill(r, 0xFF202020u);
        if (checked && u.cbCheck.pixels) ui.image(u.cbCheck, r);
        if (f && u.cbHl.pixels) ui.image(u.cbHl, r, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(ft, r.x + r.w, r.y + (r.h - th) * 0.5f, label, kColWhite);  // GameFontHighlightSmall
        return f;
    };
    // UIDropDownMenuTemplate at frame (x, y) with a middle `mid` wide: the label, the text right before the button,
    // the button (24 x 24 at the Right texture's TOPRIGHT -16,-18). PSP: the whole visible box takes the click.
    auto dropDown = [&](uint32_t id, float x, float y, float mid, const char* text, bool enabled, uint8_t kind) {
        const float right = x + 25 + mid + 25;
        const Rect box = R(x + 10, y - 17 + 13, mid + 29, 38);  // the label frame's visible box (x 10..117 of 128)
        const Rect btn = R(right - 16 - 24, y - 17 + 18, 24, 24);
        const bool f = stop(id, box, enabled);
        const bool down = (f && winFocus_.isPressed(id)) || u.dropDown == kind;
        const pp::Texture& bt = down && u.ddBtnDn.pixels ? u.ddBtnDn : u.ddBtn;
        if (bt.pixels) ui.image(bt, btn, enabled ? 0xFFFFFFFFu : 0xFF808080u);
        if (f && u.ddHl.pixels) ui.image(u.ddHl, btn, 0xFFFFFFFFu, Blend::Add);
        const float tr = PX(right - 43);
        const float ty = PY(y - 17 + 32 - 2) - th * 0.5f;
        ui.pushClip(Rect{box.x, box.y, tr - box.x, box.h});
        ui.textShadow(ft, tr, ty, text, enabled ? kColWhite : kColGrayFont, Align::Right);
        ui.popClip();
        if (f) u.ddBox = screenRect(box);
    };
    // A FauxScrollFrame's bar (UI-Character-ScrollBar's top and bottom at 0.55) and its two arrows.
    auto scrollBar = [&](float x, float y0, float y1, uint32_t idUp, uint32_t idDown, bool canUp, bool canDown) {
        if (u.scroll.pixels) {
            const float topH = 141.0f, botH = 58.0f, h = PY(y1) - PY(y0);
            const float tw = u.scroll.uw;
            const float topPart = std::max(0.0f, std::min(topH, h - botH));
            ui.image(u.scroll, Rect{PX(x), PY(y0), 17, topPart}, SrcRect{0, 0, tw, topPart});
            ui.image(u.scroll, Rect{PX(x), PY(y1) - botH, 17, botH}, SrcRect{0, topH, tw, botH});
        }
        const Rect up{PX(x) + 3.5f, PY(y0) + 7.0f, 10, 10}, dn{PX(x) + 3.5f, PY(y1) - 17.0f, 10, 10};
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDown ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (stop(idUp, up, canUp)) ui.fill(up, 0x40FFFFFFu);
        if (stop(idDown, dn, canDown)) ui.fill(dn, 0x40FFFFFFu);
    };
    // AuctionSortButtonTemplate: WhoFrame-ColumnTabs in three slices, the label (GameFontHighlightSmall LEFT 8,0),
    // the arrow after it when it is the list's first column (SortButton_UpdateArrow), the tab highlight.
    auto sortButton = [&](uint32_t id, float x, float y, float wU, const char* label, int list, uint8_t column,
                          bool enabled) {
        const Rect r = R(x, y, wU, 19);
        slices(ui, u.colTab, r, 5, 4, 5 * kS, 4 * kS);
        const bool f = stop(id, r, enabled);
        ui.pushClip(r);
        ui.textShadow(ft, r.x + 8 * kS, r.y + (r.h - th) * 0.5f, label, kColWhite);
        const a::SortTable& t = session_.auctionSort(list);
        if (t.keys[0].used && t.keys[0].column == column && enabled && u.arrow.pixels) {
            const float ax = r.x + 8 * kS + static_cast<float>(ft.measure(label)) + 3 * kS;
            const Rect ar{ax, r.y + (r.h - 5.0f) * 0.5f + 1.0f, 5.0f, 4.5f};
            const float aw = u.arrow.uw, ah = u.arrow.uh * 0.5f;  // auc_arrow: the arrow, then it flipped
            ui.image(u.arrow, ar, SrcRect{0, t.keys[0].reverse ? ah : 0.0f, aw, ah});  // reversed: SetTexCoord(0, .5625, 1, 0)
        }
        ui.popClip();
        if (f) {
            if (u.tabHl.pixels) ui.image(u.tabHl, Rect{r.x, r.y - 2.5f * kS, r.w + 4 * kS, 24 * kS}, 0xFFFFFFFFu, Blend::Add);
            else ui.fill(r, 0x30FFFFFFu);
        }
    };
    // A row's frame (UI-AuctionItemNameFrame: Left at LEFT 34,2, Right at RIGHT 0,2, 32 high) and its highlight.
    auto rowFrame = [&](float x0, float y0, float wU, bool hl) {
        const Rect fr = R(x0 + 34, y0 + 18.5f - 16.0f - 2.0f, wU - 34, 32);
        slices(ui, u.nameFr, fr, 10, 10, 10 * kS, 10 * kS);
        if (hl) {
            const Rect hr = R(x0 + 33, y0, wU - 36, 32);  // $parentHighlight (589 / 562, 758 / 735, 565 / 543)
            if (u.rowHl.pixels) ui.image(u.rowHl, hr, 0xFFFFFFFFu, Blend::Add);
            else ui.fill(hr, 0x30FFFFFFu);
        }
    };
    // The money of a row: the bid (and the buyout under it, BUYOUT_COST).
    auto rowMoney = [&](float rightU, float y0, uint32_t bid, uint32_t buyout, float buyoutLeftU, const char* buyLabel,
                        uint32_t bidAlpha, bool yourBid) {
        const float rowTop = PY(y0), rowH = kRowU * kS;
        if (buyout > 0) {
            const float by = rowTop + 1.0f;
            const float bw = coins(ui, fs, npcArt_.money, PX(rightU), by, bid, true, kColWhite, bidAlpha);
            if (yourBid) ui.textShadow(ft, PX(rightU) - bw - 5 * kS, by + (lh - th) * 0.5f, CLIENT_STR(YOUR_BID), kColGold, Align::Right);
            const float y2 = by + lh;
            coins(ui, ft, npcArt_.money, PX(rightU), y2, buyout, true, kColWhite);
            if (buyLabel) ui.textShadow(ft, PX(buyoutLeftU), y2, buyLabel, kColGold);  // GameFontNormalSmallLeft
        } else {
            const float by = rowTop + (rowH - lh) * 0.5f - 1.0f;
            const float bw = coins(ui, fs, npcArt_.money, PX(rightU), by, bid, true, kColWhite, bidAlpha);
            if (yourBid) ui.textShadow(ft, PX(rightU) - bw - 5 * kS, by + (lh - th) * 0.5f, CLIENT_STR(YOUR_BID), kColGold, Align::Right);
        }
    };
    // The row's name: the quality colour, the 10 px font when it fits, else a ticker.
    auto rowName = [&](const Rect& r, const std::string& name, uint32_t col, bool focused) {
        if (name.empty()) return;
        const float tw = static_cast<float>(fs.measure(name.c_str()));
        const float ty = r.y + (r.h - lh) * 0.5f;
        if (tw <= r.w) {
            ui.textShadow(fs, r.x, ty, name.c_str(), col);
        } else if (focused) {
            ui.textMarquee(fs, Rect{r.x, ty, r.w, lh}, name.c_str(), col);
        } else {
            ui.pushClip(r);
            ui.textShadow(ft, r.x, r.y + (r.h - th) * 0.5f, name.c_str(), col);
            ui.popClip();
        }
    };
    auto itemTip = [&](const a::Item& e, const Rect& r, uint32_t bidAmount, uint32_t buyout) {
        // AuctionFrameItem_OnEnter: GameTooltip:SetAuctionItem + the per-unit prices of a stack (after the windows).
        tipProtoItem_ = e.itemEntry;
        tipProtoCount_ = e.count;
        tipProtoRect_ = screenRect(r);
        aucTipRow_ = true;
        if (e.count > 1) {
            if (bidAmount > 0) aucTipBid_ = (bidAmount + e.count - 1) / e.count;
            if (buyout > 0) aucTipBuyout_ = (buyout + e.count - 1) / e.count;
        }
    };

    u.tipText = nullptr;
    // AuctionPortraitTexture (58 x 58 at 8,-7), round, under the frame.
    {
        const Rect pr = R(8, 7, 58, 58);
        if (const pp::Texture* face = portraitOf(session_.auctioneerGuid())) ui.imageCircle(*face, pr);
        else ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    }
    if (u.frame.pixels) ui.image(u.frame, Rect{kOx, kOy, kFrameW * kS, kFrameH * kS});
    else kit_.panel(ui, Rect{kOx, kOy, kFrameW * kS, kFrameH * kS});
    // AuctionFrameCloseButton (UIPanelCloseButton, TOPRIGHT 3,-8).
    {
        const Rect cr = R(803, 8, 32, 32);
        const bool f = stop(kIdClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                             : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // AuctionFrameTab1..3 (AuctionTabTemplate: TOPLEFT at the frame's BOTTOMLEFT 15,11; -8 apart).
    {
        const float h = 32 * kS, y = PY(kFrameH - 11);
        float x = PX(15);
        x += drawFrameTab(ui, kIdTab1, x, y, h, CLIENT_STR(BROWSE), u.tab == 1, live) - 8 * kS;
        x += drawFrameTab(ui, kIdTab2, x, y, h, CLIENT_STR(BIDS), u.tab == 2, live) - 8 * kS;
        drawFrameTab(ui, kIdTab3, x, y, h, CLIENT_STR(AUCTIONS), u.tab == 3, live);
    }
    // AuctionFrameMoneyFrame (SmallMoneyFrame, BOTTOMRIGHT at the frame's BOTTOMLEFT 181,20): our money.
    coins(ui, fs, npcArt_.money, PX(176), PY(420.5f) - lh * 0.5f, static_cast<uint32_t>(money), true, kColWhite, 0xFF,
          true);

    if (u.tab == 1) {
        // ============================ AuctionFrameBrowse ============================
        ui.textShadow(fs, PX(379), PY(23) - lh * 0.5f, CLIENT_STR(BROWSE_AUCTIONS), kColGold, Align::Center);
        ui.textShadow(ft, PX(80), PY(41), CLIENT_STR(NAME), kColWhite);
        ui.textShadow(ft, PX(230), PY(47) - th, CLIENT_STR(LEVEL_RANGE), kColWhite);
        textBox(kIdBrowseName, R(83, 53, 140, 16), aucKeep_.name);
        textBox(kIdMinLevel, R(233, 53, 25, 16), aucKeep_.minLevel);
        ui.textShadow(ft, PX(264), PY(61) - th * 0.5f, "-", kColWhite, Align::Center);              // BrowseLevelHyphen
        textBox(kIdMaxLevel, R(270, 53, 25, 16), aucKeep_.maxLevel);
        ui.textShadow(ft, PX(kDropBrowseX + 20), PY(48) - th, CLIENT_STR(RARITY), kColWhite);
        dropDown(kIdQualityDD, kDropBrowseX, 48, 115,
                 aucKeep_.quality < 0 ? CLIENT_STR(ALL) : qualityDesc(aucKeep_.quality), true, kDdQuality);
        checkBox(kIdUsable, R(470, 36, 24, 24), aucKeep_.usable, CLIENT_STR(USABLE_ITEMS));
        if (checkBox(kIdShowOnPlayer, R(470, 58, 24, 24), aucKeep_.showOnPlayer, CLIENT_STR(DISPLAY_ON_CHARACTER)))
            setTip(CLIENT_STR(DISPLAY_ON_CHAR_TOOLTIP), kColWhite, R(470, 58, 24, 24));
        // BrowseSearchButton_OnUpdate: CanSendAuctionQuery enables search, pages and sorts; the dots.
        const bool canQuery = session_.canSendAuctionQuery(a::kList);
        panelButton(kIdSearch, R(703, 39, 80, 22), CLIENT_STR(SEARCH), canQuery);
        // BrowseResetButton_OnUpdate.
        const bool resettable = !aucKeep_.name.empty() || !aucKeep_.minLevel.empty() || !aucKeep_.maxLevel.empty() ||
                                aucKeep_.usable || aucKeep_.quality != -1 || aucKeep_.classIndex != 0 ||
                                aucKeep_.subIndex != 0 || aucKeep_.invTypeIndex != 0;
        panelButton(kIdReset, R(78, 75, 80, 22), CLIENT_STR(RESET), resettable);
        {
            const bool pe = canQuery && u.prevEnabled, ne = canQuery && u.nextEnabled;
            const Rect pr = R(658, 53, 32, 32), nr = R(796, 53, 32, 32);
            const pp::Texture& pt = pe ? u.prev : u.prevDis;
            const pp::Texture& nt = ne ? u.next : u.nextDis;
            if (pt.pixels) ui.image(pt, pr);
            if (nt.pixels) ui.image(nt, nr);
            if (stop(kIdPrev, pr, pe)) ui.fill(pr, 0x40FFFFFFu);
            if (stop(kIdNext, nr, ne)) ui.fill(nr, 0x40FFFFFFu);
            ui.textShadow(ft, PX(690), PY(69) - th * 0.5f, CLIENT_STR(PREV), kColWhite);
            ui.textShadow(ft, PX(796), PY(69) - th * 0.5f, CLIENT_STR(NEXT), kColWhite, Align::Right);
        }

        // --- The filters (AuctionFrameFilters_UpdateClasses / SubClasses / InvTypes) ---
        struct FRow {
            uint8_t type;  // 0 class, 1 subclass, 2 invtype
            const char* text;
            int index;
            bool selected, isLast, shown;
        };
        std::vector<FRow> rows;
        for (size_t ci = 0; ci < u.classes.size() && ci < 12; ++ci) {
            const int ix = static_cast<int>(ci) + 1;
            const bool sel = aucKeep_.classIndex == ix;
            rows.push_back(FRow{0, u.classes[ci].name.c_str(), ix, sel, false, true});
            if (!sel || u.classes[ci].subs.size() < 2) continue;  // own rule: one subclass is not worth a row
            const auto& subs = u.classes[ci].subs;
            for (size_t si = 0; si < subs.size(); ++si) {
                const int sx = static_cast<int>(si) + 1;
                const bool ssel = aucKeep_.subIndex == sx;
                rows.push_back(FRow{1, subs[si].name.c_str(), sx, ssel, false, true});
                if (!ssel) continue;
                uint32_t slots[game::kNumAuctionSlots];
                const int ns = a::slotRows(u.classes[ci].id, subs[si].id, slots);
                for (int k = 0; k < ns; ++k)
                    rows.push_back(FRow{2, slotText(slots[k]), k + 1, aucKeep_.invTypeIndex == k + 1, k == ns - 1, true});
            }
        }
        const int nRows = static_cast<int>(rows.size());
        const uint32_t focused = winFocus_.focused();
        u.filterOffset = std::clamp(u.filterOffset, 0, std::max(0, nRows - kFilterRows));
        if (live && focused >= kIdFilter0 && focused < kIdFilter0 + kFilterRows) {  // PSP: the list follows the focus
            const int vi = static_cast<int>(focused - kIdFilter0);
            if (vi == kFilterRows - 1 && u.filterOffset + kFilterRows < nRows) {
                ++u.filterOffset;
                winFocus_.setFocus(focused - 1);
            } else if (vi == 0 && u.filterOffset > 0) {
                --u.filterOffset;
                winFocus_.setFocus(focused + 1);
            }
        }
        const float fw = nRows > kFilterRows ? 136.0f : 156.0f;
        int index = u.filterOffset;
        for (int i = 0; i < kFilterRows; ++i) {
            while (index < nRows && rows[static_cast<size_t>(index)].type == 2 && !rows[static_cast<size_t>(index)].shown)
                ++index;
            if (index >= nRows) break;
            const FRow& fr = rows[static_cast<size_t>(index)];
            ++index;
            const Rect r = R(23, 105 + kFilterU * static_cast<float>(i), fw, kFilterU);
            const uint32_t id = kIdFilter0 + static_cast<uint32_t>(i);
            const bool f = stop(id, r);
            // FilterButton_SetType: the background (class 1.0, subclass 0.4, invtype none), the lines, the text.
            if (fr.type != 2 && u.filterBg.pixels)
                ui.image(u.filterBg, r, fr.type == 0 ? 0xFFFFFFFFu : 0x66FFFFFFu);
            if (fr.type == 2 && u.filterLines.pixels) {
                const float half = u.filterLines.uw * 0.5f;
                ui.image(u.filterLines, R(23 + 13, 105 + kFilterU * static_cast<float>(i), 7, kFilterU),
                         SrcRect{fr.isLast ? half : 0.0f, 0, half, static_cast<float>(u.filterLines.uh)});
            }
            if ((fr.selected || f) && u.tabHl.pixels) ui.image(u.tabHl, r, 0xFFFFFFFFu, Blend::Add);  // LockHighlight
            const float tx = PX(23 + (fr.type == 0 ? 4.0f : fr.type == 1 ? 12.0f : 20.0f));
            const uint32_t col = fr.type != 0 || f || fr.selected ? kColWhite : kColGold;
            const Rect tr{tx, r.y, r.x + r.w - tx - 2, r.h};
            if (f && static_cast<float>(ft.measure(fr.text)) > tr.w) {
                ui.textMarquee(ft, Rect{tr.x, r.y + (r.h - th) * 0.5f, tr.w, th}, fr.text, col);
            } else {
                ui.pushClip(tr);
                ui.textShadow(ft, tx, r.y + (r.h - th) * 0.5f, fr.text, col);
                ui.popClip();
            }
        }
        if (nRows > kFilterRows)
            scrollBar(156, 100, 412, kIdFilterUp, kIdFilterDown, u.filterOffset > 0, u.filterOffset + kFilterRows < nRows);

        // --- AuctionFrameBrowse_Update ---
        const auto& list = session_.auctionItems(a::kList);
        const int numBatch = static_cast<int>(list.size());
        const uint32_t total = session_.auctionTotal(a::kList);
        const bool morePages = total > static_cast<uint32_t>(kPerPage);
        const int scrollRows = numBatch + (morePages ? 1 : 0);  // the extra row shows the count
        u.browseOffset = std::clamp(u.browseOffset, 0, std::max(0, scrollRows - kBrowseRows));
        if (live && focused >= kIdBrowseRow0 && focused < kIdBrowseRow0 + kBrowseRows) {
            const int vi = static_cast<int>(focused - kIdBrowseRow0);
            if (vi == kBrowseRows - 1 && u.browseOffset + kBrowseRows < scrollRows) {
                ++u.browseOffset;
                winFocus_.setFocus(focused - 1);
            } else if (vi == 0 && u.browseOffset > 0) {
                --u.browseOffset;
                winFocus_.setFocus(focused + 1);
            }
        }
        const bool scrolls = !(numBatch < kBrowseRows || (numBatch == kBrowseRows && total <= kBrowseRows));
        const float rowW = scrolls ? 600.0f : 625.0f, bidSortW = scrolls ? 184.0f : 207.0f;
        sortButton(kIdSortList0 + 0, 186, 82, 214, CLIENT_STR(RARITY), a::kList, a::kColQuality, canQuery);
        sortButton(kIdSortList0 + 1, 398, 82, 57, CLIENT_STR(LEVEL_ABBR), a::kList, a::kColLevel, canQuery);
        sortButton(kIdSortList0 + 2, 453, 82, 91, CLIENT_STR(CLOSES_IN), a::kList, a::kColDuration, canQuery);
        sortButton(kIdSortList0 + 3, 542, 82, 76, CLIENT_STR(AUCTION_CREATOR), a::kList, a::kColSeller, canQuery);
        sortButton(kIdSortList0 + 4, 616, 82, bidSortW, CLIENT_STR(CURRENT_BID), a::kList, a::kColBid, canQuery);
        if (numBatch == 0) {  // BrowseNoResultsText (+ the dots while searching)
            const char* t = u.searching      ? CLIENT_STR(SEARCHING_FOR_ITEMS)
                            : u.searchedOnce ? CLIENT_STR(BROWSE_NO_RESULTS)
                                             : CLIENT_STR(BROWSE_SEARCH_TEXT);
            const auto lines = wrapQuestText(fs, t, 540 * kS);
            float y = PY(120);
            for (const auto& l : lines) {
                ui.textShadow(fs, PX(494), y, l.c_str(), kColGold, Align::Center);
                y += lh;
            }
            if (u.searching) {
                if (now - u.dotMs > 300u) {
                    u.dotMs = now;
                    u.dotCount = (u.dotCount + 1) % 4;
                }
                static const char* const kDots[4] = {"", ".", "..", "..."};
                ui.textShadow(fs, PX(494) + static_cast<float>(fs.measure(t)) * 0.5f, PY(120), kDots[u.dotCount],
                              kColGold);
            }
        }
        bool isLastSlotEmpty = false;
        for (int i = 0; i < kBrowseRows; ++i) {
            const int idx = u.browseOffset + i;  // GetAuctionItemInfo("list", offset + i) (0-based here)
            if (idx >= numBatch) {
                if (i == kBrowseRows - 1) isLastSlotEmpty = true;
                continue;
            }
            const a::Item& e = list[static_cast<size_t>(idx)];
            const Info in = infoOf(a::kList, e);
            const float y0 = 110 + kRowU * static_cast<float>(i);
            const uint32_t id = kIdBrowseRow0 + static_cast<uint32_t>(i);
            const Rect rowR = R(195, y0, rowW - 25, kRowU);
            const bool f = stop(id, rowR);
            const bool selected = u.sel[a::kList] == idx + 1;
            rowFrame(195, y0, rowW, selected || f);
            const Rect icon = R(195, y0, 32, 32);
            drawItemButton(ui, icon, in.display, in.count > 1 ? in.count : 0, -1, in.canUse ? 0xFFFFFFFFu : kTintRed);
            rowName(R(195 + 43, y0, 167, 32), in.name, qualityColor(in.quality), f);
            char lv[8];
            std::snprintf(lv, sizeof(lv), "%d", in.level);
            cell(fs, R(195 + 205, y0, 52, 32), lv, in.level > static_cast<int>(playerLevel) ? kColRedFont : kColWhite);
            cell(ft, R(195 + 267, y0, 65, 32), timeLeftText(timeLeft(e)), kColWhite);
            cell(ft, R(195 + 345, y0, 78, 32), in.owner ? in.owner->c_str() : "", kColWhite);
            const uint32_t displayed = in.bid == 0 ? in.minBid : in.bid;
            const uint32_t required = in.bid == 0 ? in.minBid : in.bid + in.minIncrement;
            uint32_t buyout = in.buyout;
            if (required >= a::kMaximumBidPrice) buyout = required;  // "Lie about our buyout price"
            rowMoney(195 + rowW + 10 - 12, y0, displayed, buyout, 616, CLIENT_STR(BUYOUT_COST), 0xFF, in.highBidder);
            if (f) itemTip(e, icon, displayed, buyout);
        }
        if (morePages) {  // NUMBER_OF_RESULTS_TEMPLATE under the rows when the last row is empty
            if (isLastSlotEmpty) {
                const int itemsMin = u.page * kPerPage + 1, itemsMax = itemsMin + numBatch - 1;
                const std::string t = CLIENT_FMT(NUMBER_OF_RESULTS_TEMPLATE, itemsMin, itemsMax, static_cast<unsigned>(total));
                ui.textShadow(ft, PX(459), PY(394) - th, t.c_str(), kColWhite, Align::Center);
            }
        }
        if (scrolls)
            scrollBar(795, 100, 413, kIdBrowseUp, kIdBrowseDown, u.browseOffset > 0, u.browseOffset + kBrowseRows < scrollRows);
        // BrowseBidPrice, BID (BrowseBidText RIGHT at its LEFT -10), the buttons.
        ui.textShadow(ft, PX(316 - 10), PY(420) - th * 0.5f, CLIENT_STR(BID), kColWhite, Align::Right);
        moneyInput(kIdBrowseBidG, 316, 411, u.browseBid);
        panelButton(kIdBrowseBid, R(584, 411, 80, 22), CLIENT_STR(BID), u.browseBidEnabled);
        panelButton(kIdBrowseBuyout, R(664, 411, 80, 22), CLIENT_STR(BUYOUT), u.browseBuyoutEnabled);
        panelButton(kIdBrowseClose, R(744, 411, 80, 22), CLIENT_STR(CLOSE), true);
    } else if (u.tab == 2) {
        // ============================ AuctionFrameBid ============================
        ui.textShadow(fs, PX(379), PY(23) - lh * 0.5f, CLIENT_STR(BID), kColGold, Align::Center);  // BidTitle
        const auto& list = session_.auctionItems(a::kBidder);
        const int numBatch = static_cast<int>(list.size());
        const uint32_t total = session_.auctionTotal(a::kBidder);
        const bool more = total > static_cast<uint32_t>(kPerPage);
        const int scrollRows = numBatch + (more ? 1 : 0);
        const uint32_t focused = winFocus_.focused();
        u.bidOffset = std::clamp(u.bidOffset, 0, std::max(0, scrollRows - kBidRows));
        if (live && focused >= kIdBidRow0 && focused < kIdBidRow0 + kBidRows) {
            const int vi = static_cast<int>(focused - kIdBidRow0);
            if (vi == kBidRows - 1 && u.bidOffset + kBidRows < scrollRows) {
                ++u.bidOffset;
                winFocus_.setFocus(focused - 1);
            } else if (vi == 0 && u.bidOffset > 0) {
                --u.bidOffset;
                winFocus_.setFocus(focused + 1);
            }
        }
        const bool scrolls = !(numBatch < kBidRows || (numBatch == kBidRows && total <= kBidRows));
        const float rowW = scrolls ? 769.0f : 793.0f, bidSortW = scrolls ? 145.0f : 169.0f;
        sortButton(kIdSortBid0 + 0, 65, 52, 195, CLIENT_STR(RARITY), a::kBidder, a::kColQuality, true);
        sortButton(kIdSortBid0 + 1, 258, 52, 65, CLIENT_STR(LEVEL_ABBR), a::kBidder, a::kColLevel, true);
        sortButton(kIdSortBid0 + 2, 321, 52, 79, CLIENT_STR(CLOSES_IN), a::kBidder, a::kColDuration, true);
        sortButton(kIdSortBid0 + 3, 398, 52, 162, CLIENT_STR(BUYOUT_PRICE), a::kBidder, a::kColBuyout, true);
        sortButton(kIdSortBid0 + 4, 558, 52, 94, CLIENT_STR(BID_STATUS), a::kBidder, a::kColStatus, true);
        sortButton(kIdSortBid0 + 5, 650, 52, bidSortW, CLIENT_STR(CURRENT_BID), a::kBidder, a::kColBid, true);
        bool isLastSlotEmpty = false;
        for (int i = 0; i < kBidRows; ++i) {
            const int idx = u.bidOffset + i;
            if (idx >= numBatch) {
                isLastSlotEmpty = i == kBidRows - 1;
                continue;
            }
            const a::Item& e = list[static_cast<size_t>(idx)];
            const Info in = infoOf(a::kBidder, e);
            const float y0 = 76 + kRowU * static_cast<float>(i), x0 = 27;
            const uint32_t id = kIdBidRow0 + static_cast<uint32_t>(i);
            const Rect rowR = R(x0, y0, rowW - 25, kRowU);
            const bool f = stop(id, rowR);
            const bool selected = u.sel[a::kBidder] == idx + 1;
            rowFrame(x0, y0, rowW, selected || f);
            const Rect icon = R(x0, y0, 32, 32);
            drawItemButton(ui, icon, in.display, in.count > 1 ? in.count : 0, -1, in.canUse ? 0xFFFFFFFFu : kTintRed);
            rowName(R(x0 + 41, y0, 187, 32), in.name, qualityColor(in.quality), f);
            char lv[8];
            std::snprintf(lv, sizeof(lv), "%d", in.level);
            cell(fs, R(x0 + 250, y0, 32, 32), lv, in.level > static_cast<int>(playerLevel) ? kColRedFont : kColWhite);
            cell(ft, R(x0 + 295, y0, 75, 32), timeLeftText(timeLeft(e)), kColWhite);
            cell(ft, R(x0 + 543, y0, 75, 32), in.highBidder ? CLIENT_STR(HIGH_BIDDER) : CLIENT_STR(OUTBID),
                 in.highBidder ? kColGreenFont : kColRedFont);  // HIGH_BIDDER / OUTBID
            const uint32_t bidAmount = in.bid == 0 ? in.minBid : in.bid;
            coins(ui, fs, npcArt_.money, PX(x0 + 535), PY(y0) + (kRowU * kS - lh) * 0.5f, in.buyout, true, kColWhite);
            coins(ui, fs, npcArt_.money, PX(x0 + rowW + 2), PY(y0) + (kRowU * kS - lh) * 0.5f, bidAmount, true, kColWhite);
            if (f) itemTip(e, icon, bidAmount, in.buyout);
        }
        if (more && isLastSlotEmpty) {
            const std::string t = CLIENT_FMT(SINGLE_PAGE_RESULTS_TEMPLATE, static_cast<unsigned>(total));
            ui.textShadow(ft, PX(369), PY(394) - th, t.c_str(), kColWhite, Align::Center);
        }
        if (scrolls) scrollBar(796, 69, 411, kIdBidUp, kIdBidDown, u.bidOffset > 0, u.bidOffset + kBidRows < scrollRows);
        ui.textShadow(ft, PX(264), PY(426) - th, CLIENT_STR(BID), kColWhite, Align::Right);  // BidBidText
        moneyInput(kIdBidG, 276, 411, u.bidBid);
        panelButton(kIdBidBid, R(584, 411, 80, 22), CLIENT_STR(BID), u.bidBidEnabled);
        panelButton(kIdBidBuyout, R(664, 411, 80, 22), CLIENT_STR(BUYOUT), u.bidBuyoutEnabled);
        panelButton(kIdBidClose, R(744, 411, 80, 22), CLIENT_STR(CLOSE), true);
    } else {
        // ============================ AuctionFrameAuctions ============================
        {
            const std::string me2 = [&]() {
                const std::string* n = session_.nameOf(me);
                return n ? *n : std::string();
            }();
            // AUCTION_TITLE (ruRU |3-1(%s): no declined name, the name)
            const std::string title = pp::tradeduel::formatName(CLIENT_STR(AUCTION_TITLE), me2);
            ui.textShadow(fs, PX(379), PY(23) - lh * 0.5f, title.c_str(), kColGold, Align::Center);
        }
        ui.textShadow(ft, PX(121), PY(55), CLIENT_STR(CREATE_AUCTION), kColWhite, Align::Center);  // AuctionsTabText
        ui.textShadow(ft, PX(25), PY(80), CLIENT_STR(AUCTION_ITEM), kColGold);  // AuctionsItemText
        const bool blocked = u.blockShown;  // AuctionsBlockFrame (frame level 9001): the left panel takes no clicks
        auto lstop = [&](uint32_t id, const Rect& r, bool enabled = true) { return stop(id, r, enabled && !blocked); };
        // AuctionsItemButton (37 x 37 at 30,-94): the sell item, its name (quality colour), the count.
        {
            const Rect sr = R(30, 94, 37, 37);
            const bool f = lstop(kIdSellSlot, sr);
            if (u.sellItem) drawItemButton(ui, sr, u.sellDisplay, u.sellTotal > 1 ? u.sellTotal : 0, -1, 0xFFFFFFFFu);
            if (f) {
                if (u.slotHl.pixels) ui.image(u.slotHl, sr, 0xFFFFFFFFu, Blend::Add);
                else ui.fill(sr, 0x50FFFFFFu);
                uint8_t bag = 0, slot = 0;
                if (u.sellItem && session_.auctionSellItem(bag, slot)) {  // GameTooltip:SetAuctionSellItem (the bag item)
                    const int c = bag == game::slots::kNoContainer ? 0 : bag - game::slots::kWornBagFirst + 1;
                    const int s = bag == game::slots::kNoContainer ? slot - game::slots::kBackpackFirst : slot;
                    invTipId_ = kIdBagSlot0 + static_cast<uint32_t>(c * 36 + s);
                    invTipRect_ = screenRect(sr);
                } else {
                    setTip(CLIENT_STR(AUCTION_ITEM_TEXT), kColWhite, sr);
                }
            }
            if (u.sellItem) {
                const Rect nr = R(72, 94, 124, 30);
                const auto lines = wrapQuestText(fs, u.sellName, nr.w);
                float y = nr.y + (nr.h - lh * static_cast<float>(std::min<size_t>(2, lines.size()))) * 0.5f;
                for (size_t k = 0; k < lines.size() && k < 2; ++k) {
                    ui.textShadow(fs, nr.x, y, lines[k].c_str(), qualityColor(u.sellQuality));
                    y += lh;
                }
            }
        }
        // The stack boxes (shown for a stack of more than one).
        const bool stacks = u.sellItem && u.sellTotal > 1;
        auto inputBox = [&](uint32_t id, float x, float y, uint32_t value) {  // the EditBox's Common-Input-Border
            const Rect r = R(x - 5, y, 50 - 10 + 5 + 8, 20);
            slices(ui, u.input, r, 4, 4, 8 * kS, 8 * kS);
            char v[12] = "";
            if (value) std::snprintf(v, sizeof(v), "%u", static_cast<unsigned>(value));
            const bool f = lstop(id, r);
            ui.textShadow(fs, PX(x), r.y + (r.h - lh) * 0.5f, v, kColWhite);
            if (f) ui.fill(r, 0x30FFFFFFu);
        };
        if (stacks) {
            ui.textShadow(ft, PX(25), PY(144) - th, CLIENT_STR(AUCTION_STACK_SIZE), kColGold);
            inputBox(kIdStackSize, 33, 147, u.stackSize);
            const uint32_t maxStack = std::min(u.sellTotal, u.sellStack);  // UpdateMaximumButtons
            panelButton(kIdStackMax, R(88, 146.5f, 100, 21), CLIENT_STR(MAXIMUM), u.stackSize != maxStack && !blocked);
            ui.textShadow(ft, PX(25), PY(179) - th, CLIENT_STR(AUCTION_NUM_STACKS), kColGold);
            inputBox(kIdNumStacks, 33, 182, u.numStacks);
            const uint32_t maxStacks = u.stackSize ? u.sellTotal / u.stackSize : 0;
            panelButton(kIdNumStacksMax, R(88, 181.5f, 100, 21), CLIENT_STR(MAXIMUM), u.numStacks != maxStacks && !blocked);
        }
        // PriceDropDown (AUCTION_PRICE: AUCTION_PRICE_PER_ITEM / _PER_STACK; disabled for one item), StartPrice, BuyoutPrice.
        ui.textShadow(ft, PX(25), PY(228) - th * 0.5f, CLIENT_STR(AUCTION_PRICE), kColGold);
        dropDown(kIdPriceDD, 87, 215, 80, aucKeep_.priceType == static_cast<int>(kPriceUnit) ? CLIENT_STR(AUCTION_PRICE_PER_ITEM) : CLIENT_STR(AUCTION_PRICE_PER_STACK),
                 stacks && !blocked, kDdPrice);
        ui.textShadow(ft, PX(25), PY(256) - th, CLIENT_STR(STARTING_PRICE), kColGold);
        moneyInput(kIdStartG, 35, 259, u.startPrice, !blocked);
        // AuctionsFrameAuctions_ValidateAuction (every frame here: the original runs it on each change).
        {
            u.createEnabled = false;
            u.buyoutError = false;
            const uint32_t maxStack = u.sellStack;
            if (u.sellItem && !u.progressShown) {
                if (u.buyoutSell > 0 && u.startPrice > u.buyoutSell) {
                    u.buyoutError = true;
                } else if (u.startPrice >= 1 && u.startPrice <= a::kMaximumBidPrice &&
                           !(u.stackSize == 0 || u.stackSize > maxStack || u.numStacks == 0 ||
                             u.stackSize * u.numStacks > u.sellTotal)) {
                    u.createEnabled = true;
                }
            }
        }
        {
            const char* buyLabel = CLIENT_STR(BUYOUT_PRICE);  // BUYOUT_PRICE .. " |cff808080(" .. OPTIONAL .. ")|r"
            const std::string optional = std::string("(") + CLIENT_STR(OPTIONAL) + ")";
            const float bx = PX(25), by = PY(301) - th;
            ui.textShadow(ft, bx, by, buyLabel, u.buyoutError ? kColRedFont : kColGold);
            const float bw = static_cast<float>(ft.measure(buyLabel));
            ui.textShadow(ft, bx + bw + 3.0f, by, optional.c_str(), 0xFF808080u);
            if (u.buyoutError) {  // AuctionsBuyoutError: UI-Dialog-Icon-AlertNew + AUCTION_BUYOUT_ERROR's tooltip
                const float ow = static_cast<float>(ft.measure(optional.c_str()));
                const Rect er{bx + bw + 3.0f + ow + 6 * kS, by + th * 0.5f - 5.5f - 1.0f, 11, 11};
                if (u.alert.pixels) ui.image(u.alert, er);
                if (stop(kIdBuyoutError, er)) setTip(CLIENT_STR(AUCTION_BUYOUT_ERROR), kColRedFont, er);
            }
        }
        moneyInput(kIdBuyoutG, 35, 304, u.buyoutSell, !blocked);
        ui.textShadow(ft, PX(25), PY(339) - th * 0.5f, CLIENT_STR(AUCTION_DURATION), kColGold);
        dropDown(kIdDurationDD, 87, 326, 80, durationText(aucKeep_.duration), !blocked, kDdDuration);
        // UpdateDeposit: CalculateAuctionDeposit(duration, stackSize x numStacks) - DEPOSIT_COLON + the money.
        {
            const uint32_t rate = u.depositRate(session_.auctionHouseId());
            const uint32_t count = u.stackSize * u.numStacks;
            u.depositCopper = u.sellItem && rate > 0
                                  ? a::depositFor(rate, u.sellUnit, count, a::runTimeMinutes(aucKeep_.duration))
                                  : 0;
            ui.textShadow(fs, PX(25), PY(372) - lh * 0.5f, CLIENT_STR(DEPOSIT_COLON), kColGold);
            coins(ui, fs, npcArt_.money, PX(25) + static_cast<float>(fs.measure(CLIENT_STR(DEPOSIT_COLON))) + 5 * kS,
                  PY(372) - lh * 0.5f + 1.0f, u.depositCopper, false, kColWhite);
        }
        panelButton(kIdCreate, R(18, 388, 191, 20), CLIENT_STR(CREATE_AUCTION), u.createEnabled && !blocked);
        if (blocked) ui.fill(R(20, 75, 189, 308), 0xB3000000u);  // AuctionsBlockFrame (black, alpha 0.7)

        // --- AuctionFrameAuctions_Update ---
        const auto& list = session_.auctionItems(a::kOwner);
        const int numBatch = static_cast<int>(list.size());
        const uint32_t total = session_.auctionTotal(a::kOwner);
        const bool more = total > static_cast<uint32_t>(kPerPage);
        const int scrollRows = numBatch + (more ? 1 : 0);
        const uint32_t focused = winFocus_.focused();
        u.aucOffset = std::clamp(u.aucOffset, 0, std::max(0, scrollRows - kAuctionRows));
        if (live && focused >= kIdOwnRow0 && focused < kIdOwnRow0 + kAuctionRows) {
            const int vi = static_cast<int>(focused - kIdOwnRow0);
            if (vi == kAuctionRows - 1 && u.aucOffset + kAuctionRows < scrollRows) {
                ++u.aucOffset;
                winFocus_.setFocus(focused - 1);
            } else if (vi == 0 && u.aucOffset > 0) {
                --u.aucOffset;
                winFocus_.setFocus(focused + 1);
            }
        }
        const bool scrolls = !(numBatch < kAuctionRows || (numBatch == kAuctionRows && total <= kAuctionRows));
        const float rowW = scrolls ? 576.0f : 599.0f, bidSortW = scrolls ? 193.0f : 213.0f;
        sortButton(kIdSortOwn0 + 0, 219, 51, 185, CLIENT_STR(RARITY), a::kOwner, a::kColQuality, true);
        sortButton(kIdSortOwn0 + 1, 402, 51, 100, CLIENT_STR(CLOSES_IN), a::kOwner, a::kColDuration, true);
        sortButton(kIdSortOwn0 + 2, 500, 51, 108, CLIENT_STR(HIGH_BIDDER), a::kOwner, a::kColStatus, true);
        sortButton(kIdSortOwn0 + 3, 606, 51, bidSortW, CLIENT_STR(CURRENT_BID), a::kOwner, a::kColBid, true);
        bool isLastSlotEmpty = false;
        for (int i = 0; i < kAuctionRows; ++i) {
            const int idx = u.aucOffset + i;
            if (idx >= numBatch) {
                isLastSlotEmpty = i == kAuctionRows - 1;
                continue;
            }
            const a::Item& e = list[static_cast<size_t>(idx)];
            const Info in = infoOf(a::kOwner, e);
            const float y0 = 76 + kRowU * static_cast<float>(i), x0 = 219;
            const uint32_t id = kIdOwnRow0 + static_cast<uint32_t>(i);
            const Rect rowR = R(x0, y0, rowW - 25, kRowU);
            const bool f = stop(id, rowR);
            const bool selected = u.sel[a::kOwner] == idx + 1;
            rowFrame(x0, y0, rowW, selected || f);
            const Rect icon = R(x0, y0, 32, 32);
            const float rx = x0 + rowW + 10 - 12;
            if (in.saleStatus == 1) {  // sold: AUCTION_ITEM_SOLD grey, the buyer green, AUCTION_ITEM_TIME_UNTIL_DELIVERY
                drawItemButton(ui, icon, in.display, 0, -1, kTintGray);
                std::string sold = CLIENT_FMT(AUCTION_ITEM_SOLD, in.name);
                rowName(R(x0 + 43, y0, 142, 28), sold, kColGrayFont, f);
                if (in.highBidderName) cell(ft, R(x0 + 279, y0, 104, 28), in.highBidderName->c_str(), kColGreenFont);
                const std::string eta = CLIENT_FMT(AUCTION_ITEM_TIME_UNTIL_DELIVERY, secondsToTime(timeLeft(e)));
                cell(ft, R(x0 + 191, y0, 65, 28), eta.c_str(), kColWhite);
                const float my = PY(y0) + (kRowU * kS - lh) * 0.5f + 2.0f;
                coins(ui, fs, npcArt_.money, PX(rx), my, in.bid, true, kColWhite);
                ui.textShadow(ft, PX(rx), my - th, CLIENT_STR(AUCTION_ITEM_INCOMING_AMOUNT), kColGold, Align::Right);
            } else {
                drawItemButton(ui, icon, in.display, in.count > 1 ? in.count : 0, -1, in.canUse ? 0xFFFFFFFFu : kTintRed);
                rowName(R(x0 + 43, y0, 142, 28), in.name, qualityColor(in.quality), f);
                if (in.highBidderName) cell(ft, R(x0 + 279, y0, 104, 28), in.highBidderName->c_str(), kColWhite);
                else cell(ft, R(x0 + 279, y0, 104, 28), CLIENT_STR(NO_BIDS), kColRedFont);
                cell(ft, R(x0 + 191, y0, 65, 28), timeLeftText(timeLeft(e)), kColWhite);
                // The bid (or the minimum bid at alpha 0.5), the buyout under it (BUYOUT_COST).
                rowMoney(rx, y0, in.bid > 0 ? in.bid : in.minBid, in.buyout, 606, CLIENT_STR(BUYOUT_COST), in.bid > 0 ? 0xFF : 0x80, false);
            }
            if (f) itemTip(e, icon, in.bid > 0 ? in.bid : in.minBid, in.saleStatus == 1 ? 0 : in.buyout);
        }
        if (more && isLastSlotEmpty) {
            const std::string t = CLIENT_FMT(SINGLE_PAGE_RESULTS_TEMPLATE, static_cast<unsigned>(total));
            ui.textShadow(ft, PX(469), PY(394) - th, t.c_str(), kColWhite, Align::Center);
        }
        if (scrolls) scrollBar(796, 67, 416, kIdOwnUp, kIdOwnDown, u.aucOffset > 0, u.aucOffset + kAuctionRows < scrollRows);
        u.cancelEnabled = u.sel[a::kOwner] > 0 && session_.canCancelAuction(u.sel[a::kOwner] - 1);
        panelButton(kIdCancelAuction, R(620, 411, 126, 22), CLIENT_STR(CANCEL_AUCTION), u.cancelEnabled);
        panelButton(kIdAuctionsClose, R(744, 411, 80, 22), CLIENT_STR(CLOSE), true);
    }

    // AuctionProgressFrame (PSP: over the frame's bottom): the background, the bar, AUCTION_CREATING (n/N), the icon, the
    // cancel button; fades out after the last stack (CASTING_BAR_ALPHA_STEP 0.05 a frame).
    if (u.progressShown) {
        if (u.fadeOut) {
            u.progressAlpha -= 0.05f;
            if (u.progressAlpha <= 0.0f) {
                u.progressShown = false;
                u.fadeOut = false;
                u.progressAlpha = 1.0f;
            }
        }
        if (u.progressShown) {
            const uint32_t alpha = static_cast<uint32_t>(std::clamp(u.progressAlpha, 0.0f, 1.0f) * 255.0f);
            const uint32_t white = 0x00FFFFFFu | (alpha << 24);
            const Rect pf = R(266, 330, 300, 64);
            if (u.post.pixels) ui.image(u.post, pf, white);
            else ui.fill(pf, (alpha * 3 / 4) << 24);
            const Rect bar = R(266 + 150 - 97.5f + 3, 330 + 32 - 6.5f + 3, 195, 13);
            ui.fill(bar, (alpha * 3 / 4) << 24);
            const float frac = u.progressTotal > 0 ? std::clamp(static_cast<float>(u.progressDone) /
                                                                    static_cast<float>(u.progressTotal), 0.01f, 1.0f)
                                                   : 0.0f;
            const Rect fill{bar.x, bar.y, bar.w * frac, bar.h};
            if (u.bar.pixels) ui.image(u.bar, fill, (0x0000B2FFu & 0x00FFFFFFu) | (alpha << 24));  // the cast bar's gold
            else ui.fill(fill, (0x0000B2FFu & 0x00FFFFFFu) | (alpha << 24));
            if (u.castBorder.pixels) ui.image(u.castBorder, Rect{bar.x - 6, bar.y - 5, bar.w + 12, bar.h + 10}, white);
            const std::string t = CLIENT_FMT(AUCTION_CREATING, u.progressDone, u.progressTotal);
            ui.textShadow(ft, bar.x + bar.w * 0.5f, bar.y + (bar.h - th) * 0.5f, t.c_str(), white, Align::Center);
            if (u.progressDisplay)
                drawItemButton(ui, R(266 - 10 - 24, 330 + 32 - 2 - 12, 24, 24), u.progressDisplay, 0, -1, white);
            const Rect cb = R(266 + 150 + 3 + 97.5f + 2, 330 + 32 - 16 - 1, 32, 32);  // CancelButton-Up
            const bool f = stop(kIdProgressCancel, cb, !u.fadeOut);
            if (npcArt_.close.pixels) ui.image(npcArt_.close, cb, white);
            if (f) ui.fill(cb, 0x40FFFFFFu);
        }
    }
    return true;
}

// AuctionFrameBrowse_Update / AuctionFrameBid_Update's half for the selected row (they run on the list's events and
// on a click, not every frame - the bid box keeps what the player typed in between): the required bid in the bid box,
// the BID / BUYOUT buttons, AuctionFrame.buyoutPrice.
void WorldScreen::auctionRefresh(int list) {
    if (!auc_) return;
    AuctionUi& u = *auc_;
    const uint64_t me = session_.playerGuid();
    const uint64_t money = session_.getMoneyCopper();
    const auto& v = session_.auctionItems(list);
    const int i = u.sel[list] - 1;
    const a::Item* e = i >= 0 && i < static_cast<int>(v.size()) ? &v[static_cast<size_t>(i)] : nullptr;
    const bool high = e && e->bidder == me && me != 0;
    if (list == a::kList) {
        u.browseBidEnabled = u.browseBuyoutEnabled = false;
        if (u.type == a::kList) u.buyoutPrice = 0;
        if (!e) return;
        const uint32_t required = e->bid == 0 ? e->minBid : e->bid + e->minIncrement;
        uint32_t buyout = e->buyout;
        if (required >= a::kMaximumBidPrice) buyout = required;  // "Lie about our buyout price"
        if (buyout > 0 && buyout >= e->minBid) {
            const bool can = money >= buyout || (high && money + e->bid >= buyout);
            if (can && e->owner != me) {
                u.browseBuyoutEnabled = true;
                u.buyoutPrice = buyout;
            }
        }
        u.browseBid = required;  // MoneyInputFrame_SetCopper(BrowseBidPrice, requiredBid)
        if (!high && e->owner != me && money >= u.browseBid && u.browseBid <= a::kMaximumBidPrice) u.browseBidEnabled = true;
    } else if (list == a::kBidder) {
        u.bidBidEnabled = u.bidBuyoutEnabled = false;
        if (u.type == a::kBidder) u.buyoutPrice = 0;
        if (!e) return;
        const uint32_t bidAmount = e->bid == 0 ? e->minBid : e->bid;
        if (e->buyout > 0 && e->buyout >= bidAmount) {
            const bool can = money >= e->buyout || (high && money + bidAmount >= e->buyout);
            if (can) {
                u.bidBuyoutEnabled = true;
                u.buyoutPrice = e->buyout;
            }
        }
        u.bidBid = bidAmount + e->minIncrement;  // MoneyInputFrame_SetCopper(BidBidPrice, bidAmount + minIncrement)
        if (!high && money >= u.bidBid) u.bidBidEnabled = true;
    }
}

// --- Actions (Cross) ---------------------------------------------------------------------------------------------

bool WorldScreen::auctionAction(uint32_t act) {
    if (act < 0x8200 || act > 0x83FF || !auc_) return false;
    AuctionUi& u = *auc_;
    if (u.guard) return true;  // a Cross held since before the frame came up
    // A click anywhere else closes an open dropdown list (CloseDropDownMenus).
    if (u.dropDown != kDdNone && !(act >= kIdDdItem0 && act < kIdDdItem0 + 16)) {
        u.dropDown = kDdNone;
        closeWindow(kWinAuctionDropDown);
    }
    auto keyboard = [&](uint32_t id, const char* title, const std::string& initial, int max, pp::OskCharset cs) {
        if (pp::oskActive()) return;
        u.oskField = id;
        if (!pp::oskBeginFor(kOskOwner, title, initial, max, cs)) {
            u.oskField = 0;
            uiError(OWN_STR(PSP_KEYBOARD_UNAVAILABLE));
        }
    };
    auto moneyKey = [&](uint32_t id, uint32_t copper, int part, const char* title) {
        const uint32_t v = part == 0 ? copper / 10000u : part == 1 ? (copper / 100u) % 100u : copper % 100u;
        keyboard(id, title, v ? std::to_string(v) : std::string(), part == 0 ? 6 : 2, pp::OskCharset::Digits);
    };
    auto openDropDown = [&](uint8_t kind) {  // ToggleDropDownMenu
        if (u.dropDown == kind) {
            u.dropDown = kDdNone;
            closeWindow(kWinAuctionDropDown);
            return;
        }
        u.dropDown = kind;
        openWindow(kWinAuctionDropDown);
        winFocus_.setFocus(kIdDdItem0);
        playSoundByName("igMainMenuOptionCheckBoxOn");
    };
    // AuctionFrameBrowse_Search (+ the browse param cache: a changed search goes back to page 0).
    auto search = [&]() {
        AuctionUi::Params p;
        p.name = aucKeep_.name;
        p.minL = aucKeep_.minLevel;
        p.maxL = aucKeep_.maxLevel;
        p.invType = aucKeep_.invTypeIndex;
        p.cls = aucKeep_.classIndex;
        p.sub = aucKeep_.subIndex;
        p.usable = aucKeep_.usable;
        p.quality = aucKeep_.quality;
        if (u.hasPrevParams) {
            const auto& q = u.prevParams;
            if (p.name != q.name || p.minL != q.minL || p.maxL != q.maxL || p.invType != q.invType || p.cls != q.cls ||
                p.sub != q.sub || p.usable != q.usable || p.quality != q.quality)
                u.page = 0;
        }
        // QueryAuctionItems: the indices to ids - the class by upstream's twelve, the subclass by the class's listed
        // subclasses, the slot by the subclass's slot rows (only with a class / a subclass; 0xFFFFFFFF = any).
        uint32_t invType = 0xFFFFFFFFu, cls = 0xFFFFFFFFu, sub = 0xFFFFFFFFu;
        if (p.cls >= 1 && p.cls <= 12) {
            cls = a::classIdAt(p.cls - 1);
            if (static_cast<size_t>(p.cls - 1) < u.classes.size()) {
                const auto& subs = u.classes[static_cast<size_t>(p.cls - 1)].subs;
                if (p.sub >= 1 && static_cast<size_t>(p.sub) <= subs.size()) sub = subs[static_cast<size_t>(p.sub - 1)].id;
            }
            if (sub != 0xFFFFFFFFu && p.invType >= 1) {
                uint32_t slots[game::kNumAuctionSlots];
                if (p.invType <= a::slotRows(cls, sub, slots)) invType = slots[p.invType - 1];
            }
        }
        const uint8_t minL = static_cast<uint8_t>(digitsOf(p.minL, 255)), maxL = static_cast<uint8_t>(digitsOf(p.maxL, 255));
        session_.queryAuctionItems(p.name, minL, maxL, invType, cls, sub, u.page, p.usable,
                                   p.quality < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(p.quality), false);
        u.prevParams = p;
        u.hasPrevParams = true;
        u.searching = true;
    };
    // AuctionFrame_OnClickSortColumn.
    auto sortColumn = [&](int list, const char* column) {
        const a::SortTable& t = session_.auctionSort(list);
        uint8_t col = 0;
        a::columnByName(column, col);
        bool opposite = false;
        if (t.keys[0].used && t.keys[0].column == col) opposite = !t.keys[0].reverse;
        else if (std::strcmp(column, "level") == 0) opposite = true;
        for (const SortPreset& p : kPresets) {
            if (std::strcmp(p.table, tableName(list)) != 0 || std::strcmp(p.column, column) != 0) continue;
            session_.sortAuctionClearSort(list);
            for (int i = 0; i < p.n; ++i) {
                uint8_t c = 0;
                if (a::columnByName(p.rows[i].column, c))
                    session_.sortAuctionSetSort(list, c, opposite ? !p.rows[i].reverse : p.rows[i].reverse);
            }
            break;
        }
        if (list == a::kList) search();
        else session_.sortAuctionApplySort(list);
    };
    auto closePopups = [&]() {  // CloseAuctionStaticPopups
        if (u.popup != kPopupNone) {
            u.popup = kPopupNone;
            closeWindow(kWinAuctionPopup);
        }
    };
    auto showPopup = [&](uint8_t kind) {
        u.popup = kind;
        openWindow(kWinAuctionPopup);
        winFocus_.setFocus(kIdPopupAccept);
    };
    auto dressUp = [&](int list, int idx) {  // GetCVarBool("auctionDisplayOnCharacter") -> DressUpItemLink
        if (!aucKeep_.showOnPlayer) return;
        const auto& v = session_.auctionItems(list);
        if (idx >= 0 && idx < static_cast<int>(v.size())) dressUpItemLink(v[static_cast<size_t>(idx)].itemEntry);
    };

    switch (act) {
        case kIdClose:
        case kIdBrowseClose:
        case kIdBidClose:
        case kIdAuctionsClose: closeWindow(kWinAuction); return true;  // HideUIPanel: OnHide in updateAuction
        case kIdTab1:
        case kIdTab2:
        case kIdTab3: {  // AuctionFrameTab_OnClick
            const int tab = static_cast<int>(act - kIdTab1) + 1;
            u.tab = tab;
            playSoundByName("igCharacterInfoTab");
            if (tab == 1) {
                u.type = a::kList;
            } else if (tab == 2) {
                u.type = a::kBidder;
                if (!u.gotBids) {  // AuctionFrameBid_OnShow: once a visit
                    session_.getBidderAuctionItems(0);
                    u.gotBids = true;
                }
                auctionRefresh(a::kBidder);
            } else {
                if (!u.gotAuctions) {  // AuctionFrameAuctions_OnShow
                    session_.getOwnerAuctionItems();
                    u.gotAuctions = true;
                }
                // PSP: the backpack opens with the AUCTIONS tab (the items to sell are in it; the original leaves it to
                // the player) and closes with the frame if it opened it (MerchantFrame's BACKPACK_WAS_OPEN rule).
                if (!windowOpen(kWinBags)) {
                    openBags(false);
                    u.openedBags = true;
                }
            }
            return true;
        }
        case kIdDisabledOk: closeWindow(kWinAuctionDisabled); return true;
        case kIdPopupCancel: closePopups(); return true;
        case kIdPopupAccept: {
            const uint8_t kind = u.popup;
            closePopups();
            if (kind == kPopupBuyout) {  // PlaceAuctionBid(AuctionFrame.type, selected, AuctionFrame.buyoutPrice)
                session_.placeAuctionBid(u.type, u.sel[u.type] - 1, u.buyoutPrice);
            } else if (kind == kPopupBid) {
                session_.placeAuctionBid(u.type, u.sel[u.type] - 1, u.browseBid);
            } else if (kind == kPopupCancel) {
                session_.cancelAuction(u.sel[a::kOwner] - 1);
            }
            return true;
        }
        default: break;
    }
    if (act >= kIdDdItem0 && act < kIdDdItem0 + 16) {  // UIDropDownMenuButton_OnClick
        const int k = static_cast<int>(act - kIdDdItem0);
        playSoundByName("UChatScrollButton");
        if (u.dropDown == kDdQuality) {
            aucKeep_.quality = std::clamp(k - 1, -1, 5);  // BrowseDropDown_OnClick
        } else if (u.dropDown == kDdPrice) {  // PriceDropDown_OnClick: the prices converted for the stack
            const int type = k + 1;
            if (type != aucKeep_.priceType && u.stackSize > 1) {
                if (type == static_cast<int>(kPriceUnit)) {
                    u.startPrice /= u.stackSize;
                    u.buyoutSell /= u.stackSize;
                } else {
                    u.startPrice *= u.stackSize;
                    u.buyoutSell *= u.stackSize;
                }
            }
            aucKeep_.priceType = type;
        } else if (u.dropDown == kDdDuration) {
            aucKeep_.duration = std::clamp(k + 1, 1, 3);  // DurationDropDown_OnClick (UpdateDeposit when drawn)
        }
        u.dropDown = kDdNone;
        closeWindow(kWinAuctionDropDown);
        return true;
    }
    if (u.tab == 1) {
        switch (act) {
            case kIdBrowseName: keyboard(act, CLIENT_STR(NAME), aucKeep_.name, 63, pp::OskCharset::Text); return true;
            case kIdMinLevel: keyboard(act, CLIENT_STR(LEVEL), aucKeep_.minLevel, 2, pp::OskCharset::Digits); return true;
            case kIdMaxLevel: keyboard(act, CLIENT_STR(LEVEL), aucKeep_.maxLevel, 2, pp::OskCharset::Digits); return true;
            case kIdQualityDD: openDropDown(kDdQuality); return true;
            case kIdUsable:
                aucKeep_.usable = !aucKeep_.usable;
                playSoundByName("igMainMenuOptionCheckBoxOn");
                return true;
            case kIdShowOnPlayer:
                aucKeep_.showOnPlayer = !aucKeep_.showOnPlayer;  // SetCVar("auctionDisplayOnCharacter")
                playSoundByName("igMainMenuOptionCheckBoxOn");
                return true;
            case kIdSearch: search(); return true;
            case kIdReset:  // AuctionFrameBrowse_Reset
                aucKeep_.name.clear();
                aucKeep_.minLevel.clear();
                aucKeep_.maxLevel.clear();
                aucKeep_.usable = false;
                aucKeep_.quality = -1;
                aucKeep_.classIndex = aucKeep_.subIndex = aucKeep_.invTypeIndex = 0;
                u.filterOffset = 0;
                return true;
            case kIdPrev:
            case kIdNext:  // BrowsePrev/NextPageButton: the page, the list to the top, the search
                playSoundByName("igMainMenuOptionCheckBoxOn");
                u.page = std::max(0, u.page + (act == kIdNext ? 1 : -1));
                u.browseOffset = 0;
                search();
                return true;
            case kIdBrowseBidG: moneyKey(act, u.browseBid, 0, CLIENT_STR(BID)); return true;
            case kIdBrowseBidS: moneyKey(act, u.browseBid, 1, CLIENT_STR(BID)); return true;
            case kIdBrowseBidC: moneyKey(act, u.browseBid, 2, CLIENT_STR(BID)); return true;
            case kIdBrowseBid:  // StaticPopup_Show("BID_AUCTION"); self:Disable()
                if (u.browseBidEnabled) {
                    showPopup(kPopupBid);
                    u.browseBidEnabled = false;
                }
                return true;
            case kIdBrowseBuyout:
                if (u.browseBuyoutEnabled) {
                    showPopup(kPopupBuyout);
                    u.browseBuyoutEnabled = false;
                }
                return true;
            case kIdFilterUp: u.filterOffset = std::max(0, u.filterOffset - 1); return true;
            case kIdFilterDown: ++u.filterOffset; return true;
            case kIdBrowseUp: u.browseOffset = std::max(0, u.browseOffset - 1); return true;
            case kIdBrowseDown: ++u.browseOffset; return true;
            default: break;
        }
        if (act >= kIdSortList0 && act < kIdSortList0 + 5) {
            static const char* const kCols[5] = {"quality", "level", "duration", "seller", "bid"};
            sortColumn(a::kList, kCols[act - kIdSortList0]);
            return true;
        }
        if (act >= kIdFilter0 && act < kIdFilter0 + kFilterRows) {  // AuctionFrameFilter_OnClick
            // The rows as drawn (the same walk as the draw): the i-th shown row.
            struct FRow {
                uint8_t type;
                int index;
                bool shown;
            };
            std::vector<FRow> rows;
            for (size_t ci = 0; ci < u.classes.size() && ci < 12; ++ci) {
                const int ix = static_cast<int>(ci) + 1;
                rows.push_back(FRow{0, ix, true});
                if (aucKeep_.classIndex != ix || u.classes[ci].subs.size() < 2) continue;
                for (size_t si = 0; si < u.classes[ci].subs.size(); ++si) {
                    const int sx = static_cast<int>(si) + 1;
                    rows.push_back(FRow{1, sx, true});
                    if (aucKeep_.subIndex != sx) continue;
                    uint32_t slots[game::kNumAuctionSlots];
                    const int ns = a::slotRows(u.classes[ci].id, u.classes[ci].subs[si].id, slots);
                    for (int k = 0; k < ns; ++k) rows.push_back(FRow{2, k + 1, true});
                }
            }
            int index = u.filterOffset, want = static_cast<int>(act - kIdFilter0);
            const FRow* hit = nullptr;
            for (int i = 0; i <= want; ++i) {
                while (index < static_cast<int>(rows.size()) && rows[static_cast<size_t>(index)].type == 2 &&
                       !rows[static_cast<size_t>(index)].shown)
                    ++index;
                if (index >= static_cast<int>(rows.size())) break;
                if (i == want) hit = &rows[static_cast<size_t>(index)];
                ++index;
            }
            if (!hit) return true;
            if (hit->type == 0) {
                aucKeep_.classIndex = aucKeep_.classIndex == hit->index ? 0 : hit->index;
                aucKeep_.subIndex = aucKeep_.invTypeIndex = 0;
            } else if (hit->type == 1) {
                aucKeep_.subIndex = aucKeep_.subIndex == hit->index ? 0 : hit->index;
                aucKeep_.invTypeIndex = 0;
            } else {
                aucKeep_.invTypeIndex = hit->index;
            }
            return true;
        }
        if (act >= kIdBrowseRow0 && act < kIdBrowseRow0 + kBrowseRows) {  // BrowseButton_OnClick
            const int idx = u.browseOffset + static_cast<int>(act - kIdBrowseRow0);
            if (idx >= static_cast<int>(session_.auctionItems(a::kList).size())) return true;
            dressUp(a::kList, idx);
            u.sel[a::kList] = idx + 1;
            closePopups();
            auctionRefresh(a::kList);
            return true;
        }
    } else if (u.tab == 2) {
        switch (act) {
            case kIdBidG: moneyKey(act, u.bidBid, 0, CLIENT_STR(BID)); return true;
            case kIdBidS: moneyKey(act, u.bidBid, 1, CLIENT_STR(BID)); return true;
            case kIdBidC: moneyKey(act, u.bidBid, 2, CLIENT_STR(BID)); return true;
            case kIdBidBid:  // PlaceAuctionBid("bidder", selected, BidBidPrice); self:Disable()
                if (u.bidBidEnabled) {
                    session_.placeAuctionBid(a::kBidder, u.sel[a::kBidder] - 1, u.bidBid);
                    u.bidBidEnabled = false;
                }
                return true;
            case kIdBidBuyout:
                if (u.bidBuyoutEnabled) {
                    showPopup(kPopupBuyout);
                    u.bidBuyoutEnabled = false;
                }
                return true;
            case kIdBidUp: u.bidOffset = std::max(0, u.bidOffset - 1); return true;
            case kIdBidDown: ++u.bidOffset; return true;
            default: break;
        }
        if (act >= kIdSortBid0 && act < kIdSortBid0 + 6) {
            static const char* const kCols[6] = {"quality", "level", "duration", "buyout", "status", "bid"};
            sortColumn(a::kBidder, kCols[act - kIdSortBid0]);
            return true;
        }
        if (act >= kIdBidRow0 && act < kIdBidRow0 + kBidRows) {  // BidButton_OnClick
            const int idx = u.bidOffset + static_cast<int>(act - kIdBidRow0);
            if (idx >= static_cast<int>(session_.auctionItems(a::kBidder).size())) return true;
            dressUp(a::kBidder, idx);
            u.sel[a::kBidder] = idx + 1;
            closePopups();
            auctionRefresh(a::kBidder);
            return true;
        }
    } else {
        if (u.blockShown && act != kIdProgressCancel && act != kIdCancelAuction && act != kIdAuctionsClose &&
            !(act >= kIdOwnRow0 && act < kIdOwnRow0 + kAuctionRows) && !(act >= kIdSortOwn0 && act < kIdSortOwn0 + 4))
            return true;  // AuctionsBlockFrame
        switch (act) {
            case kIdSellSlot: {  // AuctionSellItemButton_OnClick: ClickAuctionSellItemButton
                uint8_t bag = 0, slot = 0;
                const bool had = session_.auctionSellItem(bag, slot);
                if (invHeld_.active) {
                    const a::Line err = session_.canAuctionItem(invHeld_.bag, invHeld_.slot);
                    if (err != a::kLineNone) {
                        session_.auctionGameError(err);
                        return true;
                    }
                    if (had && bag == invHeld_.bag && slot == invHeld_.slot) {  // the same item: the cursor clears
                        invHeld_ = InvHeld{};
                        return true;
                    }
                    session_.setAuctionSellItem(invHeld_.bag, invHeld_.slot);
                    invHeld_ = had ? InvHeld{true, bag, slot} : InvHeld{};  // the one in the slot on the cursor
                } else if (had) {
                    session_.clearAuctionSellItem();
                    invHeld_ = InvHeld{true, bag, slot};
                }
                playUiSound(UiSound::PickupBag);
                return true;
            }
            case kIdStackSize: keyboard(act, CLIENT_STR(AUCTION_STACK_SIZE), u.stackSize ? std::to_string(u.stackSize) : "", 4,
                                        pp::OskCharset::Digits); return true;
            case kIdNumStacks: keyboard(act, CLIENT_STR(AUCTION_NUM_STACKS), u.numStacks ? std::to_string(u.numStacks) : "", 4,
                                        pp::OskCharset::Digits); return true;
            case kIdStackMax:  // SetMaxStackSize
                if (u.sellTotal > 0) {
                    if (u.sellTotal > u.sellStack) {
                        u.stackSize = u.sellStack;
                        u.numStacks = u.sellStack ? u.sellTotal / u.sellStack : 1;
                    } else {
                        u.stackSize = u.sellTotal;
                        u.numStacks = 1;
                    }
                }
                return true;
            case kIdNumStacksMax:  // AuctionsNumStacksMaxButton
                if (u.stackSize == 0) u.stackSize = 1;
                if (u.sellTotal > 0) u.numStacks = u.sellTotal / u.stackSize;
                return true;
            case kIdPriceDD: openDropDown(kDdPrice); return true;
            case kIdDurationDD: openDropDown(kDdDuration); return true;
            case kIdStartG: moneyKey(act, u.startPrice, 0, CLIENT_STR(STARTING_PRICE)); return true;
            case kIdStartS: moneyKey(act, u.startPrice, 1, CLIENT_STR(STARTING_PRICE)); return true;
            case kIdStartC: moneyKey(act, u.startPrice, 2, CLIENT_STR(STARTING_PRICE)); return true;
            case kIdBuyoutG: moneyKey(act, u.buyoutSell, 0, CLIENT_STR(BUYOUT_PRICE)); return true;
            case kIdBuyoutS: moneyKey(act, u.buyoutSell, 1, CLIENT_STR(BUYOUT_PRICE)); return true;
            case kIdBuyoutC: moneyKey(act, u.buyoutSell, 2, CLIENT_STR(BUYOUT_PRICE)); return true;
            case kIdCreate: {  // AuctionsCreateAuctionButton_OnClick
                if (!u.createEnabled) return true;
                aucKeep_.lastStartBid = u.startPrice;
                aucKeep_.lastBuyout = u.buyoutSell;
                playSoundByName("LOOTWINDOWCOINSOUND");
                uint32_t start = u.startPrice, buyout = u.buyoutSell;
                if (aucKeep_.priceType == static_cast<int>(kPriceUnit)) {
                    start *= u.stackSize;
                    buyout *= u.stackSize;
                }
                session_.startAuction(start, buyout, aucKeep_.duration, u.stackSize, u.numStacks);
                return true;
            }
            case kIdCancelAuction:
                if (u.cancelEnabled && u.sel[a::kOwner] > 0) showPopup(kPopupCancel);  // StaticPopup_Show("CANCEL_AUCTION")
                return true;
            case kIdProgressCancel: session_.cancelSell(); return true;
            case kIdOwnUp: u.aucOffset = std::max(0, u.aucOffset - 1); return true;
            case kIdOwnDown: ++u.aucOffset; return true;
            default: break;
        }
        if (act >= kIdSortOwn0 && act < kIdSortOwn0 + 4) {
            static const char* const kCols[4] = {"quality", "duration", "status", "bid"};
            sortColumn(a::kOwner, kCols[act - kIdSortOwn0]);
            return true;
        }
        if (act >= kIdOwnRow0 && act < kIdOwnRow0 + kAuctionRows) {  // AuctionsButton_OnClick
            const int idx = u.aucOffset + static_cast<int>(act - kIdOwnRow0);
            const auto& v = session_.auctionItems(a::kOwner);
            if (idx >= static_cast<int>(v.size())) return true;
            dressUp(a::kOwner, idx);
            u.sel[a::kOwner] = idx + 1;
            closePopups();
            const a::Item& e = v[static_cast<size_t>(idx)];
            u.cancelPrice = e.saleStatus == 0 && e.bid > 0 ? static_cast<uint32_t>(std::floor(e.bid * 0.05)) : 0u;
            return true;
        }
    }
    return true;
}

// Circle: with AUCTIONS shown, a bag item goes into the sell slot (PSP: own rule, there is no drag - after the
// mail / bank / guild bank / merchant / trade branches, before the item's use).
bool WorldScreen::auctionAltAction(uint32_t alt) {
    if (!auc_ || !auc_->shown || auc_->tab != 3 || !windowOpen(kWinAuction) || !session_.isAuctionHouseOpen()) return false;
    if (alt >= 0x8200 && alt <= 0x83FF) return auctionAction(alt);
    int c = 0, s = 0;
    if (!bagSlotId(alt, c, s) || !inventoryItemAt(alt)) return false;
    if (session_.isVendorWindowOpen() || session_.tradeOpen()) return false;  // the merchant / the trade take it
    uint8_t bag = 0, slot = 0;
    if (!inventoryIdToWire(alt, bag, slot)) return false;
    if (auc_->blockShown) return true;
    const a::Line err = session_.canAuctionItem(bag, slot);
    if (err != a::kLineNone) {
        session_.auctionGameError(err);
        return true;
    }
    session_.setAuctionSellItem(bag, slot);
    if (invHeld_.active && invHeld_.bag == bag && invHeld_.slot == slot) invHeld_ = InvHeld{};
    playUiSound(UiSound::PickupBag);
    return true;
}

// Square on the sell slot = its drag (OnDragStart / OnReceiveDrag: the same click).
void WorldScreen::auctionWindowPad(pp::PadState& pad) {
    if (!auc_ || !(pad.pressed & PSP_CTRL_SQUARE)) return;
    if (winFocus_.focused() != kIdSellSlot) return;
    auctionAction(kIdSellSlot);
    pad.pressed &= ~PSP_CTRL_SQUARE;
}

// After all the windows: the frame's one-line GameTooltips (GameTooltip:SetText; the item ones are the shared
// GameTooltip of tipProtoItem_ / invTipId_).
void WorldScreen::drawAuctionOverlay(UiRenderer& ui) {
    if (!auc_ || !auc_->tipText || !uiMode_) return;
    AuctionUi& u = *auc_;
    const Font& fs = kit_.font(kFontSmall);
    const auto lines = wrapQuestText(fs, u.tipText, 200.0f);
    float w = 0.0f;
    for (const auto& l : lines) w = std::max(w, static_cast<float>(fs.measure(l.c_str())));
    const float lh = static_cast<float>(fs.lineHeight());
    Rect r{u.tipRect.x + u.tipRect.w, u.tipRect.y - lh * static_cast<float>(lines.size()) - 10.0f, w + 12.0f,
           lh * static_cast<float>(lines.size()) + 10.0f};  // ANCHOR_RIGHT: above the owner's right edge
    r.x = std::clamp(r.x, 2.0f, static_cast<float>(pp::kScreenW) - 2.0f - r.w);
    r.y = std::clamp(r.y, 2.0f, static_cast<float>(pp::kScreenH) - 2.0f - r.h);
    ui.boundsHold(true);
    ui.fill(Rect{r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 0xBA1B0D0Du);  // TOOLTIP_DEFAULT_BACKGROUND_COLOR (.05 .05 .1, .73)
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    float y = r.y + 5.0f;
    for (const auto& l : lines) {
        ui.textShadow(fs, r.x + 6.0f, y, l.c_str(), u.tipColor);
        y += lh;
    }
    ui.boundsHold(false);
    u.tipText = nullptr;
}

// --- The auction's invoice in a letter (OpenMailInvoiceFrame) ----------------------------------------------------

// OpenMailInvoiceFrame of MailFrame.xml / OpenMail_Update: an auction mail's invoice on the letter's paper.
// GetInboxInvoiceInfo: the invoice type by the subject's response (AzerothCore MailAuctionAnswers; own rule by
// their meaning: AUCTION_WON -> "buyer", AUCTION_SUCCESSFUL -> "seller", AUCTION_SALE_PENDING ->
// "seller_temp_invoice", the others carry no sale and are no invoice); the body read by upstream's
// parseAuctionMailBody (the other party, the bid, the buyout, the deposit and the house's cut - a buyer pays the bid
// only) and, for a pending sale, AzerothCore's 7th field (BuildAuctionMailBody: the packed time the money comes).
// The other party's name by a name query. The child frame (296 x 255 units) at x0, y0, sx / sy px a unit.
// False: no invoice (the body is printed as it is).
bool WorldScreen::drawAuctionInvoice(UiRenderer& ui, const game::MailMessage& m, float x0, float y0, float sx, float sy) {
    game::AuctionMailSubject sub;
    if (m.messageType != 2 || !game::parseAuctionMailSubject(m.subject, sub)) return false;
    if (sub.response != 1 && sub.response != 2 && sub.response != 6) return false;
    game::AuctionMailInvoice inv;
    if (!game::parseAuctionMailBody(m.body, inv)) return false;
    const uint64_t guid = inv.ownerGuidLow;  // a player's guid is its low part
    uint32_t eta = 0;
    if (sub.response == 6) {
        size_t at = std::string::npos;
        for (int colon = 0; colon < 6; ++colon) {
            at = m.body.find(':', at == std::string::npos ? 0 : at + 1);
            if (at == std::string::npos) break;
        }
        if (at != std::string::npos) eta = static_cast<uint32_t>(std::strtoul(m.body.c_str() + at + 1, nullptr, 10));
    }
    const uint32_t bid = inv.bid, buyout = inv.buyout;
    const uint32_t deposit = sub.response == 1 ? 0u : inv.deposit, cut = sub.response == 1 ? 0u : inv.consignment;
    session_.ensureItemInfo(sub.itemEntry);
    const auto* info = session_.getItemInfo(sub.itemEntry);
    const std::string itemName = info && info->valid ? info->name : std::string();
    const std::string* who = session_.auctionNameOf(guid);
    const std::string playerName = who ? *who : std::string();
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight());
    constexpr uint32_t kInk = 0xFF0F1F2Eu;   // InvoiceTextFontNormal / Small (0.18, 0.12, 0.06)
    constexpr uint32_t kLine = 0xFF223E5Cu;  // UI-MailFrame-InvoiceLine's line
    auto X = [&](float u) { return x0 + u * sx; };
    auto Y = [&](float u) { return y0 + u * sy; };
    const std::string modeText = std::string("(") + (bid == buyout ? CLIENT_STR(BUYOUT) : CLIENT_STR(HIGH_BIDDER)) + ")";
    const char* mode = modeText.c_str();
    std::string label, purchaser, amountLabel;
    uint32_t amount = 0;
    if (sub.response == 1) {
        label = std::string(CLIENT_STR(ITEM_PURCHASED_COLON)) + " " + itemName + "  " + mode;
        purchaser = std::string(CLIENT_STR(SOLD_BY_COLON)) + " " + playerName;
        amountLabel = CLIENT_STR(AMOUNT_PAID_COLON);
        amount = bid;
    } else if (sub.response == 2) {
        label = std::string(CLIENT_STR(ITEM_SOLD_COLON)) + " " + itemName;
        purchaser = std::string(CLIENT_STR(PURCHASED_BY_COLON)) + " " + playerName;
        amountLabel = CLIENT_STR(AMOUNT_RECEIVED_COLON);
        amount = bid + deposit - cut;
    } else {
        label = std::string(CLIENT_STR(ITEM_SOLD_COLON)) + " " + itemName + "  " + mode;
        purchaser = std::string(CLIENT_STR(PURCHASED_BY_COLON)) + " " + playerName;
        amountLabel = CLIENT_STR(AUCTION_INVOICE_PENDING_FUNDS_COLON);
        amount = bid + deposit - cut;
    }
    // OpenMailInvoiceItemLabel (TOPLEFT 30,-35, 240 wide), OpenMailInvoicePurchaser (5 under it) + BuyMode after it.
    {
        const Rect lr{X(30), Y(35), 240 * sx, lh};
        if (static_cast<float>(fs.measure(label.c_str())) > lr.w) ui.textMarquee(fs, lr, label.c_str(), kInk);
        else ui.text(fs, lr.x, lr.y, label.c_str(), kInk);
        ui.text(fs, X(30), Y(52), purchaser.c_str(), kInk);
        if (sub.response == 2)
            ui.text(ft, X(30) + static_cast<float>(fs.measure(purchaser.c_str())) + 5 * sx,
                    Y(52) + (lh - static_cast<float>(ft.lineHeight())) * 0.5f, mode, kInk);
    }
    // The money lines: the labels right-aligned at 155, the money frames (SmallMoneyFrame) right at 155 + 130.
    loadNpcArt(false);  // the coins
    const float labelR = X(155), moneyR = X(272);
    auto line = [&](float yU, const char* text, uint32_t copper, const char* sign, uint32_t moneyCol) {
        ui.text(fs, labelR, Y(yU), text, kInk, Align::Right);
        const float w = coins(ui, fs, npcArt_.money, moneyR, Y(yU), copper, true, moneyCol, 0xFF, true);
        if (sign) ui.text(fs, moneyR - w - 5 * sx, Y(yU), sign, kInk);
    };
    if (sub.response == 2) {
        line(90, CLIENT_STR(SALE_PRICE_COLON), bid, nullptr, kColWhite);
        line(115, CLIENT_STR(DEPOSIT_COLON), deposit, "+", kColWhite);
        line(140, CLIENT_STR(AUCTION_HOUSE_CUT_COLON), cut, "-", kColRed);  // SetMoneyFrameColor red
        ui.fill(Rect{X(155 - 128 + 6), Y(143 + 15), (249 - 6) * sx, 2 * sy}, kLine);  // OpenMailArithmeticLine
        line(170, amountLabel.c_str(), amount, nullptr, kColWhite);
    } else {
        ui.fill(Rect{X(155 - 128 + 6), Y(64 + 15), (249 - 6) * sx, 2 * sy}, kLine);
        line(90, amountLabel.c_str(), amount, nullptr, kColWhite);
        if (sub.response == 6) {  // OpenMailInvoiceNotYetSent / OpenMailInvoiceMoneyDelay (CENTER 128,-90 / -103)
            ui.text(fs, X(158), Y(154) - lh * 0.5f, CLIENT_STR(AUCTION_INVOICE_FUNDS_NOT_YET_SENT), kInk, Align::Center);
            char hm[16];  // AUCTION_INVOICE_FUNDS_DELAY with GameTime_GetFormattedTime (24 h): the packed time's h:mm
            std::snprintf(hm, sizeof(hm), "%u:%02u", static_cast<unsigned>((eta >> 6) & 0x1Fu),
                          static_cast<unsigned>(eta & 0x3Fu));
            const std::string t = CLIENT_FMT(AUCTION_INVOICE_FUNDS_DELAY, hm);
            ui.text(fs, X(158), Y(167) - lh * 0.5f, t.c_str(), kInk, Align::Center);
        }
    }
    return true;
}

}  // namespace wowee::ui::psp
