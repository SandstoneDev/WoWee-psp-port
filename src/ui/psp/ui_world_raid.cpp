// The raid on the world screen, as the original ruRU 3.3.5a client draws it.
//
// FriendsFrame's Raid tab (FriendsFrame.xml: 384 x 512, UI-Character-General quarters, FriendsFrameScrollIcon,
// the title RAID, the close button; RaidFrame.xml + the raid add-on) at the character frame's 0.6 x 0.55
// (ui.pak "rf_frame", build_ui_pak.py RF_*), opened by SOCIAL_BUTTON in the game menu (the original's SOCIAL_BUTTON
// micro button: ToggleFriendsFrame; the friends / who / guild tabs - ui_world_social.cpp, kWinFriends):
//   - not in a raid: RaidFrameConvertToRaidButton (ConvertToRaid: a member, the party leader, level 10+),
//     RaidFrameRaidInfoButton (PSP: always disabled - no saved instance list, GetNumSavedInstances() = 0),
//     RAID_DESCRIPTION / RAID_BROWSER_DESCRIPTION and OPEN_RAID_BROWSER (PSP: disabled, no LFRParentFrame);
//   - in a raid (RaidGroupFrame_Update): RaidGroup1..8 (UI-RaidFrame-GroupOutline, GROUP N, five slots, EMPTY),
//     the members in their subgroups in the roster's order (GetRaidRosterInfo: name / level in the class colour,
//     red when dead, grey offline; the leader / assistant, main tank / main assist and master looter icons; the
//     ready check icon instead of the second one), RaidClassButton1..13 (SpellBook-SkillLineTab +
//     UI-CharacterCreate-Classes / -Pets / -MainTank / -MainAssist, the count, disabled at 0), LOOKING_FOR_RAID
//     (PSP: disabled), RaidFrameReadyCheckButton (DoReadyCheck - the leader / assistants).
//   Controls (PSP, docs/PSP_CONTROLS.md): Cross = the left click (a member: TargetUnit), Circle = the right click
//   (a member: RaidGroupButton_ShowMenu -> UnitPopup RAID at the cursor), Square = the drag: an officer picks a
//   member up and drops him on a slot (SetRaidSubgroup into an empty one, SwapRaidSubgroup onto another member -
//   RaidGroupButton_OnDragStart / _OnDragStop), anyone else (or L + Square = Shift) makes the member's pullout;
//   Square (or Cross) on GROUP N / a class button makes that pullout (RaidPullout_GeneratePulloutFrame).
//   PSP: the name column is the name + level columns' width (the class name is in the tooltip); no
//   TOOLTIP_RAID_*_TIP lines (they describe mouse dragging).
// RaidPullout frames (RaidPulloutFrameTemplate / RaidPulloutButtonTemplate at 0.6): the label, the menu backdrop,
// per member the name in the class colour, health / mana bars in UI-RaidFrame-HealthBar, the target bars
// (showTarget / showTargetTarget: the member's UNIT_FIELD_TARGET in sight), four debuffs (or buffs), the ready
// check icon; RaidPulloutDropDown (Circle on a pullout): SHOW_TARGET, SHOW_TARGET_OF_TARGET_TEXT, SHOW_BUFFS,
// SHOW_DEBUFFS, HIDE_PULLOUT_BG, CLOSE (PSP: no voice rows). PSP: the pullouts stand side by side in one window
// (kWinPullouts - R + stick moves them, the place is kept as the other windows'), this session only.
// ReadyCheckFrame (ReadyCheck.xml, 323 x 97 at the dialogs' 0.8): the initiator's portrait, READY_CHECK_MESSAGE,
// YES / NO (ConfirmReadyCheck), PlaySound("ReadyCheck"); READY_CHECK_FINISHED hides it with
// READY_CHECK_YOU_WERE_AFK when unanswered. The icons (ReadyCheckStatusTemplate: waiting / ready / not ready, the
// unanswered ones "afk" after the end, 10 s + 1.5 s fade) on PlayerFrame (40 at the portrait), PartyMemberFrameN
// (32 at the portrait), the raid buttons and the pullouts - GetReadyCheckStatus, i.e. for the raid's leader /
// assistants and the party leader (the server sends the answers to them only).
// The raid target marks: TargetFrame's raid target icon (26 at TOPRIGHT -73,-14), the icon over every marked unit
// in sight (PSP: 16 px over its name), UnitPopup RAID_TARGET_ICON (ui_world_party.cpp) with these icons.
#include "ui/psp/ui_world_raid_bg.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kSX = 0.6f, kSY = 0.55f;             // build_ui_pak.py RF_SX / RF_SY
constexpr float kOx = 4.0f, kOy = 2.0f;              // FriendsFrame TOPLEFT 0,-104: the left, at the top (as CharacterFrame)
constexpr float kFrameW = 211.0f, kFrameH = 265.0f;  // RF_W x RF_H
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
Rect R(float x, float y, float w, float h) { return Rect{PX(x), PY(y), w * kSX, h * kSY}; }
constexpr float kPo = 0.6f;  // the pullouts' scale (the unit frames')

// GlobalStrings (strings.pak). RaidPulloutDropDown_Initialize's items.
constexpr pp::CS kPulloutMenu[6] = {pp::CS::SHOW_TARGET, pp::CS::SHOW_TARGET_OF_TARGET_TEXT, pp::CS::SHOW_BUFFS,
                                    pp::CS::SHOW_DEBUFFS, pp::CS::HIDE_PULLOUT_BG, pp::CS::CLOSE};
constexpr uint32_t kColDisable = 0xFF808080u;  // GameFontDisableSmall / GRAY_FONT_COLOR
constexpr uint32_t kColRedFont = 0xFF1A1AFFu;  // RED_FONT_COLOR (1, 0.1, 0.1)
constexpr uint32_t kColDarkGray = 0xFF595959u; // GameFontDarkGraySmall (0.35)

// CLASS_SORT_ORDER -> RaidClassButton1..10 (by ChrClasses id).
constexpr uint8_t kClassOrder[10] = {1, 6, 2, 5, 7, 11, 4, 8, 9, 3};
// RAID_CLASS_COLORS (0xAABBGGRR) by class id.
uint32_t classColor(uint8_t cls) {
    switch (cls) {
        case 1: return 0xFF6E9CC7u;   // WARRIOR (0.78, 0.61, 0.43)
        case 2: return 0xFFBA8CF5u;   // PALADIN (0.96, 0.55, 0.73)
        case 3: return 0xFF73D4ABu;   // HUNTER (0.67, 0.83, 0.45)
        case 4: return 0xFF69F5FFu;   // ROGUE (1.0, 0.96, 0.41)
        case 5: return 0xFFFFFFFFu;   // PRIEST
        case 6: return 0xFF3B1FC4u;   // DEATHKNIGHT (0.77, 0.12, 0.23)
        case 7: return 0xFFDE7000u;   // SHAMAN (0.0, 0.44, 0.87)
        case 8: return 0xFFF0CC69u;   // MAGE (0.41, 0.8, 0.94)
        case 9: return 0xFFC98294u;   // WARLOCK (0.58, 0.51, 0.79)
        case 11: return 0xFF0A7DFFu;  // DRUID (1.0, 0.49, 0.04)
        default: return kColGold;
    }
}
// CLASS_ICON_TCOORDS (= WorldStateFrame's CLASS_BUTTONS): the 4 x 4 grid cell of the class.
bool classCell(uint8_t cls, int& col, int& row) {
    switch (cls) {
        case 1: col = 0, row = 0; return true;   // WARRIOR
        case 8: col = 1, row = 0; return true;   // MAGE
        case 4: col = 2, row = 0; return true;   // ROGUE
        case 11: col = 3, row = 0; return true;  // DRUID
        case 3: col = 0, row = 1; return true;   // HUNTER
        case 7: col = 1, row = 1; return true;   // SHAMAN
        case 5: col = 2, row = 1; return true;   // PRIEST
        case 9: col = 3, row = 1; return true;   // WARLOCK
        case 2: col = 0, row = 2; return true;   // PALADIN
        case 6: col = 1, row = 2; return true;   // DEATHKNIGHT
        default: return false;
    }
}

// PowerBarColor (UnitFrame.lua).
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

// "<gold>|cffffffff<white>|r" (RAID_DESCRIPTION, READY_CHECK_MESSAGE): the two parts, the line breaks between
// them dropped (the PSP draws the parts apart).
void splitWhite(const std::string& t, std::string& gold, std::string& white) {
    const size_t c = t.find("|cffffffff");
    gold = t.substr(0, c);
    while (!gold.empty() && (gold.back() == '\n' || gold.back() == ' ')) gold.pop_back();
    white = c == std::string::npos ? std::string() : pp::text::uiPlain(t.substr(c));
}

