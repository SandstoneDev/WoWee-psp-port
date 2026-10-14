// The group on the world screen, as the original ruRU 3.3.5a client draws it.
//
// PartyMemberFrame1..4 (PartyFrame.xml, PartyFrameTemplates.xml, PartyMemberFrame.lua) - the unit frames'
// scale 0.6 (128 x 53 units -> 77 x 32 px) under the player frame:
//   - $parentPortrait 37 x 37 at 7,-6 - round (as the b99 frames): the race / sex portrait (uf_port_*,
//     resident with the unit frame art; the original's 3D portrait needs the VRAM slot the target uses);
//     PartyMemberHealthCheck tints: dead (0.35), ghost (0.2, 0.2, 0.75), <= 20 % health red with the
//     alpha pulse of PartyMemberFrame_UpdateMemberHealth; offline - desaturated (PSP: grey);
//   - $parentBackground (black 0.5, 72 x 20 at 45,-11), HealthBar 70 x 8 at 47,-12 (green, HealthBar_
//     OnValueChanged without smooth; offline: full and grey), ManaBar at 47,-21 (PowerBarColor);
//   - UI-PartyFrame at 0,-2; Name (GameFontNormalSmall, BOTTOMLEFT 50,43); LeaderIcon (0,0), MasterIcon
//     (32,0), Disconnect (LEFT -7,-1), Debuff1..4 (15 x 15 at 48,-32, 2 apart, DebuffTypeColor border);
//   - the bars' TextStatusBarText only under the cursor (partyStatusText off, the original's default) and
//     the unit tooltip (UnitFrame_OnEnter: name, TOOLTIP_UNIT_LEVEL).
// No level text on the frame (the original has none), no pet frames (no pets on the PSP), no PvP / role /
// voice icons (the ready check icon: the raid code below), no out-of-range fading (3.3.5a FrameXML does not fade
// party frames).
//
// The unit menus (UnitPopup.lua: UnitPopupMenus SELF / PARTY / PLAYER filtered by UnitPopup_HideButtons,
// enabled by UnitPopup_OnUpdate, shown by UIDropDownMenu in "MENU" mode - the tooltip backdrop, the name as
// the title, LOOT_METHOD / LOOT_THRESHOLD nested at level 2 with the current one checked): the buttons
// the PSP has a use for - LOOT_METHOD, LOOT_THRESHOLD, LOOT_PROMOTE, LEAVE, WHISPER, PROMOTE, UNINVITE,
// INVITE, CANCEL. Circle (the right click) on PlayerFrame / a party frame / TargetFrame opens it at the
// original's ToggleDropDownMenu anchor; Cross (the left click) targets the unit (TargetUnit) - on
// TargetFrame Cross opens the menu too (PSP: its left click has nothing to do). A click outside closes
// it, as does START (CloseDropDownMenus), leaving the interface and UIDROPDOWNMENU_SHOW_TIME (2 s) away.
//
// StaticPopup PARTY_INVITE: INVITATION, ACCEPT / DECLINE, timeout 60 s, hideOnEscape (START = decline),
// whileDead, sound igPlayerInvite; PARTY_INVITE_CANCEL hides it; a hide without "accept" declines (OnHide).
// PSP: the popup does not raise the cursor (SELECT), the focus waits on ACCEPT.
//
// PlayerFrame's PlayerLeaderIcon (40,-12) / PlayerMasterIcon (80,-10), TargetFrame's leader icon (TOPRIGHT
// -44,-10) are drawn here after their frames (ui_world_target.cpp calls drawPartyFrameExtras).
// Art: ui.pak pf_* (+ npc_titlehl) only while in a group or a menu is open; log party_art kb=.
//
// + UnitPopupMenus RAID (a RaidGroupButton's menu, opened by
// ui_world_raid.cpp through openRaidUnitPopup), RAID_PLAYER (TargetFrame on a raid member) and TARGET (TargetFrame on
// an NPC, titled RAID_TARGET_ICON) with RAID_LEADER / RAID_PROMOTE / RAID_MAINTANK / RAID_MAINASSIST / RAID_DEMOTE /
// RAID_REMOVE / PVP_REPORT_AFK, RAID_TARGET_ICON in every menu with its nested list (the eight marks in their colour
// with their icon, RAID_TARGET_NONE; SetRaidTargetIcon), UNINVITE hidden in a battleground; the ready check icon on
// PartyMemberFrameN (PartyMemberFrame_UpdateReadyCheck: 32 at the portrait, drawPartyReadyCheck).
#include "ui/psp/ui_world_screen.hpp"
#include "ui/psp/ui_world_raid_bg.hpp"  // the RAID / RAID_PLAYER / TARGET menus
#include "ui/psp/ui_world_social.hpp"   // the FRIEND / FRIEND_OFFLINE menus, the social buttons
#include "game/update_field_table.hpp"  // PLAYER_GUILDID (UNIT_END + 3)

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"

#include <pspctrl.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kS = 0.6f;            // the unit frames' scale (ui_world_target.cpp kFrameScale)
constexpr float kFrameW = 232.0f * kS;  // PlayerFrame / TargetFrame drawn width (139 px)
// PartyMemberFrame1: TOPLEFT 10,-160 of UIParent, PlayerFrame -19,-4: 29 units right of the player frame's art.
constexpr float kPartyX = 2.0f + 29.0f * kS;
// PSP: under the player frame (its ring ends at y 50) - the original's 156 units lower would put the 4th frame
// over the chat; the next frame 32 px down (the original: 63 units, room for the pet frame the PSP has not).
constexpr float kPartyY0 = 56.0f;
constexpr float kPartyPitch = 32.0f;

enum : uint32_t {
    kIdPlayerFrame = 0x7E00,
    kIdTargetFrame = 0x7E01,
    kIdPartyFrame1 = 0x7E02,  // .. 0x7E05
    kIdMenu1 = 0x7E10,        // level 1 rows .. 0x7E2F
    kIdMenu2 = 0x7E30,        // level 2 rows .. 0x7E3F
    kIdInviteAccept = 0x7E40,
    kIdInviteDecline = 0x7E41,
    kIdRoll0 = 0x7E50,             // GroupLootFrame k: + k * 4 + (0 need, 1 greed, 2 disenchant, 3 pass) .. 0x7E5F
    kIdRollConfirmOk = 0x7E70,
    kIdRollConfirmCancel = 0x7E71,
};

enum MenuKind : uint8_t {
    kMenuNone = 0, kMenuSelf, kMenuParty, kMenuPlayer,
    // UnitPopupMenus RAID (RaidGroupButton), RAID_PLAYER (TargetFrame on a raid member), TARGET (an NPC)
    kMenuRaid = kUnitMenuRaid, kMenuRaidPlayer = kUnitMenuRaidPlayer, kMenuTarget = kUnitMenuTarget,
    kMenuPet = 7,  // UnitPopupMenus PET (PetFrame / TargetFrame on the pet)
};

// UnitPopupButtons the PSP shows.
enum Btn : uint8_t {
    bTitle, bLootMethod, bLootThreshold, bLootPromote, bLeave, bWhisper, bPromote, bUninvite, bInvite, bCancel,
    bFfa, bRoundRobin, bMaster, bGroup, bNbg, bQ2, bQ3, bQ4,
    // the raid's buttons, PVP_REPORT_AFK, RAID_TARGET_ICON and its nested list.
    bRaidLeader, bRaidPromote, bRaidMainTank, bRaidMainAssist, bRaidDemote, bRaidRemove, bPvpReportAfk,
    bRaidTargetIcon, bRt1, bRt2, bRt3, bRt4, bRt5, bRt6, bRt7, bRt8, bRtNone,
    bInspect,  // INSPECT (InspectUnit -> InspectFrame)
    bTrade, bDuel,  // TRADE (InitiateTrade), DUEL (StartDuel)
    bPetPaperdoll, bPetRename, bPetAbandon, bPetDismiss,  // UnitPopupMenus PET
};
// the social buttons - kSocBtnBase + kSocBtn* (ui_world_social.hpp; WorldScreen::socialUnitMenuAction).
constexpr uint8_t kSocBtnBase = 0x40;
constexpr pp::CS kSocBtnText[] = {
    pp::CS::TARGET,         // (none: index 0 is not a button)
    pp::CS::CHAT_GUILD_INVITE_SEND,  // PSP: without its colon (3.3.5a has /ginvite only) - socBtnText
    pp::CS::TARGET,         // TARGET
    pp::CS::SET_NOTE,       // SET_NOTE
    pp::CS::IGNORE,         // IGNORE
    pp::CS::REMOVE_FRIEND,  // REMOVE_FRIEND
    pp::CS::GUILD_PROMOTE,  // GUILD_PROMOTE
    pp::CS::GUILD_LEAVE,    // GUILD_LEAVE
};
static_assert(sizeof(kSocBtnText) / sizeof(kSocBtnText[0]) == kSocBtnGuildLeave + 1, "kSocBtnText follows kSocBtn*");

