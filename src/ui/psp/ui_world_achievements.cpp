// AchievementFrame and AchievementAlertFrame of the original ruRU 3.3.5a client
// (the achievement add-on's .xml / .lua, AlertFrames.xml / .lua) over the session's achievements
// (psp_world_session_achievements.cpp: SMSG_ALL_ACHIEVEMENT_DATA, SMSG_CRITERIA_UPDATE, SMSG_ACHIEVEMENT_EARNED) and
// data/wotlk.pak ("ach" index, "ac_<category>" names / descriptions / rewards / criteria, the frame's art).
//
// AchievementFrame (768 x 500 + the header above and the tabs below) at 0.6 x 0.455 of its units, filling the screen:
//   - the header: ACHIEVEMENT_TITLE and GetTotalAchievementPoints with the tiny shield; the close
//     button; the tabs ACHIEVEMENTS / STATISTICS (AchievementFrameBaseTab_OnClick);
//   - AchievementFrameCategories (AchievementFrameCategories_GetCategoryList / _Update / _SelectButton): the summary and the
//     top categories (GetCategoryList / GetStatisticsCategoryList), a top one opening its children; the tooltip: the
//     name and completed / total (AchievementFrame_GetCategoryTotalNumAchievements), FEAT_OF_STRENGTH_DESCRIPTION;
//   - AchievementFrameAchievements: the category's achievements as the client lists them (completed first, then the
//     incomplete; one step of a chain at a time; the other faction's and the hidden ones out; Feats of Strength only
//     when earned), filtered by AchievementFrameFilterDropDown (ACHIEVEMENTFRAME_FILTER_*); AchievementTemplate:
//     the parchment (desaturated when incomplete), the title, the icon in its frame, the shield with the points (a
//     chain's total), the date (SHORTDATE), the description, the reward, + / -; Cross selects (expands) a button:
//     AchievementButton_DisplayObjectives - the criteria (- name / the check, green when done), the progress bars
//     (quantity / required), the meta criteria (the achievements it needs, with their icons), a completed chain's steps
//     (MiniAchievementTemplate with the points); CRITERIA_UPDATE / ACHIEVEMENT_EARNED redraw it;
//   - AchievementFrameSummary: LATEST_UNLOCKED_ACHIEVEMENTS (GetLatestCompletedAchievements, else the default
//     incomplete ones with SUMMARY_ACHIEVEMENT_INCOMPLETE), ACHIEVEMENT_CATEGORY_PROGRESS with the total and the eight
//     categories' bars (a bar opens its category);
//   - AchievementFrameStats: the category's statistics and its children's under their headers (GetStatistic);
// AchievementAlertFrame1..2 (ACHIEVEMENT_UNLOCKED, the name, the icon, the shield; 0.2 s in, 4.05 s, 1.5 s out, the glow
// and the shine; Cross opens the frame on it - AchievementAlertFrame_OnClick).
// PSP differences (PSP: below): row heights for the 10 px font (categories 26 units, statistics 26, criteria 18, the
// collapsed button 30 + two description lines); the lists scroll by rows (HybridScrollFrame by pixels); no tracking
// (the WatchFrame's achievements), no chat links, no comparison tab (InspectAchievements comes from the unit menu,
// not ported); a statistic's value without money formatting; a desaturated texture is drawn tinted grey.
#include "ui/psp/ui_world_screen.hpp"

#include "game/packed_time.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "platform/psp/psp_wotlk_db.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <array>
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

// Widget ids: 0x7B00-0x7B7F (the calendar has 0x7B80-0x7BFF).
enum : uint32_t {
    kIdAchClose = 0x7B00,
    kIdAchTab1 = 0x7B01,
    kIdAchTab2 = 0x7B02,
    kIdAchFilter = 0x7B03,
    kIdAchCatUp = 0x7B04,
    kIdAchCatDown = 0x7B05,
    kIdAchListUp = 0x7B06,
    kIdAchListDown = 0x7B07,
    kIdAchCat0 = 0x7B10,   // + 0..31 the category rows
    kIdAchRow0 = 0x7B30,   // + 0..15 the achievement buttons / statistic rows
    kIdAchSum0 = 0x7B40,   // + 0..3 the summary's achievements
    kIdAchSumCat0 = 0x7B44,  // + 0..7 the summary's category bars
    kIdAchSumTotal = 0x7B4C,
    kIdAchCrit0 = 0x7B50,  // + 0..15 the meta criteria / the chain's steps (tooltips)
    kIdAchDd0 = 0x7B60,    // + 0..2 the filter's items
    kIdAchAlert0 = 0x7B70,  // + 0..1 the alert frames
};
bool achId(uint32_t id) { return id >= 0x7B00 && id < 0x7B80; }

// AchievementFrame: 768 x 500 units at 0.6 x 0.455; the header reaches 67 units above, the tabs 30 below.
constexpr float kSX = 0.6f, kSY = 0.455f, kAx = 10.0f, kAy = 30.5f;
float AX(float x) { return kAx + x * kSX; }
float AY(float y) { return kAy + y * kSY; }
Rect AR(float x, float y, float w, float h) { return Rect{AX(x), AY(y), w * kSX, h * kSY}; }

constexpr int32_t kSummary = -1;            // the "summary" category
constexpr uint32_t kFeatOfStrength = 81;    // FEAT_OF_STRENGTH_ID
constexpr uint32_t kStatRoot = 1;           // Achievement_Category "Statistics" (its children are the tab's tops)
constexpr uint16_t kFlagStatistic = 0x01, kFlagHidden = 0x02, kFlagProgressBar = 0x80;  // ACHIEVEMENT_FLAGS_*
constexpr uint32_t kCriteriaTypeAchievement = 8;  // CRITERIA_TYPE_ACHIEVEMENT
constexpr uint16_t kCriteriaProgressBar = 0x01;   // ACHIEVEMENT_CRITERIA_PROGRESS_BAR
constexpr int kMaxSummary = 4;                    // ACHIEVEMENTUI_MAX_SUMMARY_ACHIEVEMENTS
constexpr uint32_t kDefaultSummary[5] = {6, 503, 116, 545, 1017};  // ACHIEVEMENTUI_DEFAULTSUMMARYACHIEVEMENTS
constexpr uint32_t kSummaryCategories[8] = {92, 96, 97, 95, 168, 169, 201, 155};
constexpr int kMaxAlerts = 2;  // MAX_ACHIEVEMENT_ALERTS

// The categories list (x 21..196 / 218, y 24..475) and the right panel (the container x 222..722, y 22..475).
constexpr float kCatX = 21.0f, kCatTop = 24.0f, kCatBottom = 475.0f, kCatRowU = 26.0f;  // PSP: 26 (24)
constexpr int kCatRows = static_cast<int>((kCatBottom - kCatTop) / kCatRowU);
constexpr float kPanelX = 222.0f, kPanelTop = 22.0f, kPanelBottom = 475.0f, kBtnW = 496.0f;
constexpr float kStatRowU = 26.0f;   // PSP (StatTemplate 24)
constexpr float kCritRowU = 18.0f;   // PSP (ACHIEVEMENTBUTTON_CRITERIAROWHEIGHT 15)
constexpr float kMetaRowU = 32.0f;   // PSP (MetaCriteriaTemplate 30, ACHIEVEMENTBUTTON_METAROWHEIGHT 14 a row)
constexpr float kMiniW = 42.0f, kMiniH = 50.0f;  // ACHIEVEMENTUI_PROGRESSIVEWIDTH / _HEIGHT
constexpr float kContentW = 330.0f;  // ACHIEVEMENTUI_MAXCONTENTWIDTH

// 0xAABBGGRR
constexpr uint32_t kGold = 0xFF00D1FFu, kWhite = 0xFFFFFFFFu, kBlack = 0xFF000000u, kGrey65 = 0xFFA6A6A6u,
                   kGrey60 = 0xFF999999u, kGreen = 0xFF00FF00u, kDisabled = 0xFF808080u;
constexpr uint32_t kRedBorder = 0xFF0D26B3u;   // ACHIEVEMENTUI_REDBORDER (0.7, 0.15, 0.05)
constexpr uint32_t kGoldBorder = 0xFF20ACFFu;  // ACHIEVEMENTUI_GOLDBORDER (1, 0.675, 0.125)
constexpr uint32_t kGreyBorder = 0xFF808080u;
constexpr uint32_t kStatValue = 0xFF99F7FFu;   // (1, 0.97, 0.6)

uint32_t withAlpha(uint32_t col, float a) {
    const uint32_t al = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * static_cast<float>(col >> 24) + 0.5f);
    return (al << 24) | (col & 0x00FFFFFFu);
}

// ACHIEVEMENTFRAME_FILTER_ALL / _COMPLETED / _INCOMPLETE.
const char* filterText(int k) {
    static constexpr pp::CS t[3] = {pp::CS::ACHIEVEMENTFRAME_FILTER_ALL, pp::CS::ACHIEVEMENTFRAME_FILTER_COMPLETED,
                                    pp::CS::ACHIEVEMENTFRAME_FILTER_INCOMPLETE};
    return pp::cs(t[std::clamp(k, 0, 2)]);
}

// SHORTDATE "%2$d/%1$02d/%3$02d" with (day, month, year).
std::string shortDate(uint32_t packed) {
    const game::WowDate d = game::unpackWowPackedTime(packed);
    char b[24];
    std::snprintf(b, sizeof(b), "%d/%02d/%02d", d.month, d.day, d.yearSince2000);
    return b;
}
// The earned date as a sortable key (packed time: minute, hour, weekday, day, month, year).
uint32_t dateKey(uint32_t packed) {
    const game::WowDate d = game::unpackWowPackedTime(packed);
    return (static_cast<uint32_t>(d.yearSince2000) << 24) | (static_cast<uint32_t>(d.month) << 20) |
           (static_cast<uint32_t>(d.day) << 14) | (static_cast<uint32_t>(d.hour) << 8) | static_cast<uint32_t>(d.minute);
}

}  // namespace

// --- State ------------------------------------------------------------------------------------------------------------

struct WorldScreen::AchUi {
    pp::AchIndex index;  // while the frame or an alert needs it
    struct CatSlot {
        pp::AchCategory cat;
        uint32_t used = 0;
    };
    std::array<CatSlot, 3> cats;  // the category blobs in use (LRU)
    uint32_t clock = 0;

    // Art (data/wotlk.pak): the frame's while it is open, the alert's while one shows.
    pp::Texture frame, catBg, catHl, parch, parchD, titleS, titleD, iconFrame, shields, shieldsNp, plusMinus, check,
        pbarBorder, bar, sbar, sumBar, catBar, recentHdr, statHdr, statOdd, statEven, tabArt, tabSel, tinyShield,
        miniBorder, miniShield, ddBtn, ddBtnDn;
    int frameKind = 0;  // 1 achievements, 2 statistics
    bool artTried = false;
    uint32_t artBytes = 0;
    pp::Texture alertBg, alertGlow, alertShine, alertFrame, alertShields, alertShieldsNp;
    bool alertArtTried = false;

