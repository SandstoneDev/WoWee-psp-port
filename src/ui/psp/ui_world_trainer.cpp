// The class trainer, the talent tree and the spell book tabs on the
// world screen. The logic is the session's (psp_world_session_trainer.cpp /
// _talents.cpp, upstream InventoryHandler + SpellHandler).
//
// (Buying a spell must show the result; all
// windows 1:1 with the original): the two windows are the original frames,
// composed by tools/psp/build_ui_pak.py and drawn at 0.6 x 0.55 of their
// FrameXML units (PX / PY), at ClassTrainerFrame / PlayerTalentFrame's
// TOPLEFT 0,-104 place (the left, at the top - as the merchant).
//
//   - ClassTrainerFrame (the trainer add-on's .xml / .lua): opens by itself when
//     SMSG_TRAINER_LIST comes (the trainer option of the gossip, or a trainer
//     without a gossip menu). The NPC's portrait and name, the greeting; the
//     list (ClassTrainerSkill1..) grouped under skill line headers with
//     UI-PlusButton / UI-MinusButton (getTrainerTabs: the class lines, then
//     "General"), ALL collapses / expands them all; the services in the
//     ClassTrainerFrame colours (available green, unavailable red, used grey)
//     with the rank right; the selection in UI-Listbox-Highlight2
//     tinted by its type; the filter dropdown FILTER with AVAILABLE /
//     UNAVAILABLE / USED (TRAINER_FILTER_* defaults: 1, 1, 0 -
//     wowee.cfg trainerknown=1 turns "used" on); the details: the icon in
//     UI-EmptySlot, the name and rank, REQUIRES_LABEL + TRAINER_REQ_* (the
//     unmet part red), COSTS_LABEL with the coins (red when the money is
//     short); the player's money; TRAIN (enabled only for an
//     available service the money covers, as ClassTrainer_SetSelection) and
//     EXIT; the close button. The first learnable service is
//     selected on show and on every TRAINER_UPDATE.
//   - PlayerTalentFrame (the talent add-on's .xml + TalentFrameBase.lua): the
//     player's portrait, TALENTS, the tree's background art (ui.pak
//     "tb_<background>") in the scroll frame, the talent buttons at
//     (35 + 63 * column, 20 + 63 * tier) in UI-EmptySlot-White tinted green
//     (points can go in) / gold (maxed) / grey, the rank in
//     TalentFrame-RankBorder, the prerequisite branches and arrows
//     (UI-TalentBranches / UI-TalentArrows, TalentFrame_DrawLines and
//     TalentFrame_Update's branch pass ported as they are), the points bar
//     MASTERY_POINTS_SPENT / UNSPENT_TALENT_POINTS, the tree tabs at the
//     bottom (PlayerTalentTabTemplate, shaved to fit as PlayerTalentFrame_UpdateTabs).
//     The talent tooltip (GameTooltip:SetTalent): name, TOOLTIP_TALENT_RANK, the
//     unmet requirements red (TOOLTIP_TALENT_TIER_POINTS / _PREREQ),
//     TOOLTIP_TALENT_LEARN.
//   - Talent reset (MSG_TALENT_WIPE_CONFIRM from the trainer's gossip): a
//     confirm window with the cost (upstream CONFIRM_TALENT_WIPE).
//   - Spell book tabs: the ids kIdBookTab0.. of ui_world_spellbook.cpp's
//     SpellBookSkillLineTabs are handled here.
// Buttons (PSP): Cross = the left click: on a service selects it
// (ClassTrainerSkillButton_OnClick), on a header collapses / expands, on
// TRAIN learns (BuyTrainerService); Circle on a service = select + learn
// (PSP shortcut for the click + TRAIN). A service that cannot be learned
// says why on the red line (PSP: the original only greys the button). Cross on
// a talent = LearnTalent (PlayerTalentFrameTalent_OnClick without the preview);
// wowee.cfg talentclick=0: the first Cross selects, the second learns.
// PSP: no dual spec tabs, no talent preview mode (the glyph tab - ui_world_glyphs.cpp). The spell
// descriptions (details, GameTooltip of the icon) come from spells.pak "stip".
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids: task A's range 0x6700-0x67FF (kIdMenuTalents = 0x6700 in the header).
enum : uint32_t {
    kIdTrPrev = 0x6701,  // the list's scroll arrows
    kIdTrNext,
    kIdTrLearn,          // TRAIN
    kIdTrTalents,        // (gone - the original frame has no such button)
    kIdTrKnown,          // the filter dropdown
    kIdTrExit,           // EXIT
    kIdTrClose,          // the close button
    kIdTrAll,            // ALL
    kIdTrFilter0,        // + 0..2: the filter's items
    kIdTrIcon = 0x670C,  // the detail icon (its tooltip)
    kIdTrRow0 = 0x6720,  // + 0..15 trainer rows
    kIdTalTab0 = 0x6740,  // + 0..2 talent tabs
    kIdTalUp = 0x6743,
    kIdTalDown,
    kIdTalLearn,
    kIdTalClose,
    kIdWipeYes = 0x6750,
    kIdWipeNo,
    kIdBookTab0 = 0x6760,  // + 0..7 spell book tabs
    kIdTal0 = 0x6780,      // + 0..63 talent buttons (index in the tab, row / column order)
};
constexpr int kMaxBookTabs = 8;

// Frame units -> px (tools/psp/build_ui_pak.py CT_* / TAL_*): 0.6 x 0.55, TOPLEFT 0,-104 -> the left, at the top.
constexpr float kSX = 0.6f, kSY = 0.55f;
constexpr float kOx = 4.0f, kOy = 2.0f;
constexpr float kCtW = 216.0f, kCtH = 240.0f;    // ct_frame: 360 x 437 units
constexpr float kTalW = 214.0f, kTalH = 256.0f;  // tal_frame: 356 x 465 units
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
Rect FR(float x, float y, float w, float h) { return Rect{PX(x), PY(y), w * kSX, h * kSY}; }

// ClassTrainerSkill rows: PSP 7 rows of 25.5 units (14 px: the PSP font is 13 px) in the list's 184 units
// instead of CLASS_TRAINER_SKILLS_DISPLAYED 11 of 16.
constexpr int kRows = 7;
constexpr float kRowU = 25.5f, kListTopU = 98.0f;

// 0xAABBGGRR.
constexpr uint32_t kColGreenFont = 0xFF1AFF1Au;  // GameFontNormalLeftGreen (0.1, 1, 0.1)
constexpr uint32_t kColRedFont = 0xFF1A1AFFu;    // GameFontNormalLeftRed (1, 0.1, 0.1)
constexpr uint32_t kColGreyFont = 0xFF808080u;   // GameFontNormalLeftGrey (0.5)
constexpr uint32_t kColRedCode = 0xFF2020FFu;    // |cffff2020 (RED_FONT_COLOR_CODE)
constexpr uint32_t kColGreenCode = 0xFF20FF20u;  // |cff20ff20 (GREEN_FONT_COLOR_CODE)
constexpr uint32_t kColGreyCode = 0xFF808080u;   // |cff808080 (GRAY_FONT_COLOR_CODE)

// 0 available, 1 unavailable, 2 used (upstream trainerServiceCategory).
int serviceType(const pp::WorldSession& s, const game::TrainerSpell& sp) {
    if (s.hasKnownSpell(sp.spellId)) return 2;
    return sp.state == 0 ? 0 : sp.state == 2 ? 2 : 1;
}

// ClassTrainer_SetSubTextColor / the highlight's vertex colour by service type.
uint32_t subTextColor(int type) { return type == 0 ? 0xFF009900u : type == 2 ? 0xFF808080u : 0xFF000099u; }
uint32_t highlightColor(int type) { return type == 0 ? 0xFF00FF00u : type == 2 ? 0xFF808080u : 0xFF0000E6u; }
uint32_t serviceColor(int type) { return type == 0 ? kColGreenFont : type == 2 ? kColGreyFont : kColRedFont; }

// A line of coloured pieces (|c...|r runs), word-wrapped into w; returns the lines used.
struct Seg {
    std::string s;
    uint32_t col;
};

// A client string's |cAARRGGBB ... |r runs as pieces (the rest in `base`): TRAINER_REQ_*.
void appendColorSegs(std::vector<Seg>& out, const std::string& text, uint32_t base) {
    auto hex = [](char c) -> uint32_t {
        return c >= '0' && c <= '9' ? static_cast<uint32_t>(c - '0')
               : c >= 'a' && c <= 'f' ? static_cast<uint32_t>(c - 'a' + 10)
               : c >= 'A' && c <= 'F' ? static_cast<uint32_t>(c - 'A' + 10)
                                      : 0u;
    };
    std::string cur;
    uint32_t col = base;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '|' && i + 9 < text.size() && text[i + 1] == 'c') {
            if (!cur.empty()) out.push_back(Seg{cur, col});
            cur.clear();
            uint32_t argb = 0;
            for (size_t k = i + 2; k < i + 10; ++k) argb = (argb << 4) | hex(text[k]);
            col = (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);  // ARGB -> the renderer's ABGR
            i += 9;
        } else if (text[i] == '|' && i + 1 < text.size() && text[i + 1] == 'r') {
            if (!cur.empty()) out.push_back(Seg{cur, col});
            cur.clear();
            col = base;
            ++i;
        } else {
            cur += text[i];
        }
    }
    if (!cur.empty()) out.push_back(Seg{cur, col});
}
int drawSegs(UiRenderer& ui, const Font& f, float x, float y, float w, const std::vector<Seg>& segs, int maxLines) {
    const float lh = static_cast<float>(f.lineHeight());
    float cx = x;
    int line = 0;
    for (const Seg& sg : segs) {
        size_t p = 0;
        while (p < sg.s.size()) {
            size_t q = sg.s.find(' ', p + 1);
            if (q == std::string::npos) q = sg.s.size();
            std::string word = sg.s.substr(p, q - p);
            const float ww = static_cast<float>(f.measure(word.c_str()));
            if (cx + ww > x + w && cx > x) {
                if (++line >= maxLines) return line;
                cx = x;
                if (!word.empty() && word[0] == ' ') word.erase(0, 1);
            }
            ui.textShadow(f, cx, y + lh * static_cast<float>(line), word.c_str(), sg.col);
            cx += static_cast<float>(f.measure(word.c_str()));
            p = q;
        }
    }
    return line + 1;
}

