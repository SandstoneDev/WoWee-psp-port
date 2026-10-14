// The state and art of FriendsFrame's Friends / Ignore, Who and Guild
// tabs and the guild's side frames (ui_world_social.cpp, ui_world_guild.cpp) - WorldScreen::SocialUi, made on first
// use (WorldScreen::socialUi). The frame art is loaded from ui.pak while the frame shows (one tab's at a time) and
// freed after; the pak is opened and closed at once . Internal to those two files.
#pragma once

#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::ui::psp {

// FriendsFrame at the character frame's 0.6 x 0.55, where the raid tab (ui_world_raid.cpp) draws it too.
namespace soc {
constexpr float kSX = 0.6f, kSY = 0.55f;
constexpr float kOx = 4.0f, kOy = 2.0f;
constexpr float kFrameW = 211.0f, kFrameH = 265.0f;
inline float PX(float x) { return kOx + x * kSX; }
inline float PY(float y) { return kOy + y * kSY; }
inline Rect R(float x, float y, float w, float h) { return Rect{PX(x), PY(y), w * kSX, h * kSY}; }
// the side frame art a GUILDFRAME_POPUPS index needs (SocialUi::sideKind) - the member detail is
// sf_detail (1) for an officer (the officer note shows), sf_detail_n (5) otherwise. drawGuildSideFrame loads by it and
// updateSocial frees by it (was: popup 1 against sideKind 5 - a non-officer's card freed and loaded every frame).
inline int sideArtKind(int popup, bool officer) { return popup == 1 ? (officer ? 1 : 5) : popup; }
}  // namespace soc

// Widget ids 0x8400-0x86FF.
enum : uint32_t {
    kIdSfClose = 0x8400,
    kIdSfTab1 = 0x8401,         // FriendsFrameTab1..5 .. 0x8405
    kIdSfHeader1 = 0x8406,      // FriendsTabHeaderTab1 (FRIENDS), 0x8407 Tab2 (IGNORE)
    kIdSfStatus = 0x8408,       // FriendsFrameStatusDropDown
    kIdSfFriend0 = 0x8410,      // the friends' buttons .. 0x841F
    kIdSfAddFriend = 0x8420,
    kIdSfSendMessage,
    kIdSfFriendUp,
    kIdSfFriendDown,
    kIdSfIgnore0 = 0x8428,      // FriendsFrameIgnoreButton1..19 .. 0x843A
    kIdSfIgnorePlayer = 0x8440,
    kIdSfUnsquelch,
    kIdSfIgnoreUp,
    kIdSfIgnoreDown,
    kIdSfWho0 = 0x8450,         // WhoFrameButton1..17 .. 0x8460
    kIdSfWhoCol0 = 0x8468,      // WhoFrameColumnHeader1..4 .. 0x846B
    kIdSfWhoDrop = 0x846C,      // WhoFrameDropDown (column 2)
    kIdSfWhoRefresh = 0x8470,
    kIdSfWhoAddFriend,
    kIdSfWhoInvite,
    kIdSfWhoEdit,
    kIdSfWhoUp,
    kIdSfWhoDown,
    kIdSfGuild0 = 0x8480,       // GuildFrameButton1..13 / GuildFrameGuildStatusButton1..13 .. 0x848C
    kIdSfGuildCol0 = 0x8490,    // the column headers (player status 0..3, guild status 4..7) .. 0x8497
    kIdSfGuildToggle = 0x8498,  // GuildFrameGuildListToggleButton
    kIdSfGuildOffline,          // GuildFrameLFGButton (SHOW_OFFLINE_MEMBERS)
    kIdSfGuildMotd,             // GuildMOTDEditButton
    kIdSfGuildControl,
    kIdSfGuildAddMember,
    kIdSfGuildInfo,
    kIdSfGuildUp,
    kIdSfGuildDown,
    kIdSfDetClose = 0x84A0,     // GuildMemberDetailFrame
    kIdSfDetRemove,
    kIdSfDetInvite,
    kIdSfDetPromote,
    kIdSfDetDemote,
    kIdSfDetNote,
    kIdSfDetOfficerNote,
    kIdSfInfoClose = 0x84B0,    // GuildInfoFrame
    kIdSfInfoSave,
    kIdSfInfoCancel,
    kIdSfInfoLog,
    kIdSfInfoEdit,
    kIdSfInfoUp,
    kIdSfInfoDown,
    kIdSfLogClose = 0x84C0,     // GuildEventLogFrame
    kIdSfLogCancel,
    kIdSfLogUp,
    kIdSfLogDown,
    kIdSfCtlRank = 0x84D0,      // GuildControlPopupFrame
    kIdSfCtlAddRank,
    kIdSfCtlRemoveRank,
    kIdSfCtlName,
    kIdSfCtlCheck0 = 0x84D4,    // GuildControlPopupFrameCheckbox1..17 .. 0x84E4
    kIdSfCtlGold = 0x84E8,
    kIdSfCtlTab0 = 0x84E9,      // GuildBankTabPermissionsTab1..6 .. 0x84EE
    kIdSfCtlView = 0x84EF,
    kIdSfCtlDeposit,
    kIdSfCtlUpdateText,
    kIdSfCtlWithdraw,
    kIdSfCtlAccept,
    kIdSfCtlCancel,
    kIdSfPopup1 = 0x8500,       // StaticPopup GUILD_INVITE
    kIdSfPopup2,
    kIdSfConfirm1 = 0x8502,     // the confirmations (CONFIRM_GUILD_LEAVE / _PROMOTE / _DISBAND, REMOVE_GUILDMEMBER)
    kIdSfConfirm2,
    kIdSfDrop0 = 0x8510,        // a dropdown list's rows .. 0x851F
};