    // AchievementFrame.
    bool shown = false, wasShown = false;
    int tab = 1;                                 // AchievementFrame.selectedTab
    int32_t selected[2] = {kSummary, kSummary};  // achievementFunctions.selectedCategory (per tab)
    struct CatEntry {
        int32_t id = 0;
        int32_t parent = 0;  // 0 none; -1 "true" (a parent); > 0 the parent's id
        bool hidden = false, collapsed = false;
    };
    std::vector<CatEntry> categories;            // ACHIEVEMENTUI_CATEGORIES
    int catTab = 0;
    int catScroll = 0;
    int filter = 0;                              // AchievementFrameFilters index (0 all, 1 complete, 2 incomplete)
    bool ddOpen = false;
    uint32_t selection = 0;                      // AchievementFrameAchievements.selection
    int listScroll = 0;                          // first shown button / row
    // The category's list (GetCategoryNumAchievements / GetAchievementInfo(category, i)): completed first.
    int32_t listCat = 0;
    uint32_t listVersion = ~0u;
    std::vector<uint16_t> listIds;
    int listCompleted = 0;
    std::vector<uint32_t> shownIds;              // the buttons / rows drawn this frame
    std::vector<uint32_t> critIds;               // the meta criteria / chain steps drawn (tooltips)
    bool focusPending = false;

    // AchievementAlertFrame1..2.
    struct Alert {
        uint32_t id = 0;
        float t = -1.0f, outAt = 4.05f;
        bool focus = false;
    };
    std::array<Alert, kMaxAlerts> alerts{};

    uint32_t logs = 0;

    std::array<pp::Texture*, 28> textures() {
        return {&frame,   &catBg,     &catHl,     &parch,      &parchD,   &titleS,  &titleD,  &iconFrame,
                &shields, &shieldsNp, &plusMinus, &check,      &pbarBorder, &bar,   &sbar,    &sumBar,
                &catBar,  &recentHdr, &statHdr,   &statOdd,    &statEven, &tabArt,     &tabSel,  &tinyShield,
                &miniBorder, &miniShield, &ddBtn, &ddBtnDn};
    }
    void releaseArt() {
        for (pp::Texture* t : textures())
            if (t->pixels) pp::textureRelease(*t);
        if (artTried) PSP_LOGI(pp::tag::Gfx, "ach_art_free kb=%u", static_cast<unsigned>((artBytes + 1023u) / 1024u));
        artTried = false;
        artBytes = 0;
        frameKind = 0;
    }
    void releaseAlertArt() {
        for (pp::Texture* t : {&alertBg, &alertGlow, &alertShine, &alertFrame, &alertShields, &alertShieldsNp})
            if (t->pixels) pp::textureRelease(*t);
        alertArtTried = false;
    }
    void loadArt(int kind) {
        if (artTried && frameKind == kind) return;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) {
            artTried = true;
            frameKind = kind;
            return;
        }
        if (!artTried) {
            const std::pair<const char*, pp::Texture*> list[] = {
                {"ach_cat_bg", &catBg},       {"ach_cat_hl", &catHl},       {"ach_parch", &parch},
                {"ach_parch_d", &parchD},     {"ach_title_s", &titleS},     {"ach_title_d", &titleD},
                {"ach_iconframe", &iconFrame}, {"ach_shields", &shields},   {"ach_shields_np", &shieldsNp},
                {"ach_plusminus", &plusMinus}, {"ach_check", &check},       {"ach_pbar_border", &pbarBorder},
                {"ach_bar", &bar},            {"ach_sbar", &sbar},          {"ach_sumbar", &sumBar},
                {"ach_catbar", &catBar},      {"ach_recent_hdr", &recentHdr}, {"ach_stat_hdr", &statHdr},
                {"ach_stat_odd", &statOdd},   {"ach_stat_even", &statEven}, {"ach_tab", &tabArt},
                {"ach_tab_sel", &tabSel},     {"ach_tinyshield", &tinyShield}, {"ach_mini_border", &miniBorder},
                {"ach_mini_shield", &miniShield}, {"lfg_dd_btn", &ddBtn},  {"lfg_dd_btn_dn", &ddBtnDn}};
            for (const auto& [name, tex] : list) pp::wotlkPakTexture(pak, name, *tex, &artBytes);
        }
        // The frame of the tab: UI-Achievement-AchievementBackground + -AchievementWatermark, or the statistics'.
        pp::wotlkPakTexture(pak, kind == 2 ? "ach_frame_s" : "ach_frame_a", frame, &artBytes);
        pak.close();
        artTried = true;
        frameKind = kind;
        PSP_LOGI(pp::tag::Gfx, "ach_art kind=%d frame=%d kb=%u", kind, frame.pixels ? 1 : 0,
                 static_cast<unsigned>((artBytes + 1023u) / 1024u));
    }
    void loadAlertArt() {
        if (alertArtTried) return;
        alertArtTried = true;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        uint32_t bytes = 0;
        pp::wotlkPakTexture(pak, "ach_alert_bg", alertBg, &bytes);
        pp::wotlkPakTexture(pak, "alert_glow", alertGlow, &bytes);
        pp::wotlkPakTexture(pak, "alert_shine", alertShine, &bytes);
        pp::wotlkPakTexture(pak, "ach_iconframe", alertFrame, &bytes);
        pp::wotlkPakTexture(pak, "ach_shields", alertShields, &bytes);
        pp::wotlkPakTexture(pak, "ach_shields_np", alertShieldsNp, &bytes);
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "ach_alert_art kb=%u", static_cast<unsigned>((bytes + 1023u) / 1024u));
    }
    // One category's blob (names, descriptions, rewards, criteria), loaded on demand, the least used one dropped.
    const pp::AchCategory* category(uint32_t id) {
        ++clock;
        for (CatSlot& s : cats)
            if (s.cat.loaded() && s.cat.id() == id) {
                s.used = clock;
                return &s.cat;
            }
        CatSlot* victim = &cats[0];
        for (CatSlot& s : cats)
            if (!s.cat.loaded() || s.used < victim->used) victim = &s;
        if (!victim->cat.load(id)) return nullptr;
        victim->used = clock;
        if (++logs <= 60) PSP_LOGI(pp::tag::Gfx, "ach_cat_load id=%u kb=%u", static_cast<unsigned>(id),
                                   static_cast<unsigned>((victim->cat.bytes() + 1023u) / 1024u));
        return &victim->cat;
    }
    void releaseData() {
        index.release();
        for (CatSlot& s : cats) s.cat.release();
    }
    ~AchUi() {
        releaseArt();
        releaseAlertArt();
    }
};

void WorldScreen::destroyAchUi(AchUi* p) { delete p; }

WorldScreen::AchUi& WorldScreen::achUi() {
    if (!ach_) ach_ = std::unique_ptr<AchUi, void (*)(AchUi*)>(new AchUi(), &WorldScreen::destroyAchUi);
    return *ach_;
}

bool WorldScreen::achAvailable() const { return session_.achievementsKnown(); }  // CanShowAchievementUI

namespace {

// The player's faction for Achievement.dbc Faction (0 Horde, 1 Alliance).
int8_t playerFaction(const pp::WorldSession& s) {
    if (const game::Character* me = s.activeCharacter()) {
        switch (me->race) {
            case game::Race::HUMAN:
            case game::Race::DWARF:
            case game::Race::NIGHT_ELF:
            case game::Race::GNOME:
            case game::Race::DRAENEI: return 1;
            default: return 0;
        }
    }
    return -1;
}

}  // namespace

// --- Data (the client's Get* answers over the index) -----------------------------------------------------------------

namespace {

struct AchView {
    const pp::AchIdxRec* idx = nullptr;
    const pp::AchRec* rec = nullptr;
    const pp::AchCategory* cat = nullptr;
    bool earned = false;
    uint32_t date = 0;
};

}  // namespace

// --- Per frame ----------------------------------------------------------------------------------------------------------

void WorldScreen::achToggle() {  // AchievementFrame_ToggleAchievementFrame (the achievements tab)
    if (windowOpen(kWinAchievements)) {
        closeWindow(kWinAchievements);
        return;
    }
    if (!achAvailable()) return;
    AchUi& A = achUi();
    openWindow(kWinAchievements);
    A.tab = 1;
    A.categories.clear();  // AchievementFrameTab_OnClick(1): AchievementFrameCategories_GetCategoryList
    A.focusPending = true;
}

void WorldScreen::achUpdate(float dtSec) {
    uint32_t earnedId = 0;
    while (session_.takeAchievementEarned(earnedId)) {  // ACHIEVEMENT_EARNED -> AchievementAlertFrame_ShowAlert
        AchUi& A = achUi();
        AchUi::Alert* slot = nullptr;
        for (auto& a : A.alerts)
            if (a.t < 0.0f) {
                slot = &a;
                break;
            }
        if (!slot) continue;  // "We ran out of frames! Bail!"
        slot->id = earnedId;
        slot->t = 0.0f;
        slot->outAt = 4.05f;
        slot->focus = false;
        A.listVersion = ~0u;  // the frame's lists redraw (AchievementFrameAchievements_ForceUpdate)
        PSP_LOGI(pp::tag::Gfx, "ach_alert id=%u", static_cast<unsigned>(earnedId));
    }
    if (!ach_) return;
    AchUi& A = *ach_;
    bool alerting = false;
    for (auto& a : A.alerts) {  // AlertFrame_AnimateIn / waitAndAnimOut (the focus holds it)
        if (a.t < 0.0f) continue;
        if (a.focus) a.outAt = std::max(a.outAt, a.t + 1.0f);
        a.focus = false;
        a.t += dtSec;
        if (a.t > a.outAt + 1.5f) a.t = -1.0f;
        alerting = alerting || a.t >= 0.0f;
    }
    const bool open = windowOpen(kWinAchievements);
    if (open && !A.shown) {  // AchievementFrame_OnShow
        A.shown = true;
        playSoundByName("AchievementMenuOpen");
        if (!A.wasShown) {
            A.wasShown = true;
            A.selected[0] = kSummary;  // AchievementCategoryButton_OnClick(button 1): the summary
        }
        A.listVersion = ~0u;
        PSP_LOGI(pp::tag::Gfx, "ach_open earned=%u", static_cast<unsigned>(session_.earnedAchievements().size()));
    } else if (!open && A.shown) {  // AchievementFrame_OnHide
        A.shown = false;
        A.ddOpen = false;
        playSoundByName("AchievementMenuClose");
    }
    if (open && A.focusPending) {
        A.focusPending = false;
        winFocus_.setFocus(kIdAchCat0);
    }
    if (!open && A.artTried) A.releaseArt();
    if (!alerting && A.alertArtTried) A.releaseAlertArt();
    if (!open && !alerting && (A.index.loaded() || A.cats[0].cat.loaded() || A.cats[1].cat.loaded() ||
                               A.cats[2].cat.loaded())) {
        A.releaseData();
        PSP_LOGI(pp::tag::Gfx, "ach_data_free");
    }
}

