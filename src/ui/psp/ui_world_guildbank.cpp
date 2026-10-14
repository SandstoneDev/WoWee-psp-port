// GuildBankFrame of the original ruRU 3.3.5a client
// (the guild bank add-on's .xml / .lua) over the session's guild bank (psp_world_session_guildbank.cpp):
//   - GUILDBANKFRAME_OPENED (the vault used: gameobject type 34) shows the frame with the cursor up and the backpack
//     (PSP, as the bank), GUILDBANKFRAME_CLOSED / the close button hides it (CloseGuildBankFrame);
//   - the frame (UI-GuildBankFrame-Left / -Right, 769 x 444) and GuildBankEmblemFrame's ornament over its top;
//     GuildBankFrameTab1..4 GUILD_BANK / GUILD_BANK_LOG / GUILD_BANK_MONEY_LOG / GUILD_BANK_TAB_INFO (GuildBankFrameTab_OnClick);
//   - GuildBankTab1..6 at the right: the tab's icon, checked = the current one, desaturated when not viewable / in the
//     money log, the guild leader's «buy a tab» button (UI-GuildBankFrame-NewTab) after the bought ones;
//   - the bank mode: GuildBankColumn1..7 x 14 slots (GetGuildBankItemInfo: icon, count), grey columns without deposit
//     and withdrawals; GuildBankTabTitle "<name>  (GUILDBANK_TAB_FULL_ACCESS)" and the access in red; GuildBankLimitLabel
//     GUILDBANK_REMAINING_MONEY with STACKS / NONE / UNLIMITED; GuildBankErrorMessage NO_VIEWABLE_GUILDBANK_TABS /
//     NO_GUILDBANK_TABS; GuildBankFrameBuyInfo for the leader (PURCHASE_TAB_TEXT, NUM_GUILDBANK_TABS_PURCHASED, the
//     cost red / white, BANKSLOTPURCHASE -> CONFIRM_BUY_GUILDBANK_TAB); DEPOSIT / WITHDRAW -> GUILDBANK_DEPOSIT / _WITHDRAW;
//     GuildBankMoneyFrame, GUILDBANK_AVAILABLE_MONEY with GetGuildBankWithdrawMoney or UNLIMITED;
//   - the log / money log (GuildBankFrame_UpdateLog / _UpdateMoneyLog: GUILDBANK_*_FORMAT, GUILD_BANK_LOG_TIME with
//     RecentTimeDate), the tab info (GetGuildBankText, SAVE_CHANGES with CanEditGuildTabInfo);
//   - the slots: Cross / Square = PickupGuildBankItem (take onto the cursor; with a bag item on the cursor = deposit
//     there, with a vault item = move), Circle = AutoStoreGuildBankItem; a vault item put on a bag slot = withdraw;
//     Circle on a bag item while the frame is open = UseContainerItem's deposit;
//   - GuildBankPopupFrame (the leader's right click on a tab): GUILDBANK_POPUP_TEXT, the name (the PSP keyboard), the
//     icons (GetMacroItemIconInfo: ItemDisplayInfo's icon names, text.pak "gb_icons"), OKAY / CANCEL.
// PSP: 0.55 x 0.525 of the frame's units (the emblem frame over it and the tabs under it fit the 272 px); the guild's
// tabard in the emblem frame is not drawn; MoneyInputFrame = one keyboard line "gold silver copper"; the log has its
// own arrows (FauxScrollFrame without the bar); no stack split (SplitGuildBankItem), no item lock look.
// Memory (while the frame is open): gb_frame T8 ~130 KB, gb_cols ~16 KB, gb_emblem ~33 KB, small ~8 KB; gb_icons
// (text.pak, ~130 KB) only while the tab popup is open. Log: guildbank_art kb=, guildbank_art_free, guildbank_mode.
#include "ui/psp/ui_world_glyphs_guildbank.hpp"

#include "game/inventory_slots.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings and the port's own
#include "ui/psp/ui_world_bank_mail.hpp"
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

constexpr float kSX = 0.55f, kSY = 0.525f, kOx = 4.0f, kOy = 25.0f;
float PX(float u) { return kOx + u * kSX; }
float PY(float u) { return kOy + u * kSY; }
constexpr int kOskOwner = 0x4742;  // "GB"
constexpr int kLogLines = 13;      // PSP: the log's lines (11 px) in GuildBankMessageFrame's 304 units

constexpr uint32_t kColTeal = 0xFF999900u;   // GUILD_BANK_LOG_TIME_PREPEND |cff009999
constexpr uint32_t kColRedC = 0xFF2020FFu;   // RED_FONT_COLOR_CODE
constexpr uint32_t kColGreenC = 0xFF20FF20u; // GREEN_FONT_COLOR_CODE
constexpr uint32_t kColGreyC = 0xFF808080u;

// RecentTimeDate (UIParent.lua) with LASTONLINE_*.
std::string recentTime(int y, int mo, int d, int h) {
    if (y > 0) return pp::text::plural(CLIENT_FMT(LASTONLINE_YEARS, y));
    if (mo > 0) return pp::text::plural(CLIENT_FMT(LASTONLINE_MONTHS, mo));
    if (d > 0) return pp::text::plural(CLIENT_FMT(LASTONLINE_DAYS, d));
    if (h > 0) return pp::text::plural(CLIENT_FMT(LASTONLINE_HOURS, h));
    return CLIENT_STR(LASTONLINE_MINS);
}

// GetDenominationsFromCopper -> GetCoinText(money, " "): GOLD_AMOUNT / SILVER_AMOUNT / COPPER_AMOUNT.
std::string coinText(uint32_t c) {
    const int g = static_cast<int>(c / 10000u), s = static_cast<int>((c / 100u) % 100u), m = static_cast<int>(c % 100u);
    std::string out;
    auto add = [&](const std::string& part) {
        if (!out.empty()) out += ' ';
        out += pp::text::plural(part);
    };
    if (g) add(CLIENT_FMT(GOLD_AMOUNT, g));
    if (s) add(CLIENT_FMT(SILVER_AMOUNT, s));
    if (m || out.empty()) add(CLIENT_FMT(COPPER_AMOUNT, m));
    return out;
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

// The slot of GuildBankColumnNButtonM (slot 0..97): its top-left in the frame's units.
void slotPos(int slot, float& ux, float& uy) {
    const int col = slot / 14, idx = slot % 14;
    ux = 30.0f + static_cast<float>(col) * 103.0f + (idx < 7 ? 7.0f : 56.0f);
    uy = 70.0f + 3.0f + static_cast<float>(idx % 7) * 44.0f;
}

// text.pak "gb_icons": callback per (display id, name).
template <typename F>
bool forEachIcon(F&& f) {
    char path[256];
    pp::fsPath(path, sizeof(path), "data/text.pak");
    pp::Pak pak;
    if (!pak.open(path)) return false;
    bool ok = false;
    if (const pp::PakEntry* e = pak.find("gb_icons")) {
        if (auto* b = static_cast<const uint8_t*>(pak.load(e))) {
            uint32_t n = 0;
            std::memcpy(&n, b, 4);
            const size_t names = 4 + 8 * static_cast<size_t>(n);
            if (names <= e->size) {
                ok = true;
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t did = 0, off = 0;
                    std::memcpy(&did, b + 4 + 8 * i, 4);
                    std::memcpy(&off, b + 8 + 8 * i, 4);
                    if (names + off < e->size) f(did, reinterpret_cast<const char*>(b + names + off));
                }
            }
            pp::pakFree(const_cast<uint8_t*>(b));
        }
    }
    pak.close();
    return ok;
}

}  // namespace