// The edit boxes the keyboard opens for (SocialUi::oskField, WorldScreen::socialEditPopup).
enum : int {
    kOskAddFriend = 1, kOskAddIgnore, kOskAddGuildMember, kOskFriendNote, kOskGuildNote, kOskOfficerNote, kOskMotd,
    kOskAddRank, kOskWho, kOskGuildInfo, kOskRankName, kOskRankGold, kOskRankWithdraw,
};

// The FRIEND / FRIEND_OFFLINE unit menus (ui_world_party.cpp MenuKind: SELF 1 .. TARGET 6 below them): 0x10 | the
// UnitPopup_HideButtons inputs - 1 offline (FRIEND_OFFLINE), 2 friendsList, 4 GuildFrame shown, 8 the player's name
// (openNamedUnitPopup sets it); and the social buttons of the unit menus (UnitPopupButtons).
enum : uint8_t { kUnitMenuFriend = 0x10, kUnitMenuFriendOffline = 0x11 };
enum : uint8_t {
    kSocBtnGuildInvite = 1,   // PSP: "Invite to guild" (the PLAYER menu; 3.3.5a has /ginvite only)
    kSocBtnTarget,            // TARGET (TargetByName)
    kSocBtnSetNote,           // SET_NOTE (StaticPopup SET_FRIENDNOTE)
    kSocBtnIgnore,            // IGNORE (AddOrDelIgnore)
    kSocBtnRemoveFriend,      // REMOVE_FRIEND
    kSocBtnGuildPromote,      // GUILD_PROMOTE (CONFIRM_GUILD_PROMOTE)
    kSocBtnGuildLeave,        // GUILD_LEAVE (CONFIRM_GUILD_LEAVE)
};

// Loads one ui.pak texture (the blob stays the texture's); adds its size to `bytes`.
inline bool socialLoad(platform::psp::Pak& pak, const char* name, platform::psp::Texture& t, uint32_t& bytes) {
    const platform::psp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!platform::psp::textureFromBlob(blob, e->size, t, true)) {
        platform::psp::pakFree(blob);
        return false;
    }
    t.name = "social";
    bytes += e->size;
    return true;
}

struct WorldScreen::SocialUi {
    ~SocialUi();
    // --- Art (ui.pak, all optional) ---
    platform::psp::Texture frame;      // sf_friends / sf_who / sf_guild (the tab's)
    int frameKind = -1;                // 0 friends, 1 who, 2 guild
    uint32_t frameBytes = 0;
    platform::psp::Texture status, divider, hlbar, coltab, colhl, rowhl, htabOn, htabOff, cbUp, cbHl, check, ddBox,
        ddBtn, ddBtnDn, ddHl, ddBg, plus, minus, updn, permtab, input, whoScroll, guildScroll,
        pageNext;                      // UI-SpellbookIcon-NextPage-Up (merch_next): GuildFrameGuildListToggleButton
    bool smallTried = false;
    uint32_t smallBytes = 0;
    platform::psp::Texture side;       // sf_detail / sf_info / sf_log / sf_ctrl (the guild popup shown)
    int sideKind = 0;
    uint32_t sideBytes = 0;
    uint32_t sideLoads = 0;            // side art loads since the world came (log social_side_art n=)
    void freeFrameArt();
    void freeSideArt();
    void freeSmallArt();

