// The action bar = MainMenuBar of the original client (FrameXML
// MainMenuBar.xml / .lua, ActionBarFrame.xml, ActionButton.lua, TextStatusBar.lua), on 480x272 with the
// PSP buttons. The buttons themselves (icons, cooldowns, glyphs, range) stay in ui_world_spells.cpp.
//
//   - MainMenuBarArtFrame: MainMenuBarTexture0/1 (UI-MainMenuBar-Dwarf: the twelve ActionButton slots) and
//     the gryphons MainMenuBarLeftEndCap / RightEndCap (UI-MainMenuBar-EndCap-Dwarf, the right one mirrored),
//     one 256 x 64 T8 atlas "mmb_art" (tools/psp/build_ui_pak.py mainmenubar_art) at 0.5, drawn 1:1.
//     PSP: the bar holds the twelve buttons only - no micro buttons (START / the menu), no bag buttons (the
//     bags window has them), no page arrows (controls=2 has no pages): MainMenuBarTexture2/3 are left out
//     and the right cap closes the bar at its right end, as the left one opens it. 0.5 (ActionButton 36
//     units = 18 px, 42 apart = 21 px) puts the whole bar, caps included, between the tutorial alert
//     (bottom left, x ..54) and "Book" (bottom right in cursor mode, x 412..).
//   - MainMenuExpBar (MainMenuExpBar_Update, TextStatusBar): UnitXP / UnitXPMax = PLAYER_XP /
//     PLAYER_NEXT_LEVEL_XP (upstream lua_UnitXP / lua_UnitXPMax), coloured by GetRestState (rested blue,
//     normal purple), ExhaustionLevelFillBar + ExhaustionTick at UnitXP + GetXPExhaustion
//     (PLAYER_REST_STATE_EXPERIENCE) (ExhaustionTick_OnEvent); at MAX_PLAYER_LEVEL (80) the XP bar gives way
//     to MainMenuBarMaxLevelBar (ReputationWatchBar_Update). Text "XP 120 / 400" (prefix XP from
//     CharacterFrame_OnLoad, alwaysPrefix) under the cursor (OnEnter: ShowTextStatusBarText), while the
//     character window is open (CharacterFrame_OnShow) or always with wowee.cfg xpbartext=1 (CVar
//     xpBarText). ExhaustionToolTipText 1 s after the cursor came onto the bar (ExhaustionTick.timer), at
//     once on the tick.
//   - ActionButton_UpdateUsable: IsUsableAction (upstream lua_IsUsableAction: known + the power cost) with
//     the original's cost and checks from spells.pak "suse" (ManaCostPercentage, the form, the aura states,
//     the reagents) - see actionUsable().
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using game::ActionBarSlot;
using game::UF;