// SmallMoneyFrame with SetMoneyFrameColor (ui_world_merchant.cpp drawMoneyFrame + the colour). Width.
float drawCoins(UiRenderer& ui, const Font& fs, const pp::Texture& coinsTex, float x, float y, uint32_t copper,
                uint32_t col) {
    const uint32_t parts[3] = {copper / 10000u, (copper / 100u) % 100u, copper % 100u};
    float cx = x;
    for (int p = 0; p < 3; ++p) {
        if (parts[p] == 0 && !(p == 2 && parts[0] == 0 && parts[1] == 0)) continue;
        char t[12];
        std::snprintf(t, sizeof(t), "%u", static_cast<unsigned>(parts[p]));
        ui.textShadow(fs, cx, y, t, col);
        cx += static_cast<float>(fs.measure(t)) + 1.0f;
        if (coinsTex.pixels) {
            const float uw = coinsTex.uw * 0.25f;
            ui.image(coinsTex, Rect{cx, y + 1, 10, 10}, SrcRect{uw * static_cast<float>(p), 0, uw, static_cast<float>(coinsTex.uh)});
        }
        cx += 12.0f;
    }
    return cx - x;
}

// TalentFrame's TALENT_BRANCH_ARRAY node (TalentFrame_Load).
struct BranchNode {
    int id = 0;  // talent index + 1, 0 = none
    int up = 0, down = 0, left = 0, right = 0, leftArrow = 0, rightArrow = 0, topArrow = 0;
};
constexpr int kTiers = 15, kCols = 4;  // MAX_NUM_TALENT_TIERS, NUM_TALENT_COLUMNS
using BranchGrid = BranchNode[kTiers][kCols];

// TalentFrame_DrawLines (TalentFrameBase.lua), tiers / columns from 0.
void drawLines(BranchGrid& g, int bt, int bc, int tier, int column, bool met) {
    const int rm = met ? 1 : -1;
    if (bt < 0 || bt >= kTiers || tier < 0 || tier >= kTiers || bc < 0 || bc >= kCols || column < 0 || column >= kCols)
        return;
    if (bc == column) {
        if (bt - tier > 1)
            for (int i = tier + 1; i <= bt - 1; ++i)
                if (g[i][bc].id) return;  // blocked vertically
        for (int i = tier; i <= bt - 1; ++i) {
            g[i][bc].down = rm;
            if (i + 1 <= bt - 1) g[i + 1][bc].up = rm;
        }
        g[bt][bc].topArrow = rm;
        return;
    }
    if (bt == tier) {
        const int left = std::min(bc, column), right = std::max(bc, column);
        if (right - left > 1)
            for (int i = left + 1; i <= right - 1; ++i)
                if (g[tier][i].id) return;  // a blocker
        for (int i = left; i <= right - 1; ++i) {
            g[tier][i].right = rm;
            g[tier][i + 1].left = rm;
        }
        if (bc < column) g[bt][bc].rightArrow = rm;
        else g[bt][bc].leftArrow = rm;
        return;
    }
    // Diagonal.
    int left = std::min(bc, column), right = std::max(bc, column);
    if (left == column) ++left;
    else --right;
    bool blocked = false;
    for (int i = left; i <= right; ++i)
        if (g[tier][i].id) blocked = true;
    left = std::min(bc, column);
    right = std::max(bc, column);
    if (!blocked) {
        g[tier][bc].down = rm;
        g[bt][bc].up = rm;
        for (int i = tier; i <= bt - 1; ++i) {
            g[i][bc].down = rm;
            g[i + 1][bc].up = rm;
        }
        for (int i = left; i <= right - 1; ++i) {
            g[tier][i].right = rm;
            g[tier][i + 1].left = rm;
        }
        g[bt][bc].topArrow = rm;
        return;
    }
    // Blocked going up first: across, then up.
    if (left == bc) ++left;
    else --right;
    for (int i = left; i <= right; ++i)
        if (g[bt][i].id) return;  // undrawable
    for (int i = tier; i <= bt - 1; ++i) {
        g[i][column].up = rm;
        g[i + 1][column].down = rm;
    }
    if (bc < column) g[bt][bc].rightArrow = rm;
    else g[bt][bc].leftArrow = rm;
}

// TALENT_BRANCH_TEXTURECOORDS / TALENT_ARROW_TEXTURECOORDS: {left, right, top, bottom} for [1] / [-1].
struct TC {
    float l, r, t, b;
};
enum Branch { kUp, kDown, kLeft, kRight, kTopRight, kTopLeft, kBottomRight, kBottomLeft, kTDown, kTUp };
const TC kBranchTC[10][2] = {
    {{0.12890625f, 0.25390625f, 0, 0.484375f}, {0.12890625f, 0.25390625f, 0.515625f, 1.0f}},  // up
    {{0, 0.125f, 0, 0.484375f}, {0, 0.125f, 0.515625f, 1.0f}},                                // down
    {{0.2578125f, 0.3828125f, 0, 0.5f}, {0.2578125f, 0.3828125f, 0.5f, 1.0f}},                // left
    {{0.2578125f, 0.3828125f, 0, 0.5f}, {0.2578125f, 0.3828125f, 0.5f, 1.0f}},                // right
    {{0.515625f, 0.640625f, 0, 0.5f}, {0.515625f, 0.640625f, 0.5f, 1.0f}},                    // topright
    {{0.640625f, 0.515625f, 0, 0.5f}, {0.640625f, 0.515625f, 0.5f, 1.0f}},                    // topleft
    {{0.38671875f, 0.51171875f, 0, 0.5f}, {0.38671875f, 0.51171875f, 0.5f, 1.0f}},            // bottomright
    {{0.51171875f, 0.38671875f, 0, 0.5f}, {0.51171875f, 0.38671875f, 0.5f, 1.0f}},            // bottomleft
    {{0.64453125f, 0.76953125f, 0, 0.5f}, {0.64453125f, 0.76953125f, 0.5f, 1.0f}},            // tdown
    {{0.7734375f, 0.8984375f, 0, 0.5f}, {0.7734375f, 0.8984375f, 0.5f, 1.0f}},                // tup
};
enum Arrow { kArrowTop, kArrowRight, kArrowLeft };
const TC kArrowTC[3][2] = {
    {{0, 0.5f, 0, 0.5f}, {0, 0.5f, 0.5f, 1.0f}},        // top
    {{1.0f, 0.5f, 0, 0.5f}, {1.0f, 0.5f, 0.5f, 1.0f}},  // right
    {{0.5f, 1.0f, 0, 0.5f}, {0.5f, 1.0f, 0.5f, 1.0f}},  // left
};
struct Piece {  // one branch / arrow texture: TOPLEFT at (x, y) of the scroll child, 32 x 32 units
    TC tc;
    float x, y;
    bool arrow;
};

}  // namespace

// --- Art ------------------------------------------------------------------------------------

WorldScreen::TrainerArt::~TrainerArt() {
    releaseTrainer();
    releaseTalents();
}

void WorldScreen::TrainerArt::releaseTrainer() {
    for (pp::Texture* t : {&ctFrame, &hl, &plusHl, &slot, &plus, &minus, &check})
        if (t->pixels) pp::textureRelease(*t);
    ctTried = false;
}

void WorldScreen::TrainerArt::releaseTalents() {
    for (pp::Texture* t : {&talFrame, &talSlot, &branches, &arrows})
        if (t->pixels) pp::textureRelease(*t);
    talTried = false;
}

namespace {
// through UiArtPak - a pak / texture that finds no memory logs ui_art_fail (the
// trainer / talent art is released with its window and loaded again at the next open).
void loadPakTextures(std::initializer_list<std::pair<const char*, pp::Texture*>> list) {
    UiArtPak art("trainer");
    for (const auto& [name, tex] : list) art.load(name, *tex);
}
}  // namespace

// ct_frame (T8 216 x 240) + the small art, while the trainer is open.
void WorldScreen::loadTrainerFrameArt() {
    if (trArt_.ctTried) return;
    trArt_.ctTried = true;
    loadPakTextures({{"ct_frame", &trArt_.ctFrame}, {"ct_hl", &trArt_.hl}, {"ct_plushl", &trArt_.plusHl},
                     {"ct_slot", &trArt_.slot}, {"ql_plus", &trArt_.plus}, {"ql_minus", &trArt_.minus},
                     {"ql_check", &trArt_.check}});
    PSP_LOGI(pp::tag::Gfx, "trainer_art frame=%d hl=%d slot=%d", trArt_.ctFrame.pixels ? 1 : 0, trArt_.hl.pixels ? 1 : 0,
             trArt_.slot.pixels ? 1 : 0);
}

// tal_frame (T8 214 x 258) + the button / branch art, while the talents are open.
void WorldScreen::loadTalentFrameArt() {
    if (trArt_.talTried) return;
    trArt_.talTried = true;
    loadPakTextures({{"tal_frame", &trArt_.talFrame}, {"tal_slot", &trArt_.talSlot},
                     {"tal_branches", &trArt_.branches}, {"tal_arrows", &trArt_.arrows}});
    if (!tabArtTried_) {  // CharacterFrameTabButtonTemplate (ui_world_paperdoll.cpp drawFrameTab's art)
        tabArtTried_ = true;
        loadPakTextures({{"tab_active", &tabArt_[0]}, {"tab_inactive", &tabArt_[1]}});
    }
    PSP_LOGI(pp::tag::Gfx, "talent_frame_art frame=%d slot=%d branches=%d arrows=%d", trArt_.talFrame.pixels ? 1 : 0,
             trArt_.talSlot.pixels ? 1 : 0, trArt_.branches.pixels ? 1 : 0, trArt_.arrows.pixels ? 1 : 0);
}

// --- Per frame ---------------------------------------------------------------------

