// The state and art of BankFrame (ui_world_bank.cpp) and MailFrame /
// OpenMailFrame (ui_world_mail.cpp) - WorldScreen::BankMailUi, made when one of them opens
// (WorldScreen::bankMail) and freed when they are all closed. Internal to those two files.
#pragma once

#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_texture.hpp"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::ui::psp {

// Widget ids: the bank 0x7400-0x74FF, the bank bags' ContainerFrames kIdBankBagSlot0 (0x7500) + bag * 36 + slot,
// the mail 0x7600-0x76FF.
enum : uint32_t {
    kIdBankItem0 = 0x7400,   // BankFrameItem1..28
    kIdBankBag0 = 0x7420,    // BankFrameBag1..7
    kIdBankClose = 0x7430,
    kIdBankPurchase,
    kIdBankBuyYes,
    kIdBankBuyNo,
    kIdBankBagClose0 = 0x74F0,  // the bank bags' ContainerFrame close buttons (7)
    kIdMailRow0 = 0x7600,       // MailItem1..7
    kIdMailPrev = 0x7610,
    kIdMailNext,
    kIdMailClose,
    kIdMailTab1,
    kIdMailTab2,
    kIdMailTo = 0x7620,         // SendMailNameEditBox
    kIdMailSubject,
    kIdMailBody,
    kIdMailGold,
    kIdMailSilver,
    kIdMailCopper,
    kIdMailRadioMoney,
    kIdMailRadioCod,
    kIdMailSend,
    kIdMailCancel,
    kIdMailAttach0 = 0x7640,    // SendMailAttachment1..12
    kIdOpenAttach0 = 0x7660,    // OpenMailAttachmentButton1..16
    kIdOpenMoney = 0x7680,
    kIdOpenLetter,
    kIdOpenReply,
    kIdOpenDelete,
    kIdOpenCancel,
    kIdOpenClose,
    kIdOpenBody,
    kIdMailPopupYes = 0x76F0,
    kIdMailPopupNo,
};

struct WorldScreen::BankMailUi {
    // --- Art (ui.pak; all optional: without it the windows draw plain panels) ---
    platform::psp::Texture bankFrame;  // bank_frame (UI-BankFrame, 0.55)
    platform::psp::Texture mailFrame;  // mail_inbox / mail_send (the shown tab's, 0.6 x 0.55)
    int mailFrameTab = 0;
    platform::psp::Texture openFrame;  // mail_open
    platform::psp::Texture paperSend, paperOpen;  // mail_paper_* (the letter's stationery, two halves)
    const char* paperSendName = "";
    const char* paperOpenName = "";
    platform::psp::Texture bar, scroll, slotWhite, slotBg, letter, radio, alert, checked;
    platform::psp::Texture coin[3];             // gold, silver, copper piles (GetCoinIcon)
    platform::psp::Texture prev, prevDis, next, nextDis;
    bool bankTried = false, openTried = false, smallTried = false;
    std::vector<uint32_t> bankPrices;  // BankBagSlotPrices.dbc (ui.pak raw bank_prices), by bag slot
    bool pricesTried = false;
    std::unordered_map<uint32_t, std::string> ahNames;  // AuctionHouse.dbc ruRU (ui.pak raw mail_ah)
    bool ahTried = false;

    // --- BankFrame ---
    bool bankShown = false, bankOpenedBags = false;
    bool bankBagOpen[7] = {};