namespace {

// MainMenuBar at 0.5: MainMenuBarTexture0's left edge (px) and the 512 units of Texture0 + 1.
constexpr float kS = 0.5f;
constexpr float kBarX = 106.0f, kBarW = 512.0f * kS;
constexpr float kBottom = static_cast<float>(pp::kScreenH);
// Rows (px), MainMenuBar units counted up from the screen's bottom edge, snapped to whole pixels:
constexpr float kXpY = 245.0f, kXpH = 7.0f;    // MainMenuExpBar: units 40..53
constexpr float kXpOverY = 245.0f;             // MainMenuXPBarTexture0/1: units 43..53 (BOTTOM 0,3 of the bar)
constexpr float kMaxY = 247.0f;                // MainMenuMaxLevelBar0/1: units 42..49
constexpr float kArtY = 250.0f;                // MainMenuBarTexture0/1: units 0..43
constexpr float kCapY = 234.0f;                // the end caps' drawn rows: units 0..76
constexpr float kCapLX = kBarX - 96.0f * kS;   // MainMenuBarLeftEndCap: BOTTOM -544 = 96 units left of the bar
constexpr float kCapRX = kBarX + kBarW - 32.0f * kS;  // RightEndCap: 32 units over the bar's right end
constexpr float kTickY = 241.0f;               // ExhaustionTick 32 x 32: CENTER of the XP bar (units 46.5)
constexpr float kTextY0 = 246.0f, kTextH = 4.0f;  // MainMenuBarExpText: CENTER 0,1 of the bar = the open strip

// Atlas "mmb_art" (build_ui_pak.py mainmenubar_art), content texels.
constexpr SrcRect kSrcBar[2] = {{0, 0, 128, 22}, {0, 22, 128, 22}};
constexpr SrcRect kSrcXp[2] = {{0, 44, 128, 5}, {0, 49, 128, 5}};
constexpr SrcRect kSrcMax[2] = {{0, 54, 128, 4}, {0, 58, 128, 4}};
constexpr SrcRect kSrcCapL{128, 0, 64, 38}, kSrcCapR{192, 0, 64, 38};
constexpr SrcRect kSrcTick{128, 40, 16, 16}, kSrcTickHl{144, 40, 16, 16};

constexpr uint32_t kMaxPlayerLevel = 80;  // MAX_PLAYER_LEVEL_TABLE[GetAccountExpansionLevel()] (3.3.5a)

// 3.3.5a UpdateFields.h (OBJECT_END 6) - not in upstream's UF table:
constexpr uint16_t kUnitFieldBaseMana = 120;       // UNIT_FIELD_BASE_MANA (OBJECT_END + 0x72)
constexpr uint16_t kUnitFieldBaseHealth = 121;     // UNIT_FIELD_BASE_HEALTH (+ 0x73)
constexpr uint16_t kUnitFieldBytes2 = 122;         // UNIT_FIELD_BYTES_2 (+ 0x74): byte 3 = the form
constexpr uint16_t kUnitFieldCostModifier = 131;   // UNIT_FIELD_POWER_COST_MODIFIER (+ 0x7D), int32[7]
constexpr uint16_t kUnitFieldCostMultiplier = 138; // UNIT_FIELD_POWER_COST_MULTIPLIER (+ 0x84), float[7]

constexpr uint16_t kSpellAuraModShapeshift = 36;          // SPELL_AURA_MOD_SHAPESHIFT
constexpr uint16_t kSpellAuraIgnoreAuraState = 262;       // SPELL_AURA_ABILITY_IGNORE_AURASTATE
constexpr uint32_t kSpellAttr0NotShapeshift = 0x00010000; // SPELL_ATTR0_NOT_SHAPESHIFT
constexpr uint32_t kSpellAttr1UseAllMana = 0x00000002;    // SPELL_ATTR1_USE_ALL_MANA
constexpr uint16_t kShapeshiftFlagStance = 0x0001;        // SHAPESHIFT_FLAG_STANCE (acts as no form)
constexpr uint16_t kShapeshiftFlagOnlyForm = 0x0400;      // SHAPESHIFT_FLAG_CAN_ONLY_CAST_SHAPESHIFT_SPELLS

// Exhaustion.dbc: GetRestState's name and multiplier by the PLAYER_BYTES_2 rest state. PSP: the names
// are own strings (REST_STATE_*) until the strings.pak baker carries Exhaustion.dbc.
struct RestState {
    platform::psp::PS name;
    int percent;  // exhaustionStateMultiplier * 100
};
using platform::psp::PS;
constexpr RestState kRestStates[7] = {
    {PS::kCount, 100},              // ps(): ""
    {PS::REST_STATE_RESTED, 200},   // 1 rested (Factor 2.0)
    {PS::REST_STATE_NORMAL, 100},   // 2 normal
    {PS::REST_STATE_TIRED, 100},    // 3 (the client's own placeholder names)
    {PS::REST_STATE_TIRED, 50},     // 4
    {PS::REST_STATE_EXHAUSTED, 25}, // 5
    {PS::REST_STATE_RAF, 200},      // 6 Recruit-a-Friend
};

float asFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

bool hasState(uint32_t states, uint8_t st) { return st > 0 && st <= 32 && (states & (1u << (st - 1))) != 0; }

// SpellInfo::CheckShapeshift (AzerothCore; the client greys the button on the same rules).
bool formAllows(const pp::SpellInfo& si, uint8_t form, const pp::SpellDb& db) {
    // PSP: the shapeshift / stance spells themselves stay usable - the client switches forms with them.
    for (uint16_t a : si.rec.aura)
        if (a == kSpellAuraModShapeshift) return true;
    const uint32_t mask = form > 0 && form <= 32 ? 1u << (form - 1) : 0u;
    if (mask & si.stancesNot) return false;  // SPELL_FAILED_NOT_SHAPESHIFT
    if (mask & si.stances) return true;
    uint16_t flags = 0;
    bool shifted = false;
    if (form > 0) {
        if (!db.hasFormFlags()) return true;  // an older spells.pak: unknown form - CheckShapeshift lets it pass
        flags = db.formFlags(form);
        shifted = (flags & kShapeshiftFlagStance) == 0;
    }
    if (shifted) {
        if (si.rec.attributes & kSpellAttr0NotShapeshift) return false;
        if (si.stances != 0) return false;  // needs another form
    } else if (!(si.useFlags & pp::SpellInfo::kUseNotNeedShapeshift) && si.stances != 0) {
        return false;  // needs a form
    }
    if (form > 0 && (flags & kShapeshiftFlagOnlyForm) && !(mask & si.stances)) return false;
    return true;
}

}  // namespace

