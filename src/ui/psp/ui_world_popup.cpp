// The dialog boxes of the original - StaticPopup.xml / .lua and
// GameMenuFrame.xml (every window on the original art, 1:1).
//
// StaticPopup (StaticPopupTemplate + StaticPopup_Show / StaticPopup_Resize):
//   - Backdrop UI-DialogBox-Background (black, alpha 0.6, insets 11/12/12/11)
//     + UI-DialogBox-Border (32-unit edges), 320 units wide;
//   - $parentText (GameFontHighlight, white) TOP 0,-16, 290 wide, wrapped;
//   - SmallMoneyFrame TOP of the text's BOTTOM 0,-5 (hasMoneyFrame);
//   - StaticPopupButtonTemplate buttons (UI-DialogBox-Button-Up / -Down /
//     -Disabled / -Highlight, GameFontNormal gold, GameFontHighlight white on
//     focus, GameFontDisable grey): 120 units wide (the text + 20 when wider),
//     two buttons - button1 BOTTOMRIGHT at BOTTOM -6,16, button2 13 to its
//     right; one button - BOTTOM 0,16;
//   - height = 32 + text + 8 + 21 (+ 16 with the money frame);
//   - StaticPopup1 at TOP 0,-135 of UIParent (768 high -> 48 px), the next
//     popups drawn in the same frame TOP to the previous one's BOTTOM.
// All in kDialogScale (0.8) of the units: the PSP fonts (13 px for the 12 pt
// GameFontHighlight) are ~1.8x the FontStrings of the 0.6 frames, and a popup
// grows with its text anyway (EQUIP_NO_DROP: 2 lines in 232 px).
//
// API (for any window - bind confirm, talent wipe, quest abandon; the death /
// release window can use it the same way):
//     StaticPopup p;
//     p.text = "...";                         // already formatted
//     p.button1 = "OK";   p.button1Id = kIdYes;   // OKAY
//     p.button2 = "Cancel"; p.button2Id = kIdNo;  // CANCEL (null = one button)
//     p.hasMoneyFrame = true; p.money = copper;   // optional
//     p.focusable = uiMode_ && !settings_.isOpen() && ...;
//     drawStaticPopup(ui, p);                 // returns the frame (base coords)
//   Draw it from the window's own draw (its window id in the stack: START closes
//   the top window = the popup's "hideOnEscape" cancel); the buttons are focus
//   stops with the given ids - the clicks come back through updateWindows ->
//   the window's *Action(act) as for any button. Add the window id to
//   beginPopups() so the dialog art stays loaded while it is open.
//
// GameMenuFrame (drawGameMenu, ui_world_windows.cpp): the same backdrop,
// 195 units wide, UI-DialogBox-Header at TOP 0,12 with MAIN_MENU, one column of
// GameMenuButtonTemplate (144 x 21, UIPanelButton art, GameFontHighlight), 1 unit
// apart, RETURN_TO_GAME 16 under the rest.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kS = 0.8f;  // WorldScreen::kDialogScale (static_assert in loadPopupArt)
constexpr float kPopupTop = 48.0f;               // StaticPopup1 TOP 0,-135 of the 768-unit UIParent
constexpr uint32_t kDialogBg = 0x99000000u;      // UI-DialogBox-Background: black, alpha 153
constexpr uint32_t kColDisable = 0xFF808080u;    // GameFontDisable (0.5, 0.5, 0.5)
float U(float units) { return std::floor(units * kS + 0.5f); }  // units -> whole px

// SmallMoneyFrame's width as drawMoneyFrame lays it out (ui_world_merchant.cpp).
float moneyWidth(const Font& fs, uint32_t copper) {
    const uint32_t parts[3] = {copper / 10000u, (copper / 100u) % 100u, copper % 100u};
    float w = 0.0f;
    for (int p = 0; p < 3; ++p) {
        if (parts[p] == 0 && !(p == 2 && parts[0] == 0 && parts[1] == 0)) continue;
        char t[12];
        std::snprintf(t, sizeof(t), "%u", static_cast<unsigned>(parts[p]));
        w += static_cast<float>(fs.measure(t)) + 1.0f + 12.0f;
    }
    return w;
}

}  // namespace

