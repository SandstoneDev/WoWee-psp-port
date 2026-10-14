// The mail of the original - MailFrame.xml / .lua (InboxFrame, SendMailFrame,
// OpenMailFrame) and their StaticPopups of the ruRU 3.3.5a client over the session's mail half
// (psp_world_session_mail.cpp).
//
//   - the way in: L + up by a mailbox (the nearest GAMEOBJECT_TYPE_MAILBOX in reach, nearer than any NPC -
//     ui_world_npc.cpp) = the right click on it: CMSG_GAMEOBJ_USE + the client's own MAIL_SHOW
//     (CMSG_GET_MAIL_LIST); SMSG_SHOW_MAILBOX (a mailbox NPC / spell) as well. MAIL_SHOW -> ShowUIPanel(MailFrame),
//     OpenBackpack, MailFrameTab_OnClick(1), CheckInbox; the cursor comes up;
//   - MailFrame (0.6 x 0.55 of its units, as the merchant; ui.pak mail_inbox / mail_send: the tab's frame
//     quarters, Mail-Icon, the MailItem borders / the edit boxes, the money boxes and their coins, the bar),
//     the close button, the tabs INBOX / SENDMAIL (FriendsFrameTabTemplate);
//   - InboxFrame_Update: INBOXITEMS_TO_DISPLAY (7) letters a page - the icon (the first attachment's, else
//     INV_Letter_15 as upstream) in UI-EmptySlot-White gold / grey (read), CASH_ON_DELIVERY on a C.O.D. letter, the
//     first attachment's count, the sender (GameFontNormal gold / grey) and the subject (white / grey), the
//     time left (DAYS_ABBR green, SecondsToTime red under a day), the open letter checked; PREV /
//     NEXT (UI-SpellbookIcon arrows); InboxTooMuchMail when the server holds more than it showed;
//     InboxFrameItem_OnEnter's tooltip; Cross = InboxFrame_OnClick (open / close the letter);
//   - OpenMailFrame (TOPLEFT at the inbox's TOPRIGHT -10; ui.pak mail_open): FROM / MAIL_SUBJECT_LABEL, the letter on
//     its stationery (Stationery.dbc via upstream game/stationery.hpp, ui.pak mail_paper_*), the scroll bar,
//     the bar, TAKE_ATTACHMENTS / NO_ATTACHMENTS, the buttons laid out by
//     OpenMailFrame_UpdateButtonPositions (7 a row, the last row centred): the letter (MAIL_LETTER_TOOLTIP,
//     TakeInboxTextItem), the money (GetCoinIcon, TakeInboxMoney), the attachments (red if not usable;
//     OpenMailAttachment_OnClick: COD_ALERT / COD_CONFIRMATION / TakeInboxItem); REPLY_MESSAGE (OpenMail_Reply,
//     MAIL_REPLY_PREFIX), DELETE / MAIL_RETURN (OpenMail_Delete: DELETE_MAIL / DELETE_MONEY / ReturnInboxItem),
//     CLOSE; reading marks it read (GetInboxText); OpenMailFrame_OnHide deletes an emptied copied letter;
//   - SendMailFrame: MAIL_TO_LABEL / MAIL_SUBJECT_LABEL / the body (the PSP keyboard: OSK, owned - the chat never sees it),
//     COSTS_LABEL GetSendMailPrice (30 copper a letter / an attachment, red over GetMoney), the 12
//     attachments (ATTACHMENTS_PER_ROW_SEND 7, the second row once needed - SendMailFrame_Update's rows and
//     maxRowsShown), SEND_MONEY / COD (the C.O.D. one only with an attachment), the
//     amount (MoneyInputFrame: gold / silver / copper, the keyboard's digits), MAIL_COD_ERROR over
//     MAX_COD_AMOUNT, the player's money, SEND_LABEL (SendMailFrame_CanSend: a name and a subject; disabled
//     until MAIL_FAILED / MAIL_SUCCESS) -> SEND_MONEY for money, SendMail; CANCEL; an attachment's subject
//     filled in (SendMailFrame.previousItem); MAIL_SEND_SUCCESS: ERR_MAIL_SENT (system chat), SendMailFrame_Reset, back to
//     the inbox after a reply;
//   - the bags: Circle on a bag item with the send tab shown attaches it (UseContainerItem), Cross / Square
//     take it on the cursor and Cross / Square on an attachment slot attaches the cursor item; an attached
//     item looks locked in its bag; Cross / Circle on an attachment takes it off the letter;
//   - SMSG_SEND_MAIL_RESULT errors: ERR_MAIL_* / ERR_* of GlobalStrings ruRU on the red line (the session).
// PSP:
//   - the edit boxes take their text from the PSP keyboard (✕ on a box); the body shows from its top (no
//     cursor, no scrolling while composing), a letter read is paged with ✕ on its text (the scroll bar's
//     arrows show more is there);
//   - no stationery choice (the default, as SendMailFrame_Reset selects it), no REPORT_SPAM (spam report),
//     no auction invoice panel (an invoice body is not printed), no item refund warning, no auto-loot
//     (Shift-click), no alert icon on the delete popups; the inbox tooltip shows the item, or the money / the
//     C.O.D. / MAIL_MULTIPLE_ITEMS " (N)" - not both;
//   - the open letter is kept by its id (the original by its index in the list);
//   - the auction house's name from AuctionHouse.dbc (ui.pak raw mail_ah) - the PSP has no auction house yet;
//   - a read letter's icon is drawn grey (the original desaturates it).
// an auction's invoice (OpenMailInvoiceFrame) is drawn by drawAuctionInvoice (ui_world_auction.cpp).
// Memory: one tab frame (T8 ~65 KB), mail_open (~65 KB) and one or two papers (T8 ~33 KB each) while shown,
// ~40 KB of small art; all freed when the mailbox closes. Log: mail_open, mail_art, mail_close, mail_osk.
#include "ui/psp/ui_world_bank_mail.hpp"

#include "game/inventory_slots.hpp"
#include "game/stationery.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings and the port's own
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// MailFrame units -> px (tools/psp/build_ui_pak.py MAIL_*): 0.6 x 0.55 as the merchant (the names need width).
constexpr float kSX = 0.6f, kSY = 0.55f, kOx = 4.0f, kOy = 2.0f;
constexpr float kFrameW = 356.0f, kFrameH = 445.0f;  // the baked part of the 384 x 512 frames
constexpr float kOpenX = kOx + 374.0f * kSX;         // OpenMailFrame: TOPLEFT at InboxFrame's TOPRIGHT -10
float MX(float u) { return kOx + u * kSX; }
float MY(float u) { return kOy + u * kSY; }
float OX(float u) { return kOpenX + u * kSX; }
float BY(float fromBottom) { return MY(512.0f - fromBottom); }  // an anchor to the frame's BOTTOM

constexpr int kInboxPerPage = 7;            // INBOXITEMS_TO_DISPLAY
constexpr int kSendMax = 12, kSendPerRow = 7, kSendMaxRows = 2;  // ATTACHMENTS_MAX_SEND / _PER_ROW_SEND / _MAX_ROWS_SEND
constexpr int kRecvPerRow = 7;              // ATTACHMENTS_PER_ROW_RECEIVE
constexpr uint32_t kMaxCodCopper = 10000u * 10000u;  // MAX_COD_AMOUNT * COPPER_PER_GOLD
constexpr uint32_t kPostage = 30;           // GetSendMailPrice: 30 copper a letter / an attachment (the cores)
constexpr int kOskOwner = 0x4D41;           // "MA": the mail frame's keyboard (psp_osk oskBeginFor)
constexpr uint32_t kColMailText = 0xFF0F1F2Eu;  // MailTextFontNormal (0.18, 0.12, 0.06)
constexpr uint32_t kColReadGrey = 0xFFBFBFBFu;  // (0.75, 0.75, 0.75)
constexpr uint32_t kColGreenFont = 0xFF1AFF1Au; // GREEN_FONT_COLOR (0.1, 1, 0.1)
constexpr uint32_t kColRedFont = 0xFF1A1AFFu;   // RED_FONT_COLOR (1, 0.1, 0.1)
constexpr uint32_t kColDisable = 0xFF808080u;   // GRAY_FONT_COLOR / GameFontDisable
constexpr uint32_t kMailChecked = 0x04u;        // MAIL_CHECK_MASK_COPIED (textCreated)
constexpr uint32_t kMailReturned = 0x02u;       // MAIL_CHECK_MASK_RETURNED (wasReturned)

void loadTex(pp::Pak& pak, const char* name, pp::Texture& t) {
    if (t.pixels) return;
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
}

// SecondsToTime(seconds) (UIParent.lua): two units at most, abbreviated, TIME_UNIT_DELIMITER " ".
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

// InboxItemCanDelete: a player's letter with something attached goes back (MAIL_RETURN); the rest is deleted.
bool canDelete(const game::MailMessage& m) {
    return !(m.messageType == 0 && !(m.flags & kMailReturned) && (!m.attachments.empty() || m.money > 0 || m.cod > 0));
}