void WorldScreen::updateTrainer(float dtSec) {
    PROF_SCOPE("trainer");
    if (!trainer_.init) {
        trainer_.init = true;
        trainer_.showKnown = pp::configGetInt("trainerknown", 0) != 0;
        // 1 by default - a click on a talent learns it, as the original without the preview.
        trainer_.talentClick = pp::configGetInt("talentclick", 1) != 0;
        trainer_.talentArt = pp::configGetInt("talentart", 1) != 0;
        trainer_.filter[2] = trainer_.showKnown;  // TRAINER_FILTER_USED
        PSP_LOGI(pp::tag::Ctrl, "trainer_cfg known=%d talentclick=%d talentart=%d", trainer_.showKnown ? 1 : 0,
                 trainer_.talentClick ? 1 : 0, trainer_.talentArt ? 1 : 0);
    }
    session_.trainerUpdate(dtSec, glm::vec3(playerX_, playerY_, playerZ_));
    // The session's windows -> the stack (as syncNpcWindows): opened by the
    // server, the cursor comes up; Circle on them tells the session.
    const bool want[2] = {session_.isTrainerWindowOpen(), session_.showTalentWipeConfirmDialog()};
    const uint8_t wins[2] = {kWinTrainer, kWinTalentWipe};
    for (int i = 0; i < 2; ++i) {
        const uint8_t w = wins[i];
        if (want[i] && !trainer_.shown[i]) {
            openWindow(w);
            trainer_.shown[i] = true;
            trainer_.guard = true;
            if (i == 0) {
                session_.triggerTutorial(14);  // TUTORIAL_TRIGGER class trainer
                trainer_.page = 0;
                trainer_.tab = 0;
                trainer_.sel = 0;
                trainer_.listScroll = 0;  // ClassTrainerFrame_Show
                trainer_.filterOpen = false;
                trainer_.listVersion = ~0u;
                playUiSound(UiSound::CharacterSheetOpen);  // OnShow: igCharacterInfoOpen
            }
            if (!uiMode_) {
                setUiMode(true);
                trainer_.raisedCursor = true;
                suppress_ = ~0u;  // everything held now stays dead until released
            }
            winFocus_.setFocus(i == 0 ? static_cast<uint32_t>(kIdTrRow0) : static_cast<uint32_t>(kIdWipeNo));
        } else if (want[i] && trainer_.shown[i] && !windowOpen(w)) {
            trainer_.shown[i] = false;  // Circle: the player closed it
            if (i == 0) {
                session_.closeTrainer();
                playUiSound(UiSound::CharacterSheetClose);  // OnHide: CloseTrainer, igCharacterInfoClose
            } else {
                session_.cancelTalentWipe();
            }
        } else if (!want[i] && trainer_.shown[i]) {
            closeWindow(w);
            trainer_.shown[i] = false;
        }
    }
    if (trainer_.raisedCursor) {
        if (!uiMode_) {
            trainer_.raisedCursor = false;
        } else if (!trainer_.shown[0] && !trainer_.shown[1] && windows_.empty() && !settings_.isOpen()) {
            setUiMode(false);
            trainer_.raisedCursor = false;
            suppress_ = ~0u;  // the Circle / Cross that closed it must not walk
        }
    }
    // The frames' art only while their window is open.
    if (!windowOpen(kWinTrainer) && trArt_.ctTried) trArt_.releaseTrainer();
    if (!windowOpen(kWinTalents) && trArt_.talTried) trArt_.releaseTalents();
}

void WorldScreen::trainerError(const std::string& text) {
    combatError_ = text;
    combatErrorUntilMs_ = pp::logUptimeMs() + 2000u;
}

// --- Actions ----------------------------------------------------------------------------

bool WorldScreen::trainerWindowAction(uint32_t act) {
    if (act == kIdMenuTalents) {
        closeWindow(kWinMenu);
        if (windowOpen(kWinTalents)) {
            closeWindow(kWinTalents);
        } else {
            openTalentWindow();
        }
        return true;
    }
    if (act < 0x6701 || act > 0x67FF) return false;
    // A Cross held since before the window came up: its release is not a click.
    if (trainer_.guard) return true;
    if (act >= kIdBookTab0 && act < kIdBookTab0 + kMaxBookTabs) {  // spell book tabs
        // SpellBookSkillLineTab_OnClick - each tab keeps its page (SPELLBOOK_PAGENUMBERS).
        if (bookTab_ >= 0 && bookTab_ < kMaxBookTabs) bookPages_[bookTab_] = bookPage_;
        bookTab_ = static_cast<int>(act - kIdBookTab0);
        bookPage_ = bookPages_[bookTab_];
        bookVersion_ = ~0u;  // rebuild the list for the tab
        return true;
    }
    const uint8_t top = windows_.empty() ? 0 : windows_.back();
    if (top == kWinTalentWipe) {
        if (act == kIdWipeYes) session_.confirmTalentWipe();
        if (act == kIdWipeNo) session_.cancelTalentWipe();
        return true;
    }
    // Every open window takes the clicks - by the widget's id, not the top window
    // (was: the trainer was dead under the tutorial window that opens with it the first time).
    if (act < kIdTalTab0 && windowOpen(kWinTrainer)) {
        const auto& list = session_.getTrainerSpells();
        const bool wasOpen = trainer_.filterOpen;
        trainer_.filterOpen = false;  // a click anywhere closes the dropdown list
        if (act >= kIdTrFilter0 && act < kIdTrFilter0 + 3) {  // ClassTrainerFrameFilterDropDown_OnClick
            const int k = static_cast<int>(act - kIdTrFilter0);
            trainer_.filter[k] = !trainer_.filter[k];
            trainer_.listScroll = 0;
            trainer_.filterOpen = true;  // keepShownOnClick
            PSP_LOGI(pp::tag::Ctrl, "trainer_filter avail=%d unavail=%d used=%d", trainer_.filter[0] ? 1 : 0,
                     trainer_.filter[1] ? 1 : 0, trainer_.filter[2] ? 1 : 0);
            return true;
        }
        switch (act) {
            case kIdTrKnown: trainer_.filterOpen = !wasOpen; playUiSound(UiSound::ButtonClick); return true;
            case kIdTrPrev: trainer_.listScroll = std::max(0, trainer_.listScroll - 1); return true;
            case kIdTrNext: ++trainer_.listScroll; return true;  // clamped when drawn
            case kIdTrExit:
            case kIdTrClose: closeWindow(kWinTrainer); return true;  // HideParentPanel: updateTrainer closes the session's
            case kIdTrAll: {  // ClassTrainerCollapseAllButton_OnClick
                const auto& tabs = session_.getTrainerTabs();
                if (trainer_.collapsed.empty()) {
                    for (const auto& t : tabs) trainer_.collapsed.push_back(t.name);
                    trainer_.listScroll = 0;
                } else {
                    trainer_.collapsed.clear();
                }
                playUiSound(UiSound::ButtonClick);
                return true;
            }
            case kIdTrLearn: {  // ClassTrainerTrainButton_OnClick: BuyTrainerService
                const game::TrainerSpell* s = nullptr;
                for (const auto& sp : list.spells)
                    if (sp.spellId == trainer_.sel) s = &sp;
                if (!s) return true;
                const int type = serviceType(session_, *s);
                if (type == 0 && s->spellCost <= session_.getMoneyCopper()) {
                    session_.trainSpell(s->spellId);
                } else {  // PSP: the disabled button says why (the original: nothing)
                    trainerError(type == 2 ? std::string(CLIENT_STR(USED))
                                 : type == 1 ? trainerReqText(*s)
                                             : std::string(CLIENT_STR(ERR_NOT_ENOUGH_MONEY)));
                    PSP_LOGI(pp::tag::Net, "trainer_buy_blocked spell=%u type=%d cost=%u money=%u",
                             static_cast<unsigned>(s->spellId), type, static_cast<unsigned>(s->spellCost),
                             static_cast<unsigned>(session_.getMoneyCopper()));
                }
                return true;
            }
            default: break;
        }
        if (act >= kIdTrRow0 && act < kIdTrRow0 + 16) {
            const size_t v = act - kIdTrRow0;
            if (v >= trainer_.visibleRows.size()) return true;
            const int64_t r = trainer_.visibleRows[v];
            if (r >> 40) {  // a header: ClassTrainer_SetSelection -> Collapse / ExpandTrainerSkillLine
                const auto& tabs = session_.getTrainerTabs();
                const size_t t = static_cast<size_t>(r & 0xFFFF);
                if (t < tabs.size()) {
                    auto it = std::find(trainer_.collapsed.begin(), trainer_.collapsed.end(), tabs[t].name);
                    if (it == trainer_.collapsed.end()) trainer_.collapsed.push_back(tabs[t].name);
                    else trainer_.collapsed.erase(it);
                }
            } else {
                trainer_.sel = static_cast<uint32_t>(r);  // ClassTrainerSkillButton_OnClick
            }
            playUiSound(UiSound::ButtonClick);
        }
        return true;
    }
    if (act >= kIdTalTab0 && windowOpen(kWinTalents)) {
        if (act == kIdTalClose) {
            closeWindow(kWinTalents);
            return true;
        }
        if (act >= kIdTalTab0 && act < kIdTalTab0 + 3) {  // PlayerTalentFrameTab_OnClick
            selectGlyphTab(false);  // a tree tab hides GlyphFrame
            trainer_.talTab = static_cast<int>(act - kIdTalTab0);
            trainer_.talScrollU = 0.0f;
            trainer_.talSel = 0;
            playUiSound(UiSound::ButtonClick);  // igCharacterInfoTab
        }
        if (act == kIdTalUp) trainer_.talScrollU -= 63.0f;    // clamped when drawn
        if (act == kIdTalDown) trainer_.talScrollU += 63.0f;
        if (act >= kIdTal0 && act < kIdTal0 + 64) {
            const size_t i = act - kIdTal0;
            if (i < trainer_.talIds.size()) {
                const bool again = trainer_.talSel == trainer_.talIds[i];
                trainer_.talSel = trainer_.talIds[i];
                const bool petView = talentPetView();  // LearnTalent(tab, talent, isPet)
                const game::TalentEntry* t =
                    petView ? session_.petTalentEntry(trainer_.talSel) : session_.getTalentEntry(trainer_.talSel);
                // PlayerTalentFrameTalent_OnClick: LearnTalent (wowee.cfg talentclick=0: the second click).
                if (t && (trainer_.talentClick || again) && trainer_.talSelLearnable) {
                    if (petView) session_.learnPetTalent(t->talentId);
                    else session_.learnTalent(t->talentId, session_.getTalentRank(t->talentId) + 1u);
                }
            }
        }
        return true;
    }
    return true;
}

// Circle (the right click) on a trainer row: select it and learn it (PSP shortcut for the click and
// TRAIN); on a header the same as Cross. Other ids of the range: nothing (the original's right click on a
// talent only takes back preview points).
bool WorldScreen::trainerAltAction(uint32_t alt) {
    if (alt < 0x6701 || alt > 0x67FF || (alt >= kIdBookTab0 && alt < kIdBookTab0 + kMaxBookTabs)) return false;
    if (trainer_.guard) return true;
    if (alt >= kIdTrRow0 && alt < kIdTrRow0 + 16 && windowOpen(kWinTrainer)) {
        const size_t v = alt - kIdTrRow0;
        if (v < trainer_.visibleRows.size() && !(trainer_.visibleRows[v] >> 40)) {
            trainer_.sel = static_cast<uint32_t>(trainer_.visibleRows[v]);
            return trainerWindowAction(kIdTrLearn);
        }
        return trainerWindowAction(alt);
    }
    return true;
}