// GlobalStrings (strings.pak). LootRollButtonTemplate tooltips: NEED, GREED, ROLL_DISENCHANT; the pass (close)
// button PASS.
constexpr pp::CS kRollText[4] = {pp::CS::NEED, pp::CS::GREED, pp::CS::ROLL_DISENCHANT, pp::CS::PASS};
constexpr pp::CS kBtnText[] = {
    pp::CS::CANCEL,                  // title (none: btnText gives "")
    pp::CS::LOOT_METHOD,             // LOOT_METHOD (replaced by the method's name)
    pp::CS::LOOT_THRESHOLD,          // LOOT_THRESHOLD (replaced by the quality)
    pp::CS::LOOT_PROMOTE,            // LOOT_PROMOTE
    pp::CS::PARTY_LEAVE,             // PARTY_LEAVE
    pp::CS::WHISPER,                 // WHISPER
    pp::CS::PARTY_PROMOTE,           // PARTY_PROMOTE
    pp::CS::PARTY_UNINVITE,          // PARTY_UNINVITE
    pp::CS::PARTY_INVITE,            // PARTY_INVITE
    pp::CS::CANCEL,                  // CANCEL
    pp::CS::LOOT_FREE_FOR_ALL,       // LOOT_FREE_FOR_ALL
    pp::CS::LOOT_ROUND_ROBIN,        // LOOT_ROUND_ROBIN
    pp::CS::LOOT_MASTER_LOOTER,      // LOOT_MASTER_LOOTER
    pp::CS::LOOT_GROUP_LOOT,         // LOOT_GROUP_LOOT
    pp::CS::LOOT_NEED_BEFORE_GREED,  // LOOT_NEED_BEFORE_GREED
    pp::CS::ITEM_QUALITY2_DESC,      // ITEM_QUALITY2_DESC
    pp::CS::ITEM_QUALITY3_DESC,      // ITEM_QUALITY3_DESC
    pp::CS::ITEM_QUALITY4_DESC,      // ITEM_QUALITY4_DESC

    pp::CS::SET_RAID_LEADER,         // SET_RAID_LEADER
    pp::CS::SET_RAID_ASSISTANT,      // SET_RAID_ASSISTANT
    pp::CS::SET_MAIN_TANK,           // SET_MAIN_TANK
    pp::CS::SET_MAIN_ASSIST,         // SET_MAIN_ASSIST
    pp::CS::DEMOTE,                  // DEMOTE
    pp::CS::REMOVE,                  // REMOVE
    pp::CS::PVP_REPORT_AFK,          // PVP_REPORT_AFK
    pp::CS::RAID_TARGET_ICON,        // RAID_TARGET_ICON
    pp::CS::RAID_TARGET_1, pp::CS::RAID_TARGET_2, pp::CS::RAID_TARGET_3, pp::CS::RAID_TARGET_4,
    pp::CS::RAID_TARGET_5, pp::CS::RAID_TARGET_6, pp::CS::RAID_TARGET_7, pp::CS::RAID_TARGET_8,
    pp::CS::RAID_TARGET_NONE,        // RAID_TARGET_NONE
    pp::CS::INSPECT,                 // INSPECT
    pp::CS::TRADE, pp::CS::DUEL,     // TRADE, DUEL
    pp::CS::PET_PAPERDOLL,           // PET_PAPERDOLL
    pp::CS::PET_RENAME,              // PET_RENAME
    pp::CS::PET_ABANDON,             // PET_ABANDON
    pp::CS::PET_DISMISS,             // PET_DISMISS
};
static_assert(sizeof(kBtnText) / sizeof(kBtnText[0]) == bPetDismiss + 1, "kBtnText follows Btn");
// UnitPopupButtons RAID_TARGET_1..8 color (0xAABBGGRR).
constexpr uint32_t kRaidTargetColor[8] = {0xFF00EBFFu, 0xFF0091FAu, 0xFFE638D4u, 0xFF00F20Au,
                                          0xFFDFD1B3u, 0xFFFFB500u, 0xFF2B3DFFu, 0xFFFAFAFAu};
constexpr pp::CS kQualityDesc[7] = {pp::CS::ITEM_QUALITY0_DESC, pp::CS::ITEM_QUALITY1_DESC, pp::CS::ITEM_QUALITY2_DESC,
                                    pp::CS::ITEM_QUALITY3_DESC, pp::CS::ITEM_QUALITY4_DESC, pp::CS::ITEM_QUALITY5_DESC,
                                    pp::CS::ITEM_QUALITY6_DESC};  // ITEM_QUALITY0..6_DESC
// ITEM_QUALITY_COLORS (0xAABBGGRR).
constexpr uint32_t kQualityColor[7] = {0xFF9D9D9Du, 0xFFFFFFFFu, 0xFF00FF1Eu, 0xFFDD7000u,
                                       0xFFEE35A3u, 0xFF0080FFu, 0xFF80CCE6u};
constexpr uint32_t kColDisable = 0xFF808080u;  // GameFontDisableSmall (0.5)

// PowerBarColor (UnitFrame.lua) - as ui_world_target.cpp.
uint32_t powerColor(uint8_t type) {
    switch (type) {
        case 0: return 0xFFFF0000u;
        case 1: return 0xFF0000FFu;
        case 2: return 0xFF4080FFu;
        case 3: return 0xFF00FFFFu;
        case 6: return 0xFFFFD100u;
        default: return 0xFFFF0000u;
    }
}

// DebuffTypeColor by SpellDispelType (none, Magic, Curse, Disease, Poison).
uint32_t debuffColor(uint8_t dispel) {
    switch (dispel) {
        case 1: return 0xFFFF9933u;  // Magic   (0.2, 0.6, 1)
        case 2: return 0xFFFF0099u;  // Curse   (0.6, 0, 1)
        case 3: return 0xFF006699u;  // Disease (0.6, 0.4, 0)
        case 4: return 0xFF009900u;  // Poison  (0, 0.6, 0)
        default: return 0xFF0000CCu;  // none   (0.8, 0, 0)
    }
}

// The pattern is the client's text of the running language (strings.pak): textf, not snprintf.
std::string fmt1(const char* f, const std::string& a) { return pp::text::textf(f, a); }

// UnitPopupButtons' text: kBtnText (index 0, the title, has none).
const char* btnText(uint8_t b) { return b == 0 ? "" : pp::cs(kBtnText[b]); }

// The social buttons' text (kSocBtnText); CHAT_GUILD_INVITE_SEND without its ": ".
const char* socBtnText(uint8_t i) {
    if (i == 0) return "";
    if (i == 1) {
        static std::string guildInvite;  // CLIENT_STR lives for the session: made once
        if (guildInvite.empty()) {
            guildInvite = CLIENT_STR(CHAT_GUILD_INVITE_SEND);
            while (!guildInvite.empty() && (guildInvite.back() == ' ' || guildInvite.back() == ':'))
                guildInvite.pop_back();
        }
        return guildInvite.c_str();
    }
    return pp::cs(kSocBtnText[i]);
}

struct MenuRow {
    uint8_t btn = bCancel;
    std::string text;
    uint32_t color = kColWhite;
    bool enabled = true, nested = false, checkable = false, checked = false, title = false;
    int8_t icon = -1;  // info.icon - the UI-RaidTargetingIcons cell (RAID_TARGET_1..8)
};

}  // namespace

struct WorldScreen::PartyUi {
    pp::Texture frame, leader, master, disconnect, check, arrow, hl;
    bool artTried = false;
    uint32_t artBytes = 0;
    // UnitPopup (one open menu, as UIDropDownMenu)
    uint8_t menu = kMenuNone;
    uint64_t unit = 0;
    std::string name;
    uint32_t opener = 0;      // the frame's focus id (a second right click there closes it)
    float ax = 0.0f, ay = 0.0f;
    uint8_t sub = 0;          // the nested list shown: bLootMethod / bLootThreshold, 0 none
    float subTop = 0.0f, subX = 0.0f;
    uint32_t menuFocusMs = 0;
    std::vector<uint8_t> rows1, rows2;  // the buttons the last frame showed (by row id)
    // StaticPopup PARTY_INVITE
    uint32_t inviteSeq = 0, inviteAtMs = 0;
    bool inviteShown = false, inviteAnswered = false;
    std::string inviter, inviteText;
    bool lastUi = false;
    uint32_t joinSoundSeq = 0;
    uint32_t logs = 0;
    // GroupLootFrame1..4 + CONFIRM_LOOT_ROLL
    pp::Texture rollBtn[3], rollHl[3];
    bool rollArtTried = false;
    uint32_t rollArtBytes = 0;
    std::vector<int> rollIds;   // the roll in each frame the last frame drew
    int confirmRoll = 0;
    uint8_t confirmType = 0;
    std::string confirmText;
    void loadRollArt();
    void releaseRollArt() {
        for (int i = 0; i < 3; ++i) {
            if (rollBtn[i].pixels) pp::textureRelease(rollBtn[i]);
            if (rollHl[i].pixels) pp::textureRelease(rollHl[i]);
        }
        if (rollArtTried) PSP_LOGI(pp::tag::Gfx, "grouploot_art_free kb=%u", static_cast<unsigned>(rollArtBytes / 1024u));
        rollArtTried = false;
        rollArtBytes = 0;
    }
    void loadArt();
    void releaseArt() {
        for (pp::Texture* t : {&frame, &leader, &master, &disconnect, &check, &arrow, &hl})
            if (t->pixels) pp::textureRelease(*t);
        if (artTried) PSP_LOGI(pp::tag::Gfx, "party_art_free kb=%u", static_cast<unsigned>(artBytes / 1024u));
        artTried = false;
        artBytes = 0;
    }
    ~PartyUi() {
        releaseArt();
        releaseRollArt();
    }
};

void WorldScreen::destroyPartyUi(PartyUi* p) { delete p; }

WorldScreen::PartyUi& WorldScreen::partyUi() {
    if (!party_) party_ = std::unique_ptr<PartyUi, void (*)(PartyUi*)>(new PartyUi, &WorldScreen::destroyPartyUi);
    return *party_;
}

// --- Art -------------------------------------------------------------------------------------------

namespace {

bool loadOne(pp::Pak& pak, const char* name, pp::Texture& t, uint32_t& bytes) {
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, e->size, t, true)) {
        pp::pakFree(blob);
        return false;
    }
    t.name = "party";
    bytes += e->size;
    return true;
}

}  // namespace