// --- Actions -------------------------------------------------------------------------------------------------------------

bool WorldScreen::achCloseDropDown() {
    if (!ach_ || !ach_->ddOpen || !windowOpen(kWinAchievements)) return false;
    ach_->ddOpen = false;
    return true;
}

bool WorldScreen::achAltAction(uint32_t alt) {
    if (!achId(alt)) return false;
    return achAction(alt);  // the right click: as the left one (no tracking / links on the PSP)
}

bool WorldScreen::achAction(uint32_t act) {
    if (act == kIdMenuAchievements) {  // the game menu's ACHIEVEMENTS (ToggleAchievementFrame)
        closeWindow(kWinMenu);
        achToggle();
        return true;
    }
    if (!achId(act)) return false;
    AchUi& A = achUi();
    // AchievementAlertFrame_OnClick: the frame, the achievement selected.
    if (act >= kIdAchAlert0 && act < kIdAchAlert0 + kMaxAlerts) {
        const uint32_t id = A.alerts[act - kIdAchAlert0].id;
        if (!id) return true;
        if (!windowOpen(kWinAchievements)) achToggle();
        if (!windowOpen(kWinAchievements)) return true;
        if (!A.index.loaded()) A.index.load();
        const pp::AchIdxRec* r = A.index.find(id);
        if (!r) return true;
        A.tab = 1;
        if (A.filter == 2) A.filter = 0;  // completed and the incomplete filter: all
        A.selected[0] = r->category;
        A.categories.clear();
        A.selection = id;
        A.listVersion = ~0u;
        A.listScroll = -1;  // AchievementFrame_SelectAchievement: scrolled to it when drawn
        return true;
    }
    if (!windowOpen(kWinAchievements)) return true;
    // The filter's list (AchievementFrameFilterDropDown).
    if (A.ddOpen) {
        A.ddOpen = false;
        if (act >= kIdAchDd0 && act < kIdAchDd0 + 3) {  // AchievementFrame_SetFilter
            const int f = static_cast<int>(act - kIdAchDd0);
            if (f != A.filter) {
                A.filter = f;
                A.listScroll = 0;
                A.listVersion = ~0u;
            }
            playSoundByName("UChatScrollButton");
        }
        return true;
    }
    switch (act) {
        case kIdAchClose: closeWindow(kWinAchievements); return true;
        case kIdAchTab1:
        case kIdAchTab2: {  // AchievementFrameBaseTab_OnClick
            const int t = act == kIdAchTab1 ? 1 : 2;
            if (t != A.tab) {
                A.tab = t;
                A.categories.clear();
                A.listScroll = 0;
                A.catScroll = 0;
                A.listVersion = ~0u;
                playSoundByName("igCharacterInfoTab");
            }
            return true;
        }
        case kIdAchFilter:
            A.ddOpen = true;
            playSoundByName("igMainMenuOptionCheckBoxOn");
            return true;
        case kIdAchCatUp: A.catScroll = std::max(0, A.catScroll - 1); return true;
        case kIdAchCatDown: ++A.catScroll; return true;
        case kIdAchListUp: A.listScroll = std::max(0, A.listScroll - 1); return true;
        case kIdAchListDown: ++A.listScroll; return true;
        default: break;
    }
    // A category row (AchievementCategoryButton_OnClick -> AchievementFrameCategories_SelectButton).
    if (act >= kIdAchCat0 && act < kIdAchCat0 + 32) {
        const size_t v = act - kIdAchCat0;
        // The displayed list: rebuilt as drawn (AchievementFrameCategories_Update) - find the row's element.
        std::vector<AchUi::CatEntry*> shown;
        const int32_t sel = A.selected[A.tab - 1];
        for (auto& c : A.categories)
            if (!c.hidden) shown.push_back(&c);
        const size_t i = static_cast<size_t>(A.catScroll) + v;
        if (i >= shown.size()) return true;
        AchUi::CatEntry& e = *shown[i];
        if (e.parent <= 0) {  // a top category: opens / closes its children
            if (sel == e.id && !e.collapsed) {
                e.collapsed = true;
                for (auto& c : A.categories)
                    if (c.parent == e.id) c.hidden = true;
            } else {
                for (auto& c : A.categories) {
                    if (c.parent == e.id) c.hidden = false;
                    else if (c.parent == -1) c.collapsed = true;
                    else if (c.parent > 0) c.hidden = true;
                }
                e.collapsed = false;
            }
        }
        if (sel != e.id) {
            A.selected[A.tab - 1] = e.id;
            A.selection = 0;  // clearFunc: AchievementFrameAchievements_ClearSelection
            A.listScroll = 0;
            A.listVersion = ~0u;
        }
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    // An achievement button (AchievementButton_OnClick): select / unselect (the objectives); a statistic header row
    // (AchievementStatButton_OnClick): its category.
    if (act >= kIdAchRow0 && act < kIdAchRow0 + 16) {
        const size_t v = act - kIdAchRow0;
        if (v >= A.shownIds.size()) return true;
        const uint32_t id = A.shownIds[v];
        if (A.tab == 2) {
            if (id & 0x80000000u) {  // a header: select that category
                A.selected[1] = static_cast<int32_t>(id & 0x7FFFFFFFu);
                A.listScroll = 0;
                A.listVersion = ~0u;
            }
            return true;
        }
        A.selection = A.selection == id ? 0 : id;
        playSoundByName("igMainMenuOptionCheckBoxOn");
        return true;
    }
    // The summary: an achievement (AchievementFrameSummaryAchievement_OnClick: the furthest completed step's next) or
    // a category bar (AchievementFrameSummaryCategoryButton_OnClick).
    if (act >= kIdAchSum0 && act < kIdAchSum0 + kMaxSummary) {
        const size_t v = act - kIdAchSum0;
        if (v >= A.shownIds.size() || !A.index.loaded()) return true;
        uint32_t id = A.shownIds[v];
        const auto& earned = session_.earnedAchievements();
        const pp::AchIdxRec* r = A.index.find(id);
        if (r && r->next && earned.count(r->next)) {
            uint32_t nextId = r->next;
            for (int guard = 0; guard < 32; ++guard) {
                const pp::AchIdxRec* n = A.index.find(nextId);
                if (!n || !n->next || !earned.count(n->next)) break;
                nextId = n->next;
            }
            id = nextId;
        }
        if (const pp::AchIdxRec* t = A.index.find(id)) {
            A.selected[0] = t->category;
            A.categories.clear();
            A.selection = id;
            A.listVersion = ~0u;
            A.listScroll = -1;
        }
        return true;
    }
    if (act >= kIdAchSumCat0 && act < kIdAchSumCat0 + 8) {
        A.selected[0] = static_cast<int32_t>(kSummaryCategories[act - kIdAchSumCat0]);
        A.categories.clear();
        A.selection = 0;
        A.listScroll = 0;
        A.listVersion = ~0u;
        return true;
    }
    return true;  // the criteria: tooltips only
}

// --- Drawing ---------------------------------------------------------------------------------------------------------------

bool WorldScreen::drawAchievementFrame(UiRenderer& ui) {
    AchUi& A = achUi();
    if (!A.index.loaded() && !A.index.tried()) A.index.load();
    A.loadArt(A.tab);
    if (!A.frame.pixels || !A.index.loaded()) {
        closeWindow(kWinAchievements);
        uiError(OWN_STR(NO_WOTLK_ART));
        return true;
    }
    PROF_SCOPE("achievements");
    loadNpcArt(false);
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight()), th = static_cast<float>(ft.lineHeight());
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const bool live = focusable && !A.ddOpen;
    const uint32_t focused = winFocus_.focused();
    auto screenRect = [&](const Rect& r) { return Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h}; };
    const auto& earned = session_.earnedAchievements();
    const auto& progress = session_.criteriaProgress();
    const int8_t faction = playerFaction(session_);
    const pp::AchIndex& ix = A.index;
    A.shownIds.clear();
    A.critIds.clear();

    // Eligible: the faction's (or both), not hidden unless earned, Feats of Strength only when earned.
    auto eligible = [&](const pp::AchIdxRec& r) {
        if (r.faction >= 0 && faction >= 0 && r.faction != faction) return false;
        const bool done = earned.count(r.id) != 0;
        if ((r.flags & kFlagHidden) && !done) return false;
        if (r.category == kFeatOfStrength && !done) return false;
        return true;
    };
    // GetCategoryNumAchievements(id, showAll): every eligible one (showAll) or the list as shown (a chain's step).
    auto categoryCounts = [&](uint32_t cat, bool showAll, int& total, int& done) {
        total = done = 0;
        for (uint32_t i = 0; i < ix.count(); ++i) {
            const pp::AchIdxRec& r = ix.at(i);
            if (r.category != cat || !eligible(r)) continue;
            const bool e = earned.count(r.id) != 0;
            if (!showAll) {
                if (r.prev && !earned.count(r.prev)) continue;
                if (e && r.next && earned.count(r.next)) continue;
            }
            ++total;
            if (e) ++done;
        }
    };
    auto totalCounts = [&](uint32_t cat, int& total, int& done) {  // AchievementFrame_GetCategoryTotalNumAchievements
        categoryCounts(cat, true, total, done);
        for (uint32_t i = 0; i < ix.categoryCount(); ++i) {
            const pp::AchCatRec& c = ix.categoryAt(i);
            if (c.parent != static_cast<int16_t>(cat)) continue;
            int t = 0, d = 0;
            categoryCounts(c.id, true, t, d);
            total += t;
            done += d;
        }
    };
    auto view = [&](uint32_t id) {
        AchView v;
        v.idx = ix.find(id);
        if (!v.idx) return v;
        v.cat = A.category(v.idx->category);
        v.rec = v.cat ? v.cat->find(id) : nullptr;
        auto it = earned.find(id);
        v.earned = it != earned.end();
        v.date = v.earned ? it->second : 0;
        return v;
    };

    // The frame (the composite: the borders, the backgrounds, the header) and the header's points.
    ui.image(A.frame, AR(0, -67, 768, 567));
    {
        uint32_t points = 0;  // GetTotalAchievementPoints
        for (const auto& [id, when] : earned) {
            (void)when;
            if (const pp::AchIdxRec* r = ix.find(id)) points += r->points;
        }
        ui.textShadow(fs, AX(409), AY(-21) - lh * 0.5f, CLIENT_STR(ACHIEVEMENT_TITLE), kGold, Align::Center);
        char b[16];
        std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(points));
        const float pw = static_cast<float>(fs.measure(b));
        ui.textShadow(fs, AX(409) - pw * 0.5f, AY(3) - lh * 0.5f, b, kWhite);
        if (A.tinyShield.pixels) ui.image(A.tinyShield, Rect{AX(409) + pw * 0.5f + 2.0f, AY(3) - 5.0f, 10, 10});
    }
    // The close button (TOPRIGHT 3,4).
    {
        const Rect cr{AX(755) - 9, AY(12) - 9, 18, 18};
        const bool f = live && winFocus_.addStop(kIdAchClose, cr);
        const pp::Texture& tx = f && winFocus_.isPressed(kIdAchClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                  : npcArt_.close;
        if (tx.pixels) ui.image(tx, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // The tabs (AchievementFrameTabButtonTemplate: the selected one in its "disabled" art, 5 units higher).
    for (int t = 1; t <= 2; ++t) {
        const float x = t == 1 ? 11.0f : 121.0f;
        const bool sel = A.tab == t;
        const Rect r = sel ? AR(x, 493, 127, 59) : AR(x, 498, 127, 49);
        const uint32_t id = t == 1 ? kIdAchTab1 : kIdAchTab2;
        const bool f = live && !sel && winFocus_.addStop(id, AR(x, 498, 115, 32));
        const pp::Texture& tx = sel ? A.tabSel : A.tabArt;
        if (tx.pixels) ui.image(tx, r);
        ui.textShadow(fs, AX(x + 57.5f), AY(sel ? 512 : 514) - lh * 0.5f, t == 1 ? CLIENT_STR(ACHIEVEMENTS) : CLIENT_STR(STATISTICS),
                      sel ? kWhite : f ? kWhite : kGold, Align::Center);
    }

    // --- AchievementFrameCategories ---------------------------------------------------------------------------------
    if (A.categories.empty() || A.catTab != A.tab) {
        // AchievementFrameCategories_GetCategoryList: the summary, the top ones, each parent followed by its children.
        A.catTab = A.tab;
        A.categories.clear();
        A.categories.push_back(AchUi::CatEntry{kSummary, 0, false, false});
        const bool stats = A.tab == 2;
        auto inTab = [&](const pp::AchCatRec& c) { return ((c.flags & 1) != 0) == stats && c.id != kStatRoot; };
        auto topOf = [&](const pp::AchCatRec& c) { return stats ? c.parent == static_cast<int16_t>(kStatRoot) : c.parent < 0; };
        for (uint32_t i = 0; i < ix.categoryCount(); ++i) {
            const pp::AchCatRec& c = ix.categoryAt(i);
            if (inTab(c) && topOf(c)) A.categories.push_back(AchUi::CatEntry{c.id, 0, false, true});
        }
        for (size_t j = 0; j < A.categories.size(); ++j) {
            const int32_t pid = A.categories[j].id;
            if (pid <= 0) continue;
            std::vector<AchUi::CatEntry> kids;
            for (uint32_t i = 0; i < ix.categoryCount(); ++i) {
                const pp::AchCatRec& c = ix.categoryAt(i);
                if (inTab(c) && !topOf(c) && c.parent == pid) kids.push_back(AchUi::CatEntry{c.id, pid, true, false});
            }
            if (kids.empty()) continue;
            A.categories[j].parent = -1;
            A.categories.insert(A.categories.begin() + static_cast<std::ptrdiff_t>(j) + 1, kids.begin(), kids.end());
            j += kids.size();
        }
        // The selected one's parent opens (AchievementFrameCategories_Update).
        const int32_t sel = A.selected[A.tab - 1];
        int32_t par = 0;
        for (auto& c : A.categories)
            if (c.id == sel && c.parent > 0) par = c.parent;
        for (auto& c : A.categories) {
            if (par && c.id == par) c.collapsed = false;
            if (par && c.parent == par) c.hidden = false;
        }
    }
    std::vector<const AchUi::CatEntry*> shownCats;
    for (const auto& c : A.categories)
        if (!c.hidden) shownCats.push_back(&c);
    const int nCats = static_cast<int>(shownCats.size());
    const bool catScrollShown = nCats > kCatRows;
    const float catW = catScrollShown ? 175.0f : 197.0f;  // ACHIEVEMENTUI_CATEGORIESWIDTH
    A.catScroll = std::clamp(A.catScroll, 0, std::max(0, nCats - kCatRows));
    if (live && focused >= kIdAchCat0 && focused < kIdAchCat0 + static_cast<uint32_t>(kCatRows)) {
        const int vi = static_cast<int>(focused - kIdAchCat0);
        if (vi == kCatRows - 1 && A.catScroll + kCatRows < nCats) {
            ++A.catScroll;
            winFocus_.setFocus(focused - 1);
        } else if (vi == 0 && A.catScroll > 0) {
            --A.catScroll;
            winFocus_.setFocus(focused + 1);
        }
    }
    ui.nineSlice(kit_.tex(kTexTipBorder), AR(kCatX, 19, catW, 461), 1.0f, kGoldBorder);
    for (int v = 0; v < kCatRows; ++v) {
        const int i = A.catScroll + v;
        if (i >= nCats) break;
        const AchUi::CatEntry& e = *shownCats[static_cast<size_t>(i)];
        const bool child = e.parent > 0;
        const float bw = catW - (child ? 25.0f : 10.0f);
        const float bx = kCatX + (catW - bw) * 0.5f, by = kCatTop + kCatRowU * static_cast<float>(v);
        const Rect br = AR(bx, by, bw, kCatRowU);
        const bool f = live && winFocus_.addStop(kIdAchCat0 + static_cast<uint32_t>(v), br);
        if (A.catBg.pixels) ui.image(A.catBg, AR(bx, by, bw, kCatRowU * 32.0f / 24.0f), child ? 0xFF999999u : 0xFFFFFFFFu);
        if ((e.id == A.selected[A.tab - 1] || f) && A.catHl.pixels)
            ui.image(A.catHl, AR(bx, by, bw - 1.0f, kCatRowU * 32.0f / 24.0f - 7.0f), 0xFFFFFFFFu, Blend::Add);
        const char* name = e.id == kSummary ? CLIENT_STR(ACHIEVEMENT_SUMMARY_CATEGORY)
                           : ix.category(static_cast<uint32_t>(e.id)) ? ix.str(ix.category(static_cast<uint32_t>(e.id))->name)
                                                                     : "";
        const Rect lr{AX(bx + 16), br.y + (br.h - lh) * 0.5f, (bw - 24.0f) * kSX, lh};
        if (static_cast<float>(fs.measure(name)) <= lr.w) ui.textShadow(fs, lr.x, lr.y, name, child ? kWhite : kGold);
        else ui.textMarquee(fs, lr, name, child ? kWhite : kGold);
        if (f) {  // AchievementFrameCategory_StatusBarTooltip / _FeatOfStrengthTooltip
            std::vector<WotlkTipLine> t;
            t.push_back(WotlkTipLine{name, kWhite});
            if (e.id == static_cast<int32_t>(kFeatOfStrength)) {
                t.push_back(WotlkTipLine{CLIENT_STR(FEAT_OF_STRENGTH_DESCRIPTION), kGold});
            } else if (A.tab == 1) {
                int total = 0, done = 0;
                if (e.id == kSummary) {
                    for (uint32_t k = 0; k < ix.count(); ++k) {
                        const pp::AchIdxRec& r = ix.at(k);
                        if ((r.flags & kFlagStatistic) || r.category == kFeatOfStrength || !eligible(r)) continue;
                        ++total;
                        if (earned.count(r.id)) ++done;
                    }
                } else {
                    totalCounts(static_cast<uint32_t>(e.id), total, done);
                }
                char b[32];
                std::snprintf(b, sizeof(b), "%d/%d", done, total);
                t.push_back(WotlkTipLine{b, kGold});
            }
            wotlkTip(screenRect(br), std::move(t), kTipRight);
        }
    }
    if (catScrollShown) {
        const bool canUp = A.catScroll > 0, canDn = A.catScroll + kCatRows < nCats;
        const Rect up{AX(kCatX + catW + 4), AY(kCatTop), 11, 11}, dn{AX(kCatX + catW + 4), AY(kCatBottom) - 11, 11, 11};
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (live && canUp && winFocus_.addStop(kIdAchCatUp, up)) ui.fill(up, 0x40FFFFFFu);
        if (live && canDn && winFocus_.addStop(kIdAchCatDown, dn)) ui.fill(dn, 0x40FFFFFFu);
    }

    const int32_t sel = A.selected[A.tab - 1];
    const float panelX = catScrollShown ? kPanelX : kPanelX;  // (the panel stays at 218 + 4: TOPRIGHT 22 / 0)
    // --- The statistics tab ------------------------------------------------------------------------------------------
    if (A.tab == 2) {
        // AchievementFrameStats_Update: the category's statistics, then each child's under its header. The summary: the
        // categories' first rows (PSP: the summary page of statistics is the category list of the root's children).
        struct StatRow {
            uint32_t id;  // achievement id, or 0x80000000 | category for a header
        };
        std::vector<StatRow> rows;
        auto addCat = [&](uint32_t cat, bool header) {
            if (header) rows.push_back(StatRow{0x80000000u | cat});
            std::vector<const pp::AchIdxRec*> list;
            for (uint32_t i = 0; i < ix.count(); ++i) {
                const pp::AchIdxRec& r = ix.at(i);
                if (r.category == cat && (r.flags & kFlagStatistic) && eligible(r)) list.push_back(&r);
            }
            std::sort(list.begin(), list.end(), [](const pp::AchIdxRec* a, const pp::AchIdxRec* b) {
                return a->order != b->order ? a->order < b->order : a->id < b->id;
            });
            for (const pp::AchIdxRec* r : list) rows.push_back(StatRow{r->id});
        };
        if (sel == kSummary) {
            for (const auto& c : A.categories)
                if (c.id > 0 && c.parent <= 0) addCat(static_cast<uint32_t>(c.id), true);
        } else {
            addCat(static_cast<uint32_t>(sel), true);
            for (const auto& c : A.categories)
                if (c.parent == sel) addCat(static_cast<uint32_t>(c.id), true);
        }
        const int total = static_cast<int>(rows.size());
        const int nRows = static_cast<int>((kPanelBottom - kPanelTop) / kStatRowU);
        A.listScroll = std::clamp(A.listScroll, 0, std::max(0, total - nRows));
        if (live && focused >= kIdAchRow0 && focused < kIdAchRow0 + static_cast<uint32_t>(nRows)) {
            const int vi = static_cast<int>(focused - kIdAchRow0);
            if (vi == nRows - 1 && A.listScroll + nRows < total) {
                ++A.listScroll;
                winFocus_.setFocus(focused - 1);
            } else if (vi == 0 && A.listScroll > 0) {
                --A.listScroll;
                winFocus_.setFocus(focused + 1);
            }
        }
        for (int v = 0; v < nRows && v < 16; ++v) {
            const int i = A.listScroll + v;
            if (i >= total) break;
            const uint32_t id = rows[static_cast<size_t>(i)].id;
            A.shownIds.push_back(id);
            const Rect rr = AR(panelX, kPanelTop + kStatRowU * static_cast<float>(v), kBtnW, kStatRowU);
            const bool f = live && winFocus_.addStop(kIdAchRow0 + static_cast<uint32_t>(v), rr);
            if (id & 0x80000000u) {  // AchievementFrameStats_SetHeader
                if (A.statHdr.pixels) ui.image(A.statHdr, rr);
                const pp::AchCatRec* c = ix.category(id & 0x7FFFFFFFu);
                ui.textShadow(fs, rr.x + rr.w * 0.5f, rr.y + (rr.h - lh) * 0.5f, c ? ix.str(c->name) : "", kGold,
                              Align::Center);
            } else {  // AchievementFrameStats_SetStat: every other line yellow
                const pp::Texture& bg = (i % 2 == 0) ? A.statOdd : A.statEven;
                if (bg.pixels) ui.image(bg, rr, (i % 2 == 0) ? 0xFFFFFFFFu : 0x80FFFFFFu, (i % 2 == 0) ? Blend::Alpha : Blend::Add);
                const AchView av = view(id);
                const char* name = av.rec ? av.cat->str(av.rec->name) : "";
                // GetStatistic: the criteria's counter (SUMM 0x08: their sum, MAX_USED 0x10: the largest), "--" none.
                std::string value = "--";
                if (av.rec && av.rec->critCount > 0) {
                    const pp::AchCritRec* cr = av.cat->criteria(*av.rec);
                    uint64_t sum = 0, best = 0;
                    bool any = false;
                    for (uint16_t k = 0; k < av.rec->critCount; ++k) {
                        auto it = progress.find(cr[k].id);
                        if (it == progress.end()) continue;
                        any = true;
                        sum += it->second.counter;
                        best = std::max<uint64_t>(best, it->second.counter);
                        if (!(av.idx->flags & 0x18)) break;  // the first criteria's
                    }
                    if (any) value = std::to_string((av.idx->flags & 0x10) ? best : sum);
                }
                const float vw = static_cast<float>(fs.measure(value.c_str()));
                ui.pushClip(Rect{rr.x + 4.0f * kSX, rr.y, rr.w - vw - 14.0f * kSX, rr.h});
                ui.textShadow(fs, rr.x + 4.0f * kSX, rr.y + (rr.h - lh) * 0.5f, name, kWhite);
                ui.popClip();
                ui.textShadow(fs, rr.x + rr.w - 5.0f * kSX, rr.y + (rr.h - lh) * 0.5f, value.c_str(), kStatValue, Align::Right);
            }
            if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, rr, 0x80FFFFFFu, Blend::Add);
        }
        if (total > nRows) {
            const bool canUp = A.listScroll > 0, canDn = A.listScroll + nRows < total;
            const Rect up{AX(panelX + kBtnW + 4), AY(kPanelTop), 11, 11}, dn{AX(panelX + kBtnW + 4), AY(kPanelBottom) - 11, 11, 11};
            const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
            const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
            if (tu.pixels) ui.image(tu, up);
            if (td.pixels) ui.image(td, dn);
            if (live && canUp && winFocus_.addStop(kIdAchListUp, up)) ui.fill(up, 0x40FFFFFFu);
            if (live && canDn && winFocus_.addStop(kIdAchListDown, dn)) ui.fill(dn, 0x40FFFFFFu);
        }
        ui.nineSlice(kit_.tex(kTexTipBorder), AR(218, 19, 504, 461), 1.0f, kGoldBorder);
        return true;
    }

    // --- The summary (AchievementFrameSummary) --------------------------------------------------------------------
    if (sel == kSummary) {
        constexpr float sx = 196.0f, sw = 529.0f;
        // LATEST_UNLOCKED_ACHIEVEMENTS: the header (UI-Achievement-RecentHeader) and four SummaryAchievementTemplate.
        if (A.recentHdr.pixels) ui.image(A.recentHdr, AR(sx + 5 - 20, 39, sw - 10 + 40, 20));
        ui.textShadow(fs, AX(sx + sw * 0.5f), AY(49) - lh * 0.5f, CLIENT_STR(LATEST_UNLOCKED_ACHIEVEMENTS), kGold, Align::Center);
        std::vector<std::pair<uint32_t, uint32_t>> latest;  // GetLatestCompletedAchievements: newest first
        for (const auto& [id, when] : earned) {
            const pp::AchIdxRec* r = ix.find(id);
            if (!r || (r->flags & kFlagStatistic)) continue;
            latest.emplace_back(dateKey(when), id);
        }
        std::sort(latest.begin(), latest.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second > b.second;
        });
        std::vector<std::pair<uint32_t, bool>> buttons;  // id, incomplete default
        for (size_t i = 0; i < latest.size() && buttons.size() < static_cast<size_t>(kMaxSummary); ++i)
            buttons.emplace_back(latest[i].second, false);
        size_t def = 0;
        while (buttons.size() < static_cast<size_t>(kMaxSummary) && def < 5) {  // the defaults not yet completed
            const uint32_t id = kDefaultSummary[def++];
            if (!earned.count(id)) buttons.emplace_back(id, true);
        }
        if (latest.empty()) ui.textShadow(fs, AX(sx + sw * 0.5f), AY(89), CLIENT_STR(NO_COMPLETED_ACHIEVEMENTS), kWhite, Align::Center);
        for (size_t k = 0; k < buttons.size(); ++k) {
            const auto [id, incomplete] = buttons[k];
            const AchView av = view(id);
            if (!av.rec) continue;
            const float by = 57.0f + 45.0f * static_cast<float>(k);
            const Rect br = AR(sx + 23, by, sw - 46, 48);
            A.shownIds.push_back(id);
            const bool f = live && winFocus_.addStop(kIdAchSum0 + static_cast<uint32_t>(k), br);
            const bool sat = !incomplete;
            const pp::Texture& pb = sat ? A.parch : A.parchD;
            if (pb.pixels) ui.image(pb, br, SrcRect{0, 0, static_cast<float>(pb.uw), static_cast<float>(pb.uh) * 0.25f});
            const pp::Texture& tb = sat ? A.titleS : A.titleD;
            if (tb.pixels) ui.image(tb, AR(sx + 28, by + 5, sw - 56, 20), 0x80FFFFFFu);
            ui.nineSlice(kit_.tex(kTexTipBorder), br, 1.0f, sat ? withAlpha(kRedBorder, 0.5f) : kGreyBorder);
            if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, br, 0xFFFFFFFFu, Blend::Add);
            // The icon (48 x 48 at TOPLEFT 3,-3; the texture 36 x 36), the shield (48 x 48 at TOPRIGHT -10,-4).
            const float is = 36.0f * kSY;
            const Rect ic{AX(sx + 23 + 3 + 6), AY(by + 3 + 3), is, is};
            if (const pp::Texture* t = spellDb_.iconById(av.idx->icon, spellFrame_)) ui.image(*t, ic, sat ? kWhite : 0xFF8C8C8Cu);
            if (A.iconFrame.pixels) ui.image(A.iconFrame, Rect{ic.x - 2, ic.y - 2, ic.w + 4, ic.h + 4}, sat ? kWhite : 0xFFBFBFBFu);
            const pp::Texture& sh = av.idx->points > 0 ? A.shields : A.shieldsNp;
            const Rect sr{br.x + br.w - 10.0f * kSX - 22.0f, AY(by + 4), 22, 22};
            if (sh.pixels) ui.image(sh, sr, SrcRect{sat ? 0.0f : sh.uw * 0.5f, 0, sh.uw * 0.5f, static_cast<float>(sh.uh)});
            if (av.idx->points > 0) {
                char b[8];
                std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(av.idx->points));
                ui.textShadow(fs, sr.x + sr.w * 0.5f, sr.y + (sr.h - lh) * 0.5f, b, sat ? kWhite : kGrey65, Align::Center);
            }
            ui.textShadow(fs, br.x + br.w * 0.5f, AY(by + 5) + (20.0f * kSY - lh) * 0.5f, av.cat->str(av.rec->name),
                          sat ? kWhite : kGrey65, Align::Center);
            ui.pushClip(Rect{ic.x + ic.w + 4, br.y, sr.x - ic.x - ic.w - 8, br.h});
            ui.text(ft, br.x + br.w * 0.5f, AY(by + 30), av.cat->str(av.rec->desc), sat ? kBlack : kWhite, Align::Center);
            ui.popClip();
            if (av.earned) ui.text(ft, sr.x - 4, AY(by + 8), shortDate(av.date).c_str(), kWhite, Align::Right);
            if (f && incomplete)  // AchievementFrameSummaryAchievement_OnEnter (ANCHOR_RIGHT)
                wotlkTip(screenRect(br),
                         {WotlkTipLine{CLIENT_STR(SUMMARY_ACHIEVEMENT_INCOMPLETE), kWhite},
                          WotlkTipLine{CLIENT_STR(SUMMARY_ACHIEVEMENT_INCOMPLETE_TEXT), kGold}},
                         kTipRight);
        }
        // ACHIEVEMENT_CATEGORY_PROGRESS: the header, the total (ACHIEVEMENTS_COMPLETED), the eight categories.
        if (A.recentHdr.pixels) ui.image(A.recentHdr, AR(sx + 5 - 20, 257, sw - 10 + 40, 20));
        ui.textShadow(fs, AX(sx + sw * 0.5f), AY(267) - lh * 0.5f, CLIENT_STR(ACHIEVEMENT_CATEGORY_PROGRESS), kGold, Align::Center);
        auto statusBar = [&](uint32_t id, const Rect& r, const pp::Texture& art, const char* label, int done, int total) {
            const bool f = live && winFocus_.addStop(id, r);
            ui.fill(Rect{r.x, r.y + 2, r.w, r.h - 4}, 0x66000000u);
            if (total > 0 && A.bar.pixels) {
                const float frac = std::min(1.0f, static_cast<float>(done) / static_cast<float>(total));
                ui.image(A.bar, Rect{r.x, r.y + 2, r.w * frac, r.h - 4},
                         SrcRect{0, 0, static_cast<float>(A.bar.uw) * frac, static_cast<float>(A.bar.uh)}, 0xFF009900u);
            }
            if (art.pixels) ui.image(art, Rect{r.x - 15.0f * kSX, r.y - 16.0f * kSY, r.w + 30.0f * kSX, 48.0f * kSY});
            if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, r, 0x80FFFFFFu, Blend::Add);
            ui.textShadow(fs, r.x + 6.0f * kSX, r.y + (r.h - lh) * 0.5f, label, kGold);
            char b[24];
            std::snprintf(b, sizeof(b), "%d/%d", done, total);
            ui.textShadow(fs, r.x + r.w - 5.0f * kSX, r.y + (r.h - lh) * 0.5f, b, kWhite, Align::Right);
        };
        {
            int total = 0, done = 0;  // GetNumCompletedAchievements
            for (uint32_t k = 0; k < ix.count(); ++k) {
                const pp::AchIdxRec& r = ix.at(k);
                if ((r.flags & kFlagStatistic) || r.category == kFeatOfStrength || !eligible(r)) continue;
                ++total;
                if (earned.count(r.id)) ++done;
            }
            statusBar(kIdAchSumTotal, AR(sx + 20, 283, 488, 21), A.sumBar, CLIENT_STR(ACHIEVEMENTS_COMPLETED), done, total);
        }
        for (int k = 0; k < 8; ++k) {
            const uint32_t cat = kSummaryCategories[k];
            int total = 0, done = 0;
            totalCounts(cat, total, done);
            const pp::AchCatRec* c = ix.category(cat);
            const float bx = sx + 20 + (k % 2) * (234.0f + 20.0f), by = 328.0f + 35.0f * static_cast<float>(k / 2);
            statusBar(kIdAchSumCat0 + static_cast<uint32_t>(k), AR(bx, by, 234, 21), A.catBar, c ? ix.str(c->name) : "",
                      done, total);
        }
        ui.nineSlice(kit_.tex(kTexTipBorder), AR(sx, 19, sw, 461), 1.0f, kGoldBorder);
        return true;
    }

    // --- AchievementFrameAchievements --------------------------------------------------------------------------------
    // The category's list as the client gives it: completed first (the newest first), then the incomplete in order.
    if (A.listCat != sel || A.listVersion != session_.achievementVersion()) {
        A.listCat = sel;
        A.listVersion = session_.achievementVersion();
        std::vector<const pp::AchIdxRec*> done, todo;
        for (uint32_t i = 0; i < ix.count(); ++i) {
            const pp::AchIdxRec& r = ix.at(i);
            if (r.category != static_cast<uint32_t>(sel) || (r.flags & kFlagStatistic) || !eligible(r)) continue;
            const bool e = earned.count(r.id) != 0;
            if (r.prev && !earned.count(r.prev)) continue;          // a chain: its first incomplete step
            if (e && r.next && earned.count(r.next)) continue;      // and its last completed one
            (e ? done : todo).push_back(&r);
        }
        std::sort(done.begin(), done.end(), [&](const pp::AchIdxRec* a, const pp::AchIdxRec* b) {
            const uint32_t ka = dateKey(earned.at(a->id)), kb = dateKey(earned.at(b->id));
            if (ka != kb) return ka > kb;
            return a->order != b->order ? a->order < b->order : a->id < b->id;
        });
        std::sort(todo.begin(), todo.end(), [](const pp::AchIdxRec* a, const pp::AchIdxRec* b) {
            return a->order != b->order ? a->order < b->order : a->id < b->id;
        });
        A.listIds.clear();
        for (const auto* r : done) A.listIds.push_back(r->id);
        for (const auto* r : todo) A.listIds.push_back(r->id);
        A.listCompleted = static_cast<int>(done.size());
        // AchievementFrameAchievements_ForceUpdate: a completed selection with a next step is cleared.
        if (A.selection) {
            const pp::AchIdxRec* r = ix.find(A.selection);
            if (r && r->next && earned.count(A.selection)) A.selection = 0;
        }
    }
    // ACHIEVEMENTUI_SELECTEDFILTER: all / the completed (the first numCompleted) / the incomplete (after them).
    const int first = A.filter == 2 ? A.listCompleted : 0;
    const int count = A.filter == 1 ? A.listCompleted
                      : A.filter == 2 ? static_cast<int>(A.listIds.size()) - A.listCompleted
                                      : static_cast<int>(A.listIds.size());
    if (A.listScroll < 0) {  // AchievementFrame_SelectAchievement: scrolled to the selection
        A.listScroll = 0;
        for (int i = 0; i < count; ++i)
            if (A.listIds[static_cast<size_t>(first + i)] == A.selection) A.listScroll = i;
    }
    A.listScroll = std::clamp(A.listScroll, 0, std::max(0, count - 1));
    // AchievementFrameFilterDropDown (TOPRIGHT -98,10; hidden for Feats of Strength): its text and its button.
    const bool fos = sel == static_cast<int32_t>(kFeatOfStrength);
    if (!fos) {
        ui.textShadow(fs, AX(560), AY(10) - lh * 0.5f - 2.0f * kSY, filterText(A.filter), kWhite);
        const Rect b = AR(646, -10, 24, 24);
        const bool f = live && winFocus_.addStop(kIdAchFilter, AR(556, -12, 116, 30));
        const pp::Texture& bt = (f && winFocus_.isPressed(kIdAchFilter)) || A.ddOpen ? A.ddBtnDn : A.ddBtn;
        if (bt.pixels) ui.image(bt, Rect{b.x, b.y, b.w, b.w});
        if (f) ui.fill(Rect{b.x, b.y, b.w, b.w}, 0x40FFFFFFu);
    }
    if (fos && count == 0)  // AchievementFrameAchievementsFeatOfStrengthText
        for (size_t k = 0; const std::string& l : wrapQuestText(fs, CLIENT_STR(FEAT_OF_STRENGTH_DESCRIPTION), 490.0f * kSX))
            ui.textShadow(fs, AX(panelX + 248), AY(189) + lh * static_cast<float>(k++), l.c_str(), kWhite, Align::Center);

    const float descLineU = lh / kSY, tinyLineU = th / kSY;
    // The buttons from listScroll down while they fit (PSP: by rows; HybridScrollFrame scrolls by pixels).
    float y = kPanelTop + 2.0f;
    int lastShown = -1;
    for (int i = A.listScroll; i < count && A.shownIds.size() < 16; ++i) {
        const uint32_t id = A.listIds[static_cast<size_t>(first + i)];
        const AchView av = view(id);
        if (!av.rec) continue;
        const bool sat = av.earned;
        const bool selected = A.selection == id;
        const char* desc = av.cat->str(av.rec->desc);
        const char* reward = av.cat->str(av.rec->reward);
        const bool hasReward = reward[0] != 0;
        const auto descLines = wrapQuestText(fs, desc, kContentW * kSX);
        // AchievementButton_DisplayObjectives (when selected): what is under the description, and the height.
        struct Crit {
            uint8_t kind;  // 0 text, 1 bar, 2 meta, 3 chain step
            const pp::AchCritRec* c;
            uint32_t ach;
            bool done;
            uint64_t have;
        };
        std::vector<Crit> crits;
        float objH = 0.0f;
        int textCols = 1, textRows = 0, bars = 0, metas = 0, steps = 0;
        float maxTextW = 0.0f;
        if (selected) {
            if (av.earned && av.idx->prev) {  // AchievementObjectives_DisplayProgressiveAchievement
                std::vector<uint32_t> chain{id};
                for (const pp::AchIdxRec* r = av.idx; r && r->prev && chain.size() < 24; r = ix.find(r->prev))
                    chain.insert(chain.begin(), r->prev);
                for (uint32_t a : chain) crits.push_back(Crit{3, nullptr, a, earned.count(a) != 0, 0});
                steps = static_cast<int>(chain.size());
                objH = std::ceil(static_cast<float>(steps) / 6.0f) * kMiniH;
            } else {  // AchievementObjectives_DisplayCriteria
                const pp::AchCritRec* cr = av.cat->criteria(*av.rec);
                for (uint16_t k = 0; k < av.rec->critCount; ++k) {
                    const pp::AchCritRec& c = cr[k];
                    auto it = progress.find(c.id);
                    const uint64_t have = it != progress.end() ? it->second.counter : 0;
                    const uint32_t need = std::max<uint32_t>(1, c.quantity);
                    const bool done = av.earned || have >= need;
                    if (c.type == kCriteriaTypeAchievement && c.asset) {
                        crits.push_back(Crit{2, &c, c.asset, earned.count(c.asset) != 0, have});
                        ++metas;
                    } else if (c.flags & kCriteriaProgressBar) {
                        crits.push_back(Crit{1, &c, 0, done, av.earned ? c.quantity : have});
                        ++bars;
                    } else {
                        crits.push_back(Crit{0, &c, 0, done, have});
                        maxTextW = std::max(maxTextW, static_cast<float>(ft.measure(av.cat->str(c.desc))) / kSX + 22.0f);
                        ++textRows;
                    }
                }
                if (textRows > 1 && bars == 0 && maxTextW > 0.0f) textCols = std::max(1, static_cast<int>(kContentW / maxTextW));
                objH = static_cast<float>(bars) * 20.0f + std::ceil(static_cast<float>(textRows) / static_cast<float>(textCols)) * kCritRowU +
                       (metas > 0 ? std::ceil(static_cast<float>(metas) / 2.0f) * kMetaRowU + 6.0f : 0.0f);
            }
        }
        // PSP: the collapsed height holds the title band and two description lines (the reward under them).
        const int shownLines = selected ? static_cast<int>(descLines.size()) : std::min<int>(2, static_cast<int>(descLines.size()));
        float h = 30.0f + descLineU * static_cast<float>(shownLines) + 8.0f + (hasReward ? 20.0f : 0.0f);
        h = std::max(h, 84.0f);
        if (selected && objH > 0.0f) h += objH + 8.0f;
        if (y + h > kPanelBottom && !A.shownIds.empty()) break;
        lastShown = i;
        const size_t v = A.shownIds.size();
        A.shownIds.push_back(id);
        const Rect br = AR(panelX, y, kBtnW, h);
        const bool f = live && winFocus_.addStop(kIdAchRow0 + static_cast<uint32_t>(v), br);
        // The parchment (its bottom h units: SetTexCoord(0, 1, 1 - h / 256, 1)), the title band, the red / grey border.
        const pp::Texture& pb = sat ? A.parch : A.parchD;
        if (pb.pixels) {
            const float vf = std::min(1.0f, h / 256.0f);
            ui.image(pb, Rect{br.x + 3.0f * kSX, br.y + 3.0f * kSY, br.w - 6.0f * kSX, br.h - 6.0f * kSY},
                     SrcRect{0, static_cast<float>(pb.uh) * (1.0f - vf), static_cast<float>(pb.uw), static_cast<float>(pb.uh) * vf});
        }
        const pp::Texture& tb = sat ? A.titleS : A.titleD;
        if (tb.pixels) ui.image(tb, AR(panelX + 5, y + 5, kBtnW - 10, 24));
        ui.nineSlice(kit_.tex(kTexTipBorder), br, 1.0f, sat ? kRedBorder : kGreyBorder);
        if ((selected || f) && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, br, selected ? 0xFFFFFFFFu : 0x80FFFFFFu, Blend::Add);
        // The icon (60 x 60 at TOPLEFT 8,-9; AchievementIcon_Desaturate) and its frame.
        const float is = 50.0f * kSY;
        const Rect ic{AX(panelX + 8 + 5), AY(y + 9 + 3), is, is};
        if (const pp::Texture* t = spellDb_.iconById(av.idx->icon, spellFrame_)) ui.image(*t, ic, sat ? kWhite : 0xFF8C8C8Cu);
        if (A.iconFrame.pixels) ui.image(A.iconFrame, Rect{ic.x - 3, ic.y - 3, ic.w + 6, ic.h + 6}, sat ? kWhite : 0xFFBFBFBFu);
        // The shield (TOPRIGHT -6,0; the points - a chain's total) and the date under it.
        uint32_t points = av.idx->points;
        if (av.idx->prev)  // AchievementButton_GetProgressivePoints
            for (const pp::AchIdxRec* r = ix.find(av.idx->prev); r; r = r->prev ? ix.find(r->prev) : nullptr) points += r->points;
        const pp::Texture& sh = av.idx->points > 0 ? A.shields : A.shieldsNp;
        const Rect sr{br.x + br.w - 6.0f * kSX - 30.0f, br.y + 6.0f * kSY, 30, 28};
        if (sh.pixels) ui.image(sh, sr, SrcRect{sat ? 0.0f : sh.uw * 0.5f, 0, sh.uw * 0.5f, static_cast<float>(sh.uh)});
        if (points > 0) {
            char b[12];
            std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(points));
            const Font& pf = points <= 100 ? fs : ft;  // AchievementPointsFont / -Small
            ui.textShadow(pf, sr.x + sr.w * 0.5f - 1.0f, sr.y + (sr.h - static_cast<float>(pf.lineHeight())) * 0.5f - 1.0f, b,
                          sat ? kWhite : kGrey65, Align::Center);
        }
        if (av.earned) ui.text(ft, sr.x + sr.w * 0.5f - 2.0f, sr.y + sr.h, shortDate(av.date).c_str(), kWhite, Align::Center);
        // + / - (UI-Achievement-PlusMinus at TOPLEFT 72,-9): criteria, or a completed chain.
        if ((av.rec->critCount > 0 || (av.idx->prev && sat)) && A.plusMinus.pixels) {
            const float cw = static_cast<float>(A.plusMinus.uw) * 0.5f, ch = static_cast<float>(A.plusMinus.uh) * 0.5f;
            ui.image(A.plusMinus, Rect{AX(panelX + 72), AY(y + 9), 9, 9},
                     SrcRect{sat ? 0.0f : cw, selected ? ch : 0.0f, cw, ch});
        }
        // The title (GameFontHighlightMedium) and the description (AchievementDescriptionFont: black / white).
        {
            const char* name = av.cat->str(av.rec->name);
            const Rect tr{br.x + 80.0f * kSX, AY(y + 5), br.w - 160.0f * kSX, 24.0f * kSY};
            if (static_cast<float>(fs.measure(name)) <= tr.w)
                ui.textShadow(fs, tr.x + tr.w * 0.5f, tr.y + (tr.h - lh) * 0.5f, name, sat ? kWhite : kGrey65, Align::Center);
            else
                ui.textMarquee(fs, Rect{tr.x, tr.y + (tr.h - lh) * 0.5f, tr.w, lh}, name, sat ? kWhite : kGrey65);
        }
        float ty = AY(y + 30);
        for (int k = 0; k < shownLines; ++k) {
            const std::string& l = descLines[static_cast<size_t>(k)];
            if (sat) ui.text(fs, br.x + br.w * 0.5f, ty, l.c_str(), kBlack, Align::Center);
            else ui.textShadow(fs, br.x + br.w * 0.5f, ty, l.c_str(), kWhite, Align::Center);
            ty += lh;
        }
        // The objectives (TOP at the description's BOTTOM -8; LEFT at the icon's RIGHT, RIGHT at the shield's LEFT -10).
        if (selected && !crits.empty()) {
            const float ox0 = panelX + 76.0f, ow = kBtnW - 76.0f - 80.0f;
            float oy = y + 30.0f + descLineU * static_cast<float>(shownLines) + 8.0f;
            if (steps > 0) {  // MiniAchievementTemplate: 6 a row
                for (int k = 0; k < steps; ++k) {
                    const Crit& c = crits[static_cast<size_t>(k)];
                    const pp::AchIdxRec* r = ix.find(c.ach);
                    const float mx = ox0 - 4.0f + (k % 6) * (kMiniW + 4.0f), my = oy + 4.0f + (k / 6) * kMiniH;
                    const float ms = 26.0f * kSY;
                    const Rect mr{AX(mx + 8), AY(my + 8), ms, ms};
                    if (r)
                        if (const pp::Texture* t = spellDb_.iconById(r->icon, spellFrame_)) ui.image(*t, mr);
                    if (A.miniBorder.pixels) ui.image(A.miniBorder, Rect{mr.x - 4, mr.y - 4, mr.w + 8, mr.h + 8});
                    if (A.miniShield.pixels) ui.image(A.miniShield, Rect{mr.x + mr.w - 4, mr.y + mr.h - 4, 10, 10});
                    if (r && r->points) {
                        char b[8];
                        std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(r->points));
                        ui.textShadow(ft, mr.x + mr.w + 6, mr.y + mr.h - th + 3, b, kWhite, Align::Right);
                    }
                    const uint32_t cid = kIdAchCrit0 + static_cast<uint32_t>(A.critIds.size());
                    if (A.critIds.size() < 16) {
                        A.critIds.push_back(c.ach);
                        if (live && winFocus_.addStop(cid, mr)) {  // the step's name, description, date
                            const AchView sv = view(c.ach);
                            std::vector<WotlkTipLine> t;
                            if (sv.rec) {
                                t.push_back(WotlkTipLine{sv.cat->str(sv.rec->name), kWhite});
                                t.push_back(WotlkTipLine{sv.cat->str(sv.rec->desc), kGold});
                                if (sv.earned) t.push_back(WotlkTipLine{shortDate(sv.date), kWhite});
                            }
                            wotlkTip(screenRect(mr), std::move(t), kTipRight);
                        }
                    }
                }
            } else {
                // The progress bars first (AchievementProgressBarTemplate: quantity / required), then the text criteria
                // (columns when they fit), then the meta criteria (two a row).
                for (const Crit& c : crits) {
                    if (c.kind != 1) continue;
                    const Rect pr{AX(ox0 + (ow - 212.0f) * 0.5f), AY(oy + 4), 212.0f * kSX, 14.0f};
                    ui.fill(pr, 0xFF202020u);
                    const float frac = c.c->quantity > 0 ? std::min(1.0f, static_cast<float>(c.have) / static_cast<float>(c.c->quantity)) : 0.0f;
                    if (A.sbar.pixels)
                        ui.image(A.sbar, Rect{pr.x, pr.y, pr.w * frac, pr.h},
                                 SrcRect{0, 0, static_cast<float>(A.sbar.uw) * frac, static_cast<float>(A.sbar.uh)}, 0xFF00B200u);
                    if (A.pbarBorder.pixels) {
                        const float bw2 = static_cast<float>(A.pbarBorder.uw), bh2 = static_cast<float>(A.pbarBorder.uh);
                        const float cap = bw2 * 0.0625f / 0.8745f, capR = bw2 * (0.8745f - 0.812f) / 0.8745f;
                        ui.image(A.pbarBorder, Rect{pr.x - 4, pr.y - 3, 7, pr.h + 6}, SrcRect{0, 0, cap, bh2});
                        ui.image(A.pbarBorder, Rect{pr.x + 3, pr.y - 3, pr.w - 6, pr.h + 6}, SrcRect{cap, 0, bw2 - cap - capR, bh2});
                        ui.image(A.pbarBorder, Rect{pr.x + pr.w - 3, pr.y - 3, 7, pr.h + 6}, SrcRect{bw2 - capR, 0, capR, bh2});
                    }
                    char b[40];
                    std::snprintf(b, sizeof(b), "%llu/%u", static_cast<unsigned long long>(c.have), static_cast<unsigned>(c.c->quantity));
                    ui.textShadow(ft, pr.x + pr.w * 0.5f, pr.y + (pr.h - th) * 0.5f, b, kWhite, Align::Center);
                    oy += 20.0f;
                }
                int t = 0;
                const float colW = kContentW / static_cast<float>(textCols);
                for (const Crit& c : crits) {
                    if (c.kind != 0) continue;
                    const int col = t % textCols, row = t / textCols;
                    ++t;
                    float cx = ox0 + colW * static_cast<float>(col);
                    if (textRows == 1 && bars == 0) cx = ox0 + (ow - maxTextW) * 0.5f;  // a lone one centred (TOP -14,0)
                    const float cy = AY(oy + kCritRowU * static_cast<float>(row));
                    const char* name = av.cat->str(c.c->desc);
                    // AchievementCriteriaTemplate: the check when done; black in a completed achievement, green done,
                    // grey (0.6) to do ("- name").
                    const uint32_t col2 = sat && c.done ? kBlack : c.done ? kGreen : kGrey60;
                    if (c.done && A.check.pixels) ui.image(A.check, Rect{AX(cx), cy, 10, 8});
                    const std::string txt = c.done ? std::string(name) : std::string("- ") + name;
                    if (col2 == kBlack) ui.text(ft, AX(cx) + (c.done ? 11.0f : 0.0f), cy, txt.c_str(), col2);
                    else ui.textShadow(ft, AX(cx) + (c.done ? 11.0f : 0.0f), cy, txt.c_str(), col2);
                }
                oy += std::ceil(static_cast<float>(textRows) / static_cast<float>(textCols)) * kCritRowU;
                int m = 0;
                for (const Crit& c : crits) {
                    if (c.kind != 2) continue;
                    const float mx = metas == 1 ? ox0 + (ow - 155.0f) * 0.5f : ox0 + (m % 2 == 0 ? 0.0f : 170.0f);
                    const float my = oy + 4.0f + kMetaRowU * static_cast<float>(metas == 1 ? 0 : m / 2);
                    ++m;
                    const pp::AchIdxRec* r = ix.find(c.ach);
                    const Rect mr = AR(mx, my, 155, 30);
                    const float is2 = 17.0f * kSY + 4.0f;
                    const Rect icr{mr.x + 6.0f * kSX, mr.y + (mr.h - is2) * 0.5f, is2, is2};
                    if (r)
                        if (const pp::Texture* tx = spellDb_.iconById(r->icon, spellFrame_))
                            ui.image(*tx, icr, c.done ? kWhite : 0xFF8C8C8Cu);
                    if (A.miniBorder.pixels) ui.image(A.miniBorder, Rect{icr.x - 2, icr.y - 2, icr.w + 4, icr.h + 4}, c.done ? kWhite : 0xFFBFBFBFu);
                    const std::string mname = session_.achievementName(c.ach);
                    const uint32_t col2 = sat && c.done ? kBlack : c.done ? kGreen : kGrey60;
                    ui.pushClip(Rect{icr.x + icr.w + 2, mr.y, mr.w - icr.w - 14, mr.h});
                    if (col2 == kBlack) ui.text(ft, icr.x + icr.w + 3, mr.y + (mr.h - th) * 0.5f, mname.c_str(), col2);
                    else ui.textShadow(ft, icr.x + icr.w + 3, mr.y + (mr.h - th) * 0.5f, mname.c_str(), col2);
                    ui.popClip();
                    if (c.done && A.check.pixels) ui.image(A.check, Rect{mr.x - 10, mr.y + (mr.h - 8) * 0.5f, 10, 8});
                    if (A.critIds.size() < 16) {
                        const uint32_t cid = kIdAchCrit0 + static_cast<uint32_t>(A.critIds.size());
                        A.critIds.push_back(c.ach);
                        if (live && winFocus_.addStop(cid, mr)) {  // the meta's achievement: name, description, date
                            const AchView mv = view(c.ach);
                            std::vector<WotlkTipLine> t2;
                            if (mv.rec) {
                                t2.push_back(WotlkTipLine{mv.cat->str(mv.rec->name), kWhite});
                                t2.push_back(WotlkTipLine{mv.cat->str(mv.rec->desc), kGold});
                                if (mv.earned) t2.push_back(WotlkTipLine{CLIENT_FMT(ACHIEVEMENT_META_COMPLETED_DATE, shortDate(mv.date)), kWhite});
                            }
                            wotlkTip(screenRect(mr), std::move(t2), kTipRight);
                        }
                    }
                }
            }
        }
        // The reward (GameFontNormalSmall gold / grey 0.8) at the bottom.
        if (hasReward) {
            const Rect rr{br.x + 5.0f * kSX, br.y + br.h - 22.0f * kSY, br.w - 10.0f * kSX, 20.0f * kSY};
            ui.fill(rr, sat ? 0x40000000u : 0x30000000u);
            ui.textShadow(ft, rr.x + rr.w * 0.5f, rr.y + (rr.h - th) * 0.5f, reward, sat ? kGold : 0xFFCCCCCCu, Align::Center);
        }
        y += h + 2.0f;
        (void)tinyLineU;
    }
    // The list follows the focus: the last shown button down, the first up (PSP: by buttons).
    {
        const int shownN = static_cast<int>(A.shownIds.size());
        if (live && focused >= kIdAchRow0 && focused < kIdAchRow0 + 16 && shownN > 0) {
            const int vi = static_cast<int>(focused - kIdAchRow0);
            if (vi == shownN - 1 && lastShown + 1 < count && vi > 0) {
                ++A.listScroll;
                winFocus_.setFocus(focused - 1);
            } else if (vi == 0 && A.listScroll > 0) {
                --A.listScroll;
                winFocus_.setFocus(focused + 1);
            }
        }
        const bool canUp = A.listScroll > 0, canDn = lastShown + 1 < count;
        if (canUp || canDn) {
            const Rect up{AX(panelX + kBtnW + 4), AY(kPanelTop), 11, 11}, dn{AX(panelX + kBtnW + 4), AY(kPanelBottom) - 11, 11, 11};
            const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
            const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
            if (tu.pixels) ui.image(tu, up);
            if (td.pixels) ui.image(td, dn);
            if (live && canUp && winFocus_.addStop(kIdAchListUp, up)) ui.fill(up, 0x40FFFFFFu);
            if (live && canDn && winFocus_.addStop(kIdAchListDown, dn)) ui.fill(dn, 0x40FFFFFFu);
        }
    }
    ui.nineSlice(kit_.tex(kTexTipBorder), AR(218, 19, 504, 461), 1.0f, kGoldBorder);
    // The filter's list (AchievementFrameFilterDropDown_Initialize).
    if (A.ddOpen && !fos) {
        std::vector<WotlkDdItem> items(3);
        for (int k = 0; k < 3; ++k) {
            items[static_cast<size_t>(k)].text = filterText(k);
            items[static_cast<size_t>(k)].checked = A.filter == k;
        }
        wotlkDropDownList(ui, AX(546), AY(22), items, kIdAchDd0, focusable);
    }
    return true;
}

