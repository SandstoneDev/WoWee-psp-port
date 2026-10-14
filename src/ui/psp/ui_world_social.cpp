// FriendsFrame of the original ruRU 3.3.5a client on the world screen
// (FriendsFrame.xml / .lua, StaticPopup.lua, UnitPopup.lua, ChatFrame.lua) at the character frame's 0.6 x 0.55, where
// the Raid tab (ui_world_raid.cpp) draws it too:
//   - FriendsFrame (384 x 512: FriendsFrameScrollIcon, the four textures FriendsFrame_Update sets per tab - ui.pak
//     sf_friends / sf_who / sf_guild, the Raid tab's rf_frame), FriendsFrameTitleText, the close button, FriendsFrameTab1..5
//     FRIENDS WHO GUILD CHAT RAID (PanelTemplates: the Guild tab disabled out of a guild - InGuildCheck;
// CHAT = ChannelFrame, ui_world_channels.cpp);
//   - the Friends tab: FriendsTabHeaderTab1 / 2 FRIENDS / IGNORE (TabButtonTemplate; BNFeaturesEnabled() is
//     false - no online-service tab, broadcast, presence), FriendsFrameStatusDropDown (FRIENDS_LIST_AVAILABLE / _AWAY / _BUSY:
//     SendChatMessage("", "AFK" / "DND")); FriendsList_Update / FriendsFrame_SetButton: online friends, the offline
//     divider, offline friends (34-unit buttons: the status icon, "Name, " FRIENDS_LEVEL_TEMPLATE / the name grey, the zone),
//     FriendsFrameAddFriendButton (the target, else StaticPopup ADD_FRIEND), FriendsFrameSendMessageButton
//     (ChatFrame_SendTell); IgnoreList_Update: IGNORED and the names (16-unit buttons), IGNORE_PLAYER (the target,
//     else ADD_IGNORE), REMOVE_PLAYER (DelIgnore);
//   - WhoFrame: WhoList_Update (the column headers - SortWho, WhoFrameDropDown ZONE / GUILD / RACE - the
//     17 buttons, the class colour, WHO_FRAME_TOTAL_TEMPLATE / WHO_FRAME_SHOWN_TEMPLATE), WhoFrameEditBox (SendWho),
//     REFRESH, ADD_FRIEND, GROUP_INVITE; SetWhoToUI while it shows;
//   - GuildFrame and its side frames: ui_world_guild.cpp.
// Controls (PSP, docs/PSP_CONTROLS.md): Cross = the left click (select, the buttons), Circle = the right click (a
// friend / who / guild row: FriendsFrame_ShowDropdown - the unit menu FRIEND / FRIEND_OFFLINE of ui_world_party.cpp),
// the edit boxes and the StaticPopups with an edit box: the PSP keyboard (its title = the popup's text; OK = Enter,
// cancel = Escape). START closes a popup / a dropdown first, the frame with its close button or Circle on its title.
// StaticPopup GUILD_INVITE (GUILD_INVITE_REQUEST: ACCEPT AcceptGuild / DECLINE DeclineGuild, 60 s, START =
// decline) and the confirmations CONFIRM_GUILD_LEAVE / _PROMOTE / _DISBAND, REMOVE_GUILDMEMBER; the /who chat lines
// (SMSG_WHO with SetWhoToUI off: WHO_LIST_FORMAT / WHO_LIST_GUILD_FORMAT, WHO_NUM_RESULTS); the guild
// tabard emblems of the player and the others (fillGuildEmblem: PLAYER_GUILDID's guild query).
#include "ui/psp/ui_world_social.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_osk.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
constexpr pp::CS kTabText[5] = {pp::CS::FRIENDS, pp::CS::WHO, pp::CS::GUILD, pp::CS::CHAT, pp::CS::RAID};
constexpr pp::CS kStatusText[3] = {pp::CS::FRIENDS_LIST_AVAILABLE, pp::CS::FRIENDS_LIST_AWAY,
                                   pp::CS::FRIENDS_LIST_BUSY};

// FontString colours (0xAABBGGRR).
constexpr uint32_t kColDisable = 0xFF808080u;     // GameFontDisable (0.5)
constexpr uint32_t kColFriendName = 0xFF5CE1FEu;  // FRIENDS_WOW_NAME_COLOR (0.996, 0.882, 0.361)
constexpr uint32_t kColFriendGray = 0xFF8A847Cu;  // FRIENDS_GRAY_COLOR (0.486, 0.518, 0.541)
constexpr uint32_t kBgOnline = 0x0D00D2FFu;       // FRIENDS_WOW_BACKGROUND_COLOR (1, 0.824, 0, 0.05)
constexpr uint32_t kBgOffline = 0x0D969696u;      // FRIENDS_OFFLINE_BACKGROUND_COLOR (0.588, 0.588, 0.588, 0.05)

constexpr int kOskOwner = 0x534F;  // "SO": the social frames' keyboard (psp_osk oskBeginFor)

// format() with Lua's positional (%1$s, %2$d) and plain %s / %d arguments, given as text; |3-N(x) of a player's name
// becomes x, |4one:few:many; the Russian plural of the number before it.
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
    return out;
}

std::string stripNameDeclension(std::string out) {
    for (size_t at; (at = out.find("|3-")) != std::string::npos;) {
        const size_t open = out.find('(', at), close = out.find(')', at);
        if (open == std::string::npos || close == std::string::npos || close < open) break;
        out = out.substr(0, at) + out.substr(open + 1, close - open - 1) + out.substr(close + 1);
    }
    return out;
}

// RAID_CLASS_COLORS (0xAABBGGRR) by class id; 0 = none (HIGHLIGHT_FONT_COLOR / NORMAL_FONT_COLOR as the caller says).
uint32_t classColor(uint32_t cls) {
    switch (cls) {
        case 1: return 0xFF6E9CC7u;   // WARRIOR
        case 2: return 0xFFBA8CF5u;   // PALADIN
        case 3: return 0xFF73D4ABu;   // HUNTER
        case 4: return 0xFF69F5FFu;   // ROGUE
        case 5: return 0xFFFFFFFFu;   // PRIEST
        case 6: return 0xFF3B1FC4u;   // DEATHKNIGHT
        case 7: return 0xFFDE7000u;   // SHAMAN
        case 8: return 0xFFF0CC69u;   // MAGE
        case 9: return 0xFFC98294u;   // WARLOCK
        case 11: return 0xFF0A7DFFu;  // DRUID
        default: return 0;
    }
}

// UIDropDownMenuTemplate's visible box (CharacterCreate-LabelFrame, wm_dd: the caps 8 / 7 texels).
void drawDropDownBox(UiRenderer& ui, const pp::Texture& t, const Rect& r) {
    if (!t.pixels) {
        ui.fill(r, 0xC0101010u);
        return;
    }
    const float tw = t.uw, th = t.uh, capL = 8.0f, capR = 7.0f, s = r.h / th;
    ui.image(t, Rect{r.x, r.y, capL * s, r.h}, SrcRect{0, 0, capL, th});
    ui.image(t, Rect{r.x + capL * s, r.y, r.w - (capL + capR) * s, r.h}, SrcRect{capL, 0, tw - capL - capR, th});
    ui.image(t, Rect{r.x + r.w - capR * s, r.y, capR * s, r.h}, SrcRect{tw - capR, 0, capR, th});
}

// A text cut to `w` px (the FontString's width) - the last whole characters that fit.
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

}  // namespace

// --- State and art ---------------------------------------------------------------------------------------------------

WorldScreen::SocialUi::~SocialUi() {
    freeFrameArt();
    freeSideArt();
    freeSmallArt();
}

void WorldScreen::SocialUi::freeFrameArt() {
    if (frame.pixels) pp::textureRelease(frame);
    if (frameKind >= 0) PSP_LOGI(pp::tag::Gfx, "social_frame_art_free kind=%d kb=%u", frameKind, static_cast<unsigned>(frameBytes / 1024u));
    frameKind = -1;
    frameBytes = 0;
}

void WorldScreen::SocialUi::freeSideArt() {
    if (side.pixels) pp::textureRelease(side);
    if (sideKind) PSP_LOGI(pp::tag::Gfx, "social_side_art_free kind=%d kb=%u", sideKind, static_cast<unsigned>(sideBytes / 1024u));
    sideKind = 0;
    sideBytes = 0;
}

void WorldScreen::SocialUi::freeSmallArt() {
    for (pp::Texture* t : {&status, &divider, &hlbar, &coltab, &colhl, &rowhl, &htabOn, &htabOff, &cbUp, &cbHl, &check,
                           &ddBox, &ddBtn, &ddBtnDn, &ddHl, &ddBg, &plus, &minus, &updn, &permtab, &input, &whoScroll,
                           &guildScroll, &pageNext})
        if (t->pixels) pp::textureRelease(*t);
    if (smallTried) PSP_LOGI(pp::tag::Gfx, "social_art_free kb=%u", static_cast<unsigned>(smallBytes / 1024u));
    smallTried = false;
    smallBytes = 0;
}

void WorldScreen::destroySocialUi(SocialUi* p) { delete p; }

WorldScreen::SocialUi& WorldScreen::socialUi() {
    if (!soc_) {
        soc_ = std::unique_ptr<SocialUi, void (*)(SocialUi*)>(new SocialUi, &WorldScreen::destroySocialUi);
        // the session outlives the world (a map transfer builds a new WorldScreen): its counters are taken
        // as seen, so the old WHO_LIST_UPDATE / FRIENDJOINGAME / GUILD_INVITE_REQUEST do not fire again. An invite still
        // unanswered shows again (updateSocial: the session's pending flag; the original's StaticPopup stays through
        // the loading screen).
        SocialUi& s = *soc_;
        s.whoSeen = session_.whoVersion();
        s.friendSoundSeq = session_.friendSoundSeq();
        s.inviteSeq = session_.guildInviteSeq();
        PSP_LOGI(pp::tag::Ctrl, "social_ui_new who=%u friend_snd=%u invite=%u pending=%d",
                 static_cast<unsigned>(s.whoSeen), static_cast<unsigned>(s.friendSoundSeq),
                 static_cast<unsigned>(session_.guildInviteSeq()), session_.hasPendingGuildInvite() ? 1 : 0);
    }
    return *soc_;
}

void WorldScreen::loadSocialSmallArt() {
    SocialUi& s = socialUi();
    if (s.smallTried) return;
    s.smallTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0, n = 0;
    if (pak.open(path)) {
        const struct {
            const char* name;
            pp::Texture* t;
        } list[] = {{"sf_status", &s.status},      {"sf_divider", &s.divider},     {"sf_hlbar", &s.hlbar},
                    {"sf_coltab", &s.coltab},      {"sf_colhl", &s.colhl},         {"ql_titlehl", &s.rowhl},
                    {"insp_htab_active", &s.htabOn}, {"insp_htab_inactive", &s.htabOff}, {"cb_up", &s.cbUp},
                    {"cb_hl", &s.cbHl},            {"pf_check", &s.check},         {"wm_dd", &s.ddBox},
                    {"wm_dd_btn", &s.ddBtn},       {"wm_dd_btn_dn", &s.ddBtnDn},   {"wm_dd_hl", &s.ddHl},
                    {"wm_dd_bg", &s.ddBg},         {"ql_plus", &s.plus},           {"ql_minus", &s.minus},
                    {"sf_updn", &s.updn},          {"sf_permtab", &s.permtab},     {"sf_input", &s.input},
                    {"sf_who_scroll", &s.whoScroll}, {"sf_guild_scroll", &s.guildScroll}, {"merch_next", &s.pageNext}};
        for (const auto& x : list) {
            ++n;
            if (socialLoad(pak, x.name, *x.t, s.smallBytes)) ++ok;
        }
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "social_art ok=%d/%d kb=%u", ok, n, static_cast<unsigned>(s.smallBytes / 1024u));
}