// RaidGroup frames and slots (the raid add-on's .xml): RaidGroup1 TOPLEFT 16,-70, the even ones 3 right of the odd,
// each pair 14 under the one above; slots 156 x 14 from 3,-3, 1 apart.
float groupX(int g) { return 16.0f + static_cast<float>(g % 2) * (162.0f + 3.0f); }
float groupY(int g) { return 70.0f + static_cast<float>(g / 2) * (80.0f + 14.0f); }
Rect slotRect(int g, int s) { return R(groupX(g) + 3.0f, groupY(g) + 3.0f + static_cast<float>(s) * 15.0f, 156.0f, 14.0f); }
// RaidClassButton13 BOTTOMLEFT at the frame's BOTTOMRIGHT -36,95; the others 12 above each other (18 high).
Rect classButtonRect(int k) {  // k 0..12 = RaidClassButton1..13
    const float bottom = 512.0f - 95.0f - static_cast<float>(12 - k) * 30.0f;
    return R(384.0f - 36.0f, bottom - 18.0f, 18.0f, 18.0f);
}

}  // namespace

// --- Art ----------------------------------------------------------------------------------------

namespace {

void loadRaidFrameArt(WorldScreen::RaidBgUi& r) {
    r.rfTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {
        const struct {
            const char* name;
            pp::Texture* t;
        } list[] = {{"rf_frame", &r.rfFrame}, {"rf_group", &r.rfGroup}, {"rf_btn", &r.rfBtn}, {"rf_btn_hl", &r.rfBtnHl},
                    {"rf_tab", &r.rfTab},     {"rf_classes", &r.rfClasses}, {"rf_pets", &r.rfPets},
                    {"rf_mt_btn", &r.rfMtBtn}, {"rf_ma_btn", &r.rfMaBtn}};
        for (const auto& x : list)
            if (raidBgLoad(pak, x.name, *x.t, r.rfBytes)) ++ok;
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "raid_frame_art ok=%d/9 kb=%u", ok, static_cast<unsigned>(r.rfBytes / 1024u));
}

void freeRaidFrameArt(WorldScreen::RaidBgUi& r) {
    raidBgFree({&r.rfFrame, &r.rfGroup, &r.rfBtn, &r.rfBtnHl, &r.rfTab, &r.rfClasses, &r.rfPets, &r.rfMtBtn, &r.rfMaBtn});
    if (r.rfTried) PSP_LOGI(pp::tag::Gfx, "raid_frame_art_free kb=%u", static_cast<unsigned>(r.rfBytes / 1024u));
    r.rfTried = false;
    r.rfBytes = 0;
}

void loadRaidIcons(WorldScreen::RaidBgUi& r) {
    r.iconsTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {
        const struct {
            const char* name;
            pp::Texture* t;
        } list[] = {{"pf_leader", &r.leader}, {"pf_master", &r.master}, {"rf_assist", &r.assist}, {"rf_mt", &r.mt},
                    {"rf_ma", &r.ma}, {"rf_hpframe", &r.hpFrame}, {"rc_wait", &r.rc[0]}, {"rc_ready", &r.rc[1]},
                    {"rc_notready", &r.rc[2]}, {"rt_icons", &r.rtIcons}};
        for (const auto& x : list)
            if (raidBgLoad(pak, x.name, *x.t, r.iconsBytes)) ++ok;
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "raid_icons ok=%d/10 kb=%u", ok, static_cast<unsigned>(r.iconsBytes / 1024u));
}

void freeRaidIcons(WorldScreen::RaidBgUi& r) {
    raidBgFree({&r.leader, &r.master, &r.assist, &r.mt, &r.ma, &r.hpFrame, &r.rc[0], &r.rc[1], &r.rc[2], &r.rtIcons});
    if (r.iconsTried) PSP_LOGI(pp::tag::Gfx, "raid_icons_free kb=%u", static_cast<unsigned>(r.iconsBytes / 1024u));
    r.iconsTried = false;
    r.iconsBytes = 0;
}

}  // namespace

const pp::Texture* WorldScreen::raidTargetIcons() {
    RaidBgUi& r = raidBg();
    if (!r.iconsTried) loadRaidIcons(r);
    return r.rtIcons.pixels ? &r.rtIcons : nullptr;
}

// --- Per frame (updateRaidBg) -------------------------------------------------------------------------------

void WorldScreen::updateRaid() {
    RaidBgUi& r = raidBg();
    const uint64_t me = session_.playerGuid();
    // The icons (roles, ready check, marks): while in a group, a mark is set or a menu could show them.
    const bool icons = session_.isInGroup() || session_.anyRaidMark() || windowOpen(kWinUnitPopup);
    if (icons && !r.iconsTried) loadRaidIcons(r);
    else if (!icons && r.iconsTried) freeRaidIcons(r);
    // The raid frame's art while it is shown.
    if (windowOpen(kWinRaid) && !r.rfTried) loadRaidFrameArt(r);
    else if (!windowOpen(kWinRaid) && r.rfTried) freeRaidFrameArt(r);
    // RaidGroupFrame_OnHide: a member being moved goes back; not an officer any more / the raid gone: the same.
    if (r.moving >= 0) {
        const bool gone = r.movingGuid != me && !session_.partyMember(r.movingGuid);
        if (!windowOpen(kWinRaid) || !session_.isInRaid() || !(session_.isRaidOfficer() || session_.isGroupLeader()) ||
            gone) {
            r.moving = -1;
            r.movingGuid = 0;
        }
    }
    // READY_CHECK -> ShowReadyCheck(initiator): the listener frame for everyone but the initiator.
    if (session_.readyCheckSeq() != r.rcSeq) {
        r.rcSeq = session_.readyCheckSeq();
        const uint64_t init = session_.readyCheckInitiator();
        if (init != 0) {
            r.rcInitiator = init;
            r.rcName = session_.readyCheckInitiatorName();
            r.rcShown = init != me;
            if (r.rcShown) {
                if (!r.rcTried) {
                    r.rcTried = true;
                    char path[256];
                    pp::fsPath(path, sizeof(path), "data/ui.pak");
                    pp::Pak pak;
                    uint32_t bytes = 0;
                    if (pak.open(path)) {
                        raidBgLoad(pak, "rc_frame", r.rcFrame, bytes);
                        pak.close();
                    }
                }
                openWindow(kWinReadyCheck);
                placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), kWinReadyCheck), placeNext_.end());
                winFocus_.setFocus(kIdRcYes);
                playSoundByName("ReadyCheck");  // ReadyCheckListenerFrame OnShow
                PSP_LOGI(pp::tag::Ctrl, "ready_check_frame from=%s", r.rcName.c_str());
            }
        }
    }
    // READY_CHECK_FINISHED: hidden; READY_CHECK_YOU_WERE_AFK when it was still up (not answered).
    if (session_.readyCheckFinishSeq() != r.rcFinishSeq) {
        r.rcFinishSeq = session_.readyCheckFinishSeq();
        if (r.rcShown && r.rcInitiator != 0 && r.rcInitiator != me)
            session_.addSystemChatMessage(CLIENT_STR(READY_CHECK_YOU_WERE_AFK));
        r.rcShown = false;
        if (windowOpen(kWinReadyCheck)) closeWindow(kWinReadyCheck);
    }
    if (r.rcShown && !windowOpen(kWinReadyCheck)) {  // closed another way (Circle on its title): OnHide - no answer
        r.rcShown = false;
        r.rcInitiator = 0;
    }
    if (!windowOpen(kWinReadyCheck) && r.rcTried) {
        raidBgFree({&r.rcFrame});
        r.rcTried = false;
    }
    // The pullouts: RAID_ROSTER_UPDATE renews them; out of the raid they go (RaidPullout_Update hides an empty one).
    if (!session_.isInRaid()) r.pullouts.clear();
    if (r.pullouts.empty() && windowOpen(kWinPullouts)) closeWindow(kWinPullouts);
    else if (!r.pullouts.empty() && !windowOpen(kWinPullouts)) r.pullouts.clear();  // closed by its title band
    // The pullout dropdown: closed with its pullout, by leaving the interface, UIDROPDOWNMENU_SHOW_TIME away.
    if (r.dropKind == 2) {
        const uint32_t f = winFocus_.focused();
        if (f >= kIdPoMenu0 && f < kIdPoMenu0 + 16) r.dropFocusMs = pp::logUptimeMs();
        if (!windowOpen(kWinDropDown) || r.pulloutMenu < 0 || r.pulloutMenu >= static_cast<int>(r.pullouts.size()) ||
            !uiMode_ || pp::logUptimeMs() - r.dropFocusMs > 2000u) {
            if (windowOpen(kWinDropDown)) closeWindow(kWinDropDown);
            r.dropKind = 0;
            r.pulloutMenu = -1;
        }
    }
}