WorldScreen::PopupArt::~PopupArt() { release(); }

void WorldScreen::PopupArt::release() {
    for (pp::Texture* t : {&border, &header, &btnUp, &btnDown, &btnDis, &btnHl})
        if (t->pixels) pp::textureRelease(*t);
    tried = false;
}

void WorldScreen::loadPopupArt() {
    static_assert(kS == kDialogScale, "one dialog scale");
    if (popupArt_.tried) return;
    popupArt_.tried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    auto load = [&](const char* name, pp::Texture& t) {
        const pp::PakEntry* e = pak.find(name);
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && !pp::textureFromBlob(blob, e->size, t, true)) pp::pakFree(blob);
    };
    load("sp_border", popupArt_.border);
    load("sp_header", popupArt_.header);
    load("sp_btn_up", popupArt_.btnUp);
    load("sp_btn_dn", popupArt_.btnDown);
    load("sp_btn_dis", popupArt_.btnDis);
    load("sp_btn_hl", popupArt_.btnHl);
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "popup_art border=%d header=%d buttons=%d", popupArt_.border.pixels ? 1 : 0,
             popupArt_.header.pixels ? 1 : 0, popupArt_.btnUp.pixels ? 1 : 0);
}

void WorldScreen::beginPopups() {
    popupStackY_ = 0.0f;
    // The dialog art (~60 KB) only while a dialog is open. kWinRelease: for the death popup.
    if (popupArt_.tried && !windowOpen(kWinMenu) && !windowOpen(kWinBindConfirm) && !windowOpen(kWinTalentWipe) &&
        !windowOpen(kWinQuestAbandon) && !windowOpen(kWinRelease) &&
        !windowOpen(kWinUnlearnSkill) &&  // StaticPopup UNLEARN_SKILL
        !windowOpen(kWinPartyInvite) && !windowOpen(kWinLootRollConfirm) &&  // PARTY_INVITE, CONFIRM_LOOT_ROLL
        !windowOpen(kWinBankBuy) && !windowOpen(kWinMailPopup) &&  // CONFIRM_BUY_BANK_SLOT, mail popups
        !auctionKeepsPopupArt() &&  // BUYOUT_AUCTION / BID_AUCTION / CANCEL_AUCTION / AUCTION_HOUSE_DISABLED
        !windowOpen(kWinInstanceBoot) && !windowOpen(kWinInstanceLock) &&  // INSTANCE_BOOT / _LOCK
        !windowOpen(kWinBgConfirm) && !windowOpen(kWinBgConfirm2) &&  // CONFIRM_BATTLEFIELD_ENTRY
        !wotlkKeepsPopupArt() &&  // the LFG popups, CALENDAR_ERROR, the calendar's event frame
        !socialKeepsPopupArt() &&  // GUILD_INVITE, the guild confirmations
        !windowOpen(kWinDeleteItem) && !windowOpen(kWinUseBind) &&  // DELETE_ITEM; USE_BIND
        !petKeepsPopupArt() &&  // RENAME_PET, PETRENAMECONFIRM, ABANDON_PET, CONFIRM_BUY_STABLE_SLOT
        !ggbKeepsPopupArt() &&  // the glyph / vault / channel dialogs
        !tradeDuelKeepsPopupArt())  // DUEL_REQUESTED / _OUTOFBOUNDS, CONFIRM_SUMMON, QUEST_ACCEPT...
        popupArt_.release();
}

void WorldScreen::drawDialogBox(UiRenderer& ui, const Rect& r) {
    loadPopupArt();
    if (!popupArt_.border.pixels) {  // an old ui.pak: the generic panel
        kit_.panel(ui, r);
        return;
    }
    // BackgroundInsets left 11, right 12, top 12, bottom 11; the edges stretch (they are uniform along
    // their length, as the tiled original).
    ui.fill(Rect{r.x + U(11), r.y + U(12), r.w - U(23), r.h - U(23)}, kDialogBg);
    const float edge = popupArt_.border.slice[0] ? U(32) / static_cast<float>(popupArt_.border.slice[0]) : 1.0f;
    ui.nineSlice(popupArt_.border, r, edge);
}