// The requirements of a service as text (the red line).
std::string WorldScreen::trainerReqText(const game::TrainerSpell& s) {
    const uint32_t level = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    if (s.reqLevel > level)  // REQUIRES_LABEL + TRAINER_REQ_LEVEL, as the details say it
        return std::string(CLIENT_STR(REQUIRES_LABEL)) + " " +
               pp::text::uiPlain(CLIENT_FMT(TRAINER_REQ_LEVEL, static_cast<unsigned>(s.reqLevel)));
    std::string out = CLIENT_STR(REQUIRES_LABEL);
    const char* sep = " ";
    for (uint32_t id : {s.chainNode1, s.chainNode2, s.chainNode3}) {
        if (id == 0 || session_.hasKnownSpell(id)) continue;
        out += sep;
        out += spellDb_.name(id);
        sep = ", ";
    }
    if (s.reqSkill != 0) {
        const std::string& skill = session_.getSkillLineName(s.reqSkill);
        char b[96];
        std::snprintf(b, sizeof(b), "%s%s (%u)", sep, skill.empty() ? "?" : skill.c_str(),
                      static_cast<unsigned>(s.reqSkillValue));
        out += b;
        sep = ", ";
    }
    return sep[0] == ' ' ? std::string(CLIENT_STR(UNAVAILABLE)) : out;
}

void WorldScreen::openTalentWindow() {
    const bool ok = session_.loadTalentDbc();
    openWindow(kWinTalents);
    session_.triggerTutorial(13);  // TUTORIAL_TRIGGER talents
    winFocus_.setFocus(kIdTal0);
    trainer_.talScroll = 0;
    trainer_.talScrollU = 0.0f;
    if (!trainer_.bgLoaded && ok && trainer_.talentArt) loadTalentArt();
    PSP_LOGI(pp::tag::Gfx, "talents_open data=%d tabs=%u points=%u", ok ? 1 : 0,
             static_cast<unsigned>(session_.getAllTalentTabs().size()),
             static_cast<unsigned>(session_.getUnspentTalentPoints()));
}

// ui.pak "tb_<background>" (build_ui_pak.py, TalentFrame backgrounds) of
// the player's tabs + "tal_rank" (TalentFrame-RankBorder). Optional: plain
// fills without them. One open of ui.pak, closed at once.
void WorldScreen::loadTalentArt() {
    trainer_.bgLoaded = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    int ok = 0, missing = 0;
    auto load = [&](const std::string& name, pp::Texture& tex) {
        const pp::PakEntry* e = pak.find(name.c_str());
        void* blob = e ? pak.load(e) : nullptr;
        if (blob && pp::textureFromBlob(blob, e->size, tex, true)) {
            tex.name = "talent";
            ++ok;
            return;
        }
        pp::pakFree(blob);
        ++missing;
    };
    for (const auto& [id, tab] : session_.getAllTalentTabs()) {
        std::string name = "tb_";
        for (char c : tab.backgroundFile) name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tab.orderIndex < 3) load(name, trainer_.bg[tab.orderIndex]);
    }
    load("tal_rank", trainer_.rankBorder);
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "talent_art ok=%d missing=%d", ok, missing);
}

WorldScreen::TrainerUi::~TrainerUi() {
    for (auto& t : bg) pp::textureRelease(t);
    pp::textureRelease(rankBorder);
}

// --- Drawing ----------------------------------------------------------------------------

bool WorldScreen::drawTrainerWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinTrainer && w != kWinTalents && w != kWinTalentWipe) return false;
    const bool top = !windows_.empty() && windows_.back() == w;
    // Every open window takes the focus (the covered parts not: FocusRing occluders).
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    // A press seen on one of our widgets arms the clicks (as npc_.guard).
    const uint32_t f = winFocus_.focused();
    if (f >= 0x6701 && f <= 0x67FF && winFocus_.isPressed(f)) trainer_.guard = false;
    if (!trainer_.shown[0] && !trainer_.shown[1]) trainer_.guard = false;  // opened by hand: no guard
    if (w == kWinTrainer) drawTrainerList(ui, focusable);
    else if (w == kWinTalents) drawTalents(ui, focusable);
    else drawTalentWipe(ui, top);
    return true;
}