void WorldScreen::loadSocialFrameArt(int kind) {
    SocialUi& s = socialUi();
    if (s.frameKind == kind) return;
    s.freeFrameArt();
    s.frameKind = kind;
    static const char* const kNames[4] = {"sf_friends", "sf_who", "sf_guild", "sf_channel"};  // + 3
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (pak.open(path)) {
        socialLoad(pak, kNames[kind], s.frame, s.frameBytes);
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "social_frame_art kind=%d ok=%d kb=%u", kind, s.frame.pixels ? 1 : 0,
             static_cast<unsigned>(s.frameBytes / 1024u));
}

// --- Names (text.pak / glue.pak) -------------------------------------------------------------------------------------

// The AreaTable name of an area (text.pak "zones", key > 0): the ids asked for are read in one pass, at most once a
// second; the names stay while the frame lives (bounded: 512).
std::string WorldScreen::socialZoneName(uint32_t area) {
    if (area == 0) return CLIENT_STR(UNKNOWN);
    SocialUi& s = socialUi();
    auto it = s.zones.find(area);
    if (it != s.zones.end()) return it->second;
    if (s.zones.size() > 512) s.zones.clear();
    s.zonesWanted.insert(area);
    const uint32_t now = pp::logUptimeMs();
    if (now - s.zonesReadMs < 1000u && s.zonesReadMs != 0) return std::string();
    s.zonesReadMs = now ? now : 1u;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/text.pak");
    pp::Pak pak;
    if (pak.open(path)) {
        if (const pp::PakEntry* e = pak.find("zones")) {
            if (auto* b = static_cast<const char*>(pak.load(e))) {
                const char *p = b + 4, *end = b + e->size;
                while (p + 4 < end) {
                    int32_t k = 0;
                    std::memcpy(&k, p, 4);
                    p += 4;
                    const char* str = p;
                    while (p < end && *p) ++p;
                    if (k > 0 && s.zonesWanted.count(static_cast<uint32_t>(k))) s.zones[static_cast<uint32_t>(k)].assign(str, p);
                    ++p;
                }
                pp::pakFree(const_cast<char*>(b));
            }
        }
        pak.close();
    }
    for (uint32_t id : s.zonesWanted)
        if (!s.zones.count(id)) s.zones[id] = CLIENT_STR(UNKNOWN);  // GetWhoInfo / GetFriendInfo: UNKNOWN without an AreaTable row
    s.zonesWanted.clear();
    it = s.zones.find(area);
    return it != s.zones.end() ? it->second : std::string();
}

// The who tags' searches (SendWho, psp_social_parse.hpp parseWhoText): AreaTable zones without a parent (glue.pak
// "zones"), ChrRaces / ChrClasses names in either gender form (text.pak "names").
void WorldScreen::socialSetWhoLookup() {
    SocialUi& s = socialUi();
    if (s.whoLookupSet) return;
    s.whoLookupSet = true;
    so::WhoLookup lk;
    // The tags of the running language (WHO_TAG_*, strings.pak).
    lk.tagName = CLIENT_STR(WHO_TAG_NAME);
    lk.tagGuild = CLIENT_STR(WHO_TAG_GUILD);
    lk.tagZone = CLIENT_STR(WHO_TAG_ZONE);
    lk.tagRace = CLIENT_STR(WHO_TAG_RACE);
    lk.tagClass = CLIENT_STR(WHO_TAG_CLASS);
    lk.zones = [this](const std::string& text, std::vector<uint32_t>& out) {
        SocialUi& st = socialUi();
        if (!st.topZonesTried) {
            st.topZonesTried = true;
            char path[256];
            pp::fsPath(path, sizeof(path), "data/glue.pak");
            pp::Pak pak;
            if (pak.open(path)) {
                if (const pp::PakEntry* e = pak.find("zones")) {
                    if (auto* b = static_cast<const uint8_t*>(pak.load(e))) {
                        uint32_t n = 0;
                        std::memcpy(&n, b, 4);
                        for (uint32_t i = 0; i < n && 4u + 8u * (i + 1) <= e->size; ++i) {
                            uint32_t id = 0, off = 0;
                            std::memcpy(&id, b + 4 + 8 * i, 4);
                            std::memcpy(&off, b + 8 + 8 * i, 4);
                            if (off < e->size) st.topZones.emplace_back(id, reinterpret_cast<const char*>(b + off));
                        }
                        pp::pakFree(const_cast<uint8_t*>(b));
                    }
                }
                pak.close();
            }
            PSP_LOGI(pp::tag::Gfx, "who_zones n=%u", static_cast<unsigned>(st.topZones.size()));
        }
        for (const auto& z : st.topZones)
            if (out.size() < 10 && so::beginsWithName(z.second, text)) out.push_back(z.first);
    };
    lk.raceMask = [this](const std::string& text) {
        uint32_t m = 0;
        for (uint8_t r = 1; r < 12; ++r)
            for (bool female : {false, true}) {
                const std::string n = socialRaceName(r, female);
                if (!n.empty() && so::beginsWithName(n, text)) m |= 1u << r;
            }
        return m;
    };
    lk.classMask = [this](const std::string& text) {
        uint32_t m = 0;
        for (uint8_t c = 1; c <= 11; ++c)
            for (bool female : {false, true}) {
                const std::string n = socialClassName(c, female);
                if (!n.empty() && so::beginsWithName(n, text)) m |= 1u << c;
            }
        return m;
    };
    lk.zoneName = [this](uint32_t zone) { return socialZoneName(zone); };
    session_.setWhoLookup(std::move(lk));
}

// --- Per frame -------------------------------------------------------------------------------------------------------

void WorldScreen::updateSocial() {
    session_.socialUpdate();
    SocialUi& s = socialUi();
    socialSetWhoLookup();
    socialOskTick();
    socialWhoChat();
    {  // the chat's slash commands that need a frame (ChatFrame.lua SlashCmdList)
        int code = 0;
        std::string arg;
        if (chat_.takeSocialRequest(code, arg)) socialChatRequest(code, arg);
    }
    const uint32_t now = pp::logUptimeMs();
    // FRIENDJOINGAME: a friend (or, with guildMemberNotify, a guild member) came online.
    if (session_.friendSoundSeq() != s.friendSoundSeq) {
        s.friendSoundSeq = session_.friendSoundSeq();
        playSoundByName("FriendJoinGame");
    }
    // FriendsFrame:IsShown() - the Raid tab is the raid code's window.
    if (windowOpen(kWinRaid)) s.tab = 5;
    const bool shown = windowOpen(kWinFriends) || windowOpen(kWinRaid);
    if (!shown && s.shown) {  // FriendsFrame_OnHide
        s.selectedMember = 0;
        s.popup = 0;  // GuildFramePopup_HideAll
        if (windowOpen(kWinSocialDropDown)) closeWindow(kWinSocialDropDown);
        s.dropKind = 0;
    }
    s.shown = shown;
    // SetWhoToUI: WhoFrame OnShow 1 / OnHide 0.
    session_.setWhoToUi(windowOpen(kWinFriends) && s.tab == 2);
    // WHO_LIST_UPDATE -> WhoList_Update: PanelTemplates_SetTab(FriendsFrame, 2); ShowUIPanel(FriendsFrame).
    if (session_.whoVersion() != s.whoSeen) {
        s.whoSeen = session_.whoVersion();
        if (s.whoSeen != 0) {
            if (s.selectedWho > static_cast<int>(session_.whoList().size())) s.selectedWho = 0;
            if (!windowOpen(kWinFriends) || s.tab != 2) {
                if (windowOpen(kWinRaid)) socialSwapFrame(kWinRaid, kWinFriends);
                s.tab = 2;
                if (!windowOpen(kWinFriends)) {
                    openWindow(kWinFriends);
                    playSoundByName("igCharacterInfoTab");
                }
            }
        }
    }
    // InGuildCheck: the Guild tab is disabled out of a guild; on it, the frame goes back to the Friends tab.
    if (s.tab == 3 && !session_.isInGuild() && windowOpen(kWinFriends)) {
        s.tab = 1;
        socialFriendsFrameUpdate();
    }
    // GuildFrame OnShow: GuildRoster(); GUILD_ROSTER_UPDATE(1) while it shows: GuildRoster() again.
    if (windowOpen(kWinFriends) && s.tab == 3 && session_.isInGuild()) {
        if (!s.rosterAsked || session_.guildVersion() != s.guildSeen) {
            s.rosterAsked = true;
            s.guildSeen = session_.guildVersion();
            session_.guildRosterRequest();  // the session's 5 s limit decides
        }
    } else {
        s.rosterAsked = false;
    }
    // StaticPopup GUILD_INVITE (GUILD_INVITE_REQUEST): 60 s, OnCancel (a timeout, START) = DeclineGuild.
    // shown while the session has it unanswered (as PARTY_INVITE's hasPendingGroupInvite): a new invite,
    // or one still pending when this frame state was made (a map transfer).
    const bool newInvite = session_.guildInviteSeq() != s.inviteSeq;
    s.inviteSeq = session_.guildInviteSeq();
    if (session_.hasPendingGuildInvite() && (newInvite || !s.inviteShown)) {
        s.inviteText = fmt(CLIENT_STR(GUILD_INVITATION), {session_.guildInviter(), session_.guildInviteName()});
        s.inviteAtMs = now;
        s.inviteShown = true;
        s.inviteAnswered = false;
        openWindow(kWinGuildInvite);
        placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), kWinGuildInvite), placeNext_.end());
        PSP_LOGI(pp::tag::Ctrl, "guild_invite_popup from=%s", session_.guildInviter().c_str());
    }
    if (s.inviteShown) {
        if (windowOpen(kWinGuildInvite) && now - s.inviteAtMs > 60000u) closeWindow(kWinGuildInvite);  // timeout = 60
        if (!windowOpen(kWinGuildInvite)) {
            if (!s.inviteAnswered) session_.declineGuild();
            s.inviteShown = false;
        }
    }
    if (s.confirm && !windowOpen(kWinSocialConfirm)) s.confirm = 0;
    // A dropdown list closes with its frame, out of the interface, UIDROPDOWNMENU_SHOW_TIME (2 s) without focus.
    if (s.dropKind) {
        const uint32_t f = winFocus_.focused();
        if ((f >= kIdSfDrop0 && f < kIdSfDrop0 + 16) || f == kIdSfStatus || f == kIdSfWhoDrop || f == kIdSfCtlRank)
            s.dropFocusMs = now;
        const bool owner = s.dropKind == 3 ? (windowOpen(kWinFriends) && s.popup == 3) : windowOpen(kWinFriends);
        if (!windowOpen(kWinSocialDropDown) || !owner || !uiMode_ || now - s.dropFocusMs > 2000u) {
            if (windowOpen(kWinSocialDropDown)) closeWindow(kWinSocialDropDown);
            s.dropKind = 0;
        }
    }
    // The art: the tab's frame and the small pieces while the frame shows, the guild side frame while it shows.
    if (windowOpen(kWinFriends)) {
        loadSocialSmallArt();
        loadSocialFrameArt(s.tab == 2 ? 1 : s.tab == 3 ? 2 : s.tab == 4 ? 3 : 0);
    } else {
        if (s.frameKind >= 0) s.freeFrameArt();
        if (s.smallTried && !windowOpen(kWinSocialDropDown)) s.freeSmallArt();
    }
    // the side art matched as drawGuildSideFrame loads it (the non-officer's member card is kind 5).
    if (s.sideKind && (!windowOpen(kWinFriends) || s.tab != 3 ||
                       soc::sideArtKind(s.popup, session_.guildCan(so::kRightViewOfficerNote)) != s.sideKind))
        s.freeSideArt();
    // A guild answer can bring an emblem: the players are dressed again (the scan compares their Equipment).
    static uint32_t emblemSeen = 0;
    if (session_.guildCacheVersion() != emblemSeen) {
        emblemSeen = session_.guildCacheVersion();
        seenEntityVersion_ = ~0u;
    }
}