void WorldScreen::GgbUi::freeBankArt() {
    for (pp::Texture* t : {&gbFrame, &gbCols, &gbEmblem, &gbTab, &gbNewTab, &gbNameBd, &gbSlot})
        if (t->pixels) pp::textureRelease(*t);
    if (gbArtTried) PSP_LOGI(pp::tag::Gfx, "guildbank_art_free kb=%u", static_cast<unsigned>(gbArtBytes / 1024u));
    gbArtTried = false;
    gbArtBytes = 0;
}

void WorldScreen::loadGuildBankArt() {
    GgbUi& g = ggbUi();
    if (g.gbArtTried) return;
    g.gbArtTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {
        for (const auto& [name, tex] : std::initializer_list<std::pair<const char*, pp::Texture*>>{
                 {"gb_frame", &g.gbFrame}, {"gb_cols", &g.gbCols}, {"gb_emblem", &g.gbEmblem}, {"gb_tab", &g.gbTab},
                 {"gb_newtab", &g.gbNewTab}, {"gb_namebd", &g.gbNameBd}})
            ok += ggbLoad(pak, name, *tex, g.gbArtBytes);
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "guildbank_art ok=%d/6 kb=%u", ok, static_cast<unsigned>(g.gbArtBytes / 1024u));
}

// The side tabs' icon names -> a display id of itemicons.pak (read when the names change).
void WorldScreen::guildBankIconsFor() {
    GgbUi& g = ggbUi();
    std::string key;
    const uint8_t n = session_.guildBankNumTabs();
    for (uint8_t t = 0; t < n; ++t) key += lower(session_.guildBankTabIcon(t)) + "|";
    if (key == g.tabIconsKey) return;
    g.tabIconsKey = key;
    g.tabIconDisplay.clear();
    std::vector<std::string> want;
    for (uint8_t t = 0; t < n; ++t) want.push_back(lower(session_.guildBankTabIcon(t)));
    forEachIcon([&](uint32_t did, const char* name) {
        const std::string l = lower(name);
        for (const auto& w : want)
            if (w == l) g.tabIconDisplay[w] = did;
    });
}

// Every frame (updateGlyphsGuildBank): the session's events -> the frame, the keyboard, the art.
void WorldScreen::guildBankUpdate() {
    GgbUi& g = ggbUi();
    session_.guildBankUpdate(glm::vec3(playerX_, playerY_, playerZ_));
    guildBankOskTick();
    if (session_.guildBankOpenSeq() != g.gbOpenSeen) {  // GUILDBANKFRAME_OPENED -> GuildBankFrame_OnShow
        g.gbOpenSeen = session_.guildBankOpenSeq();
        if (session_.guildBankOpen()) {
            openWindow(kWinGuildBank);
            g.gbShown = true;
            g.gbMode = 1;
            g.gbHeld = GgbUi::Held{};
            g.gbGuard = true;
            session_.setGuildBankTab(0);
            // GuildBankFrame_SelectAvailableTab: the first viewable tab.
            for (uint8_t t = 0; t < session_.guildBankNumTabs(); ++t)
                if (session_.guildBankTabFlags(t) & gbk::kTabView) {
                    session_.setGuildBankTab(t);
                    break;
                }
            session_.queryGuildBankTab(session_.guildBankTab());
            playSoundByName("GuildVaultOpen");
            if (!uiMode_) {
                setUiMode(true);
                g.gbRaisedUi = true;
                suppress_ = ~0u;
            }
            g.gbOpenedBags = !windowOpen(kWinBags);
            if (g.gbOpenedBags) openBags(false);
            winFocus_.setFocus(kIdGbSlot0);
            PSP_LOGI(pp::tag::Ctrl, "guildbank_frame_open tabs=%u", static_cast<unsigned>(session_.guildBankNumTabs()));
        }
    }
    if (g.gbShown && (!session_.guildBankOpen() || !windowOpen(kWinGuildBank))) {  // GUILDBANKFRAME_CLOSED / OnHide
        if (session_.guildBankOpen()) session_.closeGuildBank();
        if (windowOpen(kWinGuildBank)) closeWindow(kWinGuildBank);
        for (uint8_t w : {kWinGuildBankPopup, kWinGuildBankTabPopup})
            if (windowOpen(w)) closeWindow(w);
        g.gbShown = false;
        g.gbHeld = GgbUi::Held{};
        g.gbPopup = 0;
        g.tpIcons.clear();
        g.tpIcons.shrink_to_fit();
        playSoundByName("GuildVaultClose");
        if (g.gbOpenedBags && windowOpen(kWinBags)) closeWindow(kWinBags);
        g.gbOpenedBags = false;
        if (g.gbRaisedUi && uiMode_ && windows_.empty()) setUiMode(false);
        g.gbRaisedUi = false;
    }
    // GUILDBANK_TEXT_CHANGED for the shown tab: QueryGuildBankText; GUILDBANK_UPDATE_TEXT: the box.
    if (session_.guildBankTextChangedSeq() != g.gbTextChangedSeen) {
        g.gbTextChangedSeen = session_.guildBankTextChangedSeq();
        if (g.gbShown && g.gbMode == 4 && session_.guildBankTextChangedTab() == session_.guildBankTab())
            session_.guildBankQueryText(session_.guildBankTab());
    }
    if (session_.guildBankTextSeq() != g.gbTextSeen) {
        g.gbTextSeen = session_.guildBankTextSeq();
        if (!g.gbInfoEdited) g.gbInfoText = session_.guildBankText(session_.guildBankTab());
    }
    if (g.gbHeld.active && !session_.guildBankItem(g.gbHeld.tab, g.gbHeld.slot)) g.gbHeld = GgbUi::Held{};
    if (g.gbPopup && !windowOpen(kWinGuildBankPopup)) g.gbPopup = 0;
    if (!g.tpIcons.empty() && !windowOpen(kWinGuildBankTabPopup)) {
        g.tpIcons.clear();
        g.tpIcons.shrink_to_fit();
    }
    if (!g.gbShown && g.gbArtTried) g.freeBankArt();
}

