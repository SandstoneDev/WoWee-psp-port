// The Dungeon Finder of the original 3.3.5a client (LFGFrame.xml / .lua,
// LFDFrame.xml / .lua, Minimap.xml / .lua MiniMapLFGFrame, AlertFrames.xml DungeonCompletionAlertFrame,
// StaticPopup.lua VOTE_BOOT_PLAYER / LFG_OFFER_CONTINUE) over the session's LFG half (psp_world_session_lfg.cpp:
// upstream SocialHandler's) and the GetLFG* bindings of upstream's lua_lfg_api.cpp (GetLFDChoiceOrder,
// GetLFGProposal, GetLFGQueueStats ...). Also the shared pieces of the three 3.3.5a windows: the hooks of
// ui_world_windows.cpp, the dropdown lists, the deferred GameTooltip, the game menu buttons.
//
// LFDParentFrame (355 x 440, the left panel) at the trainer's 0.6 x 0.55 of its units, top left of the screen:
//   - the frame (UI-LFG-FRAME + the type's UI-LFG-BACKGROUND-* + UI-LFG-PORTRAIT, data/wotlk.pak lfd_<kind>), the
//     portrait's eye (EyeTemplate: animated while queued or in a role check), LOOKING_FOR_DUNGEON, the close button;
//   - the role buttons Tank / Healer / Damage (LFGRoleButtonWithBackgroundTemplate) and Leader (LFGRoleButtonTemplate)
//     with their check boxes: the class's roles (GetAvailableRoles), locked while queued (LFG_UpdateRolesChangeable),
//     ROLE_DESCRIPTION* / GUIDE_TOOLTIP and YOUR_CLASS_MAY_NOT_PERFORM_ROLE in the tooltip;
//   - LFDQueueFrameTypeDropDown CHOOSE_YOUR_DUNGEON - SPECIFIC_DUNGEONS and the random dungeons the server offers
//     (SMSG_LFG_PLAYER_INFO) for the level; a locked one disabled with YOU_MAY_NOT_QUEUE_FOR_THIS and the reasons;
//   - the random panel (LFDQueueFrameRandom_UpdateFrame): LFG_TYPE_RANDOM_DUNGEON (or the holiday dungeon's name and
//     description), LFD_REWARDS with LFD_RANDOM_REWARD_EXPLANATION1 / 2, the reward items, MONEY_COLON + the money,
//     EXPERIENCE_COLON + the XP, scrolled by its arrows when longer than the panel;
//   - the specific list (LFGQueueFrame_UpdateLFGDungeonList: LFDList_DefaultFilterFunction, headers without children
//     out, the headers' check states 0 / 1 (UI-MultiCheck) / 2, collapsed headers): the LFGDungeonGroup.dbc headers
//     with + / -, the dungeons with their check box or the lock (YOU_MAY_NOT_QUEUE_FOR_DUNGEON + the reasons per
//     player, INSTANCE_UNAVAILABLE_*), the name and "(15 - 25)" in GetQuestDifficultyColor of the target level;
//   - FIND_A_GROUP / JOIN_AS_PARTY / LEAVE_QUEUE (LFDQueueFrameFindGroupButton_Update), CLOSE;
//   - LFDQueueFrameCooldownFrame: Dungeon Deserter (71041) / Dungeon Cooldown (71328) with the time left.
// LFDDungeonReadyPopup at the StaticPopups' 0.8: the dialog (the dungeon's picture, the label, the instance info /
// THIS_DUNGEON_IN_PROGRESS, the role, the rewards, ENTER_DUNGEON / LEAVE_QUEUE, the hide button) and after
// the answer LFDDungeonReadyStatus (the members' role icons with the ready check icons; LFD_PROPOSAL_FAILED_CLOSE_TIME);
// LFDRoleCheckPopup; StaticPopup VOTE_BOOT_PLAYER and LFG_OFFER_CONTINUE (PartyLFGStartBackfill: the group joins its
// dungeon again); MiniMapLFGFrame (the eye at its Minimap.xml place, animated while queued; Cross =
// MiniMapLFGFrame_OnClick, Circle = the right click: MiniMapLFGFrameDropDown - teleport in / out, enter / leave);
// LFDSearchStatus while the cursor is on the eye (the roles found, LFG_STATISTIC_AVERAGE_WAIT, TIME_IN_QUEUE);
// DungeonCompletionAlertFrame (the toast: 0.2 s in, 4.05 s, 1.5 s out, the glow and the shine).
// PSP differences (marked PSP: below): 10 list rows of 24 units (the original 15 of 16: the PSP font is 10 px); the
// party backfill frame and the raid browser (LFRParentFrame, NoLFDWhileLFR, "listed") are not ported; no item links;
// the popups do not raise the cursor (SELECT) - the focus waits on their first button; START passes the ready / role
// check / vote popups by (they have no hideOnEscape); a role check or a vote closed by Circle on its title while still
// pending comes back (the original cannot dismiss them); the ready dialog's label box is 200 units (150: the PSP font
// is wider) and its boss list tooltip (GetLFGProposalEncounter) is not shown - the counts are (DungeonEncounter.dbc
// rows of the map / difficulty); a desaturated texture is drawn tinted grey.
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "platform/psp/psp_wotlk_db.hpp"
#include "ui/psp/ui_world_map.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids: 0x7900-0x79EF (0x79F0 / 0x79F1 are the game menu's, header).
enum : uint32_t {
    kIdLfdClose = 0x7900,
    kIdLfdRole0 = 0x7901,  // + 0 tank, 1 healer, 2 damage, 3 leader
    kIdLfdType = 0x7905,
    kIdLfdFind = 0x7906,
    kIdLfdCancel = 0x7907,
    kIdLfdUp = 0x7908,
    kIdLfdDown = 0x7909,
    kIdLfdRUp = 0x790A,
    kIdLfdRDown = 0x790B,
    kIdLfdReward0 = 0x7910,  // + 0..7 the random panel's reward items
    kIdLfdRow0 = 0x7920,     // + 0..9 the list rows (the check box)
    kIdLfdExp0 = 0x7930,     // + 0..9 the headers' + / -
    kIdLfdDd0 = 0x7940,      // + 0..31 the type dropdown's items
    kIdReadyEnter = 0x7960,
    kIdReadyLeave = 0x7961,
    kIdReadyHide = 0x7962,
    kIdReadyReward0 = 0x7963,  // + 0..3
    kIdRoleCheck0 = 0x7970,    // + 0 tank, 1 healer, 2 damage
    kIdRoleAccept = 0x7973,
    kIdRoleDecline = 0x7974,
    kIdRoleDesc = 0x7975,
    kIdBootYes = 0x7978,
    kIdBootNo = 0x7979,
    kIdOfferYes = 0x797A,
    kIdOfferNo = 0x797B,
    kIdEyeMenu0 = 0x7980,  // + 0..7 MiniMapLFGFrameDropDown's items
    kIdLfgEye = 0x79E0,
    kIdAlertReward0 = 0x79E1,  // + 0..7 the completion toast's rewards
};
bool lfdId(uint32_t id) { return id >= 0x7900 && id <= 0x79EF; }
bool lfdFrameId(uint32_t id) { return id >= 0x7900 && id < 0x7960; }  // the LFDParentFrame's own widgets
constexpr int kDdMax = 32;

// GetLFGMode (UIParent.lua) and its submodes.
enum : uint8_t { kModeNone = 0, kModeProposal, kModeQueued, kModeRoleCheck, kModeListed, kModeLfgParty, kModeAbandoned };
enum : uint8_t { kSubNone = 0, kSubUnaccepted, kSubAccepted, kSubEmpowered, kSubUnempowered };

// LFGDungeons.dbc TypeID; LFDQueueFrame.type "specific".
constexpr uint8_t kTypeDungeon = 1, kTypeHeroic = 5, kTypeRandom = 6;
constexpr uint32_t kSpecific = 0xFFFFFFFFu;
constexpr uint8_t kHolidayGroup = 11;         // LFGDungeonGroup: the holiday events (upstream isHoliday)
constexpr int kListRows = 10;                 // PSP (see the file comment)
constexpr float kRowU = 24.0f, kListX = 25.0f, kListY = 165.0f;
constexpr int kMaxShownLevelDiff = 15;        // LFD_MAX_SHOWN_LEVEL_DIFF
constexpr uint32_t kDeserterSpell = 71041, kCooldownSpell = 71328;  // Dungeon Deserter / Dungeon Cooldown
constexpr int kExpansionLevel = 2;            // GetExpansionLevel(): the second expansion
constexpr uint32_t kShowLfdLevel = 15;        // SHOW_LFD_LEVEL
constexpr float kProposalFailedCloseTime = 5.0f;  // LFD_PROPOSAL_FAILED_CLOSE_TIME
constexpr int kNumMembers = 5;                // NUM_LFD_MEMBERS
constexpr float kRandomViewU = 253.0f;        // LFDQueueFrameRandomScrollFrame height

// LFDParentFrame: the trainer's 0.6 x 0.55 at TOPLEFT 4,2 (as ui_world_tradeskill.cpp).
constexpr float kSX = 0.6f, kSY = 0.55f, kOx = 4.0f, kOy = 2.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
Rect FR(float x, float y, float w, float h) { return Rect{PX(x), PY(y), w * kSX, h * kSY}; }
// The popups: the StaticPopups' 0.8 (WorldScreen::kDialogScale); the search status and the toast: 0.6.
constexpr float kPS = 0.8f, kAS = 0.6f;
float U(float units) { return std::floor(units * kPS + 0.5f); }
constexpr float kPopupTop = 48.0f;           // StaticPopup1 TOP 0,-135 (ui_world_popup.cpp)
constexpr float kUiParentScale = 272.0f / 768.0f;  // UIParent units -> px (the alerts' BOTTOM 0,128)

// 0xAABBGGRR
constexpr uint32_t kColGoldC = 0xFF00D1FFu;   // NORMAL_FONT_COLOR (1, 0.82, 0)
constexpr uint32_t kColWhiteC = 0xFFFFFFFFu;  // HIGHLIGHT_FONT_COLOR
constexpr uint32_t kColHeader = 0xFFB3B3B3u;  // QuestDifficulty_Header (0.7, 0.7, 0.7)
constexpr uint32_t kColDisabled = 0xFF808080u;  // GameFontDisable
constexpr uint32_t kColRedC = 0xFF1A1AFFu;    // RED_FONT_COLOR (1, 0.1, 0.1)
constexpr uint32_t kColGreenC = 0xFF1AFF1Au;  // GREEN_FONT_COLOR (0.1, 1, 0.1)
constexpr uint32_t kColBlack = 0xFF000000u;   // QuestFont
constexpr uint32_t kColRedPure = 0xFF0000FFu;  // (1, 0, 0): YOUR_CLASS_MAY_NOT_PERFORM_ROLE
constexpr uint32_t kGreyTint = 0xFF8C8C8Cu;   // PSP: SetDesaturation

uint32_t withAlpha(uint32_t col, float a) {
    const uint32_t al = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * static_cast<float>(col >> 24) + 0.5f);
    return (al << 24) | (col & 0x00FFFFFFu);
}
uint32_t rgbF(float r, float g, float b) {
    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return 0xFF000000u | (c(b) << 16) | (c(g) << 8) | c(r);
}

// GetQuestDifficultyColor (QuestDifficultyColors) with the grey level (as ui_world_questlog.cpp).
uint32_t difficultyColor(int level, int playerLevel) {
    const int diff = level - playerLevel;
    const int pl = playerLevel;
    const int gray = pl <= 5 ? 0 : pl <= 39 ? pl - 5 - pl / 10 : pl <= 59 ? pl - 1 - pl / 5 : pl - 9;
    if (diff >= 5) return rgbF(1.0f, 0.1f, 0.1f);        // impossible
    if (diff >= 3) return rgbF(1.0f, 0.5f, 0.25f);       // very difficult
    if (diff >= -2) return rgbF(1.0f, 1.0f, 0.0f);       // difficult
    if (level > gray) return rgbF(0.25f, 0.75f, 0.25f);  // standard
    return rgbF(0.5f, 0.5f, 0.5f);                       // trivial
}

// GlobalStrings (strings.pak; |3-N(x) declension through questText; |c colour codes as colours).
// ROLE_DESCRIPTION2 / 3 / 1, GUIDE_TOOLTIP: tank, healer, damage, leader.
constexpr pp::CS kRoleDesc[4] = {pp::CS::ROLE_DESCRIPTION2, pp::CS::ROLE_DESCRIPTION3, pp::CS::ROLE_DESCRIPTION1,
                                 pp::CS::GUIDE_TOOLTIP};
constexpr pp::CS kRoleName[3] = {pp::CS::TANK, pp::CS::HEALER, pp::CS::DAMAGER};

// "...%s..." -> the text before and after the %s (a label drawn in two colours around a name).
void splitAround(const char* f, std::string& before, std::string& after) {
    const char* at = std::strstr(f, "%s");
    before.assign(f, at ? static_cast<size_t>(at - f) : std::strlen(f));
    after = at ? at + 2 : "";
}

// INSTANCE_UNAVAILABLE_SELF_* / _OTHER_* by LFG_INSTANCE_INVALID_CODES (|3-N kept for questText).
const char* lockReason(uint32_t code, bool self) {
    switch (code) {
        case 1:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_EXPANSION_TOO_LOW)
                        : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_EXPANSION_TOO_LOW);
        case 2:
        case 1001:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_LEVEL_TOO_LOW)
                        : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_LEVEL_TOO_LOW);
        case 3:
        case 1002:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_LEVEL_TOO_HIGH)
                        : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_LEVEL_TOO_HIGH);
        case 4:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_GEAR_TOO_LOW)
                        : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_GEAR_TOO_LOW);
        case 5:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_GEAR_TOO_HIGH)
                        : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_GEAR_TOO_HIGH);
        case 6:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_RAID_LOCKED)
                        : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_RAID_LOCKED);
        case 1022:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_QUEST_NOT_COMPLETED)
                        : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_QUEST_NOT_COMPLETED);
        case 1025:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_MISSING_ITEM)
                        : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_MISSING_ITEM);
        default:
            return self ? CLIENT_STR(INSTANCE_UNAVAILABLE_SELF_OTHER) : CLIENT_STR(INSTANCE_UNAVAILABLE_OTHER_OTHER);
    }
}

std::string replaceFirst(std::string s, const char* what, const std::string& with) {
    const size_t p = s.find(what);
    if (p != std::string::npos) s.replace(p, std::strlen(what), with);
    return s;
}

// SecondsToTime(seconds, noSeconds, notAbbreviated, maxCount) - the abbreviated units, TIME_UNIT_DELIMITER " ".
std::string secondsToTime(int seconds, int maxCount = 2) {
    std::string out;
    int count = 0;
    auto add = [&](const char* f, int v) {
        if (!out.empty()) out += ' ';
        out += pp::text::plural(pp::text::textf(f, v));  // enUS "%d |4Min:Min;"
        ++count;
    };
    if (seconds < 0) seconds = 0;
    if (seconds >= 86400) {
        add(CLIENT_STR(DAYS_ABBR), seconds / 86400);
        seconds %= 86400;
    }
    if (seconds >= 3600) {
        add(CLIENT_STR(HOURS_ABBR), seconds / 3600);
        seconds %= 3600;
    }
    if (count < maxCount && seconds >= 60) {
        add(CLIENT_STR(MINUTES_ABBR), seconds / 60);
        seconds %= 60;
    }
    if (count < maxCount && seconds > 0) add(CLIENT_STR(SECONDS_ABBR), seconds);
    return out;
}

