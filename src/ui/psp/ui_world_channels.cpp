// ChannelFrame (FrameXML ChannelFrame.xml / .lua) - FriendsFrame's CHANNELS tab - over the session's joined
// channels (psp_world_session_channels.cpp, GetChannelDisplayInfo):
//   - ChannelListScrollFrame: the headers CHANNEL_CATEGORY_WORLD / _CUSTOM ("+" / "-", CollapseChannelHeader) and
//     their channels "N. Name". The port's own order: each category where its lowest-numbered channel stands, its
//     channels by number under it;
//     ChannelList_OnClick: Cross = select (the roster: SetSelectedDisplayChannel -> CMSG_CHANNEL_DISPLAY_LIST),
//     Circle = ChannelListDropDown (CHAT_PASSWORD / PARTY_INVITE for the owner / moderator of a custom channel,
//     CHAT_LEAVE, CANCEL);
//   - ChannelRoster: the channel's name, MAX_CHANNEL_MEMBER_BUTTONS 22 names with UI-Group-LeaderIcon /
//     -AssistantIcon (moderator); Circle (and Cross, PSP) = UnitPopup "CHAT_ROSTER" (WHISPER, TARGET, CHAT_PROMOTE,
//     CHAT_DEMOTE, CHAT_OWNER, CANCEL - the voice items stay hidden: voice chat is off);
//   - ChannelFrameNewButton ADD / ChannelFrameJoinButton JOIN (a selected left world channel joins
//     at once, else ChannelFrameDaughterFrame: CHANNEL_NEW_CHANNEL / CHANNEL_JOIN_CHANNEL, the name and the password);
//   - StaticPopups CHAT_CHANNEL_INVITE (CHANNEL_NOTIFY INVITE), CHAT_CHANNEL_PASSWORD (WRONG_PASSWORD), CHANNEL_PASSWORD
//     and CHANNEL_INVITE (the list's dropdown);
//   - WorldChat::channelCommand: SLASH_JOIN / LEAVE / LIST_CHANNEL / CHAT_PASSWORD / OWNER / MODERATOR / UNMODERATOR /
//     MUTE / UNMUTE / CINVITE / KICK / BAN / UNBAN / ANNOUNCE / MODERATE (ChatFrame.lua) and "/N text" (CHANNEL N).
// PSP: no voice chat (the AutoJoin row, the speakers, the GROUP category's party / raid voice channels are not shown);
// the lists scroll with arrows (FauxScrollFrame without the bar); the names and passwords on the PSP keyboard.
// Memory: ch_* small art (~6 KB) while the tab shows; the frame itself is ui_world_social.cpp's sf_channel.
#include "ui/psp/ui_world_glyphs_guildbank.hpp"
#include "ui/psp/ui_world_social.hpp"

#include "ui/psp/ui_world_chat.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using game::LogicalOpcode;

namespace {

constexpr int kOskOwner = 0x4348;  // "CH"
constexpr int kListRows = 16;      // MAX_DISPLAY_CHANNEL_BUTTONS
constexpr int kRosterRows = 22;    // MAX_CHANNEL_MEMBER_BUTTONS
constexpr uint32_t kColGrayFont = 0xFF808080u;  // GRAY_FONT_COLOR_CODE
constexpr pp::CS kCategory[3] = {pp::CS::CHANNEL_CATEGORY_GROUP, pp::CS::CHANNEL_CATEGORY_WORLD,
                                 pp::CS::CHANNEL_CATEGORY_CUSTOM};

bool digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string firstWord(const std::string& s, std::string* rest = nullptr) {
    size_t a = 0;
    while (a < s.size() && s[a] == ' ') ++a;
    size_t b = a;
    while (b < s.size() && s[b] != ' ') ++b;
    if (rest) {
        size_t c = b;
        while (c < s.size() && s[c] == ' ') ++c;
        *rest = s.substr(c);
    }
    return s.substr(a, b - a);
}

// text.pak's record into a malloc'd copy (pakFree), or nullptr.
const uint8_t* loadText(const char* name, uint32_t& size) {
    char path[256];
    pp::fsPath(path, sizeof(path), "data/text.pak");
    pp::Pak pak;
    if (!pak.open(path)) return nullptr;
    const uint8_t* b = nullptr;
    if (const pp::PakEntry* e = pak.find(name)) {
        b = static_cast<const uint8_t*>(pak.load(e));
        size = e->size;
    }
    pak.close();
    return b;
}

}  // namespace

void WorldScreen::GgbUi::freeChannelArt() {
    for (pp::Texture* t : {&chFrame, &chRowBg, &chHl, &chOwner, &chMod, &chTitle})
        if (t->pixels) pp::textureRelease(*t);
    if (chArtTried) PSP_LOGI(pp::tag::Gfx, "channel_art_free kb=%u", static_cast<unsigned>(chArtBytes / 1024u));
    chArtTried = false;
    chArtBytes = 0;
}

void WorldScreen::loadChannelArt() {
    GgbUi& g = ggbUi();
    if (g.chArtTried) return;
    g.chArtTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {
        ok += ggbLoad(pak, "ch_rowbg", g.chRowBg, g.chArtBytes);
        ok += ggbLoad(pak, "ch_hl", g.chHl, g.chArtBytes);
        ok += ggbLoad(pak, "ch_owner", g.chOwner, g.chArtBytes);
        ok += ggbLoad(pak, "ch_mod", g.chMod, g.chArtBytes);
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "channel_art ok=%d/4 kb=%u", ok, static_cast<unsigned>(g.chArtBytes / 1024u));
}