WorldScreen::MmbArt::~MmbArt() { pp::textureRelease(tex); }

Rect WorldScreen::mmbButton(int i) const {
    // ActionButton1: BOTTOMLEFT 8,4 of MainMenuBarArtFrame; ActionButtonN: LEFT of N-1's RIGHT +6 (36 wide).
    return Rect{kBarX + (8.0f + 42.0f * static_cast<float>(i)) * kS, kBottom - 40.0f * kS, 36.0f * kS, 36.0f * kS};
}

// With the spell art (initSpells, ui.pak open). Kept for the session: the bar is always on screen.
void WorldScreen::loadMainMenuBarArt(pp::Pak& pak, uint32_t& bytes) {
    mmbArt_.off = pp::configGetInt("mainmenubar", 1) == 0;
    xpTextAlways_ = pp::configGetInt("xpbartext", 0) != 0;
    uint32_t kb = 0;
    bool ok = false;
    if (!mmbArt_.off) {
        if (const pp::PakEntry* e = pak.find("mmb_art")) {
            void* blob = pak.load(e);
            if (blob && pp::textureFromBlob(blob, e->size, mmbArt_.tex, true)) {
                mmbArt_.tex.name = "mainmenubar";
                kb = (e->size + 1023u) / 1024u;
                bytes += e->size;
                ok = true;
            } else if (blob) {
                pp::pakFree(blob);
            }
        }
    }
    PSP_LOGI(pp::tag::Gfx, "mainmenubar_art ok=%d kb=%u off=%d xpbartext=%d", ok ? 1 : 0, static_cast<unsigned>(kb),
             mmbArt_.off ? 1 : 0, xpTextAlways_ ? 1 : 0);
}

// MainMenuExpBar_Update + ExhaustionTick_OnEvent (+ ReputationWatchBar_Update's XP / max level choice).
void WorldScreen::updateXpView() {
    XpView v;
    const uint32_t level = session_.selfField(game::fieldIndex(UF::UNIT_FIELD_LEVEL));
    v.cur = session_.selfField(game::fieldIndex(UF::PLAYER_XP));
    const uint32_t next = session_.selfField(game::fieldIndex(UF::PLAYER_NEXT_LEVEL_XP));
    v.max = next > 0 ? next : 1;  // upstream lua_UnitXPMax
    v.rested = session_.selfField(game::fieldIndex(UF::PLAYER_REST_STATE_EXPERIENCE));  // GetXPExhaustion (0 = nil)
    // GetRestState: the server's rest state, PLAYER_BYTES_2 byte 3 (1 rested, 2 normal, 6 RAF - Exhaustion.dbc);
    // before it arrives upstream's answer (rested while there is rested XP).
    const uint8_t rs = static_cast<uint8_t>(session_.selfField(game::fieldIndex(UF::PLAYER_BYTES_2)) >> 24);
    v.state = rs >= 1 && rs <= 6 ? rs : (v.rested > 0 ? 1 : 2);
    // newLevel < MAX_PLAYER_LEVEL and not IsXPUserDisabled() (upstream: never disabled). Level 0 = no fields yet.
    v.shown = level > 0 && level < kMaxPlayerLevel;
    v.maxLevelBar = level >= kMaxPlayerLevel;
    if (v.shown && v.rested > 0) {
        const float tickSet = std::max((static_cast<float>(v.cur) + static_cast<float>(v.rested)) /
                                           static_cast<float>(v.max) * kBarW,
                                       0.0f);
        if (!(tickSet > kBarW)) {
            v.tick = true;
            v.tickX = tickSet;
        }
    }
    // UPDATE_EXHAUSTION: SetStatusBarColor by the state (1 / 2 only; another keeps the last colour).
    if (v.state == 1) xpColor_ = 0xFFE06300u;       // (0.0, 0.39, 0.88)
    else if (v.state == 2) xpColor_ = 0xFF8C0094u;  // (0.58, 0.0, 0.55)
    xpView_ = v;
}