// UI-LFG-ICON-ROLES cells (GetTexCoordsForRole: 67-unit cells of 256): 0 tank, 1 healer, 2 damage, 3 guide, 4 the
// cover (0, 0.2617, 0.5234, 0.7851); `s` the texture's content size.
SrcRect roleCell(int role, float s) {
    static constexpr int kCol[5] = {0, 1, 1, 0, 0}, kRow[5] = {1, 0, 1, 0, 2};
    const float c = 67.0f / 256.0f * s;
    if (role == 4) return SrcRect{0, 0.5234f * s, 0.2617f * s, 0.2617f * s};
    return SrcRect{static_cast<float>(kCol[role]) * c, static_cast<float>(kRow[role]) * c, c, c};
}
// GetBackgroundTexCoordsForRole (75-unit cells of 256 x 128): tank column 2, healer 1, damage 3.
SrcRect roleBgCell(int role, float w, float h) {
    static constexpr int kCol[3] = {1, 0, 2};
    const float cw = 75.0f / 256.0f * w, ch = 75.0f / 128.0f * h;
    return SrcRect{static_cast<float>(kCol[role]) * cw, 0, cw, ch};
}
// GetLFGProposalMember's role: the mask SetLFGRoles sends (2 tank, 4 healer, 8 damage).
int roleOfMask(uint32_t mask) { return (mask & 0x02) ? 0 : (mask & 0x04) ? 1 : 2; }
// EyeTemplate / AnimateTexCoords: frame k (1..29) of LFG-Eye (8 x 4 cells); `w`, `h` the content size.
SrcRect eyeCell(int frame, float w, float h) {
    const int k = std::max(1, frame) - 1;
    return SrcRect{static_cast<float>(k % 8) * w / 8.0f, static_cast<float>(k / 8) * h / 4.0f, w / 8.0f, h / 4.0f};
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int popcount(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

// The catalogue, marked as used (the update frees it when nothing shows a dungeon).
pp::LfgDb& useDb(pp::WorldSession& s, bool& used) {
    used = true;
    return s.lfgDb();
}

// IsInLFGDungeon (PSP: the current map is the map of a dungeon of the finder).
bool inLfgDungeon(const pp::LfgDb& db, uint32_t mapId) {
    for (uint32_t i = 0; i < db.dungeonCount(); ++i) {
        const pp::LfgDungeonRec& d = db.dungeonAt(i);
        if ((d.typeId == kTypeDungeon || d.typeId == kTypeHeroic) && d.mapId == mapId) return true;
    }
    return false;
}

// The aura's time left (ms), -1 when the unit has none (GetLFGDeserterExpiration / UnitHasLFGDeserter ...).
int32_t auraLeftMs(const std::vector<game::AuraSlot>* auras, uint32_t spellId) {
    if (!auras) return -1;
    const uint64_t now = pp::WorldSession::auraNowMs();
    for (const auto& a : *auras)
        if (!a.isEmpty() && a.spellId == spellId) {
            const int32_t rem = a.getRemainingMs(now);
            return rem < 0 ? 0 : rem;
        }
    return -1;
}

}  // namespace

// --- State -----------------------------------------------------------------------------------------------------------

struct WorldScreen::LfdUi {
    // Art (data/wotlk.pak): the frame of the current kind and the small pieces while the window is open; the popups'
    // while one is up; the minimap eye's while it shows; the toast's while it shows; the dropdown check mark.
    pp::Texture frame;
    std::string frameKind;
    pp::Texture sbar, rolesS, roleBg, eye32, cbUp, cbHl, cbCheck, cbCheckDis, cbMulti, cbMultiDis, lock, heroic, plus,
        minus, plusHl, ddBtn, ddBtnDn, nameFr;
    bool artTried = false;
    uint32_t artBytes = 0;
    pp::Texture rolesL, proles, fili, bottom, sep, ring, coin, hide, hideDn, hideHl, rcWait, rcReady, rcNotReady, readyBg;
    std::string readyBgName;
    bool popupArtTried = false;
    uint32_t popupBytes = 0;
    pp::Texture eye16, mmRing, mmHl, mmRoles;
    bool mmArtTried = false, eyeDrawn = false;
    pp::Texture toast, glow, shine, dungeonIcon, toastHeroic;
    std::string toastIconName;
    bool toastArtTried = false;
    pp::Texture ddCheck;
    bool ddCheckTried = false;
    bool dbUsed = false;

    // LFDQueueFrame (LFGEnabledList / LFGCollapseList / LFGLockList / LFDDungeonList, kept while the client runs).
    bool shown = false;
    uint32_t type = 0;                   // 0 unset, kSpecific, or a random dungeon id
    std::vector<int32_t> order;          // GetLFDChoiceOrder: -groupId headers, dungeon ids
    std::vector<int32_t> list, hidden;   // LFDDungeonList, LFDHiddenByCollapseList
    std::unordered_map<int32_t, int> enabled;   // dungeon 0 / 1; header -1 (false), 0, 1 (some), 2 (all)
    std::unordered_map<int32_t, int> queued;    // LFGQueuedForList (+ its headers)
    std::unordered_map<int32_t, bool> collapsed, locked;
    uint64_t listKey = ~0ull;
    int listScroll = 0;
    float randomScroll = 0.0f, randomMax = 0.0f;
    bool ddOpen = false, focusPending = false;
    std::vector<uint32_t> ddValues;      // the type dropdown's items (kSpecific / dungeon id)
    std::vector<bool> ddDisabled;

    // Popups.
    float readyCloseIn = -1.0f;          // the failure countdown (s), < 0 none
    std::array<bool, kNumMembers> readyAccepted{};  // LFDDungeonReadyStatus readyStatus == "accepted"
    bool readyGuard = false, roleGuard = false, bootGuard = false, offerGuard = false;
    uint8_t roleCheckRoles = 0;          // the role check popup's check boxes (2 / 4 / 8)
    bool roleShown = false;              // LFG_ROLE_CHECK_SHOW seen for the running check, not answered yet
    uint32_t offerDungeon = 0;

    // MiniMapLFGFrame / the portrait: EyeTemplate_OnUpdate (AnimateTexCoords: frame 1..29 every 0.1 s).
    int eyeFrame = 1, portraitFrame = 1;
    float eyeThrottle = 0.0f, portraitThrottle = 0.0f;
    bool eyeHover = false;
    Rect eyeRect{};
    std::vector<std::pair<const char*, uint8_t>> menu;  // the eye's dropdown: text, action (1 teleport out, 2 in,
    std::vector<bool> menuDisabled;                    // 3 accept, 4 reject, 5 leave)
    uint32_t menuAwayMs = 0;
    bool menuAbove = false;

    // DungeonCompletionAlertFrame.
    float alertT = -1.0f, alertOutAt = 4.05f;
    bool alertFocus = false;
    game::LfgCompletionReward alertReward;

    // The deferred GameTooltip (wotlkTip, drawn after all the windows).
    std::vector<WotlkTipLine> tip;
    Rect tipAnchor{};
    uint8_t tipMode = 0;

    uint32_t logs = 0;

    static void load(pp::Pak& pak, const char* name, pp::Texture& t, uint32_t& bytes) {
        pp::wotlkPakTexture(pak, name, t, &bytes);
    }
    void releaseArt() {
        for (pp::Texture* t : {&frame, &sbar, &rolesS, &roleBg, &eye32, &cbUp, &cbHl, &cbCheck, &cbCheckDis, &cbMulti,
                               &cbMultiDis, &lock, &heroic, &plus, &minus, &plusHl, &ddBtn, &ddBtnDn, &nameFr})
            if (t->pixels) pp::textureRelease(*t);
        frameKind.clear();
        if (artTried) PSP_LOGI(pp::tag::Gfx, "lfd_art_free kb=%u", static_cast<unsigned>((artBytes + 1023u) / 1024u));
        artTried = false;
        artBytes = 0;
    }
    void releasePopupArt() {
        for (pp::Texture* t : {&rolesL, &proles, &fili, &bottom, &sep, &ring, &coin, &hide, &hideDn, &hideHl, &rcWait,
                               &rcReady, &rcNotReady, &readyBg})
            if (t->pixels) pp::textureRelease(*t);
        readyBgName.clear();
        if (popupArtTried)
            PSP_LOGI(pp::tag::Gfx, "lfd_popup_art_free kb=%u", static_cast<unsigned>((popupBytes + 1023u) / 1024u));
        popupArtTried = false;
        popupBytes = 0;
    }
    void releaseMmArt() {
        for (pp::Texture* t : {&eye16, &mmRing, &mmHl, &mmRoles})
            if (t->pixels) pp::textureRelease(*t);
        if (mmArtTried) PSP_LOGI(pp::tag::Gfx, "lfg_minimap_art_free");
        mmArtTried = false;
    }
    void releaseToastArt() {
        for (pp::Texture* t : {&toast, &glow, &shine, &dungeonIcon, &toastHeroic})
            if (t->pixels) pp::textureRelease(*t);
        if (!popupArtTried)  // the coin and the ring the toast borrowed from the popups' art
            for (pp::Texture* t : {&coin, &ring})
                if (t->pixels) pp::textureRelease(*t);
        toastIconName.clear();
        toastArtTried = false;
    }
    void loadArt() {
        if (artTried) return;
        artTried = true;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        const std::pair<const char*, pp::Texture*> list[] = {
            {"lfd_sbar", &sbar},         {"lfg_roles_s", &rolesS},       {"lfg_rolebg", &roleBg},
            {"lfg_eye32", &eye32},       {"lfg_cb_up", &cbUp},           {"lfg_cb_hl", &cbHl},
            {"lfg_cb_check", &cbCheck},  {"lfg_cb_check_dis", &cbCheckDis}, {"lfg_cb_multi", &cbMulti},
            {"lfg_cb_multi_dis", &cbMultiDis}, {"lfg_lock", &lock},      {"lfg_heroic", &heroic},
            {"lfg_plus", &plus},         {"lfg_minus", &minus},          {"lfg_plushl", &plusHl},
            {"lfg_dd_btn", &ddBtn},      {"lfg_dd_btn_dn", &ddBtnDn},    {"lfg_namefr", &nameFr}};
        for (const auto& [name, tex] : list) load(pak, name, *tex, artBytes);
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "lfd_art small=%d kb=%u", rolesS.pixels ? 1 : 0,
                 static_cast<unsigned>((artBytes + 1023u) / 1024u));
    }
    void loadFrame(const std::string& kind) {
        if (frameKind == kind) return;
        frameKind = kind;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        uint32_t bytes = 0;
        // LFDQueueFrameBackground: a texture that is not there falls back to QUESTPAPER (LFDQueueFrameRandom_UpdateFrame).
        if (!pp::wotlkPakTexture(pak, ("lfd_" + kind).c_str(), frame, &bytes) && kind != "random")
            pp::wotlkPakTexture(pak, "lfd_random", frame, &bytes);
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "lfd_frame kind=%s ok=%d kb=%u", kind.c_str(), frame.pixels ? 1 : 0,
                 static_cast<unsigned>((bytes + 1023u) / 1024u));
    }
    void loadPopupArt() {
        if (popupArtTried) return;
        popupArtTried = true;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        const std::pair<const char*, pp::Texture*> list[] = {
            {"lfg_roles_l", &rolesL}, {"lfg_proles", &proles}, {"lfr_fili", &fili}, {"lfr_bottom", &bottom}, {"lfr_sep", &sep},
            {"lfr_ring", &ring},      {"lfr_coin", &coin},   {"lfr_hide", &hide},     {"lfr_hide_dn", &hideDn},
            {"lfr_hide_hl", &hideHl}, {"lfr_wait", &rcWait}, {"lfr_ready", &rcReady}, {"lfr_notready", &rcNotReady}};
        for (const auto& [name, tex] : list) load(pak, name, *tex, popupBytes);
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "lfd_popup_art kb=%u", static_cast<unsigned>((popupBytes + 1023u) / 1024u));
    }
    void loadReadyBg(const std::string& texture) {
        if (readyBgName == texture) return;
        readyBgName = texture;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        // no texture of its own: the default dungeon's background (LFDDungeonReadyPopup_Update)
        if (!pp::wotlkPakTexture(pak, ("lfgbg_" + lower(texture)).c_str(), readyBg, &popupBytes))
            pp::wotlkPakTexture(pak, "lfgbg_deadmines", readyBg, &popupBytes);
        pak.close();
    }
    void loadMmArt() {
        if (mmArtTried) return;
        mmArtTried = true;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        uint32_t bytes = 0;
        load(pak, "lfg_eye16", eye16, bytes);
        load(pak, "mm_lfg_ring", mmRing, bytes);
        load(pak, "mm_lfg_hl", mmHl, bytes);
        load(pak, "lfg_roles_s", mmRoles, bytes);  // LFDSearchStatus
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "lfg_minimap_art eye=%d kb=%u", eye16.pixels ? 1 : 0,
                 static_cast<unsigned>((bytes + 1023u) / 1024u));
    }
    void loadToastArt(const std::string& texture) {
        if (toastArtTried && toastIconName == texture) return;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) {
            toastArtTried = true;
            toastIconName = texture;
            return;
        }
        uint32_t bytes = 0;
        if (!toastArtTried) {
            load(pak, "lfg_toast", toast, bytes);
            load(pak, "alert_glow", glow, bytes);
            load(pak, "alert_shine", shine, bytes);
            load(pak, "lfg_heroic", toastHeroic, bytes);
            if (!coin.pixels) load(pak, "lfr_coin", coin, bytes);
            if (!ring.pixels) load(pak, "lfr_ring", ring, bytes);
        }
        toastArtTried = true;
        toastIconName = texture;
        // dungeonTexture: Interface\LFGFrame\LFGIcon-<TextureFilename>
        if (!pp::wotlkPakTexture(pak, ("lfgic_" + lower(texture)).c_str(), dungeonIcon, &bytes))
            pp::wotlkPakTexture(pak, "lfgic_dungeon", dungeonIcon, &bytes);
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "lfg_toast_art kb=%u", static_cast<unsigned>((bytes + 1023u) / 1024u));
    }
    void loadDdCheck() {
        if (ddCheckTried) return;
        ddCheckTried = true;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        uint32_t bytes = 0;
        load(pak, "lfg_cb_check", ddCheck, bytes);
        pak.close();
    }
    ~LfdUi() {
        releaseArt();
        releasePopupArt();
        releaseMmArt();
        releaseToastArt();
        if (ddCheck.pixels) pp::textureRelease(ddCheck);
    }
};

void WorldScreen::destroyLfdUi(LfdUi* p) { delete p; }

WorldScreen::LfdUi& WorldScreen::lfdUi() {
    if (!lfd_) lfd_ = std::unique_ptr<LfdUi, void (*)(LfdUi*)>(new LfdUi(), &WorldScreen::destroyLfdUi);
    return *lfd_;
}

// --- Shared pieces ---------------------------------------------------------------------------------------------------

// UIPanelButtonTemplate(2): the UIPanelButton art in three slices (as drawPanelButton), GameFontNormal gold,
// GameFontHighlight white under the focus, GameFontDisable grey; the small font (the tiny one when the label is wider).
void WorldScreen::wotlkButton(UiRenderer& ui, uint32_t id, const Rect& r, const char* label, bool enabled,
                              bool focusable) {
    loadNpcArt(false);
    const bool focused = focusable && enabled && winFocus_.addStop(id, r);
    const bool pressed = focused && winFocus_.isPressed(id);
    const pp::Texture& t = !enabled && npcArt_.btnDis.pixels ? npcArt_.btnDis
                           : pressed && npcArt_.btnDown.pixels ? npcArt_.btnDown
                                                               : npcArt_.btnUp;
    if (!t.pixels) {
        kit_.button(ui, winFocus_, id, r, label, enabled && focusable);
        return;
    }
    const float cap = std::min(6.0f, r.w * 0.25f), tw = t.uw, th = t.uh, capT = 6.0f * tw / 64.0f;
    ui.image(t, Rect{r.x, r.y, cap, r.h}, SrcRect{0, 0, capT, th});
    ui.image(t, Rect{r.x + cap, r.y, r.w - 2 * cap, r.h}, SrcRect{capT, 0, tw - 2 * capT, th});
    ui.image(t, Rect{r.x + r.w - cap, r.y, cap, r.h}, SrcRect{tw - capT, 0, capT, th});
    if (focused && npcArt_.btnHl.pixels) ui.image(npcArt_.btnHl, r, 0xFFFFFFFFu, Blend::Add);
    const Font& fs = kit_.font(kFontSmall);
    const Font& f = static_cast<float>(fs.measure(label)) <= r.w - 4.0f ? fs : kit_.font(kFontTiny);
    ui.textShadow(f, r.x + r.w * 0.5f + (pressed ? 1.0f : 0.0f),
                  r.y + std::floor((r.h - static_cast<float>(f.lineHeight())) * 0.5f + 0.5f) + (pressed ? 1.0f : 0.0f),
                  label, !enabled ? kColDisabled : focused ? kColWhiteC : kColGoldC, Align::Center);
}

void WorldScreen::wotlkTip(const Rect& anchorOnScreen, std::vector<WotlkTipLine> lines, uint8_t anchor) {
    LfdUi& L = lfdUi();
    L.tip = std::move(lines);
    L.tipAnchor = anchorOnScreen;
    L.tipMode = anchor;
}

void WorldScreen::wotlkDropDownList(UiRenderer& ui, float x, float y, const std::vector<WotlkDdItem>& items,
                                    uint32_t idBase, bool focusable) {
    if (items.empty()) return;
    LfdUi& L = lfdUi();
    L.loadDdCheck();
    loadNpcArt(false);  // UI-QuestTitleHighlight (the focused row)
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const int n = static_cast<int>(items.size());
    const Font* lf = &fs;
    float ih = static_cast<float>(fs.lineHeight()) + 2.0f;
    const float sh = static_cast<float>(pp::kScreenH);
    if (y + ui.offsetY() + ih * static_cast<float>(n) + 12.0f > sh) {  // PSP: a long list in the tiny font
        lf = &ft;
        ih = std::max(static_cast<float>(ft.lineHeight()) + 1.0f,
                      (sh - 12.0f - y - ui.offsetY()) / static_cast<float>(n));
    }
    float tw = 0.0f;
    for (const WotlkDdItem& it : items) tw = std::max(tw, static_cast<float>(lf->measure(it.text.c_str())));
    Rect lr{x, y, tw + 34.0f, ih * static_cast<float>(n) + 10.0f};
    lr.x = std::max(2.0f - ui.offsetX(), std::min(lr.x, static_cast<float>(pp::kScreenW) - 2.0f - lr.w - ui.offsetX()));
    lr.y = std::max(2.0f - ui.offsetY(), std::min(lr.y, sh - 2.0f - lr.h - ui.offsetY()));
    ui.boundsHold(true);
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{lr.x + 3, lr.y + 3, lr.w - 6, lr.h - 6}, 0xF0FFFFFFu);
    else ui.fill(lr, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), lr);
    for (int k = 0; k < n; ++k) {
        const WotlkDdItem& it = items[static_cast<size_t>(k)];
        const Rect ir{lr.x + 4, lr.y + 5 + ih * static_cast<float>(k), lr.w - 8, ih};
        const bool stop = focusable && !it.isTitle && (!it.disabled || !it.tipTitle.empty());
        const bool f = stop && winFocus_.addStop(idBase + static_cast<uint32_t>(k), ir);
        if (f && !it.disabled && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, ir, 0xFFFFFFFFu, Blend::Add);
        if (it.checked && L.ddCheck.pixels) ui.image(L.ddCheck, Rect{ir.x + 2, ir.y + (ih - 12.0f) * 0.5f, 12, 12});
        const uint32_t col = it.isTitle ? kColGoldC : it.disabled ? kColDisabled : kColWhiteC;
        ui.textShadow(*lf, ir.x + 20, ir.y + (ih - static_cast<float>(lf->lineHeight())) * 0.5f, it.text.c_str(), col);
        if (f && !it.tipTitle.empty()) {  // tooltipTitle (white) + tooltipText (gold, wrapped)
            std::vector<WotlkTipLine> t;
            t.push_back(WotlkTipLine{it.tipTitle, kColWhiteC});
            if (!it.tipText.empty()) t.push_back(WotlkTipLine{it.tipText, kColGoldC});
            wotlkTip(Rect{ir.x + ui.offsetX(), ir.y + ui.offsetY(), ir.w, ir.h}, std::move(t), kTipRight);
        }
    }
    ui.boundsHold(false);
}