    // --- MailFrame / OpenMailFrame ---
    bool mailShown = false, mailOpenedBags = false;
    int tab = 1;        // PanelTemplates_SetTab: 1 inbox, 2 send
    int page = 1;       // InboxFrame.pageNum
    uint32_t openMailId = 0;  // InboxFrame.openMailID (PSP: the mail's id, not its index)
    bool openUpdatePositions = true;  // OpenMailFrame.updateButtonPositions
    std::vector<int> openButtons;     // activeAttachmentButtons: -2 letter, -1 money, 0.. attachment index
    std::vector<std::pair<float, float>> openRows;  // activeAttachmentRowPositions (cursorxstart, cursorxend)
    int bodyPage = 0;
    std::string to, subject, body, previousItem;
    uint32_t gold = 0, silver = 0, copper = 0;  // SendMailMoney (MoneyInputFrame)
    bool codMode = false;          // SendMailCODButton checked (else SendMailSendMoneyButton)
    bool sendDisabled = false;     // SendMailMailButton:Disable() until MAIL_FAILED / MAIL_SUCCESS
    bool replyMode = false;        // SendMailFrame.sendMode == "reply"
    int maxRowsShown = 0;          // SendMailFrame.maxRowsShown
    uint8_t popup = 0;             // the StaticPopup on kWinMailPopup (MailPopup)
    int popupAttach = 0;           // OpenMailFrame.lastTakeAttachment
    uint32_t popupMoney = 0;
    std::string popupText;
    int oskField = 0;              // the edit box the keyboard is for (kIdMailTo...)

    // --- Both ---
    bool guard = false;            // opened by itself: a Cross still held does not click (npc_.guard)
    bool raisedCursor = false;     // the cursor came up for them
    uint32_t seenVersion = ~0u;
    const game::ItemDef* tipItem = nullptr;  // a bank slot's item (instance) under the focus, after the windows
    uint32_t tipProto = 0, tipCount = 1;     // a mail attachment (the item cache)
    const char* tipTitle = nullptr;          // a one-line GameTooltip (BANK_BAG...)
    std::string tipTitleBuf;
    uint32_t tipMoney = 0;                   // with a money line under the title
    bool tipMoneyRed = false;
    Rect tipRect{};
    uint32_t logs = 0;

    void releaseBankArt();
    void releaseMailArt();
    uint32_t artBytes() const;
    ~BankMailUi();
};

// SmallMoneyFrame as WorldScreen::drawMoneyFrame lays it out (ui_world_merchant.cpp), in a colour
// (SetMoneyFrameColor "red" / "white"). The coins: the NPC windows' money_icons. Returns the width.
inline float drawCoins(UiRenderer& ui, const Font& fs, const platform::psp::Texture& money, float x, float y,
                       uint32_t copper, bool right, uint32_t col) {
    const uint32_t parts[3] = {copper / 10000u, (copper / 100u) % 100u, copper % 100u};
    char txt[3][12] = {};
    float w = 0.0f;
    for (int p = 0; p < 3; ++p) {
        if (parts[p] == 0 && !(p == 2 && parts[0] == 0 && parts[1] == 0)) continue;
        std::snprintf(txt[p], sizeof(txt[p]), "%u", static_cast<unsigned>(parts[p]));
        w += static_cast<float>(fs.measure(txt[p])) + 1.0f + 12.0f;
    }
    float cx = right ? x - w : x;
    for (int p = 0; p < 3; ++p) {
        if (!txt[p][0]) continue;
        ui.textShadow(fs, cx, y, txt[p], col);
        cx += static_cast<float>(fs.measure(txt[p])) + 1.0f;
        if (money.pixels) {
            const float uw = money.uw * 0.25f;
            ui.image(money, Rect{cx, y + 1, 10, 10}, SrcRect{uw * static_cast<float>(p), 0, uw, static_cast<float>(money.uh)});
        }
        cx += 12.0f;
    }
    return w;
}

// The MailPopup kinds (StaticPopupDialogs of MailFrame.lua / StaticPopup.lua).
enum : uint8_t {
    kMailPopupNone = 0,
    kMailPopupCodConfirm,   // COD_CONFIRMATION
    kMailPopupCodAlert,     // COD_ALERT
    kMailPopupDeleteMail,   // DELETE_MAIL
    kMailPopupDeleteMoney,  // DELETE_MONEY
    kMailPopupSendMoney,    // SEND_MONEY
};

}  // namespace wowee::ui::psp