std::string trimmed(const std::string& s) {
    size_t a = 0, e = s.size();
    while (a < e && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (e > a && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(a, e - a);
}

// The UTF-8 string cut to `chars` characters (EditBox letters="N").
std::string clipChars(const std::string& s, int chars) {
    size_t i = 0;
    int n = 0;
    while (i < s.size() && n < chars) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        ++n;
    }
    return s.substr(0, std::min(i, s.size()));
}

}  // namespace

// --- Open / close, the session's events --------------------------------------------------

void WorldScreen::mailResetSend() {  // SendMailFrame_Reset
    BankMailUi& b = bankMail();
    b.to.clear();
    b.subject.clear();
    b.body.clear();
    b.previousItem.clear();
    b.gold = b.silver = b.copper = 0;
    b.codMode = false;
    b.maxRowsShown = 0;
}

void WorldScreen::syncMail() {
    BankMailUi& b = bankMail();
    pp::WorldSession::MailEvent ev;
    while (session_.takeMailEvent(ev)) {
        if (!b.mailShown) continue;
        if (std::strcmp(ev.name, "MAIL_SEND_SUCCESS") == 0) {
            // news -> a system chat line (the port's game line rule, psp_game_line.hpp; upstream
            // InventoryHandler: a system chat line too).
            session_.gameLine(pp::GameLine::News, CLIENT_STR(ERR_MAIL_SENT));
            mailResetSend();
            playSoundByName("igAbiliityPageTurn");
            if (b.replyMode) b.tab = 1;  // "reply": back to the inbox
            b.replyMode = false;
        } else if (std::strcmp(ev.name, "MAIL_FAILED") == 0 || std::strcmp(ev.name, "MAIL_SUCCESS") == 0) {
            b.sendDisabled = false;  // SendMailMailButton:Enable()
        } else if (std::strcmp(ev.name, "CLOSE_INBOX_ITEM") == 0) {
            if (b.openMailId != 0 && static_cast<int64_t>(b.openMailId) == ev.arg) {
                b.openMailId = 0;
                closeWindow(kWinOpenMail);
            }
        }
    }
    const bool want = session_.isMailboxOpen();
    if (want && !b.mailShown) {
        // MAIL_SHOW: ShowUIPanel(MailFrame), OpenBackpack, SendMailFrame_Update, MailFrameTab_OnClick(1), CheckInbox.
        b.mailShown = true;
        b.tab = 1;
        b.page = 1;
        b.openMailId = 0;
        b.popup = kMailPopupNone;
        b.sendDisabled = false;
        b.replyMode = false;
        mailResetSend();
        b.guard = true;
        if (!uiMode_) {
            setUiMode(true);
            b.raisedCursor = true;
            suppress_ = ~0u;
        }
        b.mailOpenedBags = !windowOpen(kWinBags);
        if (b.mailOpenedBags) openBags(false);
        openWindow(kWinMail);  // on top: it has the focus
        winFocus_.setFocus(kIdMailRow0);
        playSoundByName("igSpellBookOpen");
        PSP_LOGI(pp::tag::Ctrl, "mail_open bags=%d", b.mailOpenedBags ? 1 : 0);
    } else if (want && b.mailShown && !windowOpen(kWinMail)) {
        session_.closeMailbox();  // START / the close button / CANCEL: CloseMail (the rest below)
    } else if (!want && b.mailShown) {
        // MAIL_CLOSED -> HideUIPanel(MailFrame) -> OnHide: CloseMail, HideUIPanel(OpenMailFrame), the edit boxes
        // emptied, igCharacterInfoClose.
        b.mailShown = false;
        b.openMailId = 0;
        b.popup = kMailPopupNone;
        closeWindow(kWinMail);
        closeWindow(kWinOpenMail);
        closeWindow(kWinMailPopup);
        mailResetSend();
        if (b.mailOpenedBags && windowOpen(kWinBags)) closeWindow(kWinBags);
        b.mailOpenedBags = false;
        const uint32_t kb = b.artBytes() / 1024u;
        b.releaseMailArt();
        b.ahNames.clear();
        b.ahTried = false;
        playSoundByName("igCharacterInfoClose");
        PSP_LOGI(pp::tag::Ctrl, "mail_close kb=%u", static_cast<unsigned>(kb));
        // MailFrame's OnHide: the client asks for the next mail time (the minimap's envelope).
        session_.queryNextMailTime();
        return;
    }
    if (!b.mailShown) return;
    // OpenMailFrame closed by START / its close buttons: OpenMailFrame_OnHide.
    if (b.openMailId != 0 && !windowOpen(kWinOpenMail)) {
        const auto& inbox = session_.getMailInbox();
        auto it = std::find_if(inbox.begin(), inbox.end(),
                               [&](const game::MailMessage& m) { return m.messageId == b.openMailId; });
        if (it != inbox.end()) {
            // "If mail contains no items, then delete it on close" (money 0, nothing attached, its text copied,
            // not an auction's pending sale).
            game::AuctionMailInvoice inv;
            const bool invoice = it->messageType == 2 && game::parseAuctionMailBody(it->body, inv);
            if (it->money == 0 && it->attachments.empty() && (it->flags & kMailChecked) && !invoice)
                session_.mailDelete(it->messageId);
        }
        b.openMailId = 0;
        closeWindow(kWinMailPopup);
        playSoundByName("igSpellBookClose");
    }
    // The open letter is gone from the list (deleted / returned / taken by the server): its frame goes too.
    if (b.openMailId != 0) {
        const auto& inbox = session_.getMailInbox();
        if (std::none_of(inbox.begin(), inbox.end(), [&](const game::MailMessage& m) { return m.messageId == b.openMailId; })) {
            b.openMailId = 0;
            closeWindow(kWinOpenMail);
        }
    }
    // A popup START took away: its OnCancel (SEND_MONEY enables the Send button again).
    if (b.popup != kMailPopupNone && !windowOpen(kWinMailPopup)) {
        if (b.popup == kMailPopupSendMoney) b.sendDisabled = false;
        b.popup = kMailPopupNone;
    }
    // SendMailFrame_Update: the subject from the first attachment, C.O.D. only with one.
    {
        const auto& atts = session_.getMailAttachments();
        int count = 0;
        std::string itemTitle;
        for (const auto& a : atts) {
            if (!a.occupied()) continue;
            ++count;
            if (itemTitle.empty()) {
                const auto* info = session_.getItemInfo(a.item.itemId);
                const std::string name = info && info->valid ? info->name : a.item.name;
                char cnt[16];
                std::snprintf(cnt, sizeof(cnt), " (%u)", static_cast<unsigned>(a.item.stackCount));
                itemTitle = a.item.stackCount <= 1 ? name : name + cnt;
            }
        }
        if (count > 0) {
            if (b.subject.empty() || b.subject == b.previousItem) {
                b.subject = itemTitle;
                b.previousItem = itemTitle;
            }
        } else {
            if (!b.previousItem.empty() && b.subject == b.previousItem) b.subject.clear();
            b.previousItem.clear();
            b.codMode = false;  // SendMailRadioButton_OnClick(1), SendMailCODButton:Disable()
        }
    }
}

// The PSP keyboard's text for the box it was opened for (mailOskTick runs after the chat polled its own).
void WorldScreen::mailOskTick() {
    BankMailUi& b = bankMail();
    bool accepted = false;
    std::string text;
    if (!pp::oskFinishedFor(kOskOwner, accepted, text)) return;
    PSP_LOGI(pp::tag::Ctrl, "mail_osk field=%X ok=%d len=%u", static_cast<unsigned>(b.oskField), accepted ? 1 : 0,
             static_cast<unsigned>(text.size()));
    if (!accepted || !b.mailShown) return;
    auto digits = [](const std::string& s, uint32_t max) {
        uint32_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') continue;
            v = v * 10u + static_cast<uint32_t>(c - '0');
            if (v > max) v = max;
        }
        return v;
    };
    switch (b.oskField) {
        case kIdMailTo: b.to = clipChars(trimmed(text), 12); break;     // letters="12"
        case kIdMailSubject: b.subject = clipChars(text, 64); break;  // letters="64"
        case kIdMailBody: b.body = clipChars(text, 500); break;       // letters="500"
        case kIdMailGold: b.gold = digits(text, 999999); break;
        case kIdMailSilver: b.silver = digits(text, 99); break;       // letters="2"
        case kIdMailCopper: b.copper = digits(text, 99); break;
        default: break;
    }
    b.oskField = 0;
}

// L + up (ui_world_npc.cpp interactTarget): the nearest mailbox in upstream's game object reach (10 yd), when
// no NPC is nearer - the right click on it.
bool WorldScreen::tryUseMailbox(float npcDist) {
    uint64_t guid = 0;
    float dist = 0.0f;
    if (!session_.nearestMailbox(glm::vec3(playerX_, playerY_, playerZ_), 10.0f, guid, dist)) return false;
    if (dist >= npcDist) return false;
    if (npc_.range > 0.0f && dist > npc_.range) {  // PSP: as for an NPC (npcrange): the server refuses silently
        npc_.error = CLIENT_STR(ERR_OUT_OF_RANGE);
        npc_.errorUntilMs = pp::logUptimeMs() + 2000u;
        return true;
    }
    session_.useMailbox(guid);
    PSP_LOGI(pp::tag::Net, "mailbox_use guid=%08X dist=%.1f", static_cast<unsigned>(guid & 0xFFFFFFFFu), dist);
    return true;
}