// The deferred GameTooltip: the first line in the header font, the rest in the small one, every line wrapped to
// 220 px (SetText / AddLine with wrap), the money line with its coins (SetTooltipMoney), AddDoubleLine's right text.
void WorldScreen::drawWotlkOverlay(UiRenderer& ui) {
    if (!lfd_) return;
    LfdUi& L = *lfd_;
    drawLfdSearchStatus(ui);
    L.eyeHover = false;
    if (L.tip.empty()) return;
    std::vector<WotlkTipLine> lines = std::move(L.tip);
    L.tip.clear();
    if (!uiMode_ || settings_.isOpen()) return;
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    constexpr float kMaxW = 220.0f;
    struct Row {
        std::string text, right;
        uint32_t color = 0, rightColor = 0, money = 0;
        const Font* f = nullptr;
    };
    std::vector<Row> rows;
    float w = 0.0f, h = 8.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const WotlkTipLine& l = lines[i];
        const Font& f = i == 0 ? fn : fs;
        if (l.money > 0) {
            Row r;
            r.text = l.text;
            r.color = l.color;
            r.money = l.money;
            r.f = &fs;
            w = std::max(w, static_cast<float>(fs.measure(l.text.c_str())) + 4.0f + 70.0f);
            h += static_cast<float>(fs.lineHeight());
            rows.push_back(std::move(r));
            continue;
        }
        if (!l.right.empty()) {  // AddDoubleLine
            w = std::max(w, static_cast<float>(f.measure(l.text.c_str())) + 12.0f +
                                static_cast<float>(f.measure(l.right.c_str())));
            h += static_cast<float>(f.lineHeight());
            Row r;
            r.text = l.text;
            r.right = l.right;
            r.color = l.color;
            r.rightColor = l.rightColor;
            r.f = &f;
            rows.push_back(std::move(r));
            continue;
        }
        for (std::string& part : wrapQuestText(f, l.text.empty() ? std::string(" ") : l.text, kMaxW)) {
            w = std::max(w, static_cast<float>(f.measure(part.c_str())));
            h += static_cast<float>(f.lineHeight());
            Row r;
            r.text = std::move(part);
            r.color = l.color;
            r.f = &f;
            rows.push_back(std::move(r));
        }
    }
    const Rect& a = L.tipAnchor;
    Rect r{0, 0, w + 14.0f, h};
    const float sw = static_cast<float>(pp::kScreenW), sh = static_cast<float>(pp::kScreenH);
    switch (L.tipMode) {
        case kTipLeft:
            r.x = a.x - r.w - 2.0f;
            r.y = a.y;
            break;
        case kTipTop:
            r.x = a.x + a.w * 0.5f - r.w * 0.5f;
            r.y = a.y - r.h - 2.0f;
            break;
        case kTipBottom:
            r.x = a.x + a.w * 0.5f - r.w * 0.5f;
            r.y = a.y + a.h + 2.0f;
            break;
        default:
            r.x = a.x + a.w + 2.0f;
            r.y = a.y;
            break;
    }
    if (L.tipMode == kTipRight && r.x + r.w > sw - 2.0f) r.x = a.x - r.w - 2.0f;
    if (L.tipMode == kTipLeft && r.x < 2.0f) r.x = a.x + a.w + 2.0f;
    if (L.tipMode == kTipTop && r.y < 2.0f) r.y = a.y + a.h + 2.0f;
    r.x = std::clamp(r.x, 2.0f, std::max(2.0f, sw - r.w - 2.0f));
    r.y = std::clamp(r.y, 2.0f, std::max(2.0f, sh - r.h - 2.0f));
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    float y = r.y + 4.0f;
    for (const Row& row : rows) {
        ui.text(*row.f, r.x + 7, y, row.text.c_str(), row.color);
        if (!row.right.empty()) ui.text(*row.f, r.x + r.w - 7, y, row.right.c_str(), row.rightColor, Align::Right);
        if (row.money > 0) {
            const float tw = row.text.empty() ? 0.0f : static_cast<float>(row.f->measure(row.text.c_str())) + 4.0f;
            drawMoneyFrame(ui, r.x + 7 + tw, y, row.money, false);
        }
        y += static_cast<float>(row.f->lineHeight());
    }
}

// --- GetLFGMode and its helpers ---------------------------------------------------------------------------------------

// IsPartyLFG: the group's dungeon finder flag (SMSG_GROUP_LIST groupType GROUPTYPE_LFG 0x08).
bool WorldScreen::lfgPartyLfg() const {
    return session_.isInGroup() && (session_.getPartyData().groupType & 0x08) != 0;
}

// LFD_IsEmpowered (UIParent.lua): alone, or the leader - or HasLFGRestrictions (GROUPTYPE_LFG_RESTRICTED 0x04).
bool WorldScreen::lfdEmpowered() const {
    if (!session_.isInGroup() || session_.isGroupLeader()) return true;
    return (session_.getPartyData().groupType & 0x04) != 0;
}

uint8_t WorldScreen::lfgMode(uint8_t* sub) {
    using game::LfgState;
    uint8_t s = kSubNone;
    uint8_t mode = kModeNone;
    const LfgState st = session_.lfgState();
    if (st == LfgState::Proposal && session_.lfgProposalId() != 0) {  // proposalExists
        mode = kModeProposal;
        s = session_.lfgProposalAnswered() ? kSubAccepted : kSubUnaccepted;
    } else if (st == LfgState::Queued) {
        mode = kModeQueued;
        s = lfdEmpowered() ? kSubEmpowered : kSubUnempowered;
    } else if (st == LfgState::RoleCheck) {
        mode = kModeRoleCheck;
    } else if (lfgPartyLfg()) {  // PSP: "listed" (the raid browser) is not ported
        mode = kModeLfgParty;
    }
    if (sub) *sub = s;
    return mode;
}

// --- The list (LFDQueueFrame_Update) -----------------------------------------------------------------------------------

void WorldScreen::lfdRebuildList() {
    LfdUi& L = lfdUi();
    pp::LfgDb& db = useDb(session_, L.dbUsed);
    L.list.clear();
    L.hidden.clear();
    if (!db.loaded()) return;
    if (L.order.empty()) {
        // GetLFDChoiceOrder: the dungeon groups (LFGDungeonGroup typeid 1 / 5) by Order_index, each header (-id) with
        // its dungeons (type 1 / 5) of the player's faction - PSP: by level, then name (Order_index is 0 for all).
        int8_t faction = -1;
        if (const game::Character* me = session_.activeCharacter()) {
            switch (me->race) {
                case game::Race::HUMAN:
                case game::Race::DWARF:
                case game::Race::NIGHT_ELF:
                case game::Race::GNOME:
                case game::Race::DRAENEI: faction = 1; break;
                default: faction = 0; break;
            }
        }
        for (uint32_t gi = 0; gi < db.groupCount(); ++gi) {
            const pp::LfgGroupRec& g = db.groupAt(gi);
            if (g.typeId != kTypeDungeon && g.typeId != kTypeHeroic) continue;
            std::vector<const pp::LfgDungeonRec*> ds;
            for (uint32_t i = 0; i < db.dungeonCount(); ++i) {
                const pp::LfgDungeonRec& d = db.dungeonAt(i);
                if (d.groupId != g.id || (d.typeId != kTypeDungeon && d.typeId != kTypeHeroic)) continue;
                if (d.faction >= 0 && faction >= 0 && d.faction != faction) continue;
                ds.push_back(&d);
            }
            if (ds.empty()) continue;
            std::sort(ds.begin(), ds.end(), [&](const pp::LfgDungeonRec* a, const pp::LfgDungeonRec* b) {
                if (a->minLevel != b->minLevel) return a->minLevel < b->minLevel;
                if (a->maxLevel != b->maxLevel) return a->maxLevel < b->maxLevel;
                return std::strcmp(db.str(a->name), db.str(b->name)) < 0;
            });
            L.order.push_back(-static_cast<int32_t>(g.id));
            for (const pp::LfgDungeonRec* d : ds) L.order.push_back(static_cast<int32_t>(d->id));
        }
    }
    uint8_t sub = 0;
    const uint8_t mode = lfgMode(&sub);
    const bool useQueued = !(lfdEmpowered() && mode != kModeQueued);
    L.queued.clear();
    for (uint32_t id : session_.lfgQueuedList()) L.queued[static_cast<int32_t>(id)] = 1;
    std::unordered_map<int32_t, int>& enable = useQueued ? L.queued : L.enabled;
    // GetLFDChoiceLockedState: the player's locks and the party's (LFG_LOCK_INFO_RECEIVED).
    L.locked.clear();
    for (const auto& [id, status] : session_.lfgLocks())
        if (status) L.locked[static_cast<int32_t>(id)] = true;
    for (const auto& pl : session_.lfgPartyLocks())
        for (const auto& [id, status] : pl.locks)
            if (status) L.locked[static_cast<int32_t>(id)] = true;
    auto lockedId = [&](int32_t id) {
        auto it = L.locked.find(id);
        return it != L.locked.end() && it->second;
    };
    // 1. LFDList_DefaultFilterFunction.
    const int level = static_cast<int>(session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)));
    for (int32_t id : L.order) {
        if (id < 0) {
            L.list.push_back(id);
            continue;
        }
        const pp::LfgDungeonRec* d = db.dungeon(static_cast<uint32_t>(id));
        if (!d) continue;
        const bool hasHeader = d->groupId != 0;
        const bool expansion = kExpansionLevel >= d->expansion;
        const bool levelOk = level >= d->minLevel && level <= d->maxLevel;
        if (hasHeader && expansion && levelOk && (level - kMaxShownLevelDiff <= d->recLevel || !lockedId(id)))
            L.list.push_back(id);
    }
    // 2. LFGListRemoveHeadersWithoutChildren.
    {
        size_t pos = 0;
        bool nextIsChild = L.list.empty() || L.list[0] >= 0;
        while (pos < L.list.size()) {
            const bool isHeader = !nextIsChild;
            nextIsChild = pos + 1 < L.list.size() && L.list[pos + 1] >= 0;
            if (isHeader && !nextIsChild) L.list.erase(L.list.begin() + static_cast<std::ptrdiff_t>(pos));
            else ++pos;
        }
    }
    // 3. LFGListUpdateHeaderEnabledAndLockedStates (no children are hidden by a collapse yet).
    auto update = [&](int32_t id) {
        if (id < 0) {
            enable[id] = -1;
            L.locked[id] = true;
            return;
        }
        if (lockedId(id)) return;
        const pp::LfgDungeonRec* d = db.dungeon(static_cast<uint32_t>(id));
        if (!d) return;
        const int32_t g = -static_cast<int32_t>(d->groupId);
        L.locked[g] = false;
        auto it = enable.find(id);
        const bool on = it != enable.end() && it->second > 0;
        auto gi = enable.find(g);
        const int gs = gi == enable.end() ? -1 : gi->second;
        if (on) enable[g] = (gs == -1 || gs == 2) ? 2 : 1;
        else enable[g] = (gs == -1 || gs == 0) ? 0 : 1;
    };
    for (int32_t id : L.list) update(id);
    // 4. LFGListRemoveCollapsedChildren.
    for (size_t pos = 0; pos < L.list.size();) {
        const int32_t id = L.list[pos];
        auto c = L.collapsed.find(id);
        if (id >= 0 && c != L.collapsed.end() && c->second) {
            L.hidden.push_back(id);
            L.list.erase(L.list.begin() + static_cast<std::ptrdiff_t>(pos));
        } else {
            ++pos;
        }
    }
    if (++L.logs <= 40)
        PSP_LOGI(pp::tag::Gfx, "lfd_list rows=%u hidden=%u locks=%u mode=%u queued=%d", static_cast<unsigned>(L.list.size()),
                 static_cast<unsigned>(L.hidden.size()), static_cast<unsigned>(L.locked.size()),
                 static_cast<unsigned>(mode), useQueued ? 1 : 0);
}

// LFDQueueFrame_SetType: "specific" or a random dungeon id (the panel and its background follow it when drawn).
void WorldScreen::lfdSetType(uint32_t type) {
    LfdUi& L = lfdUi();
    if (L.type != type) L.randomScroll = 0.0f;
    L.type = type;
    PSP_LOGI(pp::tag::Gfx, "lfd_type %u", static_cast<unsigned>(type));
}

// LFDConstructDeclinedMessage: INSTANCE_UNAVAILABLE_SELF_* for the player, _OTHER_* with the member's name.
std::string WorldScreen::lfdDeclinedMessage(uint32_t dungeonId) {
    std::string out;
    auto add = [&](const std::string& line) {
        if (!out.empty()) out += "\n";
        out += line;
    };
    if (auto it = session_.lfgLocks().find(dungeonId); it != session_.lfgLocks().end() && it->second)
        add(lockReason(it->second, true));
    for (const auto& pl : session_.lfgPartyLocks()) {
        auto it = pl.locks.find(dungeonId);
        if (it == pl.locks.end() || !it->second) continue;
        const std::string* n = session_.nameOf(pl.guid);
        add(questText(replaceFirst(lockReason(it->second, false), "%s", n ? *n : std::string("?"))));
    }
    return out;
}

// LFDQueueFrame_Join.
void WorldScreen::lfdJoin() {
    LfdUi& L = lfdUi();
    std::vector<uint32_t> dungeons;
    if (L.type == kSpecific) {
        for (const auto* v : {&L.list, &L.hidden})
            for (int32_t id : *v) {  // LFDQueueFrame_QueueForInstanceIfEnabled
                if (id < 0) continue;
                auto e = L.enabled.find(id);
                auto k = L.locked.find(id);
                if (e != L.enabled.end() && e->second > 0 && (k == L.locked.end() || !k->second))
                    dungeons.push_back(static_cast<uint32_t>(id));
            }
    } else if (L.type != 0) {
        dungeons.push_back(L.type);
    }
    // JoinLFG (the client's checks before CMSG_LFG_JOIN): ERR_LFG_NO_ROLES_SELECTED / ERR_LFG_NO_SLOTS_SELECTED.
    if ((session_.lfgRoles() & 0x0E) == 0) {
        uiError(CLIENT_STR(ERR_LFG_NO_ROLES_SELECTED));
        return;
    }
    if (dungeons.empty()) {
        uiError(CLIENT_STR(ERR_LFG_NO_SLOTS_SELECTED));
        return;
    }
    session_.lfgJoin(dungeons);
}

// --- Per frame ---------------------------------------------------------------------------------------------------------

void WorldScreen::lfdShowPopup(uint8_t w, uint32_t focusId) {
    const bool was = windowOpen(w);
    openWindow(w);
    // PSP: a dialog of ours stands where StaticPopups stand, never beside the open panels (openWindow's rule).
    placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), w), placeNext_.end());
    autoPos_.erase(w);
    if (!was) winFocus_.setFocus(focusId);
}

// ToggleLFDParentFrame: SHOW_LFD_LEVEL and up.
void WorldScreen::lfdToggle() {
    if (windowOpen(kWinLfd)) {
        closeWindow(kWinLfd);
        return;
    }
    if (session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)) < kShowLfdLevel) return;
    LfdUi& L = lfdUi();
    openWindow(kWinLfd);
    L.focusPending = true;
}