// --- The roster as RaidGroupFrame_Update reads it ----------------------------------------------------------

namespace {

// What WorldScreen::unitView knows of a unit in sight (the free function below cannot name the private struct).
struct NearView {
    uint32_t level = 0, hp = 0, maxHp = 0, power = 0, maxPower = 0;
    uint8_t powerType = 0;
    bool dead = false;
};

struct RaidRow {
    pp::WorldSession::RaidRosterEntry e;
    uint8_t cls = 0, race = 0, sex = 0, powerType = 0;
    uint32_t level = 0, hp = 0, maxHp = 0, power = 0, maxPower = 0;
    bool dead = false;
};

}  // namespace

static std::vector<RaidRow> raidRows(const pp::WorldSession& s, const std::vector<pp::WorldSession::RaidRosterEntry>& roster,
                                     const std::function<bool(uint64_t, NearView&)>& view);

// --- RaidFrame (window kWinRaid) ---------------------------------------------------------------------------

void WorldScreen::drawRaidMenuButton(UiRenderer& ui, const Rect& r, bool focusable) {
    drawGameMenuButton(ui, kIdRfMenu, r, CLIENT_STR(SOCIAL_BUTTON), focusable);
}

void WorldScreen::openRaidPullout(uint8_t kind, uint32_t id, const std::string& name) {
    RaidBgUi& r = raidBg();
    // RaidPullout_GetFrame: a filter already shown gets no second frame.
    for (const RaidBgUi::Pullout& p : r.pullouts)
        if (p.kind == kind && p.id == id && p.name == name) return;
    if (r.pullouts.size() >= 8) return;  // PSP: eight at most (the window's ids)
    RaidBgUi::Pullout p;
    p.kind = kind;
    p.id = id;
    p.name = name;
    // RaidPullout_GeneratePulloutFrame: the main tank's shows its target and its target's target, the main assist's
    // its target.
    p.showTarget = kind == 3 || kind == 4;
    p.showTargetTarget = kind == 3;
    r.pullouts.push_back(std::move(p));
    if (!windowOpen(kWinPullouts)) {
        openWindow(kWinPullouts);
        placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), kWinPullouts), placeNext_.end());
    }
    playSoundByName("igMainMenuOptionCheckBoxOn");
    PSP_LOGI(pp::tag::Ctrl, "raid_pullout kind=%u id=%u n=%u", static_cast<unsigned>(kind), static_cast<unsigned>(id),
             static_cast<unsigned>(r.pullouts.size()));
}

