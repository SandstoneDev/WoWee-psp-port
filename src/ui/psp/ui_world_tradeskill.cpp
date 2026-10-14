// TradeSkillFrame of the original (the trade skill add-on's .xml / .lua, 3.3.5a
// ruRU) over the session's crafting window (psp_world_session_tradeskill.cpp: upstream SpellHandler's
// tradeskillOpenerSkillLine / craft queue and GameHandler::getCraftingRecipes) and the row logic of upstream's
// trade skill bindings (src/addons/lua_quest_api.cpp: visibleCraftingRecipes, buildTradeSkillRows,
// GetTradeSkillInfo / ReagentInfo / NumMade / Cooldown, DoTradeSkill, the dropdowns' lists).
//
// Casting a profession spell (the spell book, the action bar) opens it client-side, as the client. Drawn at the
// trainer's 0.6 x 0.55 of its FrameXML units at TOPLEFT 0,-104 (the left, at the top):
//   - TradeSkillFramePortrait (the player), the title (TRADE_SKILL_TITLE: the line's name), the close button;
//   - TradeSkillRankFrame: the line's skill "rank/max" on UI-Character-Skills-Bar (0, 0, 1, 0.5 over 0, 0, 0.75,
//     0.5) in its UI-Character-Skills-BarBorder;
//   - TradeSkillFrameAvailableFilterCheckButton CRAFT_IS_MAKEABLE (and its tooltip);
//   - TradeSkillSubClassDropDown ALL_SUBCLASSES / TradeSkillInvSlotDropDown ALL_INVENTORY_SLOTS (the crafted items'
//     subclasses / slots, single choice);
//   - ALL (TradeSkillCollapseAllButton) and the list: the crafted items' subclass headings with
//     UI-PlusButton / UI-MinusButton, the recipes coloured by TradeSkillTypeColor (optimal orange, medium
//     yellow, easy green, trivial grey; the bands of SkillLineAbility's trivial ranks), "[N]" when the bags hold
//     the reagents for N; the selection in UI-Listbox-Highlight2 tinted by its type;
//   - the details (TradeSkillDetailScrollFrame, scrolls when long): UI-ClassTrainer-DetailHeader, the icon of
//     what it makes (the spell's for an enchant) with the made count ("2", "1-3", "~4"), the name, the cooldown
//     (COOLDOWN_REMAINING + SecondsToTime), the description, SPELL_REAGENTS and the reagents (icon, name, "have
//     /need", greyed when short);
//   - CREATE_ALL, the count box with its arrows, CREATE (or the line's AlternateVerb for a recipe that makes no
//     item - then without Create All and the count), EXIT.
// Buttons (PSP): Cross = the left click (select, headings, check box, dropdowns, buttons); Circle on a recipe =
// select + CREATE (PSP shortcut, as Circle on a trainer service); START closes the dropdown list, then the
// window. A disabled CREATE keeps its focus: Cross says why on the red line (PSP).
// PSP: no search box (TradeSkillFrameEditBox: no keyboard - the dropdowns filter), no link button, no
// SPELL_TOTEMS tools line (upstream's GetTradeSkillTools answers nothing: the client knows neither the
// totem categories of the bag items nor the spell focus objects around); 5 list rows of 26 units (the PSP
// font is 13 px) instead of TRADE_SKILLS_DISPLAYED 8 of 16.
#include "ui/psp/ui_world_screen.hpp"

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
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

enum : uint32_t {
    kIdTsFirst = 0x7C01,  // 0x7C01-0x7CFF (bank-mail has 0x7400-0x76FF)
    kIdTsClose = kIdTsFirst,
    kIdTsExit,
    kIdTsCreate,
    kIdTsCreateAll,
    kIdTsDec,
    kIdTsInc,
    kIdTsAll,
    kIdTsAvail,
    kIdTsSubDD,
    kIdTsSlotDD,
    kIdTsUp,
    kIdTsDown,
    kIdTsDUp,
    kIdTsDDown,
    kIdTsIcon,
    kIdTsRow0 = 0x7C20,      // + 0..7 list rows
    kIdTsReagent0 = 0x7C30,  // + 0..7 reagents (MAX_TRADE_SKILL_REAGENTS)
    kIdTsDDItem0 = 0x7C40,   // + 0..63 dropdown list items
    kIdTsLast = 0x7CFF,
};
bool ourId(uint32_t id) { return id >= kIdTsFirst && id <= kIdTsLast; }

// Frame units -> px: the trainer's 0.6 x 0.55 at TOPLEFT 0,-104 (tools/psp/build_ui_pak.py CT_*).
constexpr float kSX = 0.6f, kSY = 0.55f;
constexpr float kOx = 4.0f, kOy = 2.0f;
constexpr float kFrameW = 216.0f, kFrameH = 240.0f;  // ts_frame: 360 x 437 units
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
Rect FR(float x, float y, float w, float h) { return Rect{PX(x), PY(y), w * kSX, h * kSY}; }

// TradeSkillSkill rows: PSP 5 rows of 26 units (14 px) in TradeSkillListScrollFrame's 130 (96..226).
constexpr int kRows = 5;
constexpr float kRowU = 26.0f, kListTopU = 96.0f;
// TradeSkillDetailScrollFrame: 297 x 176 at 20,-234.
constexpr float kDetailX = 20.0f, kDetailY = 234.0f, kDetailW = 297.0f, kDetailH = 176.0f;
constexpr int kMaxReagents = 8;   // MAX_TRADE_SKILL_REAGENTS
constexpr int kMaxDDItems = 64;

// 0xAABBGGRR. TradeSkillTypeColor (the trade skill add-on's Lua).
constexpr uint32_t kColOptimal = 0xFF4080FFu;  // 1.00, 0.50, 0.25 (GameFontNormalLeftOrange)
constexpr uint32_t kColMedium = 0xFF00FFFFu;   // 1.00, 1.00, 0.00 (GameFontNormalLeftYellow)
constexpr uint32_t kColEasy = 0xFF40BF40u;     // 0.25, 0.75, 0.25 (GameFontNormalLeftLightGreen)
constexpr uint32_t kColTrivial = 0xFF808080u;  // 0.50, 0.50, 0.50 (GameFontNormalLeftGrey)
constexpr uint32_t kColGray = 0xFF808080u;     // GRAY_FONT_COLOR
constexpr uint32_t kColRedSmall = 0xFF1A1AFFu; // GameFontRedSmall (1, 0.1, 0.1)
uint32_t typeColor(int difficulty) {
    switch (difficulty) {
        case 1: return kColMedium;
        case 2: return kColEasy;
        case 3: return kColTrivial;
        default: return kColOptimal;
    }
}

uint32_t rgba(float r, float g, float b, float a) {
    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

// SecondsToTime(seconds) (UIParent.lua): abbreviated, two units at most (hours always), TIME_UNIT_DELIMITER " ".
std::string secondsToTime(float seconds) {
    int s = static_cast<int>(std::floor(seconds));
    int count = 0;
    std::string out;
    auto add = [&](pp::CS fmt, int v) {
        if (!out.empty()) out += ' ';
        out += pp::text::plural(pp::csf(fmt, v));
        ++count;
    };
    if (s >= 86400) {
        add(pp::CS::DAYS_ABBR, s / 86400);
        s %= 86400;
    }
    if (s >= 3600) {
        add(pp::CS::HOURS_ABBR, s / 3600);
        s %= 3600;
    }
    if (count < 2 && s >= 60) {
        add(pp::CS::MINUTES_ABBR, s / 60);
        s %= 60;
    }
    if (count < 2 && s > 0) add(pp::CS::SECONDS_ABBR, s);
    return out;
}

const char* groupName(const pp::CraftDb& db, const pp::CraftRecipeRec& r) {
    return r.group < db.groupCount() ? db.str(db.groupAt(r.group).name) : "";
}
// The crafted item's slot label (upstream's kAuctionSlots row of its InventoryType), "" for none.
const char* slotName(const pp::CraftDb& db, const pp::CraftRecipeRec& r) {
    if (r.madeItem == 0 || r.invType == 0) return "";
    for (uint32_t i = 0; i < db.slotCount(); ++i)
        if (db.slotAt(i).invType == r.invType) return db.str(db.slotAt(i).name);
    return "";
}

}  // namespace