// FriendsFrame_Update: the tab's frame (FriendsTabHeader / WhoFrame / GuildFrame), ShowFriends for the friends list.
void WorldScreen::socialFriendsFrameUpdate() {
    SocialUi& s = socialUi();
    if (s.tab == 1 && s.header == 1) session_.showFriends();
    if (s.tab == 3) s.rosterAsked = false;
}

// The /who chat lines (SMSG_WHO with SetWhoToUI off - upstream handleWho: every result, then the count):
// WHO_LIST_FORMAT / WHO_LIST_GUILD_FORMAT with the race and class names as upstream gives them (the base form,
// the declension |3-6 of text.pak), WHO_NUM_RESULTS.
void WorldScreen::socialWhoChat() {
    std::vector<pp::WorldSession::WhoRec> list;
    uint32_t count = 0;
    if (!session_.takeWhoChat(list, count)) return;
    for (const auto& w : list) {
        std::string race = socialRaceName(static_cast<uint8_t>(w.race), false);
        std::string cls = socialClassName(static_cast<uint8_t>(w.cls), false);
        if (race.empty()) race = CLIENT_STR(UNKNOWN);
        if (cls.empty()) cls = CLIENT_STR(UNKNOWN);
        std::string zone = socialZoneName(w.zone);
        if (zone.empty()) zone = CLIENT_STR(UNKNOWN);
        const std::string lvl = std::to_string(w.level);
        std::string line = w.guild.empty() ? fmt(CLIENT_STR(WHO_LIST_FORMAT), {w.name, w.name, lvl, race, cls, zone})
                                           : fmt(CLIENT_STR(WHO_LIST_GUILD_FORMAT), {w.name, w.name, lvl, race, cls, w.guild, zone});
        session_.addSystemChatMessage(socialDecline(line));
    }
    session_.addSystemChatMessage(fmt(CLIENT_STR(WHO_NUM_RESULTS), {std::to_string(count)}));
}

void WorldScreen::socialTip(UiRenderer& ui, uint32_t id, const Rect& r, const std::string& title, const std::string& text,
                            uint32_t titleColor) {
    SocialUi& s = socialUi();
    s.tipId = id;
    s.tipRect = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};  // the screen (drawn after the windows)
    s.tipTitle = title;
    s.tipText = text;
    s.tipTitleColor = titleColor;
}

// --- The frame: open / close, tabs ---------------------------------------------------------------------------------

// ToggleFriendsFrame(tab): no tab - show / hide on the last tab; a tab - the Guild tab only in a guild, the shown tab
// again hides. FriendsFrame_OnShow: FriendsList_Update, FriendsFrame_Update, igCharacterInfoTab, no guild selection.
void WorldScreen::toggleFriendsFrame(int tab) {
    SocialUi& s = socialUi();
    const bool shown = windowOpen(kWinFriends) || windowOpen(kWinRaid);
    if (tab == 0) {
        if (shown) {
            if (windowOpen(kWinFriends)) closeWindow(kWinFriends);
            if (windowOpen(kWinRaid)) closeWindow(kWinRaid);
            playSoundByName("igMainMenuClose");
            return;
        }
        tab = s.tab;
    } else {
        if (tab == 3 && !session_.isInGuild()) return;
        if (tab == s.tab && shown) {
            if (windowOpen(kWinFriends)) closeWindow(kWinFriends);
            if (windowOpen(kWinRaid)) closeWindow(kWinRaid);
            playSoundByName("igMainMenuClose");
            return;
        }
    }
    if (tab == 3 && !session_.isInGuild()) tab = 1;  // InGuildCheck
    s.tab = tab;
    s.selectedMember = 0;
    if (tab == 5) {
        if (windowOpen(kWinFriends)) socialSwapFrame(kWinFriends, kWinRaid);
        else if (!windowOpen(kWinRaid)) openWindow(kWinRaid);
    } else {
        if (windowOpen(kWinRaid)) socialSwapFrame(kWinRaid, kWinFriends);
        else if (!windowOpen(kWinFriends)) openWindow(kWinFriends);
        socialFriendsFrameUpdate();
    }
    playSoundByName("igCharacterInfoTab");
    PSP_LOGI(pp::tag::Ctrl, "friends_frame tab=%d header=%d", s.tab, s.header);
}

// PSP: the Raid tab is its own window: a tab switch closes one window and opens the other where the
// first one stood (the place of the dragged frame is kept for this session, as one FriendsFrame).
void WorldScreen::socialSwapFrame(uint8_t from, uint8_t to) {
    auto r = winRect_.find(from);
    const bool placed = r != winRect_.end() && r->second.w > 0.0f;
    const Rect at = placed ? r->second : Rect{};
    windows_.erase(std::remove(windows_.begin(), windows_.end(), from), windows_.end());
    autoPos_.erase(from);
    placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), from), placeNext_.end());
    openWindow(to);
    placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), to), placeNext_.end());
    if (placed) winPos_[to] = WinPos{at.x, at.y};
}

// A FriendsFrameTabTemplate tab (CharacterFrameTabButtonTemplate's art, tabArt_) - drawFrameTab with the disabled
// state of PanelTemplates_DisableTab (grey text, no click).
void WorldScreen::drawFriendsTabs(UiRenderer& ui, int selected, bool focusable) {
    if (!tabArtTried_) {  // CharacterFrameTabButtonTemplate (the paperdoll / the raid frame load it the same way)
        tabArtTried_ = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        uint32_t bytes = 0;
        if (pak.open(path)) {
            socialLoad(pak, "tab_active", tabArt_[0], bytes);
            socialLoad(pak, "tab_inactive", tabArt_[1], bytes);
            pak.close();
        }
    }
    const Font& fs = kit_.font(kFontSmall);
    const float h = 32.0f * kSY, cap = 20.0f * kSX;
    float x = PX(11);
    const float y = PY(512 - 46) - h;
    for (int k = 1; k <= 5; ++k) {
        const bool sel = k == selected;
        // GUILD out of a guild (InGuildCheck). CHAT = ChannelFrame (ui_world_channels.cpp).
        const bool enabled = !(k == 3 && !session_.isInGuild());
        const char* label = pp::cs(kTabText[k - 1]);
        const float w = std::max(60.0f, static_cast<float>(fs.measure(label)) + 2.0f * cap + 4.0f);
        const float ty = y - (sel ? 5.0f * kSY : 0.0f);
        const pp::Texture& t = tabArt_[sel ? 0 : 1];
        if (t.pixels) {
            const float tw = static_cast<float>(t.uw), th = static_cast<float>(t.uh), ct = tw * 20.0f / 128.0f;
            ui.image(t, Rect{x, ty, cap, h}, SrcRect{0, 0, ct, th});
            ui.image(t, Rect{x + cap, ty, w - 2 * cap, h}, SrcRect{ct, 0, tw - 2 * ct, th});
            ui.image(t, Rect{x + w - cap, ty, cap, h}, SrcRect{tw - ct, 0, ct, th});
        }
        const Rect hit{x + 4, ty + 2, w - 8, h - 4};
        const uint32_t id = kIdSfTab1 + static_cast<uint32_t>(k - 1);
        const bool f = focusable && enabled && !sel && winFocus_.addStop(id, hit);
        ui.textShadow(fs, x + w * 0.5f, ty + (h - static_cast<float>(fs.lineHeight())) * 0.5f - 1.0f, label,
                      !enabled ? kColDisable : sel ? kColWhite : kColGold, Align::Center);
        if (f) {
            ui.fill(hit, 0x30FFFFFFu);
            const char* tip = k == 1 ? CLIENT_STR(NEWBIE_TOOLTIP_FRIENDSTAB) : k == 2 ? CLIENT_STR(NEWBIE_TOOLTIP_WHOTAB) : k == 3 ? CLIENT_STR(NEWBIE_TOOLTIP_GUILDTAB) : nullptr;
            if (tip) socialTip(ui, id, hit, label, tip);
        }
        x += w - 15.0f * kSX;
    }
}

bool WorldScreen::drawSocialWindow(UiRenderer& ui, uint8_t w) {
    switch (w) {
        case kWinFriends: return drawFriendsFrame(ui);
        case kWinGuildInvite: return drawGuildInvitePopup(ui);
        case kWinSocialConfirm: return drawSocialConfirm(ui);
        case kWinSocialDropDown: return drawSocialDropDown(ui);
        default: return false;
    }
}