// ChatChannels.dbc (text.pak "chat_chan") once, then the zone's name whenever the zone changes (the zone channels'
// names are formatted with it; a CITY_ONLY channel in a capital - the baked AreaTable AREA_FLAG_CAPITAL list - takes
// the city word: AzerothCore Player::UpdateLocalChannels).
void WorldScreen::channelZoneTick() {
    GgbUi& g = ggbUi();
    if (!g.chanDataTried) {
        g.chanDataTried = true;
        uint32_t size = 0;
        if (const uint8_t* b = loadText("chat_chan", size)) {
            const uint8_t *p = b, *end = b + size;
            std::vector<gbk::ChatChannelRow> rows;
            std::string city;
            auto u32 = [&](uint32_t& v) {
                if (end - p < 4) return false;
                std::memcpy(&v, p, 4);
                p += 4;
                return true;
            };
            auto str = [&](std::string& s) {
                const uint8_t* z = static_cast<const uint8_t*>(std::memchr(p, 0, static_cast<size_t>(end - p)));
                if (!z) return false;
                s.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(z - p));
                p = z + 1;
                return true;
            };
            uint32_t n = 0;
            bool ok = u32(n) && n <= 64;
            for (uint32_t i = 0; ok && i < n; ++i) {
                gbk::ChatChannelRow r;
                ok = u32(r.id) && u32(r.flags) && u32(r.faction) && str(r.name) && str(r.shortcut);
                if (ok) rows.push_back(std::move(r));
            }
            uint32_t caps = 0;
            if (ok) ok = str(city) && u32(caps) && caps <= 256;
            for (uint32_t i = 0; ok && i < caps; ++i) {
                uint32_t a = 0;
                ok = u32(a);
                if (ok) g.capitals.push_back(a);
            }
            pp::pakFree(const_cast<uint8_t*>(b));
            PSP_LOGI(pp::tag::Net, "chat_chan rows=%u caps=%u ok=%d", static_cast<unsigned>(rows.size()),
                     static_cast<unsigned>(g.capitals.size()), ok ? 1 : 0);
            if (!rows.empty()) session_.setChatChannelData(std::move(rows), city);
        }
    }
    const uint32_t zone = session_.worldStateZoneId();
    if (zone == 0 || zone == g.zoneSeen) return;
    g.zoneSeen = zone;
    auto it = g.zoneNames.find(zone);
    if (it == g.zoneNames.end()) {
        std::string name;
        uint32_t size = 0;
        if (const uint8_t* b = loadText("zones", size)) {  // u32 count, then (i32 key, name\0)...
            const char *p = reinterpret_cast<const char*>(b) + 4, *end = reinterpret_cast<const char*>(b) + size;
            while (p + 4 < end) {
                int32_t k = 0;
                std::memcpy(&k, p, 4);
                p += 4;
                const char* s = p;
                while (p < end && *p) ++p;
                if (k == static_cast<int32_t>(zone)) {
                    name.assign(s, p);
                    break;
                }
                ++p;
            }
            pp::pakFree(const_cast<uint8_t*>(b));
        }
        if (g.zoneNames.size() > 64) g.zoneNames.clear();
        it = g.zoneNames.emplace(zone, name).first;
    }
    const bool city = std::binary_search(g.capitals.begin(), g.capitals.end(), zone);
    session_.setChannelZone(zone, it->second, city);
    PSP_LOGI(pp::tag::Net, "channel_zone %u city=%d", static_cast<unsigned>(zone), city ? 1 : 0);
}

// Every frame (updateGlyphsGuildBank): the zone, the session's world-entry joins, the popups, the art.
void WorldScreen::channelsUpdate() {
    GgbUi& g = ggbUi();
    channelZoneTick();
    session_.channelUpdate();
    channelOskTick();
    // CHAT_CHANNEL_INVITE / CHAT_CHANNEL_PASSWORD (one dialog at a time; the next one waits in the session).
    if (!g.chPopup) {
        std::string ch, inv;
        if (session_.takeChannelInvite(ch, inv)) {
            g.chPopup = 1;
            g.chPopupChannel = ch;
            g.chPopupInviter = inv;
        } else if (session_.takeChannelPasswordRequest(ch)) {
            g.chPopup = 2;
            g.chPopupChannel = ch;
            g.chPopupEdit.clear();
        }
        if (g.chPopup) {
            g.chPopupAtMs = pp::logUptimeMs();
            openWindow(kWinChannelPopup);
            PSP_LOGI(pp::tag::Ctrl, "channel_popup %d", g.chPopup);
        }
    } else if (!windowOpen(kWinChannelPopup)) {
        if (g.chPopup == 1) session_.declineChannelInvite(g.chPopupChannel);  // the dialog hidden: OnCancel
        g.chPopup = 0;
    } else if (g.chPopup == 1 && pp::logUptimeMs() - g.chPopupAtMs > 60000u) {  // timeout = 60
        session_.declineChannelInvite(g.chPopupChannel);
        closeWindow(kWinChannelPopup);
        g.chPopup = 0;
    }
    // The tab's own windows go with it.
    const bool shown = windowOpen(kWinFriends) && soc_ && soc_->tab == 4;
    if (!shown) {
        if (windowOpen(kWinChannelDropDown)) closeWindow(kWinChannelDropDown);
        if (windowOpen(kWinChannelDaughter)) closeWindow(kWinChannelDaughter);
        g.dropKind = 0;
        g.daughter = 0;
        if (g.chArtTried) g.freeChannelArt();
        return;
    }
    if (g.dropKind && !windowOpen(kWinChannelDropDown)) g.dropKind = 0;
    if (g.daughter && !windowOpen(kWinChannelDaughter)) {
        g.daughter = 0;  // ChannelFrameDaughterFrame_OnHide: the boxes cleared
        g.dName.clear();
        g.dPassword.clear();
    }
    // GetChannelDisplayInfo: the category headers in the order of their lowest-numbered channel, each with its
    // channels by number (the session's list is in number order).
    g.rows.clear();
    int order[2] = {0, 0};
    int nCats = 0;
    for (const auto& c : session_.chatChannels()) {
        const int cat = c.channelId != 0 ? 1 : 2;
        if (nCats < 2 && order[0] != cat) order[nCats++] = cat;
        if (nCats == 2) break;
    }
    for (int k = 0; k < nCats; ++k) {
        const int cat = order[k];
        std::vector<const pp::WorldSession::ChatChannel*> list;
        for (const auto& c : session_.chatChannels())
            if ((c.channelId != 0) == (cat == 1)) list.push_back(&c);
        GgbUi::DisplayRow h;
        h.name = pp::cs(kCategory[cat]);
        h.header = true;
        h.collapsed = g.collapsed[cat];
        h.category = cat;
        h.count = static_cast<int>(list.size());
        g.rows.push_back(h);
        if (g.collapsed[cat]) continue;
        for (const auto* c : list) {
            GgbUi::DisplayRow r;
            r.name = c->name;
            r.category = cat;
            r.number = c->number;
            r.active = true;
            r.count = static_cast<int>(c->members);
            g.rows.push_back(r);
        }
    }
    g.chSelected = -1;
    for (size_t i = 0; i < g.rows.size(); ++i)
        if (!g.rows[i].header && g.rows[i].name == g.chSelectedName) g.chSelected = static_cast<int>(i);
    loadChannelArt();
}