void WorldScreen::lfdUpdate(float dtSec) {
    using Ev = pp::WorldSession::LfgEvent;
    Ev ev;
    while (session_.takeLfgEvent(ev)) {
        LfdUi& L = lfdUi();
        switch (ev.kind) {
            case Ev::Update: L.listKey = ~0ull; break;  // LFG_UPDATE: LFG_UpdateQueuedList -> LFDQueueFrame_Update
            case Ev::QueueStatus: break;                // LFDSearchStatus_Update: read when drawn
            case Ev::ProposalShow:  // LFDFrame_OnEvent LFG_PROPOSAL_SHOW
                L.readyCloseIn = -1.0f;
                L.readyAccepted.fill(false);  // LFDDungeonReadyStatus_ResetReadyStates
                L.readyGuard = true;
                lfdShowPopup(kWinLfdReady, kIdReadyEnter);
                playSoundByName("ReadyCheck");
                session_.lfgRequestLockInfo();  // OnShow: RequestLFDPlayerLockInfo / RequestLFDPartyLockInfo (rewards)
                break;
            case Ev::ProposalUpdate:  // LFDDungeonReadyPopup_Update: no proposal - hidden (the dialog; the failure
                                      // countdown keeps the status view, LFG_PROPOSAL_FAILED follows)
                if (windowOpen(kWinLfdReady) && session_.lfgProposalId() == 0 && !session_.lfgProposalAnswered())
                    closeWindow(kWinLfdReady);
                break;
            case Ev::ProposalFailed:  // LFDDungeonReadyPopup_OnFail
                playSoundByName("LFG_Denied");
                if (windowOpen(kWinLfdReady)) {
                    if (!session_.lfgProposalAnswered()) closeWindow(kWinLfdReady);
                    else L.readyCloseIn = kProposalFailedCloseTime;
                }
                break;
            case Ev::ProposalSucceeded:
                closeWindow(kWinLfdReady);
                L.readyCloseIn = -1.0f;
                break;
            case Ev::RoleCheckShow:  // StaticPopupSpecial_Show(LFDRoleCheckPopup); OnShow: LFG_UpdateRoleCheckboxes
                L.roleCheckRoles = session_.lfgRoles() & 0x0E;
                L.roleShown = true;
                L.roleGuard = true;
                lfdShowPopup(kWinLfdRoleCheck, kIdRoleAccept);
                break;
            case Ev::RoleCheckHide:
                closeWindow(kWinLfdRoleCheck);
                L.roleShown = false;
                break;
            case Ev::BootUpdate: {  // VOTE_BOOT_PLAYER while in progress and not voted
                const auto& b = session_.lfgBoot();
                if (b.inProgress && !b.didVote) {
                    if (!windowOpen(kWinLfgBoot)) L.bootGuard = true;
                    lfdShowPopup(kWinLfgBoot, kIdBootYes);
                } else {
                    closeWindow(kWinLfgBoot);
                }
                break;
            }
            case Ev::RandomInfo: {  // LFG_UPDATE_RANDOM_INFO (+ LFG_LOCK_INFO_RECEIVED)
                L.listKey = ~0ull;
                auto joinable = [&](uint32_t id) {  // IsLFGDungeonJoinable
                    if (auto it = session_.lfgLocks().find(id); it != session_.lfgLocks().end() && it->second) return false;
                    for (const auto& pl : session_.lfgPartyLocks())
                        if (auto it = pl.locks.find(id); it != pl.locks.end() && it->second) return false;
                    return true;
                };
                bool known = false;
                for (const auto& r : session_.lfgRandomDungeons()) known = known || r.dungeonId == L.type;
                if (L.type == 0 || (L.type != kSpecific && (!known || !joinable(L.type)))) {
                    uint32_t best = 0;  // GetRandomDungeonBestChoice (upstream: the first offered and not locked)
                    for (const auto& r : session_.lfgRandomDungeons())
                        if (joinable(r.dungeonId)) {
                            best = r.dungeonId;
                            break;
                        }
                    lfdSetType(best ? best : kSpecific);
                }
                break;
            }
            case Ev::CompletionReward:  // DungeonCompletionAlertFrame_ShowAlert
                L.alertReward = session_.lfgCompletionReward();
                L.alertT = 0.0f;
                L.alertOutAt = 4.05f;
                playSoundByName("LFG_Rewards");
                break;
            case Ev::OfferContinue:  // LFG_OFFER_CONTINUE
                L.offerDungeon = ev.arg;
                L.offerGuard = true;
                lfdShowPopup(kWinLfgOffer, kIdOfferYes);
                break;
            case Ev::OpenFinder:  // SMSG_OPEN_LFG_DUNGEON_FINDER (upstream openLfgCallback)
                if (!windowOpen(kWinLfd)) lfdToggle();
                break;
        }
    }
    if (!lfd_) return;
    LfdUi& L = *lfd_;
    uint8_t sub = 0;
    const uint8_t mode = lfgMode(&sub);

    // The failure countdown of the status view (LFDDungeonReadyPopup_OnUpdate).
    if (L.readyCloseIn >= 0.0f) {
        L.readyCloseIn -= dtSec;
        if (L.readyCloseIn < 0.0f || !windowOpen(kWinLfdReady)) {
            closeWindow(kWinLfdReady);
            L.readyCloseIn = -1.0f;
        }
    }
    // PSP: a pending role check / vote closed by Circle on its title comes back (the original cannot dismiss them).
    if (L.roleShown && mode == kModeRoleCheck && !windowOpen(kWinLfdRoleCheck)) lfdShowPopup(kWinLfdRoleCheck, kIdRoleAccept);
    if (mode != kModeRoleCheck) {
        L.roleShown = false;
        if (windowOpen(kWinLfdRoleCheck)) closeWindow(kWinLfdRoleCheck);
    }
    {
        const auto& b = session_.lfgBoot();
        const bool pending = b.inProgress && !b.didVote;
        if (pending && !windowOpen(kWinLfgBoot)) lfdShowPopup(kWinLfgBoot, kIdBootYes);
        if (!pending && windowOpen(kWinLfgBoot)) closeWindow(kWinLfgBoot);
    }
    // MiniMapLFGFrame hidden: its dropdown goes with it.
    if (mode == kModeNone && windowOpen(kWinWotlkDropDown)) closeWindow(kWinWotlkDropDown);

    // EyeTemplate_OnUpdate: the minimap eye while queued / listed / in a role check, the portrait while queued / role check.
    auto animate = [&](int& frame, float& throttle, bool on) {
        if (!on) {
            frame = 1;  // EyeTemplate_StopAnimating: the animation starts over
            throttle = 0.0f;
            return;
        }
        throttle += dtSec;
        while (throttle > 0.1f) {
            throttle -= 0.1f;
            frame = frame >= 29 ? 1 : frame + 1;
        }
    };
    animate(L.eyeFrame, L.eyeThrottle, mode == kModeQueued || mode == kModeListed || mode == kModeRoleCheck);
    animate(L.portraitFrame, L.portraitThrottle, mode == kModeQueued || mode == kModeRoleCheck);

    // The toast's clock (AlertFrame_AnimateIn / waitAndAnimOut; the focus on it holds it - AlertFrame_StopOutAnimation).
    if (L.alertT >= 0.0f) {
        if (L.alertFocus) L.alertOutAt = std::max(L.alertOutAt, L.alertT + 1.0f);
        L.alertFocus = false;
        L.alertT += dtSec;
        if (L.alertT > L.alertOutAt + 1.5f) L.alertT = -1.0f;
    }

    // The frame: shown / hidden (OnShow: igCharacterInfoOpen + the lock info; OnHide: igCharacterInfoClose).
    const bool open = windowOpen(kWinLfd);
    if (open && !L.shown) {
        L.shown = true;
        playUiSound(UiSound::CharacterSheetOpen);
        session_.lfgRequestLockInfo();
        L.listKey = ~0ull;
        L.ddOpen = false;
        PSP_LOGI(pp::tag::Gfx, "lfd_open type=%u mode=%u", static_cast<unsigned>(L.type), static_cast<unsigned>(mode));
    } else if (!open && L.shown) {
        L.shown = false;
        L.ddOpen = false;
        playUiSound(UiSound::CharacterSheetClose);
    }
    if (open) {
        // GetAvailableRoles: a role the class cannot take is never offered (PSP: wowee.cfg lfgroles is shared).
        const uint8_t cls = session_.getPlayerClass();
        uint8_t mask = 0x01 | 0x08;
        if (cls == 1 || cls == 2 || cls == 6 || cls == 11) mask |= 0x02;
        if (cls == 2 || cls == 5 || cls == 7 || cls == 11) mask |= 0x04;
        if (cls != 0 && (session_.lfgRoles() & ~mask) != 0) session_.lfgSetRoles(session_.lfgRoles() & mask);
        // LFDQueueFrame_Update when what it reads changes (LFG_UPDATE, the locks, the party, the level).
        const auto& pd = session_.getPartyData();
        const uint64_t key =
            (static_cast<uint64_t>(session_.lfgVersion()) << 24) ^ (static_cast<uint64_t>(pd.members.size()) << 8) ^
            (static_cast<uint64_t>(pd.groupType) << 12) ^ (session_.isGroupLeader() ? 1ull : 0ull) ^
            (static_cast<uint64_t>(session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL))) << 40);
        if (key != L.listKey) {
            L.listKey = key;
            lfdRebuildList();
        }
        if (L.focusPending) {
            L.focusPending = false;
            winFocus_.setFocus(kIdLfdFind);
        }
    }
    // The art: the frame's while it is open, the popups' while one is up, the eye's while it is drawn (freed the frame
    // after - the GE has drawn it), the toast's while it plays, the check mark while a dropdown can show.
    if (!open && L.artTried) L.releaseArt();
    const bool popups = windowOpen(kWinLfdReady) || windowOpen(kWinLfdRoleCheck);
    if (!popups && L.popupArtTried && L.alertT < 0.0f) L.releasePopupArt();
    if (!L.eyeDrawn && L.mmArtTried) L.releaseMmArt();
    L.eyeDrawn = false;
    if (L.alertT < 0.0f && L.toastArtTried) L.releaseToastArt();
    if (!open && !windowOpen(kWinAchievements) && !windowOpen(kWinCalendar) && !windowOpen(kWinCalendarEvent) &&
        L.ddCheckTried) {
        if (L.ddCheck.pixels) pp::textureRelease(L.ddCheck);
        L.ddCheckTried = false;
    }
    // The dungeon catalogue while something shows a dungeon (the frame, the popups, the toast).
    const bool needDb = open || popups || windowOpen(kWinLfgOffer) || L.alertT >= 0.0f;
    if (!needDb && L.dbUsed) {
        L.dbUsed = false;
        session_.lfgReleaseDb();
    }
}

// --- Actions -----------------------------------------------------------------------------------------------------------

bool WorldScreen::lfdAction(uint32_t act) {
    if (act == kIdMenuLfd) {  // the game menu's DUNGEONS_BUTTON (the micro button's ToggleLFDParentFrame)
        closeWindow(kWinMenu);
        lfdToggle();
        return true;
    }
    if (!lfdId(act)) return false;
    if (!lfd_) return true;
    LfdUi& L = *lfd_;
    auto tab = [] { playSoundByName("igCharacterInfoTab"); };
    // The type dropdown's list: an item picks it (UIDropDownMenuButton_OnClick), any other click of the frame closes it.
    if (L.ddOpen) {
        if (act >= kIdLfdDd0 && act < kIdLfdDd0 + kDdMax) {
            const size_t k = act - kIdLfdDd0;
            if (k < L.ddValues.size() && !L.ddDisabled[k]) {
                L.ddOpen = false;
                lfdSetType(L.ddValues[k]);
                playSoundByName("UChatScrollButton");
            }
            return true;
        }
        L.ddOpen = false;
        if (lfdFrameId(act)) return true;
    }
    uint8_t sub = 0;
    const uint8_t mode = lfgMode(&sub);
    switch (act) {
        case kIdLfdClose:
        case kIdLfdCancel: closeWindow(kWinLfd); return true;  // HideUIPanel
        case kIdLfdType:  // the dropdown's button (ToggleDropDownMenu)
            L.ddOpen = true;
            playSoundByName("igMainMenuOptionCheckBoxOn");
            return true;
        case kIdLfdFind:  // LFDQueueFrameFindGroupButton OnClick
            if (mode == kModeQueued || mode == kModeListed || mode == kModeRoleCheck) session_.lfgLeave();
            else lfdJoin();
            return true;
        case kIdLfdUp: L.listScroll = std::max(0, L.listScroll - 1); return true;
        case kIdLfdDown: ++L.listScroll; return true;  // clamped when drawn
        case kIdLfdRUp: L.randomScroll = std::max(0.0f, L.randomScroll - 40.0f); return true;
        case kIdLfdRDown: L.randomScroll = std::min(L.randomMax, L.randomScroll + 40.0f); return true;
        // LFDDungeonReadyDialog: ENTER_DUNGEON (AcceptProposal), LEAVE_QUEUE (RejectProposal), the hide button.
        case kIdReadyEnter:
        case kIdReadyLeave:
            if (L.readyGuard) return true;
            tab();
            session_.lfgAnswerProposal(act == kIdReadyEnter);
            return true;
        case kIdReadyHide:
            tab();
            closeWindow(kWinLfdReady);
            return true;
        case kIdRoleAccept: {  // LFDRoleCheckPopupAccept_OnClick
            if (L.roleGuard) return true;
            tab();
            if ((L.roleCheckRoles & 0x0E) == 0) {  // CompleteLFGRoleCheck(true) refuses: ERR_LFG_NO_ROLES_SELECTED
                uiError(CLIENT_STR(ERR_LFG_NO_ROLES_SELECTED));
                return true;
            }
            session_.lfgSetRoles(static_cast<uint8_t>((session_.lfgRoles() & 0x01) | (L.roleCheckRoles & 0x0E)));
            session_.lfgCompleteRoleCheck(true);
            L.roleShown = false;
            closeWindow(kWinLfdRoleCheck);
            return true;
        }
        case kIdRoleDecline:  // LFDRoleCheckPopupDecline_OnClick
            if (L.roleGuard) return true;
            tab();
            L.roleShown = false;
            closeWindow(kWinLfdRoleCheck);
            session_.lfgCompleteRoleCheck(false);
            return true;
        case kIdBootYes:
        case kIdBootNo:  // VOTE_BOOT_PLAYER OnAccept / OnCancel: SetLFGBootVote
            if (L.bootGuard) return true;
            session_.lfgSetBootVote(act == kIdBootYes);
            closeWindow(kWinLfgBoot);
            return true;
        case kIdOfferYes:  // LFG_OFFER_CONTINUE OnAccept: PartyLFGStartBackfill (the group's dungeon again)
            if (L.offerGuard) return true;
            closeWindow(kWinLfgOffer);
            if (L.offerDungeon) session_.lfgJoin({L.offerDungeon});
            return true;
        case kIdOfferNo:
            if (L.offerGuard) return true;
            closeWindow(kWinLfgOffer);
            return true;
        case kIdLfgEye:  // MiniMapLFGFrame_OnClick (LeftButton)
            if (mode == kModeLfgParty || mode == kModeAbandoned) {
                wotlkAltAction(kIdLfgEye);
            } else if (mode == kModeProposal) {
                if (!windowOpen(kWinLfdReady)) {
                    tab();
                    lfdShowPopup(kWinLfdReady, kIdReadyEnter);
                }
            } else if (mode == kModeQueued || mode == kModeRoleCheck) {
                lfdToggle();
            }
            return true;
        default: break;
    }
    // LFGRoleButtonTemplate OnClick: the check box, when enabled (LFDFrameRoleCheckButton_OnClick -> SetLFGRoles).
    if (act >= kIdLfdRole0 && act < kIdLfdRole0 + 4) {
        static constexpr uint8_t kBit[4] = {0x02, 0x04, 0x08, 0x01};
        const int k = static_cast<int>(act - kIdLfdRole0);
        const uint8_t cls = session_.getPlayerClass();
        const bool avail = k == 0 ? (cls == 1 || cls == 2 || cls == 6 || cls == 11)
                           : k == 1 ? (cls == 2 || cls == 5 || cls == 7 || cls == 11)
                           : k == 2 ? true
                                    : (!session_.isInGroup() || session_.isGroupLeader());
        const bool locked = mode == kModeQueued || mode == kModeListed || mode == kModeRoleCheck || mode == kModeProposal;
        if (!avail || locked) return true;
        const uint8_t roles = session_.lfgRoles() ^ kBit[k];
        playSoundByName((roles & kBit[k]) ? "igMainMenuOptionCheckBoxOn" : "igMainMenuOptionCheckBoxOff");
        session_.lfgSetRoles(roles);
        return true;
    }
    // LFDRoleCheckPopup's role buttons: their check boxes only (SetLFGRoles on ACCEPT).
    if (act >= kIdRoleCheck0 && act < kIdRoleCheck0 + 3) {
        static constexpr uint8_t kBit[3] = {0x02, 0x04, 0x08};
        const int k = static_cast<int>(act - kIdRoleCheck0);
        const uint8_t cls = session_.getPlayerClass();
        const bool avail = k == 0 ? (cls == 1 || cls == 2 || cls == 6 || cls == 11)
                           : k == 1 ? (cls == 2 || cls == 5 || cls == 7 || cls == 11)
                                    : true;
        if (!avail || L.roleGuard) return true;
        L.roleCheckRoles ^= kBit[k];
        playSoundByName((L.roleCheckRoles & kBit[k]) ? "igMainMenuOptionCheckBoxOn" : "igMainMenuOptionCheckBoxOff");
        return true;
    }
    // The specific list: a row's check box (LFDQueueFrameDungeonChoiceEnableButton_OnClick), a header's + / -
    // (LFDQueueFrameExpandOrCollapseButton_OnClick -> LFDList_SetHeaderCollapsed).
    if ((act >= kIdLfdRow0 && act < kIdLfdRow0 + kListRows) || (act >= kIdLfdExp0 && act < kIdLfdExp0 + kListRows)) {
        const bool exp = act >= kIdLfdExp0;
        const int v = static_cast<int>(act - (exp ? kIdLfdExp0 : kIdLfdRow0));
        const int i = L.listScroll + v;
        if (L.type != kSpecific || i < 0 || i >= static_cast<int>(L.list.size())) return true;
        const int32_t id = L.list[static_cast<size_t>(i)];
        pp::LfgDb& db = useDb(session_, L.dbUsed);
        auto groupOf = [&](int32_t d) {
            const pp::LfgDungeonRec* r = d >= 0 ? db.dungeon(static_cast<uint32_t>(d)) : nullptr;
            return r ? -static_cast<int32_t>(r->groupId) : 0;
        };
        if (exp) {
            if (id >= 0) return true;
            const bool now = !(L.collapsed.count(id) && L.collapsed[id]);
            L.collapsed[id] = now;
            for (const auto* vv : {&L.list, &L.hidden})
                for (int32_t d : *vv)
                    if (d >= 0 && groupOf(d) == id) L.collapsed[d] = now;
            lfdRebuildList();
            return true;
        }
        const bool clickable = !(mode == kModeRoleCheck || mode == kModeQueued || mode == kModeListed) && lfdEmpowered();
        if (!clickable || (L.locked.count(id) && L.locked[id])) return true;
        auto it = L.enabled.find(id);
        const bool checked = it != L.enabled.end() && (id < 0 ? it->second > 0 : it->second != 0);
        const bool on = !checked;
        playSoundByName("igMainMenuOptionCheckBoxOff");  // (sic: Off for both)
        if (id < 0) {  // LFDList_SetHeaderEnabled
            for (const auto* vv : {&L.list, &L.hidden})
                for (int32_t d : *vv)
                    if (d >= 0 && groupOf(d) == id) L.enabled[d] = on ? 1 : 0;
            L.enabled[id] = on ? 2 : 0;
        } else {
            L.enabled[id] = on ? 1 : 0;  // LFDList_SetDungeonEnabled
        }
        lfdRebuildList();  // LFGListUpdateHeaderEnabledAndLockedStates + LFDQueueFrameSpecificList_Update
        return true;
    }
    // MiniMapLFGFrameDropDown's items.
    if (act >= kIdEyeMenu0 && act < kIdEyeMenu0 + 8) {
        const size_t k = act - kIdEyeMenu0;
        closeWindow(kWinWotlkDropDown);
        if (k >= L.menu.size() || L.menuDisabled[k]) return true;
        playSoundByName("UChatScrollButton");
        switch (L.menu[k].second) {
            case 1: session_.lfgTeleport(true); break;   // MiniMapLFGFrame_TeleportOut: LFGTeleport(true)
            case 2: session_.lfgTeleport(false); break;  // MiniMapLFGFrame_TeleportIn
            case 3: session_.lfgAnswerProposal(true); break;   // AcceptProposal
            case 4: session_.lfgAnswerProposal(false); break;  // RejectProposal
            case 5: session_.lfgLeave(); break;                // LeaveLFG
            default: break;
        }
        return true;
    }
    return true;  // the rewards, the info: tooltips only
}

// --- Drawing: LFDParentFrame ---------------------------------------------------------------------------------------------