bool WorldScreen::drawFriendsFrame(UiRenderer& ui) {
    PROF_SCOPE("social.frame");
    SocialUi& s = socialUi();
    loadSocialSmallArt();
    loadSocialFrameArt(s.tab == 2 ? 1 : s.tab == 3 ? 2 : s.tab == 4 ? 3 : 0);
    loadNpcArt(false);  // the close button, the panel buttons (UIPanelButtonTemplate), the scroll arrows
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fn = kit_.font(kFontNormal);
    if (s.frame.pixels) ui.image(s.frame, Rect{soc::kOx, soc::kOy, soc::kFrameW, soc::kFrameH});
    else kit_.panel(ui, Rect{soc::kOx, soc::kOy, soc::kFrameW, soc::kFrameH});
    // FriendsFrameTitleText (TOP 3,-16, 250 x 16): FRIENDS_LIST / IGNORE_LIST / WHO_LIST / GUILD_TITLE_TEMPLATE.
    std::string title;
    if (s.tab == 1) title = s.header == 2 ? CLIENT_STR(IGNORE_LIST) : CLIENT_STR(FRIENDS_LIST);
    else if (s.tab == 2) title = CLIENT_STR(WHO_LIST);
    else if (s.tab == 4) title = CLIENT_STR(CHAT_CHANNELS);
    else if (s.tab == 3) {
        const so::GuildQuery* g = session_.ownGuild();
        if (g) {
            const uint32_t r = session_.guildRankIndex();
            title = fmt(CLIENT_STR(GUILD_TITLE_TEMPLATE), {r < so::kGuildRanks ? g->ranks[r] : std::string(), g->name});
        }
    }
    ui.textShadow(fn, PX(195), PY(24) - static_cast<float>(fn.lineHeight()) * 0.5f,
                  fitText(fn, title, 250.0f * kSX).c_str(), kColGold, Align::Center);
    // FriendsFrameCloseButton (TOPRIGHT -30,-8).
    {
        const Rect cr{PX(338) - 10, PY(24) - 10, 20, 20};
        const bool f = focusable && winFocus_.addStop(kIdSfClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdSfClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    drawFriendsTabs(ui, s.tab, focusable);
    if (s.tab == 1) {
        // FriendsTabHeaderTab1 / 2 (TabButtonTemplate at TOPLEFT 30,-64; PanelTemplates_TabResize(tab, 0)).
        const Font& fs = kit_.font(kFontSmall);
        const float th = 32 * kSY, cap = 16 * kSX;
        float x = PX(30);
        for (int k = 1; k <= 2; ++k) {
            const bool sel = k == s.header;
            const char* label = k == 1 ? pp::cs(kTabText[0]) : CLIENT_STR(IGNORE);
            const float w = std::max(2.0f * cap, static_cast<float>(fs.measure(label)) + 2 * cap);
            const float ty = PY(64) + (sel ? 3 * kSY : 0.0f);
            const pp::Texture& t = sel ? s.htabOn : s.htabOff;
            if (t.pixels) {
                const float tw = static_cast<float>(t.uw), tth = static_cast<float>(t.uh), ct = tw * 0.25f;
                ui.image(t, Rect{x, ty, cap, th}, SrcRect{0, 0, ct, tth});
                ui.image(t, Rect{x + cap, ty, w - 2 * cap, th}, SrcRect{ct, 0, tw - 2 * ct, tth});
                ui.image(t, Rect{x + w - cap, ty, cap, th}, SrcRect{tw - ct, 0, ct, tth});
            }
            const Rect hit{x + 2, ty + 4, w - 4, th - 8};
            const uint32_t id = kIdSfHeader1 + static_cast<uint32_t>(k - 1);
            const bool f = focusable && !sel && winFocus_.addStop(id, hit);
            ui.textShadow(fs, x + w * 0.5f, ty + (th - static_cast<float>(fs.lineHeight())) * 0.5f, label,
                          sel ? kColWhite : kColGold, Align::Center);
            if (f) {
                ui.fill(hit, 0x30FFFFFFu);
                socialTip(ui, id, hit, label, k == 1 ? CLIENT_STR(NEWBIE_TOOLTIP_FRIENDSTAB) : CLIENT_STR(NEWBIE_TOOLTIP_IGNORETAB));
            }
            x += w;
        }
        // FriendsFrameStatusDropDown (TOPLEFT 56,-40, UIDropDownMenu_SetWidth 28): the status icon at LEFT 24,1.
        {
            const Rect box = R(56 + 10, 40 - 4, 28 + 29, 38);
            drawDropDownBox(ui, s.ddBox, box);
            const uint32_t pf = session_.selfField(game::fieldIndex(game::UF::PLAYER_FLAGS));
            const int status = (pf & 0x2u) ? 1 : (pf & 0x4u) ? 2 : 0;  // UnitIsAFK / UnitIsDND
            if (s.status.pixels)
                ui.image(s.status, R(56 + 24, 40 + 16 - 8 - 1, 16, 16),
                         SrcRect{static_cast<float>(16 * (status == 1 ? 1 : status == 2 ? 2 : 0)), 0, 16, 16});
            const Rect btn = R(56 + 10 + 28, 40 + 1, 24, 24);
            const bool f = focusable && winFocus_.addStop(kIdSfStatus, box);
            const pp::Texture& bt = (f && winFocus_.isPressed(kIdSfStatus)) || s.dropKind == 1 ? s.ddBtnDn : s.ddBtn;
            if (bt.pixels) ui.image(bt, btn);
            if (f) {
                if (s.ddHl.pixels) ui.image(s.ddHl, btn, 0xFFFFFFFFu, Blend::Add);
                socialTip(ui, kIdSfStatus, box,
                          pp::text::uiPlain(CLIENT_FMT(FRIENDS_LIST_STATUS_TOOLTIP, pp::cs(kStatusText[status]))),
                          std::string());
            }
            if (s.dropKind == 1) s.dropAnchor = Rect{box.x + ui.offsetX(), box.y + box.h + ui.offsetY(), box.w, 0};
        }
        if (s.header == 1) drawFriendsList(ui, focusable);
        else drawIgnoreList(ui, focusable);
    } else if (s.tab == 2) {
        drawWhoFrame(ui, focusable);
    } else if (s.tab == 3) {
        drawGuildFrame(ui, focusable);
        drawGuildSideFrame(ui, focusable);
    } else if (s.tab == 4) {
        drawChannelFrame(ui, focusable);  // ChannelFrame
    }
    return true;
}

// --- Friends (FriendsList_Update / FriendsFrame_SetButton) -----------------------------------------------------------

void WorldScreen::drawFriendsList(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const auto& friends = session_.friends();
    // The buttons: online friends, the offline header (both kinds present), offline friends.
    struct Row {
        int index;  // into friends(), -1 = the header
    };
    std::vector<Row> rows;
    int online = 0;
    for (const auto& f : friends) online += f.online() ? 1 : 0;
    const int offline = static_cast<int>(friends.size()) - online;
    for (int i = 0; i < static_cast<int>(friends.size()); ++i)
        if (friends[static_cast<size_t>(i)].online()) rows.push_back(Row{i});
    if (online > 0 && offline > 0) rows.push_back(Row{-1});
    for (int i = 0; i < static_cast<int>(friends.size()); ++i)
        if (!friends[static_cast<size_t>(i)].online()) rows.push_back(Row{i});
    // The selection: the first friend when none (FriendsFrame_SelectFriend(…, 1)).
    bool selFound = false;
    for (const auto& f : friends) selFound = selFound || f.guid == s.selectedFriend;
    if (!selFound) s.selectedFriend = rows.empty() ? 0 : friends[static_cast<size_t>(rows[0].index >= 0 ? rows[0].index : 0)].guid;
    const pp::WorldSession::FriendInfo* sel = nullptr;
    for (const auto& f : friends)
        if (f.guid == s.selectedFriend) sel = &f;
    // FriendsFrameFriendsScrollFrame (TOPLEFT 21,-100, 302 x 307): 34-unit buttons, the header 16.
    constexpr float kTop = 100.0f, kBottom = 100.0f + 307.0f;
    const int total = static_cast<int>(rows.size());
    s.friendOffset = std::clamp(s.friendOffset, 0, std::max(0, total - 1));
    float y = kTop;
    int shown = 0;
    for (int r = s.friendOffset; r < total; ++r) {
        const float h = rows[static_cast<size_t>(r)].index < 0 ? 16.0f : 34.0f;
        if (y + h > kBottom + 0.5f) break;
        if (rows[static_cast<size_t>(r)].index < 0) {  // FriendsFrameOfflineHeader (UI-FriendsFrame-OnlineDivider)
            if (s.divider.pixels) ui.image(s.divider, R(21, y, 298, 16));
            y += h;
            continue;
        }
        const auto& f = friends[static_cast<size_t>(rows[static_cast<size_t>(r)].index)];
        const Rect b = R(21, y, 298, 34);
        const uint32_t id = kIdSfFriend0 + static_cast<uint32_t>(shown < 16 ? shown : 15);
        const bool fc = focusable && shown < 16 && winFocus_.addStop(id, b);
        ++shown;
        // Background (TOPLEFT 0,0 - BOTTOMRIGHT 0,2), the status icon (TOPLEFT 4,-2).
        ui.fill(R(21, y, 298, 32), f.online() ? kBgOnline : kBgOffline);
        const int icon = !f.online() ? 3 : (f.status & 4) ? 2 : (f.status & 2) ? 1 : 0;  // DND before AFK (GetFriendInfo)
        if (s.status.pixels)
            ui.image(s.status, R(21 + 4, y + 2, 16, 16), SrcRect{static_cast<float>(16 * icon), 0, 16, 16});
        std::string name = session_.socialName(f.guid);
        std::string nameText = name, info;
        if (f.online()) {
            const uint8_t g = session_.socialGender(f.guid);
            std::string cls = socialClassName(static_cast<uint8_t>(f.cls), g == 1);
            if (cls.empty()) cls = CLIENT_STR(UNKNOWN);
            nameText = name + ", " + fmt(CLIENT_STR(FRIENDS_LEVEL_TEMPLATE), {std::to_string(f.level), cls});
        }
        info = socialZoneName(f.area);  // GetFriendInfo: UNKNOWN without an AreaTable row (an offline friend: area 0)
        ui.textShadow(fs, PX(21 + 20), PY(y + 3), fitText(fs, nameText, 248 * kSX).c_str(),
                      f.online() ? kColFriendName : kColFriendGray);
        ui.textShadow(ft, PX(21 + 20), PY(y + 3) + static_cast<float>(fs.lineHeight()), fitText(ft, info, 260 * kSX).c_str(),
                      kColFriendGray);
        if ((fc || f.guid == s.selectedFriend) && s.rowhl.pixels) ui.image(s.rowhl, b, 0xFFFFFFFFu, Blend::Add);
        y += h;
    }
    // The scroll bar's arrows (UIPanelScrollBarTemplate at TOPRIGHT -47,-115 .. 15 above the bottom).
    if (total > 0 && (s.friendOffset > 0 || y > kBottom - 34.0f || shown < total - (online > 0 && offline > 0 ? 1 : 0))) {
        const Rect up = R(384 - 47 - 16, 115 - 16, 16, 16), dn = R(384 - 47 - 16, kBottom - 15, 16, 16);
        const bool canUp = s.friendOffset > 0, canDown = s.friendOffset + shown + (online > 0 && offline > 0 ? 1 : 0) < total;
        const bool fu = focusable && canUp && winFocus_.addStop(kIdSfFriendUp, up);
        const bool fd = focusable && canDown && winFocus_.addStop(kIdSfFriendDown, dn);
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDown ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (fu) ui.fill(up, 0x40FFFFFFu);
        if (fd) ui.fill(dn, 0x40FFFFFFu);
    }
    // FriendsFrameAddFriendButton (BOTTOMLEFT 15,81), FriendsFrameSendMessageButton (BOTTOMRIGHT -41,81, the selected
    // friend online).
    const Rect add = R(15, 512 - 81 - 21, 131, 21), msg = R(384 - 41 - 131, 512 - 81 - 21, 131, 21);
    drawPanelButton(ui, kIdSfAddFriend, add, CLIENT_STR(ADD_FRIEND), true, focusable);
    drawPanelButton(ui, kIdSfSendMessage, msg, CLIENT_STR(SEND_MESSAGE), sel && sel->online(), focusable);
    if (focusable && winFocus_.focused() == kIdSfAddFriend) socialTip(ui, kIdSfAddFriend, add, CLIENT_STR(ADD_FRIEND), CLIENT_STR(NEWBIE_TOOLTIP_ADDFRIEND));
    if (focusable && winFocus_.focused() == kIdSfSendMessage) socialTip(ui, kIdSfSendMessage, msg, CLIENT_STR(SEND_MESSAGE), CLIENT_STR(NEWBIE_TOOLTIP_SENDMESSAGE));
}

// --- Ignore (IgnoreList_Update) --------------------------------------------------------------------------------------

void WorldScreen::drawIgnoreList(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    const Font& fs = kit_.font(kFontSmall);
    const auto& ignores = session_.ignores();
    const int n = static_cast<int>(ignores.size());
    const int header = n > 0 ? 1 : 0;  // FriendsFrameIgnoredHeader at the first button
    const int entries = n + header;
    if (std::find(ignores.begin(), ignores.end(), s.selectedIgnore) == ignores.end())
        s.selectedIgnore = n > 0 ? ignores[0] : 0;  // FriendsFrame_SelectSquelched(SQUELCH_TYPE_IGNORE, 1)
    constexpr int kRows = 19;  // IGNORES_TO_DISPLAY, FRIENDS_FRAME_IGNORE_HEIGHT 16
    s.ignoreOffset = std::clamp(s.ignoreOffset, 0, std::max(0, entries - kRows));
    for (int i = 0; i < kRows; ++i) {
        const int idx = i + s.ignoreOffset + 1;  // squelchedIndex
        if (idx > entries) break;
        const Rect b = R(21, 102 + i * 16.0f, 298, 16);
        if (idx == header) {  // IgnoreList_SetHeader: GameFontHighlightLeft at LEFT 5,1
            ui.textShadow(fs, PX(21 + 5), b.y + (b.h - static_cast<float>(fs.lineHeight())) * 0.5f, CLIENT_STR(IGNORED), kColWhite);
            continue;
        }
        const uint64_t g = ignores[static_cast<size_t>(idx - header - 1)];
        const uint32_t id = kIdSfIgnore0 + static_cast<uint32_t>(i);
        const bool f = focusable && winFocus_.addStop(id, b);
        std::string name = session_.socialName(g);
        if (name.empty()) name = CLIENT_STR(UNKNOWN);  // GetIgnoreName: UNKNOWN until the name query answers
        ui.textShadow(fs, PX(21 + 10), b.y + (b.h - static_cast<float>(fs.lineHeight())) * 0.5f,
                      fitText(fs, name, 288 * kSX).c_str(), kColGold);
        if ((f || g == s.selectedIgnore) && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, b, 0xFFFFFFFFu, Blend::Add);
    }
    // FriendsFrameIgnoreScrollFrame (FauxScrollFrame, TOPRIGHT -65,-99, 298 x 309): the arrows when it scrolls.
    if (entries > kRows) {
        const Rect up = R(384 - 65 + 6, 99, 16, 16), dn = R(384 - 65 + 6, 99 + 309 - 16, 16, 16);
        const bool canUp = s.ignoreOffset > 0, canDown = s.ignoreOffset + kRows < entries;
        const bool fu = focusable && canUp && winFocus_.addStop(kIdSfIgnoreUp, up);
        const bool fd = focusable && canDown && winFocus_.addStop(kIdSfIgnoreDown, dn);
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDown ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (fu) ui.fill(up, 0x40FFFFFFu);
        if (fd) ui.fill(dn, 0x40FFFFFFu);
    }
    // FriendsFrameIgnorePlayerButton (BOTTOMLEFT 15,81, 131), FriendsFrameUnsquelchButton (BOTTOMRIGHT -38,81, 134;
    // voice chat off: no mute button).
    const Rect add = R(15, 512 - 81 - 21, 131, 21), del = R(384 - 38 - 134, 512 - 81 - 21, 134, 21);
    drawPanelButton(ui, kIdSfIgnorePlayer, add, CLIENT_STR(IGNORE_PLAYER), true, focusable);
    drawPanelButton(ui, kIdSfUnsquelch, del, CLIENT_STR(REMOVE_PLAYER), s.selectedIgnore != 0, focusable);
    if (focusable && winFocus_.focused() == kIdSfIgnorePlayer) socialTip(ui, kIdSfIgnorePlayer, add, CLIENT_STR(IGNORE_PLAYER), CLIENT_STR(NEWBIE_TOOLTIP_IGNOREPLAYER));
    if (focusable && winFocus_.focused() == kIdSfUnsquelch) socialTip(ui, kIdSfUnsquelch, del, CLIENT_STR(REMOVE_PLAYER), CLIENT_STR(NEWBIE_TOOLTIP_REMOVEPLAYER));
}

// --- Who (WhoList_Update) --------------------------------------------------------------------------------------------

void WorldScreen::drawWhoFrame(UiRenderer& ui, bool focusable) {
    SocialUi& s = socialUi();
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const auto& list = session_.whoList();
    const int num = static_cast<int>(list.size());
    constexpr int kRows = 17;  // WHOS_TO_DISPLAY, FRIENDS_FRAME_WHO_HEIGHT 16
    const bool scroll = num > kRows;
    s.whoOffset = std::clamp(s.whoOffset, 0, std::max(0, num - kRows));
    // The column headers (WhoFrameColumnHeaderTemplate, WhoFrame-ColumnTabs): NAME 83, the dropdown column 105
    // (120 without the scroll bar), LEVEL_ABBR 32, CLASS 92; TOPLEFT 20,-70, each 2 left of the previous one's right.
    const float colW[4] = {83.0f, scroll ? 105.0f : 120.0f, 32.0f, 92.0f};
    const char* colText[4] = {CLIENT_STR(NAME), nullptr, CLIENT_STR(LEVEL_ABBR), CLIENT_STR(CLASS)};
    const char* const dropText[3] = {CLIENT_STR(ZONE), pp::cs(kTabText[2]), CLIENT_STR(RACE)};  // WHOFRAME_DROPDOWN_LIST: ZONE, GUILD, RACE
    float cx = 20.0f;
    for (int c = 0; c < 4; ++c) {
        const Rect hr = R(cx, 70, colW[c], 24);
        if (s.coltab.pixels) {  // Left 5 (0..0.078), Middle (0.078..0.906), Right 4 (0.906..0.969) x 0..0.75
            const float tw = static_cast<float>(s.coltab.uw), th = static_cast<float>(s.coltab.uh) * 0.75f;
            ui.image(s.coltab, R(cx, 70, 5, 24), SrcRect{0, 0, tw * 0.078125f, th});
            ui.image(s.coltab, R(cx + 5, 70, colW[c] - 9, 24), SrcRect{tw * 0.078125f, 0, tw * 0.828125f, th});
            ui.image(s.coltab, R(cx + colW[c] - 4, 70, 4, 24), SrcRect{tw * 0.90625f, 0, tw * 0.0625f, th});
        }
        const uint32_t id = c == 1 ? kIdSfWhoDrop : kIdSfWhoCol0 + static_cast<uint32_t>(c);
        const bool f = focusable && winFocus_.addStop(id, hr);
        if (f && s.colhl.pixels) ui.image(s.colhl, hr, 0xFFFFFFFFu, Blend::Add);
        const char* label = c == 1 ? dropText[std::clamp(s.whoColumn, 1, 3) - 1] : colText[c];
        ui.textShadow(ft, hr.x + 8 * kSX, hr.y + (hr.h - static_cast<float>(ft.lineHeight())) * 0.5f, label, kColWhite);
        if (c == 1) {  // WhoFrameDropDown's arrow (UI-ChatIcon-ScrollDown) at the column's right
            const Rect btn{hr.x + hr.w - 12.0f, hr.y + (hr.h - 12.0f) * 0.5f, 12.0f, 12.0f};
            const pp::Texture& bt = s.dropKind == 2 ? s.ddBtnDn : s.ddBtn;
            if (bt.pixels) ui.image(bt, btn);
            if (s.dropKind == 2) s.dropAnchor = Rect{hr.x + ui.offsetX(), hr.y + hr.h + ui.offsetY(), hr.w, 0};
        }
        cx += colW[c] - 2.0f;
    }
    // WhoFrameButton1..17 (TOPLEFT 15,-95, 298 x 16): Name 88 (gold), the dropdown's column 95 / 110, Level 20
    // (centred), Class 80 in the class colour.
    for (int i = 0; i < kRows; ++i) {
        const int idx = s.whoOffset + i + 1;
        if (idx > num) break;
        const auto& w = list[static_cast<size_t>(idx - 1)];
        const Rect b = R(15, 95 + i * 16.0f, 298, 16);
        const uint32_t id = kIdSfWho0 + static_cast<uint32_t>(i);
        const bool f = focusable && winFocus_.addStop(id, b);
        const float ty = b.y + (b.h - static_cast<float>(ft.lineHeight())) * 0.5f;
        float x = 15 + 10;
        ui.textShadow(ft, PX(x), ty, fitText(ft, w.name, 88 * kSX).c_str(), kColGold);
        x += 88;
        const bool female = w.gender == 1;
        std::string var;
        if (s.whoColumn == 2) var = w.guild;
        else if (s.whoColumn == 3) var = socialRaceName(static_cast<uint8_t>(w.race), female);
        else var = socialZoneName(w.zone);
        const float varW = scroll ? 95.0f : 110.0f;
        ui.textShadow(ft, PX(x), ty, fitText(ft, var, varW * kSX).c_str(), kColWhite);
        x += varW + 2;
        ui.textShadow(ft, PX(x + 10), ty, std::to_string(w.level).c_str(), kColWhite, Align::Center);
        x += 20 + 12;
        const uint32_t cc = classColor(w.cls);
        ui.textShadow(ft, PX(x), ty, fitText(ft, socialClassName(static_cast<uint8_t>(w.cls), female), 80 * kSX).c_str(),
                      cc ? cc : kColWhite);
        if ((f || idx == s.selectedWho) && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, b, 0xFFFFFFFFu, Blend::Add);
    }
    // WhoListScrollFrame (TOPRIGHT -67,-96, 296 x 287) and its bar (only while it scrolls).
    if (scroll) {
        if (s.whoScroll.pixels) ui.image(s.whoScroll, R(315, 91, 31, 294));
        const Rect up = R(384 - 67 + 6, 96, 16, 16), dn = R(384 - 67 + 6, 96 + 287 - 16, 16, 16);
        const bool canUp = s.whoOffset > 0, canDown = s.whoOffset + kRows < num;
        const bool fu = focusable && canUp && winFocus_.addStop(kIdSfWhoUp, up);
        const bool fd = focusable && canDown && winFocus_.addStop(kIdSfWhoDown, dn);
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDown ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (fu) ui.fill(up, 0x40FFFFFFu);
        if (fd) ui.fill(dn, 0x40FFFFFFu);
    }
    // WhoFrameTotals (BOTTOM -10,127): WHO_FRAME_TOTAL_TEMPLATE + WHO_FRAME_SHOWN_TEMPLATE above 50.
    {
        std::string totals = pp::text::plural(fmt(CLIENT_STR(WHO_FRAME_TOTAL_TEMPLATE), {std::to_string(session_.whoTotal())}));
        if (session_.whoTotal() > so::kMaxWho) totals += "  " + fmt(CLIENT_STR(WHO_FRAME_SHOWN_TEMPLATE), {std::to_string(so::kMaxWho)});
        ui.textShadow(ft, PX(192 - 10), PY(512 - 127 - 16) + (16 * kSY - static_cast<float>(ft.lineHeight())) * 0.5f,
                      totals.c_str(), kColGold, Align::Center);
    }
    // WhoFrameEditBox (BOTTOM -20,100, 296 x 32, ChatFontNormal): Cross = the keyboard (Enter = SendWho).
    {
        const Rect eb = R(192 - 20 - 148, 512 - 100 - 32 + 6, 296, 20);
        if (s.input.pixels) {
            const float tw = static_cast<float>(s.input.uw), th = static_cast<float>(s.input.uh), ct = tw * 8.0f / 128.0f;
            ui.image(s.input, Rect{eb.x, eb.y, 4, eb.h}, SrcRect{0, 0, ct, th});
            ui.image(s.input, Rect{eb.x + 4, eb.y, eb.w - 8, eb.h}, SrcRect{ct, 0, tw - 2 * ct, th});
            ui.image(s.input, Rect{eb.x + eb.w - 4, eb.y, 4, eb.h}, SrcRect{tw - ct, 0, ct, th});
        } else {
            ui.fill(eb, 0x80000000u);
        }
        const bool f = focusable && winFocus_.addStop(kIdSfWhoEdit, eb);
        ui.textShadow(fs, eb.x + 4, eb.y + (eb.h - static_cast<float>(fs.lineHeight())) * 0.5f,
                      fitText(fs, s.whoText, eb.w - 8).c_str(), kColWhite);
        if (f) ui.fill(eb, 0x20FFFFFFu);
    }
    // WhoFrameGroupInviteButton (BOTTOMRIGHT -40,82, 120 x 22), WhoFrameAddFriendButton left of it, WhoFrameWhoButton
    // (85) left of that; the first two need a selection.
    const Rect inv = R(384 - 40 - 120, 512 - 82 - 22, 120, 22), add = R(384 - 40 - 240, 512 - 82 - 22, 120, 22),
               ref = R(384 - 40 - 240 - 85, 512 - 82 - 22, 85, 22);
    const bool selected = s.selectedWho > 0 && s.selectedWho <= num;
    drawPanelButton(ui, kIdSfWhoRefresh, ref, CLIENT_STR(REFRESH), true, focusable);
    drawPanelButton(ui, kIdSfWhoAddFriend, add, CLIENT_STR(ADD_FRIEND), selected, focusable);
    drawPanelButton(ui, kIdSfWhoInvite, inv, CLIENT_STR(GROUP_INVITE), selected, focusable);
}

// --- Popups ----------------------------------------------------------------------------------------------------------

bool WorldScreen::drawGuildInvitePopup(UiRenderer& ui) {
    SocialUi& s = socialUi();
    StaticPopup sp;
    sp.text = s.inviteText.c_str();
    sp.button1 = CLIENT_STR(ACCEPT);
    sp.button1Id = kIdSfPopup1;
    sp.button2 = CLIENT_STR(DECLINE);
    sp.button2Id = kIdSfPopup2;
    sp.focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    drawStaticPopup(ui, sp);
    return true;
}

bool WorldScreen::drawSocialConfirm(UiRenderer& ui) {
    SocialUi& s = socialUi();
    StaticPopup sp;
    sp.text = s.confirmText.c_str();
    const bool yesNo = s.confirm == 3 || s.confirm == 4;  // CONFIRM_GUILD_DISBAND / REMOVE_GUILDMEMBER: YES / NO
    sp.button1 = yesNo ? CLIENT_STR(YES) : CLIENT_STR(ACCEPT);
    sp.button1Id = kIdSfConfirm1;
    sp.button2 = yesNo ? CLIENT_STR(NO) : CLIENT_STR(CANCEL);
    sp.button2Id = kIdSfConfirm2;
    sp.focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    drawStaticPopup(ui, sp);
    return true;
}

void WorldScreen::socialConfirm(int kind, const std::string& arg) {
    SocialUi& s = socialUi();
    s.confirm = kind;
    s.confirmArg = arg;
    switch (kind) {
        case 1: s.confirmText = fmt(CLIENT_STR(CONFIRM_GUILD_LEAVE), {arg}); break;
        case 2: s.confirmText = stripNameDeclension(fmt(CLIENT_STR(CONFIRM_GUILD_PROMOTE), {arg})); break;
        case 3: s.confirmText = CLIENT_STR(CONFIRM_GUILD_DISBAND); break;
        case 4: s.confirmText = stripNameDeclension(fmt(CLIENT_STR(REMOVE_GUILDMEMBER_LABEL), {arg})); break;
        default: s.confirm = 0; return;
    }
    openWindow(kWinSocialConfirm);
    placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), kWinSocialConfirm), placeNext_.end());
    winFocus_.setFocus(kIdSfConfirm1);
}