void WorldScreen::channelOskTick() {
    GgbUi& g = ggbUi();
    bool accepted = false;
    std::string text;
    if (!g.chOskField || !pp::oskFinishedFor(kOskOwner, accepted, text)) return;
    const int field = g.chOskField;
    g.chOskField = 0;
    PSP_LOGI(pp::tag::Ctrl, "channel_osk field=%d ok=%d", field, accepted ? 1 : 0);
    if (!accepted) return;
    if (field == 1) g.dName = gbk::capUtf8(text, 31);         // ChannelFrameDaughterFrameChannelName (letters 31)
    else if (field == 2) g.dPassword = gbk::capUtf8(text, 31);
    else if (field == 3) g.chPopupEdit = gbk::capUtf8(text, 31);  // the popups' edit box
}

void WorldScreen::drawChannelFrame(UiRenderer& ui, bool focusable) {
    PROF_SCOPE("channels.frame");
    GgbUi& g = ggbUi();
    using soc::PX;
    using soc::PY;
    using soc::kSX;
    using soc::kSY;
    focusable = focusable && !windowOpen(kWinChannelDaughter) && !windowOpen(kWinChannelPopup);
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    // ChannelListScrollFrame (TOPLEFT 20,-76): ChannelButton 155 x 20.
    const int n = static_cast<int>(g.rows.size());
    g.chListOffset = std::clamp(g.chListOffset, 0, std::max(0, n - kListRows));
    for (int k = 0; k < kListRows && g.chListOffset + k < n; ++k) {
        const int idx = g.chListOffset + k;
        const GgbUi::DisplayRow& r = g.rows[static_cast<size_t>(idx)];
        const Rect b{PX(20), PY(76.0f + 20.0f * static_cast<float>(k)), 155 * kSX, 20 * kSY};
        if (g.chRowBg.pixels) ui.image(g.chRowBg, b, r.header ? 0xFFFFFFFFu : 0x80FFFFFFu);  // buttonLines alpha
        const uint32_t id = kIdChRow0 + static_cast<uint32_t>(k);
        const bool enabled = r.header ? r.count > 0 : true;  // a left channel takes the right click (JOIN)
        const bool f = focusable && enabled && winFocus_.addStop(id, b);
        if (idx == g.chSelected || f) {
            if (g.chHl.pixels) ui.image(g.chHl, b, 0xFFFFFFFFu, Blend::Add);
            else ui.fill(b, 0x30FFFFFFu);
        }
        const float ty = b.y + (b.h - static_cast<float>(fs.lineHeight())) * 0.5f;
        if (r.header) {
            ui.textShadow(fs, b.x + 5 * kSX, ty, r.name.c_str(), kColGold);  // NORMAL_FONT_COLOR_CODE
            if (r.count > 0) ui.textShadow(fs, b.x + b.w - 4, ty, r.collapsed ? "+" : "-", kColGrey, Align::Right);
        } else {
            char num[8];
            std::snprintf(num, sizeof(num), "%d. ", r.number);
            const std::string label = num + r.name;
            ui.pushClip(Rect{b.x + 10 * kSX, b.y, 135 * kSX, b.h});
            ui.textShadow(fs, b.x + 10 * kSX, ty, label.c_str(), r.active ? kColWhite : kColGrayFont);
            ui.popClip();
        }
    }
    if (n > kListRows) {
        const Rect up{PX(160), PY(78), 11, 11}, dn{PX(160), PY(398) - 11, 11, 11};
        if (npcArt_.scrUp.pixels) ui.image(g.chListOffset > 0 ? npcArt_.scrUp : npcArt_.scrUpDis, up);
        if (npcArt_.scrDown.pixels) ui.image(g.chListOffset < n - kListRows ? npcArt_.scrDown : npcArt_.scrDownDis, dn);
        if (focusable && winFocus_.addStop(kIdChListUp, up)) ui.fill(up, 0x40FFFFFFu);
        if (focusable && winFocus_.addStop(kIdChListDown, dn)) ui.fill(dn, 0x40FFFFFFu);
    }
    // ChannelRoster: the selected channel's name (183,-59) and its members (206,-75; 130 x 15).
    const bool rosterShown = g.chSelected >= 0 && g.rows[static_cast<size_t>(g.chSelected)].active &&
                             session_.rosterChannel() == g.chSelectedName;
    if (rosterShown) {
        ui.pushClip(Rect{PX(183), PY(52), 135 * kSX, 14});
        ui.textShadow(fs, PX(183), PY(59), g.chSelectedName.c_str(), kColGold);
        ui.popClip();
        const auto& roster = session_.channelRoster();
        const int m = static_cast<int>(roster.size());
        g.chRosterOffset = std::clamp(g.chRosterOffset, 0, std::max(0, m - kRosterRows));
        for (int k = 0; k < kRosterRows && g.chRosterOffset + k < m; ++k) {
            const gbk::ChannelMember& mem = roster[static_cast<size_t>(g.chRosterOffset + k)];
            const Rect b{PX(206), PY(75.0f + 15.0f * static_cast<float>(k)), 130 * kSX, 15 * kSY};
            const uint32_t id = kIdChMember0 + static_cast<uint32_t>(k);
            const bool f = focusable && winFocus_.addStop(id, b);
            if (f) {
                if (npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, b, 0xFFFFFFFFu, Blend::Add);
                else ui.fill(b, 0x30FFFFFFu);
            }
            std::string name = session_.channelMemberName(mem.guid);
            if (name.empty()) name = CLIENT_STR(UNKNOWNOBJECT);  // until the name query's answer
            const pp::Texture* rank = (mem.flags & gbk::kMemberOwner) ? &g.chOwner
                                      : (mem.flags & gbk::kMemberModerator) ? &g.chMod : nullptr;
            if (rank && rank->pixels) ui.image(*rank, Rect{b.x, b.y + (b.h - 12 * kSY) * 0.5f, 12 * kSX, 12 * kSY});
            ui.textShadow(ft, b.x + 13 * kSX, b.y + (b.h - static_cast<float>(ft.lineHeight())) * 0.5f, name.c_str(),
                          kColWhite);  // GameFontHighlightSmall
        }
        if (m > kRosterRows) {
            const Rect up{PX(320), PY(78), 11, 11}, dn{PX(320), PY(400) - 11, 11, 11};
            if (npcArt_.scrUp.pixels) ui.image(g.chRosterOffset > 0 ? npcArt_.scrUp : npcArt_.scrUpDis, up);
            if (npcArt_.scrDown.pixels)
                ui.image(g.chRosterOffset < m - kRosterRows ? npcArt_.scrDown : npcArt_.scrDownDis, dn);
            if (focusable && winFocus_.addStop(kIdChRosterUp, up)) ui.fill(up, 0x40FFFFFFu);
            if (focusable && winFocus_.addStop(kIdChRosterDown, dn)) ui.fill(dn, 0x40FFFFFFu);
        }
    }
    // ChannelFrameNewButton (BOTTOMRIGHT -12,82 of 355 x 512: 263..343, 408..430) and JoinButton to its left.
    // ChannelFrame_UpdateJoin: Join enabled for a selected world channel that is not joined (and with nothing selected
    // the client leaves it as it was - enabled from the XML).
    bool joinEnabled = true;
    if (g.chSelected >= 0) {
        const GgbUi::DisplayRow& r = g.rows[static_cast<size_t>(g.chSelected)];
        joinEnabled = !(r.category == 1 && r.active);
    }
    drawPanelButton(ui, kIdChNew, Rect{PX(263), PY(408), 80 * kSX, 22 * kSY}, CLIENT_STR(ADD), true, focusable);
    drawPanelButton(ui, kIdChJoin, Rect{PX(180), PY(408), 80 * kSX, 22 * kSY}, CLIENT_STR(JOIN), joinEnabled, focusable);
    // The list's dropdown anchor follows the row (the client's "cursor").
    if (g.dropKind == 1 && g.dropRow >= g.chListOffset && g.dropRow < g.chListOffset + kListRows)
        g.dropAnchor = Rect{PX(20) + 155 * kSX * 0.5f + ui.offsetX(),
                            PY(76.0f + 20.0f * static_cast<float>(g.dropRow - g.chListOffset + 1)) + ui.offsetY(), 0, 0};
    else if (g.dropKind == 2 && g.dropRow >= g.chRosterOffset && g.dropRow < g.chRosterOffset + kRosterRows)
        g.dropAnchor = Rect{PX(206) + 40 + ui.offsetX(),
                            PY(75.0f + 15.0f * static_cast<float>(g.dropRow - g.chRosterOffset + 1)) + ui.offsetY(), 0, 0};
}