void WorldScreen::drawDialogHeader(UiRenderer& ui, const Rect& frame, const char* title) {
    // GameMenuFrameHeader: UI-DialogBox-Header 256 x 64 at TOP 0,12; baked cropped to its visible
    // 144 x 40 (x 56..200). The title (GameFontNormal, TOP of the header 0,-14) sits in the middle of
    // that band - PSP: centred, the 13 px font is taller than the 12-unit one.
    const Rect h{std::floor(frame.x + frame.w * 0.5f - U(72) + 0.5f), frame.y - U(12), U(144), U(40)};
    if (popupArt_.header.pixels) ui.image(popupArt_.header, h);
    const Font& fn = kit_.font(kFontNormal);
    ui.textShadow(fn, h.x + h.w * 0.5f, h.y + std::floor((h.h - static_cast<float>(fn.lineHeight())) * 0.5f), title,
                  kColGold, Align::Center);
}

void WorldScreen::drawGameMenuButton(UiRenderer& ui, uint32_t id, const Rect& r, const char* label, bool focusable,
                                     bool enabled) {
    loadNpcArt(false);  // UIPanelButton art (pbtn_*)
    if (!npcArt_.btnUp.pixels) {
        kit_.button(ui, winFocus_, id, r, label, enabled && focusable);
        return;
    }
    const bool focused = focusable && enabled && winFocus_.addStop(id, r);
    const bool pressed = focused && winFocus_.isPressed(id);
    const pp::Texture& t = !enabled && npcArt_.btnDis.pixels ? npcArt_.btnDis
                           : pressed && npcArt_.btnDown.pixels ? npcArt_.btnDown
                                                               : npcArt_.btnUp;
    // Three slices as drawPanelButton (ui_world_npc_frame.cpp).
    const float cap = 6.0f, tw = t.uw, th = t.uh, capT = cap * tw / 64.0f;
    ui.image(t, Rect{r.x, r.y, cap, r.h}, SrcRect{0, 0, capT, th});
    ui.image(t, Rect{r.x + cap, r.y, r.w - 2 * cap, r.h}, SrcRect{capT, 0, tw - 2 * capT, th});
    ui.image(t, Rect{r.x + r.w - cap, r.y, cap, r.h}, SrcRect{tw - capT, 0, capT, th});
    if (focused && npcArt_.btnHl.pixels) ui.image(npcArt_.btnHl, r, 0xFFFFFFFFu, Blend::Add);
    const Font& fn = kit_.font(kFontNormal);
    ui.textShadow(fn, r.x + r.w * 0.5f + (pressed ? 1.0f : 0.0f),
                  r.y + std::floor((r.h - static_cast<float>(fn.lineHeight())) * 0.5f + 0.5f), label,
                  enabled ? kColWhite : kColDisable, Align::Center);
}