// A StaticPopup with an edit box: PSP - the system keyboard, its title the popup's text, the text it starts with
// what OnShow puts in the box; OK = EditBoxOnEnterPressed (the popup's OnAccept), cancel = EditBoxOnEscapePressed.
void WorldScreen::socialEditPopup(int field, const std::string& arg) {
    if (pp::oskActive()) return;
    SocialUi& s = socialUi();
    std::string title, initial;
    int max = 77;
    pp::OskCharset cs = pp::OskCharset::Any;
    switch (field) {
        case kOskAddFriend: title = CLIENT_STR(ADD_FRIEND_LABEL); break;          // maxLetters 12 + 1 + 64
        case kOskAddIgnore: title = CLIENT_STR(ADD_IGNORE_LABEL); break;
        case kOskAddGuildMember: title = CLIENT_STR(ADD_GUILDMEMBER_LABEL); max = 12; break;
        case kOskFriendNote:
            title = stripNameDeclension(fmt(CLIENT_STR(SET_FRIENDNOTE_LABEL), {arg}));
            if (const auto* f = session_.friendByName(arg)) initial = f->note;
            max = 48;
            cs = pp::OskCharset::Text;
            break;
        case kOskGuildNote:
        case kOskOfficerNote:
            title = field == kOskGuildNote ? CLIENT_STR(SET_GUILDPLAYERNOTE_LABEL) : CLIENT_STR(SET_GUILDOFFICERNOTE_LABEL);
            for (const auto& m : session_.guildRoster())
                if (m.guid == s.selectedMember) initial = field == kOskGuildNote ? m.note : m.officerNote;
            max = 31;
            cs = pp::OskCharset::Text;
            break;
        case kOskMotd: title = CLIENT_STR(SET_GUILDMOTD_LABEL); initial = session_.guildMotd(); max = 128; cs = pp::OskCharset::Text; break;
        case kOskAddRank: title = CLIENT_STR(ADD_GUILDRANK_LABEL); max = 15; cs = pp::OskCharset::Text; break;
        case kOskWho: title = CLIENT_STR(WHO_LIST); initial = s.whoText; max = 127; cs = pp::OskCharset::Text; break;
        case kOskGuildInfo: title = CLIENT_STR(GUILD_INFORMATION); initial = s.infoText; max = 500; cs = pp::OskCharset::Text; break;
        case kOskRankName: title = CLIENT_STR(GUILDCONTROL_RANKLABEL); initial = s.ctl.name; max = 15; cs = pp::OskCharset::Text; break;
        case kOskRankGold:
            title = CLIENT_STR(GOLD_PER_DAY);
            initial = std::to_string(s.ctl.gold);
            max = 9;
            cs = pp::OskCharset::Digits;
            break;
        case kOskRankWithdraw:
            title = CLIENT_STR(GUILDCONTROL_WITHDRAW_ITEMS);
            max = 6;
            cs = pp::OskCharset::Digits;
            break;
        default: return;
    }
    s.oskField = field;
    s.oskArg = arg;
    if (!pp::oskBeginFor(kOskOwner, title, initial, max, cs)) {
        s.oskField = 0;
        return;
    }
    PSP_LOGI(pp::tag::Ctrl, "social_osk field=%d", field);
}

