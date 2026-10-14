// The bank of the original - BankFrame.xml / .lua of the ruRU 3.3.5a client
// (every window 1:1 on the original art) over the session's bank half (psp_world_session_mail.cpp).
//
//   - the banker's gossip option (or, PSP, L + up on a banker without gossip: CMSG_BANKER_ACTIVATE by the NPC
//     flags, ui_world_npc.cpp) -> SMSG_SHOW_BANK -> BANKFRAME_OPENED: the frame opens with the cursor up, the
//     backpack with it (PSP, as MerchantFrame's OpenBackpack - BankFrame.lua leaves it to the client);
//   - the frame: UI-BankFrame (ui.pak bank_frame, one texture with the slots) at 0.55 of its units (the 37-unit
//     slots stay square), the banker's portrait (round) under it, BankFrameTitleText = the banker's name,
//     ITEMSLOTTEXT / BAGSLOTTEXT, the close button;
//   - BankFrameItem1..28 (ItemButtonTemplate: icon, count, the UI-Quickslot2 frame) = the bank's own slots
//     (PLAYER_FIELD_BANK_SLOT_1.., wire 0xFF:39..66), BankFrameBag1..7 = the bank bag slots (wire 0xFF:67..73):
//     the bag's icon or UI-PaperDoll-Slot-Bag, red (1, 0.1, 0.1) past GetNumBankSlots (PLAYER_BYTES_2 byte 2),
//     CheckButtonHilight while that bag's ContainerFrame is open (UpdateBagButtonHighlight);
//   - BankFramePurchaseInfo while a slot is left to buy: BANKSLOTPURCHASE_LABEL, COSTS_LABEL with
//     GetBankSlotCost (BankBagSlotPrices.dbc, ui.pak raw bank_prices) red when GetMoney is less, BANKSLOTPURCHASE ->
//     StaticPopup CONFIRM_BUY_BANK_SLOT (YES / NO, the money frame) -> PurchaseSlot (CMSG_BUY_BANK_SLOT);
//     BankFrameMoneyFrame = the player's money;
//   - the buttons (BankFrameItemButtonGeneric_OnClick): Cross / Square = the left click (PickupContainerItem:
//     the cursor item of the bags, ui_world_inventory.cpp invHeld_), Circle = the right click
//     (UseContainerItem: CMSG_AUTOSTORE_BANK_ITEM into the bags); a bag button (BankFrameItemButtonBag_OnClick):
//     PutItemInBag with an item on the cursor, else ToggleBag - the bank bag's ContainerFrame in the bags' stack
//     (drawBankBagFrames below), Square picks the bag up (BankFrameItemButtonBag_Pickup);
//   - Circle on a bag item while the bank is open = UseContainerItem at the bank (CMSG_AUTOBANK_ITEM);
//   - GameTooltip of the focused item (SetInventoryItem), BANK_BAG / BANK_BAG_PURCHASE on an empty bag slot.
// PSP:
//   - the backpack opens with the frame and closes with it if it opened it (MerchantFrame's rule);
//   - the frame closes past NPC_INTERACT_MAX_DISTANCE (the client's own distance check);
//   - no quest-item marks on the slots (TEXTURE_ITEM_QUEST_BANG / BORDER), no cooldowns, no locked look
//     (IsInventoryItemLocked), no stack split;
//   - the purchase row: the label wrapped in the 10 px font, the button 16 px high (11.5 at 0.55).
// Memory: bank_frame (T8 256 x 256, ~65 KB) only while the frame is open. Log: bank_open, bank_art, bank_close.
#include "ui/psp/ui_world_bank_mail.hpp"

#include "game/inventory_slots.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// BankFrame units -> px (tools/psp/build_ui_pak.py BANK_S): one scale, the slots stay square.
constexpr float kS = 0.55f, kOx = 4.0f, kOy = 2.0f;
constexpr float kFrameW = 400.0f, kFrameH = 425.0f;  // the baked part of the 425 x 512 frame
float PX(float u) { return kOx + u * kS; }
float PY(float u) { return kOy + u * kS; }
constexpr float kSlot = 20.0f;  // ItemButtonTemplate 37 units

// The game's own bank slot prices for the seven bag slots (upstream lua_GetBankSlotCost) - only when ui.pak
// predates bank_prices (BankBagSlotPrices.dbc).
constexpr uint32_t kUpstreamSlotPrices[7] = {1000, 10000, 100000, 250000, 500000, 1000000, 2500000};

constexpr uint32_t kTintRed = 0xFF1A1AFFu;    // SetItemButtonTextureVertexColor(1.0, 0.1, 0.1)
constexpr uint32_t kTintHeld = 0xFF606060u;   // on the cursor (SetItemButtonDesaturated look)