void WorldScreen::guildBankOskTick() {
    GgbUi& g = ggbUi();
    bool accepted = false;
    std::string text;
    if (!pp::oskFinishedFor(kOskOwner, accepted, text)) return;
    const int field = g.gbOskField;
    g.gbOskField = 0;
    PSP_LOGI(pp::tag::Ctrl, "guildbank_osk field=%d ok=%d", field, accepted ? 1 : 0);
    if (!accepted) return;
    if (field == 1) {  // MoneyInputFrame: "gold silver copper"
        uint32_t v[3] = {};
        int k = 0;
        const char* s = text.c_str();
        while (*s && k < 3) {
            while (*s && !std::isdigit(static_cast<unsigned char>(*s))) ++s;
            if (!*s) break;
            v[k++] = static_cast<uint32_t>(std::strtoul(s, const_cast<char**>(&s), 10));
        }
        g.gbMoney[0] = std::min<uint32_t>(v[0], 214748u);
        g.gbMoney[1] = std::min<uint32_t>(v[1], 99u);
        g.gbMoney[2] = std::min<uint32_t>(v[2], 99u);
    } else if (field == 2) {  // GuildBankTabInfoEditBox
        g.gbInfoText = gbk::capUtf8(text, 500);
        g.gbInfoEdited = true;
    } else if (field == 3) {  // GuildBankPopupEditBox
        g.tpName = gbk::capUtf8(text, 15);
    }
}