// "hud" pass (ui_world_hud.cpp): under the unit frames, the quest tracker and the chat - their text stays
// readable over the gryphons' heads (the bar's top 16 px rise beside them).
void WorldScreen::drawMainMenuBarArt(UiRenderer& ui) {
    if (!spellsInit_ || settings_.isOpen() || fxHides(kFxHideBar)) return;
    updateXpView();
    const XpView& v = xpView_;
    const pp::Texture& t = mmbArt_.tex;
    const bool art = t.pixels != nullptr;
    if (v.shown) {
        ui.fill(Rect{kBarX, kXpY, kBarW, kXpH}, 0x80000000u);  // BACKGROUND: black 0.5
        if (v.tick && v.tickX > 0.0f)                            // ExhaustionLevelFillBar (BORDER), alpha 0.15
            ui.fill(Rect{kBarX, kXpY, v.tickX, kXpH}, (xpColor_ & 0x00FFFFFFu) | 0x26000000u);
        const float f = std::clamp(static_cast<float>(v.cur) / static_cast<float>(v.max), 0.0f, 1.0f);
        if (f > 0.0f) {  // the StatusBar (UI-StatusBar, BarColor)
            const Rect fill{kBarX, kXpY, kBarW * f, kXpH};
            if (ufArt_.bar.pixels)
                ui.image(ufArt_.bar, fill, SrcRect{0, 0, ufArt_.bar.uw * f, static_cast<float>(ufArt_.bar.uh)}, xpColor_);
            else
                ui.fill(fill, xpColor_);
        }
        if (art) {  // OVERLAY: MainMenuXPBarTexture0/1
            ui.image(t, Rect{kBarX, kXpOverY, 128, 5}, kSrcXp[0]);
            ui.image(t, Rect{kBarX + 128, kXpOverY, 128, 5}, kSrcXp[1]);
        }
    } else if (v.maxLevelBar && art) {
        ui.image(t, Rect{kBarX, kMaxY, 128, 4}, kSrcMax[0]);
        ui.image(t, Rect{kBarX + 128, kMaxY, 128, 4}, kSrcMax[1]);
    }
    if (!art) return;
    // MainMenuBarArtFrame (raised over the XP bar): the slots, then the end caps (OVERLAY).
    ui.image(t, Rect{kBarX, kArtY, 128, 22}, kSrcBar[0]);
    ui.image(t, Rect{kBarX + 128, kArtY, 128, 22}, kSrcBar[1]);
    ui.image(t, Rect{kCapLX, kCapY, 64, 38}, kSrcCapL);
    ui.image(t, Rect{kCapRX, kCapY, 64, 38}, kSrcCapR);
}