struct WorldScreen::TradeSkillUi {
    // Art (ui.pak): only while the window is open.
    pp::Texture frame, lscroll, dheader, altbar, input, hl, plusHl, plus, minus, cbUp, cbHl, cbCheck, bar, border,
        nameFrame, prev, next;
    bool artTried = false;

    // The window (TradeSkillFrame).
    bool shown = false, raisedCursor = false, guard = false, focusPending = false;
    uint32_t showsSeen = 0;
    uint32_t line = 0;              // CURRENT_TRADESKILL (a skill line id here)
    uint32_t sel = 0;               // TradeSkillFrame.selectedSkill (a recipe spell id here, 0 = none)
    int listScroll = 0;             // FauxScrollFrame offset (rows)
    float detailScroll = 0.0f;      // TradeSkillDetailScrollFrame offset (units)
    float detailMax = 0.0f;
    int count = 1;                  // TradeSkillInputBox
    uint32_t queueVersion = ~0u;    // UPDATE_TRADESKILL_RECAST seen
    bool onlyMakeable = false;      // TradeSkillFrameAvailableFilterCheckButton (kept while the client runs)
    int subPick = 0, slotPick = 0;  // the dropdowns' choice (0 = all), as upstream's tradeSkillSubClassPick
    std::string slotSelected;       // TradeSkillInvSlotDropDown.selected
    int ddOpen = 0;                 // the open dropdown list: 1 subclass, 2 inventory slot
    std::vector<std::string> collapsed;  // upstream collapsedTradeSkillSubClasses (by name, not saved)

    // GetTradeSkillInfo's rows (upstream buildTradeSkillRows), rebuilt only when their data changes.
    struct Row {
        bool header = false;
        uint32_t index = 0;  // header: into headers; recipe: into recipes
    };
    std::vector<pp::WorldSession::CraftRecipe> recipes;  // getCraftingRecipes (all known, by name)
    std::vector<Row> rows;
    std::vector<std::string> headers;                // the headings drawn, in row order
    std::vector<std::string> subclasses, slots;     // GetTradeSkillSubClasses / GetTradeSkillInvSlots
    uint32_t keyKnown = ~0u, keyInv = ~0u, keySkill = ~0u;
    bool dirty = true;
    // Why the rows are rebuilt: a heading opened / closed (TradeSkillFrame_Update only), TRADE_SKILL_UPDATE (the
    // selection kept, TradeSkillFrame_SetSelection again), TradeSkillFrame_Show, TRADE_SKILL_FILTER_UPDATE (the
    // first recipe, scrolled up).
    enum Reason : uint8_t { kRedraw, kUpdate, kShow, kFirst };
    Reason reselect = kShow;
    std::vector<int64_t> visible;                    // per drawn row: header (1 << 40 | index) or spell id
    uint32_t reagentItems[kMaxReagents] = {};        // the drawn reagents (tooltips)

    // GameTooltip (drawn after all windows).
    const char* tip = nullptr;
    Rect tipAnchor{};
    uint32_t logs = 0;

    ~TradeSkillUi() { releaseArt(); }
    std::array<pp::Texture*, 17> textures() {
        return {&frame, &lscroll, &dheader, &altbar, &input,     &hl,   &plusHl, &plus, &minus,
                &cbUp,  &cbHl,    &cbCheck, &bar,    &border,    &nameFrame, &prev, &next};
    }
    void build(pp::WorldSession& s);
    uint32_t artBytes() {
        uint32_t n = 0;
        for (pp::Texture* t : textures())
            if (t->pixels) n += pp::textureBytes(*t);
        return n;
    }
    void releaseArt() {
        for (pp::Texture* t : textures())
            if (t->pixels) pp::textureRelease(*t);
        artTried = false;
    }
    void loadArt() {
        if (artTried) return;
        artTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (!pak.open(path)) return;
        const std::pair<const char*, pp::Texture*> list[] = {
            {"ts_frame", &frame},   {"ts_lscroll", &lscroll}, {"ts_dheader", &dheader},  {"ts_altbar", &altbar},
            {"ts_input", &input},   {"ct_hl", &hl},           {"ct_plushl", &plusHl},    {"ql_plus", &plus},
            {"ql_minus", &minus},   {"cb_up", &cbUp},         {"cb_hl", &cbHl},          {"ql_check", &cbCheck},
            {"skill_bar", &bar},    {"skill_border", &border}, {"loot_namefr", &nameFrame}, {"sb_prev", &prev},
            {"sb_next", &next}};
        for (const auto& [name, tex] : list) {
            const pp::PakEntry* e = pak.find(name);
            void* blob = e ? pak.load(e) : nullptr;
            if (blob && !pp::textureFromBlob(blob, e->size, *tex, true)) pp::pakFree(blob);
        }
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "tradeskill_art frame=%d kb=%u", frame.pixels ? 1 : 0,
                 static_cast<unsigned>((artBytes() + 1023u) / 1024u));
    }
    void releaseRows() {
        recipes.clear();
        recipes.shrink_to_fit();
        rows.clear();
        rows.shrink_to_fit();
        headers.clear();
        headers.shrink_to_fit();
        subclasses.clear();
        slots.clear();
        visible.clear();
        dirty = true;
    }
    bool isCollapsed(const std::string& name) const {
        return std::find(collapsed.begin(), collapsed.end(), name) != collapsed.end();
    }
    const pp::WorldSession::CraftRecipe* selected() const {
        for (const auto& r : recipes)
            if (r.spellId == sel) return &r;
        return nullptr;
    }
    // GetFirstTradeSkill: the first row that is a recipe (0 = none).
    uint32_t firstRecipe() const {
        for (const Row& r : rows)
            if (!r.header) return recipes[r.index].spellId;
        return 0;
    }
    int rowOf(uint32_t spellId) const {
        for (size_t i = 0; i < rows.size(); ++i)
            if (!rows[i].header && recipes[rows[i].index].spellId == spellId) return static_cast<int>(i);
        return -1;
    }
};

void WorldScreen::destroyTradeSkillUi(TradeSkillUi* p) { delete p; }

WorldScreen::TradeSkillUi& WorldScreen::tradeSkillUi() {
    if (!ts_) ts_ = std::unique_ptr<TradeSkillUi, void (*)(TradeSkillUi*)>(new TradeSkillUi(), &destroyTradeSkillUi);
    return *ts_;
}