// ChannelFrameDaughterFrame (the dialog backdrop; the name, the password with "(optional)", OK / Cancel).
bool WorldScreen::drawChannelDaughter(UiRenderer& ui) {
    GgbUi& g = ggbUi();
    const bool focusable = uiMode_ && !settings_.isOpen();
    const Font& fs = kit_.font(kFontSmall);
    loadNpcArt(false);
    const Rect r{std::floor((static_cast<float>(pp::kScreenW) - 190.0f) * 0.5f), 60.0f, 190.0f, 112.0f};
    drawDialogBox(ui, r);
    drawDialogHeader(ui, r, g.daughter == 1 ? CLIENT_STR(CHANNEL_NEW_CHANNEL) : CLIENT_STR(CHANNEL_JOIN_CHANNEL));
    const Rect close{r.x + r.w - 22, r.y + 4, 18, 18};
    if (npcArt_.close.pixels) ui.image(npcArt_.close, close);
    if (focusable && winFocus_.addStop(kIdChDClose, close)) ui.fill(close, 0x40FFFFFFu);
    float y = r.y + 22;
    for (int k = 0; k < 2; ++k) {
        ui.textShadow(fs, r.x + 14, y, k == 0 ? CLIENT_STR(CHANNEL_CHANNEL_NAME) : CLIENT_STR(PASSWORD), kColGold);
        if (k == 1) {
            const float w = static_cast<float>(fs.measure(CLIENT_STR(PASSWORD)));
            ui.textShadow(fs, r.x + 18 + w, y, CLIENT_STR(OPTIONAL_PARENS), kColGrey);
        }
        const Rect box{r.x + 14, y + 12, r.w - 28, 14};
        ui.fill(box, 0x90000000u);
        std::string shown = k == 0 ? g.dName : std::string(g.dPassword.size(), '*');
        ui.textShadow(fs, box.x + 3, box.y + 1, shown.c_str(), kColWhite);
        const uint32_t id = k == 0 ? kIdChDName : kIdChDPassword;
        if (focusable && winFocus_.addStop(id, box)) ui.fill(box, 0x30FFFFFFu);
        y += 30;
    }
    drawPanelButton(ui, kIdChDOk, Rect{r.x + 22, r.y + r.h - 24, 66, 14}, CLIENT_STR(OKAY), true, focusable);
    drawPanelButton(ui, kIdChDCancel, Rect{r.x + r.w - 88, r.y + r.h - 24, 66, 14}, CLIENT_STR(CANCEL), true, focusable);
    return true;
}