void WorldScreen::PartyUi::loadArt() {
    PartyUi& p = *this;
    p.artTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    int ok = 0;
    if (pak.open(path)) {
        const struct {
            const char* name;
            pp::Texture* tex;
        } list[] = {{"pf_frame", &p.frame}, {"pf_leader", &p.leader}, {"pf_master", &p.master},
                    {"pf_disconnect", &p.disconnect}, {"pf_check", &p.check}, {"pf_arrow", &p.arrow},
                    {"npc_titlehl", &p.hl}};
        for (const auto& x : list)
            if (loadOne(pak, x.name, *x.tex, p.artBytes)) ++ok;
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "party_art ok=%d/7 kb=%u ms=%u", ok, static_cast<unsigned>(p.artBytes / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
}

void WorldScreen::PartyUi::loadRollArt() {
    rollArtTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {
        const char* names[3] = {"need", "greed", "de"};
        for (int i = 0; i < 3; ++i) {
            char n[24];
            std::snprintf(n, sizeof(n), "gl_%s", names[i]);
            if (loadOne(pak, n, rollBtn[i], rollArtBytes)) ++ok;
            std::snprintf(n, sizeof(n), "gl_%s_hl", names[i]);
            if (loadOne(pak, n, rollHl[i], rollArtBytes)) ++ok;
        }
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "grouploot_art ok=%d/6 kb=%u", ok, static_cast<unsigned>(rollArtBytes / 1024u));
}

// --- Per frame (from drawWindows) ------------------------------------------------------------------

void WorldScreen::updateParty() {
    PartyUi& p = partyUi();
    const uint32_t now = pp::logUptimeMs();
    // The art: in a group (the frames) or while a menu / the invite is up; freed otherwise.
    const bool need = session_.isInGroup() || p.menu != kMenuNone;
    if (need && !p.artTried) p.loadArt();
    else if (!need && p.artTried) p.releaseArt();
    // igPlayerInviteAccept: a member joined (the roster handler's sound).
    if (session_.partyJoinSoundSeq() != p.joinSoundSeq) {
        p.joinSoundSeq = session_.partyJoinSoundSeq();
        playSoundByName("igPlayerInviteAccept");
    }
    // PARTY_INVITE_REQUEST -> StaticPopup_Show("PARTY_INVITE", arg1).
    if (session_.groupInviteSeq() != p.inviteSeq) {
        p.inviteSeq = session_.groupInviteSeq();
        if (session_.hasPendingGroupInvite()) {
            p.inviter = session_.pendingInviterName();
            p.inviteText = fmt1(CLIENT_STR(INVITATION), p.inviter);
            p.inviteAtMs = now;
            p.inviteShown = true;
            p.inviteAnswered = false;
            openWindow(kWinPartyInvite);
            winFocus_.setFocus(kIdInviteAccept);
            // StaticPopupDialogs.PARTY_INVITE.sound; upstream plays playQuestActivate.
            if (!playSoundByName("igPlayerInvite")) playUiSound(UiSound::QuestActivate);
            PSP_LOGI(pp::tag::Ctrl, "party_invite_popup from=%s", p.inviter.c_str());
        }
    }
    const bool cancelled = session_.takeGroupInviteCancel();
    if (p.inviteShown) {
        // PARTY_INVITE_CANCEL (StaticPopup_Hide), timeout = 60, START (hideOnEscape: OnCancel): hidden; the
        // OnHide declines unless it was accepted.
        const bool hide = cancelled || now - p.inviteAtMs >= 60000u || !windowOpen(kWinPartyInvite);
        if (hide) {
            if (windowOpen(kWinPartyInvite)) closeWindow(kWinPartyInvite);
            p.inviteShown = false;
            if (!p.inviteAnswered) {
                p.inviteAnswered = true;
                session_.declineGroupInvite();
            }
            PSP_LOGI(pp::tag::Ctrl, "party_invite_hide cancel=%d", cancelled ? 1 : 0);
        }
    }
    // PSP: the cursor comes up (SELECT) with the invite on top - the focus on ACCEPT.
    if (uiMode_ && !p.lastUi && p.inviteShown && !windows_.empty() && windows_.back() == kWinPartyInvite)
        winFocus_.setFocus(kIdInviteAccept);
    else if (uiMode_ && !p.lastUi && !windows_.empty() && windows_.back() == kWinGroupLoot)
        winFocus_.setFocus(kIdRoll0 + 1);  // PSP: the cursor up with a roll on top - GREED
    p.lastUi = uiMode_;
    // GroupLootFrame1..4: up while the session has rolls (START closes the window: it comes back - the original
    // frames do not close on Esc); CONFIRM_LOOT_ROLL goes with its roll (StaticPopup_Hide on CANCEL_LOOT_ROLL).
    if (!session_.lootRolls().empty()) session_.dropExpiredLootRolls();
    const auto& rolls = session_.lootRolls();
    if (!rolls.empty() && !p.rollArtTried) p.loadRollArt();
    if (!rolls.empty() && !windowOpen(kWinGroupLoot)) {
        openWindow(kWinGroupLoot);
        if (uiMode_) winFocus_.setFocus(kIdRoll0 + 1);
    } else if (rolls.empty() && windowOpen(kWinGroupLoot)) {
        closeWindow(kWinGroupLoot);
    }
    if (p.confirmRoll != 0) {
        bool alive = false;
        for (const auto& r : rolls) alive = alive || r.rollId == p.confirmRoll;
        if (!alive || !windowOpen(kWinLootRollConfirm)) {
            if (windowOpen(kWinLootRollConfirm)) closeWindow(kWinLootRollConfirm);
            p.confirmRoll = 0;
        }
    }
    if (rolls.empty() && p.confirmRoll == 0 && p.rollArtTried) p.releaseRollArt();
    // The unit menu: closed by START (the stack), by leaving the interface, UIDROPDOWNMENU_SHOW_TIME away.
    if (p.menu != kMenuNone) {
        const uint32_t f = winFocus_.focused();
        if (f >= kIdMenu1 && f < kIdMenu2 + 16) p.menuFocusMs = now;
        bool close = !windowOpen(kWinUnitPopup) || !uiMode_ || settings_.isOpen() || now - p.menuFocusMs > 2000u;
        if ((p.menu == kMenuParty || p.menu == kMenuRaid || p.menu == kMenuRaidPlayer) && !session_.partyMember(p.unit))
            close = true;  // the member is gone (a raid member too)
        if (p.menu == kMenuPet && (session_.petGuid() == 0 || p.unit != session_.petGuid())) close = true;
        if (close) {
            if (windowOpen(kWinUnitPopup)) closeWindow(kWinUnitPopup);
            p.menu = kMenuNone;
            p.sub = 0;
        }
    }
}

// --- The unit menu ---------------------------------------------------------------------------------

namespace {

// UnitPopup_ShowMenu + UnitPopup_HideButtons (shown) + UnitPopup_OnUpdate (enabled), for the PSP's buttons.
std::vector<MenuRow> buildMenuRows(const pp::WorldSession& s, uint8_t menu, uint64_t unit, const std::string& name,
                                   int level, uint8_t sub, bool canCoop, bool inspectEnabled = false,
                                   uint8_t tradeDuelMask = 0) {  // TRADE bit 0, DUEL bit 1 enabled
    std::vector<MenuRow> rows;
    const auto& pd = s.getPartyData();
    const bool inParty = s.isInGroup();
    const bool isLeader = s.isGroupLeader();
    const bool isSelfUnit = unit == s.playerGuid();
    const game::GroupMember* m = s.partyMember(unit);
    const bool connected = isSelfUnit || !m || m->isOnline;
    // UnitPopup_HideButtons' raid inputs - isLeader (IsPartyLeader), isAssistant (IsRaidOfficer: the
    // leader too), inBattleground, the unit's rank (UnitIsPartyLeader 2 / UnitIsRaidOfficer 1) and assignments.
    const bool inRaid = s.isInRaid();
    const bool isAssistant = s.isRaidOfficer();
    const bool inBattleground = s.activeBgSlot() >= 0;
    const uint8_t unitRank = s.raidRank(unit);
    const uint8_t unitFlags = isSelfUnit ? pd.flags : m ? m->flags : 0;
    auto add = [&](uint8_t btn, bool enabled = true) {
        MenuRow r;
        r.btn = btn;
        r.text = btn >= kSocBtnBase ? socBtnText(btn - kSocBtnBase) : btnText(btn);
        r.enabled = enabled;
        rows.push_back(r);
        return &rows.back();
    };
    if (level == 2) {
        if (sub == bLootMethod) {
            // UnitPopupMenus.LOOT_METHOD: all for the leader (a member sees only the current one).
            for (uint8_t b = bFfa; b <= bNbg; ++b) {
                const uint8_t method = static_cast<uint8_t>(b - bFfa);
                if (!inParty || (!isLeader && pd.lootMethod != method)) continue;
                MenuRow* r = add(b);
                r->checkable = true;
                r->checked = pd.lootMethod == method;
            }
        } else if (sub == bLootThreshold) {
            for (uint8_t b = bQ2; b <= bQ4; ++b) {
                MenuRow* r = add(b);
                r->checkable = true;
                r->checked = pd.lootThreshold == 2 + (b - bQ2);
                r->color = kQualityColor[2 + (b - bQ2)];
            }
        } else if (sub == bRaidTargetIcon) {
            // UnitPopupMenus.RAID_TARGET_ICON - the eight marks (their colour, their icon, the unit's
            // own checked: GetRaidTargetIndex) and RAID_TARGET_NONE; no CANCEL in this list.
            const uint8_t cur = s.raidMarkOf(unit);
            for (uint8_t b = bRt1; b <= bRtNone; ++b) {
                MenuRow* r = add(b);
                r->checkable = true;
                if (b != bRtNone) {
                    const uint8_t i = static_cast<uint8_t>(b - bRt1);
                    r->checked = cur == i;
                    r->color = kRaidTargetColor[i];
                    r->icon = static_cast<int8_t>(i);
                }
            }
            return rows;
        }
        MenuRow* c = add(bCancel);
        c->checkable = true;
        return rows;
    }
    MenuRow title;  // the dropdown title: the unit's name (UNKNOWN without one)
    title.btn = bTitle;
    title.text = name.empty() ? CLIENT_STR(UNKNOWN) : name;
    title.title = true;
    title.color = kColGold;
    title.enabled = false;
    rows.push_back(title);
    const bool masterLoot = pd.lootMethod == 2;
    const bool unitIsMaster = masterLoot && pd.looterGuid == unit;
    // RAID_TARGET_ICON - a raid's member without the lead / assist has none; out of SELF, none while
    // the target is an enemy player outside the group (UnitPopup_HideButtons reads "target" there).
    auto addRaidTarget = [&]() {
        if (inRaid && !isLeader && !isAssistant) return;
        if (menu != kMenuSelf) {
            const uint64_t t = s.target();
            if (t != 0 && t != s.playerGuid() && !s.partyMember(t)) {
                auto e = s.entities().getEntity(t);
                if (e && e->getType() == game::ObjectType::PLAYER && s.isHostileUnit(t)) return;
            }
        }
        add(bRaidTargetIcon)->nested = true;
    };
    // PVP_REPORT_AFK: in a battleground, another player of it.
    auto addReportAfk = [&]() {
        if (inBattleground && !isSelfUnit && m) add(bPvpReportAfk);
    };
    // INSPECT - hidden when the unit can be attacked (UnitPopup_HideButtons), disabled while
    // dead / a ghost or out of CheckInteractDistance(unit, 1) (UnitPopup_OnUpdate: dist = 1, UnitIsDeadOrGhost).
    auto addInspect = [&]() {
        if (!isSelfUnit && !s.isHostileUnit(unit)) add(bInspect, inspectEnabled);
    };
    // TRADE (shown when the unit can cooperate) and DUEL (hidden when it can be attacked) -
    // UnitPopup_HideButtons; enabled alive and in CheckInteractDistance 2 / 3 (UnitPopup_OnUpdate, tradeDuelMenuMask).
    auto addTradeDuel = [&]() {
        if (isSelfUnit) return;
        auto e = s.entities().getEntity(unit);
        if (!e || e->getType() != game::ObjectType::PLAYER) return;
        if (canCoop) add(bTrade, (tradeDuelMask & 1) != 0);
        if (!s.isHostileUnit(unit)) add(bDuel, (tradeDuelMask & 2) != 0);
    };
    // RAID_LEADER / RAID_PROMOTE / RAID_MAINTANK / RAID_MAINASSIST / RAID_DEMOTE / RAID_REMOVE (UnitPopup_HideButtons).
    const bool officer = isLeader || isAssistant;
    const bool hasName = !name.empty();
    auto showLeader = [&]() { return isLeader && unitRank != 2 && hasName; };
    auto showPromote = [&]() { return isLeader && unitRank == 0; };
    auto showDemote = [&]() {
        if (!officer || !hasName) return false;
        if (unitFlags & 0x06) return true;  // GetPartyAssignment MAINTANK / MAINASSIST: clear it
        if (!isLeader && isAssistant && unitRank >= 1) return false;
        return unitRank == 1;  // an assistant (not the leader, not a plain member)
    };
    auto showRemove = [&]() {
        if (!officer || !hasName) return false;
        if (!isLeader && isAssistant && unitRank >= 1) return false;
        return !(isLeader && isSelfUnit);
    };
    if (menu == kMenuSelf) {
        if (inParty) {
            MenuRow* r = add(bLootMethod, isLeader);
            r->text = btnText(bFfa + std::min<uint8_t>(pd.lootMethod, 4));
            r->nested = isLeader;
            const uint8_t q = std::min<uint8_t>(pd.lootThreshold > 0 ? pd.lootThreshold : 2, 6);
            r = add(bLootThreshold, isLeader);
            r->text = pp::cs(kQualityDesc[q]);
            r->color = kQualityColor[q];
            r->nested = isLeader;
            if (isLeader && masterLoot && !unitIsMaster) add(bLootPromote);
        }
        addRaidTarget();
        if (inParty) add(bLeave);
    } else if (menu == kMenuParty) {
        if (canCoop && !isSelfUnit) add(bWhisper, connected);
        if (inParty && isLeader) add(bPromote, connected);
        if (inParty && isLeader && masterLoot && !unitIsMaster) add(bLootPromote);
        if (inParty && isLeader && !inBattleground) add(bUninvite);  // not in a battleground
        addInspect();  // UnitPopupMenus.PARTY - after UNINVITE
        addTradeDuel();  // UnitPopupMenus.PARTY - INSPECT, ACHIEVEMENTS, TRADE, FOLLOW, DUEL
        addRaidTarget();
        addReportAfk();
    } else if (menu == kMenuPlayer) {
        if (canCoop && !isSelfUnit) {
            add(bWhisper);
            addInspect();  // UnitPopupMenus.PLAYER - WHISPER, INSPECT, INVITE
            add(bInvite, !(inParty && !isLeader));
            // PSP: CHAT_GUILD_INVITE_SEND (GuildInvite(UnitName(unit)), the /ginvite of 3.3.5a) - with
            // CanGuildInvite, a player not in the own guild (PLAYER_GUILDID = UNIT_END + 3).
            if (s.guildCan(pp::social::kRightInvite)) {
                const auto e = s.entities().getEntity(unit);
                const uint16_t fEnd = game::fieldIndex(game::UF::UNIT_END);
                if (e && fEnd != 0xFFFF && e->getField(static_cast<uint16_t>(fEnd + 3)) != s.guildId())
                    add(static_cast<uint8_t>(kSocBtnBase + kSocBtnGuildInvite));
            }
        }
        addTradeDuel();  // UnitPopupMenus.PLAYER - INVITE, ACHIEVEMENTS, TRADE, FOLLOW, DUEL
        addRaidTarget();
    } else if (menu == kMenuRaidPlayer) {  // UnitPopupMenus.RAID_PLAYER
        if (canCoop && !isSelfUnit) add(bWhisper, connected);
        addInspect();  // UnitPopupMenus.RAID_PLAYER - after WHISPER
        addTradeDuel();  // UnitPopupMenus.RAID_PLAYER - INSPECT, ACHIEVEMENTS, TRADE, FOLLOW, DUEL
        addRaidTarget();
        if (showLeader()) add(bRaidLeader);
        if (showPromote()) add(bRaidPromote);
        if (showDemote()) add(bRaidDemote);
        if (inParty && isLeader && masterLoot && !unitIsMaster) add(bLootPromote);
        if (showRemove()) add(bRaidRemove);
        addReportAfk();
    } else if (menu == kMenuRaid) {  // UnitPopupMenus.RAID (RaidGroupButton)
        if (showLeader()) add(bRaidLeader);
        if (showPromote()) add(bRaidPromote);
        if (officer && !(unitFlags & 0x02) && hasName) add(bRaidMainTank);
        if (officer && !(unitFlags & 0x04) && hasName) add(bRaidMainAssist);
        if (inParty && isLeader && masterLoot && !unitIsMaster) add(bLootPromote);
        if (showDemote()) add(bRaidDemote);
        if (showRemove()) add(bRaidRemove);
        addReportAfk();
    } else if (menu == kMenuTarget) {  // UnitPopupMenus.TARGET (no focus frame on the PSP)
        addRaidTarget();
    } else if ((menu & 0xF0) == kUnitMenuFriend) {
        // UnitPopupMenus.FRIEND / FRIEND_OFFLINE (FriendsDropDown: a name, no unit) - the menu's low bits
        // are UnitPopup_HideButtons' inputs at the open: 1 offline, 2 dropdownMenu.friendsList, 4 GuildFrame:IsShown(),
        // 8 the name is the player's. No POP_OUT_CHAT / REPORT_SPAM (no whisper windows, no chat line).
        const bool offline = (menu & 1) != 0, friendsList = (menu & 2) != 0, guildShown = (menu & 4) != 0,
                   self = (menu & 8) != 0;
        auto soc = [&](uint8_t b) { add(static_cast<uint8_t>(kSocBtnBase + b)); };
        if (!offline) {
            add(bWhisper);
            bool member = self;  // INVITE: not the player / a party member
            for (const auto& gm : pd.members) member = member || pp::social::sameName(gm.name, name);
            if (!member) add(bInvite);
            if (!s.inCombat()) soc(kSocBtnTarget);  // TARGET: not under InCombatLockdown
        }
        if (friendsList) soc(kSocBtnSetNote);
        soc(kSocBtnIgnore);
        if (!offline) {
            bool inMyGuild = false;  // UnitIsInMyGuild(name)
            for (const auto& gm : s.guildRoster()) inMyGuild = inMyGuild || pp::social::sameName(gm.name, name);
            if (s.isGuildLeader() && inMyGuild && !self && guildShown) soc(kSocBtnGuildPromote);
            if (self && guildShown) soc(kSocBtnGuildLeave);
            if (inBattleground && !self && unit != 0) add(bPvpReportAfk);  // PSP: ReportPlayerIsPVPAFK by the guid
        }
        if (friendsList) soc(kSocBtnRemoveFriend);
    } else if (menu == kMenuPet) {  // UnitPopupMenus.PET (no SET_FOCUS: no focus frame on the PSP)
        const bool abandon = s.petCanBeAbandoned();  // UnitPopup_HideButtons' PET_* rules
        if (abandon) add(bPetPaperdoll);
        if (abandon && s.petCanBeRenamed()) add(bPetRename);
        if (abandon) add(bPetAbandon);
        if (!abandon && s.petCanBeDismissed()) add(bPetDismiss);
    }
    add(bCancel);
    return rows;
}

}  // namespace

void WorldScreen::openUnitPopup(uint64_t guid, uint8_t menu, float x, float y) {
    PartyUi& p = partyUi();
    std::string name;
    if (guid == session_.playerGuid()) {
        name = name_;
    } else if (const game::GroupMember* m = session_.partyMember(guid)) {
        name = m->name;
    } else if (const std::string* n = session_.nameOf(guid)) {
        name = *n;
    }
    if (menu == kMenuTarget) name = btnText(bRaidTargetIcon);  // TargetFrameDropDown_Initialize
    if (menu == kMenuPet) {  // the pet's name (its name query's answer, else the creature's)
        UnitView u;
        name = !session_.petName().empty() ? session_.petName() : unitView(guid, u) ? u.name : std::string();
    }
    const bool canCoop = guid == session_.playerGuid() || session_.partyMember(guid) || !session_.isHostileUnit(guid);
    // UnitPopup_ShowMenu: only the cancel button -> no menu.
    const std::vector<MenuRow> rows = buildMenuRows(session_, menu, guid, name, 1, 0, canCoop,
                                                    inspectMenuEnabled(guid),
                                                    tradeDuelMenuMask(guid));
    int count = 0;
    for (const MenuRow& r : rows)
        if (!r.title && r.btn != bCancel) ++count;
    if (count < 1) {
        if (++p.logs <= 40) PSP_LOGI(pp::tag::Ctrl, "unit_popup empty menu=%u", static_cast<unsigned>(menu));
        return;
    }
    p.menu = menu;
    p.unit = guid;
    p.name = name;
    p.ax = x;
    p.ay = y;
    p.sub = 0;
    p.menuFocusMs = pp::logUptimeMs();
    openWindow(kWinUnitPopup);
    winFocus_.setFocus(kIdMenu1 + 1);  // the first button under the title
    playSoundByName("igMainMenuOpen");
    if (++p.logs <= 40)
        PSP_LOGI(pp::tag::Ctrl, "unit_popup menu=%u rows=%d", static_cast<unsigned>(menu), static_cast<int>(rows.size()));
}

namespace {

// ToggleDropDownMenu anchors: the list's TOPLEFT at the frame's BOTTOMLEFT + (x, y up), frame units.
void frameAnchor(uint32_t id, float& x, float& y) {
    if (id == kIdPlayerFrame) {  // ToggleDropDownMenu(1, nil, PlayerFrameDropDown, "PlayerFrame", 106, 27)
        x = 2.0f + 106.0f * kS;
        y = 2.0f + (100.0f - 27.0f) * kS;
    } else if (id == kIdTargetFrame) {  // (..., TargetFrame, 120, 10)
        x = 2.0f + kFrameW + 4.0f + 120.0f * kS;
        y = 2.0f + (100.0f - 10.0f) * kS;
    } else {  // (..., PartyMemberFrameN, 47, 15)
        const float py = kPartyY0 + static_cast<float>(id - kIdPartyFrame1) * kPartyPitch;
        x = kPartyX + 47.0f * kS;
        y = py + (53.0f - 15.0f) * kS;
    }
}

}  // namespace

bool WorldScreen::partyAltAction(uint32_t alt) {
    if (alt >= kIdMenu1 && alt < kIdMenu2 + 16) return true;  // the menu's buttons take the left click only
    if (alt < kIdPlayerFrame || alt > kIdPartyFrame1 + 3) return false;
    PartyUi& p = partyUi();
    if (p.menu != kMenuNone && p.opener == alt) {  // ToggleDropDownMenu: the same frame again closes it
        closeWindow(kWinUnitPopup);
        p.menu = kMenuNone;
        p.sub = 0;
        return true;
    }
    uint64_t guid = 0;
    uint8_t menu = kMenuNone;
    if (alt == kIdPlayerFrame) {
        guid = session_.playerGuid();
        menu = kMenuSelf;  // PlayerFrameDropDown_Initialize
    } else if (alt == kIdTargetFrame) {
        // TargetFrameDropDown_Initialize: SELF / RAID_PLAYER / PARTY / PLAYER, an NPC: TARGET (the
        // PSP has no pet or vehicle menus).
        guid = session_.target();
        if (guid == session_.playerGuid()) {
            menu = kMenuSelf;
        } else if (auto e = session_.entities().getEntity(guid); e && e->getType() == game::ObjectType::PLAYER) {
            menu = session_.partyMember(guid) ? (session_.isInRaid() ? kMenuRaidPlayer : kMenuParty) : kMenuPlayer;
        } else if (guid != 0 && guid == session_.petGuid()) {
            menu = kMenuPet;  // TargetFrameDropDown_Initialize - UnitIsUnit("target", "pet")
        } else if (guid != 0) {
            menu = kMenuTarget;
        }
    } else if (const game::GroupMember* m = session_.partyUnit(static_cast<int>(alt - kIdPartyFrame1) + 1)) {
        guid = m->guid;
        menu = kMenuParty;  // PartyFrameDropDown_Initialize
    }
    if (guid == 0 || menu == kMenuNone) return true;
    float x = 0, y = 0;
    frameAnchor(alt, x, y);
    if (alt >= kIdPartyFrame1) y += petPartyShift();  // the party frames under PetFrame
    if (p.menu != kMenuNone) closeWindow(kWinUnitPopup);
    p.menu = kMenuNone;
    openUnitPopup(guid, menu, x, y);
    if (p.menu != kMenuNone) p.opener = alt;
    return true;
}

void WorldScreen::partyWindowPad(const pp::PadState& pad) {
    if (!party_ || party_->menu == kMenuNone) return;
    if (!(pad.pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE))) return;
    // UIDropDownMenu: a click anywhere else closes the menus (the click still goes where it was aimed).
    const uint32_t f = winFocus_.focused();
    if ((f >= kIdMenu1 && f < kIdMenu2 + 16) || f == party_->opener) return;
    closeWindow(kWinUnitPopup);
    party_->menu = kMenuNone;
    party_->sub = 0;
}

bool WorldScreen::partyWindowAction(uint32_t act) {
    if (act == kIdInviteAccept || act == kIdInviteDecline) {
        PartyUi& p = partyUi();
        p.inviteAnswered = true;  // OnAccept (inviteAccepted) / OnCancel
        if (act == kIdInviteAccept) session_.acceptGroupInvite();
        else session_.declineGroupInvite();
        closeWindow(kWinPartyInvite);
        p.inviteShown = false;
        return true;
    }
    if (act == kIdRollConfirmOk || act == kIdRollConfirmCancel) {  // OnAccept: ConfirmLootRoll(id, rollType)
        PartyUi& p = partyUi();
        if (act == kIdRollConfirmOk && p.confirmRoll != 0) session_.rollOnLoot(p.confirmRoll, p.confirmType);
        p.confirmRoll = 0;
        closeWindow(kWinLootRollConfirm);
        return true;
    }
    if (act >= kIdRoll0 && act < kIdRoll0 + 16) {  // LootRollButtonTemplate: RollOnLoot(rollID, id)
        PartyUi& p = partyUi();
        const size_t frame = (act - kIdRoll0) / 4;
        const uint32_t b = (act - kIdRoll0) % 4;
        if (frame >= p.rollIds.size()) return true;
        const int rollId = p.rollIds[frame];
        const uint8_t vote = b == 3 ? 0 : static_cast<uint8_t>(b + 1);  // pass 0, need 1, greed 2, disenchant 3
        uint32_t itemId = 0;
        for (const auto& r : session_.lootRolls())
            if (r.rollId == rollId) itemId = r.itemId;
        const game::ItemQueryResponseData* info = session_.getItemInfo(itemId);
        // The client asks first: need / greed on a bind-on-pickup item (CONFIRM_LOOT_ROLL, LOOT_NO_DROP) and
        // disenchant (CONFIRM_DISENCHANT_ROLL, LOOT_NO_DROP_DISENCHANT); ConfirmLootRoll then sends it.
        const bool bop = info && info->bindType == 1;
        if ((vote == 1 || vote == 2) && bop) {
            p.confirmText = fmt1(CLIENT_STR(LOOT_NO_DROP), info->name);
        } else if (vote == 3) {
            p.confirmText = fmt1(CLIENT_STR(LOOT_NO_DROP_DISENCHANT), info ? info->name : std::string());
        } else {
            session_.rollOnLoot(rollId, vote);
            return true;
        }
        p.confirmRoll = rollId;
        p.confirmType = vote;
        openWindow(kWinLootRollConfirm);
        winFocus_.setFocus(kIdRollConfirmOk);
        return true;
    }
    if (act == kIdPlayerFrame) {  // PlayerFrame's left click: TargetUnit("player")
        session_.setTarget(session_.playerGuid());
        return true;
    }
    if (act == kIdTargetFrame) return partyAltAction(act);  // PSP: Cross on TargetFrame opens its menu
    if (act >= kIdPartyFrame1 && act <= kIdPartyFrame1 + 3) {  // SecureUnitButton: TargetUnit("partyN")
        if (const game::GroupMember* m = session_.partyUnit(static_cast<int>(act - kIdPartyFrame1) + 1))
            session_.setTarget(m->guid);
        return true;
    }
    if (act < kIdMenu1 || act >= kIdMenu2 + 16 || !party_) return false;
    PartyUi& p = *party_;
    const bool level2 = act >= kIdMenu2;
    const size_t idx = act - (level2 ? kIdMenu2 : kIdMenu1);
    const std::vector<uint8_t>& rows = level2 ? p.rows2 : p.rows1;
    if (idx >= rows.size() || p.menu == kMenuNone) return true;
    const uint8_t btn = rows[idx];
    const auto& pd = session_.getPartyData();
    auto close = [&]() {
        closeWindow(kWinUnitPopup);
        p.menu = kMenuNone;
        p.sub = 0;
    };
    PSP_LOGI(pp::tag::Ctrl, "unit_popup_click btn=%u", static_cast<unsigned>(btn));
    if (btn >= kSocBtnBase) {  // UnitPopup_OnClick of the social buttons
        const uint64_t unit = p.unit;
        const std::string name = p.name;
        playSoundByName("UChatScrollButton");
        close();
        socialUnitMenuAction(static_cast<uint8_t>(btn - kSocBtnBase), unit, name);
        return true;
    }
    // UnitPopup_OnClick.
    switch (btn) {
        case bLootMethod:
        case bLootThreshold:
        case bRaidTargetIcon:
            p.sub = p.sub == btn ? 0 : btn;  // a nested button: its list (the hover opens it too)
            return true;
        // UnitPopup_OnClick for the raid's buttons.
        case bRaidLeader: session_.promoteToLeader(p.unit); break;           // PromoteToLeader(fullname, 1)
        case bRaidPromote: session_.setGroupAssistant(p.unit, true); break;  // PromoteToAssistant(fullname, 1)
        case bRaidDemote: {
            // DemoteAssistant (the leader, an assistant), then ClearPartyAssignment of the main tank / assist.
            const game::GroupMember* gm = session_.partyMember(p.unit);
            const uint8_t fl = p.unit == session_.playerGuid() ? pd.flags : gm ? gm->flags : 0;
            if (session_.isGroupLeader() && session_.raidRank(p.unit) == 1) session_.setGroupAssistant(p.unit, false);
            if (fl & 0x02) session_.setPartyAssignment(0, p.unit, false);
            else if (fl & 0x04) session_.setPartyAssignment(1, p.unit, false);
            break;
        }
        case bRaidMainTank: session_.setPartyAssignment(0, p.unit, true); break;    // SetPartyAssignment("MAINTANK")
        case bRaidMainAssist: session_.setPartyAssignment(1, p.unit, true); break;  // SetPartyAssignment("MAINASSIST")
        case bRaidRemove: session_.uninvitePlayer(p.name); break;                   // UninviteUnit(fullname)
        case bPvpReportAfk: session_.reportPvpAfk(p.unit); break;                   // ReportPlayerIsPVPAFK
        case bRt1:
        case bRt2:
        case bRt3:
        case bRt4:
        case bRt5:
        case bRt6:
        case bRt7:
        case bRt8: {  // SetRaidTargetIcon(unit, n): the same mark again clears it
            const uint8_t n = static_cast<uint8_t>(btn - bRt1);
            session_.setRaidMark(p.unit, session_.raidMarkOf(p.unit) == n ? pp::WorldSession::kNoRaidMark : n);
            break;
        }
        case bRtNone: session_.setRaidMark(p.unit, pp::WorldSession::kNoRaidMark); break;  // SetRaidTarget(unit, 0)
        case bLootPromote:  // SetLootMethod("master", fullname, 1)
            session_.setLootMethod(2, p.unit, pd.lootThreshold);
            break;
        case bLeave: session_.leaveGroup(); break;  // LeaveParty
        case bWhisper: chat_.openWhisper(p.name); break;  // ChatFrame_SendTell
        case bPromote: session_.promoteToLeader(p.unit); break;  // PromoteToLeader(unit, 1)
        case bUninvite: session_.uninvitePlayer(p.name); break;  // UninviteUnit
        case bInvite: session_.inviteToGroup(p.name); break;  // InviteUnit
        case bInspect: openInspect(p.unit, p.opener == kIdTargetFrame); break;  // InspectUnit(unit)
        case bTrade: session_.initiateTrade(p.unit); break;  // InitiateTrade(unit)
        case bDuel: session_.startDuel(p.unit); break;       // StartDuel(unit, 1)
        case bPetPaperdoll: petMenuAction(0); break;  // PET_PAPERDOLL (ToggleCharacter PetPaperDollFrame)
        case bPetRename: petMenuAction(1); break;     // StaticPopup_Show("RENAME_PET")
        case bPetAbandon: petMenuAction(2); break;    // StaticPopup_Show("ABANDON_PET")
        case bPetDismiss: petMenuAction(3); break;    // PetDismiss()
        case bFfa:
        case bRoundRobin:
        case bMaster:
        case bGroup:
        case bNbg: {
            // SetLootMethod(method, fullname): the master by name among the members, else the player
            // (upstream's Lua SetLootMethod); the threshold as it stands.
            const uint8_t method = static_cast<uint8_t>(btn - bFfa);
            uint64_t master = 0;
            if (method == 2) {
                for (const auto& m : pd.members)
                    if (m.name == p.name) master = m.guid;
                if (master == 0) master = session_.playerGuid();
            }
            session_.setLootMethod(method, master, pd.lootThreshold);
            break;
        }
        case bQ2:
        case bQ3:
        case bQ4:  // SetLootThreshold(id)
            session_.setLootMethod(pd.lootMethod, pd.looterGuid, static_cast<uint8_t>(2 + (btn - bQ2)));
            break;
        default: break;  // CANCEL
    }
    playSoundByName("UChatScrollButton");
    close();
    return true;
}

// UIDropDownMenu (MENU): the tooltip backdrop, UIDROPDOWNMENU_BUTTON_HEIGHT 16 / BORDER_HEIGHT 15 units,
// width = the widest text + 40 (+10 arrow, -30 not checkable) + 25, text GameFontHighlightSmallLeft, the
// title GameFontNormalSmallLeft, disabled GameFontDisableSmallLeft; the highlight UI-QuestTitleHighlight (ADD),
// the check UI-CheckBox-Check, the arrow ChatFrameExpandArrow. PSP: px of the 10-px font (the rows 13 px).
void WorldScreen::drawUnitMenu(UiRenderer& ui) {
    PartyUi& p = partyUi();
    const Font& f = kit_.font(kFontSmall);
    const bool focusable = uiMode_ && !settings_.isOpen();
    const float rowH = static_cast<float>(std::max(12, f.lineHeight()));
    const float border = std::round(15.0f * kS);
    const bool canCoop =
        p.unit == session_.playerGuid() || session_.partyMember(p.unit) || !session_.isHostileUnit(p.unit);
    const uint32_t focused = winFocus_.focused();
    auto drawList = [&](const std::vector<MenuRow>& rows, float x, float y, uint32_t idBase,
                        std::vector<uint8_t>& ids, Rect* rowRects) -> Rect {
        float maxW = 0.0f;
        for (const MenuRow& r : rows) {
            float w = static_cast<float>(f.measure(r.text.c_str())) + std::round(40.0f * kS);
            if (r.nested) w += std::round(10.0f * kS);
            if (!r.checkable) w -= std::round(30.0f * kS);
            if (r.icon >= 0) w += std::round(10.0f * kS);  // "Add padding for the icon"
            maxW = std::max(maxW, w);
        }
        const float listW = maxW + std::round(25.0f * kS);
        const float listH = static_cast<float>(rows.size()) * rowH + 2.0f * border;
        Rect lr{std::clamp(x, 2.0f, static_cast<float>(pp::kScreenW) - listW - 2.0f),
                std::clamp(y, 2.0f, static_cast<float>(pp::kScreenH) - listH - 2.0f), listW, listH};
        // MenuBackdrop: UI-Tooltip-Background / -Border (the kit's tooltip art).
        if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{lr.x + 3, lr.y + 3, lr.w - 6, lr.h - 6}, 0xE0FFFFFFu);
        else ui.fill(lr, 0xE0301717u);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), lr);
        ids.clear();
        for (size_t i = 0; i < rows.size(); ++i) {
            const MenuRow& r = rows[i];
            const float xPos = r.checkable ? std::round(11.0f * kS) : std::round(15.0f * kS);
            const Rect b{lr.x + xPos, lr.y + border + static_cast<float>(i) * rowH, maxW, rowH};
            if (rowRects) rowRects[i] = b;
            const uint32_t id = idBase + static_cast<uint32_t>(i);
            ids.push_back(r.btn);
            const bool f1 = focusable && r.enabled && !r.title && i < 16 && winFocus_.addStop(id, b);
            if (f1 && p.hl.pixels) ui.image(p.hl, b, 0xFFFFFFFFu, Blend::Add);
            const float tx = r.checkable ? b.x + std::round(20.0f * kS) : b.x;
            const float ty = b.y + std::floor((rowH - static_cast<float>(f.lineHeight())) * 0.5f);
            const uint32_t col = r.title ? kColGold : !r.enabled ? kColDisable : r.color;
            ui.textShadow(f, tx, ty, r.text.c_str(), col);
            const float icon = std::round(16.0f * kS);
            if (r.checked) {
                const Rect c{b.x, b.y + std::floor((rowH - icon) * 0.5f), icon, icon};
                if (p.check.pixels) ui.image(p.check, c);
                else ui.textShadow(f, c.x, ty, "v", kColWhite);
            }
            if (r.nested) {
                const Rect a{b.x + b.w - icon, b.y + std::floor((rowH - icon) * 0.5f), icon, icon};
                if (p.arrow.pixels) ui.image(p.arrow, a);
                else ui.textShadow(f, a.x, ty, ">", kColWhite);
            }
            if (r.icon >= 0) {  // the button's icon (16 at RIGHT) - the raid target mark
                if (const pp::Texture* ic = raidTargetIcons()) {
                    const float c = static_cast<float>(ic->uw) * 0.25f;
                    ui.image(*ic, Rect{b.x + b.w - icon, b.y + std::floor((rowH - icon) * 0.5f), icon, icon},
                             SrcRect{static_cast<float>(r.icon % 4) * c, static_cast<float>(r.icon / 4) * c, c, c});
                }
            }
        }
        return lr;
    };
    const std::vector<MenuRow> rows1 =
        buildMenuRows(session_, p.menu, p.unit, p.name, 1, 0, canCoop, inspectMenuEnabled(p.unit),
                      tradeDuelMenuMask(p.unit));
    Rect rr[24];
    const size_t n1 = std::min<size_t>(rows1.size(), 24);
    const std::vector<MenuRow> shown1(rows1.begin(), rows1.begin() + static_cast<std::ptrdiff_t>(n1));
    const Rect l1 = drawList(shown1, p.ax, p.ay, kIdMenu1, p.rows1, rr);
    // UIDropDownMenuButton_OnEnter: a nested button opens its list, another button closes it.
    if (focused >= kIdMenu1 && focused < kIdMenu1 + n1) {
        const MenuRow& r = shown1[focused - kIdMenu1];
        if (r.nested) {
            if (p.sub != r.btn) p.sub = r.btn;
        } else {
            p.sub = 0;
        }
    }
    if (p.sub != 0) {
        float parentTop = l1.y;
        for (size_t i = 0; i < n1; ++i)
            if (shown1[i].btn == p.sub) parentTop = rr[i].y;
        const std::vector<MenuRow> rows2 = buildMenuRows(session_, p.menu, p.unit, p.name, 2, p.sub, canCoop);
        // Level 2: TOPLEFT at the parent button's TOPRIGHT, one border up; on the left when it does not fit.
        float w2 = 0.0f;
        for (const MenuRow& r : rows2)
            w2 = std::max(w2, static_cast<float>(f.measure(r.text.c_str())) + std::round(40.0f * kS) +
                                  (r.icon >= 0 ? std::round(10.0f * kS) : 0.0f));
        w2 += std::round(25.0f * kS);
        float x2 = l1.x + l1.w - 2.0f;
        if (x2 + w2 > static_cast<float>(pp::kScreenW) - 2.0f) x2 = l1.x - w2 + 2.0f;
        drawList(rows2, x2, parentTop - border, kIdMenu2, p.rows2, nullptr);
    } else {
        p.rows2.clear();
    }
}