void WorldScreen::drawTrainerList(UiRenderer& ui, bool top) {
    PROF_SCOPE("trainer");
    loadTrainerFrameArt();
    loadNpcArt(false);  // the panel buttons, the close button, the scroll arrows, the coins
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const float lh = static_cast<float>(fs.lineHeight());
    const auto& list = session_.getTrainerSpells();
    const auto& tabs = session_.getTrainerTabs();
    const uint32_t level = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    const uint32_t myMoney = static_cast<uint32_t>(session_.getMoneyCopper());
    const bool live = top && !trainer_.filterOpen;  // the open dropdown list takes the focus

    // ClassTrainerFramePortrait (60 x 60 at 7,-6) under the frame, then the frame.
    const Rect pr = FR(7, 6, 60, 60);
    if (const pp::Texture* face = portraitOf(list.trainerGuid)) ui.image(*face, pr);
    else ui.fill(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    if (trArt_.ctFrame.pixels) ui.image(trArt_.ctFrame, Rect{kOx, kOy, kCtW, kCtH});
    else kit_.panel(ui, Rect{kOx + 6, kOy + 4, kCtW - 20, kCtH - 8});

    // ClassTrainerNameText (TOP 0,-17, GameFontNormal), ClassTrainerGreetingText (TOPLEFT 76,-38, 260 x 30).
    {
        UnitView npc;
        const std::string name = unitView(list.trainerGuid, npc) ? npc.name : std::string();
        const Rect nr = FR(76, 13, 242, 0);
        if (static_cast<float>(fn.measure(name.c_str())) <= nr.w)
            ui.textShadow(fn, PX(197), nr.y, name.c_str(), kColGold, Align::Center);
        else
            ui.textMarquee(fs, Rect{nr.x, nr.y, nr.w, lh}, name.c_str(), kColGold);
        if (!list.greeting.empty()) {
            const auto lines = wrapQuestText(fs, list.greeting, 260 * kSX);
            if (lines.size() <= 1) ui.textShadow(fs, PX(76), PY(38), list.greeting.c_str(), kColWhite);
            else ui.textMarquee(fs, Rect{PX(76), PY(38), 260 * kSX, lh}, list.greeting.c_str(), kColWhite);
        }
    }
    // ClassTrainerFrameCloseButton (UIPanelCloseButton, TOPRIGHT -29,-8).
    {
        const Rect cr{PX(339) - 9, PY(24) - 9, 18, 18};
        const bool f = top && winFocus_.addStop(kIdTrClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdTrClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // --- GetTrainerServiceInfo: the headers (skill lines) and the services that pass the filter ---------
    struct Row {
        bool header;
        size_t tab;
        const game::TrainerSpell* sp;
    };
    std::vector<Row> rows;
    for (size_t t = 0; t < tabs.size(); ++t) {
        std::vector<const game::TrainerSpell*> vis;
        for (const game::TrainerSpell* sp : tabs[t].spells)
            if (trainer_.filter[serviceType(session_, *sp)]) vis.push_back(sp);
        if (vis.empty()) continue;
        rows.push_back(Row{true, t, nullptr});
        if (std::find(trainer_.collapsed.begin(), trainer_.collapsed.end(), tabs[t].name) != trainer_.collapsed.end())
            continue;
        for (const game::TrainerSpell* sp : vis) rows.push_back(Row{false, t, sp});
    }
    // ClassTrainer_SelectFirstLearnableSkill (on show and TRAINER_UPDATE): keep a shown selection, else the
    // first available service, else the first service; the list scrolled to it.
    if (trainer_.listVersion != session_.trainerVersion()) {
        trainer_.listVersion = session_.trainerVersion();
        int at = -1;
        for (size_t i = 0; i < rows.size(); ++i)
            if (!rows[i].header && rows[i].sp->spellId == trainer_.sel) at = static_cast<int>(i);
        if (at < 0) {
            for (size_t i = 0; i < rows.size() && at < 0; ++i)
                if (!rows[i].header && serviceType(session_, *rows[i].sp) == 0) at = static_cast<int>(i);
            for (size_t i = 0; i < rows.size() && at < 0; ++i)
                if (!rows[i].header) at = static_cast<int>(i);
            trainer_.sel = at >= 0 ? rows[static_cast<size_t>(at)].sp->spellId : 0u;
            if (at >= 2) trainer_.listScroll = at - 1;
        }
        PSP_LOGI(pp::tag::Ctrl, "trainer_show rows=%u sel=%u level=%u money=%u", static_cast<unsigned>(rows.size()),
                 static_cast<unsigned>(trainer_.sel), static_cast<unsigned>(level), static_cast<unsigned>(myMoney));
    }
    const game::TrainerSpell* selected = nullptr;
    for (const Row& r : rows)
        if (!r.header && r.sp->spellId == trainer_.sel) selected = r.sp;

    // --- ALL (ClassTrainerCollapseAllButton) -------------------------------------------------------------
    {
        bool anyExpanded = false;
        for (const Row& r : rows)
            if (r.header && std::find(trainer_.collapsed.begin(), trainer_.collapsed.end(), tabs[r.tab].name) ==
                                trainer_.collapsed.end())
                anyExpanded = true;
        const Rect b = FR(23, 72, 40, 22);
        const bool f = live && !rows.empty() && winFocus_.addStop(kIdTrAll, b);
        const pp::Texture& t = anyExpanded ? trArt_.minus : trArt_.plus;
        const Rect ic{PX(26), b.y + (b.h - 10) * 0.5f, 10, 10};
        if (t.pixels) ui.image(t, ic, rows.empty() ? 0xFF808080u : 0xFFFFFFFFu);
        if (f && trArt_.plusHl.pixels) ui.image(trArt_.plusHl, ic, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(fs, PX(44), b.y + (b.h - lh) * 0.5f, CLIENT_STR(ALL), rows.empty() ? kColGreyFont : f ? kColWhite : kColGold);
    }

    // --- The list --------------------------------------------------------------------------------------
    const int total = static_cast<int>(rows.size());
    trainer_.listScroll = std::clamp(trainer_.listScroll, 0, std::max(0, total - kRows));
    const uint32_t focused = winFocus_.focused();
    // The list follows the focus (as the quest log): the focus on the first / last row scrolls one row.
    if (live && focused >= kIdTrRow0 && focused < kIdTrRow0 + static_cast<uint32_t>(kRows)) {
        const int vi = static_cast<int>(focused - kIdTrRow0);
        if (vi == kRows - 1 && trainer_.listScroll + kRows < total) {
            ++trainer_.listScroll;
            winFocus_.setFocus(focused - 1);
        } else if (vi == 0 && trainer_.listScroll > 0) {
            --trainer_.listScroll;
            winFocus_.setFocus(focused + 1);
        }
    }
    trainer_.visibleRows.clear();
    const bool scrollShown = total > kRows;  // FauxScrollFrame_Update: the bar only when needed
    const float rowW = (scrollShown ? 293.0f : 313.0f);
    for (int v = 0; v < kRows; ++v) {
        const int i = trainer_.listScroll + v;
        if (i >= total) break;
        const Row& r = rows[static_cast<size_t>(i)];
        trainer_.visibleRows.push_back(r.header ? (static_cast<int64_t>(1) << 40) | static_cast<int64_t>(r.tab)
                                                : static_cast<int64_t>(r.sp->spellId));
        const Rect rr = FR(22, kListTopU + kRowU * static_cast<float>(v), rowW, kRowU);
        const uint32_t id = kIdTrRow0 + static_cast<uint32_t>(v);
        const bool f = live && winFocus_.addStop(id, rr);
        const float ty = rr.y + (rr.h - lh) * 0.5f;
        const Rect ic{PX(25), rr.y + (rr.h - 10) * 0.5f, 10, 10};
        if (r.header) {  // GameFontNormalLeft, UI-MinusButton / UI-PlusButton, UI-PlusButton-Hilight
            const bool col = std::find(trainer_.collapsed.begin(), trainer_.collapsed.end(), tabs[r.tab].name) !=
                             trainer_.collapsed.end();
            const pp::Texture& t = col ? trArt_.plus : trArt_.minus;
            if (t.pixels) ui.image(t, ic);
            if (f && trArt_.plusHl.pixels) ui.image(trArt_.plusHl, ic, 0xFFFFFFFFu, Blend::Add);
            ui.pushClip(Rect{PX(44), rr.y, PX(22 + rowW) - PX(44), rr.h});
            ui.textShadow(fs, PX(44), ty, tabs[r.tab].name.c_str(), f ? kColWhite : kColGold);
            ui.popClip();
            continue;
        }
        const game::TrainerSpell& sp = *r.sp;
        const int type = serviceType(session_, sp);
        const bool sel = sp.spellId == trainer_.sel;
        if (sel && trArt_.hl.pixels) ui.image(trArt_.hl, rr, highlightColor(type), Blend::Add);  // LockHighlight
        const pp::SpellInfo* info = spellDb_.get(sp.spellId);
        const std::string name = info && !info->name.empty() ? info->name : std::string(CLIENT_STR(UNKNOWN));
        const bool white = sel || f;  // HighlightFont (GameFontHighlightLeft) on the locked / hovered button
        float textRight = PX(22 + rowW - 2);
        if (info && !info->rank.empty()) {  // $parentSubText: PARENS_TEMPLATE, RIGHT -2
            const std::string sub = "(" + info->rank + ")";
            ui.textShadow(fs, textRight, ty, sub.c_str(), white ? kColWhite : subTextColor(type), Align::Right);
            textRight -= static_cast<float>(fs.measure(sub.c_str())) + 3.0f;
        }
        const Rect tr{PX(44), rr.y, std::max(0.0f, textRight - PX(44)), rr.h};
        const std::string text = "  " + name;  // skillButton:SetText("  "..serviceName)
        if (f && static_cast<float>(fs.measure(text.c_str())) > tr.w) {
            ui.textMarquee(fs, Rect{tr.x, ty, tr.w, lh}, text.c_str(), kColWhite);
        } else {
            ui.pushClip(tr);
            ui.textShadow(fs, tr.x, ty, text.c_str(), white ? kColWhite : serviceColor(type));
            ui.popClip();
        }
    }
    if (total == 0)  // PSP: the empty list says so (the original shows nothing)
        ui.textShadow(fs, PX(44), PY(kListTopU) + 2, tabs.empty() ? OWN_STR(TRAINER_NOTHING_TO_TEACH) : OWN_STR(LIST_ALL_FILTERED),
                      kColGreyFont);
    // The scroll bar's arrows (ClassTrainerListScrollFrame, the art is baked).
    if (scrollShown) {
        const bool canUp = trainer_.listScroll > 0, canDn = trainer_.listScroll + kRows < total;
        const Rect up{PX(321), PY(98), 11, 11}, dn{PX(321), PY(280) - 11, 11, 11};
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (live && canUp && winFocus_.addStop(kIdTrPrev, up)) ui.fill(up, 0x40FFFFFFu);
        if (live && canDn && winFocus_.addStop(kIdTrNext, dn)) ui.fill(dn, 0x40FFFFFFu);
    }

    // --- The details (ClassTrainerDetailScrollChildFrame at 21,-288) -------------------------------------
    const bool canTrain = selected && serviceType(session_, *selected) == 0 && selected->spellCost <= myMoney;
    if (selected) {
        const int type = serviceType(session_, *selected);
        // ClassTrainerSkillIcon (37 x 37 at 5,-4; UI-EmptySlot 64 x 64 at -13,13).
        const float side = 37.0f * kSY;
        const Rect icon{PX(26) + (37 * kSX - side) * 0.5f, PY(292), side, side};
        if (trArt_.slot.pixels) {
            const float c = side * 64.0f / 37.0f;
            ui.image(trArt_.slot, Rect{icon.x + side * 0.5f - c * 0.5f, icon.y + side * 0.5f - c * 0.5f, c, c});
        }
        if (const pp::Texture* t = spellDb_.icon(selected->spellId, spellFrame_)) ui.image(*t, icon);
        else ui.fill(icon, 0xFF303030u);
        if (top && winFocus_.addStop(kIdTrIcon, icon)) {  // OnEnter: GameTooltip:SetTrainerService
            if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, icon, 0xFFFFFFFFu, Blend::Add);
            tipSpellId_ = selected->spellId;
            tipSpellRect_ = Rect{icon.x + icon.w + ui.offsetX(), icon.y + ui.offsetY(), icon.w, icon.h};
        }
        // ClassTrainerSkillName (46,-2, GameFontNormal) + ClassTrainerSubSkillName "(rank)".
        const pp::SpellInfo* info = spellDb_.get(selected->spellId);
        std::string name = info && !info->name.empty() ? info->name : std::string(CLIENT_STR(UNKNOWN));
        if (info && !info->rank.empty()) name += " (" + info->rank + ")";
        const float nx = PX(67), nw = 244 * kSX;
        const Font& nf = static_cast<float>(fn.measure(name.c_str())) <= nw ? fn : fs;
        float y = PY(290);
        ui.pushClip(Rect{nx, y - 2, nw, static_cast<float>(nf.lineHeight()) + 4});
        ui.textShadow(nf, nx, y, name.c_str(), kColGold);
        ui.popClip();
        y += static_cast<float>(nf.lineHeight());
        // ClassTrainerSkillRequirements (GameFontHighlightSmall): REQUIRES_LABEL + TRAINER_REQ_LEVEL(_RED),
        // TRAINER_REQ_SKILL_RANK(_RED), TRAINER_REQ_ABILITY(_RED) joined by ", ".
        std::vector<Seg> req;
        auto sep = [&]() { req.push_back(Seg{req.size() > 1 ? ", " : " ", kColWhite}); };
        req.push_back(Seg{CLIENT_STR(REQUIRES_LABEL), kColWhite});
        if (selected->reqLevel > 1) {
            sep();
            appendColorSegs(req,
                            level >= selected->reqLevel
                                ? CLIENT_FMT(TRAINER_REQ_LEVEL, static_cast<unsigned>(selected->reqLevel))
                                : CLIENT_FMT(TRAINER_REQ_LEVEL_RED, static_cast<unsigned>(selected->reqLevel)),
                            kColWhite);
        }
        if (selected->reqSkill != 0) {
            sep();
            const std::string& skill = session_.getSkillLineName(selected->reqSkill);
            // PSP: no skill values in the session - unmet when the service is unavailable at the level.
            const bool met = !(type == 1 && level >= selected->reqLevel);
            const std::string sk = skill.empty() ? std::string("?") : skill;
            const unsigned value = static_cast<unsigned>(selected->reqSkillValue);
            appendColorSegs(req, met ? CLIENT_FMT(TRAINER_REQ_SKILL_RANK, sk, value)
                                     : CLIENT_FMT(TRAINER_REQ_SKILL_RANK_RED, sk, value),
                            kColWhite);
        }
        for (uint32_t ab : {selected->chainNode1, selected->chainNode2, selected->chainNode3}) {
            if (ab == 0) continue;
            sep();
            const pp::SpellInfo* ai = spellDb_.get(ab);
            std::string an = ai && !ai->name.empty() ? ai->name : std::string(CLIENT_STR(UNKNOWN));
            if (ai && !ai->rank.empty()) an += " (" + ai->rank + ")";
            req.push_back(Seg{an, session_.hasKnownSpell(ab) ? kColWhite : kColRedCode});
        }
        if (req.size() > 1) drawSegs(ui, fs, nx, y, nw, req, 2);
        // ClassTrainerCostLabel (5,-50, GameFontNormalSmall) + ClassTrainerDetailMoneyFrame (red when short).
        if (selected->spellCost > 0) {
            const float cy = PY(338);
            ui.textShadow(fs, PX(26), cy, CLIENT_STR(COSTS_LABEL), kColGold);
            const float cx = PX(26) + static_cast<float>(fs.measure(CLIENT_STR(COSTS_LABEL))) + 5 * kSX + 2;
            drawCoins(ui, fs, npcArt_.money, cx, cy, selected->spellCost, selected->spellCost <= myMoney ? kColWhite
                                                                                                       : kColRedCode);
        }
        // ClassTrainerSkillDescription (TOPLEFT at the cost label's BOTTOMLEFT 0,-10, or in its place).
        const float descY = selected->spellCost > 0 ? PY(338) + static_cast<float>(fs.lineHeight()) + 10 * kSY : PY(338);
        drawTrainerDescription(ui, selected->spellId, Rect{PX(26), descY, 290 * kSX, PY(420) - 8.0f - descY});
    }

    // ClassTrainerMoneyFrame (BOTTOMRIGHT at BOTTOMLEFT 180,86): the player's money.
    drawMoneyFrame(ui, PX(180), PY(426) - 12.0f, myMoney, true);
    // ClassTrainerTrainButton (CENTER 224,-420) TRAIN / ClassTrainerCancelButton (305,-420) EXIT.
    {
        const float bh = 14.0f, cy = PY(420);
        const Rect tb{PX(224) - 40 * kSX, cy - bh * 0.5f, 80 * kSX, bh};
        const Rect eb{PX(305) - 40 * kSX, cy - bh * 0.5f, 80 * kSX, bh};
        // PSP: the disabled button keeps a focus stop - Cross on it says why on the red line.
        if (!canTrain && selected && live) winFocus_.addStop(kIdTrLearn, tb);
        drawPanelButton(ui, kIdTrLearn, tb, CLIENT_STR(TRAIN), canTrain, live);
        drawPanelButton(ui, kIdTrExit, eb, CLIENT_STR(EXIT), true, live);
    }

    // --- ClassTrainerFrameFilterDropDown FILTER (TOPRIGHT -26,-64; the label frame is baked) -----------
    {
        const Rect btn{PX(318), PY(65), 24 * kSX, 24 * kSY};
        const Rect hit{PX(203), PY(62), PX(342) - PX(203), 30 * kSY};
        const bool f = top && winFocus_.addStop(kIdTrKnown, hit);
        if (npcArt_.scrDown.pixels) ui.image(npcArt_.scrDown, btn);
        if (f) ui.fill(btn, 0x40FFFFFFu);
        ui.textShadow(fs, PX(315), PY(77) - lh * 0.5f, CLIENT_STR(FILTER), kColWhite, Align::Right);
        if (trainer_.filterOpen) {  // DropDownList1: the three check items under the button
            const char* kItems[3] = {CLIENT_STR(AVAILABLE), CLIENT_STR(UNAVAILABLE), CLIENT_STR(USED)};
            static const uint32_t kCols[3] = {kColGreenCode, kColRedCode, kColGreyCode};
            float w = 0;
            for (const char* s : kItems) w = std::max(w, static_cast<float>(fs.measure(s)));
            const float iw = w + 30, ih = lh + 2;
            const Rect lr{btn.x + btn.w - iw - 8, btn.y + btn.h + 2, iw + 8, ih * 3 + 10};
            if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{lr.x + 3, lr.y + 3, lr.w - 6, lr.h - 6}, 0xF0FFFFFFu);
            else ui.fill(lr, 0xF0100000u);
            if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), lr);
            for (int k = 0; k < 3; ++k) {
                const Rect ir{lr.x + 4, lr.y + 5 + ih * static_cast<float>(k), lr.w - 8, ih};
                const bool fi = top && winFocus_.addStop(kIdTrFilter0 + static_cast<uint32_t>(k), ir);
                if (fi && trArt_.hl.pixels) ui.image(trArt_.hl, ir, 0xFFFFFFFFu, Blend::Add);
                if (trainer_.filter[k] && trArt_.check.pixels) ui.image(trArt_.check, Rect{ir.x + 2, ir.y + 1, 12, 12});
                ui.textShadow(fs, ir.x + 20, ir.y + 1, kItems[k], kCols[k]);
            }
        }
    }
}