// ContainerFrame for a bank bag: the same numbers as ui_world_bags.cpp (ContainerFrame_GenerateFrame).
constexpr float kBagS = 0.55f, kBagW = 192.0f, kBagBottom = 240.0f, kBagTop = 2.0f;
constexpr int kBagColumns = 4;
struct BagLayout {
    float topSrc0 = 0, topSrc1 = 0, topH = 0, midSrc0 = 0, midH = 0, height = 0;
    bool middle = false;
};
BagLayout bagLayout(int size) {
    BagLayout l;
    const int rows = (size + kBagColumns - 1) / kBagColumns;
    if (size % kBagColumns == 2) {
        l.topSrc0 = 97;
        l.topSrc1 = 169;
        l.topH = 72;
    } else if (rows == 1) {
        l.topSrc0 = 2;
        l.topSrc1 = 86;
        l.topH = 86;
    } else {
        l.topSrc0 = 2;
        l.topSrc1 = 94;
        l.topH = 94;
    }
    if (rows > 1) {
        l.middle = true;
        l.midSrc0 = 181;
        l.midH = static_cast<float>((rows - 1) * 41 - 9);
    }
    l.height = l.topH + l.midH + 10;
    return l;
}

// Word wrap for the purchase label (UTF-8, the font's advances).
std::vector<std::string> wrapText(const Font& f, const char* text, float maxW) {
    std::vector<std::string> out;
    std::string cur;
    float w = 0.0f, breakW = 0.0f;
    size_t breakAt = 0;
    const char* s = text;
    while (*s) {
        const char* p0 = s;
        const uint32_t cp = utf8Next(&s);
        const Glyph* g = f.find(cp);
        const float adv = g ? static_cast<float>(g->advance) : 0.0f;
        if (w + adv > maxW && !cur.empty() && breakAt > 0) {
            out.push_back(cur.substr(0, breakAt));
            cur.erase(0, breakAt);
            w -= breakW;
            breakAt = 0;
        }
        cur.append(p0, static_cast<size_t>(s - p0));
        w += adv;
        if (cp == ' ') {
            breakAt = cur.size();
            breakW = w;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

// --- The shared state --------------------------------------------------------------------

WorldScreen::BankMailUi::~BankMailUi() {
    releaseBankArt();
    releaseMailArt();
}

void WorldScreen::BankMailUi::releaseBankArt() {
    if (bankFrame.pixels) pp::textureRelease(bankFrame);
    bankTried = false;
}

void WorldScreen::BankMailUi::releaseMailArt() {
    for (pp::Texture* t : {&mailFrame, &openFrame, &paperSend, &paperOpen, &bar, &scroll, &slotWhite, &slotBg, &letter,
                           &radio, &alert, &checked, &coin[0], &coin[1], &coin[2], &prev, &prevDis, &next, &nextDis})
        if (t->pixels) pp::textureRelease(*t);
    mailFrameTab = 0;
    paperSendName = paperOpenName = "";
    openTried = smallTried = false;
}

uint32_t WorldScreen::BankMailUi::artBytes() const {
    uint32_t n = 0;
    for (const pp::Texture* t : {&bankFrame, &mailFrame, &openFrame, &paperSend, &paperOpen, &bar, &scroll, &slotWhite,
                                 &slotBg, &letter, &radio, &alert, &checked, &coin[0], &coin[1], &coin[2], &prev,
                                 &prevDis, &next, &nextDis})
        if (t->pixels) n += pp::textureBytes(*t);
    return n;
}

void WorldScreen::destroyBankMailUi(BankMailUi* p) { delete p; }

WorldScreen::BankMailUi& WorldScreen::bankMail() {
    if (!bm_) bm_ = std::unique_ptr<BankMailUi, void (*)(BankMailUi*)>(new BankMailUi(), &destroyBankMailUi);
    return *bm_;
}

// Per frame (ui_world_npc.cpp updateNpc): the session's distance check and events, the windows follow it.
void WorldScreen::updateBankMail(float dtSec) {
    PROF_SCOPE("bankmail");
    session_.bankMailUpdate(dtSec, glm::vec3(playerX_, playerY_, playerZ_));
    if (!bm_ && !session_.isBankOpen() && !session_.isMailboxOpen()) {
        pp::WorldSession::MailEvent ev;
        while (session_.takeMailEvent(ev)) {}  // nothing on screen listens
        return;
    }
    BankMailUi& b = bankMail();
    syncBank();
    syncMail();
    mailOskTick();
    // The cursor came up for these frames: down again once they and every other window are gone.
    if (b.raisedCursor) {
        if (!uiMode_) {
            b.raisedCursor = false;
        } else if (!b.bankShown && !b.mailShown && windows_.empty() && !settings_.isOpen()) {
            setUiMode(false);
            b.raisedCursor = false;
            suppress_ = ~0u;  // the Cross / START that closed it must not walk
        }
    }
    // Nothing open: the state and every texture go (log with KB).
    if (!b.bankShown && !b.mailShown && !session_.isBankOpen() && !session_.isMailboxOpen() && !b.raisedCursor &&
        !windowOpen(kWinMailPopup) && !windowOpen(kWinBankBuy)) {
        PSP_LOGI(pp::tag::Gfx, "bankmail_release kb=%u", static_cast<unsigned>(b.artBytes() / 1024u));
        bm_.reset();
    }
}

// --- BankFrame: open / close -------------------------------------------------------------

void WorldScreen::syncBank() {
    BankMailUi& b = bankMail();
    const bool want = session_.isBankOpen();
    if (want && !b.bankShown) {
        // BANKFRAME_OPENED -> ShowUIPanel(BankFrame) -> BankFrame_OnShow.
        b.bankShown = true;
        std::fill(std::begin(b.bankBagOpen), std::end(b.bankBagOpen), false);
        b.guard = true;
        if (!uiMode_) {
            setUiMode(true);
            b.raisedCursor = true;
            suppress_ = ~0u;
        }
        // PSP: the backpack with it (MerchantFrame_OnShow's BACKPACK_WAS_OPEN rule).
        b.bankOpenedBags = !windowOpen(kWinBags);
        if (b.bankOpenedBags) openBags(false);
        openWindow(kWinBank);  // on top: it has the focus
        winFocus_.setFocus(kIdBankItem0);
        playSoundByName("igMainMenuOpen");
        PSP_LOGI(pp::tag::Ctrl, "bank_open purchased=%d bags=%d", session_.purchasedBankBagSlots(),
                 b.bankOpenedBags ? 1 : 0);
    } else if (want && b.bankShown && !windowOpen(kWinBank)) {
        session_.closeBank();  // START / the close button: CloseBankFrame (the session's flag, then the rest below)
    } else if (!want && b.bankShown) {
        // BANKFRAME_CLOSED -> HideUIPanel -> BankFrame_OnHide: StaticPopup_Hide("CONFIRM_BUY_BANK_SLOT"),
        // CloseBankBagFrames, updateContainerFrameAnchors.
        b.bankShown = false;
        closeWindow(kWinBank);
        closeWindow(kWinBankBuy);
        std::fill(std::begin(b.bankBagOpen), std::end(b.bankBagOpen), false);
        if (b.bankOpenedBags && windowOpen(kWinBags)) closeWindow(kWinBags);
        b.bankOpenedBags = false;
        if (invHeld_.active && invHeld_.bag == game::slots::kNoContainer &&
            invHeld_.slot >= game::slots::kBankGeneralFirst && invHeld_.slot < game::slots::keyringFirst())
            invHeld_ = InvHeld{};  // a bank item on the cursor goes back (ClearCursor)
        if (invHeld_.active && invHeld_.bag >= game::slots::bankBagFirst() &&
            invHeld_.bag < game::slots::bankBagFirst() + game::slots::bankBagCount())
            invHeld_ = InvHeld{};
        const uint32_t kb = pp::textureBytes(b.bankFrame) / 1024u;
        b.releaseBankArt();
        playSoundByName("igMainMenuClose");
        PSP_LOGI(pp::tag::Ctrl, "bank_close kb=%u", static_cast<unsigned>(kb));
    }
}

// --- BankFrame: drawing ------------------------------------------------------------------

bool WorldScreen::drawBankFrame(UiRenderer& ui) {
    BankMailUi& b = bankMail();
    PROF_SCOPE("bank.frame");
    if (!b.bankTried) {
        b.bankTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            const pp::PakEntry* e = pak.find("bank_frame");
            void* blob = e ? pak.load(e) : nullptr;
            if (blob && !pp::textureFromBlob(blob, e->size, b.bankFrame, true)) pp::pakFree(blob);
            if (!b.checked.pixels) {  // CheckButtonHilight (a bag button's open bag)
                const pp::PakEntry* ce = pak.find("sb_checked");
                void* cb = ce ? pak.load(ce) : nullptr;
                if (cb && !pp::textureFromBlob(cb, ce->size, b.checked, true)) pp::pakFree(cb);
            }
            if (!b.pricesTried) {  // GetBankSlotCost: BankBagSlotPrices.dbc (u32 by slot id - 1)
                b.pricesTried = true;
                const pp::PakEntry* pe = pak.find("bank_prices");
                if (void* raw = pe ? pak.load(pe) : nullptr) {
                    const auto* v = static_cast<const uint32_t*>(raw);
                    b.bankPrices.assign(v, v + pe->size / 4);
                    pp::pakFree(raw);
                }
            }
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "bank_art frame=%d kb=%u prices=%u", b.bankFrame.pixels ? 1 : 0,
                 static_cast<unsigned>(pp::textureBytes(b.bankFrame) / 1024u), static_cast<unsigned>(b.bankPrices.size()));
    }
    loadNpcArt(false);  // the close button, the coins, the panel buttons
    loadBagArt();       // UI-PaperDoll-Slot-Bag for an empty bag slot
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const auto& inv = session_.getInventory();
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    auto seen = [&](uint32_t id) {
        if (winFocus_.isPressed(id)) b.guard = false;
    };
    auto held = [&](uint8_t bag, uint8_t slot) { return invHeld_.active && invHeld_.bag == bag && invHeld_.slot == slot; };
    auto emptyFrame = [&](const Rect& r) {  // ItemButtonTemplate's NormalTexture without an item
        if (!spellArt_.slot.pixels) return;
        const float c = r.w * 64.0f / 37.0f;
        ui.image(spellArt_.slot, Rect{r.x + r.w * 0.5f - c * 0.5f, r.y + r.h * 0.5f - c * 0.5f, c, c});
    };

    // BankPortraitTexture (60 x 60 at 7,-6), round, under the frame.
    const Rect pr{PX(7), PY(6), 60 * kS, 60 * kS};
    if (const pp::Texture* face = portraitOf(session_.bankerGuid())) ui.imageCircle(*face, pr);
    else ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    if (b.bankFrame.pixels) ui.image(b.bankFrame, Rect{kOx, kOy, kFrameW * kS, kFrameH * kS});
    else kit_.panel(ui, Rect{kOx, kOy, kFrameW * kS, kFrameH * kS});

    // BankFrameTitleText (GameFontHighlight, CENTER -3,233 of the 425 x 512 frame) = UnitName("npc").
    {
        UnitView u;
        const std::string name = unitView(session_.bankerGuid(), u) ? u.name : std::string();
        const Rect nr{PX(80), PY(23) - 7, (340 - 80) * kS, 14};
        if (static_cast<float>(fn.measure(name.c_str())) <= nr.w)
            ui.textShadow(fn, PX(209.5f), PY(23) - fn.lineHeight() * 0.5f, name.c_str(), kColWhite, Align::Center);
        else
            ui.textMarquee(fs, nr, name.c_str(), kColWhite);
    }
    // ITEMSLOTTEXT / BAGSLOTTEXT (GameFontNormal, CENTER -3,200 / -3,-2).
    ui.textShadow(fs, PX(209.5f), PY(56) - lh * 0.5f, CLIENT_STR(ITEMSLOTTEXT), kColGold, Align::Center);
    ui.textShadow(fs, PX(209.5f), PY(258) - lh * 0.5f, CLIENT_STR(BAGSLOTTEXT), kColGold, Align::Center);
    // BankCloseButton (UIPanelCloseButton, TOPRIGHT -22,-8).
    {
        const Rect cr{PX(371), PY(8), 32 * kS, 32 * kS};
        const bool f = focusable && winFocus_.addStop(kIdBankClose, cr);
        if (f) seen(kIdBankClose);
        const pp::Texture& t = f && winFocus_.isPressed(kIdBankClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                  : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // BankFrameItem1..28: TOPLEFT 40,-73, 12 apart, rows 7 apart.
    for (int i = 0; i < game::Inventory::BANK_SLOTS; ++i) {
        const float ux = 40.0f + static_cast<float>(i % 7) * 49.0f, uy = 73.0f + static_cast<float>(i / 7) * 44.0f;
        const Rect r{std::floor(PX(ux)), std::floor(PY(uy)), kSlot, kSlot};
        const uint32_t id = kIdBankItem0 + static_cast<uint32_t>(i);
        const game::ItemSlot& s = inv.getBankSlot(i);
        const bool f = focusable && winFocus_.addStop(id, r);
        if (f) seen(id);
        const bool h = held(game::slots::kNoContainer, static_cast<uint8_t>(game::slots::bankGeneralWireSlot(i)));
        if (!s.empty()) drawItemButton(ui, r, s.item.displayInfoId, s.item.stackCount, -1, h ? kTintHeld : 0xFFFFFFFFu);
        else emptyFrame(r);
        if (f) {
            ui.fill(r, 0x50FFFFFFu);  // ButtonHilight-Square
            if (!s.empty()) {
                b.tipItem = &s.item;
                b.tipRect = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};
            }
        }
    }
    // BankFrameBag1..7: Item22's BOTTOMLEFT 0,-32, 12 apart; red past GetNumBankSlots.
    const int purchased = session_.purchasedBankBagSlots();
    for (int i = 0; i < game::Inventory::BANK_BAG_SLOTS; ++i) {
        const Rect r{std::floor(PX(40.0f + static_cast<float>(i) * 49.0f)), std::floor(PY(274)), kSlot, kSlot};
        const uint32_t id = kIdBankBag0 + static_cast<uint32_t>(i);
        const game::ItemSlot& bag = inv.getBankBagItem(i);
        const bool f = focusable && winFocus_.addStop(id, r);
        if (f) seen(id);
        const bool h = held(game::slots::kNoContainer, static_cast<uint8_t>(game::slots::bankBagWireSlot(i)));
        const uint32_t tint = i >= purchased ? kTintRed : h ? kTintHeld : 0xFFFFFFFFu;
        if (!bag.empty()) {
            drawItemButton(ui, r, bag.item.displayInfoId, 1, -1, tint);
        } else {
            if (bagArt_.emptyBag.pixels) ui.image(bagArt_.emptyBag, r, tint);
            emptyFrame(r);
        }
        if (b.bankBagOpen[i] && b.checked.pixels) ui.image(b.checked, r, 0xFFFFFFFFu, Blend::Add);  // UpdateBagButtonHighlight
        else if (b.bankBagOpen[i]) ui.fill(r, 0x40FFFFFFu);
        if (f) {
            ui.fill(r, 0x50FFFFFFu);
            b.tipRect = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};
            if (!bag.empty()) b.tipItem = &bag.item;
            else b.tipTitle = i < purchased ? CLIENT_STR(BANK_BAG) : CLIENT_STR(BANK_BAG_PURCHASE);
        }
    }

    // BankFramePurchaseInfo (380 x 72, CENTER -23,-105): shown while a bag slot is left to buy.
    if (purchased < game::Inventory::BANK_BAG_SLOTS) {
        const auto lines = wrapText(fs, CLIENT_STR(BANKSLOTPURCHASE_LABEL), 380 * kS);
        float ty = PY(341) - lh * static_cast<float>(lines.size()) * 0.5f;
        for (const auto& l : lines) {
            ui.textShadow(fs, PX(42), ty, l.c_str(), kColWhite);
            ty += lh;
        }
        // GetBankSlotCost(numSlots): BankBagSlotPrices.dbc row numSlots + 1.
        const uint32_t cost = static_cast<size_t>(purchased) < b.bankPrices.size()
                                  ? b.bankPrices[static_cast<size_t>(purchased)]
                                  : kUpstreamSlotPrices[purchased];
        const float cy = PY(371);
        const char* costLabel = CLIENT_STR(COSTS_LABEL);
        ui.textShadow(fs, PX(44.5f), cy - lh * 0.5f, costLabel, kColGold);
        const bool afford = session_.getMoneyCopper() >= cost;  // SetMoneyFrameColor white / red
        drawCoins(ui, fs, npcArt_.money, PX(44.5f) + static_cast<float>(fs.measure(costLabel)) + 10 * kS, cy - lh * 0.5f,
                  cost, false, afford ? kColWhite : kColRed);
        drawPanelButton(ui, kIdBankPurchase, Rect{PX(245.5f), cy - 8, 124 * kS, 16}, CLIENT_STR(BANKSLOTPURCHASE), true,
                        focusable);
        if (focusable && winFocus_.isPressed(kIdBankPurchase)) b.guard = false;
    }
    // BankFrameMoneyFrame (BOTTOMRIGHT -30,103): the player's money.
    drawMoneyFrame(ui, PX(395), PY(409) - 11.0f, static_cast<uint32_t>(session_.getMoneyCopper()), true);
    return true;
}

// StaticPopupDialogs["CONFIRM_BUY_BANK_SLOT"]: CONFIRM_BUY_BANK_SLOT, YES / NO, the money frame
// (BankFrame.nextSlotCost); OnAccept PurchaseSlot. START = hideOnEscape (NO).
bool WorldScreen::drawBankBuyPopup(UiRenderer& ui) {
    BankMailUi& b = bankMail();
    const int purchased = session_.purchasedBankBagSlots();
    const uint32_t cost = static_cast<size_t>(purchased) < b.bankPrices.size()
                              ? b.bankPrices[static_cast<size_t>(purchased)]
                              : purchased < 7 ? kUpstreamSlotPrices[purchased] : 0u;
    StaticPopup p;
    p.text = CLIENT_STR(CONFIRM_BUY_BANK_SLOT);
    p.button1 = CLIENT_STR(YES);
    p.button1Id = kIdBankBuyYes;
    p.button2 = CLIENT_STR(NO);
    p.button2Id = kIdBankBuyNo;
    p.hasMoneyFrame = true;
    p.money = cost;
    p.focusable = uiMode_ && !settings_.isOpen();
    drawStaticPopup(ui, p);
    return true;
}

// --- BankFrame: the buttons --------------------------------------------------------------

bool WorldScreen::bankIdToWire(uint32_t id, uint8_t& bag, uint8_t& slot) const {
    const auto& inv = session_.getInventory();
    if (id >= kIdBankItem0 && id < kIdBankItem0 + game::Inventory::BANK_SLOTS) {
        bag = game::slots::kNoContainer;
        slot = static_cast<uint8_t>(game::slots::bankGeneralWireSlot(static_cast<int>(id - kIdBankItem0)));
        return true;
    }
    if (id >= kIdBankBag0 && id < kIdBankBag0 + game::Inventory::BANK_BAG_SLOTS) {
        bag = game::slots::kNoContainer;
        slot = static_cast<uint8_t>(game::slots::bankBagWireSlot(static_cast<int>(id - kIdBankBag0)));
        return true;
    }
    if (id >= kIdBankBagSlot0 && id < kIdBankBagSlot0 + game::Inventory::BANK_BAG_SLOTS * 36) {
        const int bb = static_cast<int>(id - kIdBankBagSlot0) / 36, bs = static_cast<int>(id - kIdBankBagSlot0) % 36;
        if (bs >= inv.getBankBagSize(bb)) return false;
        bag = static_cast<uint8_t>(game::slots::bankBagContainer(bb));
        slot = static_cast<uint8_t>(bs);
        return true;
    }
    return false;
}

const game::ItemDef* WorldScreen::bankItemAt(uint32_t id) const {
    const auto& inv = session_.getInventory();
    const game::ItemSlot* s = nullptr;
    if (id >= kIdBankItem0 && id < kIdBankItem0 + game::Inventory::BANK_SLOTS) {
        s = &inv.getBankSlot(static_cast<int>(id - kIdBankItem0));
    } else if (id >= kIdBankBag0 && id < kIdBankBag0 + game::Inventory::BANK_BAG_SLOTS) {
        s = &inv.getBankBagItem(static_cast<int>(id - kIdBankBag0));
    } else if (id >= kIdBankBagSlot0 && id < kIdBankBagSlot0 + game::Inventory::BANK_BAG_SLOTS * 36) {
        const int bb = static_cast<int>(id - kIdBankBagSlot0) / 36, bs = static_cast<int>(id - kIdBankBagSlot0) % 36;
        if (bs < inv.getBankBagSize(bb)) s = &inv.getBankBagSlot(bb, bs);
    }
    return s && !s->empty() ? &s->item : nullptr;
}

// PickupContainerItem on a bank slot: take it on the cursor, or put the cursor item there (CMSG_SWAP_ITEM;
// the server merges stacks and swaps, and refuses with SMSG_INVENTORY_CHANGE_FAILURE).
void WorldScreen::bankPickPut(uint32_t id) {
    uint8_t bag = 0, slot = 0;
    if (!bankIdToWire(id, bag, slot)) return;
    if (!invHeld_.active) {
        if (!bankItemAt(id)) return;
        invHeld_ = InvHeld{true, bag, slot};
        playUiSound(UiSound::PickupBag);
        return;
    }
    const InvHeld held = invHeld_;
    invHeld_ = InvHeld{};
    if (held.bag == bag && held.slot == slot) return;  // put back where it was
    session_.swapContainerItems(held.bag, held.slot, bag, slot);
}

bool WorldScreen::bankFrameAction(uint32_t act, bool alt) {
    BankMailUi& b = bankMail();
    if (b.guard) return true;  // a Cross held since before the frame came up
    const auto& inv = session_.getInventory();
    switch (act) {
        case kIdBankClose: closeWindow(kWinBank); return true;  // syncBank closes the session's bank
        case kIdBankPurchase:
            playSoundByName("igMainMenuOption");
            openWindow(kWinBankBuy);  // StaticPopup_Show("CONFIRM_BUY_BANK_SLOT")
            winFocus_.setFocus(kIdBankBuyYes);
            return true;
        case kIdBankBuyYes:
            session_.buyBankSlot();  // PurchaseSlot
            closeWindow(kWinBankBuy);
            return true;
        case kIdBankBuyNo: closeWindow(kWinBankBuy); return true;
        default: break;
    }
    if (act >= kIdBankBagClose0 && act < kIdBankBagClose0 + game::Inventory::BANK_BAG_SLOTS) {
        b.bankBagOpen[act - kIdBankBagClose0] = false;  // the bank bag's ContainerFrame close button
        return true;
    }
    if (act >= kIdBankBag0 && act < kIdBankBag0 + game::Inventory::BANK_BAG_SLOTS) {
        // BankFrameItemButtonBag_OnClick: PutItemInBag(inventoryID); without an item on the cursor ToggleBag(id).
        const int i = static_cast<int>(act - kIdBankBag0);
        if (invHeld_.active) {
            const InvHeld held = invHeld_;
            invHeld_ = InvHeld{};
            const int size = inv.getBankBagSize(i);
            if (inv.getBankBagItem(i).empty() || size == 0) {  // an empty slot: the bag goes there
                session_.swapContainerItems(held.bag, held.slot, game::slots::kNoContainer,
                                            static_cast<uint8_t>(game::slots::bankBagWireSlot(i)));
                return true;
            }
            for (int s = 0; s < size; ++s) {
                if (!inv.getBankBagSlot(i, s).empty()) continue;
                session_.swapContainerItems(held.bag, held.slot, static_cast<uint8_t>(game::slots::bankBagContainer(i)),
                                            static_cast<uint8_t>(s));
                return true;
            }
            uiError(CLIENT_STR(ERR_BAG_FULL));
            return true;
        }
        if (inv.getBankBagSize(i) > 0) {
            b.bankBagOpen[i] = !b.bankBagOpen[i];
            if (b.bankBagOpen[i] && !windowOpen(kWinBags)) {  // its ContainerFrame lives in the bags' stack
                std::fill(bagOpen_, bagOpen_ + 5, false);
                openWindow(kWinBags);
            }
            playUiSound(b.bankBagOpen[i] ? UiSound::BagOpen : UiSound::BagClose);
        }
        return true;
    }
    const bool item = (act >= kIdBankItem0 && act < kIdBankItem0 + game::Inventory::BANK_SLOTS) ||
                      (act >= kIdBankBagSlot0 && act < kIdBankBagSlot0 + game::Inventory::BANK_BAG_SLOTS * 36);
    if (!item) return false;
    if (!alt) {
        bankPickPut(act);  // BankFrameItemButtonGeneric_OnClick "LeftButton": PickupContainerItem
        return true;
    }
    // "RightButton": UseContainerItem(BANK_CONTAINER, id) - the client moves it into the bags.
    uint8_t bag = 0, slot = 0;
    if (bankItemAt(act) && bankIdToWire(act, bag, slot)) session_.withdrawItem(bag, slot);
    return true;
}

// --- The bank's bags: ContainerFrames in the bags' stack ----------------------------------

void WorldScreen::drawBankBagFrames(UiRenderer& ui, bool focusable, float& colRight, float& bottom, int& shown) {
    if (!bm_ || !session_.isBankOpen()) return;
    BankMailUi& b = *bm_;
    const auto& inv = session_.getInventory();
    const Font& fs = kit_.font(kFontSmall);
    const float lh = static_cast<float>(fs.lineHeight());
    for (int bb = 0; bb < game::Inventory::BANK_BAG_SLOTS; ++bb) {
        const int size = std::min(inv.getBankBagSize(bb), 36);  // ContainerFrame: MAX_CONTAINER_ITEMS
        if (!b.bankBagOpen[bb] || size <= 0) continue;
        const BagLayout l = bagLayout(size);
        const float h = l.height * kBagS, w = kBagW * kBagS;
        if (shown > 0 && bottom - h < kBagTop) {
            colRight -= w;
            bottom = kBagBottom;
        }
        const float fx = colRight - w, fy = bottom - h;
        bottom = fy;
        ++shown;
        const auto& bag = inv.getBankBagItem(bb);
        // $parentPortrait (the bag's icon), the art (ContainerFrame_GenerateFrame), the name, the close button.
        const Rect por{fx + 7 * kBagS, fy + 5 * kBagS, 40 * kBagS, 40 * kBagS};
        if (!bag.empty())
            if (const pp::Texture* t = itemIcons().byDisplay(bag.item.displayInfoId, spellFrame_)) ui.image(*t, por);
        if (bagArt_.comps.pixels) {
            const pp::Texture& t = bagArt_.comps;
            const float tw = static_cast<float>(t.uw);
            ui.image(t, Rect{fx, fy, w, l.topH * kBagS}, SrcRect{0, l.topSrc0 * kBagS, tw, (l.topSrc1 - l.topSrc0) * kBagS});
            float y = fy + l.topH * kBagS;
            if (l.middle) {
                ui.image(t, Rect{fx, y, w, l.midH * kBagS}, SrcRect{0, l.midSrc0 * kBagS, tw, l.midH * kBagS});
                y += l.midH * kBagS;
            }
            ui.image(t, Rect{fx, y, w, 10 * kBagS}, SrcRect{0, 169 * kBagS, tw, 10 * kBagS});
        } else {
            kit_.panel(ui, Rect{fx, fy, w, h});
        }
        {
            const std::string name = bag.empty() ? std::string() : bag.item.name;
            const Rect nr{fx + 47 * kBagS, fy + 10 * kBagS + (12 * kBagS - lh) * 0.5f, 112 * kBagS, lh};
            if (static_cast<float>(fs.measure(name.c_str())) > nr.w) ui.textMarquee(fs, nr, name.c_str(), kColWhite);
            else ui.textShadow(fs, nr.x, nr.y, name.c_str(), kColWhite);
        }
        {
            const Rect cr{fx + w - 32 * kBagS, fy + 1 * kBagS, 32 * kBagS, 32 * kBagS};
            const uint32_t id = kIdBankBagClose0 + static_cast<uint32_t>(bb);
            const bool f = focusable && winFocus_.addStop(id, cr);
            const pp::Texture& t = f && winFocus_.isPressed(id) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                           : npcArt_.close;
            if (t.pixels) ui.image(t, cr);
            if (f) ui.fill(cr, 0x40FFFFFFu);
        }
        // The items: Item1 bottom right (the last slot), rows upwards, columns to the left.
        const float baseY = l.height - 9.0f;
        for (int k = 0; k < size && k < 36; ++k) {
            const int slot = size - 1 - k;
            const float brx = kBagW - 12.0f - static_cast<float>(k % kBagColumns) * 42.0f;
            const float bry = baseY - static_cast<float>(k / kBagColumns) * 41.0f;
            const Rect r{fx + (brx - 37.0f) * kBagS, fy + (bry - 37.0f) * kBagS, 37.0f * kBagS, 37.0f * kBagS};
            const uint32_t id = kIdBankBagSlot0 + static_cast<uint32_t>(bb * 36 + slot);
            const game::ItemSlot& s = inv.getBankBagSlot(bb, slot);
            const bool f = focusable && winFocus_.addStop(id, r);
            const bool h = invHeld_.active && invHeld_.bag == game::slots::bankBagContainer(bb) && invHeld_.slot == slot;
            if (!s.empty()) drawItemButton(ui, r, s.item.displayInfoId, s.item.stackCount, -1, h ? kTintHeld : 0xFFFFFFFFu);
            if (f) {
                ui.fill(r, 0x50FFFFFFu);
                if (!s.empty()) {
                    b.tipItem = &s.item;
                    b.tipRect = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};
                }
            }
        }
    }
}

// --- Dispatch from the window stack (ui_world_windows.cpp) ---------------------------------

bool WorldScreen::drawBankMailWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinBank && w != kWinBankBuy && w != kWinMail && w != kWinOpenMail && w != kWinMailPopup) return false;
    // A Cross going down on any of their buttons (tabs and popups too): the click is the player's own.
    const uint32_t fid = winFocus_.focused();
    if (bm_ && fid >= 0x7400 && fid <= 0x76FF && winFocus_.isPressed(fid)) bm_->guard = false;
    if (w == kWinBank) return drawBankFrame(ui);
    if (w == kWinBankBuy) return drawBankBuyPopup(ui);
    if (w == kWinMail) return drawMailFrame(ui);
    if (w == kWinOpenMail) return drawOpenMailFrame(ui);
    return drawMailPopup(ui);
}