bool WorldScreen::drawPartyWindow(UiRenderer& ui, uint8_t w) {
    if (w == kWinPartyInvite) {
        PartyUi& p = partyUi();
        StaticPopup sp;
        sp.text = p.inviteText.c_str();
        sp.button1 = CLIENT_STR(ACCEPT);
        sp.button1Id = kIdInviteAccept;
        sp.button2 = CLIENT_STR(DECLINE);
        sp.button2Id = kIdInviteDecline;
        sp.focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
        drawStaticPopup(ui, sp);
        return true;
    }
    if (w == kWinUnitPopup) {
        if (partyUi().menu != kMenuNone) drawUnitMenu(ui);
        return true;
    }
    if (w == kWinGroupLoot) {
        drawGroupLoot(ui);
        return true;
    }
    if (w == kWinLootRollConfirm) {  // StaticPopupDialogs.CONFIRM_LOOT_ROLL
        PartyUi& p = partyUi();
        StaticPopup sp;
        sp.text = p.confirmText.c_str();
        sp.button1 = CLIENT_STR(OKAY);
        sp.button1Id = kIdRollConfirmOk;
        sp.button2 = CLIENT_STR(CANCEL);
        sp.button2Id = kIdRollConfirmCancel;
        sp.focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
        drawStaticPopup(ui, sp);
        return true;
    }
    return false;
}