bool WorldScreen::drawGuildBankFrame(UiRenderer& ui) {
    PROF_SCOPE("guildbank.frame");
    GgbUi& g = ggbUi();
    loadGuildBankArt();
    loadNpcArt(false);
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    if (focusable) {
        const uint32_t f = winFocus_.focused();
        if (f >= kIdGbClose && f <= 0x8FFF && winFocus_.isPressed(f)) g.gbGuard = false;
    }
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight());
    const uint8_t tab = session_.guildBankTab();
    const uint8_t numTabs = session_.guildBankNumTabs();
    const bool leader = session_.isGuildLeader();
    // The frame and the emblem frame's ornament over its top.
    if (g.gbFrame.pixels) ui.image(g.gbFrame, Rect{kOx, kOy, 769 * kSX, 444 * kSY});
    else kit_.panel(ui, Rect{kOx, kOy, 769 * kSX, 444 * kSY});
    if (g.gbEmblem.pixels) ui.image(g.gbEmblem, Rect{kOx, PY(-46), 769 * kSX, 80 * kSY});
    // The close button (UIPanelCloseButton, TOPRIGHT 3,-8).
    {
        const Rect cr{PX(769 + 3 - 32), PY(8), 32 * kSX, 32 * kSY};
        const bool f = focusable && winFocus_.addStop(kIdGbClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdGbClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // GuildBankFrame_UpdateTabs: noViewableTabs, the title and its access, the withdrawal line.
    int unviewable = 0;
    for (uint8_t t = 0; t < numTabs; ++t)
        if (!(session_.guildBankTabFlags(t) & gbk::kTabView)) ++unviewable;
    const bool noViewable = unviewable == numTabs && numTabs > 0 && !leader;
    const uint32_t flags = session_.guildBankTabFlags(tab);
    const bool canDeposit = (flags & gbk::kTabDeposit) != 0;
    const uint32_t numWithdrawals = session_.guildBankTabWithdrawals(tab);
    const bool buyTab = leader && tab == numTabs && numTabs < gbk::kBankTabs;
    std::string title, access;
    uint32_t accessCol = kColRedC;
    bool showLimit = false;
    if (g.gbMode == 3) {
        title = CLIENT_STR(GUILD_BANK_MONEY_LOG);
    } else if (buyTab) {
        title = CLIENT_STR(BUY_GUILDBANK_TAB);
    } else if (tab < numTabs && (flags & gbk::kTabView)) {
        const std::string name = session_.guildBankTabName(tab);
        title = g.gbMode == 2   ? CLIENT_FMT(GUILDBANK_LOG_TITLE_FORMAT, name)
                : g.gbMode == 4 ? CLIENT_FMT(GUILDBANK_INFO_TITLE_FORMAT, name)
                                : name;
        showLimit = g.gbMode == 1 || g.gbMode == 2;
        // GuildBankTabLimit: "(" .. GUILDBANK_TAB_* .. ")"
        if (!canDeposit && numWithdrawals == 0) access = "(" + std::string(CLIENT_STR(GUILDBANK_TAB_LOCKED)) + ")";
        else if (!canDeposit) access = "(" + std::string(CLIENT_STR(GUILDBANK_TAB_WITHDRAW_ONLY)) + ")";
        else if (numWithdrawals == 0) access = "(" + std::string(CLIENT_STR(GUILDBANK_TAB_DEPOSIT_ONLY)) + ")";
        else {
            access = "(" + std::string(CLIENT_STR(GUILDBANK_TAB_FULL_ACCESS)) + ")";
            accessCol = kColGreenC;
        }
    }
    auto nameBorder = [&](float cxU, float topU, float wPx) {  // UI-TabNameBorder: left 8, middle, right 8
        const Rect r{PX(cxU) - wPx * 0.5f - 8 * kSX, PY(topU), wPx + 16 * kSX, 18 * kSY};
        if (g.gbNameBd.pixels) {
            const float tw = static_cast<float>(g.gbNameBd.uw), th = static_cast<float>(g.gbNameBd.uh);
            const float c = tw * 0.0625f, mid = tw * (0.546875f - 0.0625f), rr = tw * 0.0625f;
            ui.image(g.gbNameBd, Rect{r.x, r.y, 8 * kSX, r.h}, SrcRect{0, 0, c, th * 0.5625f});
            ui.image(g.gbNameBd, Rect{r.x + 8 * kSX, r.y, wPx, r.h}, SrcRect{c, 0, mid, th * 0.5625f});
            ui.image(g.gbNameBd, Rect{r.x + r.w - 8 * kSX, r.y, 8 * kSX, r.h}, SrcRect{tw * 0.546875f, 0, rr, th * 0.5625f});
        }
        return r;
    };
    if (!title.empty() && !(noViewable && g.gbMode != 3)) {
        const std::string full = access.empty() ? title : title + "  ";
        const float w1 = static_cast<float>(fn.measure(full.c_str())), w2 = static_cast<float>(fn.measure(access.c_str()));
        const Font& tf = w1 + w2 > 330.0f ? fs : fn;
        const float tw1 = static_cast<float>(tf.measure(full.c_str())), tw2 = static_cast<float>(tf.measure(access.c_str()));
        const Rect r = nameBorder(390.5f, 43.0f, tw1 + tw2 + 20 * kSX);
        const float ty = r.y + (r.h - static_cast<float>(tf.lineHeight())) * 0.5f;
        const float x0 = PX(390.5f) - (tw1 + tw2) * 0.5f;
        ui.textShadow(tf, x0, ty, full.c_str(), kColGold);
        if (!access.empty()) ui.textShadow(tf, x0 + tw1, ty, access.c_str(), accessCol);
    }
    // The side tabs (GuildBankTab1..6 at TOPRIGHT -1,-32, 50 apart).
    uint32_t tipTab = 0;
    for (uint8_t t = 0; t < gbk::kBankTabs; ++t) {
        const bool bought = t < numTabs, buy = leader && t == numTabs;
        if (!bought && !buy) continue;
        const float tx = PX(768), ty = PY(32 + 50.0f * t);
        if (g.gbTab.pixels) ui.image(g.gbTab, Rect{tx, ty, 64 * kSX, 64 * kSY});
        const Rect b{tx + 2 * kSX, ty + 8 * kSY, 36 * kSX, 34 * kSY};
        const bool viewable = bought && (session_.guildBankTabFlags(t) & gbk::kTabView);
        const bool disabled = g.gbMode == 3 || (bought && !viewable) ||
                              (buy && (g.gbMode == 2 || g.gbMode == 4));
        const uint32_t tint = disabled ? 0xFF808080u : 0xFFFFFFFFu;  // SetDesaturation
        if (buy) {
            if (g.gbNewTab.pixels) ui.image(g.gbNewTab, b, tint);
            else ui.fill(b, 0xFF303030u);
        } else {
            const std::string icon = lower(session_.guildBankTabIcon(t));
            auto it = g.tabIconDisplay.find(icon);
            const pp::Texture* tex = it != g.tabIconDisplay.end() ? itemIcons().byDisplay(it->second, spellFrame_) : nullptr;
            if (!tex) tex = itemIcons().fixed(pp::ItemIconDb::kQuestionMark, spellFrame_);
            if (tex) ui.image(*tex, b, tint);
            else ui.fill(b, 0xFF303030u);
        }
        if (spellArt_.slot.pixels) {
            const float c = b.w * 60.0f / 36.0f;
            ui.image(spellArt_.slot, Rect{b.x + b.w * 0.5f - c * 0.5f, b.y + b.h * 0.5f - c * 0.5f - kSY, c, c * kSY / kSX});
        }
        if (t == tab && g.gbMode != 3) ui.fill(b, 0x40FFFFFFu);  // CheckButtonHilight (checked)
        const uint32_t id = kIdGbTab0 + t;
        const bool f = focusable && !disabled && winFocus_.addStop(id, b);
        if (f) {
            ui.fill(b, 0x50FFFFFFu);
            tipTab = id;
            wotlkTip(Rect{b.x + ui.offsetX(), b.y + ui.offsetY(), b.w, b.h},
                     {WotlkTipLine(buy ? std::string(CLIENT_STR(BUY_GUILDBANK_TAB)) : session_.guildBankTabName(t))},
                     kTipLeft);
        }
    }
    (void)tipTab;
    // The mode tabs (GuildBankFrameTab1..4 at BOTTOMLEFT 19,-24; each LEFT of the last RIGHT -16).
    {
        const char* const kModes[4] = {CLIENT_STR(GUILD_BANK), CLIENT_STR(GUILD_BANK_LOG), CLIENT_STR(GUILD_BANK_MONEY_LOG),
                                       CLIENT_STR(GUILD_BANK_TAB_INFO)};
        float x = PX(19);
        const float th = 32 * kSY;
        for (int m = 1; m <= 4; ++m) {
            const float w = std::max(60.0f, static_cast<float>(fs.measure(kModes[m - 1])) + 2.0f * 20 * kSX + 4.0f);
            const float y = PY(444 + 24) - th;
            drawFrameTab(ui, kIdGbMode1 + static_cast<uint32_t>(m - 1), x, y, th, kModes[m - 1], g.gbMode == m, focusable);
            x += w - 16 * kSX;
        }
    }
    g.tipEntry = 0;
    if (g.gbMode == 1) {
        if (noViewable || (tab >= numTabs && !buyTab)) {  // GuildBankErrorMessage
            const char* msg = noViewable ? CLIENT_STR(NO_VIEWABLE_GUILDBANK_TABS) : CLIENT_STR(NO_GUILDBANK_TABS);
            ui.textShadow(fn, PX(384.5f), PY(216) - static_cast<float>(fn.lineHeight()) * 0.5f, msg, kColGold, Align::Center);
        } else if (buyTab) {  // GuildBankFrameBuyInfo
            const uint32_t cost = session_.guildBankTabCost();
            ui.textShadow(fn, PX(384.5f), PY(194), CLIENT_STR(PURCHASE_TAB_TEXT), kColWhite, Align::Center);
            const std::string b = CLIENT_FMT(NUM_GUILDBANK_TABS_PURCHASED, numTabs, gbk::kBankTabs);
            ui.textShadow(fs, PX(384.5f), PY(194) + static_cast<float>(fn.lineHeight()) + 1, b.c_str(), kColWhite,
                          Align::Center);
            const float cy = PY(194 + 38);
            const char* costLabel = CLIENT_STR(COSTS_LABEL);
            const float cx = PX(384.5f - 93) - static_cast<float>(fs.measure(costLabel));
            ui.textShadow(fs, cx, cy, costLabel, kColGold);
            const bool afford = session_.getMoneyCopper() + session_.guildBankMoney() >= cost;
            const float mw = drawCoins(ui, fs, npcArt_.money, cx + static_cast<float>(fs.measure(costLabel)) + 10 * kSX, cy,
                                       cost, false, afford ? kColWhite : kColRed);
            wotlkButton(ui, kIdGbPurchase, Rect{cx + static_cast<float>(fs.measure(costLabel)) + 10 * kSX + mw + 12 * kSX,
                                                 cy - 2, 124 * kSX, 21 * kSY},
                        CLIENT_STR(BANKSLOTPURCHASE), afford, focusable);
        } else {
            // The columns (grey without deposit and withdrawals) and the 98 buttons.
            const uint32_t colTint = !canDeposit && numWithdrawals == 0 ? 0xFF808080u : 0xFFFFFFFFu;
            for (int c = 0; c < 7; ++c) {
                const Rect cr{PX(30.0f + 103.0f * static_cast<float>(c)), PY(70), 100 * kSX, 311 * kSY};
                if (g.gbCols.pixels) ui.image(g.gbCols, cr, colTint);
                else ui.fill(cr, 0x60000000u);
            }
            for (int s = 0; s < gbk::kBankSlots; ++s) {
                float ux = 0, uy = 0;
                slotPos(s, ux, uy);
                const Rect r{std::floor(PX(ux)), std::floor(PY(uy)), 37 * kSX, 37 * kSY};
                const uint32_t id = kIdGbSlot0 + static_cast<uint32_t>(s);
                const bool f = focusable && winFocus_.addStop(id, r);
                const gbk::BankItem* it = session_.guildBankItem(tab, static_cast<uint8_t>(s));
                const bool held = g.gbHeld.active && g.gbHeld.tab == tab && g.gbHeld.slot == s;
                if (it) {
                    const game::ItemDef def = session_.protoItemDef(it->entry, it->count);
                    drawItemButton(ui, r, def.displayInfoId, it->count, -1, held ? 0xFF606060u : 0xFFFFFFFFu);
                }
                if (f) {
                    ui.fill(r, 0x50FFFFFFu);
                    if (it) {
                        g.tipEntry = it->entry;
                        g.tipCount = it->count;
                        g.tipItemRect = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};
                    }
                }
            }
        }
        // DEPOSIT / WITHDRAW and the bank's money.
        wotlkButton(ui, kIdGbDeposit, Rect{PX(658), PY(386), 100 * kSX, 21 * kSY}, CLIENT_STR(DEPOSIT), true, focusable);
        wotlkButton(ui, kIdGbWithdraw, Rect{PX(561), PY(386), 100 * kSX, 21 * kSY}, CLIENT_STR(WITHDRAW),
                    session_.canWithdrawGuildBankMoney(), focusable);
        drawMoneyFrame(ui, PX(765), PY(428) - 11.0f, static_cast<uint32_t>(std::min<uint64_t>(session_.guildBankMoney(), 0xFFFFFFFFull)),
                       true);
    } else if (g.gbMode == 2 || g.gbMode == 3) {
        const uint8_t logTab = g.gbMode == 3 ? gbk::kBankMoneyTab : tab;
        if (g.gbMode == 2 && noViewable) {
            ui.textShadow(fn, PX(384.5f), PY(216) - static_cast<float>(fn.lineHeight()) * 0.5f,
                          CLIENT_STR(NO_VIEWABLE_GUILDBANK_LOGS), kColGold, Align::Center);
        } else {
            const auto& log = session_.guildBankLog(logTab);
            // The newest first (insertMode TOP): the entries as they came, the last one on top.
            const int n = static_cast<int>(log.size());
            g.gbLogScroll = std::clamp(g.gbLogScroll, 0, std::max(0, n - kLogLines));
            const float x0 = PX(33), w = 688 * kSX;
            float y = PY(73);
            for (int k = 0; k < kLogLines && g.gbLogScroll + k < n; ++k) {
                const gbk::BankLogEntry& e = log[static_cast<size_t>(n - 1 - (g.gbLogScroll + k))];
                std::string who = session_.guildBankName(e.guid);
                if (who.empty()) who = CLIENT_STR(UNKNOWN);
                std::string msg;
                if (g.gbMode == 3) {
                    const std::string money = coinText(e.amount);
                    switch (e.type) {
                        case 4: msg = CLIENT_FMT(GUILDBANK_DEPOSIT_MONEY_FORMAT, who, money); break;
                        case 5: msg = CLIENT_FMT(GUILDBANK_WITHDRAW_MONEY_FORMAT, who, money); break;
                        case 8: msg = CLIENT_FMT(GUILDBANK_WITHDRAWFORTAB_MONEY_FORMAT, who, money); break;
                        case 9: msg = CLIENT_FMT(GUILDBANK_BUYTAB_MONEY_FORMAT, who, money); break;
                        default: msg = CLIENT_FMT(GUILDBANK_REPAIR_MONEY_FORMAT, who, money); break;
                    }
                } else {
                    const auto* info = session_.getItemInfo(e.entry);
                    const std::string item = "[" + (info ? info->name : std::string("?")) + "]";
                    const std::string q = e.count > 1 ? CLIENT_FMT(GUILDBANK_LOG_QUANTITY, e.count) : std::string();
                    if (e.type == 1) msg = CLIENT_FMT(GUILDBANK_DEPOSIT_FORMAT, who, item) + q;
                    else if (e.type == 2) msg = CLIENT_FMT(GUILDBANK_WITHDRAW_FORMAT, who, item) + q;
                    else if (e.type == 3 || e.type == 7) {
                        const uint8_t t1 = e.type == 3 ? logTab : e.otherTab, t2 = e.type == 3 ? e.otherTab : logTab;
                        msg = CLIENT_FMT(GUILDBANK_MOVE_FORMAT, who, item, e.count, session_.guildBankTabName(t1),
                                         session_.guildBankTabName(t2));
                    } else continue;
                }
                msg = pp::text::uiPlain(msg);  // the |cffff2020 .. |r of the withdraw forms (the PSP draws one colour)
                const uint32_t age = e.secondsAgo + (pp::logUptimeMs() - session_.guildBankLogAtMs(logTab)) / 1000u;
                int yy = 0, mo = 0, d = 0, h = 0;
                gbk::bankLogAge(age, yy, mo, d, h);
                const std::string when = "   " + CLIENT_FMT(GUILD_BANK_LOG_TIME, recentTime(yy, mo, d, h));
                const Font& lf = static_cast<float>(fs.measure((msg + when).c_str())) <= w ? fs : ft;
                ui.pushClip(Rect{x0, y, w, lh + 1});
                ui.textShadow(lf, x0, y, msg.c_str(), kColWhite);
                ui.textShadow(lf, x0 + static_cast<float>(lf.measure(msg.c_str())), y, when.c_str(), kColTeal);
                ui.popClip();
                y += lh + 1.0f;
            }
            if (n > kLogLines) {
                const Rect up{PX(735), PY(75), 11, 11}, dn{PX(735), PY(379) - 11, 11, 11};
                if (npcArt_.scrUp.pixels) ui.image(g.gbLogScroll > 0 ? npcArt_.scrUp : npcArt_.scrUpDis, up);
                if (npcArt_.scrDown.pixels) ui.image(g.gbLogScroll < n - kLogLines ? npcArt_.scrDown : npcArt_.scrDownDis, dn);
                if (focusable && winFocus_.addStop(kIdGbLogUp, up)) ui.fill(up, 0x40FFFFFFu);
                if (focusable && winFocus_.addStop(kIdGbLogDown, dn)) ui.fill(dn, 0x40FFFFFFu);
            }
        }
    } else {  // tab info (GuildBankInfo: the text box, SAVE_CHANGES with CanEditGuildTabInfo)
        const Rect box{PX(32), PY(74), 691 * kSX, 306 * kSY};
        ui.fill(box, 0x40000000u);
        const bool canEdit = tab < numTabs && (leader || (flags & gbk::kTabUpdateText));
        const bool f = focusable && canEdit && winFocus_.addStop(kIdGbInfoEdit, box);
        if (f) ui.fill(box, 0x20FFFFFFu);
        // The text word-wrapped (GameFontHighlight), as many lines as fit.
        std::vector<std::string> lines;
        {
            std::string cur;
            const char* s = g.gbInfoText.c_str();
            std::string word;
            auto flush = [&]() {
                if (cur.empty()) cur = word;
                else if (static_cast<float>(fs.measure((cur + " " + word).c_str())) <= box.w - 8) cur += " " + word;
                else {
                    lines.push_back(cur);
                    cur = word;
                }
                word.clear();
            };
            for (; *s; ++s) {
                if (*s == '\n') {
                    flush();
                    lines.push_back(cur);
                    cur.clear();
                } else if (*s == ' ') {
                    flush();
                } else {
                    word += *s;
                }
            }
            if (!word.empty()) flush();
            if (!cur.empty()) lines.push_back(cur);
        }
        float y = box.y + 3;
        for (const std::string& l : lines) {
            if (y + lh > box.y + box.h) break;
            ui.textShadow(fs, box.x + 4, y, l.c_str(), kColWhite);
            y += lh;
        }
        if (canEdit)
            wotlkButton(ui, kIdGbInfoSave, Rect{PX(25), PY(444 - 37 - 22), 100 * kSX, 22 * kSY}, CLIENT_STR(SAVE_CHANGES),
                        g.gbInfoEdited, focusable);
    }
    // GuildBankLimitLabel (TOP 6,-388): GUILDBANK_REMAINING_MONEY "<tab>", STACKS / NONE / UNLIMITED.
    if (showLimit && !noViewable && g.gbMode != 4) {
        const uint32_t left = session_.guildBankWithdrawalsLeft();
        std::string stack;
        if (static_cast<int32_t>(left) > 0) stack = pp::text::plural(CLIENT_FMT(STACKS, left));
        else if (left == 0) stack = CLIENT_STR(NONE);
        else stack = CLIENT_STR(UNLIMITED);
        // GUILDBANK_REMAINING_MONEY: the head gold, the white |c..|r part the amount.
        const std::string full = CLIENT_FMT(GUILDBANK_REMAINING_MONEY, session_.guildBankTabName(tab), stack);
        const size_t colour = full.find("|c");
        const std::string head = pp::text::uiPlain(full.substr(0, colour));
        stack = colour == std::string::npos ? std::string() : pp::text::uiPlain(full.substr(colour));
        const Font& lf = static_cast<float>(fs.measure((head + stack).c_str())) <= 300.0f ? fs : ft;
        const float tw = static_cast<float>(lf.measure((head + stack).c_str()));
        const Rect r = nameBorder(390.5f, 388.0f, tw + 20 * kSX);
        const float ty = r.y + (r.h - static_cast<float>(lf.lineHeight())) * 0.5f;
        const float x0 = PX(390.5f) - tw * 0.5f;
        ui.textShadow(lf, x0, ty, head.c_str(), kColGold);
        ui.textShadow(lf, x0 + static_cast<float>(lf.measure(head.c_str())), ty, stack.c_str(), kColWhite);
    }
    // GuildBankMoneyLimitLabel GUILDBANK_AVAILABLE_MONEY (BOTTOMLEFT 26,16) + GetGuildBankWithdrawMoney / UNLIMITED.
    {
        const float y = PY(428 - 13);
        const char* label = CLIENT_STR(GUILDBANK_AVAILABLE_MONEY);
        ui.textShadow(fs, PX(26), y, label, kColGold);
        const float x = PX(26) + static_cast<float>(fs.measure(label)) + 5 * kSX;
        const int64_t lim = session_.guildBankWithdrawMoney();
        if (lim >= 0) {
            const bool canW = session_.canWithdrawGuildBankMoney();
            const uint64_t amount = canW ? session_.guildBankMoney() : 0u;
            drawCoins(ui, fs, npcArt_.money, x + 8 * kSX, y, static_cast<uint32_t>(std::min<uint64_t>(
                                                                std::min<uint64_t>(static_cast<uint64_t>(lim), amount), 0xFFFFFFFFull)),
                      false, kColWhite);
        } else {
            ui.textShadow(fs, x, y, CLIENT_STR(UNLIMITED), kColWhite);  // GameFontHighlightSmall
        }
    }
    if (g.tipEntry != 0) {
        tipProtoItem_ = g.tipEntry;
        tipProtoCount_ = g.tipCount;
        tipProtoRect_ = g.tipItemRect;
    }
    return true;
}