bool WorldScreen::drawLfdFrame(UiRenderer& ui) {
    LfdUi& L = lfdUi();
    pp::LfgDb& db = useDb(session_, L.dbUsed);
    L.loadArt();
    const pp::LfgDungeonRec* typeRec = (L.type != 0 && L.type != kSpecific) ? db.dungeon(L.type) : nullptr;
    // LFDQueueFrameBackground: DUNGEONWALL (specific), HEROIC, the holiday's, else QUESTPAPER.
    std::string kind = "random";
    if (L.type == kSpecific) kind = "specific";
    else if (typeRec && typeRec->difficulty > 0) kind = "heroic";
    else if (typeRec && typeRec->groupId == kHolidayGroup) kind = "hol_" + lower(db.str(typeRec->texture));
    L.loadFrame(kind);
    if (!L.frame.pixels) {  // no wotlk.pak
        closeWindow(kWinLfd);
        uiError(OWN_STR(NO_WOTLK_ART));
        return true;
    }
    PROF_SCOPE("lfd");
    loadNpcArt(false);  // the panel buttons, the close button, the scroll arrows
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight());
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const bool live = focusable && !L.ddOpen;  // the open dropdown list takes the focus
    const uint32_t focused = winFocus_.focused();
    uint8_t sub = 0;
    const uint8_t mode = lfgMode(&sub);
    const bool empowered = lfdEmpowered();
    const bool inParty = session_.isInGroup();
    const int level = static_cast<int>(session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)));
    auto screenRect = [&](const Rect& r) { return Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h}; };

    ui.image(L.frame, FR(0, 0, 356, 440));
    // LFDParentFramePortrait (EyeTemplate 64 x 64 at TOPLEFT 9,-5).
    if (L.eye32.pixels) ui.image(L.eye32, FR(9, 5, 64, 64), eyeCell(L.portraitFrame, L.eye32.uw, L.eye32.uh));
    // LFDQueueFrameTitleText (TOP 12,-17, GameFontNormal). PSP: centred in the title bar (the font is taller).
    ui.textShadow(fn, PX(189.5f), PY(23.5f) - static_cast<float>(fn.lineHeight()) * 0.5f,
                  CLIENT_STR(LOOKING_FOR_DUNGEON), kColGoldC, Align::Center);
    // UIPanelCloseButton (TOPRIGHT 2,-8).
    {
        const Rect cr{PX(341) - 9, PY(24) - 9, 18, 18};
        const bool f = live && winFocus_.addStop(kIdLfdClose, cr);
        const pp::Texture& tx = f && winFocus_.isPressed(kIdLfdClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                  : npcArt_.close;
        if (tx.pixels) ui.image(tx, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // The role buttons (LFG_UpdateAvailableRoles / LFG_UpdateRolesChangeable / LFG_UpdateRoleCheckboxes).
    {
        const uint8_t cls = session_.getPlayerClass();
        const bool rolesLocked = mode == kModeQueued || mode == kModeListed || mode == kModeRoleCheck || mode == kModeProposal;
        const uint8_t roles = session_.lfgRoles();
        static constexpr float kRoleX[4] = {65, 138, 209, 289};
        static constexpr uint8_t kBit[4] = {0x02, 0x04, 0x08, 0x01};
        static constexpr float kBgAlpha[3] = {0.6f, 0.4f, 0.6f};
        for (int k = 0; k < 4; ++k) {
            const bool avail = k == 0 ? (cls == 1 || cls == 2 || cls == 6 || cls == 11)
                               : k == 1 ? (cls == 2 || cls == 5 || cls == 7 || cls == 11)
                               : k == 2 ? true
                                        : (!inParty || session_.isGroupLeader());
            const bool perm = !avail;  // LFG_PermanentlyDisableRoleButton
            const bool disabled = perm || rolesLocked;
            const Rect br = FR(kRoleX[k], 60, 48, 48);
            // motionScriptsWhileDisabled: the tooltip shows on a disabled button too.
            const bool f = live && winFocus_.addStop(kIdLfdRole0 + static_cast<uint32_t>(k), br);
            if (k < 3 && !disabled && L.roleBg.pixels)
                ui.image(L.roleBg, FR(kRoleX[k] + 24 - 40, 84 - 40, 80, 80), roleBgCell(k, L.roleBg.uw, L.roleBg.uh),
                         withAlpha(0xFFFFFFFFu, kBgAlpha[k]));
            if (L.rolesS.pixels) {
                ui.image(L.rolesS, br, roleCell(k == 3 ? 3 : k, L.rolesS.uw), perm ? kGreyTint : 0xFFFFFFFFu);
                if (disabled) ui.image(L.rolesS, br, roleCell(4, L.rolesS.uw), withAlpha(0xFFFFFFFFu, perm ? 0.7f : 0.5f));
            }
            if (!perm) {  // the check box (24 x 24 at BOTTOMLEFT -5,-5; hidden when permanently disabled)
                const Rect cb = FR(kRoleX[k] - 5, 89, 24, 24);
                if (L.cbUp.pixels) ui.image(L.cbUp, cb);
                if (roles & kBit[k]) {
                    const pp::Texture& c = disabled && L.cbCheckDis.pixels ? L.cbCheckDis : L.cbCheck;
                    if (c.pixels) ui.image(c, cb);
                }
                if (f && !disabled && L.cbHl.pixels) ui.image(L.cbHl, cb, 0xFFFFFFFFu, Blend::Add);  // LockHighlight
            }
            if (f) {  // OnEnter: ROLE_DESCRIPTIONn / GUIDE_TOOLTIP, YOUR_CLASS_MAY_NOT_PERFORM_ROLE (ANCHOR_RIGHT)
                std::vector<WotlkTipLine> t;
                t.push_back(WotlkTipLine{pp::cs(kRoleDesc[k]), kColWhiteC});
                if (perm && k < 3) t.push_back(WotlkTipLine{CLIENT_STR(YOUR_CLASS_MAY_NOT_PERFORM_ROLE), kColRedPure});
                wotlkTip(screenRect(br), std::move(t), kTipRight);
            }
        }
    }

    // LFDQueueFrameTypeDropDown (TOPLEFT 131,-125; SetWidth 180): CHOOSE_YOUR_DUNGEON (GameFontNormal, RIGHT at its LEFT 0,2), the
    // text (GameFontHighlightSmall, RIGHT at the right cap -43,2), the arrow button (TOPRIGHT of the right cap -16,-18).
    {
        ui.textShadow(fs, PX(131), PY(139) - lh * 0.5f, CLIENT_STR(CHOOSE_YOUR_DUNGEON), kColGoldC, Align::Right);
        const Rect hit = FR(150, 125, 195, 32);
        const bool f = live && winFocus_.addStop(kIdLfdType, hit);
        const char* text = L.type == kSpecific ? CLIENT_STR(SPECIFIC_DUNGEONS) : typeRec ? db.str(typeRec->name) : "";
        const Rect tr{PX(172), PY(138) - lh * 0.5f, PX(318) - PX(172), lh};
        if (static_cast<float>(fs.measure(text)) <= tr.w) ui.textShadow(fs, tr.x + tr.w, tr.y, text, kColWhiteC, Align::Right);
        else ui.textMarquee(fs, tr, text, kColWhiteC);
        const Rect b = FR(321, 126, 24, 24);
        const pp::Texture& bt = (f && winFocus_.isPressed(kIdLfdType)) || L.ddOpen ? L.ddBtnDn : L.ddBtn;
        if (bt.pixels) ui.image(bt, b);
        if (f) ui.fill(b, 0x40FFFFFFu);
    }

    // LFDQueueFrameCooldownFrame (LFDQueueFrameRandomCooldownFrame_Update): the deserter / cooldown auras.
    const int32_t deserterMs = auraLeftMs(&session_.playerAuras(), kDeserterSpell);
    const int32_t cooldownMs = auraLeftMs(&session_.playerAuras(), kCooldownSpell);
    const int32_t myMs = deserterMs >= 0 ? deserterMs : cooldownMs;
    bool hasDeserter = deserterMs >= 0, cdShow = myMs > 0;
    struct MemberCd {
        std::string name;
        uint8_t state;  // 0 ready, 1 deserter, 2 cooldown
    };
    std::vector<MemberCd> memberCd;
    for (const auto& m : session_.getPartyData().members) {
        if (m.guid == session_.playerGuid()) continue;
        const auto* auras = session_.unitAuras(m.guid);
        uint8_t st = 0;
        if (auraLeftMs(auras, kDeserterSpell) >= 0) {
            st = 1;
            hasDeserter = true;
            cdShow = true;
        } else if (auraLeftMs(auras, kCooldownSpell) >= 0) {
            st = 2;
            cdShow = true;
        }
        memberCd.push_back(MemberCd{m.name, st});
    }
    const bool specific = L.type == kSpecific;
    const bool covered = cdShow && (hasDeserter || !specific);  // a deserter covers both panels, a cooldown the random
    const bool panelLive = live && !covered;

    if (specific) {
        // --- LFDQueueFrameSpecific (LFDQueueFrameSpecificList_Update) ---------------------------------------------
        const int total = static_cast<int>(L.list.size());
        L.listScroll = std::clamp(L.listScroll, 0, std::max(0, total - kListRows));
        // The list follows the focus (as the trainer): the focus on the first / last row scrolls one row.
        const bool onRow = (focused >= kIdLfdRow0 && focused < kIdLfdRow0 + kListRows) ||
                           (focused >= kIdLfdExp0 && focused < kIdLfdExp0 + kListRows);
        if (panelLive && onRow) {
            const uint32_t base = focused >= kIdLfdExp0 ? kIdLfdExp0 : kIdLfdRow0;
            const int vi = static_cast<int>(focused - base);
            if (vi == kListRows - 1 && L.listScroll + kListRows < total) {
                ++L.listScroll;
                winFocus_.setFocus(focused - 1);
            } else if (vi == 0 && L.listScroll > 0) {
                --L.listScroll;
                winFocus_.setFocus(focused + 1);
            }
        }
        const bool scrollShown = total > kListRows;  // FauxScrollFrame_Update
        const float rowW = scrollShown ? 295.0f : 315.0f;  // areButtonsBig
        const bool clickable = !(mode == kModeRoleCheck || mode == kModeQueued || mode == kModeListed) && empowered;
        const bool queuedView = mode == kModeQueued || mode == kModeListed;
        for (int v = 0; v < kListRows; ++v) {
            const int i = L.listScroll + v;
            if (i >= total) break;
            const int32_t id = L.list[static_cast<size_t>(i)];
            const float ry = kListY + kRowU * static_cast<float>(v);
            const Rect rr = FR(kListX, ry, rowW, kRowU);
            const float cy = rr.y + rr.h * 0.5f;
            const bool header = id < 0;
            const bool locked = L.locked.count(id) && L.locked[id];
            // The row: its check box (or the lock) - PSP: one stop over the row's hit rect (insets 25 / 58).
            const Rect hit{PX(kListX + 25), rr.y, (rowW - 25.0f - 58.0f) * kSX, rr.h};
            const bool f = panelLive && winFocus_.addStop(kIdLfdRow0 + static_cast<uint32_t>(v), hit);
            if (header) {
                const pp::LfgGroupRec* g = db.group(static_cast<uint32_t>(-id));
                const char* name = g ? db.str(g->name) : "";
                const bool heroic = g && g->typeId == kTypeHeroic;
                float nx = PX(kListX + 40);
                if (heroic && L.heroic.pixels) {  // $parentHeroicIcon (16 x 20 at LEFT 40,-1)
                    ui.image(L.heroic, Rect{PX(kListX + 40), cy - 10.0f * kSY + 1.0f * kSY, 16 * kSX, 20 * kSY});
                    nx = PX(kListX + 56);
                }
                ui.pushClip(Rect{nx, rr.y, rr.x + rr.w - nx, rr.h});
                ui.textShadow(fs, nx, cy - lh * 0.5f, name, kColHeader);  // QuestDifficulty_Header
                ui.popClip();
                // $parentExpandOrCollapseButton (13 x 13 at LEFT 3,0): UI-PlusButton-UP / UI-MinusButton-UP.
                const Rect eb{PX(kListX + 3), cy - 6.5f * kSY, 16 * kSX, 13 * kSY};
                const bool fe = panelLive && winFocus_.addStop(kIdLfdExp0 + static_cast<uint32_t>(v), eb);
                const bool col = L.collapsed.count(id) && L.collapsed[id];
                const Rect ic{PX(kListX + 6), cy - 5.0f, 10, 10};
                const pp::Texture& pm = col ? L.plus : L.minus;
                if (pm.pixels) ui.image(pm, ic);
                if (fe && L.plusHl.pixels) ui.image(L.plusHl, ic, 0xFFFFFFFFu, Blend::Add);
            } else {
                const pp::LfgDungeonRec* d = db.dungeon(static_cast<uint32_t>(id));
                if (!d) continue;
                char lv[24];
                if (d->minLevel == d->maxLevel) std::snprintf(lv, sizeof(lv), "(%u)", static_cast<unsigned>(d->minLevel));
                else std::snprintf(lv, sizeof(lv), "(%u - %u)", static_cast<unsigned>(d->minLevel),
                                   static_cast<unsigned>(d->maxLevel));  // LFD_LEVEL_FORMAT_SINGLE / _RANGE
                const uint32_t dc = difficultyColor(d->recLevel, level);
                const bool grey = mode == kModeRoleCheck || mode == kModeQueued || mode == kModeListed || !empowered;
                // $parentInstanceLevel (60 x 16 at RIGHT 0,0), $parentInstanceName (LEFT 40,0 .. the level's LEFT -10).
                const float lx = rr.x + rr.w - 60.0f * kSX;
                ui.textShadow(fs, lx, cy - lh * 0.5f, lv, dc);
                const float nx = PX(kListX + 40);
                const char* name = db.str(d->name);
                const Rect nb{nx, cy - lh * 0.5f, lx - 10.0f * kSX - nx, lh};
                if (f && static_cast<float>(fs.measure(name)) > nb.w) {
                    ui.textMarquee(fs, nb, name, grey ? kColHeader : dc);
                } else {
                    ui.pushClip(Rect{nb.x, rr.y, nb.w, rr.h});
                    ui.textShadow(fs, nb.x, nb.y, name, grey ? kColHeader : dc);
                    ui.popClip();
                }
            }
            // $parentLockedIndicator (12 x 14 at LEFT 25,0) or $parentEnableButton (20 x 20 on it).
            const float bx = kListX + 25.0f + 6.0f;
            if (locked && !header) {
                if (L.lock.pixels) ui.image(L.lock, Rect{PX(bx) - 6.0f * kSX, cy - 7.0f * kSY, 12 * kSX, 14 * kSY});
                if (f) {  // LFDQueueFrameDungeonListButton_OnEnter (ANCHOR_TOP)
                    std::vector<WotlkTipLine> t;
                    t.push_back(WotlkTipLine{CLIENT_STR(YOU_MAY_NOT_QUEUE_FOR_DUNGEON), kColWhiteC});
                    const std::string why = lfdDeclinedMessage(static_cast<uint32_t>(id));
                    size_t p = 0;
                    while (p <= why.size() && !why.empty()) {
                        const size_t e = why.find('\n', p);
                        t.push_back(WotlkTipLine{why.substr(p, e == std::string::npos ? std::string::npos : e - p), kColGoldC});
                        if (e == std::string::npos) break;
                        p = e + 1;
                    }
                    wotlkTip(screenRect(hit), std::move(t), kTipTop);
                }
            } else if (!locked) {
                const auto& en = queuedView ? L.queued : L.enabled;
                auto it = en.find(id);
                const int state = it == en.end() ? (header ? -1 : 0) : it->second;
                const bool checked = header ? state > 0 : state != 0;
                const Rect cb{PX(bx) - 10.0f * kSX, cy - 10.0f * kSY, 20 * kSX, 20 * kSY};
                if (L.cbUp.pixels) ui.image(L.cbUp, cb);
                if (checked) {
                    const bool multi = header && state == 1;  // UI-MultiCheck-Up / -Disabled
                    const pp::Texture& c = multi ? (clickable ? L.cbMulti : L.cbMultiDis) : (clickable ? L.cbCheck : L.cbCheckDis);
                    if (c.pixels) ui.image(c, cb);
                }
                if (f && clickable && L.cbHl.pixels) ui.image(L.cbHl, cb, 0xFFFFFFFFu, Blend::Add);
            }
        }
        if (scrollShown) {  // LFDQueueFrameSpecificListScrollFrame's bar and arrows
            if (L.sbar.pixels) ui.image(L.sbar, FR(319, 153, 31, 261));
            const bool canUp = L.listScroll > 0, canDn = L.listScroll + kListRows < total;
            const Rect up{PX(328), PY(160), 11, 11}, dn{PX(328), PY(410) - 11, 11, 11};
            const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
            const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
            if (tu.pixels) ui.image(tu, up);
            if (td.pixels) ui.image(td, dn);
            if (panelLive && canUp && winFocus_.addStop(kIdLfdUp, up)) ui.fill(up, 0x40FFFFFFu);
            if (panelLive && canDn && winFocus_.addStop(kIdLfdDown, dn)) ui.fill(dn, 0x40FFFFFFu);
        }
    } else if (L.type != 0) {
        // --- LFDQueueFrameRandom (LFDQueueFrameRandom_UpdateFrame) -----------------------------------------------
        const game::LfgReward* rw = nullptr;
        for (const auto& r : session_.lfgRandomDungeons())
            if (r.dungeonId == L.type) rw = &r;
        const bool holiday = typeRec && typeRec->groupId == kHolidayGroup;
        const bool done = rw && rw->done;
        const uint32_t money = rw ? rw->money : 0, xp = rw ? rw->experience : 0;  // moneyVar / experienceVar: 0 (AC)
        const size_t numRewards = rw ? rw->items.size() : 0;
        constexpr float vx = 23.0f, vy = 157.0f, vw = 298.0f;
        const Rect view = FR(vx, vy, vw, kRandomViewU);
        auto CX = [&](float x) { return PX(vx + x); };
        auto CY = [&](float y) { return PY(vy + y - L.randomScroll); };
        const float tl = static_cast<float>(fn.lineHeight()) / kSY, ql = lh / kSY;  // line heights in units
        const float wrapW = 280.0f * kSX;
        ui.pushClip(view);
        float y = 15.0f;
        // $parentTitle (QuestTitleFontBlackShadow: gold, black shadow) at TOPLEFT 10,-15.
        ui.textShadow(fn, CX(10), CY(y), holiday ? db.str(typeRec->name) : CLIENT_STR(LFG_TYPE_RANDOM_DUNGEON), kColGoldC);
        y += tl + 5.0f;
        // $parentDescription (QuestFont: black, 280 wide).
        const char* explanation = holiday ? db.str(typeRec->desc) : CLIENT_STR(LFD_RANDOM_EXPLANATION);
        for (const std::string& l : wrapQuestText(fs, explanation, wrapW)) {
            ui.text(fs, CX(10), CY(y), l.c_str(), kColBlack);
            y += ql;
        }
        const bool showRewards = numRewards > 0 || money > 0 || xp > 0;
        if (showRewards) {
            y += 10.0f;
            ui.textShadow(fn, CX(10), CY(y), CLIENT_STR(LFD_REWARDS), kColGoldC);  // $parentRewardsLabel
            y += tl + 5.0f;
            const char* rd = holiday ? (done ? CLIENT_STR(LFD_HOLIDAY_REWARD_EXPLANATION2)
                                             : CLIENT_STR(LFD_HOLIDAY_REWARD_EXPLANATION1))
                                     : (done ? CLIENT_STR(LFD_RANDOM_REWARD_EXPLANATION2)
                                             : CLIENT_STR(LFD_RANDOM_REWARD_EXPLANATION1));
            for (const std::string& l : wrapQuestText(fs, rd, wrapW)) {
                ui.text(fs, CX(10), CY(y), l.c_str(), kColBlack);
                y += ql;
            }
        }
        // $parentItem1..n (LFDRandomDungeonLootTemplate = LargeItemButtonTemplate, 147 x 41, two a row, 5 apart).
        if (numRewards > 0) {
            y += 10.0f;
            const float side = 39.0f * kSY;
            for (size_t k = 0; k < numRewards && k < 8; ++k) {
                const game::LfgRewardItem& it = rw->items[k];
                const float bx = 10.0f + 147.0f * static_cast<float>(k % 2), by = y + 46.0f * static_cast<float>(k / 2);
                const Rect ic{CX(bx), CY(by), side, side};
                if (L.nameFr.pixels) ui.image(L.nameFr, Rect{CX(bx + 29), CY(by + 19.5f - 32), 128 * kSX, 64 * kSY});
                drawItemButton(ui, ic, it.displayInfoId, it.count, -1, 0xFFFFFFFFu);
                session_.ensureItemInfo(it.itemId);
                const auto* info = session_.getItemInfo(it.itemId);
                if (info && info->valid) {  // $parentName (GameFontHighlight, 90 x 36 at the name frame +15)
                    const Rect nb{CX(bx + 44), CY(by + 19.5f - 18), 90 * kSX, 36 * kSY};
                    const auto lines = wrapQuestText(ft, info->name, nb.w);
                    if (static_cast<float>(fs.measure(info->name.c_str())) <= nb.w) {
                        ui.textShadow(fs, nb.x, nb.y + (nb.h - lh) * 0.5f, info->name.c_str(), kColWhiteC);
                    } else if (lines.size() <= 2) {
                        const float th = static_cast<float>(ft.lineHeight());
                        float ly = nb.y + (nb.h - th * static_cast<float>(lines.size())) * 0.5f;
                        for (const std::string& l : lines) {
                            ui.textShadow(ft, nb.x, ly, l.c_str(), kColWhiteC);
                            ly += th;
                        }
                    } else {
                        ui.textMarquee(ft, Rect{nb.x, nb.y + (nb.h - 8.0f) * 0.5f, nb.w, 8.0f}, info->name.c_str(),
                                       kColWhiteC);
                    }
                }
                const float midY = ic.y + ic.h * 0.5f;
                const uint32_t rid = kIdLfdReward0 + static_cast<uint32_t>(k);
                if (panelLive && midY >= view.y && midY <= view.y + view.h && winFocus_.addStop(rid, ic)) {
                    if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, ic, 0xFFFFFFFFu, Blend::Add);
                    if (info && info->valid) {  // GameTooltip:SetLFGDungeonReward
                        tipProtoItem_ = it.itemId;
                        tipProtoCount_ = std::max<uint32_t>(1u, it.count);
                        tipProtoRect_ = Rect{ic.x + ic.w + ui.offsetX(), ic.y + ui.offsetY(), ic.w, ic.h};
                    }
                    if (ic.y + ic.h > view.y + view.h) L.randomScroll += (ic.y + ic.h - view.y - view.h) / kSY + 2.0f;
                    if (ic.y < view.y) L.randomScroll -= (view.y - ic.y) / kSY + 2.0f;
                }
            }
            y += 46.0f * static_cast<float>((numRewards + 1) / 2) - 5.0f;
        }
        // $parentMoneyLabel (TOPLEFT of the last BOTTOMLEFT 20,-10) + MoneyFrame; $parentXPLabel + $parentXPAmount.
        if (money > 0) {
            y += 10.0f;
            ui.text(fs, CX(30), CY(y), CLIENT_STR(MONEY_COLON), kColBlack);
            drawMoneyFrame(ui, CX(30) + static_cast<float>(fs.measure(CLIENT_STR(MONEY_COLON))) + 10.0f * kSX, CY(y), money,
                           false);
            y += ql;
        }
        if (xp > 0) {
            y += money > 0 ? 5.0f : 10.0f;
            ui.text(fs, CX(30), CY(y), CLIENT_STR(EXPERIENCE_COLON), kColBlack);
            char b[16];
            std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(xp));
            ui.textShadow(fs, CX(30) + static_cast<float>(fs.measure(CLIENT_STR(EXPERIENCE_COLON))) + 10.0f * kSX, CY(y), b,
                          kColWhiteC);  // NumberFontNormalLarge
            y += ql;
        }
        y += 10.0f;  // $parentSpacer
        ui.popClip();
        L.randomMax = std::max(0.0f, y - kRandomViewU);
        L.randomScroll = std::clamp(L.randomScroll, 0.0f, L.randomMax);
        if (L.randomMax > 0.0f) {  // UIPanelScrollFrameTemplate's arrows (the bar art is baked into the frame)
            ui.fill(FR(321, 162, 29, 250), 0x8C000000u);  // $parentScrollBackground (black, 0.55)
            const bool canUp = L.randomScroll > 0.0f, canDn = L.randomScroll < L.randomMax;
            const Rect up{PX(328), PY(159), 11, 11}, dn{PX(328), PY(410) - 11, 11, 11};
            const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
            const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
            if (tu.pixels) ui.image(tu, up);
            if (td.pixels) ui.image(td, dn);
            if (panelLive && canUp && winFocus_.addStop(kIdLfdRUp, up)) ui.fill(up, 0x40FFFFFFu);
            if (panelLive && canDn && winFocus_.addStop(kIdLfdRDown, dn)) ui.fill(dn, 0x40FFFFFFu);
        }
    }

    // LFDQueueFrameCooldownFrame (330 x 257 at BOTTOMRIGHT -7,28; the black filter 0.93).
    if (covered) {
        ui.fill(FR(18, 155, 330, 257), 0xED000000u);
        const char* desc = myMs > 0 ? (deserterMs >= 0 ? CLIENT_STR(LFG_DESERTER_YOU) : CLIENT_STR(LFG_RANDOM_COOLDOWN_YOU))
                                    : (hasDeserter ? CLIENT_STR(LFG_DESERTER_OTHER) : CLIENT_STR(LFG_RANDOM_COOLDOWN_OTHER));
        float y = PY(155.0f + (memberCd.empty() ? 85.0f : 30.0f));  // $parentDescription TOP 0,-85 (alone) / -30
        for (const std::string& l : wrapQuestText(fs, desc, 300.0f * kSX)) {
            ui.textShadow(fs, PX(183), y, l.c_str(), kColGoldC, Align::Center);
            y += lh;
        }
        if (myMs > 0) {  // $parentTime (GameFontHighlightLarge): SecondsToTime(ceil(myExpireTime - GetTime()))
            y += 10.0f * kSY;
            ui.textShadow(fn, PX(183), y, secondsToTime((myMs + 999) / 1000).c_str(), kColWhiteC, Align::Center);
            y += static_cast<float>(fn.lineHeight());
        }
        y += 60.0f * kSY - (myMs > 0 ? static_cast<float>(fn.lineHeight()) + 10.0f * kSY : 0.0f);
        for (const MemberCd& m : memberCd) {  // $parentName1..4 / $parentStatus1..4
            ui.textShadow(fs, PX(58), y, m.name.c_str(), kColGoldC);
            const char* st = m.state == 1 ? CLIENT_STR(DESERTER) : m.state == 2 ? CLIENT_STR(ON_COOLDOWN) : CLIENT_STR(READY);
            ui.textShadow(fs, PX(288), y, st, m.state ? kColRedC : kColGreenC, Align::Right);
            y += lh + 5.0f * kSY;
        }
    }

    // LFDQueueFrameFindGroupButton (135 x 22 at BOTTOMLEFT 18,6) and LFDQueueFrameCancelButton (112 x 22, BOTTOMRIGHT).
    {
        const char* label = (mode == kModeQueued || mode == kModeRoleCheck || mode == kModeProposal) ? CLIENT_STR(LEAVE_QUEUE)
                            : inParty                                                              ? CLIENT_STR(JOIN_AS_PARTY)
                                                                                                   : CLIENT_STR(FIND_A_GROUP);
        const bool enabled = empowered && mode != kModeProposal && mode != kModeListed;
        wotlkButton(ui, kIdLfdFind, FR(18, 412, 135, 22), label, enabled, live);
        wotlkButton(ui, kIdLfdCancel, FR(236, 412, 112, 22), CLIENT_STR(CLOSE), true, live);
    }

    // The open dropdown list (LFDQueueFrameTypeDropDown_Initialize).
    if (L.ddOpen) {
        std::vector<WotlkDdItem> items;
        L.ddValues.clear();
        L.ddDisabled.clear();
        WotlkDdItem s;
        s.text = CLIENT_STR(SPECIFIC_DUNGEONS);
        s.checked = L.type == kSpecific;
        items.push_back(s);
        L.ddValues.push_back(kSpecific);
        L.ddDisabled.push_back(false);
        for (const auto& r : session_.lfgRandomDungeons()) {
            if (items.size() >= static_cast<size_t>(kDdMax)) break;
            const pp::LfgDungeonRec* d = db.dungeon(r.dungeonId);
            if (!d) continue;
            // isRandomDungeonDisplayable: the level and the expansion.
            if (level < d->minLevel || level > d->maxLevel || kExpansionLevel < d->expansion) continue;
            bool joinable = true;  // IsLFGDungeonJoinable
            if (auto it = session_.lfgLocks().find(r.dungeonId); it != session_.lfgLocks().end() && it->second) joinable = false;
            for (const auto& pl : session_.lfgPartyLocks())
                if (auto it = pl.locks.find(r.dungeonId); it != pl.locks.end() && it->second) joinable = false;
            WotlkDdItem it;
            it.text = db.str(d->name);
            if (joinable) {
                it.checked = L.type == r.dungeonId;
            } else {
                it.disabled = true;
                it.tipTitle = CLIENT_STR(YOU_MAY_NOT_QUEUE_FOR_THIS);
                it.tipText = lfdDeclinedMessage(r.dungeonId);
            }
            items.push_back(std::move(it));
            L.ddValues.push_back(r.dungeonId);
            L.ddDisabled.push_back(!joinable);
        }
        wotlkDropDownList(ui, PX(147), PY(152), items, kIdLfdDd0, focusable);
    }
    return true;
}