void WorldScreen::drawTalents(UiRenderer& ui, bool top) {
    PROF_SCOPE("talents");
    loadTalentFrameArt();
    loadNpcArt(false);  // the close button, the scroll arrows
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight());
    char buf[160];

    // PlayerTalentFramePortrait (60 x 60 at 7,-6): the player (SetPortraitTexture "player": the race portrait).
    // PlayerTalentFrame.unit = "pet" in the pet's spec - the pet's portrait.
    if (talentPetView() && petPortraitTex_.pixels) {
        ui.imageCircle(petPortraitTex_, FR(7, 6, 60, 60));
    } else {
        const Rect pr = FR(7, 6, 60, 60);
        const game::Character* me = session_.activeCharacter();
        const int race = me ? static_cast<int>(me->race) : 0, sex = me && me->gender == game::Gender::FEMALE ? 1 : 0;
        if (race > 0 && race < 12 && ufArt_.portrait[race][sex].pixels) ui.image(ufArt_.portrait[race][sex], pr);
        else ui.fill(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    }
    if (trArt_.talFrame.pixels) ui.image(trArt_.talFrame, Rect{kOx, kOy, kTalW, kTalH});
    else kit_.panel(ui, Rect{kOx + 6, kOy + 4, kTalW - 20, kTalH - 30});
    // PlayerTalentFrameTitleText TALENTS (TOP 0,-18), the close button (CENTER at TOPRIGHT -44,-25).
    ui.textShadow(fn, PX(197), PY(18), CLIENT_STR(TALENTS), kColGold, Align::Center);
    {
        const Rect cr{PX(340) - 9, PY(25) - 9, 18, 18};
        const bool f = top && winFocus_.addStop(kIdTalClose, cr);
        const pp::Texture& t = f && winFocus_.isPressed(kIdTalClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                 : npcArt_.close;
        if (t.pixels) ui.image(t, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    const Rect pane = FR(23, 77, 296, 328);  // PlayerTalentFrameScrollFrame
    if (!session_.loadTalentDbc()) {
        ui.textShadow(fs, pane.x + 4, pane.y + 4,
                      session_.talentDataMissing() ? OWN_STR(NO_TALENT_DATA) : OWN_STR(TALENTS_LOADING),
                      kColRed);
        return;
    }
    // PlayerSpecTab1 / PlayerSpecTab3 ("petspec1": TOPRIGHT -32,-65, then 32 + 22 + 32 + 39 lower) and the
    // pet's tree when it is selected (GetTalentTabInfo / GetTalentInfo / LearnTalent with isPet).
    drawTalentSpecTabs(ui, top, FR(384 - 32, 65, 32, 32), FR(384 - 32, 65 + 32 + 22 + 32 + 39, 32, 32));
    const bool petView = talentPetView();
    auto rankOf = [&](uint32_t id) { return petView ? session_.petTalentRank(id) : session_.getTalentRank(id); };
    auto entryOf = [&](uint32_t id) { return petView ? session_.petTalentEntry(id) : session_.getTalentEntry(id); };
    // Tabs in TalentTab.OrderIndex order (upstream classTalentTabs).
    std::vector<const game::TalentTabEntry*> tabs;
    if (petView) tabs.push_back(session_.petTalentTab());
    else
        for (const auto& [id, t] : session_.getAllTalentTabs()) tabs.push_back(&t);
    std::sort(tabs.begin(), tabs.end(), [](const auto* a, const auto* b) { return a->orderIndex < b->orderIndex; });
    if (tabs.empty()) return;
    if (trainer_.talTab >= static_cast<int>(tabs.size())) trainer_.talTab = 0;
    // PlayerTalentFrameTab1..3 (BOTTOMLEFT 15,46; each LEFT of the last RIGHT -15), shaved by 10 units from the
    // largest while their sum is not under the frame's 384 (PlayerTalentFrame_UpdateTabs). + the
    // glyph tab (GLYPH_TALENT_TAB 4: PlayerGlyphTabTemplate GLYPHS) after them while glyphTabShown().
    auto drawTreeTabs = [&]() {
        const size_t n = std::min<size_t>(tabs.size(), 3);
        const float cap = 20.0f * kSX, th = 32.0f * kSY;
        const bool glyphTab = glyphTabShown(), glyphSel = glyphTab && glyphTabSelected();
        float w[4] = {};
        float sum = 0;
        for (size_t i = 0; i < n; ++i) {
            w[i] = std::max(60.0f, static_cast<float>(fs.measure(tabs[i]->name.c_str())) + 2.0f * cap + 4.0f);
            sum += w[i];
        }
        const size_t nAll = n + (glyphTab ? 1 : 0);
        if (glyphTab) {
            w[n] = std::max(60.0f, static_cast<float>(fs.measure(CLIENT_STR(GLYPHS))) + 2.0f * cap + 4.0f);
            sum += w[n];
        }
        while (sum >= 384.0f * kSX && sum > 0) {
            size_t big = 0;
            for (size_t i = 1; i < nAll; ++i)
                if (w[i] > w[big]) big = i;
            w[big] -= 10.0f * kSX;
            sum -= 10.0f * kSX;
        }
        float x = PX(15);
        const float y0 = PY(512 - 46) - th;
        for (size_t i = 0; i < n; ++i) {
            const bool sel = static_cast<int>(i) == trainer_.talTab && !glyphSel;
            const float ty = y0 - (sel ? 5.0f * kSY : 0.0f);
            const pp::Texture& t = tabArt_[sel ? 0 : 1];
            if (t.pixels) {
                const float tw = static_cast<float>(t.uw), tth = static_cast<float>(t.uh), ct = tw * 20.0f / 128.0f;
                ui.image(t, Rect{x, ty, cap, th}, SrcRect{0, 0, ct, tth});
                ui.image(t, Rect{x + cap, ty, w[i] - 2 * cap, th}, SrcRect{ct, 0, tw - 2 * ct, tth});
                ui.image(t, Rect{x + w[i] - cap, ty, cap, th}, SrcRect{tw - ct, 0, ct, tth});
            }
            const Rect hit{x + 4, ty + 2, w[i] - 8, th - 4};
            const bool f = top && winFocus_.addStop(kIdTalTab0 + static_cast<uint32_t>(i), hit);
            const char* label = tabs[i]->name.c_str();
            const float ly = ty + (th - lh) * 0.5f - 1.0f;
            const Rect tr{x + cap - 4, ly, w[i] - 2 * cap + 8, lh};
            if (static_cast<float>(fs.measure(label)) > tr.w) {  // ellipsized: the whole name runs (OnEnter tooltip)
                ui.textMarquee(fs, tr, label, sel ? kColWhite : kColGold);
            } else {
                ui.textShadow(fs, x + w[i] * 0.5f, ly, label, sel ? kColWhite : kColGold, Align::Center);
            }
            if (f) ui.fill(hit, 0x30FFFFFFu);
            x += w[i] - 15.0f * kSX;
        }
        if (glyphTab) drawGlyphTabButton(ui, x, y0, w[n], th, top);
    };
    // GLYPH_TALENT_TAB selected - GlyphFrame over the tree and its points bar
    // (PlayerTalentFrame_ShowGlyphFrame), the tabs under it.
    if (!petView && glyphTabSelected()) {
        drawGlyphFrame(ui, top);
        drawTreeTabs();
        return;
    }
    const int unspent = petView ? static_cast<int>(session_.petUnspentTalents()) : session_.getUnspentTalentPoints();
    const auto& learned = petView ? session_.petLearnedTalents() : session_.getLearnedTalents();
    auto spentIn = [&](uint32_t tabId) {
        int n = 0;
        for (const auto& [tid, rank] : learned)
            if (const game::TalentEntry* e = entryOf(tid); e && e->tabId == tabId) n += rank;
        return n;
    };
    const game::TalentTabEntry& tab = *tabs[static_cast<size_t>(trainer_.talTab)];
    const int tabSpent = spentIn(tab.tabId);

    // The tab's talents by tier / column (GetTalentInfo order).
    std::vector<const game::TalentEntry*> tal;
    if (petView)
        for (const auto& t : session_.petTalents()) tal.push_back(&t);
    else
        for (const auto& [id, t] : session_.getAllTalents())
            if (t.tabId == tab.tabId) tal.push_back(&t);
    std::sort(tal.begin(), tal.end(), [](const auto* a, const auto* b) {
        return a->row != b->row ? a->row < b->row : a->column < b->column;
    });
    if (tal.size() > 64) tal.resize(64);
    int tiers = 0;
    for (const auto* t : tal) tiers = std::max(tiers, t->row + 1);

    // The scroll child: buttons at (35 + 63 * column, 20 + 63 * tier); the frame scrolls in units.
    const float contentH = 20.0f + 63.0f * static_cast<float>(std::max(0, tiers - 1)) + 37.0f + 20.0f;
    const float maxScroll = std::max(0.0f, contentH - 328.0f);
    const uint32_t focused = winFocus_.focused();
    // The scroll follows the focus: a focused talent on the bottom / top visible tier with more beyond
    // scrolls one tier (PSP: the D-pad has no other way into the hidden tiers).
    if (top && focused >= kIdTal0 && focused < kIdTal0 + tal.size() && focused != trainer_.talFocus) {
        const auto* t = tal[focused - kIdTal0];
        const float y = 20.0f + 63.0f * static_cast<float>(t->row) - trainer_.talScrollU;
        if (y + 37.0f + 63.0f > 328.0f && trainer_.talScrollU < maxScroll) trainer_.talScrollU += 63.0f;
        else if (y - 63.0f < 0.0f && trainer_.talScrollU > 0.0f) trainer_.talScrollU -= 63.0f;
    }
    trainer_.talFocus = focused;
    trainer_.talScrollU = std::clamp(trainer_.talScrollU, 0.0f, maxScroll);
    const float scroll = trainer_.talScrollU;
    auto childX = [&](float x) { return PX(23 + x); };
    auto childY = [&](float y) { return PY(77 + y - scroll); };

    // The tree's background (PlayerTalentFrameBackgroundTopLeft..BottomRight: 300 x 331 of the 320 x 384
    // composite baked as tb_*), static under the scrolling child.
    const pp::Texture* bgTex = petView ? petTalentBg()  // ui.pak tb_hunterpet*
                                       : tab.orderIndex < 3 && trainer_.bg[tab.orderIndex].pixels ? &trainer_.bg[tab.orderIndex]
                                                                                                  : nullptr;
    if (bgTex) {
        const pp::Texture& bg = *bgTex;
        ui.image(bg, pane, SrcRect{0, 0, bg.uw * 300.0f / 320.0f, bg.uh * 331.0f / 384.0f});
    } else {
        ui.fill(pane, 0xFF101010u);
    }

    // TalentFrame_Update: each button's state and TalentFrame_SetPrereqs -> TalentFrame_DrawLines.
    BranchGrid grid;
    struct State {
        uint8_t rank;
        bool reqMet, meetsPrereq, tierOpen;
    };
    std::vector<State> st(tal.size());
    auto prereqOk = [&](const game::TalentEntry& t, int p) {
        return rankOf(t.prereqTalent[p]) >= t.prereqRank[p] + 1;  // the DBC counts from zero
    };
    for (size_t i = 0; i < tal.size(); ++i) {
        const game::TalentEntry& t = *tal[i];
        if (t.row >= kTiers || t.column >= kCols) continue;
        grid[t.row][t.column].id = static_cast<int>(i) + 1;
        const uint8_t rank = rankOf(t.talentId);
        const bool forceDesaturated = unspent <= 0 && rank == 0;
        // PET_TALENTS_PER_TIER 3 for the pet (TalentFrameBase.lua), PLAYER_TALENTS_PER_TIER 5 otherwise
        const bool tierOpen = static_cast<int>(t.row) * (petView ? 3 : 5) <= tabSpent;
        bool met = tierOpen && !forceDesaturated, meets = true;
        for (int p = 0; p < 3; ++p) {
            if (t.prereqTalent[p] == 0) continue;
            const game::TalentEntry* pre = entryOf(t.prereqTalent[p]);
            if (!pre) continue;
            const bool learnable = prereqOk(t, p);
            if (forceDesaturated || !learnable) met = false;
            if (!learnable) meets = false;
            drawLines(grid, t.row, t.column, pre->row, pre->column, met);
        }
        st[i] = State{rank, met, meets, tierOpen};
    }
    // The branch pass (TalentFrame_Update's second half).
    std::vector<Piece> pieces;
    auto branch = [&](Branch b, int v, float x, float y) {
        if (v != 0) pieces.push_back(Piece{kBranchTC[b][v > 0 ? 0 : 1], x, y, false});
    };
    auto arrow = [&](Arrow a, int v, float x, float y) {
        if (v != 0) pieces.push_back(Piece{kArrowTC[a][v > 0 ? 0 : 1], x, y, true});
    };
    bool ignoreUp = false;
    for (int i = 0; i < kTiers; ++i) {
        for (int j = 0; j < kCols; ++j) {
            const BranchNode& node = grid[i][j];
            const float xo = static_cast<float>(j) * 63.0f + 35.0f + 2.0f;  // xOffset
            const float yo = static_cast<float>(i) * 63.0f + 20.0f + 2.0f;  // -yOffset (y down)
            if (node.id) {
                if (node.up) {
                    if (!ignoreUp) branch(kUp, node.up, xo, yo - 32.0f);
                    else ignoreUp = false;
                }
                if (node.down) branch(kDown, node.down, xo, yo + 32.0f - 1.0f);
                if (node.left) branch(kLeft, node.left, xo - 32.0f, yo);
                if (node.right) {
                    const BranchNode* temp = j + 1 < kCols ? &grid[i][j + 1] : nullptr;
                    if (temp && temp->left && temp->down < 0) branch(kRight, temp->down, xo + 32.0f, yo);
                    else branch(kRight, node.right, xo + 32.0f + 1.0f, yo);
                }
                arrow(kArrowRight, node.rightArrow, xo + 16.0f + 5.0f, yo);
                arrow(kArrowLeft, node.leftArrow, xo - 16.0f - 5.0f, yo);
                arrow(kArrowTop, node.topArrow, xo, yo - 16.0f - 5.0f);
            } else if (node.up && node.left && node.right) {
                branch(kTUp, node.up, xo, yo);
            } else if (node.down && node.left && node.right) {
                branch(kTDown, node.down, xo, yo);
            } else if (node.left && node.down) {
                branch(kTopRight, node.left, xo, yo);
                branch(kDown, node.down, xo, yo + 32.0f);
            } else if (node.left && node.up) {
                branch(kBottomRight, node.left, xo, yo);
            } else if (node.left && node.right) {
                branch(kRight, node.right, xo + 32.0f, yo);
                branch(kLeft, node.left, xo + 1.0f, yo);
            } else if (node.right && node.down) {
                branch(kTopLeft, node.right, xo, yo);
                branch(kDown, node.down, xo, yo + 32.0f);
            } else if (node.right && node.up) {
                branch(kBottomLeft, node.right, xo, yo);
            } else if (node.up && node.down) {
                branch(kUp, node.up, xo, yo);
                branch(kDown, node.down, xo, yo + 32.0f);
                ignoreUp = true;
            }
        }
    }
    ui.pushClip(pane);
    auto drawPiece = [&](const Piece& p) {
        const pp::Texture& t = p.arrow ? trArt_.arrows : trArt_.branches;
        const Rect r{childX(p.x), childY(p.y), 32 * kSX, 32 * kSY};
        if (t.pixels) {
            ui.image(t, r, SrcRect{p.tc.l * t.uw, p.tc.t * t.uh, (p.tc.r - p.tc.l) * t.uw, (p.tc.b - p.tc.t) * t.uh});
        } else if (!p.arrow) {  // no art baked: a plain line in the middle of the piece
            const bool lit = p.tc.t < 0.25f;
            ui.fill(Rect{r.x + r.w * 0.5f - 1, r.y, 2, r.h}, lit ? 0xFF00C8FFu : 0xFF707070u);
        }
    };
    for (const Piece& p : pieces)
        if (!p.arrow) drawPiece(p);  // BACKGROUND layer of the scroll child, under the buttons

    // The buttons (PlayerTalentButtonTemplate: ItemButtonTemplate 37 x 37 + Slot + RankBorder + Rank).
    trainer_.talIds.clear();
    trainer_.talSelLearnable = false;
    const game::TalentEntry* tipTalent = nullptr;
    pp::TalentLearnCheck tipCheck;
    Rect tipRect{};
    const float side = 37.0f * kSY;
    for (size_t i = 0; i < tal.size(); ++i) {
        const game::TalentEntry& t = *tal[i];
        trainer_.talIds.push_back(t.talentId);
        const float bx = childX(35.0f + 63.0f * static_cast<float>(t.column)) + (37 * kSX - side) * 0.5f;
        const float by = childY(20.0f + 63.0f * static_cast<float>(t.row));
        const Rect b{bx, by, side, side};
        if (by + side < pane.y || by > pane.y + pane.h) continue;
        const bool inside = by >= pane.y - 1 && by + side <= pane.y + pane.h + 1;
        const bool f = top && inside && winFocus_.addStop(kIdTal0 + static_cast<uint32_t>(i), b);
        const State& s = st[i];
        uint8_t have[3] = {}, need[3] = {};
        for (int p = 0; p < 3; ++p) {
            if (t.prereqTalent[p] == 0) continue;
            need[p] = static_cast<uint8_t>(t.prereqRank[p] + 1);
            have[p] = rankOf(t.prereqTalent[p]);
        }
        const pp::TalentLearnCheck c =
            pp::talentLearnCheck(t.row, s.rank, t.maxRank, tabSpent, unspent, have, need, petView ? 3 : 5);
        const bool lit = s.reqMet && s.meetsPrereq;
        // $parentSlot (UI-EmptySlot-White 64 x 64): green while points can go in, gold at max, grey otherwise.
        const uint32_t slotCol = !lit ? 0xFF808080u : s.rank < t.maxRank ? 0xFF1AFF1Au : 0xFF00D1FFu;
        if (trArt_.talSlot.pixels) {
            const float cw = 64 * kSX * side / (37 * kSX), ch = 64 * kSY;
            ui.image(trArt_.talSlot, Rect{b.x + side * 0.5f - cw * 0.5f, b.y + side * 0.5f - ch * 0.5f, cw, ch}, slotCol);
        }
        if (const pp::Texture* tex = spellDb_.icon(t.rankSpells[0], spellFrame_)) {
            // SetItemButtonDesaturated(button, 1, 0.65, 0.65, 0.65) - PSP: no desaturation in the GE; the icon
            // darkened and washed grey.
            ui.image(*tex, b, lit ? 0xFFFFFFFFu : 0xFFA6A6A6u);
            if (!lit) ui.fill(b, 0x60595959u);
        } else {
            ui.fill(b, 0xFF404040u);
        }
        if (!trainer_.talentClick && t.talentId == trainer_.talSel) ui.fill(Rect{b.x, b.y + b.h - 2, b.w, 2}, kColGold);
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, b, 0xFFFFFFFFu, Blend::Add);  // ButtonHilight-Square
        // $parentRankBorder (32 x 32 at the button's BOTTOMRIGHT) + $parentRank (GameFontNormalSmall).
        if (lit || s.rank > 0) {
            const Rect rb{b.x + b.w - 32 * kSX * 0.5f, b.y + b.h - 32 * kSY * 0.5f, 32 * kSX, 32 * kSY};
            if (trainer_.rankBorder.pixels) ui.image(trainer_.rankBorder, rb, lit ? 0xFFFFFFFFu : 0xFF808080u);
            else ui.fill(Rect{rb.x + 3, rb.y + 4, rb.w - 6, rb.h - 8}, 0xC0000000u);
            std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(s.rank));
            const uint32_t rc = !lit ? kColGreyFont : s.rank < t.maxRank ? kColGreenCode : kColGold;
            ui.textShadow(ft, rb.x + rb.w * 0.5f, rb.y + (rb.h - static_cast<float>(ft.lineHeight())) * 0.5f, buf, rc,
                          Align::Center);
        }
        if (f) {
            tipTalent = &t;
            tipCheck = c;
            tipRect = Rect{b.x + b.w + ui.offsetX(), b.y + ui.offsetY(), b.w, b.h};
            trainer_.talSel = trainer_.talentClick ? t.talentId : trainer_.talSel;
        }
        if (t.talentId == trainer_.talSel) trainer_.talSelLearnable = c.learnable();
    }
    for (const Piece& p : pieces)
        if (p.arrow) drawPiece(p);  // PlayerTalentFrameArrowFrame: OVERLAY, over the buttons
    ui.popClip();

    // The scroll bar's arrows (UIPanelScrollFrameTemplate; the bar art is baked).
    if (maxScroll > 0.0f) {
        const bool canUp = scroll > 0.5f, canDn = scroll < maxScroll - 0.5f;
        const Rect up{PX(326), PY(80), 11, 11}, dn{PX(326), PY(402) - 11, 11, 11};
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (top && canUp && winFocus_.addStop(kIdTalUp, up)) ui.fill(up, 0x40FFFFFFu);
        if (top && canDn && winFocus_.addStop(kIdTalDown, dn)) ui.fill(dn, 0x40FFFFFFu);
    }

    // PlayerTalentFramePointsBar: MASTERY_POINTS_SPENT left, UNSPENT_TALENT_POINTS right (the numbers white).
    {
        const float cy = PY(418);
        const float xl = PX(28), xr = PX(336);
        std::snprintf(buf, sizeof(buf), "%d", unspent);
        const std::string un = buf;
        const Font* pf = &fs;
        // MASTERY_POINTS_SPENT ("%1$s ...: %2$s") with the number left out (drawn white after it), UNSPENT_TALENT_POINTS
        // the same.
        const std::string left = CLIENT_FMT(MASTERY_POINTS_SPENT, tab.name, "");
        const std::string unLabel = CLIENT_FMT(UNSPENT_TALENT_POINTS, "");
        std::snprintf(buf, sizeof(buf), "%d", tabSpent);
        const std::string sp = buf;
        auto widthOf = [&](const Font& f) {
            return static_cast<float>(f.measure((left + sp).c_str()) + f.measure((unLabel + un).c_str())) + 8;
        };
        if (widthOf(fs) > xr - xl) pf = &ft;  // PSP: the tiny font when the two do not fit
        const float y = cy - static_cast<float>(pf->lineHeight()) * 0.5f;
        const float unW = static_cast<float>(pf->measure(un.c_str()));
        ui.textShadow(*pf, xr, y, un.c_str(), kColWhite, Align::Right);
        ui.textShadow(*pf, xr - unW, y, unLabel.c_str(), kColGold, Align::Right);
        const float rightStart = xr - unW - static_cast<float>(pf->measure(unLabel.c_str())) - 6;
        ui.pushClip(Rect{xl, y - 2, std::max(0.0f, rightStart - xl), static_cast<float>(pf->lineHeight()) + 4});
        ui.textShadow(*pf, xl, y, left.c_str(), kColGold);
        ui.textShadow(*pf, xl + static_cast<float>(pf->measure(left.c_str())), y, sp.c_str(), kColWhite);
        ui.popClip();
    }

    drawTreeTabs();  // the tab bar is a lambda (the glyph view draws it too)

    // GameTooltip:SetTalent for the focused talent (drawn after all the windows, drawTrainerOverlay).
    if (tipTalent) {
        const game::TalentEntry& t = *tipTalent;
        const uint8_t rank = rankOf(t.talentId);
        const uint32_t shownSpell = t.rankSpells[rank > 0 ? rank - 1 : 0];
        const std::string& name = spellDb_.name(shownSpell ? shownSpell : t.rankSpells[0]);
        trTip_.lines.clear();
        trTip_.lines.push_back({name.empty() ? std::string(CLIENT_STR(UNKNOWN)) : name, kColWhite});
        trTip_.lines.push_back(
            {CLIENT_FMT(TOOLTIP_TALENT_RANK, static_cast<unsigned>(rank), static_cast<unsigned>(t.maxRank)), kColWhite});
        if (!tipCheck.tierOpen) {  // TOOLTIP_TALENT_TIER_POINTS
            const int need = static_cast<int>(t.row) * (petView ? 3 : 5);  // PET_TALENTS_PER_TIER
            trTip_.lines.push_back(
                {pp::text::plural(CLIENT_FMT(TOOLTIP_TALENT_TIER_POINTS, need, tab.name)), kColRedFont});
        }
        for (int p = 0; p < 3; ++p) {  // TOOLTIP_TALENT_PREREQ
            if (t.prereqTalent[p] == 0 || prereqOk(t, p)) continue;
            const game::TalentEntry* pre = entryOf(t.prereqTalent[p]);
            const int need = t.prereqRank[p] + 1;
            const std::string& pn = pre ? spellDb_.name(pre->rankSpells[0]) : std::string();
            trTip_.lines.push_back(
                {pp::text::plural(CLIENT_FMT(TOOLTIP_TALENT_PREREQ, need, pn)), kColRedFont});
        }
        if (tipCheck.learnable()) trTip_.lines.push_back({CLIENT_STR(TOOLTIP_TALENT_LEARN), kColGreenFont});
        trTip_.anchor = tipRect;
    }
}

// After all the windows (drawWindows) - the talent tooltip, and the red error line again over the
// trainer / talent frames (UIErrorsFrame is above the panels in the original; the HUD draws it under them).
void WorldScreen::drawTrainerOverlay(UiRenderer& ui) {
    const bool open = windowOpen(kWinTrainer) || windowOpen(kWinTalents);
    if (open) drawUiErrors(ui);  // the whole UIErrorsFrame stack (ui_world_errors.cpp)
    if (open && !trTip_.lines.empty() && uiMode_) {
        const Font& fs = kit_.font(kFontSmall);
        const Font& fn = kit_.font(kFontNormal);
        float w = 0, h = 8;
        for (size_t i = 0; i < trTip_.lines.size(); ++i) {
            const Font& f = i == 0 ? fn : fs;
            w = std::max(w, static_cast<float>(f.measure(trTip_.lines[i].first.c_str())));
            h += static_cast<float>(f.lineHeight());
        }
        Rect r{trTip_.anchor.x + 2, trTip_.anchor.y, w + 14, h};  // ANCHOR_RIGHT
        if (r.x + r.w > pp::kScreenW - 2) r.x = trTip_.anchor.x - trTip_.anchor.w - r.w - 2;
        r.x = std::max(2.0f, r.x);
        r.y = std::clamp(r.y, 2.0f, std::max(2.0f, pp::kScreenH - r.h - 2));
        if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
        else ui.fill(r, 0xF0100000u);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
        float y = r.y + 4;
        for (size_t i = 0; i < trTip_.lines.size(); ++i) {
            const Font& f = i == 0 ? fn : fs;
            ui.text(f, r.x + 7, y, trTip_.lines[i].first.c_str(), trTip_.lines[i].second);
            y += static_cast<float>(f.lineHeight());
        }
    }
    trTip_.lines.clear();
}

// StaticPopupDialogs["CONFIRM_TALENT_WIPE"] - CONFIRM_TALENT_WIPE, the cost in the popup's
// SmallMoneyFrame (UIParent: MoneyFrame_Update(dialog.MoneyFrame, arg1)), ACCEPT / CANCEL.
void WorldScreen::drawTalentWipe(UiRenderer& ui, bool top) {
    StaticPopup p;
    p.text = CLIENT_STR(CONFIRM_TALENT_WIPE);
    p.hasMoneyFrame = true;
    p.money = session_.getTalentWipeCost();
    p.button1 = CLIENT_STR(ACCEPT);
    p.button1Id = kIdWipeYes;
    p.button2 = CLIENT_STR(CANCEL);
    p.button2Id = kIdWipeNo;
    p.focusable = top && uiMode_ && !settings_.isOpen();
    drawStaticPopup(ui, p);
}

}  // namespace wowee::ui::psp