// StaticPopup GUILDBANK_WITHDRAW / GUILDBANK_DEPOSIT (MoneyInputFrame; PSP: one keyboard line) and
// CONFIRM_BUY_GUILDBANK_TAB (the cost; YES / NO).
bool WorldScreen::drawGuildBankPopup(UiRenderer& ui) {
    GgbUi& g = ggbUi();
    StaticPopup p;
    p.focusable = uiMode_ && !settings_.isOpen();
    std::string edit;
    if (g.gbPopup == 3) {
        p.text = CLIENT_STR(CONFIRM_BUY_GUILDBANK_TAB);
        p.hasMoneyFrame = true;
        p.money = session_.guildBankTabCost();
        p.button1 = CLIENT_STR(YES);
        p.button2 = CLIENT_STR(NO);
    } else {
        p.text = g.gbPopup == 1 ? CLIENT_STR(GUILDBANK_WITHDRAW) : CLIENT_STR(GUILDBANK_DEPOSIT);
        edit = OWN_FMT(GB_MONEY_EDIT, g.gbMoney[0], g.gbMoney[1], g.gbMoney[2]);
        p.editText = edit.c_str();
        p.editId = kIdGbMoneyGold;
        p.button1 = CLIENT_STR(ACCEPT);
        p.button2 = CLIENT_STR(CANCEL);
    }
    p.button1Id = kIdGbPopupOk;
    p.button2Id = kIdGbPopupCancel;
    drawStaticPopup(ui, p);
    return true;
}