// upstream visibleCraftingRecipes + buildTradeSkillRows over the session's getCraftingRecipes: the headings are
// the crafted items' subclasses (a std::map: by name), each followed by its recipes unless closed, then the
// recipes that make no item, without a heading. The dropdowns' lists come from every known recipe.
void WorldScreen::TradeSkillUi::build(pp::WorldSession& s) {
    TradeSkillUi& t = *this;
    const pp::CraftDb& db = s.craftDb();
    t.recipes = s.getCraftingRecipes();
    t.subclasses.clear();
    t.slots.clear();
    for (const auto& r : t.recipes) {
        const std::string g = groupName(db, *r.rec), sl = slotName(db, *r.rec);
        if (!g.empty() && std::find(t.subclasses.begin(), t.subclasses.end(), g) == t.subclasses.end())
            t.subclasses.push_back(g);
        if (!sl.empty() && std::find(t.slots.begin(), t.slots.end(), sl) == t.slots.end()) t.slots.push_back(sl);
    }
    std::sort(t.subclasses.begin(), t.subclasses.end());
    std::sort(t.slots.begin(), t.slots.end());
    if (t.subPick > static_cast<int>(t.subclasses.size())) t.subPick = 0;
    if (t.slotPick > static_cast<int>(t.slots.size())) t.slotPick = 0;
    std::map<std::string, std::vector<uint32_t>> grouped;
    std::vector<uint32_t> ungrouped;
    for (uint32_t i = 0; i < t.recipes.size(); ++i) {
        const auto& r = t.recipes[i];
        if (t.onlyMakeable && r.canMake <= 0) continue;
        // A recipe that makes no item passes the two dropdowns (upstream: no item info, shown).
        if (r.rec->madeItem != 0) {
            if (t.subPick > 0 && t.subclasses[static_cast<size_t>(t.subPick) - 1] != groupName(db, *r.rec)) continue;
            if (t.slotPick > 0 && t.slots[static_cast<size_t>(t.slotPick) - 1] != slotName(db, *r.rec)) continue;
        }
        const char* g = groupName(db, *r.rec);
        if (g[0]) grouped[g].push_back(i);
        else ungrouped.push_back(i);
    }
    t.rows.clear();
    t.headers.clear();
    for (const auto& [name, list] : grouped) {
        t.rows.push_back(Row{true, static_cast<uint32_t>(t.headers.size())});
        t.headers.push_back(name);
        if (t.isCollapsed(name)) continue;  // a closed heading takes its recipes out of the list
        for (uint32_t i : list) t.rows.push_back(Row{false, i});
    }
    for (uint32_t i : ungrouped) t.rows.push_back(Row{false, i});
}

// --- Per frame ---------------------------------------------------------------------------------

void WorldScreen::updateTradeSkill(float dtSec) {
    (void)dtSec;
    const bool want = session_.isCraftingWindowOpen();
    if (!ts_ && !want) return;  // never opened: nothing allocated
    PROF_SCOPE("tradeskill");
    TradeSkillUi& t = tradeSkillUi();
    if (want && (!t.shown || t.showsSeen != session_.craftingShows())) {
        // TRADE_SKILL_SHOW -> TradeSkillFrame_Show (ShowUIPanel; OnShow only when it was hidden).
        t.showsSeen = session_.craftingShows();
        if (!windowOpen(kWinTradeSkill)) {
            openWindow(kWinTradeSkill);
            t.count = 1;  // OnShow: TradeSkillInputBox:SetNumber(1)
            playUiSound(UiSound::CharacterSheetOpen);  // OnShow: igCharacterInfoOpen
        }
        t.shown = true;
        t.guard = true;
        t.ddOpen = 0;  // CloseDropDownMenus
        t.listScroll = 0;
        t.detailScroll = 0.0f;
        t.reselect = TradeSkillUi::kShow;
        t.dirty = true;
        t.focusPending = true;
        if (!uiMode_) {
            setUiMode(true);
            t.raisedCursor = true;
            suppress_ = ~0u;  // everything held now stays dead until released
        }
    } else if (want && t.shown && !windowOpen(kWinTradeSkill)) {
        t.shown = false;  // closed by the player (START, the close button, EXIT): OnHide -> CloseTradeSkill
        t.ddOpen = 0;
        session_.closeCraftingWindow();
        playUiSound(UiSound::CharacterSheetClose);  // igCharacterInfoClose
    } else if (!want && t.shown) {
        closeWindow(kWinTradeSkill);
        t.shown = false;
        t.ddOpen = 0;
    }
    if (t.raisedCursor) {
        if (!uiMode_) {
            t.raisedCursor = false;
        } else if (!t.shown && windows_.empty() && !settings_.isOpen()) {
            setUiMode(false);
            t.raisedCursor = false;
            suppress_ = ~0u;  // the button that closed it must not walk
        }
    }
    if (!t.shown) {
        if (t.artTried || !t.recipes.empty()) {
            const uint32_t kb = (t.artBytes() + static_cast<uint32_t>(t.recipes.capacity() *
                                                                      sizeof(pp::WorldSession::CraftRecipe)) +
                                 1023u) / 1024u;
            t.releaseArt();
            t.releaseRows();
            PSP_LOGI(pp::tag::Gfx, "tradeskill_release kb=%u", static_cast<unsigned>(kb));
        }
        return;
    }
    // TradeSkillFrame_Update: another line than the one shown - StopTradeSkillRepeat, the dropdowns back to "all".
    if (t.line != session_.getCraftingSkillLine()) {
        if (t.line != 0) {
            session_.cancelCraftQueue();
            t.subPick = t.slotPick = 0;
            t.slotSelected.clear();
        }
        t.line = session_.getCraftingSkillLine();
        t.dirty = true;
    }
    // TRADE_SKILL_UPDATE: the known recipes, the bags (reagents / [N]) or the skill (the colours) changed.
    const uint32_t known = session_.knownSpellsVersion(), inv = session_.inventoryVersion(),
                   skill = session_.skillsVersion();
    if (known != t.keyKnown || inv != t.keyInv || skill != t.keySkill) {
        t.keyKnown = known;
        t.keyInv = inv;
        t.keySkill = skill;
        t.dirty = true;
        if (t.reselect == TradeSkillUi::kRedraw) t.reselect = TradeSkillUi::kUpdate;
    }
    if (t.dirty) {
        t.dirty = false;
        t.build(session_);
        const uint32_t was = t.sel;
        // PSP: the selection is a spell id (the original keeps an index): a redraw keeps any known recipe, an
        // update / show one still in the list, a filter none.
        const bool keep = t.sel != 0 && (t.reselect == TradeSkillUi::kRedraw ? t.selected() != nullptr
                                         : t.reselect != TradeSkillUi::kFirst && t.rowOf(t.sel) >= 0);
        if (!keep) {
            t.sel = t.firstRecipe();  // GetFirstTradeSkill
            t.listScroll = 0;
        }
        // TradeSkillFrame_SetSelection (on show, TRADE_SKILL_UPDATE and the filters; not for a heading):
        // TradeSkillInputBox:SetNumber(GetTradeskillRepeatCount()).
        if (t.reselect != TradeSkillUi::kRedraw || t.sel != was) {
            t.count = std::max(1, session_.getCraftQueueRemaining());
            if (t.sel != was) t.detailScroll = 0.0f;
        }
        t.reselect = TradeSkillUi::kRedraw;
        if (++t.logs <= 40)
            PSP_LOGI(pp::tag::Gfx, "tradeskill_rows line=%u recipes=%u rows=%u sel=%u only=%d sub=%d slot=%d",
                     static_cast<unsigned>(t.line), static_cast<unsigned>(t.recipes.size()),
                     static_cast<unsigned>(t.rows.size()), static_cast<unsigned>(t.sel), t.onlyMakeable ? 1 : 0,
                     t.subPick, t.slotPick);
    }
    // UPDATE_TRADESKILL_RECAST: the count box counts the queue down (GetTradeskillRepeatCount).
    if (t.queueVersion != session_.craftQueueVersion()) {
        const bool first = t.queueVersion == ~0u;
        t.queueVersion = session_.craftQueueVersion();
        if (!first) t.count = std::max(1, session_.getCraftQueueRemaining());
    }
    if (t.focusPending && windowOpen(kWinTradeSkill)) {
        t.focusPending = false;
        const int at = t.rowOf(t.sel);
        winFocus_.setFocus(at >= 0 && at < kRows ? kIdTsRow0 + static_cast<uint32_t>(at) : kIdTsRow0);
    }
}

// --- Actions --------------------------------------------------------------------------------------

bool WorldScreen::tradeSkillCloseDropDown() {
    if (!ts_ || ts_->ddOpen == 0 || !windowOpen(kWinTradeSkill)) return false;
    ts_->ddOpen = 0;
    return true;
}