// --- Frames ----------------------------------------------------------------------------------------

// PlayerLeaderIcon / PlayerMasterIcon (PlayerFrame_UpdatePartyLeader), TargetFrame's leader icon
// (TargetFrame_CheckPartyLeader: UnitIsPartyLeader), and the frame's focus stop (its menu / left click).
void WorldScreen::drawPartyFrameExtras(UiRenderer& ui, float x, float y, bool self) {
    const uint64_t guid = self ? session_.playerGuid() : session_.target();
    if (guid == 0) return;
    if (!self && guid != session_.playerGuid()) {
        UnitView tv;
        if (!unitView(guid, tv)) return;  // no target frame drawn
    }
    PartyUi& p = partyUi();
    if (session_.isInGroup()) {
        const auto& pd = session_.getPartyData();
        const float icon = 16.0f * kS;
        if (self) {
            if (pd.leaderGuid == guid && p.leader.pixels) ui.image(p.leader, Rect{x + 40.0f * kS, y + 12.0f * kS, icon, icon});
            if (pd.lootMethod == 2 && pd.looterGuid == guid && p.master.pixels)
                ui.image(p.master, Rect{x + 80.0f * kS, y + 10.0f * kS, icon, icon});
        } else if (pd.leaderGuid == guid && p.leader.pixels) {
            ui.image(p.leader, Rect{x + (232.0f - 44.0f - 16.0f) * kS, y + 10.0f * kS, icon, icon});
        }
    }
    if (uiMode_ && !settings_.isOpen())
        winFocus_.addStop(self ? kIdPlayerFrame : kIdTargetFrame, Rect{x, y + 6.0f * kS, kFrameW, 80.0f * kS});
}