// AchievementAlertFrame1..2 (300 x 88 at 0.6; BOTTOM 0,128 of UIParent, the second over the first - BOTTOM at TOP -10).
void WorldScreen::drawAchievementAlerts(UiRenderer& ui, float& stackTop) {
    AchUi& A = *ach_;
    bool any = false;
    for (const auto& a : A.alerts) any = any || a.t >= 0.0f;
    if (!any) return;
    if (!A.index.loaded() && !A.index.tried()) A.index.load();
    A.loadAlertArt();
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const bool focusable = uiMode_ && !settings_.isOpen();
    auto S = [](float u) { return u * 0.6f; };
    const float base = stackTop;
    float highestTop = -1.0f;
    for (int k = 0; k < kMaxAlerts; ++k) {
        AchUi::Alert& al = A.alerts[static_cast<size_t>(k)];
        if (al.t < 0.0f) continue;
        const float t = al.t;
        float a = t < 0.2f ? t / 0.2f : 1.0f;
        if (t > al.outAt) a = std::max(0.0f, 1.0f - (t - al.outAt) / 1.5f);
        const float w = S(300), h = S(88);
        // AchievementAlertFrame1 at BOTTOM 0,128; frame k + 1 BOTTOM at frame k's TOP 0,-10 (whether shown or not).
        const Rect r{std::floor((static_cast<float>(pp::kScreenW) - w) * 0.5f),
                     base - h - static_cast<float>(k) * (h - S(10)), w, h};
        highestTop = r.y;
        const uint32_t white = withAlpha(0xFFFFFFFFu, a);
        const pp::AchIdxRec* ir = A.index.find(al.id);
        const pp::AchCategory* cat = ir ? A.category(ir->category) : nullptr;
        const pp::AchRec* rec = cat ? cat->find(al.id) : nullptr;
        if (A.alertBg.pixels) ui.image(A.alertBg, r, white);
        // $parentUnlocked (GameFontBlackTiny, TOP 7,-23), $parentName (GameFontHighlight, BOTTOMLEFT 72,36 .. -60,36).
        ui.text(ft, r.x + r.w * 0.5f + S(7), r.y + S(23), CLIENT_STR(ACHIEVEMENT_UNLOCKED), withAlpha(kBlack, a), Align::Center);
        const char* name = rec ? cat->str(rec->name) : "";
        const Rect nr{r.x + S(72), r.y + r.h - S(36) - static_cast<float>(fs.lineHeight()), r.w - S(132), static_cast<float>(fs.lineHeight())};
        if (static_cast<float>(fs.measure(name)) <= nr.w) ui.textShadow(fs, nr.x + nr.w * 0.5f, nr.y, name, white, Align::Center);
        else ui.textMarquee(fs, nr, name, white);
        // $parentIcon (AchievementIconFrameTemplate 124 x 124 at TOPLEFT -26,16: the texture 50 x 50 at CENTER 0,3).
        const float icx = r.x + S(-26 + 62), icy = r.y + S(-16 + 62 - 3);
        const Rect ic{icx - S(25), icy - S(25), S(50), S(50)};
        if (ir)
            if (const pp::Texture* tx = spellDb_.iconById(ir->icon, spellFrame_)) ui.image(*tx, ic, white);
        if (A.alertFrame.pixels) ui.image(A.alertFrame, Rect{icx - S(37), icy - S(35), S(72), S(72)}, white);
        // $parentShield (64 x 64 at TOPRIGHT -10,-13): UI-Achievement-Shields / -NoPoints, the points.
        const pp::Texture& sh = ir && ir->points > 0 ? A.alertShields : A.alertShieldsNp;
        const Rect sr{r.x + r.w - S(10) - S(52), r.y + S(13 + 6), S(52), S(48)};
        if (sh.pixels) ui.image(sh, sr, SrcRect{0, 0, sh.uw * 0.5f, static_cast<float>(sh.uh) * 0.9f}, white);
        if (ir && ir->points) {
            char b[8];
            std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(ir->points));
            ui.textShadow(fs, sr.x + sr.w * 0.5f + S(7), sr.y + (sr.h - static_cast<float>(fs.lineHeight())) * 0.5f, b, white,
                          Align::Center);
        }
        // The glow (400 x 171 at CENTER) and the shine (67 x 72 at BOTTOMLEFT 0,8 moving 240 units), ADD.
        if (t < 0.7f && A.alertGlow.pixels) {
            const float ga = t < 0.2f ? t / 0.2f : 1.0f - (t - 0.2f) / 0.5f;
            ui.image(A.alertGlow, Rect{r.x + r.w * 0.5f - S(200), r.y + r.h * 0.5f - S(85.5f), S(400), S(171)},
                     withAlpha(0xFFFFFFFFu, ga), Blend::Add);
        }
        if (t < 0.85f && A.alertShine.pixels) {
            const float sa = t < 0.2f ? t / 0.2f : t < 0.35f ? 1.0f : 1.0f - (t - 0.35f) / 0.5f;
            ui.pushClip(r);
            ui.image(A.alertShine, Rect{r.x + S(240.0f * std::min(1.0f, t / 0.85f)), r.y + r.h - S(8) - S(72), S(67), S(72)},
                     withAlpha(0xFFFFFFFFu, sa), Blend::Add);
            ui.popClip();
        }
        // AchievementAlertFrame_OnClick (Cross on it); the focus holds it (AlertFrame_StopOutAnimation).
        if (focusable && winFocus_.addStop(kIdAchAlert0 + static_cast<uint32_t>(k), r)) al.focus = true;
    }
    // DungeonCompletionAlertFrame_FixAnchors: BOTTOM at the highest shown achievement alert's TOP 0,10.
    if (highestTop >= 0.0f) stackTop = highestTop - S(10);
}

}  // namespace wowee::ui::psp