bool WorldScreen::tradeSkillAction(uint32_t act) {
    if (!ourId(act)) return false;
    if (!ts_ || !windowOpen(kWinTradeSkill)) return true;
    TradeSkillUi& t = *ts_;
    if (t.guard) return true;  // a Cross held since before the window came up: its release is not a click
    const pp::CraftDb& db = session_.craftDb();
    const pp::WorldSession::CraftRecipe* sel = t.selected();
    auto click = [] { playUiSound(UiSound::ButtonClick); };
    // The dropdown lists: a click on an item picks it (no keepShownOnClick), anywhere else closes the list.
    if (t.ddOpen != 0) {
        const int dd = t.ddOpen;
        t.ddOpen = 0;
        if (act >= kIdTsDDItem0 && act < kIdTsDDItem0 + kMaxDDItems) {
            const int k = static_cast<int>(act - kIdTsDDItem0);
            if (dd == 1) {  // TradeSkillSubClassDropDownButton_OnClick
                t.subPick = k;
                if (k != 0) {
                    const std::string name =
                        t.slotPick > 0 && t.slotPick <= static_cast<int>(t.slots.size()) ? t.slots[t.slotPick - 1] : "";
                    if (name != t.slotSelected) {  // TradeSkillFilterFrame_InvSlotName ~= .selected
                        t.slotPick = 0;
                        t.slotSelected.clear();
                    }
                }
            } else {  // TradeSkillInvSlotDropDownButton_OnClick
                t.slotPick = k;
                t.slotSelected = k > 0 && k <= static_cast<int>(t.slots.size()) ? t.slots[k - 1] : "";
            }
            // PSP: a filter reselects the first recipe and scrolls up (TRADE_SKILL_FILTER_UPDATE, as the check
            // box) - the list has just changed length.
            t.reselect = TradeSkillUi::kFirst;
            t.dirty = true;
            t.listScroll = 0;
            click();
        }
        return true;
    }
    switch (act) {
        case kIdTsClose:
        case kIdTsExit: closeWindow(kWinTradeSkill); return true;  // HideParentPanel (updateTradeSkill: CloseTradeSkill)
        case kIdTsUp: t.listScroll = std::max(0, t.listScroll - 1); return true;
        case kIdTsDown: ++t.listScroll; return true;  // clamped when drawn
        case kIdTsDUp: t.detailScroll = std::max(0.0f, t.detailScroll - 43.0f); return true;
        case kIdTsDDown: t.detailScroll = std::min(t.detailMax, t.detailScroll + 43.0f); return true;
        case kIdTsSubDD:
        case kIdTsSlotDD:
            t.ddOpen = act == kIdTsSubDD ? 1 : 2;
            click();
            return true;
        case kIdTsAvail:  // TradeSkillOnlyShowMakeable -> TRADE_SKILL_FILTER_UPDATE
            t.onlyMakeable = !t.onlyMakeable;
            if (!playSoundByName(t.onlyMakeable ? "igMainMenuOptionCheckBoxOn" : "igMainMenuOptionCheckBoxOff"))
                click();
            t.reselect = TradeSkillUi::kFirst;
            t.dirty = true;
            t.listScroll = 0;
            return true;
        case kIdTsAll: {  // TradeSkillCollapseAllButton_OnClick (Expand / CollapseTradeSkillSubClass(0) = all)
            bool allOpen = true;
            for (const std::string& h : t.headers)
                if (t.isCollapsed(h)) allOpen = false;
            if (!t.headers.empty() && allOpen) {
                for (const std::string& h : t.headers) t.collapsed.push_back(h);
                t.listScroll = 0;
            } else {
                t.collapsed.clear();
            }
            t.dirty = true;
            click();
            return true;
        }
        case kIdTsDec:  // TradeSkillFrameDecrement_OnClick (and OnTextChanged: "0" -> "1")
            t.count = std::max(1, t.count - 1);
            click();
            return true;
        case kIdTsInc:  // TradeSkillFrameIncrement_OnClick
            if (t.count < 100) ++t.count;
            click();
            return true;
        case kIdTsCreate:
        case kIdTsCreateAll: {
            if (!sel) return true;
            // creatable: every reagent in the bags (TradeSkillFrame_SetSelection).
            const pp::CraftReagentRec* rg = db.reagents(*sel->rec);
            uint32_t shortItem = 0;
            for (uint8_t k = 0; k < sel->rec->reagentCount && !shortItem; ++k)
                if (session_.countItemInBags(rg[k].item) < rg[k].count) shortItem = rg[k].item;
            const bool altVerb = sel->rec->madeItem == 0 && db.altVerb()[0] != '\0';
            if (shortItem) {  // PSP: the disabled button says why (the original greys it)
                const auto* info = session_.getItemInfo(shortItem);
                uiError(info && info->valid && !info->name.empty()
                            ? CLIENT_FMT(SPELL_FAILED_REAGENTS, info->name)
                            : std::string(CLIENT_STR(ERR_SPELL_FAILED_REAGENTS_GENERIC)));
                PSP_LOGI(pp::tag::Net, "tradeskill_blocked spell=%u item=%u", static_cast<unsigned>(sel->spellId),
                         static_cast<unsigned>(shortItem));
                return true;
            }
            if (act == kIdTsCreateAll) {
                if (altVerb) return true;  // hidden for an AlternateVerb recipe
                t.count = std::max(0, sel->canMake);  // TradeSkillInputBox:SetNumber(numAvailable)
            }
            // DoTradeSkill(selectedSkill, count) -> upstream startCraftQueue (count < 1 -> 1).
            const int n = altVerb ? 1 : std::max(1, t.count);
            PSP_LOGI(pp::tag::Net, "tradeskill_create spell=%u count=%d all=%d", static_cast<unsigned>(sel->spellId), n,
                     act == kIdTsCreateAll ? 1 : 0);
            session_.startCraftQueue(sel->spellId, n, wantMove_ || moving_);
            return true;
        }
        default: break;
    }
    if (act >= kIdTsRow0 && act < kIdTsRow0 + kRows) {
        const size_t v = act - kIdTsRow0;
        if (v >= t.visible.size()) return true;
        const int64_t r = t.visible[v];
        if (r >> 40) {  // a heading: TradeSkillFrame_SetSelection -> Collapse / ExpandTradeSkillSubClass
            const size_t h = static_cast<size_t>(r & 0xFFFF);
            if (h < t.headers.size()) {
                auto it = std::find(t.collapsed.begin(), t.collapsed.end(), t.headers[h]);
                if (it == t.collapsed.end()) t.collapsed.push_back(t.headers[h]);
                else t.collapsed.erase(it);
                t.dirty = true;
            }
        } else {  // TradeSkillSkillButton_OnClick -> TradeSkillFrame_SetSelection
            if (static_cast<uint32_t>(r) != t.sel) t.detailScroll = 0.0f;
            t.sel = static_cast<uint32_t>(r);
            t.count = std::max(1, session_.getCraftQueueRemaining());  // SetNumber(GetTradeskillRepeatCount())
        }
        click();
    }
    return true;
}

// Circle on a recipe: select it and CREATE (PSP shortcut for the click and the button); elsewhere as Cross.
bool WorldScreen::tradeSkillAltAction(uint32_t alt) {
    if (!ourId(alt)) return false;
    if (!ts_ || !windowOpen(kWinTradeSkill) || ts_->guard) return true;
    TradeSkillUi& t = *ts_;
    if (t.ddOpen == 0 && alt >= kIdTsRow0 && alt < kIdTsRow0 + kRows) {
        const size_t v = alt - kIdTsRow0;
        if (v < t.visible.size() && !(t.visible[v] >> 40)) {
            if (static_cast<uint32_t>(t.visible[v]) != t.sel) {  // the click: TradeSkillFrame_SetSelection
                t.sel = static_cast<uint32_t>(t.visible[v]);
                t.count = std::max(1, session_.getCraftQueueRemaining());
                t.detailScroll = 0.0f;
            }
            return tradeSkillAction(kIdTsCreate);
        }
    }
    return tradeSkillAction(alt);
}

// --- Drawing -------------------------------------------------------------------------------------