Rect WorldScreen::drawStaticPopup(UiRenderer& ui, const StaticPopup& p) {
    PROF_SCOPE("popup");
    loadPopupArt();
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);
    const float lh = static_cast<float>(fn.lineHeight());
    const std::vector<std::string> lines = wrapQuestText(fn, p.text ? p.text : "", U(290));
    const float textH = lh * static_cast<float>(std::max<size_t>(1, lines.size()));
    // StaticPopup_Resize: 320 wide (no button3 / close button here), 32 + text + 8 + button1.
    // showAlert / showAlertGear - 420 wide, the icon (36 x 36) at LEFT 24,0.
    const float w = U(p.alert ? 420 : 320);
    const float h = U(32) + textH + U(8) + U(21) + (p.hasMoneyFrame ? U(16) : 0.0f) +
                    (p.editText ? U(8) + U(32) : 0.0f);  // StaticPopup_Resize, + 8 + the edit box
    const Rect r{std::floor((static_cast<float>(pp::kScreenW) - w) * 0.5f), popupStackY_ > 0.0f ? popupStackY_ : kPopupTop,
                 w, h};
    popupStackY_ = r.y + r.h;
    drawDialogBox(ui, r);
    if (p.alert && p.alert->pixels)  // AlertIcon
        ui.image(*p.alert, Rect{r.x + U(24), std::floor(r.y + (r.h - U(36)) * 0.5f), U(36), U(36)});
    const float cx = r.x + r.w * 0.5f;
    float ty = r.y + U(16);
    for (const std::string& l : lines) {
        ui.textShadow(fn, cx, ty, l.c_str(), kColWhite, Align::Center);
        ty += lh;
    }
    if (lines.empty()) ty += lh;
    if (p.hasMoneyFrame) drawMoneyFrame(ui, cx + std::floor(moneyWidth(fs, p.money) * 0.5f), ty + U(5), p.money, true);
    if (p.editText) {
        // StaticPopupEditBoxTemplate (130 wide) under the text; Cross on it = the caller's keyboard.
        const Rect eb{std::floor(cx - U(65)), ty + U(8) + (p.hasMoneyFrame ? U(16) : 0.0f), U(130), U(20)};
        const bool f = p.focusable && p.editId && winFocus_.addStop(p.editId, eb);
        ui.fill(eb, 0xC0000000u);
        const uint32_t edge = f ? 0xFF00D0FFu : 0xFF808080u;
        ui.fill(Rect{eb.x, eb.y, eb.w, 1.0f}, edge);
        ui.fill(Rect{eb.x, eb.y + eb.h - 1.0f, eb.w, 1.0f}, edge);
        ui.fill(Rect{eb.x, eb.y, 1.0f, eb.h}, edge);
        ui.fill(Rect{eb.x + eb.w - 1.0f, eb.y, 1.0f, eb.h}, edge);
        ui.textShadow(fn, eb.x + U(6), eb.y + std::floor((eb.h - lh) * 0.5f + 0.5f), p.editText, kColWhite);
    }

    // The buttons (StaticPopup_Show): 120 wide, or the text + 20 (button1 past 120, button2 past 110).
    const float bh = U(21), by = r.y + r.h - U(16) - bh;
    auto width = [&](const char* label, float past) {
        const float tw = static_cast<float>(fn.measure(label));
        return tw > U(past) ? tw + U(20) : U(120);
    };
    auto button = [&](uint32_t id, const Rect& b, const char* label, bool enabled) {
        const bool f = p.focusable && enabled && winFocus_.addStop(id, b);
        const bool pressed = f && winFocus_.isPressed(id);
        const pp::Texture& t = !enabled && popupArt_.btnDis.pixels ? popupArt_.btnDis
                               : pressed && popupArt_.btnDown.pixels ? popupArt_.btnDown
                                                                     : popupArt_.btnUp;
        if (t.pixels) ui.image(t, b);
        else ui.fill(b, 0xFF101060u);
        if (f && popupArt_.btnHl.pixels) ui.image(popupArt_.btnHl, b, 0xFFFFFFFFu, Blend::Add);
        // $parentText CENTER 0,1; NormalFont GameFontNormal, HighlightFont GameFontHighlight.
        const uint32_t col = !enabled ? kColDisable : f ? kColWhite : kColGold;
        ui.textShadow(fn, b.x + b.w * 0.5f + (pressed ? 1.0f : 0.0f),
                      b.y + std::floor((b.h - lh) * 0.5f + 0.5f) - 1.0f + (pressed ? 1.0f : 0.0f), label, col,
                      Align::Center);
    };
    if (p.button1 && p.button2) {
        const float w1 = width(p.button1, 120), w2 = width(p.button2, 110);
        const float r1 = std::floor(cx - U(6));
        button(p.button1Id, Rect{r1 - w1, by, w1, bh}, p.button1, p.button1Enabled);
        button(p.button2Id, Rect{r1 + U(13), by, w2, bh}, p.button2, p.button2Enabled);
    } else if (p.button1) {
        const float w1 = width(p.button1, 120);
        button(p.button1Id, Rect{std::floor(cx - w1 * 0.5f), by, w1, bh}, p.button1, p.button1Enabled);
    }
    return r;
}

}  // namespace wowee::ui::psp