bool WorldScreen::drawRaidFrame(UiRenderer& ui) {
    PROF_SCOPE("raid.frame");
    RaidBgUi& r = raidBg();
    if (!r.rfTried) loadRaidFrameArt(r);
    if (!r.iconsTried) loadRaidIcons(r);
    loadNpcArt(false);  // the close button, the panel buttons (UIPanelButtonTemplate)
    if (!tabArtTried_) {  // CharacterFrameTabButtonTemplate (the paperdoll loads it the same way)
        tabArtTried_ = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        uint32_t bytes = 0;
        if (pak.open(path)) {
            raidBgLoad(pak, "tab_active", tabArt_[0], bytes);
            raidBgLoad(pak, "tab_inactive", tabArt_[1], bytes);
            pak.close();
        }
    }
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    if (r.rfFrame.pixels) ui.image(r.rfFrame, Rect{kOx, kOy, kFrameW, kFrameH});
    else kit_.panel(ui, Rect{kOx, kOy, kFrameW, kFrameH});
    // FriendsFrameTitleText (TOP 3,-16, 250 x 16): RAID.
    ui.textShadow(fn, PX(195), PY(24) - static_cast<float>(fn.lineHeight()) * 0.5f, CLIENT_STR(RAID), kColGold, Align::Center);
    // FriendsFrameCloseButton (TOPRIGHT -30,-8, 32: CENTER at 338,24).
    {
        const Rect cr{PX(338) - 10, PY(24) - 10, 20, 20};
        const bool f = focusable && winFocus_.addStop(kIdRfClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdRfClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f) ui.fill(cr, 0x40FFFFFFu);
    }
    // FriendsFrameTab1..5 (FRIENDS / WHO / GUILD / CHANNELS / RAID) with RAID selected
    // (ui_world_social.cpp: a tab switch swaps this window for kWinFriends in place).
    drawFriendsTabs(ui, 5, focusable);

    const bool inRaid = session_.isInRaid();
    const bool officer = session_.isRaidOfficer() || (inRaid && session_.isGroupLeader());
    if (!inRaid) {
        // RaidFrame_Update: RaidFrameConvertToRaidButton - a member, the party leader, level 10+ (no LFG restrictions).
        const auto& pd = session_.getPartyData();
        const bool canConvert = !pd.members.empty() && session_.isGroupLeader() &&
                                session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)) >= 10;
        drawPanelButton(ui, kIdRfConvert, R(72, 37, 115, 21), CLIENT_STR(CONVERT_TO_RAID), canConvert, focusable);
        drawPanelButton(ui, kIdRfInfo, R(72 + 115 + 65, 37, 90, 21), CLIENT_STR(RAID_INFO), false, focusable);
        // RaidFrameNotInRaid: RAID_DESCRIPTION (GameFontNormal, 300 wide at 29,-85), RAID_BROWSER_DESCRIPTION 50 under
        // it (centred, white), the raid browser button 10 under that. PSP: the tiny font at an 8 px pitch, half lines
        // between the paragraphs and 50 units -> 5 px - the ruRU text is ~20 lines at 180 px (the frame is 0.55 high).
        const float maxW = 300.0f * kSX;
        constexpr float lh = 8.0f;
        float y = PY(85);
        std::string desc1, desc2;  // RAID_DESCRIPTION: its gold part, then the |cffffffff part
        splitWhite(CLIENT_STR(RAID_DESCRIPTION), desc1, desc2);
        for (const std::string& l : wrapQuestText(ft, desc1, maxW)) {
            ui.textShadow(ft, PX(29), y, l.c_str(), kColGold);
            y += lh;
        }
        y += lh * 0.5f;
        for (const std::string& l : wrapQuestText(ft, desc2, maxW)) {
            if (!l.empty()) ui.textShadow(ft, PX(29), y, l.c_str(), kColWhite);
            y += l.empty() ? lh * 0.5f : lh;
        }
        y += 5.0f;
        for (const std::string& l : wrapQuestText(ft, CLIENT_STR(RAID_BROWSER_DESCRIPTION), maxW)) {
            ui.textShadow(ft, PX(29) + maxW * 0.5f, y, l.c_str(), kColWhite, Align::Center);
            y += lh;
        }
        y += 10.0f * kSY;
        const float bw = 260.0f * kSX;
        drawPanelButton(ui, kIdRfBrowser2, Rect{PX(29) + (maxW - bw) * 0.5f, y, bw, 22.0f * kSY},
                        CLIENT_STR(OPEN_RAID_BROWSER), false, focusable);
        return true;
    }
    // In a raid: RaidFrameRaidBrowserButton (PSP: no LFR - disabled), RaidFrameReadyCheckButton (the officers),
    // RaidFrameRaidInfoButton (beside the ready check button, else beside the hidden ConvertToRaid one).
    drawPanelButton(ui, kIdRfBrowser, R(71, 37, 90, 21), CLIENT_STR(LOOKING_FOR_RAID), false, focusable);
    if (officer) drawPanelButton(ui, kIdRfReadyCheck, R(71 + 90 + 2, 37, 90, 21), CLIENT_STR(READY_CHECK), true, focusable);
    drawPanelButton(ui, kIdRfInfo, officer ? R(163 + 90 + 2, 37, 90, 21) : R(72 + 115 + 65, 37, 90, 21), CLIENT_STR(RAID_INFO),
                    false, focusable);

    // RaidGroupFrame_Update.
    const auto roster = session_.raidRoster();
    std::vector<RaidRow> rows = raidRows(session_, roster, [this](uint64_t g, NearView& n) {
        UnitView u;
        if (!unitView(g, u)) return false;
        n.level = u.level;
        n.hp = u.hp;
        n.maxHp = u.maxHp;
        n.power = u.power;
        n.maxPower = u.maxPower;
        n.powerType = u.powerType;
        n.dead = u.dead;
        return true;
    });
    for (auto& g : r.slots)
        for (auto& s : g) s.guid = 0;
    r.rows.assign(rows.size(), 0);
    int next[8] = {};
    std::vector<int> rowSlot(rows.size(), -1);  // group * 5 + slot of each row (-1: the group was full)
    for (size_t i = 0; i < rows.size(); ++i) {
        const int g = std::min<int>(rows[i].e.subGroup, 7);
        r.rows[i] = rows[i].e.guid;
        if (next[g] >= 5) continue;  // "To prevent errors when the server hiccups"
        r.slots[g][next[g]].guid = rows[i].e.guid;
        rowSlot[i] = g * 5 + next[g];
        ++next[g];
    }
    // The groups: the outline, GROUP N, the empty slots.
    for (int g = 0; g < 8; ++g) {
        const Rect go = R(groupX(g), groupY(g), 162, 80);
        if (r.rfGroup.pixels) ui.image(r.rfGroup, go);
        else ui.fill(Rect{go.x, go.y, go.w, 1}, 0xFF808080u);
        const Rect label = R(groupX(g) + 3, groupY(g) - 14, 156, 14);
        const uint32_t lid = kIdRfLabel0 + static_cast<uint32_t>(g);
        const bool lf = focusable && winFocus_.addStop(lid, label);
        char text[64];
        std::snprintf(text, sizeof(text), "%s %d", CLIENT_STR(GROUP), g + 1);
        // GameFontNormalSmall (PSP: the tiny font - the label's 14 units are 8 px).
        ui.textShadow(ft, label.x + label.w * 0.5f, label.y + (label.h - static_cast<float>(ft.lineHeight())) * 0.5f, text,
                      lf ? kColWhite : kColGold, Align::Center);
        for (int s = 0; s < 5; ++s) {
            const Rect sr = slotRect(g, s);
            const uint32_t sid = kIdRfSlot0 + static_cast<uint32_t>(g * 5 + s);
            if (r.slots[g][s].guid != 0) continue;
            // RaidGroupSlotTemplate: EMPTY, the highlight under a member being moved (LockHighlight).
            const bool sf = focusable && r.moving >= 0 && winFocus_.addStop(sid, sr);
            if (sf && r.rfBtnHl.pixels) ui.image(r.rfBtnHl, sr, 0xFFFFFFFFu, Blend::Add);
            ui.textShadow(ft, sr.x + sr.w * 0.5f, sr.y + (sr.h - static_cast<float>(ft.lineHeight())) * 0.5f, CLIENT_STR(EMPTY),
                          kColDarkGray, Align::Center);
        }
    }
    // The members (RaidGroupButton1..40).
    for (size_t i = 0; i < rows.size() && i < 40; ++i) {
        if (rowSlot[i] < 0) continue;
        const RaidRow& row = rows[i];
        const int g = rowSlot[i] / 5, s = rowSlot[i] % 5;
        const Rect b = slotRect(g, s);
        const uint32_t id = kIdRfMember0 + static_cast<uint32_t>(i);
        const bool f = focusable && winFocus_.addStop(id, b);
        const bool picked = r.moving >= 0 && r.movingGuid == row.e.guid;
        if (r.rfBtn.pixels) ui.image(r.rfBtn, b, picked ? 0xFF80FFFFu : 0xFFFFFFFFu);
        if ((f || picked) && r.rfBtnHl.pixels) ui.image(r.rfBtnHl, b, 0xFFFFFFFFu, Blend::Add);
        // The colour: online - dead red, else the class colour; offline grey.
        uint32_t col = !row.e.online ? kColDisable : row.dead ? kColRedFont : classColor(row.cls);
        // The jobs: rank (leader / assistant), role (main tank / assist), master looter - 11 (10 / 9 with all three).
        const pp::Texture* jobs[3] = {};
        int n = 0;
        if (row.e.rank == 2 && r.leader.pixels) jobs[n++] = &r.leader;
        else if (row.e.rank == 1 && r.assist.pixels) jobs[n++] = &r.assist;
        if ((row.e.flags & 0x02) && r.mt.pixels) jobs[n++] = &r.mt;
        else if ((row.e.flags & 0x04) && r.ma.pixels) jobs[n++] = &r.ma;
        if (row.e.master && r.master.pixels) jobs[n++] = &r.master;
        const bool small = n == 3;
        float jx = b.x + 2.0f * kSX;
        const pp::WorldSession::ReadyState rs = session_.readyCheckState(row.e.guid);
        for (int k = 0; k < n; ++k) {
            const float side = (small ? (k == 2 ? 9.0f : 10.0f) : 11.0f) * kSX;
            const Rect jr{jx, b.y + (b.h - side) * 0.5f, side, side};
            // The ready check icon takes the second job's place (button.jobs[2]:Hide()).
            if (!(rs != pp::WorldSession::ReadyState::None && k == 1)) ui.image(*jobs[k], jr);
            jx += side - 1.0f * kSX;
        }
        if (rs != pp::WorldSession::ReadyState::None) {
            const float side = 11.0f * kSX;
            const float rx = b.x + (2.0f + 11.0f) * kSX;  // LEFT at $parentRank's RIGHT
            drawReadyIcon(ui, row.e.guid, Rect{rx, b.y + (b.h - side) * 0.5f, side, side});
        }
        // Name (LEFT 29; PSP: through the level column's place) and the level (the class's column).
        const Rect nameBox{b.x + 29.0f * kSX, b.y, (55.0f + 2.0f + 18.0f + 2.0f + 30.0f) * kSX, b.h};
        const float ty = b.y + (b.h - static_cast<float>(ft.lineHeight())) * 0.5f;
        ui.pushClip(nameBox);
        ui.textShadow(ft, nameBox.x, ty, row.e.name.empty() ? CLIENT_STR(UNKNOWN) : row.e.name.c_str(), col);
        ui.popClip();
        if (row.level > 0) {
            char lv[16];
            std::snprintf(lv, sizeof(lv), "%u", static_cast<unsigned>(row.level));
            ui.textShadow(ft, b.x + (29.0f + 107.0f + 10.0f) * kSX, ty, lv, col, Align::Center);
        }
        if (f) {  // RaidGroupButton_OnEnter -> UnitFrame_UpdateTooltip (GameTooltip:SetUnit)
            r.tipId = id;
            r.tipRect = Rect{b.x + ui.offsetX(), b.y + ui.offsetY(), b.w, b.h};  // screen (the overlay)
            r.tipTitle = row.e.name.empty() ? CLIENT_STR(UNKNOWN) : row.e.name;
            const RaidBgUi& rr = r;
            std::string cls = row.cls < 12 ? rr.className[row.cls][row.sex == 1 && !rr.className[row.cls][1].empty() ? 1 : 0]
                                           : std::string();
            // TOOLTIP_UNIT_LEVEL_CLASS: the level, then the class (enUS "Level %s %s").
            const std::string line = row.level > 0 ? CLIENT_FMT(TOOLTIP_UNIT_LEVEL_CLASS, std::to_string(row.level), cls) : cls;
            r.tipLine = row.e.online ? line : std::string(CLIENT_STR(PLAYER_OFFLINE));
            r.tipLineColor = kColWhite;
            r.tipLine2.clear();
            if (row.e.rank == 2) r.tipLine2 = CLIENT_STR(RAID_LEADER);
            else if (row.e.rank == 1) r.tipLine2 = CLIENT_STR(RAID_ASSISTANT);
            if (row.e.flags & 0x02) r.tipLine2 += (r.tipLine2.empty() ? "" : ", ") + std::string(CLIENT_STR(MAINTANK));
            else if (row.e.flags & 0x04) r.tipLine2 += (r.tipLine2.empty() ? "" : ", ") + std::string(CLIENT_STR(MAINASSIST));
            if (row.e.master) r.tipLine2 += (r.tipLine2.empty() ? "" : ", ") + std::string(CLIENT_STR(MASTER_LOOTER));
            r.tipLine2Color = kColGold;
        }
    }
    // RaidClassButton1..13 (RaidClassButton_Update): the count per class, the main tank / assist; PETS: PSP has no
    // raid pets (disabled).
    int count[13] = {};
    for (const RaidRow& row : rows) {
        for (int k = 0; k < 10; ++k)
            if (kClassOrder[k] == row.cls) ++count[k];
        if (row.e.flags & 0x02) ++count[11];
        if (row.e.flags & 0x04) ++count[12];
    }
    for (int k = 0; k < 13; ++k) {
        const Rect br = classButtonRect(k);
        const bool enabled = count[k] > 0;
        const uint32_t id = kIdRfClass0 + static_cast<uint32_t>(k);
        if (r.rfTab.pixels) ui.image(r.rfTab, Rect{br.x, br.y, 40.0f * kSX, 40.0f * kSY});
        const Rect icon{br.x + 2.0f * kSX, br.y + 8.0f * kSY, 18.0f * kSX, 18.0f * kSY};
        const uint32_t tint = enabled ? 0xFFFFFFFFu : 0x80808080u;  // alpha 0.5, desaturated (PSP: grey)
        if (k < 10) {
            int col = 0, rowc = 0;
            if (r.rfClasses.pixels && classCell(kClassOrder[k], col, rowc)) {
                const float c = static_cast<float>(r.rfClasses.uw) * 0.25f;
                ui.image(r.rfClasses, icon, SrcRect{static_cast<float>(col) * c, static_cast<float>(rowc) * c, c, c}, tint);
            }
        } else {
            const pp::Texture& t = k == 10 ? r.rfPets : k == 11 ? r.rfMtBtn : r.rfMaBtn;
            if (t.pixels) ui.image(t, icon, tint);
        }
        const bool f = focusable && enabled && winFocus_.addStop(id, Rect{br.x, br.y, 22.0f * kSX, 26.0f * kSY});
        if (f && npcArt_.btnHl.pixels) ui.fill(icon, 0x40FFFFFFu);
        if (enabled && k < 11) {  // the count (NumberFontNormalSmall, BOTTOMRIGHT of the icon +2,0)
            char c[16];
            std::snprintf(c, sizeof(c), "%d", count[k]);
            ui.textShadow(ft, icon.x + icon.w + 2.0f * kSX, icon.y + icon.h - static_cast<float>(ft.lineHeight()) + 1.0f, c,
                          kColWhite, Align::Right);
        }
        if (f) {  // RaidClassButton_OnEnter: "class (n)" and the names
            r.tipId = id;
            r.tipRect = Rect{icon.x + ui.offsetX(), icon.y + ui.offsetY(), icon.w, icon.h};
            const char* title = k < 10    ? nullptr
                                : k == 10 ? CLIENT_STR(PETS)
                                : k == 11 ? CLIENT_STR(MAINTANK)
                                          : CLIENT_STR(MAINASSIST);
            std::string t = title ? title : r.className[kClassOrder[k]][0];
            char head[96];
            std::snprintf(head, sizeof(head), "%s (%d)", t.c_str(), count[k]);
            r.tipTitle = head;
            std::string names;
            for (const RaidRow& row : rows) {
                const bool match = k < 10 ? row.cls == kClassOrder[k] : k == 11 ? (row.e.flags & 0x02) != 0
                                                                     : k == 12 ? (row.e.flags & 0x04) != 0 : false;
                if (match) names += (names.empty() ? "" : ", ") + row.e.name;
            }
            r.tipLine = names;
            r.tipLineColor = kColWhite;
            r.tipLine2.clear();
        }
    }
    return true;
}