bool WorldScreen::drawTradeSkillWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinTradeSkill) return false;
    TradeSkillUi& t = tradeSkillUi();
    t.loadArt();
    if (!t.frame.pixels) {  // an old ui.pak
        closeWindow(kWinTradeSkill);
        uiError(OWN_STR(TRADESKILL_ART_MISSING));
        return true;
    }
    PROF_SCOPE("tradeskill");
    loadNpcArt(false);  // the panel buttons, the close button, the scroll arrows
    const pp::CraftDb& db = session_.craftDb();
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight()), th = static_cast<float>(ft.lineHeight());
    // Every open window takes the focus (the covered parts not: FocusRing occluders).
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const uint32_t focused = winFocus_.focused();
    if (ourId(focused) && winFocus_.isPressed(focused)) t.guard = false;  // a press seen on the window arms it
    const bool live = focusable && t.ddOpen == 0;  // the open dropdown list takes the focus
    t.tip = nullptr;
    const pp::WorldSession::CraftRecipe* sel = t.selected();
    const bool altVerb = sel && sel->rec->madeItem == 0 && db.altVerb()[0] != '\0';

    // TradeSkillFramePortrait (60 x 60 at 7,-6): SetPortraitTexture(.., "player") - the race portrait.
    {
        const Rect pr = FR(7, 6, 60, 60);
        const game::Character* me = session_.activeCharacter();
        const int race = me ? static_cast<int>(me->race) : 0, sex = me && me->gender == game::Gender::FEMALE ? 1 : 0;
        if (race > 0 && race < 12 && ufArt_.portrait[race][sex].pixels) ui.imageCircle(ufArt_.portrait[race][sex], pr);
        else ui.fillCircle(Rect{pr.x + 2, pr.y + 2, pr.w - 4, pr.h - 4}, 0xFF101010u);
    }
    ui.image(t.frame, Rect{kOx, kOy, kFrameW, kFrameH});
    if (altVerb && t.altbar.pixels) ui.image(t.altbar, FR(10, 402, 202, 36));  // UI-ClassTrainer-BotLeft

    // TradeSkillFrameTitleText (TOP 0,-17, GameFontNormal): TRADE_SKILL_TITLE "%s" = the line's name.
    {
        const pp::SpellDb::SkillLineInfo* line = spellDb_.skillLine(t.line);
        const char* title = line && !line->name.empty() ? line->name.c_str() : CLIENT_STR(TRADE_SKILLS);
        const Font& tf = static_cast<float>(fn.measure(title)) <= 242 * kSX ? fn : fs;
        // PSP: centred in the title bar (its inside is y 15..32 units; the 13 px font is taller than 0.55 x 12).
        ui.textShadow(tf, PX(192), PY(23.5f) - static_cast<float>(tf.lineHeight()) * 0.5f, title, kColGold,
                      Align::Center);
    }
    // TradeSkillFrameCloseButton (UIPanelCloseButton, TOPRIGHT -29,-8).
    {
        const Rect cr{PX(339) - 9, PY(24) - 9, 18, 18};
        const bool f = live && winFocus_.addStop(kIdTsClose, cr);
        const pp::Texture& tx = f && winFocus_.isPressed(kIdTsClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                  : npcArt_.close;
        if (tx.pixels) ui.image(tx, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }

    // TradeSkillRankFrame (75,-36, 265 x 14): the background (SetVertexColor 0, 0, 0.75, 0.5), the bar
    // (SetStatusBarColor 0, 0, 1, 0.5), UI-Character-Skills-BarBorder (274 x 27 at LEFT -5,0), "rank/max".
    {
        uint32_t rank = 0, maxRank = 0;
        session_.craftSkill(rank, maxRank);
        const Rect bar = FR(75, 36, 265, 14);
        ui.fill(bar, rgba(0.0f, 0.0f, 0.75f, 0.5f));
        if (maxRank > 0 && rank > 0 && t.bar.pixels) {
            const float frac = std::min(1.0f, static_cast<float>(rank) / static_cast<float>(maxRank));
            ui.image(t.bar, Rect{bar.x, bar.y, bar.w * frac, bar.h},
                     SrcRect{0, 0, static_cast<float>(t.bar.uw) * frac, static_cast<float>(t.bar.uh)},
                     rgba(0.0f, 0.0f, 1.0f, 0.5f));
        }
        if (t.border.pixels) ui.image(t.border, FR(70, 29.5f, 274, 27));
        char b[32];
        std::snprintf(b, sizeof(b), "%u/%u", static_cast<unsigned>(rank), static_cast<unsigned>(maxRank));
        ui.textShadow(ft, PX(192), bar.y + (bar.h - th) * 0.5f, b, kColWhite, Align::Center);  // GameFontHighlightSmall
    }

    // TradeSkillFrameAvailableFilterCheckButton (UICheckButtonTemplate 24 x 24 at 70,-46): CRAFT_IS_MAKEABLE.
    {
        const Rect box = FR(70, 46, 24, 24);
        const char* label = CLIENT_STR(CRAFT_IS_MAKEABLE);
        const float tx = PX(92);  // $parentText LEFT at the box's RIGHT -2
        const Rect hit{box.x, box.y, tx + static_cast<float>(fs.measure(label)) - box.x, box.h};  // HitRect -75
        const bool f = live && winFocus_.addStop(kIdTsAvail, hit);
        if (t.cbUp.pixels) ui.image(t.cbUp, box);
        if (t.onlyMakeable && t.cbCheck.pixels) ui.image(t.cbCheck, box);
        if (f && t.cbHl.pixels) ui.image(t.cbHl, box, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(fs, tx, box.y + (box.h - lh) * 0.5f, label, kColGold);  // GameFontNormalSmall
        if (f) {
            t.tip = CLIENT_STR(CRAFT_IS_MAKEABLE_TOOLTIP);
            t.tipAnchor = Rect{box.x + ui.offsetX(), box.y + ui.offsetY(), box.w, box.h};
        }
    }

    // The two UIDropDownMenuTemplate (SetWidth 120; the label frames are baked): the text (GameFontHighlightSmall,
    // RIGHT at the right cap -43,2), the arrow button (24 x 24 at the right cap's TOPRIGHT -16,-18).
    auto dropDown = [&](uint32_t id, float x0, const char* text) {
        const Rect boxR = FR(x0 + 10, 62, 149, 38);
        const Rect btn = FR(x0 + 130, 67, 24, 24);
        const bool f = live && winFocus_.addStop(id, boxR);
        if (npcArt_.scrDown.pixels) ui.image(npcArt_.scrDown, btn);
        if (f) ui.fill(btn, 0x40FFFFFFu);
        const Rect tr{PX(x0 + 30), PY(79) - lh * 0.5f, (127 - 30) * kSX, lh};
        const Font& df = static_cast<float>(fs.measure(text)) <= tr.w ? fs : ft;
        if (static_cast<float>(df.measure(text)) <= tr.w) {
            ui.textShadow(df, tr.x + tr.w, tr.y + (lh - static_cast<float>(df.lineHeight())) * 0.5f, text, kColWhite,
                          Align::Right);
        } else {
            ui.textMarquee(ft, Rect{tr.x, tr.y + (lh - th) * 0.5f, tr.w, th}, text, kColWhite);
        }
    };
    const char* subText = t.subPick > 0 && t.subPick <= static_cast<int>(t.subclasses.size())
                              ? t.subclasses[static_cast<size_t>(t.subPick) - 1].c_str()
                              : CLIENT_STR(ALL_SUBCLASSES);
    const char* slotText = t.slotPick > 0 && t.slotPick <= static_cast<int>(t.slots.size())
                               ? t.slots[static_cast<size_t>(t.slotPick) - 1].c_str()
                               : CLIENT_STR(ALL_INVENTORY_SLOTS);
    dropDown(kIdTsSubDD, 54, subText);
    dropDown(kIdTsSlotDD, 189, slotText);

    // ALL (TradeSkillCollapseAllButton, 40 x 22 at the expand tab's left +0,+3): minus while any heading is
    // open (TradeSkillFrame_Update: notExpanded ~= numHeaders), disabled with an empty list.
    const int total = static_cast<int>(t.rows.size());
    {
        int notExpanded = 0;
        for (const std::string& h : t.headers)
            if (t.isCollapsed(h)) ++notExpanded;
        const bool minus = notExpanded != static_cast<int>(t.headers.size());
        const Rect b = FR(23, 73, 40, 22);
        const bool f = live && total > 0 && winFocus_.addStop(kIdTsAll, b);
        const pp::Texture& tx = minus ? t.minus : t.plus;
        const Rect ic{PX(26), b.y + (b.h - 10) * 0.5f, 10, 10};
        if (tx.pixels) ui.image(tx, ic, total == 0 ? 0xFF808080u : 0xFFFFFFFFu);
        if (f && t.plusHl.pixels) ui.image(t.plusHl, ic, 0xFFFFFFFFu, Blend::Add);
        ui.textShadow(fs, PX(44), b.y + (b.h - lh) * 0.5f, CLIENT_STR(ALL), total == 0 ? kColGray : f ? kColWhite : kColGold);
    }

    // --- The list (GetTradeSkillInfo) ---------------------------------------------------------------
    t.listScroll = std::clamp(t.listScroll, 0, std::max(0, total - kRows));
    // The list follows the focus (as the trainer): the focus on the first / last row scrolls one row.
    if (live && focused >= kIdTsRow0 && focused < kIdTsRow0 + static_cast<uint32_t>(kRows)) {
        const int vi = static_cast<int>(focused - kIdTsRow0);
        if (vi == kRows - 1 && t.listScroll + kRows < total) {
            ++t.listScroll;
            winFocus_.setFocus(focused - 1);
        } else if (vi == 0 && t.listScroll > 0) {
            --t.listScroll;
            winFocus_.setFocus(focused + 1);
        }
    }
    t.visible.clear();
    const bool scrollShown = total > kRows;  // FauxScrollFrame_Update: the bar only when needed
    const float rowW = scrollShown ? 293.0f : 323.0f;
    const float textW = 275.0f * kSX;  // TRADE_SKILL_TEXT_WIDTH
    for (int v = 0; v < kRows; ++v) {
        const int i = t.listScroll + v;
        if (i >= total) break;
        const TradeSkillUi::Row& row = t.rows[static_cast<size_t>(i)];
        const Rect rr = FR(22, kListTopU + kRowU * static_cast<float>(v), rowW, kRowU);
        const uint32_t id = kIdTsRow0 + static_cast<uint32_t>(v);
        const bool f = live && winFocus_.addStop(id, rr);
        const float ty = rr.y + (rr.h - lh) * 0.5f;
        const Rect ic{PX(25), rr.y + (rr.h - 10) * 0.5f, 10, 10};
        const float tx = PX(43);
        if (row.header) {  // GameFontNormalLeft, UI-MinusButton / UI-PlusButton, UI-PlusButton-Hilight
            t.visible.push_back((static_cast<int64_t>(1) << 40) | static_cast<int64_t>(row.index));
            const std::string& name = t.headers[row.index];
            const pp::Texture& tx2 = t.isCollapsed(name) ? t.plus : t.minus;
            if (tx2.pixels) ui.image(tx2, ic);
            if (f && t.plusHl.pixels) ui.image(t.plusHl, ic, 0xFFFFFFFFu, Blend::Add);
            ui.pushClip(Rect{tx, rr.y, textW, rr.h});
            ui.textShadow(fs, tx, ty, name.c_str(), f ? kColWhite : kColGold);
            ui.popClip();
            continue;
        }
        const auto& r = t.recipes[row.index];
        t.visible.push_back(static_cast<int64_t>(r.spellId));
        const bool isSel = r.spellId == t.sel;
        const uint32_t col = typeColor(r.difficulty);
        if (isSel && t.hl.pixels) ui.image(t.hl, FR(22, kListTopU + kRowU * static_cast<float>(v), 293, kRowU), col);
        const bool white = isSel || f;  // HighlightFont on the locked / hovered button, the count white too
        const std::string name = " " + r.name;  // skillNamePrefix " "
        char cnt[16] = "";
        if (r.canMake > 0) std::snprintf(cnt, sizeof(cnt), "[%d]", r.canMake);
        const float cw = cnt[0] ? static_cast<float>(fs.measure(cnt)) : 0.0f;
        float nameW = static_cast<float>(fs.measure(name.c_str()));
        if (cnt[0] && nameW + 2 * kSX + cw > textW) nameW = textW - 2 * kSX - cw;  // TRADE_SKILL_TEXT_WIDTH-2-count
        if (f && static_cast<float>(fs.measure(name.c_str())) > nameW) {
            ui.textMarquee(fs, Rect{tx, ty, nameW, lh}, name.c_str(), kColWhite);
        } else {
            ui.pushClip(Rect{tx, rr.y, std::max(0.0f, nameW), rr.h});
            ui.textShadow(fs, tx, ty, name.c_str(), white ? kColWhite : col);
            ui.popClip();
        }
        if (cnt[0]) ui.textShadow(fs, tx + nameW + 2 * kSX, ty, cnt, white ? kColWhite : col);  // $parentCount
    }
    if (total == 0)  // PSP: the empty list says so (the original shows nothing)
        ui.textShadow(fs, PX(43), PY(kListTopU) + 2, t.recipes.empty() ? OWN_STR(TRADESKILL_NO_RECIPES) : OWN_STR(LIST_ALL_FILTERED),
                      kColGray);
    if (scrollShown) {  // TradeSkillListScrollFrame's bar and arrows
        if (t.lscroll.pixels) ui.image(t.lscroll, FR(314, 94, 30, 134));
        const bool canUp = t.listScroll > 0, canDn = t.listScroll + kRows < total;
        const Rect up{PX(321), PY(97), 11, 11}, dn{PX(321), PY(226) - 12, 11, 11};
        const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
        const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
        if (tu.pixels) ui.image(tu, up);
        if (td.pixels) ui.image(td, dn);
        if (live && canUp && winFocus_.addStop(kIdTsUp, up)) ui.fill(up, 0x40FFFFFFu);
        if (live && canDn && winFocus_.addStop(kIdTsDown, dn)) ui.fill(dn, 0x40FFFFFFu);
    }

    // --- The details (TradeSkillFrame_SetSelection; TradeSkillDetailScrollChildFrame at 20,-234) -------------
    bool creatable = sel != nullptr;
    for (uint32_t& it : t.reagentItems) it = 0;
    if (sel) {
        const pp::CraftRecipeRec& rec = *sel->rec;
        const Rect view = FR(kDetailX, kDetailY, kDetailW, kDetailH);
        // Child units -> px, scrolled.
        auto CX = [&](float x) { return PX(kDetailX + x); };
        auto CY = [&](float y) { return PY(kDetailY + y - t.detailScroll); };
        ui.pushClip(view);
        if (t.dheader.pixels) ui.image(t.dheader, Rect{CX(0), CY(-3), 320 * kSX, 64 * kSY});
        // TradeSkillSkillIcon (37 x 37 at 8,-3): GetTradeSkillIcon - what it makes, else the spell's icon.
        const float side = 37.0f * kSY;
        const Rect icon{CX(8) + (37 * kSX - side) * 0.5f, CY(3), side, side};
        const uint32_t made = rec.madeItem;
        const pp::Texture* itex = made ? itemIcons().byDisplay(db.displayOf(made), spellFrame_)
                                       : spellDb_.icon(sel->spellId, spellFrame_);
        if (itex) ui.image(*itex, icon);
        else ui.fill(icon, 0xFF303030u);
        // TradeSkillSkillIconCount (NumberFontNormal, BOTTOMRIGHT -5,2): GetTradeSkillNumMade.
        if (rec.madeMax > 1) {
            char b[24];
            if (rec.madeMin == rec.madeMax) std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(rec.madeMin));
            else std::snprintf(b, sizeof(b), "%u-%u", static_cast<unsigned>(rec.madeMin), static_cast<unsigned>(rec.madeMax));
            if (static_cast<float>(fs.measure(b)) > 39.0f * kSX)
                std::snprintf(b, sizeof(b), "~%u", static_cast<unsigned>((rec.madeMin + rec.madeMax) / 2u));
            ui.textShadow(fs, icon.x + icon.w - 1, icon.y + icon.h - lh + 1, b, kColWhite, Align::Right);
        }
        const bool iconVisible = icon.y + icon.h * 0.5f >= view.y && icon.y + icon.h * 0.5f <= view.y + view.h;
        if (live && iconVisible && winFocus_.addStop(kIdTsIcon, icon)) {  // TradeSkillItem_OnEnter
            if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, icon, 0xFFFFFFFFu, Blend::Add);
            const Rect anchor{icon.x + icon.w + ui.offsetX(), icon.y + ui.offsetY(), icon.w, icon.h};
            if (made) {  // GameTooltip:SetTradeSkillItem - the item
                session_.ensureItemInfo(made);
                tipProtoItem_ = made;
                tipProtoCount_ = 1;
                tipProtoRect_ = anchor;
            } else {  // an enchant: the spell
                tipSpellId_ = sel->spellId;
                tipSpellRect_ = anchor;
            }
        }
        // TradeSkillSkillName (50,-5, 244 wide, GameFontNormal), wrapped in the small font when long.
        float y = 5.0f;  // child units
        {
            const float nw = 244 * kSX;
            if (static_cast<float>(fn.measure(sel->name.c_str())) <= nw) {
                ui.textShadow(fn, CX(50), CY(y), sel->name.c_str(), kColGold);
                y += static_cast<float>(fn.lineHeight()) / kSY;
            } else {
                for (const std::string& l : wrapQuestText(fs, sel->name, nw)) {
                    ui.textShadow(fs, CX(50), CY(y), l.c_str(), kColGold);
                    y += lh / kSY;
                }
            }
        }
        // TradeSkillSkillCooldown (GameFontRedSmall): COOLDOWN_REMAINING .." ".. SecondsToTime(GetTradeSkillCooldown).
        const float cd = session_.spellCooldown(sel->spellId);
        if (cd > 0.0f) {
            const std::string text = std::string(CLIENT_STR(COOLDOWN_REMAINING)) + " " + secondsToTime(cd);
            const float cw = 244 * kSX;
            if (static_cast<float>(ft.measure(text.c_str())) <= cw) ui.textShadow(ft, CX(50), CY(y), text.c_str(), kColRedSmall);
            else ui.textMarquee(ft, Rect{CX(50), CY(y), cw, th}, text.c_str(), kColRedSmall);
            y += th / kSY;
        }
        // TradeSkillDescription (5,-50, 290 wide, GameFontHighlightSmall): GetTradeSkillDescription.
        float dy = std::max(50.0f, y + 2.0f);
        const char* desc = db.str(rec.descOff);
        if (desc[0]) {
            for (const std::string& l : wrapQuestText(fs, desc, 290 * kSX)) {
                ui.textShadow(fs, CX(5), CY(dy), l.c_str(), kColWhite);
                dy += lh / kSY;
            }
            dy += 10.0f;  // TradeSkillReagentLabel: TOPLEFT at the description's BOTTOMLEFT 0,-10
        }
        // TradeSkillReagentLabel (GameFontNormalSmall): SPELL_REAGENTS ("...: |n" - the |n: an empty line, below).
        const pp::CraftReagentRec* rg = db.reagents(rec);
        const int numReagents = std::min<int>(rec.reagentCount, kMaxReagents);
        if (numReagents > 0) {
            std::string label = CLIENT_STR(SPELL_REAGENTS);
            while (!label.empty() && (label.back() == '\n' || label.back() == ' ')) label.pop_back();
            ui.textShadow(fs, CX(5), CY(dy), label.c_str(), kColGold);
            dy += lh / kSY + 3.0f;  // TradeSkillReagent1: TOPLEFT at the label's BOTTOMLEFT -2,-3
        }
        // TradeSkillReagent1..8 (QuestItemTemplate 147 x 41: two columns, 43 units a row): the icon (39 x 39),
        // UI-QuestItemNameFrame, the name (90 x 36 at the frame +15), "have /need" (NumberFontNormal).
        for (int k = 0; k < numReagents; ++k) {
            const uint32_t item = rg[k].item, need = rg[k].count;
            const uint32_t have = session_.countItemInBags(item);
            const bool enough = have >= need;
            if (!enough) creatable = false;
            const float bx = 3.0f + 147.0f * static_cast<float>(k % 2), by = dy + 43.0f * static_cast<float>(k / 2);
            const Rect ic2{CX(bx), CY(by), 39 * kSY, 39 * kSY};
            if (t.nameFrame.pixels) ui.image(t.nameFrame, Rect{CX(bx + 29), CY(by + 19.5f - 32), 128 * kSX, 64 * kSY});
            const pp::Texture* rtex = itemIcons().byDisplay(db.displayOf(item), spellFrame_);
            const uint32_t tint = enough ? 0xFFFFFFFFu : 0xFF808080u;  // SetItemButtonTextureVertexColor 0.5
            if (rtex) ui.image(*rtex, ic2, tint);
            else ui.fill(ic2, 0xFF202020u);
            // The name (GetTradeSkillReagentInfo: the item cache; asked for here).
            session_.ensureItemInfo(item);
            const auto* info = session_.getItemInfo(item);
            const std::string rname = info && info->valid ? info->name : std::string();
            const Rect nb{CX(bx + 44), CY(by + 19.5f - 18), 90 * kSX, 36 * kSY};
            const uint32_t ncol = enough ? kColWhite : kColGray;
            if (!rname.empty()) {
                if (static_cast<float>(fs.measure(rname.c_str())) <= nb.w) {
                    ui.textShadow(fs, nb.x, nb.y + (nb.h - lh) * 0.5f, rname.c_str(), ncol);
                } else {
                    const auto lines = wrapQuestText(ft, rname, nb.w);
                    if (lines.size() <= 2) {
                        float ly = nb.y + (nb.h - th * static_cast<float>(lines.size())) * 0.5f;
                        for (const std::string& l : lines) {
                            ui.textShadow(ft, nb.x, ly, l.c_str(), ncol);
                            ly += th;
                        }
                    } else {
                        ui.textMarquee(ft, Rect{nb.x, nb.y + (nb.h - th) * 0.5f, nb.w, th}, rname.c_str(), ncol);
                    }
                }
            }
            char b[24];
            if (have >= 100) std::snprintf(b, sizeof(b), "* /%u", static_cast<unsigned>(need));
            else std::snprintf(b, sizeof(b), "%u /%u", static_cast<unsigned>(have), static_cast<unsigned>(need));
            ui.textShadow(ft, ic2.x + ic2.w, ic2.y + ic2.h - th, b, kColWhite, Align::Right);
            // Focus (the tooltip), only when on view; the details follow a focused reagent.
            const float midY = ic2.y + ic2.h * 0.5f;
            const uint32_t rid = kIdTsReagent0 + static_cast<uint32_t>(k);
            if (live && midY >= view.y && midY <= view.y + view.h &&
                winFocus_.addStop(rid, Rect{ic2.x, ic2.y, nb.x + nb.w - ic2.x, ic2.h})) {
                if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, ic2, 0xFFFFFFFFu, Blend::Add);
                t.reagentItems[k] = item;
                if (info && info->valid) {  // TradeSkillItemTemplate OnEnter: GameTooltip:SetTradeSkillItem(.., n)
                    tipProtoItem_ = item;
                    tipProtoCount_ = 1;
                    tipProtoRect_ = Rect{ic2.x + ic2.w + ui.offsetX(), ic2.y + ui.offsetY(), ic2.w, ic2.h};
                }
                if (ic2.y + ic2.h > view.y + view.h) t.detailScroll += (ic2.y + ic2.h - view.y - view.h) / kSY + 2.0f;
                if (ic2.y < view.y) t.detailScroll -= (view.y - ic2.y) / kSY + 2.0f;
            }
        }
        const float contentH = numReagents > 0 ? dy + 43.0f * static_cast<float>((numReagents + 1) / 2) : dy;
        ui.popClip();
        t.detailMax = std::max(0.0f, contentH - kDetailH);
        t.detailScroll = std::clamp(t.detailScroll, 0.0f, t.detailMax);
        if (t.detailMax > 0.0f) {  // the detail frame's scroll bar (ScrollFrame_OnScrollRangeChanged)
            const bool canUp = t.detailScroll > 0.0f, canDn = t.detailScroll < t.detailMax;
            const Rect up{PX(321), PY(236), 11, 11}, dn{PX(321), PY(410) - 12, 11, 11};
            const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
            const pp::Texture& td = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
            if (tu.pixels) ui.image(tu, up);
            if (td.pixels) ui.image(td, dn);
            if (live && canUp && winFocus_.addStop(kIdTsDUp, up)) ui.fill(up, 0x40FFFFFFu);
            if (live && canDn && winFocus_.addStop(kIdTsDDown, dn)) ui.fill(dn, 0x40FFFFFFu);
        }
    } else {
        t.detailMax = 0.0f;
        t.detailScroll = 0.0f;
    }

    // --- The buttons (CENTER y -422) ---------------------------------------------------------------------
    {
        const float bh = 14.0f, cy = PY(422);
        auto button = [&](uint32_t id, float x, const char* label, bool enabled) {
            const Rect r{PX(x), cy - bh * 0.5f, 80 * kSX, bh};
            // PSP: a disabled CREATE keeps a focus stop - Cross on it says why on the red line.
            if (!enabled && live && sel && (id == kIdTsCreate || id == kIdTsCreateAll)) winFocus_.addStop(id, r);
            drawPanelButton(ui, id, r, "", enabled, live);
            const Font& bf = static_cast<float>(fs.measure(label)) <= r.w - 4 ? fs : ft;
            const bool pressed = enabled && winFocus_.isPressed(id);
            ui.textShadow(bf, r.x + r.w * 0.5f + (pressed ? 1.0f : 0.0f), r.y + (r.h - static_cast<float>(bf.lineHeight())) * 0.5f,
                          label, enabled ? 0xFF00D1FFu : 0xFF808080u, Align::Center);  // GameFontNormal / Disable
        };
        if (!altVerb) {
            button(kIdTsCreateAll, 18, CLIENT_STR(CREATE_ALL), creatable);  // RIGHT at Create's LEFT -86
            // TradeSkillDecrementButton / TradeSkillInputBox / TradeSkillIncrementButton.
            auto arrow = [&](uint32_t id, float x, const pp::Texture& up) {
                const Rect r = FR(x, 411, 23, 22);
                const bool f = live && winFocus_.addStop(id, r);
                if (up.pixels) ui.image(up, r);
                if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
            };
            arrow(kIdTsDec, 101, t.prev);
            if (t.input.pixels) ui.image(t.input, FR(123, 412, 35, 20));
            char b[8];
            std::snprintf(b, sizeof(b), "%d", t.count);
            ui.textShadow(fs, PX(143), PY(422) - lh * 0.5f, b, kColWhite, Align::Center);  // ChatFontNormal
            arrow(kIdTsInc, 158, t.next);
        }
        // TradeSkillCreateButton: CREATE, or the line's AlternateVerb (altVerb) for a recipe without an item.
        button(kIdTsCreate, 184, altVerb ? db.altVerb() : CLIENT_STR(CREATE), creatable);
        button(kIdTsExit, 265, CLIENT_STR(EXIT), true);  // TradeSkillCancelButton
    }

    // --- The open dropdown list (DropDownList1 under its button; single choice, the checked one ticked) ---------
    if (t.ddOpen != 0) {
        const bool sub = t.ddOpen == 1;
        const std::vector<std::string>& names = sub ? t.subclasses : t.slots;
        const int pick = sub ? t.subPick : t.slotPick;
        const int n = std::min(kMaxDDItems, static_cast<int>(names.size()) + 1);
        auto itemText = [&](int k) -> const char* {
            if (k == 0) return sub ? CLIENT_STR(ALL_SUBCLASSES) : CLIENT_STR(ALL_INVENTORY_SLOTS);
            return names[static_cast<size_t>(k) - 1].c_str();
        };
        float wmax = 0;
        for (int k = 0; k < n; ++k) wmax = std::max(wmax, static_cast<float>(fs.measure(itemText(k))));
        const float x0 = PX(sub ? 54.0f + 10.0f : 189.0f + 10.0f), top = PY(100);
        float ih = lh + 2;
        const Font* lf = &fs;
        if (top + ih * static_cast<float>(n) + 10 > static_cast<float>(pp::kScreenH) - ui.offsetY() - 2) {
            lf = &ft;  // PSP: a long list in the 8 px font
            ih = std::max(th, (static_cast<float>(pp::kScreenH) - ui.offsetY() - 12 - top) / static_cast<float>(n));
        }
        const float iw = std::max(wmax, static_cast<float>(lf->measure(itemText(0)))) + 30;
        const Rect lr{x0, top, iw + 8, ih * static_cast<float>(n) + 10};
        ui.boundsHold(true);
        if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{lr.x + 3, lr.y + 3, lr.w - 6, lr.h - 6}, 0xF0FFFFFFu);
        else ui.fill(lr, 0xF0100000u);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), lr);
        for (int k = 0; k < n; ++k) {
            const Rect ir{lr.x + 4, lr.y + 5 + ih * static_cast<float>(k), lr.w - 8, ih};
            const bool fi = focusable && winFocus_.addStop(kIdTsDDItem0 + static_cast<uint32_t>(k), ir);
            if (fi && t.hl.pixels) ui.image(t.hl, ir, 0xFFFFFFFFu, Blend::Add);
            if (k == pick && t.cbCheck.pixels) ui.image(t.cbCheck, Rect{ir.x + 2, ir.y + (ih - 12) * 0.5f, 12, 12});
            ui.textShadow(*lf, ir.x + 20, ir.y + (ih - static_cast<float>(lf->lineHeight())) * 0.5f, itemText(k),
                          kColWhite);
        }
        ui.boundsHold(false);
    }
    return true;
}