// GuildBankPopupFrame: the tab's name (GUILDBANK_POPUP_TEXT) and its icon (16 of the item icons per page).
bool WorldScreen::drawGuildBankTabPopup(UiRenderer& ui) {
    GgbUi& g = ggbUi();
    const bool focusable = uiMode_ && !settings_.isOpen();
    const Font& fs = kit_.font(kFontSmall);
    const Rect r{std::floor((pp::kScreenW - 160.0f) * 0.5f), 30.0f, 160.0f, 200.0f};
    drawDialogBox(ui, r);
    ui.textShadow(fs, r.x + 12, r.y + 10, CLIENT_STR(GUILDBANK_POPUP_TEXT), kColGold);
    const Rect nameBox{r.x + 12, r.y + 24, r.w - 24, 14};
    ui.fill(nameBox, 0x80000000u);
    ui.textShadow(fs, nameBox.x + 3, nameBox.y + 1, g.tpName.c_str(), kColWhite);
    if (focusable && winFocus_.addStop(kIdGbTpName, nameBox)) ui.fill(nameBox, 0x30FFFFFFu);
    const int n = static_cast<int>(g.tpIcons.size());
    const int rows = (n + 3) / 4;
    g.tpIconOffset = std::clamp(g.tpIconOffset, 0, std::max(0, rows - 4));
    for (int k = 0; k < 16; ++k) {
        const int idx = g.tpIconOffset * 4 + k;
        if (idx >= n) break;
        const Rect b{r.x + 14 + static_cast<float>(k % 4) * 30.0f, r.y + 44 + static_cast<float>(k / 4) * 30.0f, 24, 24};
        if (const pp::Texture* t = itemIcons().byDisplay(g.tpIcons[static_cast<size_t>(idx)].first, spellFrame_)) ui.image(*t, b);
        else ui.fill(b, 0xFF303030u);
        if (idx == g.tpIconSel) ui.fill(b, 0x50FFFFFFu);  // checked
        if (focusable && winFocus_.addStop(kIdGbTpIcon0 + static_cast<uint32_t>(k), b)) ui.fill(b, 0x30FFFFFFu);
    }
    if (rows > 4) {
        const Rect up{r.x + r.w - 18, r.y + 44, 11, 11}, dn{r.x + r.w - 18, r.y + 150, 11, 11};
        if (npcArt_.scrUp.pixels) ui.image(npcArt_.scrUp, up);
        if (npcArt_.scrDown.pixels) ui.image(npcArt_.scrDown, dn);
        if (focusable && winFocus_.addStop(kIdGbTpUp, up)) ui.fill(up, 0x40FFFFFFu);
        if (focusable && winFocus_.addStop(kIdGbTpDown, dn)) ui.fill(dn, 0x40FFFFFFu);
    }
    wotlkButton(ui, kIdGbTpOk, Rect{r.x + 12, r.y + r.h - 22, 64, 14}, CLIENT_STR(OKAY), true, focusable);
    wotlkButton(ui, kIdGbTpCancel, Rect{r.x + r.w - 76, r.y + r.h - 22, 64, 14}, CLIENT_STR(CANCEL), true, focusable);
    return true;
}