void WorldScreen::socialOskTick() {
    if (!soc_ || soc_->oskField == 0) return;
    SocialUi& s = *soc_;
    bool accepted = false;
    std::string text;
    if (!pp::oskFinishedFor(kOskOwner, accepted, text)) return;
    const int field = s.oskField;
    s.oskField = 0;
    PSP_LOGI(pp::tag::Ctrl, "social_osk_done field=%d ok=%d len=%u", field, accepted ? 1 : 0, static_cast<unsigned>(text.size()));
    if (!accepted) return;  // EditBoxOnEscapePressed: the popup hides
    switch (field) {
        case kOskAddFriend: session_.addFriend(text); break;          // AddFriend(self.editBox:GetText())
        case kOskAddIgnore: session_.addIgnore(text); break;          // AddIgnore
        case kOskAddGuildMember: session_.guildInvite(text); break;   // GuildInvite
        case kOskFriendNote: session_.setFriendNotes(s.oskArg, text); break;  // SetFriendNotes(FriendsFrame.NotesID, ...)
        case kOskGuildNote: session_.guildSetPublicNote(s.selectedName, text); break;
        case kOskOfficerNote: session_.guildSetOfficerNote(s.selectedName, text); break;
        case kOskMotd: session_.guildSetMotd(text); break;            // GuildSetMOTD
        case kOskAddRank:                                              // GuildControlAddRank + the dropdown again
            session_.guildAddRank(text);
            guildControlLoadRank(s.ctl.rank);
            break;
        case kOskWho:  // WhoFrameEditBox_OnEnterPressed: SendWho(text)
            s.whoText = text;
            session_.sendWho(text);
            break;
        case kOskGuildInfo:
            s.infoText = text;
            s.infoEdited = true;
            break;
        case kOskRankName:
            s.ctl.name = so::capText(text, so::kRankNameChars, false);
            s.ctl.acceptEnabled = true;
            break;
        case kOskRankGold:
            s.ctl.gold = static_cast<uint32_t>(std::min<long>(std::max<long>(std::strtol(text.c_str(), nullptr, 10), 0), 1000000000));
            s.ctl.goldChanged = true;
            s.ctl.acceptEnabled = true;
            break;
        case kOskRankWithdraw: {
            const int t = std::clamp(s.ctl.bankTab, 1, 6) - 1;
            s.ctl.pending[t].withdraw = static_cast<int>(std::min<long>(std::max<long>(std::strtol(text.c_str(), nullptr, 10), 0), 100000));
            s.ctl.acceptEnabled = true;
            break;
        }
        default: break;
    }
}

// --- The dropdown lists (kWinSocialDropDown) ---------------------------------------------------------------------------

bool WorldScreen::drawSocialDropDown(UiRenderer& ui) {
    SocialUi& s = socialUi();
    if (!s.dropKind) return true;
    loadSocialSmallArt();
    loadNpcArt(false);
    const Font& f = kit_.font(kFontSmall);
    const bool focusable = uiMode_ && !settings_.isOpen();
    struct Item {
        std::string text;
        bool checked = false;
        int icon = -1;
    };
    std::vector<Item> items;
    if (s.dropKind == 1) {  // FriendsFrameStatusDropDown_Initialize: notCheckable, the status icon before the text
        for (int k = 0; k < 3; ++k) items.push_back(Item{pp::cs(kStatusText[k]), false, k});
    } else if (s.dropKind == 2) {  // WhoFrameDropDown_Initialize
        const char* const t[3] = {CLIENT_STR(ZONE), pp::cs(kTabText[2]), CLIENT_STR(RACE)};
        for (int k = 0; k < 3; ++k) items.push_back(Item{t[k], s.whoColumn == k + 1, -1});
    } else if (s.dropKind == 3) {  // GuildControlPopupFrameDropDown_Initialize: the ranks
        const uint32_t n = session_.guildNumRanks();
        const so::GuildQuery* g = session_.ownGuild();
        for (uint32_t r = 0; r < n; ++r)
            items.push_back(Item{g ? so::capText(g->ranks[r], so::kRankNameChars, true) : std::string(),
                                 s.ctl.rank == static_cast<int>(r) + 1, -1});
    }
    const float rowH = static_cast<float>(std::max(12, f.lineHeight()));
    float maxW = 0.0f;
    for (const Item& it : items) maxW = std::max(maxW, static_cast<float>(f.measure(it.text.c_str())) + (it.icon >= 0 ? 18.0f : 0.0f));
    const float listW = maxW + 34.0f, listH = rowH * static_cast<float>(items.size()) + 16.0f;
    Rect lr{s.dropAnchor.x, s.dropAnchor.y, listW, listH};
    lr.x = std::clamp(lr.x, 2.0f, static_cast<float>(pp::kScreenW) - listW - 2.0f);
    lr.y = std::clamp(lr.y, 2.0f, static_cast<float>(pp::kScreenH) - listH - 2.0f);
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{lr.x + 3, lr.y + 3, lr.w - 6, lr.h - 6}, 0xE0FFFFFFu);
    else ui.fill(lr, 0xE0301717u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), lr);
    for (size_t i = 0; i < items.size() && i < 16; ++i) {
        const Item& it = items[i];
        const Rect b{lr.x + 6, lr.y + 8 + rowH * static_cast<float>(i), lr.w - 12, rowH};
        const bool fc = focusable && winFocus_.addStop(kIdSfDrop0 + static_cast<uint32_t>(i), b);
        if (fc && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, b, 0xFFFFFFFFu, Blend::Add);
        float tx = b.x + 16.0f;
        if (it.checked && s.check.pixels) ui.image(s.check, Rect{b.x + 1, b.y + (rowH - 12) * 0.5f, 12, 12});
        if (it.icon >= 0 && s.status.pixels) {
            ui.image(s.status, Rect{tx, b.y + (rowH - 12) * 0.5f, 12, 12}, SrcRect{static_cast<float>(16 * it.icon), 0, 16, 16});
            tx += 16.0f;
        }
        ui.textShadow(f, tx, b.y + (rowH - static_cast<float>(f.lineHeight())) * 0.5f, it.text.c_str(), kColWhite);
    }
    return true;
}