// After the action buttons ("spells" pass): ExhaustionTick and MainMenuBarExpText (DIALOG strata, over the
// buttons), the cursor stops of the bar and the tick.
void WorldScreen::drawMainMenuBarOverlay(UiRenderer& ui) {
    xpTip_ = 0;
    const XpView& v = xpView_;
    if (!v.shown) {
        xpHoverMs_ = 0;
        return;
    }
    const bool clickable = uiMode_ && !settings_.isOpen();
    const bool barFocus = clickable && winFocus_.addStop(kIdXpBar, Rect{kBarX, kXpY, kBarW, kXpH});
    bool tickFocus = false;
    if (v.tick) {
        const Rect r{std::floor(kBarX + v.tickX - 8.0f + 0.5f), kTickY, 16, 16};
        if (clickable) tickFocus = winFocus_.addStop(kIdXpTick, r);
        if (mmbArt_.tex.pixels) {
            ui.image(mmbArt_.tex, r, kSrcTick);
            if (tickFocus)  // HighlightTexture (ADD), ExhaustionTickHighlight:SetVertexColor by the rest state
                ui.image(mmbArt_.tex, r, kSrcTickHl, xpColor_ | 0xFF000000u, Blend::Add);
        } else {
            ui.fill(Rect{r.x + 7, r.y + 3, 2, 10}, 0xFFC0C0C0u);
        }
    }
    // TextStatusBar_UpdateTextString: prefix XP (alwaysPrefix) + value / max; shown under the cursor
    // (lockShow), with the character window open, or always (CVar xpBarText).
    if (barFocus || xpTextAlways_ || windowOpen(kWinCharacter)) {
        char line[48];
        std::snprintf(line, sizeof(line), "%s %u / %u", CLIENT_STR(XP), static_cast<unsigned>(v.cur),
                      static_cast<unsigned>(v.max));
        const Font& tiny = kit_.font(kFontTiny);
        const float capH = std::round(static_cast<float>(tiny.pixelSize()) * 0.72f);  // as the unit frames' bars
        const float y = std::floor(kTextY0 + (kTextH - capH) * 0.5f + capH - static_cast<float>(tiny.ascent()) + 0.5f);
        ui.textShadow(tiny, kBarX + kBarW * 0.5f, y, line, kColWhite, Align::Center);
    }
    // ExhaustionTick.timer = 1 on the bar's OnEnter -> ExhaustionToolTipText after 1 s; the tick's OnEnter at once.
    const uint32_t now = pp::logUptimeMs();
    if (barFocus) {
        if (xpHoverMs_ == 0) xpHoverMs_ = now ? now : 1u;
        if (now - xpHoverMs_ >= 1000u) xpTip_ = 1;
    } else {
        xpHoverMs_ = 0;
    }
    if (tickFocus) xpTip_ = 1;
}

// ExhaustionToolTipText, drawn after the windows (ui_world_windows.cpp drawWindows). GameTooltip:SetText puts
// the whole text in the first line - GameTooltipHeaderText (font_normal). Anchored to the tick when it shows
// (ANCHOR_LEFT right of the screen's centre, else ANCHOR_RIGHT), else GameTooltip_SetDefaultAnchor (bottom
// right, above the bar).
void WorldScreen::drawXpTooltip(UiRenderer& ui) {
    if (xpTip_ == 0 || !uiMode_ || settings_.isOpen()) {
        xpTip_ = 0;
        return;
    }
    const XpView& v = xpView_;
    const RestState& rs = kRestStates[v.state <= 6 ? v.state : 0];
    const int percentXp = static_cast<int>(std::ceil(static_cast<double>(v.cur) / static_cast<double>(v.max) * 100.0));
    char xpText[48];
    std::snprintf(xpText, sizeof(xpText), "%u / %u  ( %d%% )", static_cast<unsigned>(v.cur),
                  static_cast<unsigned>(v.max), percentXp);  // XP_TEXT
    // EXHAUST_TOOLTIP1 (the state's name - drawn after XP_TEXT in its colour - then two lines of the multiplier).
    const std::string tip1 = pp::text::uiPlain(CLIENT_FMT(EXHAUST_TOOLTIP1, "", rs.percent));
    const size_t nl1 = tip1.find('\n'), nl2 = nl1 == std::string::npos ? nl1 : tip1.find('\n', nl1 + 1);
    const std::string line2 = nl1 == std::string::npos ? std::string() : tip1.substr(nl1 + 1, nl2 - nl1 - 1);
    const std::string line3 = nl2 == std::string::npos ? std::string() : tip1.substr(nl2 + 1);
    // append: IsResting() (PLAYER_FLAGS_RESTING) with GetXPExhaustion() and GetTimeToWellRested() ->
    // EXHAUST_TOOLTIP4 - upstream's GetTimeToWellRested answers nil (no rest timer), so never; rest states
    // 4 / 5 -> EXHAUST_TOOLTIP2 (AzerothCore sends 1, 2 or 6).
    std::string tip2;
    if (v.state == 4 || v.state == 5) {
        tip2 = pp::text::uiPlain(CLIENT_STR(EXHAUST_TOOLTIP2));
        tip2.erase(0, tip2.find_first_not_of('\n'));
    }
    const char* append = tip2.empty() ? nullptr : tip2.c_str();
    const Font& f = kit_.font(kFontNormal);
    const float lh = static_cast<float>(f.lineHeight() > 0 ? f.lineHeight() : 14);
    const float w1 = static_cast<float>(f.measure(xpText)), w1b = static_cast<float>(f.measure(pp::ps(rs.name)));
    float w = std::max({w1 + w1b, static_cast<float>(f.measure(line2.c_str())), static_cast<float>(f.measure(line3.c_str()))});
    if (append) w = std::max(w, static_cast<float>(f.measure(append)));
    const int lines = append ? 4 : 3;
    Rect r{0, 0, w + 16.0f, lh * static_cast<float>(lines) + 10.0f};
    if (v.tick) {
        const float tx = std::floor(kBarX + v.tickX - 8.0f + 0.5f);
        r.x = tx + 8.0f >= pp::kScreenW * 0.5f ? tx - r.w : tx + 16.0f;  // ANCHOR_LEFT / ANCHOR_RIGHT
        r.y = kTickY - r.h;
    } else {
        r.x = pp::kScreenW - 4.0f - r.w;
        r.y = kXpY - 1.0f - r.h;
    }
    r.x = std::max(2.0f, std::min(r.x, pp::kScreenW - 2.0f - r.w));
    r.y = std::max(2.0f, r.y);
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    float y = r.y + 5.0f;
    ui.text(f, r.x + 8, y, xpText, kColWhite);
    ui.text(f, r.x + 8 + w1, y, pp::ps(rs.name), 0xFF00D2FFu);  // |cffffd200
    y += lh;
    ui.text(f, r.x + 8, y, line2.c_str(), kColWhite);
    y += lh;
    ui.text(f, r.x + 8, y, line3.c_str(), kColWhite);
    if (append) ui.text(f, r.x + 8, y + lh, append, 0xFF0000FFu);  // |cffff0000
    xpTip_ = 0;  // set again by the next frame's drawMainMenuBarOverlay
}