// --- ReadyCheckFrame (window kWinReadyCheck) ---------------------------------------------------------------

bool WorldScreen::drawReadyCheckFrame(UiRenderer& ui) {
    RaidBgUi& r = raidBg();
    if (!r.rcShown) return true;
    loadNpcArt(false);
    auto U = [](float units) { return std::floor(units * kDialogScale + 0.5f); };
    const float w = U(323), h = U(97);
    const Rect f{std::floor((static_cast<float>(pp::kScreenW) - w) * 0.5f),
                 std::floor((static_cast<float>(pp::kScreenH) - h) * 0.5f) + U(10), w, h};
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    // ReadyCheckPortrait (50 at 7,-6): SetPortraitTexture(initiator) - PSP: the race / sex portrait (round).
    const Rect pr{f.x + U(7), f.y + U(6), U(50), U(50)};
    uint8_t race = 0, sex = 0, cls = 0;
    if (session_.partyMemberLook(r.rcInitiator, race, sex, cls) && race < 12 && sex < 2 &&
        ufArt_.portrait[race][sex].pixels)
        ui.imageCircle(ufArt_.portrait[race][sex], pr);
    else
        ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    if (r.rcFrame.pixels) ui.image(r.rcFrame, f);
    else kit_.panel(ui, f);
    // ReadyCheckFrameText (GameFontNormal, TOP 15,-24): READY_CHECK_MESSAGE - the name line gold, the question white.
    const Font& fn = kit_.font(kFontNormal);
    const float cx = f.x + U(323 * 0.5f + 15);
    std::string rcLine1, rcLine2;  // READY_CHECK_MESSAGE: the gold line, then the |cffffffff one
    splitWhite(CLIENT_FMT(READY_CHECK_MESSAGE, r.rcName), rcLine1, rcLine2);
    ui.textShadow(fn, cx, f.y + U(24) - 4.0f, rcLine1.c_str(), kColGold, Align::Center);
    ui.textShadow(fn, cx, f.y + U(24) - 4.0f + static_cast<float>(fn.lineHeight()), rcLine2.c_str(), kColWhite,
                  Align::Center);
    // ReadyCheckFrameYesButton (109 x 24 at BOTTOMLEFT 55,18), ReadyCheckFrameNoButton right of it.
    drawPanelButton(ui, kIdRcYes, Rect{f.x + U(55), f.y + h - U(18) - U(24), U(109), U(24)}, CLIENT_STR(YES), true,
                    focusable);
    drawPanelButton(ui, kIdRcNo, Rect{f.x + U(55 + 109), f.y + h - U(18) - U(24), U(109), U(24)}, CLIENT_STR(NO), true,
                    focusable);
    return true;
}

// --- The ready check icon (ReadyCheckStatusTemplate) -------------------------------------------------------

void WorldScreen::drawReadyIcon(UiRenderer& ui, uint64_t guid, const Rect& r) {
    const pp::WorldSession::ReadyState st = session_.readyCheckState(guid);
    if (st == pp::WorldSession::ReadyState::None) return;
    RaidBgUi& rb = raidBg();
    if (!rb.iconsTried) loadRaidIcons(rb);
    const pp::Texture& t = st == pp::WorldSession::ReadyState::Waiting ? rb.rc[0]
                           : st == pp::WorldSession::ReadyState::Ready ? rb.rc[1]
                                                                       : rb.rc[2];  // not ready / afk
    if (!t.pixels) return;
    // ReadyCheck_Finish: 10 s shown, then the alpha down over 1.5 s.
    float a = 1.0f;
    if (const uint32_t fin = session_.readyCheckFinishedMs()) {
        const uint32_t t0 = pp::logUptimeMs() - fin;
        if (t0 > 10000u) a = std::max(0.0f, 1.0f - static_cast<float>(t0 - 10000u) / 1500.0f);
    }
    ui.image(t, r, (static_cast<uint32_t>(a * 255.0f) << 24) | 0x00FFFFFFu);
}

// PlayerFrameReadyCheck (40 at PlayerPortrait's centre), TargetFrame's raid target icon (26 at TOPRIGHT -73,-14).
void WorldScreen::drawRaidFrameExtras(UiRenderer& ui, float x, float y, bool self) {
    constexpr float s = 0.6f;  // the unit frames' scale (ui_world_target.cpp kFrameScale)
    if (self) {
        const float cx = x + 74.0f * s, cy = y + 44.0f * s;  // PlayerPortrait 64 at 42,-12 (mirrored TargetFrame 126)
        drawReadyIcon(ui, session_.playerGuid(), Rect{cx - 20.0f * s, cy - 20.0f * s, 40.0f * s, 40.0f * s});
        return;
    }
    const uint64_t t = session_.target();
    if (t == 0) return;
    const uint8_t mark = session_.raidMarkOf(t);
    if (mark >= 8) return;
    UnitView tv;
    if (t != session_.playerGuid() && !unitView(t, tv)) return;
    if (const pp::Texture* icons = raidTargetIcons()) {
        const float c = static_cast<float>(icons->uw) * 0.25f;
        const float cx = x + (232.0f - 73.0f) * s, cy = y + 14.0f * s;
        ui.image(*icons, Rect{cx - 13.0f * s, cy - 13.0f * s, 26.0f * s, 26.0f * s},
                 SrcRect{static_cast<float>(mark % 4) * c, static_cast<float>(mark / 4) * c, c, c});
    }
}

// PartyMemberFrameNReadyCheck (32 at the portrait's centre).
void WorldScreen::drawPartyReadyCheck(UiRenderer& ui, uint64_t guid, const Rect& portrait) {
    constexpr float s = 0.6f;
    const float cx = portrait.x + portrait.w * 0.5f, cy = portrait.y + portrait.h * 0.5f;
    const game::GroupMember* m = session_.partyMember(guid);
    if (!m || !m->isOnline) return;  // PartyMemberFrame_UpdateReadyCheck: UnitIsConnected
    drawReadyIcon(ui, guid, Rect{cx - 16.0f * s, cy - 16.0f * s, 32.0f * s, 32.0f * s});
}