// --- Drawing -----------------------------------------------------------------------------

bool WorldScreen::drawMailFrame(UiRenderer& ui) {
    BankMailUi& b = bankMail();
    PROF_SCOPE("mail.frame");
    // The art: the shown tab's frame (one at a time), the small parts, the auction houses' names.
    if (b.mailFrameTab != b.tab || !b.smallTried) {
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            if (b.mailFrameTab != b.tab) {
                if (b.mailFrame.pixels) pp::textureRelease(b.mailFrame);
                loadTex(pak, b.tab == 1 ? "mail_inbox" : "mail_send", b.mailFrame);
            }
            if (!b.smallTried) {
                loadTex(pak, "mail_bar", b.bar);
                loadTex(pak, "mail_scroll", b.scroll);
                loadTex(pak, "mail_slotw", b.slotWhite);
                loadTex(pak, "mail_slotbg", b.slotBg);
                loadTex(pak, "mail_letter", b.letter);
                loadTex(pak, "mail_radio", b.radio);
                loadTex(pak, "mail_alert", b.alert);
                loadTex(pak, "mail_coin_g", b.coin[0]);
                loadTex(pak, "mail_coin_s", b.coin[1]);
                loadTex(pak, "mail_coin_c", b.coin[2]);
                loadTex(pak, "sb_checked", b.checked);
                loadTex(pak, "sb_prev", b.prev);
                loadTex(pak, "sb_prev_dis", b.prevDis);
                loadTex(pak, "sb_next", b.next);
                loadTex(pak, "sb_next_dis", b.nextDis);
            }
            if (!b.ahTried) {  // AuctionHouse.dbc ruRU: "id\tname\n"
                b.ahTried = true;
                const pp::PakEntry* e = pak.find("mail_ah");
                if (char* raw = e ? static_cast<char*>(pak.load(e)) : nullptr) {
                    const std::string all(raw, e->size);
                    pp::pakFree(raw);
                    size_t at = 0;
                    while (at < all.size()) {
                        const size_t nl = all.find('\n', at);
                        const std::string line = all.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                        const size_t tab = line.find('\t');
                        if (tab != std::string::npos)
                            b.ahNames[static_cast<uint32_t>(std::atoi(line.c_str()))] = line.substr(tab + 1);
                        if (nl == std::string::npos) break;
                        at = nl + 1;
                    }
                }
            }
            pak.close();
        }
        b.mailFrameTab = b.tab;
        b.smallTried = true;
        PSP_LOGI(pp::tag::Gfx, "mail_art tab=%d frame=%d small=%d kb=%u", b.tab, b.mailFrame.pixels ? 1 : 0,
                 b.slotWhite.pixels ? 1 : 0, static_cast<unsigned>(b.artBytes() / 1024u));
    }
    if (!tabArtTried_) {  // CharacterFrameTabButtonTemplate (drawFrameTab's art; FriendsFrameTabTemplate is the same)
        tabArtTried_ = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            loadTex(pak, "tab_active", tabArt_[0]);
            loadTex(pak, "tab_inactive", tabArt_[1]);
            pak.close();
        }
    }
    loadNpcArt(false);  // the close button, the coins, the panel buttons
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight());
    auto seen = [&](uint32_t id) {
        if (winFocus_.isPressed(id)) b.guard = false;
    };
    auto stop = [&](uint32_t id, const Rect& r) {
        const bool f = focusable && winFocus_.addStop(id, r);
        if (f) seen(id);
        return f;
    };

    if (b.mailFrame.pixels) ui.image(b.mailFrame, Rect{kOx, kOy, kFrameW * kSX, kFrameH * kSY});
    else kit_.panel(ui, Rect{kOx, kOy, kFrameW * kSX, kFrameH * kSY});
    // InboxTitleText / SendMailTitleText (GameFontNormal, CENTER 6,232 / 6,230).
    ui.textShadow(fn, MX(198), MY(b.tab == 1 ? 24 : 26) - fn.lineHeight() * 0.5f,
                  b.tab == 1 ? CLIENT_STR(INBOX) : CLIENT_STR(SENDMAIL), kColGold, Align::Center);
    // InboxCloseButton (UIPanelCloseButton, CENTER at TOPRIGHT -45,-25).
    {
        const Rect cr{MX(339) - 9, MY(25) - 9, 18, 18};
        const bool f = stop(kIdMailClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdMailClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                  : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // MailFrameTab1 / 2 (BOTTOMLEFT 24,44; Tab2 at Tab1's RIGHT -8).
    {
        const float th = 32 * kSY, ty = MY(512 - 44) - th;
        float x = MX(24);
        x += drawFrameTab(ui, kIdMailTab1, x, ty, th, CLIENT_STR(INBOX), b.tab == 1, focusable) - 8 * kSX;
        drawFrameTab(ui, kIdMailTab2, x, ty, th, CLIENT_STR(SENDMAIL), b.tab == 2, focusable);
    }

    if (b.tab == 1) {
        // --- InboxFrame_Update ---
        const auto& inbox = session_.getMailInbox();
        const int numItems = static_cast<int>(inbox.size());
        const int pages = std::max(1, (numItems + kInboxPerPage - 1) / kInboxPerPage);
        b.page = std::clamp(b.page, 1, pages);
        const float ageDays = static_cast<float>(session_.mailListAgeSec()) / 86400.0f;
        if (session_.mailTotalCount() > static_cast<uint32_t>(numItems)) {  // InboxTooMuchMail (TOP 0,-38)
            const char* t = CLIENT_STR(INBOX_TOO_MUCH_MAIL);
            const float tw = static_cast<float>(fs.measure(t));
            const float cx = MX(192 + 16), cy = MY(54);
            ui.textShadow(fs, cx, cy - lh * 0.5f, t, kColRedFont, Align::Center);
            if (b.alert.pixels) ui.image(b.alert, Rect{cx - tw * 0.5f - 18, cy - 9, 18, 18});
        }
        for (int i = 0; i < kInboxPerPage; ++i) {
            const int index = (b.page - 1) * kInboxPerPage + i;
            if (index >= numItems) continue;  // the button hidden, the row's border stays (baked)
            const game::MailMessage& m = inbox[static_cast<size_t>(index)];
            const float rowTop = MY(80.0f + 45.0f * static_cast<float>(i));
            const uint32_t id = kIdMailRow0 + static_cast<uint32_t>(i);
            const Rect hit{MX(28), rowTop, 305 * kSX, 45 * kSY - 1};
            const bool f = stop(id, hit);
            const Rect btn{std::floor(MX(33)), std::floor(MY(83.0f + 45.0f * static_cast<float>(i))), 20, 20};
            // $parentButtonSlot (UI-EmptySlot-White 64 x 64, gold unread / grey read), $parentButtonIcon.
            if (b.slotWhite.pixels) {
                const float c = btn.w * 64.0f / 37.0f;
                ui.image(b.slotWhite, Rect{btn.x + btn.w * 0.5f - c * 0.5f, btn.y + btn.h * 0.5f - c * 0.5f, c, c},
                         m.read ? 0xFF808080u : 0xFF00D1FFu);
            }
            const uint32_t iconTint = m.read ? 0xFF808080u : 0xFFFFFFFFu;
            const pp::Texture* icon = nullptr;
            if (!m.attachments.empty()) {  // packageIcon: the first attachment
                const auto* info = session_.getItemInfo(m.attachments[0].itemId);
                if (info && info->valid) icon = itemIcons().byDisplay(info->displayInfoId, spellFrame_);
            }
            if (!icon && b.letter.pixels) icon = &b.letter;  // stationeryIcon (upstream INV_Letter_15)
            if (icon) ui.image(*icon, btn, iconTint);
            if (m.messageId == b.openMailId) {  // SetChecked: CheckButtonHilight
                if (b.checked.pixels) ui.image(b.checked, btn, 0xFFFFFFFFu, Blend::Add);
                else ui.fill(btn, 0x40FFFFFFu);
            }
            if (m.cod > 0) {  // $parentButtonCODBackground + CASH_ON_DELIVERY
                ui.fill(Rect{btn.x + 1, btn.y + 1, btn.w - 2, 7}, 0x66000000u);
                ui.textShadow(ft, btn.x + btn.w * 0.5f, btn.y, CLIENT_STR(CASH_ON_DELIVERY), kColWhite, Align::Center);
            }
            if (!m.attachments.empty() && m.attachments[0].stackCount > 1) {  // SetItemButtonCount(firstItemQuantity)
                char cnt[12];
                std::snprintf(cnt, sizeof(cnt), "%u", static_cast<unsigned>(m.attachments[0].stackCount));
                ui.textShadow(fs, btn.x + btn.w + 1, btn.y + btn.h - lh + 2, cnt, kColWhite, Align::Right);
            }
            // The time left (GREEN DAYS_ABBR, RED SecondsToTime under a day), TOPRIGHT -4,-4.
            const float daysLeft = m.expirationTime - ageDays;
            char exp[48];
            uint32_t expCol = kColGreenFont;
            if (daysLeft >= 1.0f) {
                std::snprintf(exp, sizeof(exp), "%s",
                              pp::text::plural(CLIENT_FMT(DAYS_ABBR, static_cast<int>(std::floor(daysLeft)))).c_str());
            } else {
                std::snprintf(exp, sizeof(exp), "%s", secondsToTime(std::max(0, static_cast<int>(daysLeft * 86400.0f))).c_str());
                expCol = kColRedFont;
            }
            ui.textShadow(fs, MX(329), rowTop + 2, exp, expCol, Align::Right);
            const float expW = static_cast<float>(fs.measure(exp));
            // $parentSender (GameFontNormal, gold / 0.75 grey) and $parentSubject (GameFontHighlightSmall).
            std::string sender = m.messageType == 2 ? std::string() : session_.getMailSenderName(m);
            if (m.messageType == 2) {
                auto it = b.ahNames.find(m.senderEntry);
                sender = it != b.ahNames.end() ? it->second : std::string(CLIENT_STR(UNKNOWN));
            }
            const Rect sr{MX(75), rowTop + 1, MX(329) - expW - 4 - MX(75), static_cast<float>(fn.lineHeight())};
            if (f && static_cast<float>(fn.measure(sender.c_str())) > sr.w) {
                ui.textMarquee(fn, sr, sender.c_str(), m.read ? kColReadGrey : kColGold);
            } else {
                ui.pushClip(sr);
                ui.textShadow(fn, sr.x, sr.y, sender.c_str(), m.read ? kColReadGrey : kColGold);
                ui.popClip();
            }
            const std::string subject = session_.getMailDisplaySubject(m);
            const Rect jr{MX(75), rowTop + 13, 248 * kSX, lh};
            if (f && static_cast<float>(fs.measure(subject.c_str())) > jr.w) {
                ui.textMarquee(fs, jr, subject.c_str(), m.read ? kColReadGrey : kColWhite);
            } else {
                ui.pushClip(jr);
                ui.textShadow(fs, jr.x, jr.y, subject.c_str(), m.read ? kColReadGrey : kColWhite);
                ui.popClip();
            }
            if (f) {
                ui.fill(btn, 0x50FFFFFFu);  // ButtonHilight-Square
                // InboxFrameItem_OnEnter (PSP: one of the item / the money / the C.O.D.).
                b.tipRect = Rect{btn.x + ui.offsetX(), btn.y + ui.offsetY(), btn.w, btn.h};
                if (m.money > 0) {
                    b.tipTitle = CLIENT_STR(ENCLOSED_MONEY);
                    b.tipMoney = static_cast<uint32_t>(m.money);
                    b.tipMoneyRed = false;
                } else if (m.cod > 0) {
                    b.tipTitle = CLIENT_STR(COD_AMOUNT);
                    b.tipMoney = static_cast<uint32_t>(m.cod);
                    b.tipMoneyRed = m.cod > session_.getMoneyCopper();
                } else if (m.attachments.size() == 1) {
                    b.tipProto = m.attachments[0].itemId;
                    b.tipCount = m.attachments[0].stackCount;
                } else if (m.attachments.size() > 1) {
                    b.tipTitleBuf = std::string(CLIENT_STR(MAIL_MULTIPLE_ITEMS)) + " (" +
                                    std::to_string(m.attachments.size()) + ")";  // MAIL_MULTIPLE_ITEMS.." ("..n..")"
                    b.tipTitle = b.tipTitleBuf.c_str();
                }
            }
        }
        // InboxPrevPageButton / InboxNextPageButton (CENTER at BOTTOMLEFT 50,104 / 314,104).
        auto pageBtn = [&](uint32_t id, float cxu, bool enabled, const pp::Texture& up, const pp::Texture& dis) {
            const Rect r{MX(cxu) - 9, BY(104) - 9, 18, 18};
            const bool f = enabled && stop(id, r);
            const pp::Texture& t = enabled ? up : dis;
            if (t.pixels) ui.image(t, r);
            if (f) ui.fill(r, 0x40FFFFFFu);
            return r;
        };
        const bool prevOn = b.page > 1, nextOn = b.page * kInboxPerPage < numItems;
        const Rect pr = pageBtn(kIdMailPrev, 50, prevOn, b.prev, b.prevDis);
        const Rect nr = pageBtn(kIdMailNext, 314, nextOn, b.next, b.nextDis);
        ui.textShadow(fs, pr.x + pr.w + 1, pr.y + (pr.h - lh) * 0.5f, CLIENT_STR(PREV), prevOn ? kColGold : kColDisable);
        ui.textShadow(fs, nr.x - 1, nr.y + (nr.h - lh) * 0.5f, CLIENT_STR(NEXT), nextOn ? kColGold : kColDisable,
                      Align::Right);
        return true;
    }

    // --- SendMailFrame ---
    const auto& atts = session_.getMailAttachments();
    int itemCount = 0, last = 0;
    bool gap = false;
    for (int i = 0; i < kSendMax; ++i) {
        if (!atts[static_cast<size_t>(i)].occupied()) continue;
        ++itemCount;
        if (last + 1 != i + 1) gap = true;
        last = i + 1;
    }
    // SendMailCostMoneyFrame (TOPRIGHT -36,-48): GetSendMailPrice, red over GetMoney; SEND_MAIL_COST to its left.
    {
        const uint32_t price = kPostage * static_cast<uint32_t>(std::max(1, itemCount));
        const bool red = price > session_.getMoneyCopper();
        const float w = drawCoins(ui, fs, npcArt_.money, MX(348), MY(48), price, true, red ? kColRed : kColWhite);
        ui.textShadow(fs, MX(348) - w - 3 * kSX, MY(48), CLIENT_STR(SEND_MAIL_COST), kColGold, Align::Right);
    }
    // The edit boxes (their borders are baked): MAIL_TO_LABEL SendMailNameEditBox (105,-46, 109 wide), MAIL_SUBJECT_LABEL
    // SendMailSubjectEditBox (3 below, 220 wide). ✕ = the PSP keyboard.
    auto editBox = [&](uint32_t id, const char* label, const std::string& text, float x0, float x1, float y0) {
        const Rect r{MX(x0 - 8), MY(y0), (x1 - x0 + 16) * kSX, 20 * kSY};
        const bool f = stop(id, r);
        ui.textShadow(fn, MX(x0 - 12), MY(y0 + 10) - fn.lineHeight() * 0.5f, label, kColGold, Align::Right);
        const Rect tr{MX(x0), MY(y0 + 10) - lh * 0.5f, (x1 - x0) * kSX, lh};
        if (f && static_cast<float>(fs.measure(text.c_str())) > tr.w) {
            ui.textMarquee(fs, tr, text.c_str(), kColWhite);
        } else {
            ui.pushClip(tr);
            ui.textShadow(fs, tr.x, tr.y, text.c_str(), kColWhite);
            ui.popClip();
        }
        if (f) ui.fill(r, 0x30FFFFFFu);
    };
    editBox(kIdMailTo, CLIENT_STR(MAIL_TO_LABEL), b.to, 105, 214, 46);
    editBox(kIdMailSubject, CLIENT_STR(MAIL_SUBJECT_LABEL), b.subject, 105, 325, 69);
    // SendMailFrame_Update: the attachment rows (one, the second once the seventh is used) and maxRowsShown.
    int rows = 1, temp = last;
    while (temp > kSendPerRow && rows < kSendMaxRows) {
        ++rows;
        temp -= kSendPerRow;
    }
    if (!gap && temp == kSendPerRow && rows < kSendMaxRows) ++rows;
    if (b.maxRowsShown > 0 && last > 0 && rows < b.maxRowsShown) rows = b.maxRowsShown;
    else b.maxRowsShown = rows;
    const float areay = 12.0f + 5.0f * static_cast<float>(rows - 1) + 39.0f * static_cast<float>(rows);
    const float scrollH = 249.0f - areay;
    // The letter's paper (SendStationeryBackgroundLeft / Right: the selected stationery, the default one).
    if (b.paperSendName[0] == '\0') {
        b.paperSendName = "mail_paper_stationerytest";
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            loadTex(pak, b.paperSendName, b.paperSend);
            pak.close();
        }
    }
    if (b.paperSend.pixels)
        ui.image(b.paperSend, Rect{MX(21), MY(97), 316 * kSX, scrollH * kSY},
                 SrcRect{0, 0, static_cast<float>(b.paperSend.uw), static_cast<float>(b.paperSend.uh) * scrollH / 256.0f});
    // SendScrollBarBackgroundTop (TOPRIGHT -2,5 of the scroll frame) and its bottom (BOTTOMRIGHT -2,-2).
    if (b.scroll.pixels) {
        const float tu = b.scroll.uh * 141.0f / 199.0f;  // the atlas: the top 141 rows, the bottom 58 under it
        ui.image(b.scroll, Rect{MX(315), MY(92), 31 * kSX, std::min(scrollH, 256.0f) * kSY},
                 SrcRect{0, 0, static_cast<float>(b.scroll.uw), tu * std::min(scrollH, 256.0f) / 256.0f});
        ui.image(b.scroll, Rect{MX(315), MY(97 + scrollH + 2 - 106), 31 * kSX, 106 * kSY},
                 SrcRect{0, tu, static_cast<float>(b.scroll.uw), static_cast<float>(b.scroll.uh) - tu});
    }
    // SendMailHorizontalBarLeft2 (TOPLEFT at BOTTOMLEFT 15, 170 + areay).
    if (b.bar.pixels) ui.image(b.bar, Rect{MX(15), BY(170 + areay), 331 * kSX, 16 * kSY});
    // SendMailBodyEditBox (20,-10 in the scroll child, 270 wide): MailTextFontNormal on the paper; ✕ = the keyboard.
    {
        const Rect area{MX(21), MY(97), 296 * kSX, scrollH * kSY};
        const bool f = stop(kIdMailBody, area);
        const auto lines = wrapQuestText(fs, b.body, 270 * kSX);
        float y = MY(107);
        for (const auto& l : lines) {
            if (y + lh > MY(97 + scrollH)) break;
            ui.text(fs, MX(41), y, l.c_str(), kColMailText);
            y += lh;
        }
        if (f) ui.fill(area, 0x20FFFFFFu);
    }
    // SendMailAttachment1..12: 7 a row, rows from the top (cursory = rows - 1 .. 0).
    {
        const int marginxl = 31, marginxr = 46, areax = 384 - marginxl - marginxr;
        const int iconx = 39, gapx1 = (areax - iconx * kSendPerRow) / (kSendPerRow - 1);
        const int gapx2 = (areax - iconx * kSendPerRow - gapx1 * (kSendPerRow - 1)) / 2;
        const float indentx = static_cast<float>(marginxl + gapx2), indenty = 156.0f + 6.0f + 39.0f;
        const float tabx = static_cast<float>(iconx + gapx1), taby = 39.0f + 5.0f;
        int cursorx = 0, cursory = rows - 1;
        for (int i = 0; i < kSendMax; ++i) {
            if (cursory < 0) break;
            const float ux = indentx + tabx * static_cast<float>(cursorx);
            const float uyTop = 512.0f - (indenty + taby * static_cast<float>(cursory));
            const Rect r{std::floor(MX(ux)), std::floor(MY(uyTop)), 20, 20};
            if (b.slotBg.pixels) ui.image(b.slotBg, Rect{r.x - 1, r.y - 1, 22, 22});
            const auto& a = atts[static_cast<size_t>(i)];
            const uint32_t id = kIdMailAttach0 + static_cast<uint32_t>(i);
            const bool f = stop(id, r);
            if (a.occupied()) {
                const auto* info = session_.getItemInfo(a.item.itemId);
                const uint32_t disp = info && info->valid ? info->displayInfoId : a.item.displayInfoId;
                if (const pp::Texture* t = itemIcons().byDisplay(disp, spellFrame_)) ui.image(*t, r);
                if (a.item.stackCount > 1) {
                    char cnt[12];
                    std::snprintf(cnt, sizeof(cnt), "%u", static_cast<unsigned>(a.item.stackCount));
                    ui.textShadow(fs, r.x + r.w + 1, r.y + r.h - lh + 2, cnt, kColWhite, Align::Right);
                }
            }
            if (f) {
                ui.fill(r, 0x50FFFFFFu);
                b.tipRect = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};
                if (a.occupied()) {
                    b.tipProto = a.item.itemId;
                    b.tipCount = a.item.stackCount;
                } else {
                    b.tipTitle = CLIENT_STR(ATTACHMENT_TEXT);
                }
            }
            if (++cursorx >= kSendPerRow) {
                --cursory;
                cursorx = 0;
            }
        }
    }
    // SendMailMoneyButton (37 x 37 at BOTTOMLEFT 30,110): the label, the amount, the radio buttons.
    {
        // MoneyInputFrame at SendMailMoneyText's BOTTOMLEFT 5,-3 = 35,-383 (its boxes and coins are baked there):
        // gold 58 wide, silver 30 (+26), copper 30 (+16). PSP: the label (GameFontNormalSmall) in the 8 px font
        // right over the boxes - the 10 px one would run into them.
        const float boxY = MY(383);
        const float textY = boxY - static_cast<float>(ft.lineHeight()) - 1.0f;
        ui.textShadow(ft, MX(30), textY, b.codMode ? CLIENT_STR(COD_AMOUNT) : CLIENT_STR(AMOUNT_TO_SEND), kColGold);
        const uint32_t copper = b.gold * 10000u + b.silver * 100u + b.copper;
        if (b.codMode && copper > kMaxCodCopper)  // SendMailErrorText: MAIL_COD_ERROR (BOTTOMLEFT at the label's TOPLEFT 0,2)
            ui.textShadow(ft, MX(30), textY - static_cast<float>(ft.lineHeight()) - 1.0f,
                          CLIENT_FMT(MAIL_COD_ERROR, 10000).c_str(), kColRedFont);  // MAX_COD_AMOUNT
        auto moneyBox = [&](uint32_t id, uint32_t value, float x0, float w) {
            const Rect r{MX(x0 - 5), boxY, (w + 5) * kSX, 20 * kSY};
            const bool f = stop(id, r);
            char v[16];
            std::snprintf(v, sizeof(v), "%u", static_cast<unsigned>(value));
            ui.textShadow(fs, MX(x0), boxY + (r.h - lh) * 0.5f, v, kColWhite);
            if (f) ui.fill(r, 0x30FFFFFFu);
        };
        moneyBox(kIdMailGold, b.gold, 35, 58);
        moneyBox(kIdMailSilver, b.silver, 35 + 58 + 26, 20);
        moneyBox(kIdMailCopper, b.copper, 35 + 58 + 26 + 30 + 16, 20);
        // SendMailSendMoneyButton (TOPRIGHT of the amount +0,12) / SendMailCODButton (1 under it): UI-RadioButton.
        auto radio = [&](uint32_t id, float y, bool checked, bool enabled, const char* label) {
            const Rect r{MX(211), y, 9, 9};
            const bool f = enabled && stop(id, Rect{r.x - 1, r.y - 1, 90, r.h + 2});
            if (b.radio.pixels) {
                const float q = b.radio.uw * 0.25f;
                ui.image(b.radio, r, SrcRect{checked ? q : 0.0f, 0, q, static_cast<float>(b.radio.uh)},
                         enabled ? 0xFFFFFFFFu : 0xFF808080u);
                if (f) ui.image(b.radio, r, SrcRect{q * 2, 0, q, static_cast<float>(b.radio.uh)}, 0xFFFFFFFFu, Blend::Add);
            } else {
                ui.fill(r, checked ? kColGold : 0xFF404040u);
            }
            ui.textShadow(ft, r.x + r.w + 5 * kSX, r.y + (r.h - static_cast<float>(ft.lineHeight())) * 0.5f, label,
                          enabled ? kColGold : kColDisable);
        };
        const float ry = boxY - 12 * kSY;
        radio(kIdMailRadioMoney, ry, !b.codMode, true, CLIENT_STR(SEND_MONEY));
        radio(kIdMailRadioCod, ry + 15 * kSY, b.codMode, itemCount > 0, CLIENT_STR(COD));
    }
    // SendMailMoneyFrame (BOTTOMRIGHT at BOTTOMLEFT 183,84): the player's money.
    drawMoneyFrame(ui, MX(183), BY(84) - 11.0f, static_cast<uint32_t>(session_.getMoneyCopper()), true);
    // SendMailFrame_CanSend: a stationery (always, the default), a name, a subject (+ C.O.D. within MAX_COD_AMOUNT).
    {
        int checks = 1, required = 3;
        if (!trimmed(b.to).empty()) ++checks;
        if (!b.subject.empty()) ++checks;
        if (b.codMode) {
            required = 4;
            if (b.gold * 10000u + b.silver * 100u + b.copper <= kMaxCodCopper) ++checks;
        }
        const bool canSend = checks == required && !b.sendDisabled;
        // SendMailCancelButton (80 x 22 at BOTTOMRIGHT -39,80), SendMailMailButton left of it.
        const float by = BY(80 + 22), bh = 22 * kSY + 1;
        drawPanelButton(ui, kIdMailSend, Rect{MX(384 - 39 - 160), by, 80 * kSX, bh}, CLIENT_STR(SEND_LABEL), canSend,
                        focusable);
        drawPanelButton(ui, kIdMailCancel, Rect{MX(384 - 39 - 80), by, 80 * kSX, bh}, CLIENT_STR(CANCEL), true, focusable);
        if (focusable && (winFocus_.isPressed(kIdMailSend) || winFocus_.isPressed(kIdMailCancel))) b.guard = false;
    }
    return true;
}