// ChannelListDropDown_Initialize / UnitPopup "CHAT_ROSTER" - the items as the open list shows them (rebuilt each frame:
// the owner / moderator flags of the roster's answer may come after the list opened).
void WorldScreen::channelDropItems(std::vector<std::pair<std::string, int>>& out) {
    GgbUi& g = ggbUi();
    platform::psp::WorldSession& s = session_;
    out.clear();
    enum { kPass = 1, kInvite, kJoin, kLeave, kCancel, kWhisper, kTarget, kPromote, kDemote, kOwner, kTitle };
    if (g.dropKind == 1) {
        if (g.dropRow < 0 || g.dropRow >= static_cast<int>(g.rows.size())) return;
        const auto& r = g.rows[static_cast<size_t>(g.dropRow)];
        const bool displayed = s.rosterChannel() == r.name;
        const uint8_t own = displayed ? s.ownChannelFlags() : 0;
        if (r.category == 2) {
            if (own & gbk::kMemberOwner) out.emplace_back(CLIENT_STR(CHAT_PASSWORD), kPass);
            if (own & (gbk::kMemberOwner | gbk::kMemberModerator)) out.emplace_back(CLIENT_STR(PARTY_INVITE), kInvite);
        }
        if (r.category == 1 && !r.active) out.emplace_back(CLIENT_STR(CHAT_JOIN), kJoin);
        if (r.active) out.emplace_back(CLIENT_STR(CHAT_LEAVE), kLeave);
        if (!out.empty()) out.emplace_back(CLIENT_STR(CANCEL), kCancel);
    } else if (g.dropKind == 2) {
        const auto& roster = s.channelRoster();
        if (g.dropRow < 0 || g.dropRow >= static_cast<int>(roster.size())) return;
        const gbk::ChannelMember& m = roster[static_cast<size_t>(g.dropRow)];
        const std::string name = s.channelMemberName(m.guid);
        out.emplace_back(name.empty() ? std::string(CLIENT_STR(UNKNOWNOBJECT)) : name, kTitle);  // UnitPopup's title: the name
        const bool self = m.guid == s.playerGuid();
        if (!self) out.emplace_back(CLIENT_STR(WHISPER), kWhisper);
        out.emplace_back(CLIENT_STR(TARGET), kTarget);
        const bool ownerSelf = (s.ownChannelFlags() & gbk::kMemberOwner) != 0;  // IsDisplayChannelOwner
        const bool owner = (m.flags & gbk::kMemberOwner) != 0, mod = (m.flags & gbk::kMemberModerator) != 0;
        if (ownerSelf && !owner && !mod && !self) out.emplace_back(CLIENT_STR(MAKE_MODERATOR), kPromote);
        if (ownerSelf && !owner && mod && !self) out.emplace_back(CLIENT_STR(REMOVE_MODERATOR), kDemote);
        if (ownerSelf && !owner && !self) out.emplace_back(CLIENT_STR(CHAT_OWNER), kOwner);
        out.emplace_back(CLIENT_STR(CANCEL), kCancel);
    }
}

bool WorldScreen::drawChannelDropDown(UiRenderer& ui) {
    GgbUi& g = ggbUi();
    std::vector<std::pair<std::string, int>> items;
    channelDropItems(items);
    if (items.empty()) {
        closeWindow(kWinChannelDropDown);
        g.dropKind = 0;
        return true;
    }
    std::vector<WotlkDdItem> dd;
    for (const auto& [text, code] : items) {
        WotlkDdItem it;
        it.text = text;
        it.isTitle = code == 11;
        dd.push_back(std::move(it));
    }
    wotlkDropDownList(ui, g.dropAnchor.x - ui.offsetX(), g.dropAnchor.y - ui.offsetY(), dd, kIdChDrop0,
                      uiMode_ && !settings_.isOpen());
    return true;
}