void WorldScreen::drawPartyFrames(UiRenderer& ui) {
    if (!session_.isInGroup()) return;
    PROF_SCOPE("hud.party");
    PartyUi& p = partyUi();
    const Font& small = kit_.font(kFontSmall);
    const Font& tiny = kit_.font(kFontTiny);
    const bool focusable = uiMode_ && !settings_.isOpen();
    const auto& pd = session_.getPartyData();
    const uint32_t now = pp::logUptimeMs();
    int tipIndex = 0;
    std::string tipName, tipLine;
    Rect tipAnchor{};
    for (int i = 1; i <= 4; ++i) {
        const game::GroupMember* m = session_.partyUnit(i);
        if (!m) break;
        // + petPartyShift() - PetFrame takes the place under PlayerFrame while the pet is out.
        const float x = kPartyX, y = kPartyY0 + petPartyShift() + static_cast<float>(i - 1) * kPartyPitch;
        auto R = [&](float lx, float ly, float lw, float lh) { return Rect{x + lx * kS, y + ly * kS, lw * kS, lh * kS}; };
        // UnitHealth("partyN") & co.: the unit when it is near (its fields), else the group's stats (upstream Lua).
        UnitView u;
        const bool near = unitView(m->guid, u);
        uint32_t hp = near ? u.hp : m->curHealth, maxHp = near ? u.maxHp : m->maxHealth;
        uint32_t pw = near ? u.power : m->curPower, maxPw = near ? u.maxPower : m->maxPower;
        const uint8_t pt = near ? u.powerType : m->powerType;
        const uint32_t level = near && u.level ? u.level : m->level;
        const bool online = m->isOnline != 0;
        const bool ghost = (m->onlineStatus & pp::WorldSession::kMemberGhost) != 0;
        const bool dead = !ghost && (near ? u.dead : ((m->onlineStatus & pp::WorldSession::kMemberDead) != 0 ||
                                                      (hp == 0 && maxHp > 0)));
        uint8_t race = near ? u.race : 0, sex = near ? u.sex : 0, cls = 0;
        if (!near) session_.partyMemberLook(m->guid, race, sex, cls);
        // Portrait (BACKGROUND) - PartyMemberHealthCheck / _UpdateMemberHealth / _UpdateOnlineStatus.
        const float pct = maxHp > 0 ? static_cast<float>(hp) / static_cast<float>(maxHp) : 0.0f;
        uint32_t tint = kColWhite;
        if (!online) tint = 0xFF808080u;               // SetDesaturation (PSP: grey)
        else if (dead) tint = 0xFF595959u;             // (0.35, 0.35, 0.35)
        else if (ghost) tint = 0xFFBF3333u;            // (0.2, 0.2, 0.75)
        else if (pct > 0.0f && pct <= 0.2f) {          // red, the alpha 0.5..1 over 0.5 s each way
            const float t = std::fmod(static_cast<float>(now % 100000u) / 1000.0f, 1.0f);
            const float a = t < 0.5f ? (255.0f - t * 256.0f) / 255.0f : (127.0f + (t - 0.5f) * 256.0f) / 255.0f;
            tint = 0x000000FFu | (static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24);
        }
        const Rect port = R(7, 6, 37, 37);
        const pp::Texture* face = race < 12 && sex < 2 ? &ufArt_.portrait[race][sex] : nullptr;
        if (face && face->pixels) {
            ui.imageCircle(*face, port, tint);
        } else {
            const float in = port.w * 0.1f;
            ui.fillCircle(Rect{port.x + in, port.y + in, port.w - 2 * in, port.h - 2 * in}, 0xC0101010u);
        }
        ui.fill(R(45, 11, 72, 20), 0x80000000u);  // $parentBackground
        auto bar = [&](const Rect& r, uint32_t cur, uint32_t max, uint32_t color) {
            if (max == 0) return;
            const float f = std::clamp(static_cast<float>(cur) / static_cast<float>(max), 0.0f, 1.0f);
            if (f <= 0.0f) return;
            const Rect filled{r.x, r.y, r.w * f, r.h};
            if (ufArt_.bar.pixels)
                ui.image(ufArt_.bar, filled, SrcRect{0, 0, ufArt_.bar.uw * f, static_cast<float>(ufArt_.bar.uh)}, color);
            else ui.fill(filled, color);
        };
        const Rect hpBar = R(47, 12, 70, 8), mpBar = R(47, 21, 70, 8);
        if (!online) bar(hpBar, 1, 1, 0xFF808080u);  // PartyMemberFrame_UpdateOnlineStatus: full, (0.5, 0.5, 0.5)
        else bar(hpBar, dead ? 0 : hp, maxHp, 0xFF00FF00u);
        bar(mpBar, pw, maxPw, powerColor(pt));
        // UI-PartyFrame at 0,-2 (or a plain outline without the art).
        if (p.frame.pixels) {
            ui.image(p.frame, R(0, 2, 128, 64));
        } else {
            const Rect b = R(45, 10, 74, 22);
            ui.fill(Rect{b.x, b.y, b.w, 1}, 0xFF808080u);
            ui.fill(Rect{b.x, b.y + b.h - 1, b.w, 1}, 0xFF808080u);
        }
        // Name: GameFontNormalSmall, BOTTOMLEFT 50,43 (the text's bottom 10 units under the frame's top).
        ui.textShadow(small, x + 50.0f * kS, std::floor(y + 10.0f * kS - 2.0f - static_cast<float>(small.ascent())),
                      m->name.c_str(), kColGold);
        if (pd.leaderGuid == m->guid && p.leader.pixels) ui.image(p.leader, R(0, 0, 16, 16));
        if (pd.lootMethod == 2 && pd.looterGuid == m->guid && p.master.pixels) ui.image(p.master, R(32, 0, 16, 16));
        if (!online && p.disconnect.pixels) ui.image(p.disconnect, R(-7, 26.5f - 32.0f + 1.0f, 64, 64));
        drawPartyReadyCheck(ui, m->guid, port);  // PartyMemberFrameNReadyCheck
        // Debuff1..4 (RefreshDebuffs: the first MAX_PARTY_DEBUFFS harmful auras).
        if (const std::vector<game::AuraSlot>* auras = session_.unitAuras(m->guid)) {
            int k = 0;
            for (const auto& au : *auras) {
                if (au.isEmpty() || !(au.flags & 0x80) || k >= 4) continue;
                const Rect r = R(48.0f + 17.0f * static_cast<float>(k), 32, 15, 15);
                if (const pp::Texture* ic = spellDb_.icon(au.spellId, spellFrame_)) ui.image(*ic, r);
                else if (const pp::Texture* q = itemIcons().fixed(pp::ItemIconDb::kQuestionMark, spellFrame_)) ui.image(*q, r);
                else ui.fill(r, 0xFF404040u);
                if (spellArt_.debuff.pixels) {
                    const pp::SpellInfo* info = spellDb_.get(au.spellId);
                    ui.image(spellArt_.debuff, R(47.0f + 17.0f * static_cast<float>(k), 31, 17, 17),
                             debuffColor(info ? info->rec.dispel : 0));
                }
                ++k;
            }
        }
        // The button (HitRectInsets 7 / 85 / 6 / 7 = the portrait; the bars click it too).
        const bool f = focusable && winFocus_.addStop(kIdPartyFrame1 + static_cast<uint32_t>(i - 1), R(7, 6, 110, 40));
        if (f) {
            // TextStatusBar text under the cursor (TextStatusBarText, the zero text DEAD).
            const float capH = std::round(static_cast<float>(tiny.pixelSize()) * 0.72f);
            auto numY = [&](const Rect& b) {
                return std::floor(b.y + (b.h - capH) * 0.5f + capH - static_cast<float>(tiny.ascent()) + 0.5f);
            };
            char line[48];
            if (online && (dead || (hp == 0 && maxHp > 0))) std::snprintf(line, sizeof(line), "%s", CLIENT_STR(DEAD));
            else if (maxHp > 0) std::snprintf(line, sizeof(line), "%u / %u", static_cast<unsigned>(hp), static_cast<unsigned>(maxHp));
            else line[0] = '\0';
            if (line[0]) ui.textShadow(tiny, hpBar.x + hpBar.w * 0.5f, numY(hpBar), line, kColWhite, Align::Center);
            if (maxPw > 0 && online && !dead) {
                const uint32_t div = (pt == 1 || pt == 6) ? 10u : 1u;
                std::snprintf(line, sizeof(line), "%u / %u", static_cast<unsigned>(pw / div),
                              static_cast<unsigned>(maxPw / div));
                ui.textShadow(tiny, mpBar.x + mpBar.w * 0.5f, numY(mpBar), line, kColWhite, Align::Center);
            }
            tipIndex = i;
            tipName = m->name;
            if (level > 0) tipLine = fmt1(CLIENT_STR(TOOLTIP_UNIT_LEVEL), std::to_string(level));
            tipAnchor = Rect{x + 122.0f * kS, y + 44.0f, 1.0f, 1.0f};
        }
    }
    // UnitFrame_OnEnter: GameTooltip:SetUnit - the name and the level line (PSP: beside the frame).
    if (tipIndex) kit_.tooltip(ui, tipAnchor, tipName.c_str(), tipLine.c_str());
}