// --- Drawing: the popups -----------------------------------------------------------------------------------------------

bool WorldScreen::drawLfdReadyPopup(UiRenderer& ui) {
    LfdUi& L = lfdUi();
    if (session_.lfgProposalId() == 0 && L.readyCloseIn < 0.0f) {  // LFDDungeonReadyPopup_Update: no proposal
        closeWindow(kWinLfdReady);
        return true;
    }
    pp::LfgDb& db = useDb(session_, L.dbUsed);
    L.loadPopupArt();
    loadNpcArt(false);
    PROF_SCOPE("lfd.ready");
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight());
    const bool focusable = uiMode_ && !settings_.isOpen();
    if (L.readyGuard && !winFocus_.isPressed(winFocus_.focused())) L.readyGuard = false;
    auto screenRect = [&](const Rect& r) { return Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h}; };
    const bool answered = session_.lfgProposalAnswered();
    const pp::LfgDungeonRec* d = db.dungeon(session_.lfgDungeonId());
    const auto& members = session_.lfgProposalMembers();
    const float W = U(306);
    auto place = [&](float hUnits) {
        const Rect r{std::floor((static_cast<float>(pp::kScreenW) - W) * 0.5f),
                     popupStackY_ > 0.0f ? popupStackY_ : kPopupTop, W, U(hUnits)};
        popupStackY_ = r.y + r.h;
        return r;
    };
    // The hide button (UI-Panel-HideButton, 32 x 32 at TOPRIGHT -2,-2): StaticPopupSpecial_Hide.
    auto hideButton = [&](const Rect& r) {
        const Rect hb{r.x + r.w - U(34), r.y + U(2), U(32), U(32)};
        const bool f = focusable && winFocus_.addStop(kIdReadyHide, hb);
        const pp::Texture& t = f && winFocus_.isPressed(kIdReadyHide) && L.hideDn.pixels ? L.hideDn : L.hide;
        if (t.pixels) ui.image(t, hb);
        if (f && L.hideHl.pixels) ui.image(L.hideHl, hb, 0xFFFFFFFFu, Blend::Add);
    };
    auto button = [&](uint32_t id, const Rect& b, const char* label) {  // UIPanelButtonTemplate2 at 0.8
        const bool f = focusable && winFocus_.addStop(id, b);
        const bool pressed = f && winFocus_.isPressed(id);
        const pp::Texture& t = pressed && npcArt_.btnDown.pixels ? npcArt_.btnDown : npcArt_.btnUp;
        if (t.pixels) {
            const float cap = 7.0f, tw = t.uw, th = t.uh, capT = 6.0f * tw / 64.0f;
            ui.image(t, Rect{b.x, b.y, cap, b.h}, SrcRect{0, 0, capT, th});
            ui.image(t, Rect{b.x + cap, b.y, b.w - 2 * cap, b.h}, SrcRect{capT, 0, tw - 2 * capT, th});
            ui.image(t, Rect{b.x + b.w - cap, b.y, cap, b.h}, SrcRect{tw - capT, 0, capT, th});
        } else {
            ui.fill(b, 0xFF101060u);
        }
        if (f && npcArt_.btnHl.pixels) ui.image(npcArt_.btnHl, b, 0xFFFFFFFFu, Blend::Add);
        const Font& bf = static_cast<float>(fn.measure(label)) <= b.w - 6.0f ? fn : fs;
        ui.textShadow(bf, b.x + b.w * 0.5f + (pressed ? 1.0f : 0.0f),
                      b.y + std::floor((b.h - static_cast<float>(bf.lineHeight())) * 0.5f + 0.5f) + (pressed ? 1.0f : 0.0f),
                      label, f ? kColWhiteC : kColGoldC, Align::Center);
    };

    if (answered) {
        // --- LFDDungeonReadyStatus (306 x 120): READY_CHECK, the members' role icons with their answers ----------------
        const Rect r = place(120);
        drawDialogBox(ui, r);
        ui.textShadow(fn, r.x + r.w * 0.5f, r.y + U(15), CLIENT_STR(READY_CHECK), kColWhiteC, Align::Center);
        hideButton(r);
        const int n = std::min<int>(kNumMembers, static_cast<int>(members.size()));
        for (int i = 0; i < n; ++i) {
            const game::LfgProposalMember& m = members[static_cast<size_t>(i)];
            const Rect pr{r.x + U(15.0f + 55.0f * static_cast<float>(i)), r.y + U(42.5f), U(55), U(55)};
            if (L.rolesL.pixels) ui.image(L.rolesL, pr, roleCell(roleOfMask(m.role), L.rolesL.uw));
            const pp::Texture* st = &L.rcWait;  // READY_CHECK_WAITING_TEXTURE
            if (m.answered && m.accepted) {
                st = &L.rcReady;
                if (!L.readyAccepted[static_cast<size_t>(i)]) {  // readyStatus "accepted": the sound once
                    L.readyAccepted[static_cast<size_t>(i)] = true;
                    playSoundByName("LFG_RoleCheck");
                }
            } else if (m.answered) {
                st = &L.rcNotReady;
            }
            if (st->pixels) ui.image(*st, Rect{pr.x + U(5), pr.y + U(25), U(30), U(30)});
        }
        return true;
    }

    // --- LFDDungeonReadyDialog -----------------------------------------------------------------------------------------
    const uint8_t typeId = d ? d->typeId : 0;
    const bool random = typeId == kTypeRandom;
    const int completed = popcount(session_.lfgProposalEncounters());
    const int total = d ? d->encounters : 0;
    const float H = random && completed == 0 ? 193.0f : 223.0f;
    const Rect r = place(H);
    drawDialogBox(ui, r);
    // $parentBackground (294 wide, TOP 0,-11 .. BOTTOM 0,64): UI-LFG-BACKGROUND-RANDOMDUNGEON / -<TextureFilename>.
    L.loadReadyBg(random ? std::string("randomdungeon") : d && db.str(d->texture)[0] ? std::string(db.str(d->texture))
                                                                                      : std::string("deadmines"));
    if (L.readyBg.pixels) {
        const Rect bg{r.x + U(6), r.y + U(11), U(294), U(H - 11.0f - 64.0f)};
        const float vh = random && completed == 0 ? static_cast<float>(L.readyBg.uh) * 118.0f / 128.0f
                                                  : static_cast<float>(L.readyBg.uh);
        ui.image(L.readyBg, bg, SrcRect{0, 0, static_cast<float>(L.readyBg.uw), vh});
    }
    if (L.fili.pixels) ui.image(L.fili, Rect{r.x + U(7), r.y + U(3), U(292), U(54)});         // $parentFiligree
    if (L.bottom.pixels) ui.image(L.bottom, Rect{r.x + U(9.5f), r.y + r.h - U(127), U(287), U(72)});  // $parentBottomArt
    hideButton(r);
    // $parentLabel (GameFontHighlight, TOP 0,-15). PSP: 200 units wide (the original 150).
    float y = r.y + U(15);
    const char* label = random ? CLIENT_STR(RANDOM_DUNGEON_IS_READY) : CLIENT_STR(SPECIFIC_DUNGEON_IS_READY);
    for (const std::string& l : wrapQuestText(fs, label, U(200))) {
        ui.textShadow(fs, r.x + r.w * 0.5f, y, l.c_str(), kColWhiteC, Align::Center);
        y += lh;
    }
    const float cx = r.x + r.w * 0.5f;
    if (random) {
        if (completed > 0) {  // $parentRandomInProgressFrame (170 x 30 at the label's BOTTOM 0,-5)
            const float top = y + U(5);
            if (L.sep.pixels) ui.image(L.sep, Rect{cx - U(105), top, U(210), U(40)});
            ui.textShadow(fs, cx, top + U(30) - lh, CLIENT_STR(THIS_DUNGEON_IN_PROGRESS), kColGoldC, Align::Center);
        }
    } else {
        // $parentInstanceInfoFrame (170 x 50, BOTTOM at the role icon's TOP 0,10): the name, BOSSES_KILLED.
        const float top = r.y + r.h - U(181);
        if (L.sep.pixels) ui.image(L.sep, Rect{cx - U(85), top, U(170), U(40)});
        const char* name = d ? db.str(d->name) : "";
        const Font& nf = static_cast<float>(fn.measure(name)) + U(20) > r.w ? fs : fn;  // GameFontNormalLarge / Normal
        ui.textShadow(nf, cx, top + U(13), name, kColGoldC, Align::Center);
        const std::string b = CLIENT_FMT(BOSSES_KILLED, completed, total);
        ui.textShadow(fs, cx, top + U(50) - lh, b.c_str(), kColGoldC, Align::Center);
    }
    // $parentRoleIcon (67 x 67 at BOTTOM -1,54) with the leader icon; YOUR_ROLE and the role (right of x 108).
    const game::LfgProposalMember* self = nullptr;
    for (const auto& m : members)
        if (m.isSelf) self = &m;
    const int role = self ? roleOfMask(self->role) : 2;
    const Rect ri{r.x + U(152.0f - 33.5f), r.y + r.h - U(121), U(67), U(67)};
    if (L.rolesL.pixels) ui.image(L.rolesL, ri, roleCell(role, L.rolesL.uw));
    if (self && (self->role & 0x01) && L.proles.pixels) {
        // $parentLeaderIcon (19 x 19 at TOPLEFT 0,-4): UI-LFG-ICON-PORTRAITROLES (0, 0.296875, 0.015625, 0.3125).
        const float pw = static_cast<float>(L.proles.uw), ph = static_cast<float>(L.proles.uh);
        ui.image(L.proles, Rect{ri.x, ri.y + U(4), U(19), U(19)},
                 SrcRect{0, 0.015625f * ph, 0.296875f * pw, (0.3125f - 0.015625f) * ph});
    }
    ui.textShadow(ft, r.x + U(108), r.y + r.h - U(93) - static_cast<float>(ft.lineHeight()), CLIENT_STR(YOUR_ROLE),
                  kColWhiteC, Align::Right);
    ui.textShadow(fn, r.x + U(108), r.y + r.h - U(93), pp::cs(kRoleName[role]), kColGoldC, Align::Right);
    // $parentRewardsFrame (72 x 40 at the role icon's BOTTOMRIGHT 19,15): LFDDungeonReadyDialog_UpdateRewards.
    {
        const game::LfgReward* rw = nullptr;
        for (const auto& x : session_.lfgRandomDungeons())
            if (x.dungeonId == session_.lfgDungeonId()) rw = &x;
        const uint32_t money = rw ? rw->money : 0, xp = rw ? rw->experience : 0;
        const int numRewards = rw ? static_cast<int>(std::min<size_t>(rw->items.size(), 3)) : 0;
        const bool misc = money > 0 || xp > 0;
        const int used = numRewards + (misc ? 1 : 0);
        const float fx = r.x + U(152.0f + 33.5f + 19.0f), fy = r.y + r.h - U(109);
        if (used > 0) ui.textShadow(fs, fx + U(36), fy + U(40) - lh, CLIENT_STR(LFD_REWARDS), kColGoldC, Align::Center);
        const float ppi = 72.0f / (2.0f * static_cast<float>(std::max(1, used)));  // positionPerIcon (units)
        for (int k = 0; k < used; ++k) {
            const float ccx = fx + U(ppi + 2.0f * ppi * static_cast<float>(k)), ccy = fy + U(20.0f - 5.0f);
            const Rect ring{ccx - U(20), ccy - U(20), U(40), U(40)};
            const Rect tex{ccx - U(15) - U(3), ccy - U(15) - U(3), U(30), U(30)};  // CENTER -3,3
            const bool isMisc = misc && k == 0;
            if (isMisc) {
                if (L.coin.pixels) ui.image(L.coin, tex);  // SetPortraitToTexture inv_misc_coin_02
            } else {
                const auto& it = rw->items[static_cast<size_t>(k - (misc ? 1 : 0))];
                if (const pp::Texture* t = itemIcons().byDisplay(it.displayInfoId, spellFrame_)) ui.imageCircle(*t, tex);
            }
            if (L.ring.pixels) ui.image(L.ring, ring);
            const uint32_t id = kIdReadyReward0 + static_cast<uint32_t>(k);
            if (focusable && winFocus_.addStop(id, ring)) {  // LFDDungeonReadyDialogReward_OnEnter (ANCHOR_RIGHT)
                if (isMisc) {
                    std::vector<WotlkTipLine> t;
                    t.push_back(WotlkTipLine{CLIENT_STR(REWARD_ITEMS_ONLY), kColGoldC});
                    if (xp > 0) {
                        t.push_back(WotlkTipLine{pp::text::uiPlain(CLIENT_FMT(GAIN_EXPERIENCE, static_cast<unsigned>(xp))),
                                                 kColWhiteC});
                    }
                    if (money > 0) {
                        WotlkTipLine ml;
                        ml.money = money;
                        t.push_back(ml);
                    }
                    wotlkTip(screenRect(ring), std::move(t), kTipRight);
                } else {
                    const auto& it = rw->items[static_cast<size_t>(k - (misc ? 1 : 0))];
                    session_.ensureItemInfo(it.itemId);
                    const auto* info = session_.getItemInfo(it.itemId);
                    if (info && info->valid) {
                        tipProtoItem_ = it.itemId;
                        tipProtoCount_ = std::max<uint32_t>(1u, it.count);
                        tipProtoRect_ = screenRect(ring);
                    }
                }
            }
        }
    }
    // $parentEnterDungeonButton (115 x 22, BOTTOMRIGHT at BOTTOM -7,25) / $parentLeaveQueueButton (BOTTOMLEFT 7,25).
    button(kIdReadyEnter, Rect{cx - U(7) - U(115), r.y + r.h - U(47), U(115), U(22)}, CLIENT_STR(ENTER_DUNGEON));
    button(kIdReadyLeave, Rect{cx + U(7), r.y + r.h - U(47), U(115), U(22)}, CLIENT_STR(LEAVE_QUEUE));
    return true;
}