bool WorldScreen::socialCloseDropDown() {
    if (!soc_ || !soc_->dropKind) return false;
    if (windowOpen(kWinSocialDropDown)) closeWindow(kWinSocialDropDown);
    soc_->dropKind = 0;
    return true;
}

void WorldScreen::socialWindowPad(const pp::PadState& pad) {
    if (!soc_ || !soc_->dropKind) return;
    // A click outside the open list closes it (as the UIDropDownMenu's own buttons are the only stops on it).
    if (!(pad.pressed & PSP_CTRL_CROSS)) return;
    auto r = winRect_.find(kWinSocialDropDown);
    if (r == winRect_.end() || r->second.contains(winFocus_.cursorX(), winFocus_.cursorY())) return;
    const uint32_t f = winFocus_.focused();
    if (f == kIdSfStatus || f == kIdSfWhoDrop || f == kIdSfCtlRank) return;  // the owner's button toggles it
    socialCloseDropDown();
}

bool WorldScreen::socialKeepsPopupArt() const {
    return windowOpen(kWinGuildInvite) || windowOpen(kWinSocialConfirm);
}

// --- Clicks ------------------------------------------------------------------------------------------------------------

bool WorldScreen::socialAction(uint32_t act) {
    if (act < 0x8400 || act > 0x86FF) return false;
    SocialUi& s = socialUi();
    // The tabs (also from the raid frame): PanelTemplates_Tab_OnClick + FriendsFrame_Update / ToggleFriendsFrame.
    if (act >= kIdSfTab1 && act < kIdSfTab1 + 5) {
        const int tab = static_cast<int>(act - kIdSfTab1) + 1;
        if (tab == 3 && !session_.isInGuild()) return true;  // tab 4 = ChannelFrame
        if (tab == s.tab) return true;
        const uint8_t from = windowOpen(kWinRaid) ? kWinRaid : kWinFriends;
        s.tab = tab;
        s.popup = 0;
        if (tab == 5 && from != kWinRaid) socialSwapFrame(kWinFriends, kWinRaid);
        else if (tab != 5 && from == kWinRaid) socialSwapFrame(kWinRaid, kWinFriends);
        socialFriendsFrameUpdate();
        playSoundByName("igCharacterInfoTab");
        PSP_LOGI(pp::tag::Ctrl, "friends_frame tab=%d", tab);
        return true;
    }
    if (act == kIdSfClose) {
        closeWindow(kWinFriends);
        playSoundByName("igMainMenuClose");  // FriendsFrame_OnHide
        return true;
    }
    if (act == kIdSfHeader1 || act == kIdSfHeader1 + 1) {  // FriendsTabHeaderTab: Tab_OnClick + FriendsFrame_Update
        s.header = act == kIdSfHeader1 ? 1 : 2;
        socialFriendsFrameUpdate();
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    if (act == kIdSfStatus || act == kIdSfWhoDrop) {  // ToggleDropDownMenu
        const int kind = act == kIdSfStatus ? 1 : 2;
        if (s.dropKind == kind) {
            socialCloseDropDown();
        } else {
            s.dropKind = kind;
            s.dropFocusMs = pp::logUptimeMs();
            openWindow(kWinSocialDropDown);
            placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), kWinSocialDropDown), placeNext_.end());
            winFocus_.setFocus(kIdSfDrop0);
            playSoundByName("igMainMenuOptionCheckBoxOn");
        }
        return true;
    }
    if (act >= kIdSfDrop0 && act < kIdSfDrop0 + 16) {
        const int i = static_cast<int>(act - kIdSfDrop0);
        if (s.dropKind == 1) {  // FriendsFrame_SetOnlineStatus: 1 available (clears AFK / DND), 2 AFK, 3 DND
            const uint32_t pf = session_.selfField(game::fieldIndex(game::UF::PLAYER_FLAGS));
            const int cur = (pf & 0x2u) ? 2 : (pf & 0x4u) ? 3 : 1;
            const int want = i + 1;
            if (want != cur) {
                if (want == 1) session_.sendAwayFlag((pf & 0x2u) == 0);  // the flag set now toggles off
                else session_.sendAwayFlag(want == 3);
            }
        } else if (s.dropKind == 2) {  // WhoFrameDropDownButton_OnClick: the column
            s.whoColumn = i + 1;
        } else if (s.dropKind == 3) {  // GuildControlPopupFrameDropDownButton_OnClick
            guildControlLoadRank(i + 1);
        }
        playSoundByName("UChatScrollButton");
        socialCloseDropDown();
        return true;
    }
    // Friends
    if (act >= kIdSfFriend0 && act < kIdSfFriend0 + 16) {  // FriendsFrameFriendButton_OnClick (LeftButton)
        const int k = static_cast<int>(act - kIdSfFriend0);
        const auto& friends = session_.friends();
        // The k-th friend button shown (the order drawFriendsList laid out).
        std::vector<int> order;
        for (int i = 0; i < static_cast<int>(friends.size()); ++i)
            if (friends[static_cast<size_t>(i)].online()) order.push_back(i);
        int online = static_cast<int>(order.size());
        if (online > 0 && online < static_cast<int>(friends.size())) order.push_back(-1);
        for (int i = 0; i < static_cast<int>(friends.size()); ++i)
            if (!friends[static_cast<size_t>(i)].online()) order.push_back(i);
        int seen = 0;
        for (size_t r = static_cast<size_t>(std::max(0, s.friendOffset)); r < order.size(); ++r) {
            if (order[r] < 0) continue;
            if (seen++ == k) {
                s.selectedFriend = friends[static_cast<size_t>(order[r])].guid;
                break;
            }
        }
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    if (act == kIdSfFriendUp) {
        s.friendOffset = std::max(0, s.friendOffset - 1);
        return true;
    }
    if (act == kIdSfFriendDown) {
        ++s.friendOffset;
        return true;
    }
    if (act == kIdSfAddFriend) {  // FriendsFrameAddFriendButton_OnClick
        const uint64_t t = session_.target();
        auto e = session_.entities().getEntity(t);
        const std::string tn = t ? session_.socialName(t) : std::string();
        if (e && e->getType() == game::ObjectType::PLAYER && t != session_.playerGuid() && !session_.isHostileUnit(t) &&
            !tn.empty() && !session_.friendByName(tn)) {
            session_.addFriend(tn);
            playSoundByName("UChatScrollButton");
        } else {
            socialEditPopup(kOskAddFriend, std::string());  // StaticPopup_Show("ADD_FRIEND") (BN off)
        }
        return true;
    }
    if (act == kIdSfSendMessage) {  // FriendsFrameSendMessageButton_OnClick: ChatFrame_SendTell(name)
        const std::string n = session_.socialName(s.selectedFriend);
        if (!n.empty()) {
            playSoundByName("igMainMenuOptionCheckBoxOn");
            chat_.openWhisper(n);
        }
        return true;
    }
    // Ignore
    if (act >= kIdSfIgnore0 && act < kIdSfIgnore0 + 19) {  // FriendsFrameIgnoreButton_OnClick
        const auto& ignores = session_.ignores();
        const int header = ignores.empty() ? 0 : 1;
        const int idx = static_cast<int>(act - kIdSfIgnore0) + s.ignoreOffset + 1 - header - 1;
        if (idx >= 0 && idx < static_cast<int>(ignores.size())) s.selectedIgnore = ignores[static_cast<size_t>(idx)];
        return true;
    }
    if (act == kIdSfIgnoreUp) {
        s.ignoreOffset = std::max(0, s.ignoreOffset - 1);
        return true;
    }
    if (act == kIdSfIgnoreDown) {
        ++s.ignoreOffset;
        return true;
    }
    if (act == kIdSfIgnorePlayer) {  // UnitCanCooperate(target): AddIgnore(UnitName("target")), else ADD_IGNORE
        const uint64_t t = session_.target();
        auto e = session_.entities().getEntity(t);
        const std::string tn = t ? session_.socialName(t) : std::string();
        if (e && e->getType() == game::ObjectType::PLAYER && !session_.isHostileUnit(t) && !tn.empty()) {
            session_.addIgnore(tn);
            playSoundByName("UChatScrollButton");
        } else {
            socialEditPopup(kOskAddIgnore, std::string());
        }
        return true;
    }
    if (act == kIdSfUnsquelch) {  // FriendsFrameUnsquelchButton_OnClick: DelIgnore(GetIgnoreName(GetSelectedIgnore()))
        const std::string n = session_.socialName(s.selectedIgnore);
        if (!n.empty()) session_.delIgnore(n);
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    // Who
    if (act >= kIdSfWho0 && act < kIdSfWho0 + 17) {  // FriendsFrameWhoButton_OnClick (LeftButton)
        const int idx = s.whoOffset + static_cast<int>(act - kIdSfWho0) + 1;
        const auto& list = session_.whoList();
        if (idx <= static_cast<int>(list.size())) {
            s.selectedWho = idx;
            s.selectedWhoName = list[static_cast<size_t>(idx - 1)].name;
        }
        return true;
    }
    if (act >= kIdSfWhoCol0 && act < kIdSfWhoCol0 + 4) {  // the column header: SortWho(sortType)
        const int c = static_cast<int>(act - kIdSfWhoCol0);
        session_.sortWho(c == 0 ? 4 : c == 2 ? 1 : 2);  // "name", "level", "class"
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    if (act == kIdSfWhoUp) {
        s.whoOffset = std::max(0, s.whoOffset - 1);
        return true;
    }
    if (act == kIdSfWhoDown) {
        ++s.whoOffset;
        return true;
    }
    if (act == kIdSfWhoEdit) {
        socialEditPopup(kOskWho, std::string());
        return true;
    }
    if (act == kIdSfWhoRefresh) {  // WhoFrameEditBox_OnEnterPressed; WhoFrame.selectedWho = nil
        session_.sendWho(s.whoText);
        s.selectedWho = 0;
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    if (act == kIdSfWhoAddFriend) {  // AddFriend(WhoFrame.selectedName)
        if (s.selectedWho > 0) session_.addFriend(s.selectedWhoName);
        return true;
    }
    if (act == kIdSfWhoInvite) {  // InviteUnit(WhoFrame.selectedName)
        if (s.selectedWho > 0) session_.inviteToGroup(s.selectedWhoName);
        return true;
    }
    // Popups
    if (act == kIdSfPopup1 || act == kIdSfPopup2) {  // GUILD_INVITE: AcceptGuild / DeclineGuild
        s.inviteAnswered = true;
        if (act == kIdSfPopup1) session_.acceptGuild();
        else session_.declineGuild();
        closeWindow(kWinGuildInvite);
        return true;
    }
    if (act == kIdSfConfirm1 || act == kIdSfConfirm2) {
        if (act == kIdSfConfirm1) {
            switch (s.confirm) {
                case 1: session_.guildLeave(); break;                    // CONFIRM_GUILD_LEAVE: GuildLeave()
                case 2: session_.guildSetLeader(s.confirmArg); break;    // CONFIRM_GUILD_PROMOTE: GuildSetLeader(name)
                case 3: session_.guildDisband(); break;                  // CONFIRM_GUILD_DISBAND: GuildDisband()
                case 4:                                                  // REMOVE_GUILDMEMBER: GuildUninvite, hide the detail
                    session_.guildUninvite(s.confirmArg);
                    if (s.popup == 1) s.popup = 0;
                    break;
                default: break;
            }
        }
        s.confirm = 0;
        closeWindow(kWinSocialConfirm);
        return true;
    }
    return guildAction(act, false);
}

// Circle (the right click): a friend / who / guild row - FriendsFrame_ShowDropdown (the FRIEND unit menu at the cursor).
bool WorldScreen::socialAltAction(uint32_t alt) {
    if (alt < 0x8400 || alt > 0x86FF) return false;
    SocialUi& s = socialUi();
    const float cx = winFocus_.cursorX(), cy = winFocus_.cursorY();
    if (alt >= kIdSfFriend0 && alt < kIdSfFriend0 + 16) {  // RightButton: the friend's menu (connected / offline)
        socialAction(alt);  // the row under it becomes the selection first (as the left click)
        for (const auto& f : session_.friends()) {
            if (f.guid != s.selectedFriend) continue;
            const std::string n = session_.socialName(f.guid);
            if (n.empty()) break;
            const bool guildShown = windowOpen(kWinFriends) && s.tab == 3;
            const uint8_t menu = static_cast<uint8_t>(kUnitMenuFriend | (f.online() ? 0 : 1) | 2 | (guildShown ? 4 : 0));
            openNamedUnitPopup(f.guid, n, menu, cx, cy, alt);
            break;
        }
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    if (alt >= kIdSfWho0 && alt < kIdSfWho0 + 17) {  // FriendsFrame_ShowDropdown(name, 1)
        const int idx = s.whoOffset + static_cast<int>(alt - kIdSfWho0) + 1;
        const auto& list = session_.whoList();
        if (idx <= static_cast<int>(list.size()))
            openNamedUnitPopup(0, list[static_cast<size_t>(idx - 1)].name, kUnitMenuFriend, cx, cy, alt);
        return true;
    }
    if (alt >= kIdSfGuild0 && alt < kIdSfGuild0 + 13) return guildAction(alt, true);
    return socialAction(alt);
}

// The unit menus' social buttons (ui_world_party.cpp UnitPopup_OnClick).
void WorldScreen::socialUnitMenuAction(uint8_t btn, uint64_t unit, const std::string& name) {
    switch (btn) {
        case kSocBtnGuildInvite: session_.guildInvite(name); break;  // PSP: GuildInvite(UnitName(unit)) (/ginvite)
        case kSocBtnTarget: {  // TargetUnit(fullname, 1): the nearest player of that name in sight
            uint64_t found = unit;
            if (found == 0)
                for (const auto& [guid, e] : session_.entities().getEntities())
                    if (e->getType() == game::ObjectType::PLAYER && so::sameName(session_.socialName(guid), name)) found = guid;
            if (found != 0 && session_.entities().getEntity(found)) session_.setTarget(found);
            break;
        }
        case kSocBtnSetNote:  // FriendsFrame.NotesID = name; StaticPopup_Show("SET_FRIENDNOTE", name)
            socialEditPopup(kOskFriendNote, name);
            playSoundByName("igCharacterInfoClose");
            break;
        case kSocBtnIgnore: session_.addOrDelIgnore(name); break;      // AddOrDelIgnore(fullname)
        case kSocBtnRemoveFriend: session_.removeFriend(name); break;  // RemoveFriend(name)
        case kSocBtnGuildPromote: socialConfirm(2, name); break;       // CONFIRM_GUILD_PROMOTE
        case kSocBtnGuildLeave: {                                      // CONFIRM_GUILD_LEAVE, GetGuildInfo("player")
            const so::GuildQuery* g = session_.ownGuild();
            socialConfirm(1, g ? g->name : std::string());
            break;
        }
        default: break;
    }
}

// The chat's requests (ChatFrame.lua's SlashCmdList handlers that need a frame): 1 /friends without a name
// (ToggleFriendsPanel), 2 /who without text (WhoFrame_GetDefaultWhoCommand + ShowWhoPanel + SendWho), 3 /groster
// (the Guild tab), 4 /gdisband (CONFIRM_GUILD_DISBAND for the leader), 5 /ignore without a name (ToggleIgnorePanel).
void WorldScreen::socialChatRequest(int code, const std::string& arg) {
    SocialUi& s = socialUi();
    switch (code) {
        case 1:
        case 5: {
            const int header = code == 1 ? 1 : 2;
            const bool shown = windowOpen(kWinFriends) && s.tab == 1 && s.header == header;
            if (shown) {
                closeWindow(kWinFriends);
                playSoundByName("igMainMenuClose");
                break;
            }
            s.header = header;
            if (windowOpen(kWinRaid)) socialSwapFrame(kWinRaid, kWinFriends);
            s.tab = 1;
            if (!windowOpen(kWinFriends)) {
                openWindow(kWinFriends);
                playSoundByName("igCharacterInfoTab");
            }
            socialFriendsFrameUpdate();
            break;
        }
        case 2: {
            // WhoFrame_GetDefaultWhoCommand: WHO_TAG_ZONE"<zone>" level-3 - level+3 (at least 1).
            const int level = static_cast<int>(session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)));
            std::string zone = socialZoneName(zoneId_);
            const std::string cmd = std::string(CLIENT_STR(WHO_TAG_ZONE)) + "\"" + zone + "\" " + std::to_string(std::max(1, level - 3)) +
                                    "-" + std::to_string(level + 3);
            if (windowOpen(kWinRaid)) socialSwapFrame(kWinRaid, kWinFriends);
            s.tab = 2;
            if (!windowOpen(kWinFriends)) openWindow(kWinFriends);
            s.whoText = cmd;
            session_.setWhoToUi(true);
            session_.sendWho(cmd);
            break;
        }
        case 3:  // SLASH_GUILD_ROSTER: PanelTemplates_SetTab(FriendsFrame, 3), ShowUIPanel
            if (!session_.isInGuild()) break;
            if (windowOpen(kWinRaid)) socialSwapFrame(kWinRaid, kWinFriends);
            s.tab = 3;
            if (!windowOpen(kWinFriends)) openWindow(kWinFriends);
            socialFriendsFrameUpdate();
            break;
        case 4:
            if (session_.isGuildLeader()) socialConfirm(3, std::string());
            break;
        case 6:  // /who text: WhoFrameEditBox:SetText(msg); SendWho(msg)
            s.whoText = arg;
            session_.sendWho(arg);
            break;
        default: break;
    }
}

// --- GameTooltip of our widgets (after the windows) ------------------------------------------------------------------

void WorldScreen::drawSocialOverlay(UiRenderer& ui) {
    if (!soc_ || !uiMode_ || settings_.isOpen()) return;
    SocialUi& s = *soc_;
    const uint32_t f = winFocus_.focused();
    if (f == 0 || f != s.tipId) {
        s.tipId = 0;
        return;
    }
    const Font& fn = kit_.font(kFontNormal);
    const Font& fs = kit_.font(kFontSmall);
    constexpr float kMaxW = 220.0f;
    struct L {
        std::string text;
        uint32_t color;
        bool title;
    };
    std::vector<L> lines;
    for (const std::string& l : wrapQuestText(fn, s.tipTitle, kMaxW)) lines.push_back(L{l, s.tipTitleColor, true});
    if (!s.tipText.empty())  // GameTooltip_AddNewbieTip: the newbie text gold
        for (const std::string& l : wrapQuestText(fs, s.tipText, kMaxW)) lines.push_back(L{l, kColGold, false});
    if (lines.empty()) return;
    float w = 0.0f, h = 10.0f;
    for (const L& l : lines) {
        w = std::max(w, static_cast<float>((l.title ? fn : fs).measure(l.text.c_str())));
        h += static_cast<float>((l.title ? fn : fs).lineHeight());
    }
    w += 16.0f;
    Rect t{s.tipRect.x + s.tipRect.w + 4.0f, s.tipRect.y, w, h};
    if (t.x + t.w > static_cast<float>(pp::kScreenW) - 2.0f) t.x = s.tipRect.x - 4.0f - w;
    t.x = std::clamp(t.x, 2.0f, static_cast<float>(pp::kScreenW) - w - 2.0f);
    t.y = std::clamp(t.y, 2.0f, static_cast<float>(pp::kScreenH) - h - 2.0f);
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{t.x + 3, t.y + 3, t.w - 6, t.h - 6}, 0xE0FFFFFFu);
    else ui.fill(t, 0xE0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), t);
    float y = t.y + 5.0f;
    for (const L& l : lines) {
        const Font& font = l.title ? fn : fs;
        ui.text(font, t.x + 8.0f, y, l.text.c_str(), l.color);
        y += static_cast<float>(font.lineHeight());
    }
}

// --- The guild tabard emblem ------------------------------------------------------------------------------------------

// The wearer's guild emblem for a worn tabard: the five emblem fields of the guild query's answer (upstream
// GuildQueryResponseParser, AzerothCore Guild::HandleQuery). Own rule: a player in a guild who wears any tabard has
// the guild asked for (the cache answers at once after that); the emblem is filled when the answer is there and
// none meanwhile. Whether the tabard shows it is the body rule's (psp_body_rules.hpp R6: the tabard's display flag).
void WorldScreen::fillGuildEmblem(uint32_t guildId, const rendering::psp::Equipment& base,
                                  rendering::psp::Equipment& equip) {
    (void)base;
    equip.emblem = rendering::psp::emblem::Emblem{};
    if (guildId == 0 || equip.displayId[18] == 0) return;
    if (const so::GuildQuery* g = session_.guildQuery(guildId)) {
        equip.emblem.style = g->emblem[0];
        equip.emblem.color = g->emblem[1];
        equip.emblem.borderStyle = g->emblem[2];
        equip.emblem.borderColor = g->emblem[3];
        equip.emblem.background = g->emblem[4];
    }
}

}  // namespace wowee::ui::psp