// GroupLootFrame1..4 (LootFrame.xml GroupLootFrameTemplate, 253 x 84 units at 0.6): frame 1 BOTTOM 0,60 of
// UIParent (PSP: over the XP bar, y 241), the next ones 15 units above; the dialog backdrop (PSP: the kit's
// panel; no gold variant / dragon for bind on pickup), the item button (slot 3,-3 + icon 34 x 34 at 15,-15), the
// name in its quality colour (90 x 30 at the slot's RIGHT -5,5), RollButton (dice) TOPRIGHT -48,-14, GreedButton
// (coin) under it -2,2, DisenchantButton right of it 3,3 (disabled: alpha 0.35, desaturated - PSP: grey), the
// pass button (UIPanelCloseButton) at the corner, the timer bar 152 x 10 (yellow) at the slot's BOTTOMLEFT 13,10.
void WorldScreen::drawGroupLoot(UiRenderer& ui) {
    PartyUi& p = partyUi();
    loadNpcArt(false);  // the close button (UIPanelCloseButton)
    const Font& small = kit_.font(kFontSmall);
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const uint32_t now = pp::logUptimeMs();
    const float fw = 253.0f * kS, fh = 84.0f * kS;
    p.rollIds.clear();
    uint32_t tipButton = 0;
    Rect tipRect{};
    int k = 0;
    for (const auto& roll : session_.lootRolls()) {
        if (k >= 4) break;  // NUM_GROUP_LOOT_FRAMES: a fifth roll gets no frame (GroupLootFrame_OpenNewFrame)
        const float x = std::floor((static_cast<float>(pp::kScreenW) - fw) * 0.5f);
        const float y = 241.0f - fh - static_cast<float>(k) * (fh + 15.0f * kS);
        auto R = [&](float lx, float ly, float lw, float lh) { return Rect{x + lx * kS, y + ly * kS, lw * kS, lh * kS}; };
        kit_.panel(ui, Rect{x, y, fw, fh});
        const game::ItemQueryResponseData* info = session_.getItemInfo(roll.itemId);
        drawItemButton(ui, R(18, 18, 34, 34), info ? info->displayInfoId : 0, roll.count, -1, kColWhite);
        const Rect nameBox = R(62, 15, 90, 30);
        const std::string name = info && !info->name.empty() ? info->name : std::to_string(roll.itemId);
        const uint32_t q = info ? std::min<uint32_t>(info->quality, 6) : 1;
        if (static_cast<float>(small.measure(name.c_str())) <= nameBox.w)
            ui.textShadow(small, nameBox.x,
                          nameBox.y + std::floor((nameBox.h - static_cast<float>(small.lineHeight())) * 0.5f),
                          name.c_str(), kQualityColor[q]);
        else
            ui.textMarquee(small, nameBox, name.c_str(), kQualityColor[q]);
        // GroupLootFrame_OnUpdate: the time left of the roll.
        const Rect timer = R(16, 57, 152, 10);
        ui.fill(timer, 0xA0000000u);
        const float elapsed = static_cast<float>(now - roll.startedMs);
        const float left =
            roll.countdownMs > 0 ? 1.0f - std::min(1.0f, elapsed / static_cast<float>(roll.countdownMs)) : 0.0f;
        if (left > 0.0f) {
            const Rect fill{timer.x, timer.y, timer.w * left, timer.h};
            if (ufArt_.bar.pixels)
                ui.image(ufArt_.bar, fill, SrcRect{0, 0, ufArt_.bar.uw * left, static_cast<float>(ufArt_.bar.uh)},
                         0xFF00FFFFu);
            else
                ui.fill(fill, 0xFF00FFFFu);
        }
        // The buttons: need (0x02), greed (0x04), disenchant (0x08) by the vote mask; pass always.
        const Rect btn[4] = {R(173, 14, 32, 32), R(171, 44, 32, 32), R(206, 41, 32, 32), R(220, 2, 32, 32)};
        const bool can[4] = {(roll.voteMask & 0x02) != 0, (roll.voteMask & 0x04) != 0, (roll.voteMask & 0x08) != 0,
                             true};
        for (int b = 0; b < 4; ++b) {
            const uint32_t id = kIdRoll0 + static_cast<uint32_t>(k * 4 + b);
            const bool f = focusable && can[b] && winFocus_.addStop(id, btn[b]);
            const bool pressed = f && winFocus_.isPressed(id);
            Rect r = btn[b];
            if (pressed) {
                r.x += 1.0f;
                r.y += 1.0f;
            }
            if (b < 3) {
                const uint32_t tint = can[b] ? kColWhite : 0x59808080u;  // GroupLootFrame_DisableLootButton
                if (p.rollBtn[b].pixels) ui.image(p.rollBtn[b], r, tint);
                else ui.textShadow(small, r.x + r.w * 0.5f, r.y + 4, b == 0 ? "?" : b == 1 ? "$" : "*", tint, Align::Center);
                if (f && p.rollHl[b].pixels) ui.image(p.rollHl[b], btn[b], 0xFFFFFFFFu, Blend::Add);
            } else {
                const pp::Texture& t = pressed && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
                if (t.pixels) ui.image(t, r);
                else ui.textShadow(small, r.x + r.w * 0.5f, r.y + 4, "x", kColWhite, Align::Center);
            }
            if (f) {
                tipButton = static_cast<uint32_t>(b);
                tipRect = btn[b];
            }
        }
        p.rollIds.push_back(roll.rollId);
        ++k;
    }
    // LootRollButtonTemplate OnEnter: GameTooltip with the button's name (PSP: beside it, one line).
    if (tipRect.w > 0.0f)
        kit_.tooltip(ui, Rect{tipRect.x + tipRect.w + 2.0f, tipRect.y + 44.0f, 1, 1}, pp::cs(kRollText[tipButton]), "");
}