bool WorldScreen::drawLfdRoleCheck(UiRenderer& ui) {
    LfdUi& L = lfdUi();
    pp::LfgDb& db = useDb(session_, L.dbUsed);
    L.loadPopupArt();
    L.loadArt();  // the check boxes
    loadNpcArt(false);
    PROF_SCOPE("lfd.rolecheck");
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const bool focusable = uiMode_ && !settings_.isOpen();
    if (L.roleGuard && !winFocus_.isPressed(winFocus_.focused())) L.roleGuard = false;
    auto screenRect = [&](const Rect& r) { return Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h}; };
    const float W = U(306);
    const Rect r{std::floor((static_cast<float>(pp::kScreenW) - W) * 0.5f), popupStackY_ > 0.0f ? popupStackY_ : kPopupTop,
                 W, U(180)};
    popupStackY_ = r.y + r.h;
    drawDialogBox(ui, r);
    ui.textShadow(fn, r.x + r.w * 0.5f, r.y + U(15), CLIENT_STR(CONFIRM_YOUR_ROLE), kColWhiteC, Align::Center);
    // $parentRoleButtonTank / Healer / DPS (70 x 70: TOPLEFT 35,-35, then 15 apart).
    const uint8_t cls = session_.getPlayerClass();
    static constexpr uint8_t kBit[3] = {0x02, 0x04, 0x08};
    for (int k = 0; k < 3; ++k) {
        const bool avail = k == 0 ? (cls == 1 || cls == 2 || cls == 6 || cls == 11)
                           : k == 1 ? (cls == 2 || cls == 5 || cls == 7 || cls == 11)
                                    : true;
        const Rect br{r.x + U(35.0f + 85.0f * static_cast<float>(k)), r.y + U(35), U(70), U(70)};
        const bool f = focusable && winFocus_.addStop(kIdRoleCheck0 + static_cast<uint32_t>(k), br);
        if (L.rolesL.pixels) {
            ui.image(L.rolesL, br, roleCell(k, L.rolesL.uw), avail ? 0xFFFFFFFFu : kGreyTint);
            if (!avail) ui.image(L.rolesL, br, roleCell(4, L.rolesL.uw), withAlpha(0xFFFFFFFFu, 0.7f));
        }
        if (avail) {
            const Rect cb{br.x - U(5), br.y + U(51), U(24), U(24)};
            if (L.cbUp.pixels) ui.image(L.cbUp, cb);
            if ((L.roleCheckRoles & kBit[k]) && L.cbCheck.pixels) ui.image(L.cbCheck, cb);
            if (f && L.cbHl.pixels) ui.image(L.cbHl, cb, 0xFFFFFFFFu, Blend::Add);
        }
        if (f) {
            std::vector<WotlkTipLine> t;
            t.push_back(WotlkTipLine{pp::cs(kRoleDesc[k]), kColWhiteC});
            if (!avail) t.push_back(WotlkTipLine{CLIENT_STR(YOUR_CLASS_MAY_NOT_PERFORM_ROLE), kColRedPure});
            wotlkTip(screenRect(br), std::move(t), kTipRight);
        }
    }
    // $parentDescription (CENTER at BOTTOM 0,57): QUEUED_FOR with the dungeon in NORMAL_FONT_COLOR.
    {
        const auto& slots = session_.lfgRoleCheckDungeons();
        std::string name;
        if (slots.size() == 1) {
            const pp::LfgDungeonRec* dd = db.dungeon(slots[0].dungeonId);
            const char* n = dd ? db.str(dd->name) : "";
            if (slots[0].typeId == kTypeRandom) name = CLIENT_STR(A_RANDOM_DUNGEON);
            else if (slots[0].typeId == kTypeHeroic) name = CLIENT_FMT(HEROIC_PREFIX, n);
            else name = n;
        } else {
            name = CLIENT_STR(MULTIPLE_DUNGEONS);
        }
        std::string pre, post;  // QUEUED_FOR: the label white, the dungeon gold
        splitAround(CLIENT_STR(QUEUED_FOR), pre, post);
        const float pw = static_cast<float>(fs.measure(pre.c_str())), nw = static_cast<float>(fs.measure(name.c_str()));
        const float qw = static_cast<float>(fs.measure(post.c_str()));
        const float x0 = r.x + r.w * 0.5f - (pw + nw + qw) * 0.5f, ty = r.y + r.h - U(57) - lh * 0.5f;
        ui.textShadow(fs, x0, ty, pre.c_str(), kColWhiteC);
        ui.textShadow(fs, x0 + pw, ty, name.c_str(), kColGoldC);
        if (!post.empty()) ui.textShadow(fs, x0 + pw + nw, ty, post.c_str(), kColWhiteC);
        const Rect db2{x0, ty, pw + nw + qw, lh};
        if (slots.size() > 1 && focusable && winFocus_.addStop(kIdRoleDesc, db2)) {  // _OnEnter (ANCHOR_BOTTOM)
            std::vector<WotlkTipLine> t;
            t.push_back(WotlkTipLine{CLIENT_STR(QUEUED_FOR_SHORT), kColWhiteC});
            for (const auto& s : slots) {
                const pp::LfgDungeonRec* dd = db.dungeon(s.dungeonId);
                const std::string n = dd ? db.str(dd->name) : "";
                t.push_back(WotlkTipLine{"    " + (s.typeId == kTypeHeroic ? CLIENT_FMT(HEROIC_PREFIX, n) : n), kColWhiteC});
            }
            wotlkTip(screenRect(db2), std::move(t), kTipBottom);
        }
    }
    // $parentAcceptButton (115 x 22, BOTTOMRIGHT at BOTTOM -3,15) and $parentDeclineButton (LEFT at its RIGHT 6,0).
    const float bx = r.x + r.w * 0.5f - U(3) - U(115), by = r.y + r.h - U(37);
    wotlkButton(ui, kIdRoleAccept, Rect{bx, by, U(115), U(22)}, CLIENT_STR(ACCEPT), true, focusable);
    wotlkButton(ui, kIdRoleDecline, Rect{bx + U(121), by, U(115), U(22)}, CLIENT_STR(DECLINE), true, focusable);
    return true;
}

// StaticPopupDialogs VOTE_BOOT_PLAYER (YES / NO: SetLFGBootVote) and LFG_OFFER_CONTINUE (YES: PartyLFGStartBackfill).
bool WorldScreen::drawLfgVotePopup(UiRenderer& ui, uint8_t w) {
    LfdUi& L = lfdUi();
    bool& guard = w == kWinLfgBoot ? L.bootGuard : L.offerGuard;
    if (guard && !winFocus_.isPressed(winFocus_.focused())) guard = false;
    std::string text;
    StaticPopup p;
    if (w == kWinLfgBoot) {
        const auto& b = session_.lfgBoot();
        // VOTE_BOOT_PLAYER: "|3-3(%1$s)" declined, the reason in |cffffd200 (PSP: one colour - the popup's white);
        // the reason (the player's text) goes in after questText.
        text = pp::text::uiPlain(questText(CLIENT_FMT(VOTE_BOOT_PLAYER, session_.lfgBootTargetName(), "\x01")));
        text = replaceFirst(text, "\x01", b.reason);
        p.button1Id = kIdBootYes;
        p.button2Id = kIdBootNo;
    } else {
        pp::LfgDb& db = useDb(session_, L.dbUsed);
        text = CLIENT_FMT(LFG_OFFER_CONTINUE, db.name(L.offerDungeon));
        p.button1Id = kIdOfferYes;
        p.button2Id = kIdOfferNo;
    }
    p.text = text.c_str();
    p.button1 = CLIENT_STR(YES);
    p.button2 = CLIENT_STR(NO);
    p.focusable = uiMode_ && !settings_.isOpen();
    drawStaticPopup(ui, p);
    return true;
}

// MiniMapLFGFrameDropDown: UIDropDownMenu "MENU" (the tooltip backdrop) at the eye (under it; above-left while queued).
bool WorldScreen::drawWotlkDropDown(UiRenderer& ui) {
    if (!lfd_ || !uiMode_ || settings_.isOpen() || lfd_->menu.empty()) {
        closeWindow(kWinWotlkDropDown);
        return true;
    }
    LfdUi& L = *lfd_;
    loadNpcArt(false);
    const Font& f = kit_.font(kFontSmall);
    const float lh = static_cast<float>(f.lineHeight());
    constexpr float kRowH = 12.0f, kPad = 8.0f;
    float tw = 0.0f;
    for (const auto& m : L.menu) tw = std::max(tw, static_cast<float>(f.measure(m.first)));
    Rect r{0, 0, kPad * 2 + 14.0f + tw, kPad * 2 + kRowH * static_cast<float>(L.menu.size())};
    if (L.menuAbove) {  // point BOTTOMRIGHT, relativePoint TOPLEFT
        r.x = L.eyeRect.x - r.w;
        r.y = L.eyeRect.y - r.h;
    } else {  // TOPLEFT at the eye's BOTTOMLEFT 0,-5
        r.x = L.eyeRect.x;
        r.y = L.eyeRect.y + L.eyeRect.h + 3.0f;
    }
    r.x = std::clamp(r.x, 2.0f, static_cast<float>(pp::kScreenW) - 2.0f - r.w);
    r.y = std::clamp(r.y, 2.0f, static_cast<float>(pp::kScreenH) - 2.0f - r.h);
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    bool onMenu = winFocus_.focused() == kIdLfgEye;
    for (size_t i = 0; i < L.menu.size(); ++i) {
        const Rect ir{r.x + kPad, r.y + kPad + kRowH * static_cast<float>(i), r.w - kPad * 2, kRowH};
        const bool dis = L.menuDisabled[i];
        const bool focus = winFocus_.addStop(kIdEyeMenu0 + static_cast<uint32_t>(i), ir);
        onMenu = onMenu || focus;
        if (focus && !dis && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, ir, 0xFFFFFFFFu, Blend::Add);
        ui.text(f, ir.x + 14.0f, ir.y + (kRowH - lh) * 0.5f, L.menu[i].first, dis ? kColDisabled : kColWhiteC);
    }
    const uint32_t now = pp::logUptimeMs();
    if (onMenu) {
        L.menuAwayMs = 0;
    } else if (L.menuAwayMs == 0) {
        L.menuAwayMs = now ? now : 1;
    } else if (now - L.menuAwayMs > 2000) {  // UIDROPDOWNMENU_SHOW_TIME
        closeWindow(kWinWotlkDropDown);
    }
    return true;
}