// What IsUsableAction reads of the player and the target, once per frame (drawActionBar).
void WorldScreen::updateUseCtx() {
    UseCtx c;
    const uint16_t pw = game::fieldIndex(UF::UNIT_FIELD_POWER1), mpw = game::fieldIndex(UF::UNIT_FIELD_MAXPOWER1);
    for (int i = 0; i < 7; ++i) {
        c.power[i] = session_.selfField(static_cast<uint16_t>(pw + i));
        c.maxPower[i] = session_.selfField(static_cast<uint16_t>(mpw + i));
        c.costMod[i] = static_cast<int32_t>(session_.selfField(static_cast<uint16_t>(kUnitFieldCostModifier + i)));
        c.costMul[i] = asFloat(session_.selfField(static_cast<uint16_t>(kUnitFieldCostMultiplier + i)));
    }
    c.health = session_.selfField(game::fieldIndex(UF::UNIT_FIELD_HEALTH));
    c.baseHealth = session_.selfField(kUnitFieldBaseHealth);
    c.baseMana = session_.selfField(kUnitFieldBaseMana);
    c.form = static_cast<uint8_t>(session_.selfField(kUnitFieldBytes2) >> 24);
    const uint16_t as = game::fieldIndex(UF::UNIT_FIELD_AURASTATE);
    c.auraState = session_.selfField(as);
    const uint64_t t = session_.target();
    if (t != 0 && t == session_.playerGuid()) {
        c.haveTarget = true;
        c.targetAuraState = c.auraState;
    } else if (t != 0) {
        if (auto e = session_.entities().getEntity(t)) {
            c.haveTarget = true;
            c.targetAuraState = e->getField(as);
        }
    }
    // SPELL_AURA_ABILITY_IGNORE_AURASTATE on us (e.g. Sudden Death: Execute at any health) - every 250 ms.
    c.ignoreAuraState = useCtx_.ignoreAuraState;
    const uint32_t now = pp::logUptimeMs();
    if (now - useAuraScanMs_ >= 250u) {
        useAuraScanMs_ = now;
        c.ignoreAuraState = false;
        for (const game::AuraSlot& au : session_.playerAuras()) {
            if (au.isEmpty()) continue;
            if (const pp::SpellInfo* si = spellDb_.get(au.spellId)) {
                for (uint16_t a : si->rec.aura) c.ignoreAuraState |= a == kSpellAuraIgnoreAuraState;
            }
        }
    }
    useCtx_ = c;
}