// A bag slot clicked while the vault is open: a vault item on the cursor goes there; Circle on a bag item
// deposits it (upstream lua_UseContainerItem / guildBankDepositFromInventory: unless the mail or the bank window takes
// it first, into the first empty slot of the tab being viewed).
bool WorldScreen::guildBankBagClick(uint32_t id, bool alt) {
    GgbUi& g = ggbUi();
    int bc = 0, bs = 0;
    if (!bagSlotId(id, bc, bs)) return false;
    uint8_t bag = 0, slot = 0;
    if (!inventoryIdToWire(id, bag, slot)) return false;
    if (!alt && g.gbHeld.active) {
        session_.guildBankWithdraw(g.gbHeld.tab, g.gbHeld.slot, bag, slot);
        g.gbHeld = GgbUi::Held{};
        return true;
    }
    if (alt && !windowOpen(kWinBank) && !windowOpen(kWinMail)) {
        if (!itemAtWire(bag, slot)) return false;
        const uint8_t tab = session_.guildBankTab();
        int free = -1;
        for (int i = 0; i < gbk::kBankSlots && free < 0; ++i)
            if (!session_.guildBankItem(tab, static_cast<uint8_t>(i))) free = i;
        if (free < 0) {
            uiError(CLIENT_STR(ERR_BANK_FULL));  // the tab is full
            return true;
        }
        session_.guildBankDeposit(tab, static_cast<uint8_t>(free), bag, slot);
        return true;
    }
    return false;
}

