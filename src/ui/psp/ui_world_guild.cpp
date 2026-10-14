// FriendsFrame's Guild tab of the original ruRU 3.3.5a client
// (FriendsFrame.xml GuildFrame and its GUILDFRAME_POPUPS, FriendsFrame.lua GuildStatus_Update and the GuildControl
// functions, StaticPopup.lua) over the session's guild half (psp_world_session_social.cpp):
//   - GuildFrame: GuildFrameLFGFrame SHOW_OFFLINE_MEMBERS (CVar guildShowOffline - PSP: wowee.cfg), the two
//     views (GuildPlayerStatusFrame: NAME / ZONE / LEVEL_ABBR / CLASS, GuildStatusFrame: NAME / RANK /
//     LABEL_NOTE / LASTONLINE - the column headers SortGuildRoster; GuildFrameGuildListToggleButton switches), 13 buttons
//     (offline grey, the class colour, the status CHAT_FLAG_AFK / CHAT_FLAG_DND), GUILD_TOTAL / GUILD_TOTALONLINE,
//     GUILD_MOTD_LABEL and the MOTD (GuildMOTDEditButton: SET_GUILDMOTD), GUILD_INFORMATION / ADDMEMBER
//     (ADD_GUILDMEMBER) / GUILDCONTROL (the leader);
//   - GuildMemberDetailFrame (the left click on a member): name, level / class, ZONE_COLON, RANK_COLON (promote /
//     demote by the rights - GuildPromote / GuildDemote), LAST_ONLINE_COLON (GuildFrame_GetLastOnline), NOTE_COLON
//     (SET_GUILDPLAYERNOTE) and OFFICER_NOTE_COLON (CanViewOfficerNote / CanEditOfficerNote), REMOVE
//     (REMOVE_GUILDMEMBER), GROUP_INVITE (InviteUnit);
//   - GuildInfoFrame (GetGuildInfoText / SetGuildInfoText, CanEditGuildInfo) with GUILD_BANK_LOG -> GuildEventLogFrame
//     (QueryGuildEventLog, GuildEventLog_Update: GUILDEVENT_TYPE_* + GUILD_BANK_LOG_TIME);
//   - GuildControlPopupFrame (the leader): the rank dropdown, the rank's name, «+» ADD_GUILDRANK (below 10 ranks) /
//     KEY_MINUS (the lowest rank above 5, nobody in it), the 16 rights (GUILDCONTROL_OPTION1..17 without 14), GUILDCONTROL_WITHDRAW_GOLD
//     the gold per day, the bank tabs' permissions (view / deposit / update text, items per day), ACCEPT
//     (GuildControlSaveRank: CMSG_GUILD_RANK) / CANCEL.
// PSP: the edit boxes are the system keyboard; Circle on a member = the right click (the FRIEND unit menu, online).
#include "ui/psp/ui_world_social.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace so = wowee::platform::psp::social;
using soc::kSX;
using soc::kSY;
using soc::PX;
using soc::PY;
using soc::R;