// The raid target icons over the marked units in sight (PSP: 16 px over the name drawNpcNames puts at sy - 10).
void WorldScreen::drawRaidMarks(UiRenderer& ui) {
    if (!session_.anyRaidMark()) return;
    const pp::Texture* icons = raidTargetIcons();
    if (!icons) return;
    const float c = static_cast<float>(icons->uw) * 0.25f;
    for (uint8_t i = 0; i < 8; ++i) {
        const uint64_t g = session_.raidMarkGuid(i);
        if (g == 0) continue;
        glm::vec3 pos;
        float h = 0.0f, sx = 0.0f, sy = 0.0f;
        if (!unitPosition(g, pos, h)) continue;
        const glm::vec3 d(pos.x - playerX_, pos.y - playerY_, pos.z - playerZ_);
        if (glm::dot(d, d) > 100.0f * 100.0f) continue;
        if (!projectHud(pos + glm::vec3(0, 0, h + 0.4f), sx, sy)) continue;
        ui.image(*icons, Rect{std::floor(sx - 8.0f), std::floor(sy - 10.0f - 18.0f), 16.0f, 16.0f},
                 SrcRect{static_cast<float>(i % 4) * c, static_cast<float>(i / 4) * c, c, c});
    }
}

// --- RaidPullout frames (window kWinPullouts) --------------------------------------------------------------

void WorldScreen::drawRaidPullouts(UiRenderer& ui) {
    RaidBgUi& r = raidBg();
    if (!r.iconsTried) loadRaidIcons(r);
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& ft = kit_.font(kFontTiny);
    const Font& fs = kit_.font(kFontSmall);
    const auto roster = session_.raidRoster();
    std::vector<RaidRow> rows = raidRows(session_, roster, [this](uint64_t g, NearView& n) {
        UnitView u;
        if (!unitView(g, u)) return false;
        n.level = u.level;
        n.hp = u.hp;
        n.maxHp = u.maxHp;
        n.power = u.power;
        n.maxPower = u.maxPower;
        n.powerType = u.powerType;
        n.dead = u.dead;
        return true;
    });
    // PSP: side by side from the right, under the minimap cluster (the window's default place; R + stick moves it).
    const float colW = 92.0f * kPo + 6.0f;
    float x = static_cast<float>(pp::kScreenW) - 4.0f - colW * static_cast<float>(r.pullouts.size());
    const float y0 = 120.0f;
    for (size_t pi = 0; pi < r.pullouts.size(); ++pi, x += colW) {
        RaidBgUi::Pullout& p = r.pullouts[pi];
        // RAID_SUBGROUP_LISTS[filterID]: the members of the group / class / role, or the one player.
        p.members.clear();
        for (const RaidRow& row : rows) {
            bool in = false;
            switch (p.kind) {
                case 1: in = row.e.subGroup + 1u == p.id; break;
                case 2: in = row.cls == p.id; break;
                case 3: in = (row.e.flags & 0x02) != 0; break;
                case 4: in = (row.e.flags & 0x04) != 0; break;
                case 6: in = row.e.name == p.name; break;
                default: break;
            }
            if (in) p.members.push_back(row.e.guid);
        }
        const float btnH = (33.0f + (p.showTarget ? 15.0f : 0.0f)) * kPo;
        const float h = static_cast<float>(p.members.size()) * btnH + 14.0f * kPo;
        const Rect fr{x, y0, 92.0f * kPo, h};
        if (pi < 8) r.poRect[pi] = Rect{fr.x + ui.offsetX(), fr.y + ui.offsetY(), fr.w, fr.h};
        // The label (GameFontNormalSmall, BOTTOM at the frame's TOP): GROUP n / the class / none for a player.
        std::string label;
        if (p.kind == 1) label = std::string(CLIENT_STR(GROUP)) + " " + std::to_string(p.id);
        else if (p.kind == 2) label = p.id < 12 ? r.className[p.id][0] : std::string();
        else if (p.kind == 3) label = CLIENT_STR(MAINTANK);
        else if (p.kind == 4) label = CLIENT_STR(MAINASSIST);
        const uint32_t tid = kIdPoTitle0 + static_cast<uint32_t>(pi);
        const bool tf = focusable && winFocus_.addStop(tid, Rect{fr.x, fr.y - 13.0f * kPo, fr.w, 13.0f * kPo + 6.0f});
        if (!label.empty())
            ui.textShadow(fs, fr.x + fr.w * 0.5f, fr.y - static_cast<float>(fs.lineHeight()), label.c_str(),
                          tf ? kColWhite : kColGold, Align::Center);
        // $parentMenuBackdrop: the tooltip backdrop at 0.7 (HIDE_PULLOUT_BG hides it).
        if (p.showBg) {
            if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{fr.x + 3, fr.y + 3, fr.w - 6, fr.h - 6}, 0xB3FFFFFFu);
            else ui.fill(fr, 0xB3000000u);
            if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), fr, 1.0f, 0xB3808080u);
        }
        for (size_t k = 0; k < p.members.size(); ++k) {
            const uint64_t g = p.members[k];
            const RaidRow* row = nullptr;
            for (const RaidRow& rr : rows)
                if (rr.e.guid == g) row = &rr;
            if (!row) continue;
            // RaidPulloutButtonTemplate: TOP 1,-10 of the frame, 8 apart; the name 65 x 12 at TOPLEFT 2,5 (over the
            // bars), HealthBar 68 x 6 at 1,-11, ManaBar under it, Target 68 x 14 3 under that.
            const float bx = fr.x + (92.0f - 70.0f) * 0.5f * kPo + 1.0f * kPo;
            const float by = fr.y + 10.0f * kPo + static_cast<float>(k) * btnH;
            const Rect bb{bx, by, 70.0f * kPo, btnH - 8.0f * kPo};
            const uint32_t id = kIdPoButton0 + static_cast<uint32_t>(pi * 7 + k);
            const bool f = focusable && k < 7 && winFocus_.addStop(id, bb);  // PSP: seven buttons a pullout take focus
            const uint32_t col = !row->e.online ? kColDisable : row->dead ? kColRedFont : classColor(row->cls);
            ui.pushClip(Rect{bx + 2.0f * kPo, by - 5.0f * kPo, 65.0f * kPo, 12.0f * kPo + 4.0f});
            ui.textShadow(ft, bx + 2.0f * kPo, by - 5.0f * kPo, row->e.name.c_str(), f ? kColWhite : col);
            ui.popClip();
            auto bar = [&](const Rect& br, uint32_t cur, uint32_t max, uint32_t c) {
                ui.fill(br, 0x54000000u);  // $parentBackground (0, 0, 0, 0.33)
                if (max > 0 && cur > 0) {
                    const float fr2 = std::clamp(static_cast<float>(cur) / static_cast<float>(max), 0.0f, 1.0f);
                    const Rect fill{br.x, br.y, br.w * fr2, br.h};
                    if (ufArt_.bar.pixels)
                        ui.image(ufArt_.bar, fill, SrcRect{0, 0, ufArt_.bar.uw * fr2, static_cast<float>(ufArt_.bar.uh)}, c);
                    else ui.fill(fill, c);
                }
                if (r.hpFrame.pixels) ui.image(r.hpFrame, Rect{br.x - 1.0f * kPo, br.y - 1.0f * kPo, 70.0f * kPo, 8.0f * kPo});
            };
            const Rect hp{bx + 1.0f * kPo, by + 11.0f * kPo, 68.0f * kPo, 6.0f * kPo};
            const Rect mp{hp.x, hp.y + 7.0f * kPo, hp.w, hp.h};
            if (!row->e.online) bar(hp, 1, 1, 0xFF808080u);
            else bar(hp, row->dead ? 0 : row->hp, row->maxHp, 0xFF00FF00u);
            bar(mp, row->power, row->maxPower, powerColor(row->powerType));
            // The member's target (and its target) in sight: RaidPullout_UpdateTarget - the name, the health in
            // UnitSelectionColor (PSP: the reaction colour of the target circle).
            if (p.showTarget) {
                const Rect tb{hp.x, mp.y + mp.h + 3.0f * kPo, 68.0f * kPo, 14.0f * kPo};
                uint64_t tg = 0;
                if (g == session_.playerGuid()) {
                    tg = session_.target();
                } else if (auto e = session_.entities().getEntity(g)) {
                    tg = static_cast<uint64_t>(e->getField(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO))) |
                         (static_cast<uint64_t>(e->getField(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI))) << 32);
                }
                UnitView tu;
                if (tg != 0 && unitView(tg, tu)) {
                    bar(tb, tu.dead ? 0 : tu.hp, tu.maxHp, selectionColor(tg));
                    ui.pushClip(tb);
                    ui.textShadow(ft, tb.x + tb.w * 0.5f, tb.y, tu.name.c_str(), kColGold, Align::Center);
                    ui.popClip();
                } else {
                    bar(tb, 1, 1, 0xFF808080u);
                }
            }
            // ReadyCheck (18 at the button's RIGHT +8); else the auras (four debuffs - or buffs - 12 each, two rows).
            const Rect rc{bb.x + bb.w + 8.0f * kPo, by + (14.0f - 9.0f) * kPo, 18.0f * kPo, 18.0f * kPo};
            if (session_.readyCheckState(g) != pp::WorldSession::ReadyState::None) {
                drawReadyIcon(ui, g, rc);
            } else if (const std::vector<game::AuraSlot>* auras = session_.unitAuras(g)) {
                int a = 0;
                for (const auto& au : *auras) {
                    if (au.isEmpty() || a >= 4 || (((au.flags & 0x80) != 0) == p.showBuffs)) continue;
                    const Rect ar{bb.x + bb.w + 8.0f * kPo + static_cast<float>(a % 2) * 13.0f * kPo,
                                  by - 4.0f * kPo + static_cast<float>(a / 2) * 13.0f * kPo, 12.0f * kPo, 12.0f * kPo};
                    if (const pp::Texture* ic = spellDb_.icon(au.spellId, spellFrame_)) ui.image(*ic, ar);
                    ++a;
                }
            }
            if (f) {
                r.tipId = id;
                r.tipRect = Rect{bb.x + ui.offsetX(), bb.y + ui.offsetY(), bb.w, bb.h};
                r.tipTitle = row->e.name;
                r.tipLine = row->e.online ? (row->level > 0 ? CLIENT_FMT(TOOLTIP_UNIT_LEVEL, std::to_string(row->level))
                                                            : std::string())
                                          : std::string(CLIENT_STR(PLAYER_OFFLINE));
                r.tipLineColor = kColWhite;
                r.tipLine2.clear();
            }
        }
    }
}