    // --- FriendsFrame ---
    int tab = 1;                       // FriendsFrame.selectedTab (1 friends .. 5 raid)
    int header = 1;                    // FriendsTabHeader.selectedTab (1 friends, 2 ignore)
    bool shown = false;                // FriendsFrame:IsShown() last frame (kWinFriends or kWinRaid)
    bool soundPlayed = false;
    // Friends (FriendsList_Update): the selected friend (by guid), the scroll offset in buttons.
    uint64_t selectedFriend = 0;
    int friendOffset = 0;
    uint32_t friendsSeen = ~0u;
    uint32_t friendSoundSeq = 0;
    // Ignore (IgnoreList_Update)
    uint64_t selectedIgnore = 0;
    int ignoreOffset = 0;
    // Who (WhoList_Update)
    int whoOffset = 0;
    int selectedWho = 0;               // WhoFrame.selectedWho (1-based, 0 none)
    std::string selectedWhoName;
    int whoColumn = 1;                 // WhoFrameDropDown: 1 zone, 2 guild, 3 race
    std::string whoText;               // WhoFrameEditBox
    uint32_t whoSeen = 0;
    // Guild (GuildStatus_Update)
    int guildOffset = 0;
    uint64_t selectedMember = 0;       // SetGuildRosterSelection (by guid, as the client)
    std::string selectedName;
    bool playerStatus = true;          // FriendsFrame.playerStatusFrame
    int popup = 0;                     // GUILDFRAME_POPUPS shown: 1 detail, 2 info, 3 control, 4 event log
    uint32_t guildSeen = ~0u;
    bool rosterAsked = false;
    // GuildInfoFrame: the edited text (the save sends it), its scroll.
    std::string infoText;
    bool infoEdited = false;
    int infoScroll = 0;
    int logScroll = 0;
    uint32_t logSeen = ~0u;
    // GuildControlPopupFrame (GuildControlSetRank / GetRankFlags / pending bank permissions)
    struct Ctl {
        bool initialized = false;
        int rank = 1;                  // the dropdown's selected id (1-based)
        std::string name;              // GuildControlPopupFrameEditBox
        bool flags[17] = {};
        uint32_t gold = 0;             // GuildControlWithdrawGoldEditBox (gold per day)
        bool goldChanged = false;
        int bankTab = 1;               // GuildControlPopupFrameTabPermissions.selectedTab
        struct Pending {
            int flag[3] = {-1, -1, -1};  // view / deposit / update text: -1 not changed
            int withdraw = -1;
        } pending[6];
        bool acceptEnabled = false;
        int previousRank = 0;
    } ctl;
    // StaticPopup GUILD_INVITE (timeout 60 s, hideOnEscape: START = decline)
    uint32_t inviteSeq = 0, inviteAtMs = 0;
    bool inviteShown = false, inviteAnswered = false;
    std::string inviteText;
    // The confirmations (one at a time): 1 CONFIRM_GUILD_LEAVE, 2 CONFIRM_GUILD_PROMOTE, 3 CONFIRM_GUILD_DISBAND,
    // 4 REMOVE_GUILDMEMBER.
    int confirm = 0;
    std::string confirmArg, confirmText;
    // The PSP keyboard for an edit box (the StaticPopups with an edit box, WhoFrameEditBox, the guild texts).
    int oskField = 0;
    std::string oskArg;
    // A dropdown list (kWinSocialDropDown): 1 FriendsFrameStatusDropDown, 2 WhoFrameDropDown, 3 the rank dropdown.
    int dropKind = 0;
    Rect dropAnchor{};
    uint32_t dropFocusMs = 0;
    // GameTooltip of the focused widget (after the windows).
    uint32_t tipId = 0;
    Rect tipRect{};
    std::string tipTitle, tipText;
    uint32_t tipTitleColor = 0xFFFFFFFFu;
    // Names: ChrClasses / ChrRaces (text.pak via WorldScreen) and the zones (text.pak "zones", only the ids shown).
    std::unordered_map<uint32_t, std::string> zones;
    std::unordered_set<uint32_t> zonesWanted;
    uint32_t zonesReadMs = 0;
    bool whoLookupSet = false;
    std::vector<std::pair<uint32_t, std::string>> topZones;  // glue.pak "zones" (no parent): the who zone tag
    bool topZonesTried = false;
    uint32_t logs = 0;
};

}  // namespace wowee::ui::psp