bool WorldScreen::bankMailAction(uint32_t act) {
    const bool bankId = act >= 0x7400 && act < kIdBankBagSlot0 + game::Inventory::BANK_BAG_SLOTS * 36;
    const bool mailId = act >= 0x7600 && act <= 0x76FF;
    if (!bm_ || (!bankId && !mailId)) return false;
    if (bankId) return bankFrameAction(act, false);
    return mailFrameAction(act, false);
}

bool WorldScreen::bankMailAltAction(uint32_t alt) {
    if (!bm_) return false;
    const bool bankId = alt >= 0x7400 && alt < kIdBankBagSlot0 + game::Inventory::BANK_BAG_SLOTS * 36;
    if (bankId) return bankFrameAction(alt, true);
    if (alt >= 0x7600 && alt <= 0x76FF) return mailFrameAction(alt, true);
    // UseContainerItem on a bag item: attached to the letter while the send tab shows (ClickSendMailItemButton),
    // moved into the bank while the bank is open.
    int c = 0, s = 0;
    if (!bagSlotId(alt, c, s) || !inventoryItemAt(alt)) return false;
    BankMailUi& b = *bm_;
    if (b.mailShown && b.tab == 2 && windowOpen(kWinMail)) {
        const bool ok = c == 0 ? session_.attachItemFromBackpack(s) : session_.attachItemFromBag(c - 1, s);
        if (ok) playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    if (b.bankShown && session_.isBankOpen()) {
        uint8_t bag = 0, slot = 0;
        if (inventoryIdToWire(alt, bag, slot)) session_.depositItem(bag, slot);
        return true;
    }
    return false;
}

// Square = the left click on their slots (pick up / put down), before the bags' own Square handler.
void WorldScreen::bankMailWindowPad(pp::PadState& pad) {
    if (!bm_ || !(pad.pressed & PSP_CTRL_SQUARE)) return;
    const uint32_t id = winFocus_.focused();
    if (id >= kIdBankItem0 && id < kIdBankItem0 + game::Inventory::BANK_SLOTS) {
        bankPickPut(id);
    } else if (id >= kIdBankBagSlot0 && id < kIdBankBagSlot0 + game::Inventory::BANK_BAG_SLOTS * 36) {
        bankPickPut(id);
    } else if (id >= kIdBankBag0 && id < kIdBankBag0 + game::Inventory::BANK_BAG_SLOTS) {
        if (invHeld_.active) bankFrameAction(id, false);  // put the cursor item in / there
        else bankPickPut(id);                             // BankFrameItemButtonBag_Pickup
    } else if (id >= kIdMailAttach0 && id < kIdMailAttach0 + pp::WorldSession::MAIL_MAX_ATTACHMENTS) {
        mailFrameAction(id, false);  // SendMailAttachmentButton_OnClick (the drag)
    } else {
        return;
    }
    pad.pressed &= ~PSP_CTRL_SQUARE;  // the bags' handler must not act on it too
}

// After all the windows: GameTooltip of what the focus is on in them.
void WorldScreen::drawBankMailOverlay(UiRenderer& ui) {
    if (!bm_) return;
    BankMailUi& b = *bm_;
    if (b.tipItem) {
        drawItemTooltip(ui, *b.tipItem, b.tipRect);
    } else if (b.tipProto != 0) {
        const auto* info = session_.getItemInfo(b.tipProto);
        if (info && info->valid) drawItemTooltip(ui, session_.protoItemDef(b.tipProto, b.tipCount), b.tipRect);
    } else if (b.tipTitle) {
        const bool money = b.tipMoney != 0;
        kit_.tooltip(ui, Rect{b.tipRect.x, b.tipRect.y, b.tipRect.w, b.tipRect.h}, b.tipTitle, money ? " " : "");
        if (money) {  // SetTooltipMoney: the coins on the second line (UiKit::tooltip's box: 44 px above, x clamped)
            loadNpcArt(false);
            const Font& ft = kit_.font(kFontNormal);
            const Font& fs = kit_.font(kFontSmall);
            const float w = std::max(static_cast<float>(ft.measure(b.tipTitle)), static_cast<float>(fs.measure(" "))) + 16.0f;
            const float x = std::clamp(b.tipRect.x, 4.0f, static_cast<float>(pp::kScreenW) - 4.0f - w);
            ui.boundsHold(true);
            drawCoins(ui, fs, npcArt_.money, x + 8, b.tipRect.y - 44.0f + 21.0f, b.tipMoney, false,
                      b.tipMoneyRed ? kColRed : kColWhite);
            ui.boundsHold(false);
        }
    }
    b.tipItem = nullptr;
    b.tipProto = 0;
    b.tipTitle = nullptr;
    b.tipMoney = 0;
}

bool WorldScreen::bankMailItemLocked(uint8_t bag, uint8_t slot) const {
    return bm_ && session_.isMailboxOpen() && session_.isItemAttachedToMail(bag, slot);
}

}  // namespace wowee::ui::psp