// PetFrameDropDown_Initialize -> UnitPopup_ShowMenu(PetFrameDropDown, "PET", "pet"); the same frame
// again closes it (ToggleDropDownMenu).
void WorldScreen::openPetUnitMenu(float x, float y, uint32_t opener) {
    PartyUi& p = partyUi();
    if (p.menu != kMenuNone) {
        const bool same = p.opener == opener;
        closeWindow(kWinUnitPopup);
        p.menu = kMenuNone;
        p.sub = 0;
        if (same) return;
    }
    if (session_.petGuid() == 0) return;
    openUnitPopup(session_.petGuid(), kMenuPet, x, y);
    if (p.menu != kMenuNone) p.opener = opener;
}

// the unit menu of a RaidGroupButton (RaidGroupButton_ShowMenu: ToggleDropDownMenu at the cursor) -
// the same button again closes it (ToggleDropDownMenu), a click elsewhere too (partyWindowPad: the opener).
void WorldScreen::openRaidUnitPopup(uint64_t guid, uint8_t menu, float x, float y, uint32_t opener) {
    PartyUi& p = partyUi();
    if (p.menu != kMenuNone) {
        const bool same = p.opener == opener;
        closeWindow(kWinUnitPopup);
        p.menu = kMenuNone;
        p.sub = 0;
        if (same) return;
    }
    openUnitPopup(guid, menu, x, y);
    if (p.menu != kMenuNone) p.opener = opener;
}

// FriendsFrame_ShowDropdown - a FRIEND / FRIEND_OFFLINE menu for a name (a friend, a who result, a guild
// member; `guid` 0 when only the name is known) at the cursor; the same row again closes it (ToggleDropDownMenu).
void WorldScreen::openNamedUnitPopup(uint64_t guid, const std::string& name, uint8_t menu, float x, float y,
                                     uint32_t opener) {
    PartyUi& p = partyUi();
    if (p.menu != kMenuNone) {
        const bool same = p.opener == opener;
        closeWindow(kWinUnitPopup);
        p.menu = kMenuNone;
        p.sub = 0;
        if (same) return;
    }
    if (pp::social::sameName(name, name_)) menu = static_cast<uint8_t>(menu | 8);
    const std::vector<MenuRow> rows = buildMenuRows(session_, menu, guid, name, 1, 0, true);
    int count = 0;
    for (const MenuRow& r : rows)
        if (!r.title && r.btn != bCancel) ++count;
    if (count < 1) return;
    p.menu = menu;
    p.unit = guid;
    p.name = name;
    p.ax = x;
    p.ay = y;
    p.sub = 0;
    p.menuFocusMs = pp::logUptimeMs();
    p.opener = opener;
    openWindow(kWinUnitPopup);
    winFocus_.setFocus(kIdMenu1 + 1);
    playSoundByName("igMainMenuOpen");
    if (++p.logs <= 40)
        PSP_LOGI(pp::tag::Ctrl, "unit_popup_named menu=%u rows=%d", static_cast<unsigned>(menu), static_cast<int>(rows.size()));
}

}  // namespace wowee::ui::psp