// CHAT_CHANNEL_INVITE / CHAT_CHANNEL_PASSWORD / CHANNEL_PASSWORD / CHANNEL_INVITE.
bool WorldScreen::drawChannelPopup(UiRenderer& ui) {
    GgbUi& g = ggbUi();
    StaticPopup p;
    p.focusable = uiMode_ && !settings_.isOpen();
    std::string text, edit;
    switch (g.chPopup) {
        case 1: text = CLIENT_FMT(CHAT_INVITE_NOTICE_POPUP, g.chPopupChannel, g.chPopupInviter); break;
        case 2: text = CLIENT_FMT(CHAT_PASSWORD_NOTICE_POPUP, g.chPopupChannel); break;
        case 3: text = CLIENT_FMT(CHANNEL_PASSWORD, g.chPopupChannel); break;
        case 4: text = CLIENT_FMT(CHANNEL_INVITE, g.chPopupChannel); break;
        default: return true;
    }
    p.text = text.c_str();
    p.button1 = CLIENT_STR(ACCEPT);
    p.button2 = g.chPopup == 1 ? CLIENT_STR(DECLINE) : CLIENT_STR(CANCEL);
    p.button1Id = kIdChPopup1;
    p.button2Id = kIdChPopup2;
    if (g.chPopup != 1) {
        edit = g.chPopup == 4 ? g.chPopupEdit : std::string(g.chPopupEdit.size(), '*');
        p.editText = edit.c_str();
        p.editId = kIdChPopupEdit;
    }
    drawStaticPopup(ui, p);
    return true;
}