// RaidPulloutDropDown (kWinDropDown, dropKind 2): UIDropDownMenu MENU at the pullout's TOPRIGHT.
bool WorldScreen::drawPulloutMenu(UiRenderer& ui) {
    RaidBgUi& r = raidBg();
    if (r.pulloutMenu < 0 || r.pulloutMenu >= static_cast<int>(r.pullouts.size())) return true;
    const RaidBgUi::Pullout& p = r.pullouts[static_cast<size_t>(r.pulloutMenu)];
    const Font& fs = kit_.font(kFontSmall);
    const bool focusable = uiMode_ && !settings_.isOpen();
    // RaidPulloutDropDown_Initialize: SHOW_TARGET, SHOW_TARGET_OF_TARGET_TEXT (when showTarget), SHOW_BUFFS,
    // SHOW_DEBUFFS, HIDE_PULLOUT_BG, CLOSE.
    r.pulloutMenuRows.clear();
    r.pulloutMenuRows.push_back(0);
    if (p.showTarget) r.pulloutMenuRows.push_back(1);
    for (uint8_t k = 2; k < 6; ++k) r.pulloutMenuRows.push_back(k);
    const float rowH = static_cast<float>(std::max(12, fs.lineHeight()));
    const float border = std::round(15.0f * kPo);
    float maxW = 0.0f;
    for (uint8_t k : r.pulloutMenuRows)
        maxW = std::max(maxW, static_cast<float>(fs.measure(pp::cs(kPulloutMenu[k]))) + std::round(40.0f * kPo));
    const float w = maxW + std::round(25.0f * kPo), h = static_cast<float>(r.pulloutMenuRows.size()) * rowH + 2.0f * border;
    // UIDropDownMenu_SetAnchor(self, 0, 0, "TOPLEFT", the pullout, "TOPRIGHT"); on the left when it does not fit.
    const Rect& po = r.poRect[std::min(r.pulloutMenu, 7)];
    float px = po.x + po.w;
    if (px + w > static_cast<float>(pp::kScreenW) - 2.0f) px = po.x - w;
    const Rect lr{std::clamp(px, 2.0f, static_cast<float>(pp::kScreenW) - w - 2.0f),
                  std::clamp(po.y, 2.0f, static_cast<float>(pp::kScreenH) - h - 2.0f), w, h};
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{lr.x + 3, lr.y + 3, lr.w - 6, lr.h - 6}, 0xE0FFFFFFu);
    else ui.fill(lr, 0xE0301717u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), lr);
    loadNpcArt(false);
    for (size_t i = 0; i < r.pulloutMenuRows.size(); ++i) {
        const uint8_t k = r.pulloutMenuRows[i];
        const Rect b{lr.x + std::round(11.0f * kPo), lr.y + border + static_cast<float>(i) * rowH, maxW, rowH};
        const bool f = focusable && winFocus_.addStop(kIdPoMenu0 + static_cast<uint32_t>(i), b);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, b, 0xFFFFFFFFu, Blend::Add);
        else if (f) ui.fill(b, 0x30FFFFFFu);
        const bool checked = (k == 0 && p.showTarget) || (k == 1 && p.showTargetTarget) || (k == 2 && p.showBuffs) ||
                             (k == 3 && !p.showBuffs) || (k == 4 && !p.showBg);
        const float ty = b.y + std::floor((rowH - static_cast<float>(fs.lineHeight())) * 0.5f);
        if (checked) ui.textShadow(fs, b.x, ty, "v", kColWhite);
        ui.textShadow(fs, b.x + std::round(20.0f * kPo), ty, pp::cs(kPulloutMenu[k]), kColWhite);
    }
    return true;
}

// --- Actions -------------------------------------------------------------------------------------------------