// After all the windows: the UIErrorsFrame stack over the frame (it sits where the window is), the check box's
// GameTooltip (SetText(CRAFT_IS_MAKEABLE_TOOLTIP, .., 1): wrapped).
void WorldScreen::drawTradeSkillOverlay(UiRenderer& ui) {
    if (!ts_ || !windowOpen(kWinTradeSkill)) return;
    if (!windowOpen(kWinTrainer) && !windowOpen(kWinTalents)) drawUiErrors(ui);  // (the trainer's overlay draws it)
    TradeSkillUi& t = *ts_;
    if (!t.tip || !uiMode_) return;
    const Font& fs = kit_.font(kFontSmall);
    const auto lines = wrapQuestText(fs, t.tip, 200.0f);
    float w = 0;
    for (const std::string& l : lines) w = std::max(w, static_cast<float>(fs.measure(l.c_str())));
    const float h = 8 + static_cast<float>(fs.lineHeight()) * static_cast<float>(lines.size());
    Rect r{t.tipAnchor.x - w - 16, t.tipAnchor.y, w + 14, h};  // ANCHOR_LEFT
    if (r.x < 2) r.x = t.tipAnchor.x + t.tipAnchor.w + 2;      // no room: on the right
    r.y = std::clamp(r.y, 2.0f, std::max(2.0f, static_cast<float>(pp::kScreenH) - r.h - 2));
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    float y = r.y + 4;
    for (const std::string& l : lines) {
        ui.text(fs, r.x + 7, y, l.c_str(), kColWhite);
        y += static_cast<float>(fs.lineHeight());
    }
}

}  // namespace wowee::ui::psp