bool WorldScreen::drawOpenMailFrame(UiRenderer& ui) {
    BankMailUi& b = bankMail();
    PROF_SCOPE("mail.open");
    const auto& inbox = session_.getMailInbox();
    auto it = std::find_if(inbox.begin(), inbox.end(),
                           [&](const game::MailMessage& m) { return m.messageId == b.openMailId; });
    if (it == inbox.end()) return true;  // gone: syncMail closes the frame
    const game::MailMessage& m = *it;
    const char* paper = game::stationeryTexture(m.stationeryId, m.messageType);
    char paperName[48];
    std::snprintf(paperName, sizeof(paperName), "mail_paper_%s", paper);
    for (char* c = paperName; *c; ++c) *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    if (!b.openTried || std::strcmp(b.paperOpenName, paperName) != 0) {
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            if (!b.openTried) loadTex(pak, "mail_open", b.openFrame);
            if (b.paperOpen.pixels) pp::textureRelease(b.paperOpen);
            loadTex(pak, paperName, b.paperOpen);
            if (!b.paperOpen.pixels) loadTex(pak, "mail_paper_stationerytest", b.paperOpen);
            pak.close();
        }
        b.openTried = true;
        static char s_name[48];  // the paper's pak name (one open letter at a time)
        std::snprintf(s_name, sizeof(s_name), "%s", paperName);
        b.paperOpenName = s_name;
        PSP_LOGI(pp::tag::Gfx, "mail_art open=%d paper=%s kb=%u", b.openFrame.pixels ? 1 : 0, paperName,
                 static_cast<unsigned>(b.artBytes() / 1024u));
    }
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    auto stop = [&](uint32_t id, const Rect& r) {
        const bool f = focusable && winFocus_.addStop(id, r);
        if (f && winFocus_.isPressed(id)) b.guard = false;
        return f;
    };
    if (b.openFrame.pixels) ui.image(b.openFrame, Rect{kOpenX, kOy, kFrameW * kSX, kFrameH * kSY});
    else kit_.panel(ui, Rect{kOpenX, kOy, kFrameW * kSX, kFrameH * kSY});
    ui.textShadow(fn, OX(198), MY(24) - fn.lineHeight() * 0.5f, CLIENT_STR(OPENMAIL), kColGold, Align::Center);
    {  // OpenMailCloseButton (CENTER at TOPRIGHT -47,-25)
        const Rect cr{OX(337) - 9, MY(25) - 9, 18, 18};
        const bool f = stop(kIdOpenClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdOpenClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                  : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // FROM / MAIL_SUBJECT_LABEL (GameFontHighlight, TOPRIGHT at TOPLEFT 114,-45 / -65), the sender (GameFontNormal) and the
    // subject (GameFontNormalSmall) 5 to their right, 225 wide.
    std::string sender;
    if (m.messageType == 2) {
        auto ah = b.ahNames.find(m.senderEntry);
        sender = ah != b.ahNames.end() ? ah->second : std::string(CLIENT_STR(UNKNOWN));
    } else {
        sender = session_.getMailSenderName(m);
    }
    const std::string subject = session_.getMailDisplaySubject(m);
    ui.textShadow(fs, OX(114), MY(53) - lh * 0.5f, CLIENT_STR(FROM), kColWhite, Align::Right);
    ui.textShadow(fs, OX(114), MY(73) - lh * 0.5f, CLIENT_STR(MAIL_SUBJECT_LABEL), kColWhite, Align::Right);
    {
        const Rect sr{OX(119), MY(53) - lh * 0.5f, 225 * kSX, lh};
        ui.pushClip(sr);
        ui.textShadow(fs, sr.x, sr.y, sender.c_str(), kColGold);
        ui.popClip();
        const Rect jr{OX(119), MY(73) - lh * 0.5f, 225 * kSX, lh};
        if (static_cast<float>(fs.measure(subject.c_str())) > jr.w) ui.textMarquee(fs, jr, subject.c_str(), kColGold);
        else ui.textShadow(fs, jr.x, jr.y, subject.c_str(), kColGold);
    }
    // GetInboxText: the body, isTakeable / textCreated (the letter button), isInvoice (an auction's).
    game::AuctionMailInvoice invoice;
    const bool isInvoice = m.messageType == 2 && game::parseAuctionMailBody(m.body, invoice);
    const bool letterTakeable = !m.body.empty() && !isInvoice;
    const bool textCreated = (m.flags & kMailChecked) != 0;
    // OpenMailFrame_UpdateButtonPositions (once when the letter opens; taking an item does not move the rest).
    if (b.openUpdatePositions) {
        b.openUpdatePositions = false;
        b.openButtons.clear();
        b.openRows.clear();
        int rowCount = 0;
        if (letterTakeable && !textCreated) {
            b.openButtons.push_back(-2);
            ++rowCount;
        }
        if (m.money > 0) {
            b.openButtons.push_back(-1);
            ++rowCount;
        }
        for (const auto& a : m.attachments) {
            b.openButtons.push_back(static_cast<int>(a.slot));
            if (++rowCount >= kRecvPerRow) {
                b.openRows.emplace_back(0.0f, static_cast<float>(kRecvPerRow - 1));
                rowCount = 0;
            }
        }
        if (rowCount > 0) {
            const float xs = static_cast<float>(kRecvPerRow - rowCount) * 0.5f;
            b.openRows.emplace_back(xs, xs + static_cast<float>(rowCount - 1));
        }
    }
    // OpenMail_GetItemCounts: what is still there.
    auto attachmentBySlot = [&](int slot) -> const game::MailAttachment* {
        for (const auto& a : m.attachments)
            if (static_cast<int>(a.slot) == slot) return &a;
        return nullptr;
    };
    int buttonCount = (letterTakeable && !textCreated ? 1 : 0) + (m.money > 0 ? 1 : 0) +
                      static_cast<int>(m.attachments.size());
    const int rows = static_cast<int>(b.openRows.size());
    // The layout of OpenMail_Update: marginxl 27, gapx1 6, the rows from the bottom (indenty 106).
    const float icony = 39.0f, gapy1 = 3.0f, gapy2 = 3.0f, textH = 10.0f;
    float areay = gapy2 + textH + gapy2 + icony * static_cast<float>(rows) + gapy1 * static_cast<float>(rows - 1) + gapy2;
    float scrollH = 305.0f - areay;
    if (scrollH > 256.0f) {
        scrollH = 256.0f;
        areay = 305.0f - scrollH;
    }
    if (b.paperOpen.pixels)
        ui.image(b.paperOpen, Rect{OX(21), MY(97), 316 * kSX, scrollH * kSY},
                 SrcRect{0, 0, static_cast<float>(b.paperOpen.uw), static_cast<float>(b.paperOpen.uh) * scrollH / 256.0f});
    if (b.scroll.pixels) {
        const float tu = b.scroll.uh * 141.0f / 199.0f;
        ui.image(b.scroll, Rect{OX(315), MY(92), 31 * kSX, scrollH * kSY},
                 SrcRect{0, 0, static_cast<float>(b.scroll.uw), tu * scrollH / 256.0f});
        ui.image(b.scroll, Rect{OX(315), MY(97 + scrollH + 2 - 106), 31 * kSX, 106 * kSY},
                 SrcRect{0, tu, static_cast<float>(b.scroll.uw), static_cast<float>(b.scroll.uh) - tu});
    }
    if (b.bar.pixels) ui.image(b.bar, Rect{OX(15), BY(114 + areay), 331 * kSX, 16 * kSY});  // OpenMailHorizontalBarLeft
    // OpenMailBodyText (10,-10 in the scroll child, 276 wide, MailTextFontNormal); ✕ pages it (PSP).
    {
        const Rect area{OX(21), MY(97), 296 * kSX, scrollH * kSY};
        const auto lines = isInvoice ? std::vector<std::string>{} : wrapQuestText(fs, m.body, 276 * kSX);
        const int perPage = std::max(1, static_cast<int>((scrollH - 20.0f) * kSY / lh));
        const int pages = std::max(1, (static_cast<int>(lines.size()) + perPage - 1) / perPage);
        if (b.bodyPage >= pages) b.bodyPage = 0;
        const bool f = pages > 1 && stop(kIdOpenBody, area);
        float y = MY(107);
        for (int k = b.bodyPage * perPage; k < static_cast<int>(lines.size()) && k < (b.bodyPage + 1) * perPage; ++k) {
            ui.text(fs, OX(31), y, lines[static_cast<size_t>(k)].c_str(), kColMailText);
            y += lh;
        }
        if (pages > 1) {  // the scroll frame's arrows: more above / below
            const Rect up{OX(318), MY(97), 12, 12}, dn{OX(318), MY(97 + scrollH) - 12, 12, 12};
            const pp::Texture& tu = b.bodyPage > 0 ? npcArt_.scrUp : npcArt_.scrUpDis;
            const pp::Texture& td = b.bodyPage + 1 < pages ? npcArt_.scrDown : npcArt_.scrDownDis;
            if (tu.pixels) ui.image(tu, up);
            if (td.pixels) ui.image(td, dn);
        }
        if (f) ui.fill(area, 0x18FFFFFFu);
    }
    if (isInvoice) drawAuctionInvoice(ui, m, OX(21), MY(97), kSX, kSY);  // OpenMailInvoiceFrame
    // OpenMailAttachmentText: TAKE_ATTACHMENTS over the buttons, or NO_ATTACHMENTS (grey) in the middle.
    if (buttonCount > 0) {
        const float fromBottom = 106.0f + icony * rows + gapy1 * (rows - 1) + gapy2 + textH;
        ui.textShadow(fs, OX(27), BY(fromBottom), CLIENT_STR(TAKE_ATTACHMENTS), kColWhite);
    } else {
        const float fromBottom = 106.0f + (areay - textH) * 0.5f + textH;
        ui.textShadow(fs, OX(27 + 310 * 0.5f), BY(fromBottom), CLIENT_STR(NO_ATTACHMENTS), kColDisable, Align::Center);
    }
    // The buttons at their rows: TOPLEFT at BOTTOMLEFT (27 + 45 x, 106 + 39 + 42 y).
    const uint32_t playerLevel = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    if (rows > 0) {
        size_t rowIndex = 0;
        float cursorx = b.openRows[0].first, cursorxend = b.openRows[0].second;
        int cursory = rows - 1;
        for (int k = 0; k < static_cast<int>(b.openButtons.size()); ++k) {
            const int which = b.openButtons[static_cast<size_t>(k)];
            const Rect r{std::floor(OX(27 + 45 * cursorx)), std::floor(BY(106 + 39 + 42 * cursory)), 20, 20};
            const bool shown = which == -2 ? (letterTakeable && !textCreated)
                               : which == -1 ? m.money > 0
                                             : attachmentBySlot(which) != nullptr;
            if (shown) {
                const uint32_t id = which == -2 ? kIdOpenLetter : which == -1 ? kIdOpenMoney
                                                                             : kIdOpenAttach0 + static_cast<uint32_t>(which);
                const bool f = stop(id, r);
                const pp::Texture* icon = nullptr;
                uint32_t tint = 0xFFFFFFFFu, count = 0;
                if (which == -2) {
                    icon = b.letter.pixels ? &b.letter : nullptr;
                } else if (which == -1) {  // GetCoinIcon
                    icon = &b.coin[m.money >= 10000 ? 0 : m.money >= 100 ? 1 : 2];
                } else {
                    const auto* a = attachmentBySlot(which);
                    const auto* info = session_.getItemInfo(a->itemId);
                    if (info && info->valid) {
                        icon = itemIcons().byDisplay(info->displayInfoId, spellFrame_);
                        if (info->requiredLevel > playerLevel) tint = 0xFF1A1AFFu;  // canUse: red (1, 0.1, 0.1)
                    } else {
                        session_.ensureItemInfo(a->itemId);
                    }
                    count = a->stackCount;
                }
                if (icon && icon->pixels) ui.image(*icon, r, tint);
                else ui.fill(r, 0xFF202020u);
                if (spellArt_.slot.pixels) {  // ItemButtonTemplate's UI-Quickslot2
                    const float c = r.w * 64.0f / 37.0f;
                    ui.image(spellArt_.slot, Rect{r.x + r.w * 0.5f - c * 0.5f, r.y + r.h * 0.5f - c * 0.5f, c, c});
                }
                if (count > 1) {
                    char cnt[12];
                    std::snprintf(cnt, sizeof(cnt), "%u", static_cast<unsigned>(count));
                    ui.textShadow(fs, r.x + r.w + 1, r.y + r.h - lh + 2, cnt, kColWhite, Align::Right);
                }
                if (f) {
                    ui.fill(r, 0x50FFFFFFu);
                    b.tipRect = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};
                    if (which == -2) {
                        b.tipTitleBuf = CLIENT_STR(MAIL_LETTER_TOOLTIP);  // PSP: one line (the client breaks it in two)
                        std::replace(b.tipTitleBuf.begin(), b.tipTitleBuf.end(), '\n', ' ');
                        b.tipTitle = b.tipTitleBuf.c_str();
                    } else if (which == -1) {
                        b.tipTitle = CLIENT_STR(ENCLOSED_MONEY);  // PSP: a title over SetTooltipMoney's coins
                        b.tipMoney = static_cast<uint32_t>(m.money);
                    } else {
                        b.tipProto = attachmentBySlot(which)->itemId;
                        b.tipCount = attachmentBySlot(which)->stackCount;
                    }
                }
            }
            cursorx += 1.0f;
            if (cursorx > cursorxend + 0.01f) {
                ++rowIndex;
                --cursory;
                if (rowIndex < b.openRows.size()) {
                    cursorx = b.openRows[rowIndex].first;
                    cursorxend = b.openRows[rowIndex].second;
                }
            }
        }
    }
    // REPLY_MESSAGE / DELETE or MAIL_RETURN / CLOSE (82 + 82 + 80 x 22 at BOTTOMRIGHT -39,80).
    {
        const bool canReply = m.messageType == 0 && sender != CLIENT_STR(UNKNOWN);
        const float by = BY(80 + 22), bh = 22 * kSY + 1;
        drawPanelButton(ui, kIdOpenReply, Rect{OX(384 - 39 - 80 - 164), by, 82 * kSX, bh}, CLIENT_STR(REPLY_MESSAGE),
                        canReply, focusable);
        drawPanelButton(ui, kIdOpenDelete, Rect{OX(384 - 39 - 80 - 82), by, 82 * kSX, bh},
                        canDelete(m) ? CLIENT_STR(DELETE) : CLIENT_STR(MAIL_RETURN), true, focusable);
        drawPanelButton(ui, kIdOpenCancel, Rect{OX(384 - 39 - 80), by, 80 * kSX, bh}, CLIENT_STR(CLOSE), true, focusable);
        if (focusable && (winFocus_.isPressed(kIdOpenReply) || winFocus_.isPressed(kIdOpenDelete) ||
                          winFocus_.isPressed(kIdOpenCancel)))
            b.guard = false;
    }
    return true;
}