bool WorldScreen::raidFrameAction(uint32_t act, bool alt) {
    RaidBgUi& r = raidBg();
    const uint64_t me = session_.playerGuid();
    // ReadyCheckFrame: ConfirmReadyCheck(1) / ConfirmReadyCheck() and Hide.
    if (act == kIdRcYes || act == kIdRcNo) {
        if (alt) return true;
        session_.confirmReadyCheck(act == kIdRcYes);
        r.rcShown = false;
        r.rcInitiator = 0;  // ReadyCheckFrame_OnHide
        closeWindow(kWinReadyCheck);
        return true;
    }
    if (act == kIdRfClose) {
        closeWindow(kWinRaid);
        playSoundByName("igMainMenuClose");  // FriendsFrame_OnHide
        return true;
    }
    if (act == kIdRfMenu) {  // the game menu's SOCIAL_BUTTON: ToggleFriendsFrame() (on the last tab)
        if (alt) return true;
        closeWindow(kWinMenu);
        toggleFriendsFrame(0);
        return true;
    }
    if (act == kIdRfConvert) {
        if (!alt) session_.convertToRaid();
        return true;
    }
    if (act == kIdRfReadyCheck) {  // DoReadyCheck(); PlaySound("UChatScrollButton")
        if (!alt) {
            session_.doReadyCheck();
            playSoundByName("UChatScrollButton");
        }
        return true;
    }
    if (act == kIdRfInfo || act == kIdRfBrowser || act == kIdRfBrowser2) return true;  // disabled (PSP)
    if (act >= kIdRfLabel0 && act < kIdRfLabel0 + 8) {  // the label's drag: RaidPullout_GeneratePulloutFrame(group)
        openRaidPullout(1, act - kIdRfLabel0 + 1, std::string());
        return true;
    }
    if (act >= kIdRfClass0 && act < kIdRfClass0 + 13) {  // RaidClassButton's drag: the class / role pullout
        const uint32_t k = act - kIdRfClass0;
        if (k < 10) openRaidPullout(2, kClassOrder[k], std::string());
        else if (k == 11) openRaidPullout(3, 0, std::string());
        else if (k == 12) openRaidPullout(4, 0, std::string());
        return true;
    }
    if (act >= kIdRfSlot0 && act < kIdRfSlot0 + 40) {  // an empty slot: drop the member being moved there
        if (r.moving < 0 || alt) return true;
        const int g = static_cast<int>(act - kIdRfSlot0) / 5;
        const game::GroupMember* m = session_.partyMember(r.movingGuid);
        const std::string name = r.movingGuid == me ? name_ : m ? m->name : std::string();
        const int cur = r.movingGuid == me ? session_.getPartyData().subGroup : m ? m->subGroup : -1;
        if (!name.empty() && cur != g) session_.setRaidSubgroup(name, static_cast<uint8_t>(g + 1));  // SetRaidSubgroup
        r.moving = -1;
        r.movingGuid = 0;
        return true;
    }
    if (act >= kIdRfMember0 && act < kIdRfMember0 + 40) {
        const size_t i = act - kIdRfMember0;
        if (i >= r.rows.size()) return true;
        const uint64_t g = r.rows[i];
        if (alt) {  // RaidGroupButton_ShowMenu: UnitPopup RAID at the cursor
            openRaidUnitPopup(g, kUnitMenuRaid, winFocus_.cursorX(), winFocus_.cursorY(), act);
            return true;
        }
        if (r.moving >= 0) {  // a member picked up: dropped onto another one (SwapRaidSubgroup), or put back
            if (r.movingGuid != g) {
                auto nameOf = [&](uint64_t guid) -> std::string {
                    if (guid == me) return name_;
                    const game::GroupMember* m = session_.partyMember(guid);
                    return m ? m->name : std::string();
                };
                const game::GroupMember* a = session_.partyMember(r.movingGuid);
                const game::GroupMember* b = session_.partyMember(g);
                const uint8_t ga = r.movingGuid == me ? session_.getPartyData().subGroup : a ? a->subGroup : 0;
                const uint8_t gb = g == me ? session_.getPartyData().subGroup : b ? b->subGroup : 0;
                if (ga != gb) session_.swapRaidSubgroup(nameOf(r.movingGuid), nameOf(g));
            }
            r.moving = -1;
            r.movingGuid = 0;
            return true;
        }
        session_.setTarget(g);  // SecureUnitButton_OnClick: TargetUnit("raidN")
        return true;
    }
    // A pullout's frame: its RaidPulloutDropDown (the right click; Cross too - PSP).
    if (act >= kIdPoTitle0 && act < kIdPoTitle0 + 8) {
        const int pi = static_cast<int>(act - kIdPoTitle0);
        if (pi >= static_cast<int>(r.pullouts.size())) return true;
        if (r.dropKind == 2 && r.pulloutMenu == pi && windowOpen(kWinDropDown)) {
            closeWindow(kWinDropDown);
            r.dropKind = 0;
            r.pulloutMenu = -1;
            return true;
        }
        if (windowOpen(kWinDropDown)) closeWindow(kWinDropDown);
        r.dropKind = 2;
        r.pulloutMenu = pi;
        r.dropFocusMs = pp::logUptimeMs();
        openWindow(kWinDropDown);
        winFocus_.setFocus(kIdPoMenu0);
        playSoundByName("igMainMenuOpen");
        return true;
    }
    if (act >= kIdPoButton0 && act < kIdPoButton0 + 56) {
        const size_t pi = (act - kIdPoButton0) / 7, k = (act - kIdPoButton0) % 7;
        if (pi >= r.pullouts.size() || k >= r.pullouts[pi].members.size()) return true;
        const uint64_t g = r.pullouts[pi].members[k];
        if (alt) {  // RaidPulloutButton_ShowMenu: the pullout's dropdown (SecureUnitButton "menu")
            return raidFrameAction(kIdPoTitle0 + static_cast<uint32_t>(pi), false);
        }
        session_.setTarget(g);
        return true;
    }
    if (act >= kIdPoMenu0 && act < kIdPoMenu0 + 16) {
        if (alt || r.pulloutMenu < 0 || r.pulloutMenu >= static_cast<int>(r.pullouts.size())) return true;
        const size_t i = act - kIdPoMenu0;
        if (i >= r.pulloutMenuRows.size()) return true;
        RaidBgUi::Pullout& p = r.pullouts[static_cast<size_t>(r.pulloutMenu)];
        switch (r.pulloutMenuRows[i]) {
            case 0: p.showTarget = !p.showTarget; break;
            case 1: p.showTargetTarget = !p.showTargetTarget; break;
            case 2: p.showBuffs = true; break;
            case 3: p.showBuffs = false; break;
            case 4: p.showBg = !p.showBg; break;
            case 5:  // CLOSE
                r.pullouts.erase(r.pullouts.begin() + r.pulloutMenu);
                break;
            default: break;
        }
        closeWindow(kWinDropDown);
        r.dropKind = 0;
        r.pulloutMenu = -1;
        playSoundByName("UChatScrollButton");
        return true;
    }
    return false;
}

// Square = the drag (RaidGroupButton_OnDragStart / _OnDragStop, RaidPulloutButton_OnDragStart); a click outside an
// open pullout dropdown closes it (UIDropDownMenu).
void WorldScreen::raidBgWindowPad(const pp::PadState& pad) {
    if (!rbg_) return;
    RaidBgUi& r = *rbg_;
    const uint32_t f = winFocus_.focused();
    if (r.dropKind != 0 && (pad.pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE))) {
        const bool onMenu = (r.dropKind == 2 && ((f >= kIdPoMenu0 && f < kIdPoMenu0 + 16) || (f >= kIdPoTitle0 && f < kIdPoTitle0 + 8))) ||
                            (r.dropKind == 1 && ((f >= kIdMmBgMenu0 && f < kIdMmBgMenu0 + 8) || f == kIdMmBattlefield));
        if (!onMenu && windowOpen(kWinDropDown)) {
            closeWindow(kWinDropDown);
            r.dropKind = 0;
            r.pulloutMenu = -1;
        }
    }
    if (!(pad.pressed & PSP_CTRL_SQUARE) || !uiMode_ || settings_.isOpen()) return;
    if (f >= kIdRfLabel0 && f < kIdRfLabel0 + 8) {
        raidFrameAction(f, false);
        return;
    }
    if (f >= kIdRfClass0 && f < kIdRfClass0 + 13) {
        raidFrameAction(f, false);
        return;
    }
    if (f >= kIdRfSlot0 && f < kIdRfSlot0 + 40) {
        raidFrameAction(f, false);
        return;
    }
    if (f < kIdRfMember0 || f >= kIdRfMember0 + 40) return;
    const size_t i = f - kIdRfMember0;
    if (i >= r.rows.size()) return;
    const uint64_t g = r.rows[i];
    const bool officer = session_.isRaidOfficer() || session_.isGroupLeader();
    const bool shift = (pad.held & PSP_CTRL_LTRIGGER) != 0;  // L = Shift (as the tooltips' compare key)
    if (!officer || shift) {  // RaidPulloutButton_OnDragStart(RaidPullout_GeneratePulloutFrame(self.name))
        const game::GroupMember* gm = session_.partyMember(g);
        const std::string name = g == session_.playerGuid() ? name_ : gm ? gm->name : std::string();
        if (!name.empty()) openRaidPullout(6, 0, name);
        return;
    }
    if (r.moving >= 0) {  // dropped on a member (the same: put back)
        raidFrameAction(f, false);
        return;
    }
    r.moving = static_cast<int>(i);  // RaidGroupButton_OnDragStart: MOVING_RAID_MEMBER (SetRaidRosterSelection)
    r.movingGuid = g;
    playSoundByName("igMainMenuOptionCheckBoxOn");
}

// --- The roster rows ------------------------------------------------------------------------------------------

static std::vector<RaidRow> raidRows(const pp::WorldSession& s, const std::vector<pp::WorldSession::RaidRosterEntry>& roster,
                                     const std::function<bool(uint64_t, NearView&)>& view) {
    std::vector<RaidRow> rows;
    rows.reserve(roster.size());
    for (const auto& e : roster) {
        RaidRow row;
        row.e = e;
        NearView u;
        const bool near = view(e.guid, u);
        const game::GroupMember* m = s.partyMember(e.guid);
        if (e.self) {
            if (const game::Character* c = s.activeCharacter()) {
                row.cls = static_cast<uint8_t>(c->characterClass);
                row.race = static_cast<uint8_t>(c->race);
                row.sex = c->gender == game::Gender::FEMALE ? 1 : 0;
            }
        } else {
            s.partyMemberLook(e.guid, row.race, row.sex, row.cls);
        }
        if (near) {
            row.level = u.level;
            row.hp = u.hp;
            row.maxHp = u.maxHp;
            row.power = u.power;
            row.maxPower = u.maxPower;
            row.powerType = u.powerType;
            row.dead = u.dead;
            if (auto ent = s.entities().getEntity(e.guid)) {
                const uint32_t b0 = ent->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
                if (row.cls == 0) row.cls = static_cast<uint8_t>((b0 >> 8) & 0xFF);
            }
        } else if (m) {
            row.level = m->level;
            row.hp = m->curHealth;
            row.maxHp = m->maxHealth;
            row.power = m->curPower;
            row.maxPower = m->maxPower;
            row.powerType = m->powerType;
            row.dead = (m->onlineStatus & (pp::WorldSession::kMemberDead | pp::WorldSession::kMemberGhost)) != 0 ||
                       (m->curHealth == 0 && m->maxHealth > 0);
        }
        rows.push_back(row);
    }
    return rows;
}

}  // namespace wowee::ui::psp