uint32_t WorldScreen::reagentCount(uint32_t item) {
    if (session_.inventoryVersion() != reagentInvVersion_) {
        reagentCount_.clear();
        reagentInvVersion_ = session_.inventoryVersion();
    }
    auto it = reagentCount_.find(item);
    if (it != reagentCount_.end()) return it->second;
    const uint32_t n = session_.getInventory().countItem(item);  // upstream GetItemCount (bags, no bank)
    reagentCount_[item] = n;
    return n;
}

// IsUsableAction -> ActionButton_UpdateUsable. Upstream (lua_IsUsableAction): a spell is usable when known
// and affordable; items and macros always (the original; Spell::CheckCast / SpellInfo of
// AzerothCore, which the client mirrors): the form (Stances / StancesNot, SpellInfo::CheckShapeshift) and
// the aura states (CasterAuraState(Not) on us, TargetAuraState(Not) on the target - e.g. Execute below
// 20 %, Victory Rush after a kill, Revenge after a dodge) grey the button; the cost is
// SpellInfo::CalcPowerCost (ManaCost + ManaCostPercentage of the base mana / base health / max power, the
// school's POWER_COST_MODIFIER and MULTIPLIER) against the power of the spell's own type (upstream: the
// unit's shown power) - blue; missing reagents grey it. PSP: no SPELLMOD_COST (talents: the
// SMSG_SET_FLAT/PCT_SPELL_MODIFIER table is not kept), no rune costs, no totem / equipped item checks;
// an older spells.pak (no "suse") = upstream (the flat cost).
uint8_t WorldScreen::actionUsable(const ActionBarSlot& s) {
    if (s.type == ActionBarSlot::ITEM && s.id != 0) return itemActionUsable(s.id);  // none carried = grey
    if (s.type != ActionBarSlot::SPELL || s.id == 0) return 0;
    if (!session_.hasKnownSpell(s.id)) return 2;
    const pp::SpellInfo* si = spellDb_.get(s.id);
    if (!si) return 0;
    const UseCtx& c = useCtx_;
    if (!formAllows(*si, c.form, spellDb_)) return 2;
    // HasAuraState: the IGNORE_AURASTATE aura answers "has it" (PSP: without the aura's spell mask).
    if (si->auraState[0] && !c.ignoreAuraState && !hasState(c.auraState, si->auraState[0])) return 2;
    if (si->auraState[2] && hasState(c.auraState, si->auraState[2])) return 2;
    if (c.haveTarget) {
        if (si->auraState[1] && !c.ignoreAuraState && !hasState(c.targetAuraState, si->auraState[1])) return 2;
        if (si->auraState[3] && hasState(c.targetAuraState, si->auraState[3])) return 2;
    }
    const uint8_t pt = si->rec.powerType;  // 255 = POWER_HEALTH (-2)
    if (pt != 5 && !(si->rec.attributesEx & kSpellAttr1UseAllMana)) {  // 5 POWER_RUNE: PSP - not checked
        float cost = static_cast<float>(si->rec.manaCost);
        if (si->manaPct) {
            const float pct = static_cast<float>(si->manaPct) / 100.0f;
            if (pt == 255) cost += static_cast<float>(static_cast<int32_t>(static_cast<float>(c.baseHealth) * pct));
            else if (pt == 0) cost += static_cast<float>(static_cast<int32_t>(static_cast<float>(c.baseMana) * pct));
            else if (pt >= 1 && pt <= 4)
                cost += static_cast<float>(static_cast<int32_t>(static_cast<float>(c.maxPower[pt]) * pct));
        }
        int school = 0;  // GetFirstSchoolInMask
        for (int k = 0; k < 7; ++k)
            if (si->rec.schoolMask & (1u << k)) {
                school = k;
                break;
            }
        cost += static_cast<float>(c.costMod[school]);
        const int32_t total = static_cast<int32_t>(cost * (1.0f + c.costMul[school]));
        if (total > 0) {
            const uint32_t have = pt == 255 ? c.health : pt < 7 ? c.power[pt] : 0xFFFFFFFFu;
            if (have < static_cast<uint32_t>(total)) return 1;
        }
    }
    for (uint32_t r : si->reagents)
        if (reagentCount(r & 0x00FFFFFFu) < (r >> 24)) return 2;  // SPELL_FAILED_REAGENTS
    return 0;
}

}  // namespace wowee::ui::psp