// The MailPopup window: one StaticPopup at a time (COD_CONFIRMATION, COD_ALERT, DELETE_MAIL, DELETE_MONEY,
// SEND_MONEY); START = hideOnEscape.
bool WorldScreen::drawMailPopup(UiRenderer& ui) {
    BankMailUi& b = bankMail();
    StaticPopup p;
    p.text = b.popupText.c_str();
    p.button1Id = kIdMailPopupYes;
    p.button2Id = kIdMailPopupNo;
    p.focusable = uiMode_ && !settings_.isOpen();
    switch (b.popup) {
        case kMailPopupCodAlert:
            p.button1 = CLIENT_STR(CLOSE);
            break;
        case kMailPopupCodConfirm:
        case kMailPopupDeleteMoney:
        case kMailPopupSendMoney:
            p.hasMoneyFrame = true;
            p.money = b.popupMoney;
            [[fallthrough]];
        default:
            p.button1 = CLIENT_STR(ACCEPT);
            p.button2 = CLIENT_STR(CANCEL);
            break;
    }
    drawStaticPopup(ui, p);
    return true;
}

void WorldScreen::mailShowPopup(uint8_t kind) {
    BankMailUi& b = bankMail();
    b.popup = kind;
    openWindow(kWinMailPopup);
    winFocus_.setFocus(kIdMailPopupYes);
}