bool WorldScreen::guildBankAction(uint32_t act, bool alt) {
    GgbUi& g = ggbUi();
    if (g.gbGuard && !alt && windowOpen(kWinGuildBank) && act < kIdGbPopupOk) return true;
    const uint8_t tab = session_.guildBankTab();
    auto openPopup = [&](int kind) {
        g.gbPopup = kind;
        if (kind != 3) g.gbMoney[0] = g.gbMoney[1] = g.gbMoney[2] = 0;  // MoneyInputFrame_ResetMoney
        openWindow(kWinGuildBankPopup);
        winFocus_.setFocus(kind == 3 ? kIdGbPopupCancel : kIdGbMoneyGold);
        playSoundByName("igMainMenuOption");
    };
    switch (act) {
        case kIdGbClose:
            if (!alt) closeWindow(kWinGuildBank);
            return true;
        case kIdGbDeposit:
            if (!alt) openPopup(2);
            return true;
        case kIdGbWithdraw:
            if (!alt && session_.canWithdrawGuildBankMoney()) openPopup(1);
            return true;
        case kIdGbPurchase:
            if (!alt) openPopup(3);
            return true;
        case kIdGbPopupOk: {
            const uint32_t copper = g.gbMoney[0] * 10000u + g.gbMoney[1] * 100u + g.gbMoney[2];
            if (g.gbPopup == 1) session_.guildBankWithdrawMoney(copper);
            else if (g.gbPopup == 2) session_.guildBankDepositMoney(copper);
            else if (g.gbPopup == 3) session_.guildBankBuyTab();
            g.gbPopup = 0;
            closeWindow(kWinGuildBankPopup);
            return true;
        }
        case kIdGbPopupCancel:
            g.gbPopup = 0;
            closeWindow(kWinGuildBankPopup);
            return true;
        case kIdGbMoneyGold:
        case kIdGbMoneySilver:
        case kIdGbMoneyCopper:
            if (pp::oskActive()) return true;
            g.gbOskField = 1;
            if (!pp::oskBeginFor(kOskOwner,
                                 std::string(g.gbPopup == 1 ? CLIENT_STR(GUILDBANK_WITHDRAW) : CLIENT_STR(GUILDBANK_DEPOSIT)) +
                                     " " + OWN_STR(GB_MONEY_INPUT_HINT),
                                 std::string(), 20, pp::OskCharset::Text))
                g.gbOskField = 0;
            return true;
        case kIdGbInfoEdit:
            if (pp::oskActive()) return true;
            g.gbOskField = 2;
            if (!pp::oskBeginFor(kOskOwner, CLIENT_FMT(GUILDBANK_INFO_TITLE_FORMAT, session_.guildBankTabName(tab)),
                                 g.gbInfoText, 500, pp::OskCharset::Text))
                g.gbOskField = 0;
            return true;
        case kIdGbInfoSave:  // GuildBankInfoSaveButton: SetGuildBankText
            session_.guildBankSetText(tab, g.gbInfoText);
            g.gbInfoEdited = false;
            return true;
        case kIdGbLogUp: g.gbLogScroll = std::max(0, g.gbLogScroll - 3); return true;
        case kIdGbLogDown: g.gbLogScroll += 3; return true;
        case kIdGbTpName:
            if (pp::oskActive()) return true;
            g.gbOskField = 3;
            if (!pp::oskBeginFor(kOskOwner, CLIENT_STR(GUILDBANK_POPUP_TEXT), g.tpName, 15, pp::OskCharset::Text))
                g.gbOskField = 0;
            return true;
        case kIdGbTpUp: g.tpIconOffset = std::max(0, g.tpIconOffset - 1); return true;
        case kIdGbTpDown: ++g.tpIconOffset; return true;
        case kIdGbTpOk: {  // GuildBankPopupOkayButton_OnClick: SetGuildBankTabInfo(tab, name or GUILDBANK_TAB_NUMBER, icon)
            std::string name = g.tpName.empty() ? session_.guildBankTabName(tab) : g.tpName;
            const std::string icon = g.tpIconSel >= 0 && g.tpIconSel < static_cast<int>(g.tpIcons.size())
                                         ? g.tpIcons[static_cast<size_t>(g.tpIconSel)].second
                                         : session_.guildBankTabIcon(tab);
            session_.guildBankSetTabInfo(tab, name, icon);
            closeWindow(kWinGuildBankTabPopup);
            return true;
        }
        case kIdGbTpCancel: closeWindow(kWinGuildBankTabPopup); return true;
        default: break;
    }
    if (act >= kIdGbTpIcon0 && act < kIdGbTpIcon0 + 16) {
        g.tpIconSel = g.tpIconOffset * 4 + static_cast<int>(act - kIdGbTpIcon0);
        return true;
    }
    if (act >= kIdGbMode1 && act < kIdGbMode1 + 4) {  // GuildBankFrameTab_OnClick
        if (alt) return true;
        g.gbMode = static_cast<int>(act - kIdGbMode1) + 1;
        g.gbLogScroll = 0;
        if (g.gbMode == 1) session_.queryGuildBankTab(tab);
        else if (g.gbMode == 2) session_.guildBankQueryLog(tab);
        else if (g.gbMode == 3) session_.guildBankQueryLog(gbk::kBankMoneyTab);
        else {
            g.gbInfoEdited = false;
            g.gbInfoText = session_.guildBankText(tab);
            session_.guildBankQueryText(tab);
        }
        playSoundByName("igCharacterInfoTab");
        PSP_LOGI(pp::tag::Ctrl, "guildbank_mode %d", g.gbMode);
        return true;
    }
    if (act >= kIdGbTab0 && act < kIdGbTab0 + gbk::kBankTabs) {  // GuildBankTab_OnClick
        const uint8_t t = static_cast<uint8_t>(act - kIdGbTab0);
        if (g.gbMode == 4 && g.gbInfoEdited) session_.guildBankSetText(tab, g.gbInfoText);  // the save button's click
        session_.setGuildBankTab(t);
        g.gbLogScroll = 0;
        g.gbInfoEdited = false;
        g.gbHeld = GgbUi::Held{};
        if (alt && session_.isGuildLeader() && t < session_.guildBankNumTabs()) {  // GuildBankPopupFrame
            g.tpName = session_.guildBankTabName(t);
            g.tpIconSel = -1;
            g.tpIconOffset = 0;
            g.tpIcons.clear();
            const std::string cur = lower(session_.guildBankTabIcon(t));
            forEachIcon([&](uint32_t did, const char* name) {
                if (g.tpIcons.size() < 6000) g.tpIcons.emplace_back(did, name);
                if (lower(name) == cur) g.tpIconSel = static_cast<int>(g.tpIcons.size()) - 1;
            });
            if (g.tpIconSel > 0) g.tpIconOffset = g.tpIconSel / 4;
            openWindow(kWinGuildBankTabPopup);
            winFocus_.setFocus(kIdGbTpOk);
            PSP_LOGI(pp::tag::Gfx, "guildbank_icons n=%u kb=%u", static_cast<unsigned>(g.tpIcons.size()),
                     static_cast<unsigned>(g.tpIcons.size() * 40u / 1024u));
            return true;
        }
        if (g.gbMode == 2) {
            session_.queryGuildBankTab(t);  // the withdrawals left
            session_.guildBankQueryLog(t);
        } else if (g.gbMode == 4) {
            g.gbInfoText = session_.guildBankText(t);
            session_.guildBankQueryText(t);
        } else {
            session_.queryGuildBankTab(t);
        }
        return true;
    }
    if (act >= kIdGbSlot0 && act < kIdGbSlot0 + gbk::kBankSlots) {
        const uint8_t s = static_cast<uint8_t>(act - kIdGbSlot0);
        if (alt) {  // AutoStoreGuildBankItem
            if (!g.gbHeld.active) session_.guildBankAutoStore(tab, s);
            return true;
        }
        // PickupGuildBankItem: a bag item on the cursor = the deposit here; a vault item = the move; else take it.
        if (invHeld_.active) {
            const InvHeld held = invHeld_;
            invHeld_ = InvHeld{};
            session_.guildBankDeposit(tab, s, held.bag, held.slot);
            return true;
        }
        if (g.gbHeld.active) {
            const GgbUi::Held h = g.gbHeld;
            g.gbHeld = GgbUi::Held{};
            if (h.tab != tab || h.slot != s) session_.guildBankMove(tab, s, h.tab, h.slot);
            return true;
        }
        if (session_.guildBankItem(tab, s)) {
            g.gbHeld = GgbUi::Held{true, tab, s};
            playUiSound(UiSound::PickupBag);
        }
        return true;
    }
    return true;
}

}  // namespace wowee::ui::psp