// LFDSearchStatus (350 x 145 / 170 at the eye's TOPLEFT; the tooltip backdrop), shown while the eye has the focus and
// the player is queued (MiniMapLFGFrame_OnEnter). PSP: 0.6 of its units.
void WorldScreen::drawLfdSearchStatus(UiRenderer& ui) {
    LfdUi& L = *lfd_;
    if (!L.eyeHover || !uiMode_ || settings_.isOpen()) return;
    uint8_t sub = 0;
    if (lfgMode(&sub) != kModeQueued) return;
    const Font& fs = kit_.font(kFontSmall);
    const float lh = static_cast<float>(fs.lineHeight());
    const auto& q = session_.lfgQueueStats();
    const bool has = q.known;
    const float hU = has && q.myWait != -1 ? 170.0f : 145.0f;
    auto S = [](float u) { return u * kAS; };
    const Rect r{L.eyeRect.x - S(350), L.eyeRect.y, S(350), S(hU)};
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    ui.textShadow(fs, r.x + r.w * 0.5f, r.y + S(15), CLIENT_STR(ASSEMBLING_GROUP), kColWhiteC,
                  Align::Center);  // $parentTitle
    // Tank1, Healer1, Damage1..3 (55 x 55 from TOPLEFT 25,-40, 5 apart): LFDSearchStatusPlayer_SetFound.
    const pp::Texture& rt = L.mmRoles;
    for (int k = 0; k < 5; ++k) {
        const int role = k == 0 ? 0 : k == 1 ? 1 : 2;
        bool found = false;
        if (has) found = k == 0 ? q.needTank == 0 : k == 1 ? q.needHealer == 0 : (k - 1) <= 3 - static_cast<int>(q.needDps);
        const Rect pr{r.x + S(25.0f + 60.0f * static_cast<float>(k)), r.y + S(40), S(55), S(55)};
        if (rt.pixels) {
            ui.image(rt, pr, roleCell(role, rt.uw), found ? 0xFFFFFFFFu : kGreyTint);
            if (!found) ui.image(rt, pr, roleCell(4, rt.uw), withAlpha(0xFFFFFFFFu, 0.3f));
        }
    }
    const uint32_t now = pp::logUptimeMs();
    if (has && q.myWait != -1) {  // $parentStatistic (TOP 0,-90): SecondsToTime(myWait, false, false, 1)
        const std::string s = CLIENT_FMT(LFG_STATISTIC_AVERAGE_WAIT, secondsToTime(q.myWait, 1));
        ui.textShadow(fs, r.x + r.w * 0.5f, r.y + S(90) + (S(25) - lh) * 0.5f, s.c_str(), kColGoldC, Align::Center);
    }
    // $parentElapsedWaitTime (BOTTOM 0,38): GetTime() - queuedTime.
    {
        const int elapsed = has ? static_cast<int>(q.queuedSec + (now - q.receivedMs) / 1000u) : 0;
        const std::string s = CLIENT_FMT(TIME_IN_QUEUE, elapsed >= 60 ? secondsToTime(elapsed)
                                                                       : std::string(CLIENT_STR(LESS_THAN_ONE_MINUTE)));
        ui.textShadow(fs, r.x + r.w * 0.5f, r.y + r.h - S(38) - lh, s.c_str(), kColGoldC, Align::Center);
    }
    // $parentLookingFor (BOTTOM -extraWidth/2,14) + the role icons (25 x 25, 2 apart) of GetLFGRoles.
    {
        const uint8_t roles = session_.lfgRoles();
        int icons[3];
        int n = 0;
        if (roles & 0x02) icons[n++] = 0;
        if (roles & 0x04) icons[n++] = 1;
        if (roles & 0x08) icons[n++] = 2;
        const float extra = S(27.0f * static_cast<float>(n));
        const float tw = static_cast<float>(fs.measure(CLIENT_STR(SEARCHING_FOR_GROUPS_NEEDS)));
        const float tx = r.x + r.w * 0.5f - extra * 0.5f - tw * 0.5f, ty = r.y + r.h - S(14) - lh;
        ui.textShadow(fs, tx, ty, CLIENT_STR(SEARCHING_FOR_GROUPS_NEEDS), kColWhiteC);
        float ix = tx + tw + S(2);
        for (int i = 0; i < n; ++i) {
            if (rt.pixels) ui.image(rt, Rect{ix, ty + lh * 0.5f - S(12.5f), S(25), S(25)}, roleCell(icons[i], rt.uw));
            ix += S(27);
        }
    }
}

// --- MiniMapLFGFrame ------------------------------------------------------------------------------------------------------

void WorldScreen::drawMinimapLfg(UiRenderer& ui, bool focusable) {
    uint8_t sub = 0;
    const uint8_t mode = lfgMode(&sub);
    if (mode == kModeNone) return;  // MiniMapLFG_UpdateIsShown
    LfdUi& L = lfdUi();
    L.loadMmArt();
    L.eyeDrawn = true;
    namespace ml = minimap_layout;
    const float s = ml::kScale;
    // 33 x 33 at MinimapBackdrop's TOPLEFT 25,-100; the eye (30 x 30) centred, 1 px down-right while pushed
    // (MinimapButton_OnMouseDown); MiniMap-TrackingBorder (52 at TOPLEFT 1,-1: its ring); the zoom button highlight.
    const Rect r{ml::kBackdropX - 96.0f * s + 25.0f * s, ml::kBackdropY - 96.0f * s + 100.0f * s, 33.0f * s, 33.0f * s};
    const bool f = focusable && winFocus_.addStop(kIdLfgEye, r);
    const bool down = f && winFocus_.isPressed(kIdLfgEye);
    const float es = 30.0f * s;
    const Rect er{r.x + (r.w - es) * 0.5f + (down ? 1.0f : 0.0f), r.y + (r.h - es) * 0.5f + (down ? 1.0f : 0.0f), es, es};
    if (L.eye16.pixels) ui.image(L.eye16, er, eyeCell(L.eyeFrame, L.eye16.uw, L.eye16.uh));
    if (L.mmRing.pixels) ui.image(L.mmRing, Rect{r.x + 1.0f * s, r.y + 1.0f * s, 52.0f * 40.0f / 64.0f * s, 52.0f * 40.0f / 64.0f * s});
    if (f && L.mmHl.pixels) ui.image(L.mmHl, r, 0xFFFFFFFFu, Blend::Add);
    L.eyeHover = f;
    L.eyeRect = r;
    if (!f) return;
    // MiniMapLFGFrame_OnEnter: queued - LFDSearchStatus (drawn after the windows); else GameTooltip ANCHOR_LEFT.
    std::vector<WotlkTipLine> t;
    if (mode == kModeProposal) {
        t.push_back(WotlkTipLine{CLIENT_STR(LOOKING_FOR_DUNGEON), kColWhiteC});
        t.push_back(WotlkTipLine{CLIENT_STR(DUNGEON_GROUP_FOUND_TOOLTIP), kColGoldC});
        t.push_back(WotlkTipLine{" ", kColGoldC});
        t.push_back(WotlkTipLine{CLIENT_STR(CLICK_HERE_FOR_MORE_INFO), kColGoldC});
    } else if (mode == kModeRoleCheck) {
        t.push_back(WotlkTipLine{CLIENT_STR(LOOKING_FOR_DUNGEON), kColWhiteC});
        t.push_back(WotlkTipLine{CLIENT_STR(ROLE_CHECK_IN_PROGRESS_TOOLTIP), kColGoldC});
    } else if (mode == kModeLfgParty) {
        t.push_back(WotlkTipLine{CLIENT_STR(LOOKING_FOR_DUNGEON), kColWhiteC});
        t.push_back(WotlkTipLine{CLIENT_STR(YOU_ARE_IN_DUNGEON_GROUP), kColGoldC});
    }
    if (!t.empty()) wotlkTip(r, std::move(t), kTipLeft);
}

// --- DungeonCompletionAlertFrame ---------------------------------------------------------------------------------------

void WorldScreen::drawLfgCompletionAlert(UiRenderer& ui, float& stackTop) {
    LfdUi& L = *lfd_;
    if (L.alertT < 0.0f) return;
    pp::LfgDb& db = useDb(session_, L.dbUsed);
    const game::LfgCompletionReward& rw = L.alertReward;
    const pp::LfgDungeonRec* d = db.dungeon(rw.dungeonId);
    L.loadToastArt(d ? db.str(d->texture) : "");
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const float t = L.alertT;
    // AlertFrame animIn (alpha 0 -> 1, 0.2 s) and waitAndAnimOut (alpha 1 -> 0 over 1.5 s after the hold).
    float a = t < 0.2f ? t / 0.2f : 1.0f;
    if (t > L.alertOutAt) a = std::max(0.0f, 1.0f - (t - L.alertOutAt) / 1.5f);
    auto S = [](float u) { return u * kAS; };
    const float w = S(336), h = S(80);
    const Rect r{std::floor((static_cast<float>(pp::kScreenW) - w) * 0.5f), stackTop - h, w, h};
    stackTop = r.y - 10.0f * kUiParentScale;
    const uint32_t white = withAlpha(0xFFFFFFFFu, a);
    if (L.dungeonIcon.pixels) ui.image(L.dungeonIcon, Rect{r.x + S(13), r.y + h - S(13) - S(45), S(45), S(45)}, white);
    if (L.toast.pixels) ui.image(L.toast, r, white);
    ui.text(ft, r.x + r.w * 0.5f + S(25), r.y + S(29), CLIENT_STR(DUNGEON_COMPLETED), white,
            Align::Center);  // GameFontWhiteTiny
    const bool heroic = d && d->typeId == kTypeHeroic;
    const char* name = d ? db.str(d->name) : "";
    const float nx = r.x + r.w * 0.5f + S(heroic ? 33.0f : 25.0f);
    ui.textShadow(fs, nx, r.y + S(44), name, withAlpha(kColGoldC, a), Align::Center);
    if (heroic && L.toastHeroic.pixels) {  // $parentHeroicIcon (16 x 20, RIGHT at the name's LEFT -3,-3)
        const float nl = nx - static_cast<float>(fs.measure(name)) * 0.5f;
        const float ny = r.y + S(44) + static_cast<float>(fs.lineHeight()) * 0.5f;
        ui.image(L.toastHeroic, Rect{nl - S(3) - S(16), ny + S(3) - S(10), S(16), S(20)}, white);
    }
    // The rewards: the misc one (the coin) when money / XP, then the items; 36 apart from TOP -18 n + 41.
    const bool misc = rw.money > 0 || rw.xp > 0;
    const int used = static_cast<int>(std::min<size_t>(rw.items.size(), 7)) + (misc ? 1 : 0);
    const bool focusable = uiMode_ && !settings_.isOpen();
    for (int k = 0; k < used; ++k) {
        const float ccx = r.x + r.w * 0.5f + S(-18.0f * static_cast<float>(used) + 41.0f + 36.0f * static_cast<float>(k));
        const float ccy = r.y + S(18);
        const Rect ring{ccx - S(18), ccy - S(18), S(36), S(36)};
        const Rect tex{ccx - S(13.5f) - S(3), ccy - S(13.5f) - S(3), S(27), S(27)};
        const bool isMisc = misc && k == 0;
        if (isMisc) {
            if (L.coin.pixels) ui.image(L.coin, tex, white);
        } else {
            const auto& it = rw.items[static_cast<size_t>(k - (misc ? 1 : 0))];
            if (const pp::Texture* tx = itemIcons().byDisplay(it.displayId, spellFrame_)) ui.imageCircle(*tx, tex, white);
        }
        if (L.ring.pixels) ui.image(L.ring, ring, white);
        if (focusable && winFocus_.addStop(kIdAlertReward0 + static_cast<uint32_t>(k), ring)) {
            L.alertFocus = true;  // DungeonCompletionAlertFrameReward_OnEnter: AlertFrame_StopOutAnimation
            if (isMisc) {
                std::vector<WotlkTipLine> tl;
                tl.push_back(WotlkTipLine{CLIENT_STR(YOU_RECEIVED), kColGoldC});
                if (rw.xp > 0) {
                    tl.push_back(WotlkTipLine{pp::text::uiPlain(CLIENT_FMT(GAIN_EXPERIENCE, static_cast<unsigned>(rw.xp))),
                                              kColWhiteC});
                }
                if (rw.money > 0) {
                    WotlkTipLine ml;
                    ml.money = rw.money;
                    tl.push_back(ml);
                }
                wotlkTip(ring, std::move(tl), kTipRight);
            } else {
                const auto& it = rw.items[static_cast<size_t>(k - (misc ? 1 : 0))];
                const auto* info = session_.getItemInfo(it.itemId);
                if (info && info->valid) {  // GameTooltip:SetLFGCompletionReward
                    tipProtoItem_ = it.itemId;
                    tipProtoCount_ = std::max<uint32_t>(1u, it.count);
                    tipProtoRect_ = ring;
                }
            }
        }
    }
    // The glow (415 x 171 at CENTER 0,-5: alpha 0 -> 1 in 0.2 s, -> 0 in 0.5 s) and the shine (67 x 72 at BOTTOMLEFT:
    // in 0.2 s, 260 units to the right over 0.85 s, out from 0.35 s over 0.5 s), both ADD.
    if (t < 0.7f && L.glow.pixels) {
        const float ga = t < 0.2f ? t / 0.2f : 1.0f - (t - 0.2f) / 0.5f;
        ui.image(L.glow, Rect{r.x + r.w * 0.5f - S(207.5f), r.y + r.h * 0.5f + S(5) - S(85.5f), S(415), S(171)},
                 withAlpha(0xFFFFFFFFu, ga), Blend::Add);
    }
    if (t < 0.85f && L.shine.pixels) {
        const float sa = t < 0.2f ? t / 0.2f : t < 0.35f ? 1.0f : 1.0f - (t - 0.35f) / 0.5f;
        const float dx = S(260.0f * std::min(1.0f, t / 0.85f));
        ui.pushClip(r);
        ui.image(L.shine, Rect{r.x + dx, r.y + h - S(72), S(67), S(72)}, withAlpha(0xFFFFFFFFu, sa), Blend::Add);
        ui.popClip();
    }
}

// --- The hooks ---------------------------------------------------------------------------------------------------------

void WorldScreen::updateWotlk() {
    const uint32_t now = pp::logUptimeMs();
    const float dt = wotlkLastMs_ ? std::clamp(static_cast<float>(now - wotlkLastMs_) / 1000.0f, 0.0f, 0.25f) : 0.0f;
    wotlkLastMs_ = now;
    PROF_SCOPE("wotlk");
    lfdUpdate(dt);
    achUpdate(dt);
    calUpdate(dt);
}

void WorldScreen::drawWotlkAlerts(UiRenderer& ui) {
    if (fullMapOpen()) return;  // UIParent hidden
    float stack = static_cast<float>(pp::kScreenH) - 128.0f * kUiParentScale;  // AlertFrame BOTTOM 0,128 of UIParent
    if (ach_) drawAchievementAlerts(ui, stack);
    if (lfd_) drawLfgCompletionAlert(ui, stack);
}

bool WorldScreen::drawWotlkWindow(UiRenderer& ui, uint8_t w) {
    switch (w) {
        case kWinLfd: return drawLfdFrame(ui);
        case kWinLfdReady: return drawLfdReadyPopup(ui);
        case kWinLfdRoleCheck: return drawLfdRoleCheck(ui);
        case kWinLfgBoot:
        case kWinLfgOffer: return drawLfgVotePopup(ui, w);
        case kWinWotlkDropDown: return drawWotlkDropDown(ui);
        case kWinAchievements: return drawAchievementFrame(ui);
        case kWinCalendar: return drawCalendarFrame(ui);
        case kWinCalendarEvent: return drawCalendarEventFrame(ui);
        case kWinWotlkPopup: return drawCalendarPopup(ui);
        default: return false;
    }
}

bool WorldScreen::wotlkWindowAction(uint32_t act) {
    if (act == minimap_layout::kIdMmCalendar) {
        // GameTimeFrame_OnClick: with the invites shown Calendar_Show, else ToggleCalendar; then the minimap's handler
        // takes the invites off (the flash).
        const bool invitesShown = map_ && map_->cluster.flashInvite;  // GameTimeCalendarInvitesTexture:IsShown()
        if (!invitesShown || !windowOpen(kWinCalendar)) calToggle();
        return false;
    }
    // A click elsewhere closes our dropdown lists (CloseDropDownMenus).
    if (lfd_ && lfd_->ddOpen && !lfdFrameId(act)) lfd_->ddOpen = false;
    if (lfdId(act) || act == kIdMenuLfd) return lfdAction(act);
    if (act == kIdMenuAchievements || (act >= 0x7B00 && act < 0x7B80)) return achAction(act);
    if (act >= 0x7B80 && act <= 0x7BFF) return calAction(act);
    achCloseDropDown();
    calCloseDropDown();
    return false;
}

bool WorldScreen::wotlkAltAction(uint32_t alt) {
    if (alt == kIdLfgEye) {  // MiniMapLFGFrame_OnClick (RightButton): MiniMapLFGFrameDropDown
        LfdUi& L = lfdUi();
        if (windowOpen(kWinWotlkDropDown)) {
            closeWindow(kWinWotlkDropDown);
            return true;
        }
        uint8_t sub = 0;
        const uint8_t mode = lfgMode(&sub);
        L.menu.clear();
        L.menuDisabled.clear();
        if (lfgPartyLfg()) {  // MiniMapLFGFrameDropDown_Update
            pp::LfgDb& db = useDb(session_, L.dbUsed);
            if (inLfgDungeon(db, session_.worldEntry().mapId)) {
                L.menu.emplace_back(CLIENT_STR(TELEPORT_OUT_OF_DUNGEON), 1);
                L.menuDisabled.push_back(false);
            } else if (session_.isInGroup()) {
                L.menu.emplace_back(CLIENT_STR(TELEPORT_TO_DUNGEON), 2);
                L.menuDisabled.push_back(false);
            }
        }
        if (mode == kModeProposal && sub == kSubUnaccepted) {
            L.menu.emplace_back(CLIENT_STR(ENTER_DUNGEON), 3);
            L.menuDisabled.push_back(false);
            L.menu.emplace_back(CLIENT_STR(LEAVE_QUEUE), 4);
            L.menuDisabled.push_back(false);
        } else if (mode == kModeQueued) {
            L.menu.emplace_back(CLIENT_STR(LEAVE_QUEUE), 5);
            L.menuDisabled.push_back(sub == kSubUnempowered);
        }
        if (L.menu.empty()) return true;
        L.menuAbove = mode == kModeQueued;  // "Weird hack so that the popup isn't under the queued status window"
        L.menuAwayMs = 0;
        playSoundByName("igMainMenuOpen");
        openWindow(kWinWotlkDropDown);
        winFocus_.setFocus(kIdEyeMenu0);
        return true;
    }
    if (lfdId(alt)) return true;
    if (alt >= 0x7B00 && alt < 0x7B80) return achAltAction(alt);
    if (alt >= 0x7B80 && alt <= 0x7BFF) return calAltAction(alt);
    return false;
}

bool WorldScreen::wotlkCloseDropDown() {
    if (lfd_ && lfd_->ddOpen && windowOpen(kWinLfd)) {
        lfd_->ddOpen = false;
        return true;
    }
    if (achCloseDropDown()) return true;
    return calCloseDropDown();
}

bool WorldScreen::wotlkKeepsPopupArt() const {  // the dialog art users: the popups and the calendar's event frame
    return windowOpen(kWinLfdReady) || windowOpen(kWinLfdRoleCheck) || windowOpen(kWinLfgBoot) ||
           windowOpen(kWinLfgOffer) || windowOpen(kWinWotlkPopup) || windowOpen(kWinCalendarEvent);
}

// GameMenuFrame (PSP: the micro buttons as menu buttons): ACHIEVEMENT_BUTTON (CanShowAchievementUI) and
// DUNGEONS_BUTTON (SHOW_LFD_LEVEL).
void WorldScreen::drawWotlkMenuButtons(UiRenderer& ui, const Rect& achievements, const Rect& lfd, bool focusable) {
    drawGameMenuButton(ui, kIdMenuAchievements, achievements, CLIENT_STR(ACHIEVEMENT_BUTTON), focusable, achAvailable());
    const bool lfdOk = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)) >= kShowLfdLevel;
    drawGameMenuButton(ui, kIdMenuLfd, lfd, CLIENT_STR(DUNGEONS_BUTTON), focusable, lfdOk);
}

}  // namespace wowee::ui::psp