// --- The buttons -------------------------------------------------------------------------

bool WorldScreen::mailFrameAction(uint32_t act, bool alt) {
    BankMailUi& b = bankMail();
    if (b.guard) return true;  // a Cross held since before the frame came up
    const auto& inbox = session_.getMailInbox();
    auto findMail = [&](uint32_t id) -> const game::MailMessage* {
        for (const auto& m : inbox)
            if (m.messageId == id) return &m;
        return nullptr;
    };
    auto keyboard = [&](uint32_t field, const std::string& title, const std::string& initial, int max,
                        pp::OskCharset cs) {
        if (pp::oskActive()) return;
        b.oskField = static_cast<int>(field);
        if (!pp::oskBeginFor(kOskOwner, title, initial, max, cs)) {
            b.oskField = 0;
            uiError(OWN_STR(PSP_KEYBOARD_FAILED));
        }
    };
    auto sendNow = [&](uint32_t money, uint32_t cod) {  // SendMailFrame_SendMail
        session_.sendMail(trimmed(b.to), b.subject, b.body, money, cod);
    };
    switch (act) {
        case kIdMailClose:
        case kIdMailCancel: closeWindow(kWinMail); return true;  // HideUIPanel(MailFrame): syncMail closes the mailbox
        case kIdMailTab1:
        case kIdMailTab2: {  // MailFrameTab_OnClick
            const int tab = act == kIdMailTab1 ? 1 : 2;
            if (tab != b.tab) {
                b.tab = tab;
                if (tab == 2) b.replyMode = false;  // sendMode "send"
                playSoundByName("igSpellBookOpen");
            }
            return true;
        }
        case kIdMailPrev:
        case kIdMailNext:  // InboxPrevPage / InboxNextPage
            b.page += act == kIdMailNext ? 1 : -1;
            playSoundByName("igMainMenuOptionCheckBoxOn");
            if (session_.mailTotalCount() > inbox.size()) session_.refreshMailList();  // InboxGetMoreMail
            return true;
        case kIdMailTo: keyboard(act, CLIENT_STR(MAIL_TO_LABEL), b.to, 12, pp::OskCharset::Any); return true;
        case kIdMailSubject: keyboard(act, CLIENT_STR(MAIL_SUBJECT_LABEL), b.subject, 64, pp::OskCharset::Text); return true;
        case kIdMailBody: keyboard(act, b.subject.empty() ? CLIENT_STR(SENDMAIL) : b.subject.c_str(), b.body, 500, pp::OskCharset::Text);
            return true;
        case kIdMailGold:
        case kIdMailSilver:
        case kIdMailCopper: {
            const uint32_t v = act == kIdMailGold ? b.gold : act == kIdMailSilver ? b.silver : b.copper;
            const char* coin = act == kIdMailGold     ? CLIENT_STR(GOLD_AMOUNT_SYMBOL)
                               : act == kIdMailSilver ? CLIENT_STR(SILVER_AMOUNT_SYMBOL)
                                                      : CLIENT_STR(COPPER_AMOUNT_SYMBOL);
            const std::string title =
                std::string(b.codMode ? CLIENT_STR(COD_AMOUNT) : CLIENT_STR(AMOUNT_TO_SEND)) + " (" + coin + ")";
            keyboard(act, title, v ? std::to_string(v) : std::string(), act == kIdMailGold ? 6 : 2, pp::OskCharset::Digits);
            return true;
        }
        case kIdMailRadioMoney:
        case kIdMailRadioCod:  // SendMailRadioButton_OnClick
            b.codMode = act == kIdMailRadioCod && session_.getMailAttachmentCount() > 0;
            playSoundByName("igMainMenuOptionCheckBoxOn");
            return true;
        case kIdMailSend: {  // SendMailMailButton_OnClick
            b.sendDisabled = true;
            const uint32_t copper = b.gold * 10000u + b.silver * 100u + b.copper;
            if (!b.codMode) {
                if (copper > 0) {  // StaticPopup_Show("SEND_MONEY", name)
                    b.popupText = CLIENT_FMT(SEND_MONEY_CONFIRMATION, trimmed(b.to));
                    b.popupMoney = copper;
                    mailShowPopup(kMailPopupSendMoney);
                    return true;
                }
                sendNow(0, 0);
            } else {
                sendNow(0, copper);  // SetSendMailCOD(copper)
            }
            return true;
        }
        case kIdMailPopupYes:
        case kIdMailPopupNo: {
            const uint8_t kind = b.popup;
            b.popup = kMailPopupNone;
            closeWindow(kWinMailPopup);
            const bool yes = act == kIdMailPopupYes;
            if (kind == kMailPopupSendMoney) {
                if (yes) sendNow(b.popupMoney, 0);  // SetSendMailMoney + SendMailFrame_SendMail
                else b.sendDisabled = false;         // OnCancel
            } else if (yes && kind == kMailPopupCodConfirm) {
                if (const auto* m = findMail(b.openMailId))  // TakeInboxItem(openMailID, lastTakeAttachment)
                    for (const auto& a : m->attachments)
                        if (static_cast<int>(a.slot) == b.popupAttach) session_.mailTakeItem(m->messageId, a.itemGuidLow);
            } else if (yes && (kind == kMailPopupDeleteMail || kind == kMailPopupDeleteMoney)) {
                session_.mailDelete(b.openMailId);  // DeleteInboxItem; openMailID = nil; HideUIPanel(OpenMailFrame)
                b.openMailId = 0;
                closeWindow(kWinOpenMail);
            }
            return true;
        }
        // OpenMailFrame.
        case kIdOpenClose:
        case kIdOpenCancel: closeWindow(kWinOpenMail); return true;  // HideParentPanel: OpenMailFrame_OnHide (syncMail)
        case kIdOpenBody: ++b.bodyPage; return true;                 // PSP: the next page of the letter
        case kIdOpenReply: {                                          // OpenMail_Reply
            const auto* m = findMail(b.openMailId);
            if (!m) return true;
            b.tab = 2;
            playSoundByName("igSpellBookOpen");
            b.to = session_.getMailSenderName(*m);
            const std::string prefix = "RE: ";  // MAIL_REPLY_PREFIX .. " "
            const std::string subject = session_.getMailDisplaySubject(*m);
            b.subject = subject.compare(0, prefix.size(), prefix) == 0 ? subject : prefix + subject;
            b.replyMode = true;
            winFocus_.setFocus(kIdMailBody);  // SendMailBodyEditBox:SetFocus()
            return true;
        }
        case kIdOpenDelete: {  // OpenMail_Delete
            const auto* m = findMail(b.openMailId);
            if (!m) return true;
            if (canDelete(*m)) {
                if (!m->attachments.empty()) {  // DELETE_MAIL with the first attachment's name
                    const auto* info = session_.getItemInfo(m->attachments[0].itemId);
                    b.popupText = CLIENT_FMT(DELETE_MAIL_CONFIRMATION, info && info->valid ? info->name : std::string("?"));
                    mailShowPopup(kMailPopupDeleteMail);
                    return true;
                }
                if (m->money > 0) {
                    b.popupText = CLIENT_STR(DELETE_MONEY_CONFIRMATION);
                    b.popupMoney = static_cast<uint32_t>(m->money);
                    mailShowPopup(kMailPopupDeleteMoney);
                    return true;
                }
                session_.mailDelete(m->messageId);
            } else {
                session_.mailReturnToSender(m->messageId);  // ReturnInboxItem; StaticPopup_Hide("COD_CONFIRMATION")
                closeWindow(kWinMailPopup);
            }
            b.openMailId = 0;
            closeWindow(kWinOpenMail);
            return true;
        }
        case kIdOpenLetter:  // TakeInboxTextItem
            session_.takeInboxTextItem(b.openMailId);
            playSoundByName("igMainMenuOptionCheckBoxOn");
            return true;
        case kIdOpenMoney:  // TakeInboxMoney
            session_.mailTakeMoney(b.openMailId);
            return true;
        default: break;
    }
    // An inbox letter: InboxFrame_OnClick (the checked one closes, another opens).
    if (act >= kIdMailRow0 && act < kIdMailRow0 + kInboxPerPage) {
        const size_t index = static_cast<size_t>((b.page - 1) * kInboxPerPage + static_cast<int>(act - kIdMailRow0));
        if (index >= inbox.size()) return true;
        const game::MailMessage& m = inbox[index];
        if (m.messageId == b.openMailId) {
            b.openMailId = 0;  // InboxFrame.openMailID = 0: no OnHide deletion
            closeWindow(kWinOpenMail);
            playSoundByName("igSpellBookClose");
            return true;
        }
        b.openMailId = m.messageId;
        b.openUpdatePositions = true;
        b.bodyPage = 0;
        openWindow(kWinOpenMail);  // on top: START closes the letter first
        if (!m.read) session_.mailMarkAsRead(m.messageId);  // GetInboxText marks it read
        playSoundByName("igSpellBookOpen");
        PSP_LOGI(pp::tag::Ctrl, "mail_read id=%u items=%u money=%u cod=%u", static_cast<unsigned>(m.messageId),
                 static_cast<unsigned>(m.attachments.size()), static_cast<unsigned>(m.money), static_cast<unsigned>(m.cod));
        return true;
    }
    // An open letter's attachment: OpenMailAttachment_OnClick.
    if (act >= kIdOpenAttach0 && act < kIdOpenAttach0 + 16) {
        const auto* m = findMail(b.openMailId);
        if (!m) return true;
        const int slot = static_cast<int>(act - kIdOpenAttach0);
        if (m->cod > 0 && m->cod > session_.getMoneyCopper()) {
            b.popupText = CLIENT_STR(COD_INSUFFICIENT_MONEY);
            mailShowPopup(kMailPopupCodAlert);
        } else if (m->cod > 0) {
            b.popupAttach = slot;
            b.popupText = CLIENT_STR(COD_CONFIRMATION);
            b.popupMoney = static_cast<uint32_t>(m->cod);
            mailShowPopup(kMailPopupCodConfirm);
        } else {
            for (const auto& a : m->attachments)
                if (static_cast<int>(a.slot) == slot) session_.mailTakeItem(m->messageId, a.itemGuidLow);
        }
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    // A letter's attachment slot (SendMailAttachmentButton_OnClick -> ClickSendMailItemButton): the cursor item
    // goes on the letter; without one the attached item comes off (the right click: always off).
    if (act >= kIdMailAttach0 && act < kIdMailAttach0 + kSendMax) {
        const int i = static_cast<int>(act - kIdMailAttach0);
        const bool occupied = session_.getMailAttachments()[static_cast<size_t>(i)].occupied();
        if (invHeld_.active && !alt) {
            const InvHeld held = invHeld_;
            invHeld_ = InvHeld{};
            bool ok = false;
            if (held.bag == game::slots::kNoContainer && held.slot >= game::slots::kBackpackFirst &&
                held.slot < game::slots::kBackpackFirst + game::slots::kBackpackCount) {
                ok = session_.attachItemFromBackpack(held.slot - game::slots::kBackpackFirst);
            } else if (held.bag >= game::slots::kWornBagFirst &&
                       held.bag < game::slots::kWornBagFirst + game::slots::kWornBagCount) {
                ok = session_.attachItemFromBag(held.bag - game::slots::kWornBagFirst, held.slot);
            }
            if (!ok) uiError(CLIENT_STR(ERR_MAIL_INVALID_ATTACHMENT));
        } else if (occupied) {
            session_.detachMailAttachment(i);
        }
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    return true;
}

}  // namespace wowee::ui::psp