bool WorldScreen::channelAction(uint32_t act, bool alt) {
    GgbUi& g = ggbUi();
    auto osk = [&](int field, const char* title, const std::string& initial) {
        if (pp::oskActive()) return;
        g.chOskField = field;
        if (!pp::oskBeginFor(kOskOwner, title, initial, 31, pp::OskCharset::Any)) g.chOskField = 0;
    };
    auto closeDrop = [&]() {
        if (windowOpen(kWinChannelDropDown)) closeWindow(kWinChannelDropDown);
        g.dropKind = 0;
    };
    auto openDrop = [&](int kind, int row) {
        closeDrop();
        g.dropKind = kind;
        g.dropRow = row;
        g.dropFocusMs = pp::logUptimeMs();
        openWindow(kWinChannelDropDown);
        winFocus_.setFocus(kIdChDrop0 + (kind == 2 ? 1u : 0u));
    };
    auto openDaughter = [&](int kind) {
        if (windowOpen(kWinChannelDaughter)) {  // the button toggles it
            closeWindow(kWinChannelDaughter);
            return;
        }
        g.daughter = kind;
        g.dName.clear();
        g.dPassword.clear();
        openWindow(kWinChannelDaughter);
        winFocus_.setFocus(kIdChDName);
        playSoundByName("UChatScrollButton");
    };
    if (act >= kIdChRow0 && act < kIdChRow0 + kListRows) {  // ChannelList_OnClick
        const int idx = g.chListOffset + static_cast<int>(act - kIdChRow0);
        if (idx >= static_cast<int>(g.rows.size())) return true;
        const GgbUi::DisplayRow r = g.rows[static_cast<size_t>(idx)];
        playSoundByName("igMainMenuOptionCheckBoxOn");
        closeDrop();
        if (!alt) {
            if (r.header) {
                g.collapsed[r.category] = !g.collapsed[r.category];  // Expand / CollapseChannelHeader
            } else {
                g.chSelectedName = r.name;
                g.chSelected = idx;
                g.chRosterOffset = 0;
                if (r.active) session_.requestChannelRoster(r.name);  // SetSelectedDisplayChannel
            }
            return true;
        }
        if (r.header) return true;
        g.chSelectedName = r.name;
        g.chSelected = idx;
        g.chRosterOffset = 0;
        if (r.active && session_.rosterChannel() != r.name) session_.requestChannelRoster(r.name);  // GetNumChannelMembers
        openDrop(1, idx);
        return true;
    }
    if (act >= kIdChMember0 && act < kIdChMember0 + kRosterRows) {  // ChannelRoster_OnClick (PSP: Cross too)
        const int idx = g.chRosterOffset + static_cast<int>(act - kIdChMember0);
        if (idx < static_cast<int>(session_.channelRoster().size())) openDrop(2, idx);
        return true;
    }
    if (act >= kIdChDrop0 && act < kIdChDrop0 + 16) {
        std::vector<std::pair<std::string, int>> items;
        channelDropItems(items);
        const size_t k = act - kIdChDrop0;
        const int code = k < items.size() ? items[k].second : 0;
        if (code == 11 || code == 0) return true;
        std::string channel, member;
        uint64_t memberGuid = 0;
        if (g.dropKind == 1 && g.dropRow >= 0 && g.dropRow < static_cast<int>(g.rows.size()))
            channel = g.rows[static_cast<size_t>(g.dropRow)].name;
        if (g.dropKind == 2) {
            channel = session_.rosterChannel();
            const auto& roster = session_.channelRoster();
            if (g.dropRow >= 0 && g.dropRow < static_cast<int>(roster.size()))
            {
                memberGuid = roster[static_cast<size_t>(g.dropRow)].guid;
                member = session_.channelMemberName(memberGuid);
            }
        }
        int rowCategory = 0;
        uint32_t rowNumber = 0;
        if (g.dropKind == 1 && g.dropRow >= 0 && g.dropRow < static_cast<int>(g.rows.size())) {
            rowCategory = g.rows[static_cast<size_t>(g.dropRow)].category;
            rowNumber = static_cast<uint32_t>(g.rows[static_cast<size_t>(g.dropRow)].number);
        }
        closeDrop();
        switch (code) {
            case 1:  // ChannelListDropDown_SetPassword -> CHANNEL_PASSWORD
            case 2:  // ChannelListDropDown_Invite -> CHANNEL_INVITE
                if (g.chPopup) return true;
                g.chPopup = code == 1 ? 3 : 4;
                g.chPopupChannel = channel;
                g.chPopupEdit.clear();
                g.chPopupAtMs = pp::logUptimeMs();
                openWindow(kWinChannelPopup);
                winFocus_.setFocus(kIdChPopupEdit);
                return true;
            case 3: {  // JoinPermanentChannel(name): a world channel by its ChatChannels shortcut
                const auto* c = session_.chatChannelByNumber(static_cast<int>(rowNumber));
                std::string shortcut;
                if (c && rowCategory == 1)
                    for (const auto& row : session_.chatChannelRows())
                        if (row.id == c->channelId) shortcut = row.shortcut;
                if (!session_.joinChannel(shortcut.empty() ? channel : shortcut))
                    session_.addSystemChatMessage(CLIENT_STR(CHAT_INVALID_NAME_NOTICE));
                return true;
            }
            case 4: session_.leaveChannel(channel); return true;  // LeaveChannelByName
            case 6:
                if (!member.empty()) chat_.openWhisper(member);   // ChatFrame_SendTell
                return true;
            case 7: {  // TargetUnit(name): only a unit the client sees
                UnitView u;
                if (memberGuid != 0 && unitView(memberGuid, u)) session_.setTarget(memberGuid);
                return true;
            }
            case 8: session_.channelMemberCommand(LogicalOpcode::CMSG_CHANNEL_MODERATOR, channel, member); return true;
            case 9: session_.channelMemberCommand(LogicalOpcode::CMSG_CHANNEL_UNMODERATOR, channel, member); return true;
            case 10: session_.channelMemberCommand(LogicalOpcode::CMSG_CHANNEL_SET_OWNER, channel, member); return true;
            default: return true;
        }
    }
    switch (act) {
        case kIdChListUp: g.chListOffset = std::max(0, g.chListOffset - 1); return true;
        case kIdChListDown: ++g.chListOffset; return true;
        case kIdChRosterUp: g.chRosterOffset = std::max(0, g.chRosterOffset - 1); return true;
        case kIdChRosterDown: ++g.chRosterOffset; return true;
        case kIdChNew:  // ChannelFrame_New_OnClick
            if (!alt) openDaughter(1);
            return true;
        case kIdChJoin: {  // ChannelFrame_Join_OnClick
            if (alt) return true;
            if (g.chSelected >= 0) {
                const GgbUi::DisplayRow& r = g.rows[static_cast<size_t>(g.chSelected)];
                if (r.category == 1) {  // button.global: JoinPermanentChannel(button.channel)
                    const auto* c = session_.chatChannelByNumber(r.number);
                    for (const auto& row : session_.chatChannelRows())
                        if (c && row.id == c->channelId) session_.joinChannel(row.shortcut);
                    return true;
                }
            }
            openDaughter(2);
            return true;
        }
        case kIdChDName: osk(1, CLIENT_STR(CHANNEL_CHANNEL_NAME), g.dName); return true;
        case kIdChDPassword: osk(2, CLIENT_STR(PASSWORD), g.dPassword); return true;
        case kIdChDOk: {  // ChannelFrameDaughterFrame_Okay
            if (!session_.joinChannel(g.dName, g.dPassword))
                session_.addSystemChatMessage(CLIENT_STR(CHAT_INVALID_NAME_NOTICE));
            closeWindow(kWinChannelDaughter);
            return true;
        }
        case kIdChDCancel:
        case kIdChDClose: closeWindow(kWinChannelDaughter); return true;
        case kIdChPopupEdit: osk(3, g.chPopup == 4 ? CLIENT_STR(GLUE_NAME) : CLIENT_STR(PASSWORD), g.chPopupEdit); return true;
        case kIdChPopup1:
        case kIdChPopup2: {
            const bool yes = act == kIdChPopup1;
            const int kind = g.chPopup;
            g.chPopup = 0;  // before the close: the update would decline it
            closeWindow(kWinChannelPopup);
            if (kind == 1) {
                if (yes) session_.joinChannel(g.chPopupChannel);  // JoinPermanentChannel(data)
                else session_.declineChannelInvite(g.chPopupChannel);
            } else if (yes && kind == 2) {
                session_.joinChannel(g.chPopupChannel, g.chPopupEdit);
            } else if (yes && kind == 3) {
                session_.channelSimpleCommand(LogicalOpcode::CMSG_CHANNEL_PASSWORD, g.chPopupChannel, g.chPopupEdit);
            } else if (yes && kind == 4 && !g.chPopupEdit.empty()) {
                session_.channelMemberCommand(LogicalOpcode::CMSG_CHANNEL_INVITE, g.chPopupChannel, g.chPopupEdit);
            }
            g.chPopupEdit.clear();
            return true;
        }
        default: return true;
    }
}