namespace {

// GlobalStrings.lua (strings.pak of the running language).
// GUILDCONTROL_OPTION1..17 (14 has no check box: "a deprecated flag").
constexpr pp::CS kOption[17] = {
    pp::CS::GUILDCONTROL_OPTION1,  pp::CS::GUILDCONTROL_OPTION2,  pp::CS::GUILDCONTROL_OPTION3,
    pp::CS::GUILDCONTROL_OPTION4,  pp::CS::GUILDCONTROL_OPTION5,  pp::CS::GUILDCONTROL_OPTION6,
    pp::CS::GUILDCONTROL_OPTION7,  pp::CS::GUILDCONTROL_OPTION8,  pp::CS::GUILDCONTROL_OPTION9,
    pp::CS::GUILDCONTROL_OPTION10, pp::CS::GUILDCONTROL_OPTION11, pp::CS::GUILDCONTROL_OPTION12,
    pp::CS::GUILDCONTROL_OPTION13, pp::CS::GUILDCONTROL_OPTION14, pp::CS::GUILDCONTROL_OPTION15,
    pp::CS::GUILDCONTROL_OPTION16, pp::CS::GUILDCONTROL_OPTION17};

constexpr uint32_t kColDisable = 0xFF808080u;   // (0.5) offline members / GRAY_FONT_COLOR
constexpr uint32_t kColNoteDis = 0xFFA6A6A6u;   // (0.65) a note the rank cannot edit
constexpr uint32_t kColCyan = 0xFF999900u;      // |cff009999 (the event log's time)

std::string fmt(const char* f, std::initializer_list<std::string> args) {
    const std::vector<std::string> a(args);
    std::string out;
    size_t next = 0;
    for (const char* p = f; *p; ++p) {
        if (*p == '%' && p[1] >= '1' && p[1] <= '9' && p[2] == '$' && (p[3] == 's' || p[3] == 'd')) {
            const size_t i = static_cast<size_t>(p[1] - '1');
            if (i < a.size()) out += a[i];
            p += 3;
        } else if (*p == '%' && (p[1] == 's' || p[1] == 'd')) {
            if (next < a.size()) out += a[next];
            ++next;
            ++p;
        } else {
            out.push_back(*p);
        }
    }
    for (size_t at; (at = out.find("|3-")) != std::string::npos;) {  // a player's name stays as written
        const size_t open = out.find('(', at), close = out.find(')', at);
        if (open == std::string::npos || close == std::string::npos || close < open) break;
        out = out.substr(0, at) + out.substr(open + 1, close - open - 1) + out.substr(close + 1);
    }
    return pp::text::plural(out);  // |4one:many; / |4one:few:many; after the number
}

uint32_t classColor(uint32_t cls) {  // RAID_CLASS_COLORS
    switch (cls) {
        case 1: return 0xFF6E9CC7u;
        case 2: return 0xFFBA8CF5u;
        case 3: return 0xFF73D4ABu;
        case 4: return 0xFF69F5FFu;
        case 5: return 0xFFFFFFFFu;
        case 6: return 0xFF3B1FC4u;
        case 7: return 0xFFDE7000u;
        case 8: return 0xFFF0CC69u;
        case 9: return 0xFFC98294u;
        case 11: return 0xFF0A7DFFu;
        default: return 0;
    }
}

std::string fitText(const Font& f, const std::string& s, float w) {
    if (static_cast<float>(f.measure(s.c_str())) <= w) return s;
    std::string out = s;
    while (!out.empty() && static_cast<float>(f.measure(out.c_str())) > w) {
        size_t cut = out.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.erase(cut);
    }
    return out;
}

// A FontString with |cAARRGGBB ... |r colour codes: the segments in their colours. Returns the width.
float drawColored(UiRenderer& ui, const Font& f, float x, float y, const std::string& s, uint32_t base) {
    float cx = x;
    uint32_t col = base;
    size_t i = 0;
    std::string seg;
    auto flush = [&]() {
        if (seg.empty()) return;
        ui.textShadow(f, cx, y, seg.c_str(), col);
        cx += static_cast<float>(f.measure(seg.c_str()));
        seg.clear();
    };
    while (i < s.size()) {
        if (s[i] == '|' && i + 9 < s.size() + 1 && i + 1 < s.size() && s[i + 1] == 'c' && i + 10 <= s.size()) {
            flush();
            const uint32_t argb = static_cast<uint32_t>(std::strtoul(s.substr(i + 2, 8).c_str(), nullptr, 16));
            col = 0xFF000000u | ((argb & 0xFF) << 16) | (argb & 0xFF00) | ((argb >> 16) & 0xFF);
            i += 10;
        } else if (s[i] == '|' && i + 1 < s.size() && s[i + 1] == 'r') {
            flush();
            col = base;
            i += 2;
        } else {
            seg += s[i++];
        }
    }
    flush();
    return cx - x;
}

// The rows GuildStatus_Update shows: GetNumGuildMembers() - every kept member with guildShowOffline, else the online
// ones (the roster sorts them first then).
int guildRows(const pp::WorldSession& s) {
    const auto& r = s.guildRoster();
    if (s.guildShowOffline()) return static_cast<int>(r.size());
    int n = 0;
    for (const auto& m : r) n += m.online() ? 1 : 0;
    return n;
}

// UICheckButtonTemplate: the box (UI-CheckBox-Up), the check (UI-CheckBox-Check), the highlight (ADD); disabled grey.
void drawCheck(UiRenderer& ui, const pp::Texture& up, const pp::Texture& check, const pp::Texture& hl, const Rect& r,
               bool checked, bool enabled, bool focused) {
    if (up.pixels) ui.image(up, r, enabled ? 0xFFFFFFFFu : 0xFF808080u);
    else ui.fill(r, 0x80000000u);
    if (checked && check.pixels) ui.image(check, r, enabled ? 0xFFFFFFFFu : 0xFF808080u);
    if (focused && hl.pixels) ui.image(hl, r, 0xFFFFFFFFu, Blend::Add);
}

// The tooltip backdrop of the note / info boxes (Interface\Tooltips, TOOLTIP_DEFAULT_COLOR border, dark background).
void drawTipBox(UiRenderer& ui, const UiKit& kit, const Rect& r) {
    if (kit.has(kTexTipBg)) ui.tile(kit.tex(kTexTipBg), Rect{r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 0xC0FFFFFFu);
    else ui.fill(r, 0xC0100000u);
    if (kit.has(kTexTipBorder)) ui.nineSlice(kit.tex(kTexTipBorder), r);
}

}  // namespace

// GuildFrame_GetLastOnline -> RecentTimeDate(GetGuildRosterLastOnline(i)).
std::string WorldScreen::guildLastOnline(float days) {
    int y = 0, mo = 0, d = 0, h = 0;
    so::lastOnline(days, y, mo, d, h);
    if (y) return fmt(CLIENT_STR(LASTONLINE_YEARS), {std::to_string(y)});
    if (mo) return fmt(CLIENT_STR(LASTONLINE_MONTHS), {std::to_string(mo)});
    if (d) return fmt(CLIENT_STR(LASTONLINE_DAYS), {std::to_string(d)});
    if (h) return fmt(CLIENT_STR(LASTONLINE_HOURS), {std::to_string(h)});
    return CLIENT_STR(LASTONLINE_MINS);
}

// GuildFramePopup_Show: one of GUILDFRAME_POPUPS at a time (the others hide); 0 hides them all.
void WorldScreen::guildPopupShow(int popup) {
    SocialUi& s = socialUi();
    s.popup = popup;
    if (popup == 2) {  // GuildInfoFrame OnShow: the cached text or GetGuildInfoText
        s.infoText = session_.guildInfoText();
        s.infoEdited = false;
        s.infoScroll = 0;
    } else if (popup == 3) {  // GuildControlPopupFrame_OnShow: the accept disabled, GuildControlPopupframe_Update
        if (!s.ctl.initialized) {  // GuildControlPopupFrame_Initialize: rank 1, bank tab 1
            s.ctl.initialized = true;
            s.ctl.bankTab = 1;
            guildControlLoadRank(1);
        } else {
            guildControlLoadRank(s.ctl.rank);
        }
        s.ctl.acceptEnabled = false;
    } else if (popup == 4) {  // GuildEventLogFrame OnShow: QueryGuildEventLog
        s.logScroll = 0;
        session_.guildEventLogQuery();
    }
}

// GuildControlSetRank(rank) + GuildControlPopupframe_Update: the rank's flags, name, gold, pending tab permissions.
void WorldScreen::guildControlLoadRank(int rank) {
    SocialUi& s = socialUi();
    const int n = static_cast<int>(session_.guildNumRanks());
    rank = std::clamp(rank, 1, std::max(1, n));
    if (s.ctl.previousRank != rank)
        for (auto& p : s.ctl.pending) p = SocialUi::Ctl::Pending{};  // ClearPendingGuildBankPermissions
    s.ctl.previousRank = rank;
    s.ctl.rank = rank;
    const auto& rk = session_.guildRank(static_cast<uint32_t>(rank - 1));
    for (int i = 0; i < 17; ++i) s.ctl.flags[i] = (rk.rights & so::kRankFlagBit[i]) != 0;
    const so::GuildQuery* g = session_.ownGuild();
    s.ctl.name = g ? so::capText(g->ranks[rank - 1], so::kRankNameChars, true) : std::string();
    s.ctl.gold = rk.moneyPerDay == 0xFFFFFFFFu ? 1000u : rk.moneyPerDay / 10000u;  // -1 (the leader): MAX_GOLD_WITHDRAW
    s.ctl.goldChanged = false;
    s.ctl.acceptEnabled = false;
}

// --- GuildFrame (GuildStatus_Update) ---------------------------------------------------------------------------------

void WorldScreen::drawGuildFrame(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const auto& roster = session_.guildRoster();
    const int num = guildRows(session_);
    constexpr int kRows = 13;  // GUILDMEMBERS_TO_DISPLAY (the buttons 16 units, TOPLEFT 15,-95)
    const bool scroll = num > kRows;
    s.guildOffset = std::clamp(s.guildOffset, 0, std::max(0, num - kRows));
    // The selection is by guid (SetGuildRosterSelection): a member gone - none.
    int selIdx = 0;
    for (int i = 0; i < num; ++i)
        if (roster[static_cast<size_t>(i)].guid == s.selectedMember && s.selectedMember != 0) selIdx = i + 1;
    if (selIdx == 0) {
        s.selectedMember = 0;
        if (s.popup == 1) s.popup = 0;  // GuildMemberDetailFrame:Hide()
    }
    // GuildFrameLFGFrame: GuildFrameLFGButton (RIGHT -8,0 of the 210 x 23 frame at TOPRIGHT -42,-38) and its text.
    {
        const Rect cb = R(342 - 8 - 20, 38 + 1.5f, 20, 20);
        const bool f = focusable && winFocus_.addStop(kIdSfGuildOffline, cb);
        drawCheck(ui, s.cbUp, s.check, s.cbHl, cb, session_.guildShowOffline(), true, f);
        ui.textShadow(ft, cb.x - 10 * kSX, cb.y + (cb.h - static_cast<float>(ft.lineHeight())) * 0.5f, CLIENT_STR(SHOW_OFFLINE_MEMBERS),
                      kColWhite, Align::Right);
    }
    // The column headers: GuildPlayerStatusFrame (NAME 83, ZONE 105 / 120, LEVEL_ABBR 32, CLASS 92) or
    // GuildStatusFrame (NAME 83, RANK 75, LABEL_NOTE 75 / 90, LASTONLINE 75).
    const float colPlayer[4] = {83.0f, scroll ? 105.0f : 120.0f, 32.0f, 92.0f};
    const float colGuild[4] = {83.0f, 75.0f, scroll ? 75.0f : 90.0f, 75.0f};
    const char* const textPlayer[4] = {CLIENT_STR(NAME), CLIENT_STR(ZONE), CLIENT_STR(LEVEL_ABBR), CLIENT_STR(CLASS)};
    const char* const textGuild[4] = {CLIENT_STR(NAME), CLIENT_STR(RANK), CLIENT_STR(LABEL_NOTE), CLIENT_STR(LASTONLINE)};
    const float* colW = s.playerStatus ? colPlayer : colGuild;
    const char* const* colText = s.playerStatus ? textPlayer : textGuild;
    float cx = 20.0f;
    for (int c = 0; c < 4; ++c) {
        const Rect hr = R(cx, 70, colW[c], 24);
        if (s.coltab.pixels) {
            const float tw = static_cast<float>(s.coltab.uw), th = static_cast<float>(s.coltab.uh) * 0.75f;
            ui.image(s.coltab, R(cx, 70, 5, 24), SrcRect{0, 0, tw * 0.078125f, th});
            ui.image(s.coltab, R(cx + 5, 70, colW[c] - 9, 24), SrcRect{tw * 0.078125f, 0, tw * 0.828125f, th});
            ui.image(s.coltab, R(cx + colW[c] - 4, 70, 4, 24), SrcRect{tw * 0.90625f, 0, tw * 0.0625f, th});
        }
        const uint32_t id = kIdSfGuildCol0 + static_cast<uint32_t>(c + (s.playerStatus ? 0 : 4));
        const bool f = focusable && winFocus_.addStop(id, hr);
        if (f && s.colhl.pixels) ui.image(s.colhl, hr, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(ft, hr.x + 8 * kSX, hr.y + (hr.h - static_cast<float>(ft.lineHeight())) * 0.5f,
                      fitText(ft, colText[c], hr.w - 8 * kSX).c_str(), kColWhite);
        cx += colW[c] - 2.0f;
    }
    // The buttons.
    for (int i = 0; i < kRows; ++i) {
        const int idx = s.guildOffset + i + 1;
        if (idx > num) break;
        const auto& m = roster[static_cast<size_t>(idx - 1)];
        const Rect b = R(15, 95 + i * 16.0f, 298, 16);
        const uint32_t id = kIdSfGuild0 + static_cast<uint32_t>(i);
        const bool f = focusable && winFocus_.addStop(id, b);
        if ((f || idx == selIdx) && s.hlbar.pixels) ui.image(s.hlbar, R(15 + 5, 95 + i * 16.0f + 2, 298, 16), 0xFFFFFFFFu, Blend::Add);
        const float ty = b.y + (b.h - static_cast<float>(ft.lineHeight())) * 0.5f;
        const bool on = m.online();
        const uint32_t cc = classColor(m.cls);
        const std::string clsName = socialClassName(m.cls, m.gender == 1);
        if (s.playerStatus) {  // Name 78 | Zone 90 (95 / 110) | Level 20 | Class 80
            float x = 15 + 14;
            ui.textShadow(ft, PX(x), ty, fitText(ft, m.name, 78 * kSX).c_str(), on ? kColGold : kColDisable);
            x += 78 + 3;
            const float zw = scroll ? 95.0f : 110.0f;
            ui.textShadow(ft, PX(x), ty, fitText(ft, socialZoneName(m.zone), zw * kSX).c_str(), on ? kColWhite : kColDisable);
            x += zw + 4;
            ui.textShadow(ft, PX(x + 10), ty, std::to_string(m.level).c_str(), on ? kColWhite : kColDisable, Align::Center);
            x += 20 + 12;
            ui.textShadow(ft, PX(x), ty, fitText(ft, clsName, 80 * kSX).c_str(), !on ? kColDisable : cc ? cc : kColGold);
        } else {  // Name 75 | Rank 55 | Note 85 (70) | Online 80
            const so::GuildQuery* g = session_.ownGuild();
            const std::string rank = g && m.rank < so::kGuildRanks ? g->ranks[m.rank] : std::string();
            std::string online;
            if (on) online = (m.flags & 4) ? CLIENT_STR(CHAT_FLAG_DND) : (m.flags & 2) ? CLIENT_STR(CHAT_FLAG_AFK) : CLIENT_STR(GUILD_ONLINE_LABEL);
            else online = guildLastOnline(m.offlineDays);
            float x = 15 + 14;
            ui.textShadow(ft, PX(x), ty, fitText(ft, m.name, 75 * kSX).c_str(), on ? kColGold : kColDisable);
            x += 75 + 5;
            ui.textShadow(ft, PX(x), ty, fitText(ft, rank, 55 * kSX).c_str(), on ? kColWhite : kColDisable);
            x += 55 + 11;
            const float nw = scroll ? 70.0f : 85.0f;
            ui.textShadow(ft, PX(x), ty, fitText(ft, m.note, nw * kSX).c_str(), on ? kColWhite : kColDisable);
            x += nw + 10;
            ui.textShadow(ft, PX(x), ty, fitText(ft, online, 80 * kSX).c_str(), !on ? kColDisable : cc ? cc : kColGold);
        }
    }
    // GuildListScrollFrame's bar (TOPRIGHT -67,-98, 296 x 237) while it scrolls.
    if (scroll) {
        if (s.guildScroll.pixels) ui.image(s.guildScroll, R(315, 93, 31, 244));
        const Rect up = R(384 - 67 + 6, 98, 16, 16), dn = R(384 - 67 + 6, 98 + 237 - 16, 16, 16);
        const bool canUp = s.guildOffset > 0, canDown = s.guildOffset + kRows < num;
        const bool fu = focusable && canUp && winFocus_.addStop(kIdSfGuildUp, up);
        const bool fd = focusable && canDown && winFocus_.addStop(kIdSfGuildDown, dn);
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDown ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (fu) ui.fill(up, 0x40FFFFFFu);
        if (fd) ui.fill(dn, 0x40FFFFFFu);
    }
    // GUILD_TOTAL (BOTTOM at the frame's LEFT + 75,-75) and GUILD_TOTALONLINE beside it.
    {
        int online = 0;
        for (int i = 0; i < num; ++i) online += roster[static_cast<size_t>(i)].online() ? 1 : 0;
        const uint32_t total = session_.guildShowOffline() ? std::max<uint32_t>(session_.guildMemberCount(),
                                                                                 static_cast<uint32_t>(num))
                                                           : static_cast<uint32_t>(num);
        if (session_.guildShowOffline() && total > static_cast<uint32_t>(num))
            online = static_cast<int>(session_.guildOnlineCount());  // PSP: a roster above 500 kept - the real count
        const std::string t1 = fmt(CLIENT_STR(GUILD_TOTAL), {std::to_string(total)}), t2 = fmt(CLIENT_STR(GUILD_TOTALONLINE), {std::to_string(online)});
        const float y = PY(256 + 75 - 16) + (16 * kSY - static_cast<float>(ft.lineHeight())) * 0.5f;
        std::string plain1;  // the width of t1 without its colour codes (centred at x 75)
        for (size_t i = 0; i < t1.size();) {
            if (t1[i] == '|' && i + 1 < t1.size() && t1[i + 1] == 'c') i += 10;
            else if (t1[i] == '|' && i + 1 < t1.size() && t1[i + 1] == 'r') i += 2;
            else plain1 += t1[i++];
        }
        const float w1 = static_cast<float>(ft.measure(plain1.c_str()));
        const float x1 = PX(75) - w1 * 0.5f;
        drawColored(ui, ft, x1, y, t1, kColGold);
        drawColored(ui, ft, x1 + w1 + 3 * kSX, y, t2, kColGold);
    }
    // GuildFrameNotesLabel (TOPLEFT 23,-344) and GuildFrameNotesText (315 x 45 under it) = GuildMOTDEditButton.
    {
        ui.textShadow(ft, PX(23), PY(344), CLIENT_STR(GUILD_MOTD_LABEL), kColGold);
        const bool canEdit = session_.guildCan(so::kRightSetMotd);
        std::string motd = session_.guildMotd();
        if (canEdit && motd.empty()) motd = CLIENT_STR(GUILD_MOTD_EDITLABEL);
        const Rect box = R(23, 344 + 11 + 3, 315, 45);
        const bool f = focusable && canEdit && winFocus_.addStop(kIdSfGuildMotd, box);
        float y = box.y;
        for (const std::string& l : wrapQuestText(ft, motd, box.w)) {
            if (y + static_cast<float>(ft.lineHeight()) > box.y + box.h + 1.0f) break;
            ui.textShadow(ft, box.x, y, l.c_str(), canEdit ? kColWhite : kColNoteDis);
            y += static_cast<float>(ft.lineHeight());
        }
        if (f) ui.fill(box, 0x20FFFFFFu);
    }
    // GuildFrameGuildListToggleButton (LEFT 307,-67; 284 with the scroll bar): its text at RIGHT -32, NextPage arrow.
    {
        const float bx = scroll ? 284.0f : 307.0f;
        const Rect btn = R(bx, 256 + 67 - 16, 32, 32);
        const char* label = s.playerStatus ? CLIENT_STR(PLAYER_STATUS) : CLIENT_STR(GUILD_STATUS);
        const float tw = static_cast<float>(ft.measure(label));
        const Rect hit{btn.x - tw - 4, btn.y, btn.w + tw + 4, btn.h};
        const bool f = focusable && winFocus_.addStop(kIdSfGuildToggle, hit);
        ui.textShadow(ft, btn.x - 2, btn.y + (btn.h - static_cast<float>(ft.lineHeight())) * 0.5f, label,
                      f ? kColWhite : kColGold, Align::Right);
        if (s.pageNext.pixels) ui.image(s.pageNext, btn);
        else ui.textShadow(fs, btn.x + btn.w * 0.35f, btn.y + (btn.h - static_cast<float>(fs.lineHeight())) * 0.5f, ">",
                           kColGold);
        if (f && s.pageNext.pixels) ui.image(s.pageNext, btn, 0x60FFFFFFu, Blend::Add);  // UI-Common-MouseHilight
        if (f) ui.fill(hit, 0x20FFFFFFu);
    }
    // GuildFrameGuildInformationButton / GuildFrameAddMemberButton / GuildFrameControlButton (BOTTOMRIGHT -39,82).
    const Rect bInfo = R(384 - 39 - 104 - 2 - 98 - 2 - 123, 512 - 82 - 22, 123, 22);
    const Rect bAdd = R(384 - 39 - 104 - 2 - 98, 512 - 82 - 22, 98, 22);
    const Rect bCtl = R(384 - 39 - 104, 512 - 82 - 22, 104, 22);
    drawPanelButton(ui, kIdSfGuildInfo, bInfo, CLIENT_STR(GUILD_INFORMATION), true, focusable);
    drawPanelButton(ui, kIdSfGuildAddMember, bAdd, CLIENT_STR(ADDMEMBER), session_.guildCan(so::kRightInvite), focusable);
    drawPanelButton(ui, kIdSfGuildControl, bCtl, CLIENT_STR(GUILDCONTROL), session_.isGuildLeader(), focusable);
    if (s.popup == 3 && npcArt_.btnHl.pixels) ui.image(npcArt_.btnHl, bCtl, 0xFFFFFFFFu, Blend::Add);  // LockHighlight
    const uint32_t fid = winFocus_.focused();
    if (focusable && fid == kIdSfGuildInfo) socialTip(ui, fid, bInfo, CLIENT_STR(GUILD_INFORMATION), CLIENT_STR(NEWBIE_TOOLTIP_GUILD_INFORMATION));
    if (focusable && fid == kIdSfGuildAddMember) socialTip(ui, fid, bAdd, CLIENT_STR(ADDMEMBER), CLIENT_STR(NEWBIE_TOOLTIP_ADDMEMBER));
    if (focusable && fid == kIdSfGuildControl) socialTip(ui, fid, bCtl, CLIENT_STR(GUILDCONTROL), CLIENT_STR(NEWBIE_TOOLTIP_GUILDCONTROL));
}

// --- The side frames (GUILDFRAME_POPUPS) at GuildFrame's TOPRIGHT -33,-28 / -37,-6 -----------------------------------

void WorldScreen::drawGuildSideFrame(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    if (!s.popup) return;
    if (s.popup == 3 && !session_.isGuildLeader()) s.popup = 0;  // GuildControlPopupFrame_OnEvent: not the leader
    if (!s.popup) return;
    // The side frame's art (one at a time): detail (officer / normal height), info, control, event log.
    const bool officer = session_.guildCan(so::kRightViewOfficerNote);
    const int kind = soc::sideArtKind(s.popup, officer);  // one rule with updateSocial's free
    if (s.sideKind != kind) {
        s.freeSideArt();
        s.sideKind = kind;
        ++s.sideLoads;
        static const char* const kNames[6] = {"", "sf_detail", "sf_info", "sf_ctrl", "sf_log", "sf_detail_n"};
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            socialLoad(pak, kNames[kind], s.side, s.sideBytes);
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "social_side_art kind=%d ok=%d kb=%u n=%u", kind, s.side.pixels ? 1 : 0,
                 static_cast<unsigned>(s.sideBytes / 1024u), static_cast<unsigned>(s.sideLoads));
    }
    switch (s.popup) {
        case 1: drawGuildMemberDetail(ui, focusable); break;
        case 2: drawGuildInfoFrame(ui, focusable); break;
        case 3: drawGuildControlPopup(ui, focusable); break;
        case 4: drawGuildEventLog(ui, focusable); break;
        default: break;
    }
}

void WorldScreen::drawGuildMemberDetail(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const pp::WorldSession::GuildMember* m = nullptr;
    for (const auto& x : session_.guildRoster())
        if (x.guid == s.selectedMember) m = &x;
    if (!m) return;
    const bool viewOfficer = session_.guildCan(so::kRightViewOfficerNote);
    // TOPLEFT at GuildFrame's TOPRIGHT -33,-28; 212 x 255 / 195 (GUILD_DETAIL_OFFICER_HEIGHT / _NORM_HEIGHT).
    const float fx = 384 - 33, fy = 28, fh = viewOfficer ? 255.0f : 195.0f;
    const Rect frame = R(fx, fy, 212, fh);
    if (s.side.pixels) ui.image(s.side, frame);
    else kit_.panel(ui, frame);
    auto D = [&](float x, float y, float w, float h) { return R(fx + x, fy + y, w, h); };
    // GuildMemberDetailCloseButton (TOPRIGHT -3,-3).
    {
        const Rect cr{D(212 - 3 - 32, 3, 32, 32).x + 4, D(0, 3, 0, 0).y + 1, 14, 14};
        const bool f = focusable && winFocus_.addStop(kIdSfDetClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdSfDetClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    const uint32_t myRank = session_.guildRankIndex();
    const uint32_t maxRank = session_.guildNumRanks() > 0 ? session_.guildNumRanks() - 1 : 0;
    const so::GuildQuery* g = session_.ownGuild();
    const std::string rankName = g && m->rank < so::kGuildRanks ? g->ranks[m->rank] : std::string();
    const float lh = static_cast<float>(ft.lineHeight());
    float y = D(0, 18, 0, 0).y;
    const float x0 = D(17, 0, 0, 0).x;
    ui.textShadow(fs, x0, y, m->name.c_str(), kColGold);  // GuildMemberDetailName (GameFontNormal)
    y += static_cast<float>(fs.lineHeight()) + 2 * kSY;
    std::string cls = socialClassName(m->cls, m->gender == 1);
    if (cls.empty()) cls = CLIENT_STR(UNKNOWN);
    ui.textShadow(ft, x0, y, fmt(CLIENT_STR(FRIENDS_LEVEL_TEMPLATE), {std::to_string(m->level), cls}).c_str(), kColWhite);
    y += lh + 11 * kSY;
    auto labelValue = [&](const char* label, const std::string& value, float maxW) {
        ui.textShadow(ft, x0, y, label, kColGold);
        const float lw = static_cast<float>(ft.measure(label)) + 2 * kSX;
        ui.textShadow(ft, x0 + lw, y, fitText(ft, value, maxW - lw).c_str(), kColWhite);
        y += lh + 8 * kSY;
    };
    labelValue(CLIENT_STR(ZONE_COLON), socialZoneName(m->zone), 180 * kSX);
    // The rank and the promote / demote buttons (LEFT at the frame's RIGHT -60,50): by the rights and the ranks.
    const bool canPromote = session_.guildCan(so::kRightPromote) && m->rank > 1 && m->rank > myRank + 1;
    const bool canDemote = session_.guildCan(so::kRightDemote) && m->rank >= 1 && m->rank > myRank && m->rank != maxRank;
    const Rect bUp = D(212 - 60, fh * 0.5f - 50 - 14, 28, 28), bDn = D(212 - 60 + 20, fh * 0.5f - 50 - 14, 28, 28);
    labelValue(CLIENT_STR(RANK_COLON), rankName, (bUp.x - x0));
    if (canPromote || canDemote) {
        auto arrow = [&](uint32_t id, const Rect& r, int row, bool enabled, const char* tip, const char* title) {
            const bool f = focusable && enabled && winFocus_.addStop(id, r);
            const int col = !enabled ? 2 : (f && winFocus_.isPressed(id)) ? 1 : 0;
            if (s.updn.pixels) ui.image(s.updn, r, SrcRect{16.0f * col, 16.0f * row, 16, 16});
            if (f && s.updn.pixels) ui.image(s.updn, r, SrcRect{48, 16.0f * row, 16, 16}, 0xFFFFFFFFu, Blend::Add);
            if (f) socialTip(ui, id, r, title, tip);
        };
        arrow(kIdSfDetPromote, bUp, 0, canPromote, CLIENT_STR(NEWBIE_TOOLTIP_PROMOTE), "");
        arrow(kIdSfDetDemote, bDn, 1, canDemote, CLIENT_STR(NEWBIE_TOOLTIP_DEMOTE), CLIENT_STR(DEMOTE));
    }
    labelValue(CLIENT_STR(LAST_ONLINE_COLON), m->online() ? std::string(CLIENT_STR(GUILD_ONLINE_LABEL)) : guildLastOnline(m->offlineDays), 190 * kSX);
    // GuildMemberDetailNoteLabel and GuildMemberNoteBackground (181 x 40 under it): CanEditPublicNote - white, the
    // edit label when empty, a click = SET_GUILDPLAYERNOTE; else grey.
    auto noteBox = [&](uint32_t id, const char* label, const std::string& note, bool canEdit, const char* editLabel) {
        ui.textShadow(ft, x0, y, label, kColGold);
        y += lh;
        const Rect box{x0 - 2 * kSX, y, 181 * kSX, 40 * kSY};
        drawTipBox(ui, kit_, box);
        const bool f = focusable && canEdit && winFocus_.addStop(id, box);
        std::string text = note;
        if (canEdit && text.empty()) text = editLabel;
        float ty = box.y + 6 * kSY;
        for (const std::string& l : wrapQuestText(ft, text, 165 * kSX)) {
            if (ty + lh > box.y + box.h) break;
            ui.textShadow(ft, box.x + 8 * kSX, ty, l.c_str(), canEdit ? kColWhite : kColNoteDis);
            ty += lh;
        }
        if (f) ui.fill(box, 0x20FFFFFFu);
        y = box.y + box.h + 3 * kSY;
    };
    noteBox(kIdSfDetNote, CLIENT_STR(NOTE_COLON), m->note, session_.guildCan(so::kRightEditPublicNote), CLIENT_STR(GUILD_NOTE_EDITLABEL));
    if (viewOfficer)
        noteBox(kIdSfDetOfficerNote, CLIENT_STR(OFFICER_NOTE_COLON), m->officerNote, session_.guildCan(so::kRightEditOfficerNote),
                CLIENT_STR(GUILD_OFFICERNOTE_EDITLABEL));
    // GuildMemberRemoveButton (BOTTOMLEFT 10,12, 96 x 22), GuildMemberGroupInviteButton beside it.
    const bool canRemove = session_.guildCan(so::kRightRemove) && m->rank >= 1 && m->rank > myRank;
    const bool canInvite = m->online() && !so::sameName(m->name, name_);
    const Rect bRem = D(10, fh - 12 - 22, 96, 22), bInv = D(10 + 96 + 1, fh - 12 - 22, 96, 22);
    drawPanelButton(ui, kIdSfDetRemove, bRem, CLIENT_STR(REMOVE), canRemove, focusable);
    drawPanelButton(ui, kIdSfDetInvite, bInv, CLIENT_STR(GROUP_INVITE), canInvite, focusable);
    const uint32_t fid = winFocus_.focused();
    if (focusable && fid == kIdSfDetRemove) socialTip(ui, fid, bRem, CLIENT_STR(REMOVE), CLIENT_STR(NEWBIE_TOOLTIP_GUILDREMOVE));
    if (focusable && fid == kIdSfDetInvite) socialTip(ui, fid, bInv, CLIENT_STR(GROUP_INVITE), CLIENT_STR(NEWBIE_TOOLTIP_GUILDGROUPINVITE));
}

void WorldScreen::drawGuildInfoFrame(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float fx = 384 - 37, fy = 6;  // GuildControlPopupFrame's place (GuildFrame TOPRIGHT -37,-6)
    const Rect frame = R(fx, fy, 297, 298);
    if (s.side.pixels) ui.image(s.side, frame);
    else kit_.panel(ui, frame);
    auto D = [&](float x, float y, float w, float h) { return R(fx + x, fy + y, w, h); };
    {
        const Rect cr{D(297 - 3 - 32, 3, 32, 32).x + 4, D(0, 3, 0, 0).y + 1, 14, 14};
        const bool f = focusable && winFocus_.addStop(kIdSfInfoClose, cr);
        if (npcArt_.close.pixels) ui.image(f && winFocus_.isPressed(kIdSfInfoClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    ui.textShadow(fn, D(17, 17, 0, 0).x, D(0, 17, 0, 0).y - 2, CLIENT_STR(GUILD_INFORMATION), kColGold);  // GuildInfoTitle
    // GuildInfoTextBackground (276 x 230 at 11,-32): the text (CanEditGuildInfo: white, the edit label when empty).
    const Rect box = D(11, 32, 276, 230);
    drawTipBox(ui, kit_, box);
    const bool canEdit = session_.guildCan(so::kRightModifyInfo);
    std::string text = s.infoText;
    if (canEdit && text.empty()) text = CLIENT_STR(GUILD_INFO_EDITLABEL);
    const Rect inner{box.x + 5 * kSX, box.y + 7 * kSY, 243 * kSX, 218 * kSY};
    const bool f = focusable && canEdit && winFocus_.addStop(kIdSfInfoEdit, inner);
    const auto lines = wrapQuestText(ft, text, inner.w);
    const float lh = static_cast<float>(ft.lineHeight());
    const int fit = std::max(1, static_cast<int>(inner.h / lh));
    s.infoScroll = std::clamp(s.infoScroll, 0, std::max(0, static_cast<int>(lines.size()) - fit));
    for (int i = 0; i < fit && s.infoScroll + i < static_cast<int>(lines.size()); ++i)
        ui.textShadow(ft, inner.x, inner.y + lh * static_cast<float>(i), lines[static_cast<size_t>(s.infoScroll + i)].c_str(),
                      canEdit ? kColWhite : kColNoteDis);
    if (f) ui.fill(inner, 0x20FFFFFFu);
    if (static_cast<int>(lines.size()) > fit) {  // GuildInfoFrameScrollFrame's arrows
        const Rect up = D(11 + 276 - 22, 32 + 4, 16, 16), dn = D(11 + 276 - 22, 32 + 230 - 20, 16, 16);
        const bool canUp = s.infoScroll > 0, canDown = s.infoScroll + fit < static_cast<int>(lines.size());
        const bool fu = focusable && canUp && winFocus_.addStop(kIdSfInfoUp, up);
        const bool fd = focusable && canDown && winFocus_.addStop(kIdSfInfoDown, dn);
        if (npcArt_.scrUp.pixels) ui.image(canUp ? npcArt_.scrUp : npcArt_.scrUpDis, up);
        if (npcArt_.scrDown.pixels) ui.image(canDown ? npcArt_.scrDown : npcArt_.scrDownDis, dn);
        if (fu) ui.fill(up, 0x40FFFFFFu);
        if (fd) ui.fill(dn, 0x40FFFFFFu);
    }
    // GuildInfoSaveButton ACCEPT (BOTTOMLEFT 108,12, 90 x 22; CanEditGuildInfo), GuildInfoCancelButton CLOSE
    // beside it, GuildInfoGuildEventButton GUILD_BANK_LOG (65 x 22, 32 left of the save).
    drawPanelButton(ui, kIdSfInfoLog, D(108 - 32 - 65, 298 - 12 - 22, 65, 22), CLIENT_STR(GUILD_EVENT_LOG), true, focusable);
    drawPanelButton(ui, kIdSfInfoSave, D(108, 298 - 12 - 22, 90, 22), CLIENT_STR(ACCEPT), canEdit, focusable);
    drawPanelButton(ui, kIdSfInfoCancel, D(108 + 90, 298 - 12 - 22, 90, 22), CLIENT_STR(CLOSE), true, focusable);
}

void WorldScreen::drawGuildEventLog(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float fx = 384 - 37, fy = 6;
    const Rect frame = R(fx, fy, 395, 435);
    if (s.side.pixels) ui.image(s.side, frame);
    else kit_.panel(ui, frame);
    auto D = [&](float x, float y, float w, float h) { return R(fx + x, fy + y, w, h); };
    {
        const Rect cr{D(395 - 3 - 32, 3, 32, 32).x + 4, D(0, 3, 0, 0).y + 1, 14, 14};
        const bool f = focusable && winFocus_.addStop(kIdSfLogClose, cr);
        if (npcArt_.close.pixels) ui.image(f && winFocus_.isPressed(kIdSfLogClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    ui.textShadow(fn, D(17, 17, 0, 0).x, D(0, 17, 0, 0).y - 2, CLIENT_STR(GUILD_EVENT_LOG), kColGold);  // GuildEventLogTitle
    const Rect box = D(11, 32, 375, 370);
    drawTipBox(ui, kit_, box);
    // GuildEventLog_Update: the newest first, GUILDEVENT_TYPE_* + "|cff009999   " GUILD_BANK_LOG_TIME "|r".
    const auto& log = session_.guildEventLog();
    const uint32_t ageMs = pp::logUptimeMs() - session_.guildEventLogAtMs();
    struct Line {
        std::string text, time;
    };
    std::vector<Line> lines;
    const Rect inner{box.x + 6 * kSX, box.y + 6 * kSY, 340 * kSX, 359 * kSY};
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        const so::EventLogEntry& e = *it;
        std::string p1 = session_.socialName(e.guid1), p2 = session_.socialName(e.guid2);
        if (p1.empty()) p1 = CLIENT_STR(UNKNOWN);
        if (p2.empty()) p2 = CLIENT_STR(UNKNOWN);
        const so::GuildQuery* g = session_.ownGuild();
        const std::string rank = g && e.rank < so::kGuildRanks ? g->ranks[e.rank] : std::string();
        std::string msg;
        switch (e.type) {
            case 1: msg = fmt(CLIENT_STR(GUILDEVENT_TYPE_INVITE), {p1, p2}); break;
            case 2: msg = fmt(CLIENT_STR(GUILDEVENT_TYPE_JOIN), {p1}); break;
            case 3: msg = fmt(CLIENT_STR(GUILDEVENT_TYPE_PROMOTE), {p1, p2, rank}); break;
            case 4: msg = fmt(CLIENT_STR(GUILDEVENT_TYPE_DEMOTE), {p1, p2, rank}); break;
            case 5: msg = fmt(CLIENT_STR(GUILDEVENT_TYPE_REMOVE), {p1, p2}); break;
            case 6: msg = fmt(CLIENT_STR(GUILDEVENT_TYPE_QUIT), {p1}); break;
            default: continue;
        }
        // GetGuildEventInfo: years / months / days / hours of the age (8760 / 720 / 24 h) -> RecentTimeDate.
        const uint32_t secs = e.secondsAgo + ageMs / 1000u;
        const uint32_t hours = secs / 3600u;
        const int yy = static_cast<int>(hours / 8760u), mo = static_cast<int>((hours % 8760u) / 720u),
                  dd = static_cast<int>((hours % 720u) / 24u), hh = static_cast<int>(hours % 24u);
        std::string when;
        if (yy) when = fmt(CLIENT_STR(LASTONLINE_YEARS), {std::to_string(yy)});
        else if (mo) when = fmt(CLIENT_STR(LASTONLINE_MONTHS), {std::to_string(mo)});
        else if (dd) when = fmt(CLIENT_STR(LASTONLINE_DAYS), {std::to_string(dd)});
        else if (hh) when = fmt(CLIENT_STR(LASTONLINE_HOURS), {std::to_string(hh)});
        else when = CLIENT_STR(LASTONLINE_MINS);
        const auto wrapped = wrapQuestText(ft, msg + "   " + fmt(CLIENT_STR(GUILD_BANK_LOG_TIME), {when}), inner.w);
        for (size_t k = 0; k < wrapped.size(); ++k) lines.push_back(Line{wrapped[k], std::string()});
    }
    const float lh = static_cast<float>(ft.lineHeight());
    const int fit = std::max(1, static_cast<int>(inner.h / lh));
    s.logScroll = std::clamp(s.logScroll, 0, std::max(0, static_cast<int>(lines.size()) - fit));
    // GUILD_BANK_LOG_TIME's words after its %s (enUS " ago )") mark the time part.
    std::string timeTail = CLIENT_STR(GUILD_BANK_LOG_TIME);
    timeTail = timeTail.find("%s") != std::string::npos ? timeTail.substr(timeTail.find("%s") + 2) : std::string();
    for (int i = 0; i < fit && s.logScroll + i < static_cast<int>(lines.size()); ++i) {
        const std::string& l = lines[static_cast<size_t>(s.logScroll + i)].text;
        const size_t cut = l.rfind('(');  // the GUILD_BANK_LOG_TIME "(... ago)" part in |cff009999
        if (cut != std::string::npos && !timeTail.empty() && l.find(timeTail) != std::string::npos) {
            ui.textShadow(ft, inner.x, inner.y + lh * static_cast<float>(i), l.substr(0, cut).c_str(), kColWhite);
            ui.textShadow(ft, inner.x + static_cast<float>(ft.measure(l.substr(0, cut).c_str())),
                          inner.y + lh * static_cast<float>(i), l.substr(cut).c_str(), kColCyan);
        } else {
            ui.textShadow(ft, inner.x, inner.y + lh * static_cast<float>(i), l.c_str(), kColWhite);
        }
    }
    if (static_cast<int>(lines.size()) > fit) {
        const Rect up = D(11 + 375 - 24, 32 + 4, 16, 16), dn = D(11 + 375 - 24, 32 + 370 - 20, 16, 16);
        const bool canUp = s.logScroll > 0, canDown = s.logScroll + fit < static_cast<int>(lines.size());
        const bool fu = focusable && canUp && winFocus_.addStop(kIdSfLogUp, up);
        const bool fd = focusable && canDown && winFocus_.addStop(kIdSfLogDown, dn);
        if (npcArt_.scrUp.pixels) ui.image(canUp ? npcArt_.scrUp : npcArt_.scrUpDis, up);
        if (npcArt_.scrDown.pixels) ui.image(canDown ? npcArt_.scrDown : npcArt_.scrDownDis, dn);
        if (fu) ui.fill(up, 0x40FFFFFFu);
        if (fd) ui.fill(dn, 0x40FFFFFFu);
    }
    // GuildEventLogCancelButton CLOSE (BOTTOMRIGHT -9,13, 139 x 22).
    drawPanelButton(ui, kIdSfLogCancel, D(395 - 9 - 139, 435 - 13 - 22, 139, 22), CLIENT_STR(CLOSE), true, focusable);
}

void WorldScreen::drawGuildControlPopup(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    auto& c = s.ctl;
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const float fx = 384 - 37, fy = 6;
    const Rect frame = R(fx, fy, 320, 457);
    if (s.side.pixels) ui.image(s.side, frame);
    else kit_.panel(ui, frame);
    auto D = [&](float x, float y, float w, float h) { return R(fx + x, fy + y, w, h); };
    const float lh = static_cast<float>(ft.lineHeight());
    const int numRanks = static_cast<int>(session_.guildNumRanks());
    const bool leaderRank = session_.isGuildLeader() && c.rank == 1;  // the guild master's own rank: all grey
    ui.textShadow(fs, D(160, 15, 0, 0).x, D(0, 15, 0, 0).y, CLIENT_STR(GUILDCONTROL_SELECTRANK), kColGold, Align::Center);
    // GuildControlPopupFrameDropDown (TOP 0,-30 of the check boxes frame at TOPRIGHT -22,1; width 160).
    {
        const Rect box = D(43 + 10, 29 - 4, 160 + 29, 38);
        if (s.ddBox.pixels) {
            const float tw = s.ddBox.uw, th = s.ddBox.uh, sc = box.h / th;
            ui.image(s.ddBox, Rect{box.x, box.y, 8 * sc, box.h}, SrcRect{0, 0, 8, th});
            ui.image(s.ddBox, Rect{box.x + 8 * sc, box.y, box.w - 15 * sc, box.h}, SrcRect{8, 0, tw - 15, th});
            ui.image(s.ddBox, Rect{box.x + box.w - 7 * sc, box.y, 7 * sc, box.h}, SrcRect{tw - 7, 0, 7, th});
        }
        const bool f = focusable && winFocus_.addStop(kIdSfCtlRank, box);
        const so::GuildQuery* g = session_.ownGuild();
        const std::string cur = g && c.rank >= 1 && c.rank <= 10 ? g->ranks[c.rank - 1] : std::string();
        ui.textShadow(ft, box.x + 8, box.y + (box.h - lh) * 0.5f, fitText(ft, cur, box.w - 26).c_str(), kColWhite);
        const Rect btn{box.x + box.w - 15, box.y + (box.h - 12) * 0.5f, 12, 12};
        const pp::Texture& bt = s.dropKind == 3 ? s.ddBtnDn : s.ddBtn;
        if (bt.pixels) ui.image(bt, btn);
        if (f && s.ddHl.pixels) ui.image(s.ddHl, btn, 0xFFFFFFFFu, Blend::Add);
        if (s.dropKind == 3) s.dropAnchor = Rect{box.x + ui.offsetX(), box.y + box.h + ui.offsetY(), box.w, 0};
        // «+» (ADD_GUILDRANK, below 10 ranks) and «-» (the lowest rank, above 5 ranks, nobody in it).
        const Rect add = D(238, 36, 16, 16), rem = D(252, 36, 16, 16);
        const bool canAdd = numRanks < 10;
        const bool fa = focusable && canAdd && winFocus_.addStop(kIdSfCtlAddRank, add);
        if (s.plus.pixels) ui.image(s.plus, add, canAdd ? 0xFFFFFFFFu : 0xFF808080u);
        if (fa) socialTip(ui, kIdSfCtlAddRank, add, CLIENT_STR(GUILDADDRANK_BUTTON_TOOLTIP), std::string());
        if (c.rank == numRanks && numRanks > 5) {
            int inBottom = 0;
            for (const auto& m : session_.guildRoster()) inBottom += static_cast<int>(m.rank) == numRanks - 1 ? 1 : 0;
            const bool canRem = inBottom == 0;
            const bool fr = focusable && canRem && winFocus_.addStop(kIdSfCtlRemoveRank, rem);
            if (s.minus.pixels) ui.image(s.minus, rem, canRem ? 0xFFFFFFFFu : 0xFF808080u);
            if (fr) socialTip(ui, kIdSfCtlRemoveRank, rem, CLIENT_STR(GUILDREMOVERANK_BUTTON_TOOLTIP), std::string());
        }
    }
    // GuildControlPopupFrameEditBox (130 x 32 at TOP 35,-68) with GUILDCONTROL_RANKLABEL before it.
    auto inputBox = [&](uint32_t id, const Rect& r, const std::string& text, bool enabled) {
        if (s.input.pixels) {
            const float tw = static_cast<float>(s.input.uw), th = static_cast<float>(s.input.uh), ct = tw * 8.0f / 128.0f;
            ui.image(s.input, Rect{r.x, r.y, 4, r.h}, SrcRect{0, 0, ct, th});
            ui.image(s.input, Rect{r.x + 4, r.y, r.w - 8, r.h}, SrcRect{ct, 0, tw - 2 * ct, th});
            ui.image(s.input, Rect{r.x + r.w - 4, r.y, 4, r.h}, SrcRect{tw - ct, 0, ct, th});
        } else {
            ui.fill(r, 0x80000000u);
        }
        const bool f = focusable && enabled && winFocus_.addStop(id, r);
        ui.textShadow(ft, r.x + 3, r.y + (r.h - lh) * 0.5f, fitText(ft, text, r.w - 6).c_str(),
                      enabled ? kColWhite : kColDisable);
        if (f) ui.fill(r, 0x20FFFFFFu);
    };
    {
        const Rect eb = D(130, 74, 130, 20);
        ui.textShadow(ft, eb.x - 12 * kSX, eb.y + (eb.h - lh) * 0.5f, CLIENT_STR(GUILDCONTROL_RANKLABEL), kColGold, Align::Right);
        inputBox(kIdSfCtlName, eb, c.name, true);
    }
    ui.textShadow(ft, D(150, 106, 0, 0).x, D(0, 106, 0, 0).y, CLIENT_STR(GUILDCONTROL_ALLOWRANK), kColWhite, Align::Center);
    // GuildControlPopupFrameCheckbox1..17 (no 14): two columns from 19,122 / 156,121, 19 units apart; 15 / 16 beside
    // GUILDCONTROL_WITHDRAW_GOLD.
    auto checkBox = [&](int i, const Rect& r, bool enabled) {
        const uint32_t id = kIdSfCtlCheck0 + static_cast<uint32_t>(i);
        const bool f = focusable && enabled && winFocus_.addStop(id, r);
        drawCheck(ui, s.cbUp, s.check, s.cbHl, r, c.flags[i], enabled, f);
        ui.textShadow(ft, r.x + r.w + 1 * kSX, r.y + (r.h - lh) * 0.5f,
                      fitText(ft, pp::cs(kOption[i]), 115 * kSX).c_str(),
                      enabled ? kColGold : kColDisable);
        if (f && (i == 14 || i == 15))
            socialTip(ui, id, r, pp::cs(kOption[i]),
                      i == 14 ? CLIENT_STR(GUILDCONTROL_OPTION15_TOOLTIP) : CLIENT_STR(GUILDCONTROL_OPTION16_TOOLTIP));
    };
    const int left[7] = {0, 2, 4, 6, 8, 10, 12}, right[6] = {1, 3, 5, 7, 9, 11};
    for (int k = 0; k < 7; ++k) checkBox(left[k], D(19, 122 + 19.0f * k, 20, 20), !leaderRank);
    for (int k = 0; k < 6; ++k) checkBox(right[k], D(156, 121 + 19.0f * k, 20, 20), !leaderRank);
    checkBox(16, D(156, 121 + 19.0f * 6, 20, 20), !leaderRank);
    // GuildControlWithdrawGold (TOPLEFT at Checkbox13.BOTTOMLEFT 0,-16): GUILDCONTROL_WITHDRAW_GOLD, 16 GUILDCONTROL_OPTION16 / 15 GUILDCONTROL_OPTION15 beside
    // it, GOLD_PER_DAY and its edit box (70 x 32).
    {
        const float gy = 236 + 20 + 16;
        ui.textShadow(ft, D(19, 0, 0, 0).x, D(0, gy, 0, 0).y, CLIENT_STR(GUILDCONTROL_WITHDRAW_GOLD), leaderRank ? kColDisable : kColGold);
        const float tw = static_cast<float>(ft.measure(CLIENT_STR(GUILDCONTROL_WITHDRAW_GOLD))) / kSX;
        checkBox(15, D(19 + tw + 4, gy - 4, 20, 20), !leaderRank);
        checkBox(14, D(19 + tw + 4 + 20 + 50, gy - 4, 20, 20), !leaderRank);
        const bool goldOn = !leaderRank && (c.flags[14] || c.flags[15]);  // WithdrawGoldEditBox_Update
        const float ay = gy + 13 + 11;
        ui.textShadow(ft, D(19, 0, 0, 0).x, D(0, ay, 0, 0).y, CLIENT_STR(GOLD_PER_DAY), goldOn ? kColGold : kColDisable);
        const float aw = static_cast<float>(ft.measure(CLIENT_STR(GOLD_PER_DAY))) / kSX;
        inputBox(kIdSfCtlGold, D(19 + aw + 11, ay - 5, 70, 20), leaderRank ? CLIENT_STR(UNLIMITED) : std::to_string(c.gold), goldOn);
    }
    // GuildControlPopupFrameTabPermissions (268 x 62 at BOTTOMLEFT 13,58): GUILDBANK_TAB_COLON, the tabs 1..6 the
    // guild bought (UI-GuildFrame-PermissionTab), view / deposit / update text, items per day.
    {
        const float by = 457 - 58 - 62;
        const int tabs = session_.guildBankTabs();
        const bool tabsOn = !leaderRank && tabs > 0;
        drawTipBox(ui, kit_, D(13, by, 268, 62));
        ui.textShadow(ft, D(22, 0, 0, 0).x, D(0, by - 13, 0, 0).y, CLIENT_STR(GUILDBANK_TAB_COLON), tabsOn ? kColGold : kColDisable);
        for (int t = 1; t <= 6 && t <= tabs; ++t) {
            const Rect tr = D(251 - (6 - t) * 22.0f, by - 27 + (t == c.bankTab ? 0 : 4), 26, t == c.bankTab ? 32 : 28);
            const uint32_t id = kIdSfCtlTab0 + static_cast<uint32_t>(t - 1);
            const bool f = focusable && !leaderRank && winFocus_.addStop(id, tr);
            if (s.permtab.pixels) ui.image(s.permtab, tr);
            ui.textShadow(ft, tr.x + tr.w * 0.5f, tr.y + tr.h * 0.5f - lh * 0.5f, std::to_string(t).c_str(),
                          t == c.bankTab ? kColWhite : kColGold, Align::Center);
            if (f) ui.fill(tr, 0x30FFFFFFu);
        }
        const int ti = std::clamp(c.bankTab, 1, 6) - 1;
        const auto& rk = session_.guildRank(static_cast<uint32_t>(c.rank - 1));
        auto flagOf = [&](int k) {  // GetGuildBankTabPermissions + the pending changes; the master: all
            if (c.rank == 1) return true;
            if (c.pending[ti].flag[k] >= 0) return c.pending[ti].flag[k] != 0;
            return (rk.tabFlags[ti] & (1u << k)) != 0;
        };
        const bool view = flagOf(0);
        auto tabCheck = [&](uint32_t id, const Rect& r, bool checked, bool enabled, const char* label) {
            const bool f = focusable && enabled && winFocus_.addStop(id, r);
            drawCheck(ui, s.cbUp, s.check, s.cbHl, r, checked, enabled, f);
            ui.textShadow(ft, r.x + r.w + 1 * kSX, r.y + (r.h - lh) * 0.5f, label, enabled ? kColGold : kColDisable);
        };
        const Rect rView = D(23, by + 6, 20, 20);
        tabCheck(kIdSfCtlView, rView, view, tabsOn, CLIENT_STR(GUILDCONTROL_VIEW_TAB));
        const float vw = static_cast<float>(ft.measure(CLIENT_STR(GUILDCONTROL_VIEW_TAB))) / kSX;
        tabCheck(kIdSfCtlDeposit, D(23 + 20 + vw + 45, by + 5, 20, 20), flagOf(1), tabsOn && view, CLIENT_STR(GUILDCONTROL_DEPOSIT_ITEMS));
        tabCheck(kIdSfCtlUpdateText, D(23, by + 22, 20, 20), flagOf(2), tabsOn && view, CLIENT_STR(GUILDCONTROL_UPDATE_TEXT));
        const float wy = by + 6 + 20 + 16;
        ui.textShadow(ft, D(23, 0, 0, 0).x, D(0, wy, 0, 0).y, CLIENT_STR(GUILDCONTROL_WITHDRAW_ITEMS), tabsOn ? kColGold : kColDisable);
        const float ww = static_cast<float>(ft.measure(CLIENT_STR(GUILDCONTROL_WITHDRAW_ITEMS))) / kSX;
        const int withdraw = c.pending[ti].withdraw >= 0 ? c.pending[ti].withdraw : static_cast<int>(rk.tabSlots[ti]);
        inputBox(kIdSfCtlWithdraw, D(23 + ww + 20, wy - 5, 70, 20), tabsOn ? std::to_string(withdraw) : std::string(CLIENT_STR(UNLIMITED)),
                 tabsOn);
    }
    // GuildControlPopupAcceptButton / GuildControlPopupFrameCancelButton (BOTTOMRIGHT -33,34, 78 x 22).
    drawPanelButton(ui, kIdSfCtlAccept, D(320 - 33 - 78 - 2 - 78, 457 - 34 - 22, 78, 22), CLIENT_STR(ACCEPT), c.acceptEnabled, focusable);
    drawPanelButton(ui, kIdSfCtlCancel, D(320 - 33 - 78, 457 - 34 - 22, 78, 22), CLIENT_STR(CANCEL), true, focusable);
}

// --- Clicks ------------------------------------------------------------------------------------------------------------

bool WorldScreen::guildAction(uint32_t act, bool alt) {
    SocialUi& s = socialUi();
    auto& c = s.ctl;
    const auto& roster = session_.guildRoster();
    if (act >= kIdSfGuild0 && act < kIdSfGuild0 + 13) {  // FriendsFrameGuildStatusButton_OnClick
        const int idx = s.guildOffset + static_cast<int>(act - kIdSfGuild0) + 1;
        if (idx > guildRows(session_)) return true;
        const auto& m = roster[static_cast<size_t>(idx - 1)];
        if (alt) {  // RightButton: FriendsFrame_ShowDropdown(name, online) - nothing for an offline member
            if (m.online()) {
                const uint8_t menu = static_cast<uint8_t>(kUnitMenuFriend | 4);  // GuildFrame shown
                openNamedUnitPopup(m.guid, m.name, menu, winFocus_.cursorX(), winFocus_.cursorY(), act);
            }
            return true;
        }
        const bool same = s.selectedMember == m.guid;
        if (s.popup == 1 && same) {  // the shown member again: the detail hides, no selection
            s.popup = 0;
            s.selectedMember = 0;
        } else {
            s.selectedMember = m.guid;
            s.selectedName = m.name;
            guildPopupShow(1);
        }
        return true;
    }
    if (alt) return true;
    if (act >= kIdSfGuildCol0 && act < kIdSfGuildCol0 + 8) {  // the column header: SortGuildRoster(sortType)
        static const int kSort[8] = {2, 3, 1, 4, 2, 0, 7, 6};  // name zone level class / name rank note online
        session_.sortGuildRoster(kSort[act - kIdSfGuildCol0]);
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    switch (act) {
        case kIdSfGuildOffline:  // GuildFrameLFGButton: SetGuildRosterSelection(0); SetGuildRosterShowOffline
            s.selectedMember = 0;
            if (s.popup == 1) s.popup = 0;
            playSoundByName(session_.guildShowOffline() ? "igMainMenuOptionCheckBoxOff" : "igMainMenuOptionCheckBoxOn");
            session_.setGuildShowOffline(!session_.guildShowOffline());
            return true;
        case kIdSfGuildToggle:  // GuildFrameGuildListToggleButton_OnClick
            s.playerStatus = !s.playerStatus;
            playSoundByName("igMainMenuOptionCheckBoxOn");
            return true;
        case kIdSfGuildUp: s.guildOffset = std::max(0, s.guildOffset - 1); return true;
        case kIdSfGuildDown: ++s.guildOffset; return true;
        case kIdSfGuildMotd: socialEditPopup(kOskMotd, std::string()); return true;  // SET_GUILDMOTD
        case kIdSfGuildAddMember: socialEditPopup(kOskAddGuildMember, std::string()); return true;  // ADD_GUILDMEMBER
        case kIdSfGuildInfo: guildPopupShow(s.popup == 2 ? 0 : 2); return true;  // ToggleGuildInfoFrame
        case kIdSfGuildControl: guildPopupShow(s.popup == 3 ? 0 : 3); return true;
        // GuildMemberDetailFrame
        case kIdSfDetClose:
            s.popup = 0;
            s.selectedMember = 0;
            return true;
        case kIdSfDetRemove: socialConfirm(4, s.selectedName); return true;  // REMOVE_GUILDMEMBER
        case kIdSfDetInvite: session_.inviteToGroup(s.selectedName); return true;  // InviteUnit(GuildFrame.selectedName)
        case kIdSfDetPromote:
            session_.guildPromote(s.selectedName);
            playSoundByName("UChatScrollButton");
            return true;
        case kIdSfDetDemote:
            session_.guildDemote(s.selectedName);
            playSoundByName("UChatScrollButton");
            return true;
        case kIdSfDetNote: socialEditPopup(kOskGuildNote, s.selectedName); return true;          // SET_GUILDPLAYERNOTE
        case kIdSfDetOfficerNote: socialEditPopup(kOskOfficerNote, s.selectedName); return true;   // SET_GUILDOFFICERNOTE
        // GuildInfoFrame
        case kIdSfInfoClose:
        case kIdSfInfoCancel: s.popup = 0; return true;
        case kIdSfInfoEdit: socialEditPopup(kOskGuildInfo, std::string()); return true;
        case kIdSfInfoSave:  // SetGuildInfoText; GuildRoster(); Hide
            session_.guildSetInfoText(s.infoText);
            session_.guildRosterRequest();
            s.popup = 0;
            return true;
        case kIdSfInfoLog: guildPopupShow(s.popup == 4 ? 0 : 4); return true;  // ToggleGuildEventLog
        case kIdSfInfoUp: s.infoScroll = std::max(0, s.infoScroll - 1); return true;
        case kIdSfInfoDown: ++s.infoScroll; return true;
        // GuildEventLogFrame
        case kIdSfLogClose:
        case kIdSfLogCancel: s.popup = 0; return true;
        case kIdSfLogUp: s.logScroll = std::max(0, s.logScroll - 1); return true;
        case kIdSfLogDown: ++s.logScroll; return true;
        // GuildControlPopupFrame
        case kIdSfCtlRank:  // the rank dropdown
            if (s.dropKind == 3) {
                socialCloseDropDown();
            } else {
                s.dropKind = 3;
                s.dropFocusMs = pp::logUptimeMs();
                openWindow(kWinSocialDropDown);
                placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), kWinSocialDropDown), placeNext_.end());
                winFocus_.setFocus(kIdSfDrop0);
            }
            return true;
        case kIdSfCtlAddRank: socialEditPopup(kOskAddRank, std::string()); return true;  // ADD_GUILDRANK
        case kIdSfCtlRemoveRank:  // GuildControlPopupFrameRemoveRankButton_OnClick: GuildControlDelRank, rank 1
            session_.guildDelRank();
            guildControlLoadRank(1);
            return true;
        case kIdSfCtlName: socialEditPopup(kOskRankName, std::string()); return true;
        case kIdSfCtlGold: socialEditPopup(kOskRankGold, std::string()); return true;
        case kIdSfCtlWithdraw: socialEditPopup(kOskRankWithdraw, std::string()); return true;
        case kIdSfCtlView:
        case kIdSfCtlDeposit:
        case kIdSfCtlUpdateText: {  // SetPendingGuildBankTabPermissions(tab, id, checked)
            const int ti = std::clamp(c.bankTab, 1, 6) - 1;
            const int k = static_cast<int>(act - kIdSfCtlView);
            const auto& rk = session_.guildRank(static_cast<uint32_t>(c.rank - 1));
            const bool cur = c.pending[ti].flag[k] >= 0 ? c.pending[ti].flag[k] != 0 : (rk.tabFlags[ti] & (1u << k)) != 0;
            c.pending[ti].flag[k] = cur ? 0 : 1;
            c.acceptEnabled = true;
            return true;
        }
        case kIdSfCtlAccept: {  // GuildControlPopupAcceptButton_OnClick
            const uint32_t rank = static_cast<uint32_t>(c.rank - 1);
            const auto& rk = session_.guildRank(rank);
            uint32_t rights = rk.rights;
            for (int i = 0; i < 17; ++i) {
                if (i == 13) continue;  // checkbox 14: a deprecated flag (kept as it is)
                if (c.flags[i]) rights |= so::kRankFlagBit[i];
                else rights &= ~so::kRankFlagBit[i];
            }
            uint32_t tabFlags[6], tabSlots[6];
            for (int t = 0; t < 6; ++t) {
                tabFlags[t] = rk.tabFlags[t];
                tabSlots[t] = rk.tabSlots[t];
                for (int k = 0; k < 3; ++k)
                    if (c.pending[t].flag[k] >= 0) {
                        if (c.pending[t].flag[k]) tabFlags[t] |= 1u << k;
                        else tabFlags[t] &= ~(1u << k);
                    }
                if (c.pending[t].withdraw >= 0) tabSlots[t] = static_cast<uint32_t>(c.pending[t].withdraw);
            }
            const uint32_t gold = (c.flags[14] || c.flags[15]) ? c.gold : 0u;  // SetGuildBankWithdrawLimit(amount or 0)
            session_.guildSaveRank(rank, rights, c.name, gold, tabFlags, tabSlots);
            c.acceptEnabled = false;
            for (auto& p : c.pending) p = SocialUi::Ctl::Pending{};
            s.popup = 0;  // GuildControlPopupFrame:Hide()
            return true;
        }
        case kIdSfCtlCancel: s.popup = 0; return true;
        default: break;
    }
    if (act >= kIdSfCtlCheck0 && act < kIdSfCtlCheck0 + 17) {  // GuildControlCheckbox OnClick: the flag, accept on
        const int i = static_cast<int>(act - kIdSfCtlCheck0);
        c.flags[i] = !c.flags[i];
        c.acceptEnabled = true;
        playSoundByName(c.flags[i] ? "igMainMenuOptionCheckBoxOn" : "igMainMenuOptionCheckBoxOff");
        return true;
    }
    if (act >= kIdSfCtlTab0 && act < kIdSfCtlTab0 + 6) {  // GuildBankTabPermissionsTab_OnClick
        c.bankTab = static_cast<int>(act - kIdSfCtlTab0) + 1;
        return true;
    }
    return true;  // our range: nothing else takes these ids
}

}  // namespace wowee::ui::psp