// ChatFrame.lua's channel commands (SlashCmdList JOIN / LEAVE / LIST_CHANNEL / CHAT_*) and "/N text". The
// client's own aliases (SLASH_<CMD><n>, Cyrillic in ruRU) from strings.pak of the running language.
bool WorldChat::channelCommand(const std::string& key, const std::string& args) {
    using CS = pp::CS;
    auto is = [&](std::initializer_list<const char*> names, std::initializer_list<CS> client = {}) {
        for (const char* n : names)
            if (key == n) return true;
        return clientSlash(key, client);
    };
    // A channel number or name -> the channel's name (GetChannelName).
    auto channelName = [&](const std::string& s) {
        if (digits(s)) {
            const auto* c = session_.chatChannelByNumber(std::atoi(s.c_str()));
            return c ? c->name : std::string();
        }
        return s;
    };
    if (digits(key)) {  // "/1 text": SendChatMessage(text, "CHANNEL", nil, 1), the sticky type
        const int n = std::atoi(key.c_str());
        if (n < 1 || n > 10) return false;
        const auto* c = session_.chatChannelByNumber(n);
        if (!c) return true;
        stickyType_ = game::ChatType::CHANNEL;
        stickyTarget_ = c->name;
        if (!args.empty()) session_.sendChannelMessage(n, args);
        return true;
    }
    std::string rest;
    const std::string first = firstWord(args, &rest);
    if (is({"join", "channel", "chan"}, {CS::SLASH_JOIN2, CS::SLASH_JOIN3, CS::SLASH_JOIN4})) {
        if (first.empty()) {
            session_.addSystemChatMessage(CLIENT_STR(CHAT_JOIN_HELP));
        } else if (!session_.joinChannel(first, firstWord(rest))) {
            session_.addSystemChatMessage(CLIENT_STR(CHAT_INVALID_NAME_NOTICE));
        }
        return true;
    }
    if (is({"leave", "chatleave", "chatexit"}, {CS::SLASH_LEAVE2, CS::SLASH_LEAVE3, CS::SLASH_LEAVE4})) {
        if (!first.empty()) session_.leaveChannel(first);
        return true;
    }
    if (is({"chatlist", "chatwho", "chatinfo"},
           {CS::SLASH_LIST_CHANNEL2, CS::SLASH_LIST_CHANNEL3, CS::SLASH_LIST_CHANNEL4, CS::SLASH_LIST_CHANNEL5})) {
        if (!first.empty()) {
            session_.listChannel(first);
        } else {  // ListChannels: the joined channels
            std::string line;
            for (int n = 1; n <= 10; ++n)
                if (const auto* c = session_.chatChannelByNumber(n)) {
                    if (!line.empty()) line += ", ";
                    line += "[" + std::to_string(n) + ". " + c->name + "]";
                }
            game::MessageChatData m;
            m.type = game::ChatType::CHANNEL_LIST;
            m.language = game::ChatLanguage::UNIVERSAL;
            m.message = std::string(CLIENT_STR(CHANNELS)) + ":" + (line.empty() ? std::string() : " " + line);
            session_.addLocalChatMessage(m);
        }
        return true;
    }
    if (is({"password", "pass"}, {CS::SLASH_CHAT_PASSWORD3, CS::SLASH_CHAT_PASSWORD5})) {  // SetChannelPassword(name, password)
        if (!first.empty()) session_.channelSimpleCommand(LogicalOpcode::CMSG_CHANNEL_PASSWORD, channelName(first), firstWord(rest));
        return true;
    }
    if (is({"owner"}, {CS::SLASH_CHAT_OWNER2})) {  // SetChannelOwner / DisplayChannelOwner
        const std::string who = firstWord(rest);
        if (first.empty()) return true;
        if (!who.empty()) session_.channelMemberCommand(LogicalOpcode::CMSG_CHANNEL_SET_OWNER, channelName(first), who);
        else session_.channelSimpleCommand(LogicalOpcode::CMSG_CHANNEL_OWNER, channelName(first));
        return true;
    }
    struct MemberCmd {
        std::initializer_list<const char*> names;
        std::initializer_list<CS> client;
        LogicalOpcode op;
    };
    const MemberCmd cmds[] = {
        {{"mod", "moderator"}, {CS::SLASH_CHAT_MODERATOR2}, LogicalOpcode::CMSG_CHANNEL_MODERATOR},
        {{"unmod", "unmoderator"}, {CS::SLASH_CHAT_UNMODERATOR1, CS::SLASH_CHAT_UNMODERATOR2},
         LogicalOpcode::CMSG_CHANNEL_UNMODERATOR},
        {{"unvoice", "mute", "squelch"}, {CS::SLASH_CHAT_MUTE1, CS::SLASH_CHAT_MUTE2, CS::SLASH_CHAT_MUTE6},
         LogicalOpcode::CMSG_CHANNEL_MUTE},
        {{"unmute", "unsquelch", "voice"}, {CS::SLASH_CHAT_UNMUTE1, CS::SLASH_CHAT_UNMUTE2},
         LogicalOpcode::CMSG_CHANNEL_UNMUTE},
        {{"cinvite", "chatinvite"}, {CS::SLASH_CHAT_CINVITE1, CS::SLASH_CHAT_CINVITE2}, LogicalOpcode::CMSG_CHANNEL_INVITE},
        {{"ckick"}, {CS::SLASH_CHAT_KICK1}, LogicalOpcode::CMSG_CHANNEL_KICK},
        {{"ban"}, {CS::SLASH_CHAT_BAN1}, LogicalOpcode::CMSG_CHANNEL_BAN},
        {{"unban"}, {CS::SLASH_CHAT_UNBAN2}, LogicalOpcode::CMSG_CHANNEL_UNBAN},
    };
    for (const MemberCmd& c : cmds)
        if (is(c.names, c.client)) {
            const std::string who = firstWord(rest);
            if (!first.empty() && !who.empty()) session_.channelMemberCommand(c.op, channelName(first), who);
            return true;
        }
    if (is({"ann", "announce"}, {CS::SLASH_CHAT_ANNOUNCE1})) {  // ChannelToggleAnnouncements
        if (!first.empty()) session_.channelSimpleCommand(LogicalOpcode::CMSG_CHANNEL_ANNOUNCEMENTS, channelName(first));
        return true;
    }
    if (is({"moderate"}, {CS::SLASH_CHAT_MODERATE1})) {  // ChannelModerate
        if (!first.empty()) session_.channelSimpleCommand(LogicalOpcode::CMSG_CHANNEL_MODERATE, channelName(first));
        return true;
    }
    return false;
}

}  // namespace wowee::ui::psp
