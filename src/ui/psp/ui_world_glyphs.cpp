// GlyphFrame
// (the glyph add-on's .xml / .lua over PlayerTalentFrame, the talent add-on's GLYPH_TALENT_TAB) on the world screen, and the
// dispatchers of the three frames of this branch (GgbUi's life, the window hooks of ui_world_windows.cpp).
//   - the tab GLYPHS (PlayerGlyphTabTemplate) after the tree tabs from SHOW_INSCRIPTION_LEVEL (15), not on the pet's
//     spec; selected, GlyphFrame covers the tree (UI-GlyphFrame 352 x 441 at TOPLEFT, GlyphFrameTitleText GLYPHS);
//   - GlyphFrameGlyph1..6 (GlyphTemplate) at their XML anchors: 1 / 4 / 6 major, 2 / 3 / 5 minor by GlyphSlot.Type
//     (GlyphFrameGlyph_SetGlyphType: setting 108 / 86, ring 82 / 62, background 70 / 64, the rune tinted GLYPH_MAJOR
//     1,.25,0 / GLYPH_MINOR 0,.25,1), locked (UI-GlyphFrame-Locked), empty (GLYPH_SLOTS[0]), filled (GLYPH_SLOTS[id] +
//     the rune icon of GlyphProperties); GlyphFrameGlyph_OnUpdate's setting pulse (6 s) and the tint of the sockets a
//     glyph on the cursor fits (1.6 s); GlyphFrame_OnUpdate's sparkles (GlyphSparkleTexture along slotAnimations);
//     GlyphFrame_PulseGlow (UI-GlyphFrame-Glow) and the Glyph_Major / Minor Create / Destroy sounds on GLYPH_ADDED /
//     _UPDATED / _REMOVED;
//   - USE_GLYPH (a glyph item used: the session's glyphUseItem) -> OpenGlyphFrame: the talents window on the glyph tab;
//     the glyph's icon follows the cursor (SpellIsTargeting); Cross on a socket = GlyphFrameGlyph_OnClick: a filled
//     socket the glyph fits -> StaticPopup CONFIRM_GLYPH_PLACEMENT, else PlaceGlyphInSocket; START = SpellStopTargeting;
//   - Circle on a filled socket = the right click (PSP: without Shift) -> CONFIRM_REMOVE_GLYPH -> RemoveGlyphFromSocket;
//   - GameTooltip:SetGlyph of the focused socket.
// Memory: gly_frame (T8, 65 KB) + gly_parts (32 KB) + gly_locked 8 KB + the shown runes (2 KB each) while the glyph tab
// shows; gly_glow (65 KB) only during a pulse (1.6 s). Log: glyph_art kb=, glyph_art_free, glyph_tab, glyph_glow.
#include "ui/psp/ui_world_glyphs_guildbank.hpp"

#include "game/inventory_slots.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <pspctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// PlayerTalentFrame's units -> px (ui_world_trainer.cpp kSX / kSY / kOx / kOy).
constexpr float kSX = 0.6f, kSY = 0.55f, kOx = 4.0f, kOy = 2.0f;
float PX(float x) { return kOx + x * kSX; }
float PY(float y) { return kOy + y * kSY; }
constexpr int kInscriptionLevel = 15;  // SHOW_INSCRIPTION_LEVEL (Constants.lua)

// GlyphFrameGlyph1..6: the centres of the XML anchors in the 384 x 512 frame (TOP-LEFT origin, y down).
constexpr float kSocketX[6] = {177.0f, 178.0f, 73.0f, 283.0f, 283.0f, 71.0f};
constexpr float kSocketY[6] = {116.0f, 359.0f, 178.0f, 299.0f, 178.0f, 299.0f};
// slotAnimations[id]: start / stop from the frame's CENTER (192, 256), y up.
constexpr float kSparkle[6][4] = {{-13, 17, -13, 100}, {-13, 17, -13, -64}, {-13, 17, -85, 60},
                                  {-13, 18, 61, -27},  {-13, 17, 59, 60},   {-13, 18, -87, -27}};
constexpr float kSparkleSize[3] = {7.0f, 10.0f, 13.0f};           // SparkleTextureSmall / KindaSmall / Normal
constexpr float kSparkleDuration[3] = {1.25f, 3.0f, 5.4f};        // size x GLYPH_DURATION_MODIFIERS[size]

// gly_parts: the atlas tools/psp/build_ui_pak.py bakes (256 x 128 px, the pieces at their drawn sizes).
struct Piece {
    float x, y, w, h;
};
constexpr Piece kSetMajor{0, 0, 65, 59}, kSetMinor{65, 0, 52, 47}, kRingMajor{117, 0, 49, 45}, kRingMinor{166, 0, 37, 34},
    kBgEmpty{203, 0, 42, 39}, kShineMajor{0, 100, 10, 9}, kShineMinor{10, 100, 10, 9};
Piece bgSlot(int socket) { return Piece{42.0f * static_cast<float>(socket), 60.0f, 42.0f, 39.0f}; }

uint32_t alphaWhite(float a) {
    const uint32_t v = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (v << 24) | 0x00FFFFFFu;
}
uint32_t abgr(float r, float g, float b, float a = 1.0f) {
    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

// GLYPH_SLOT_TOOLTIP1..6.
const char* slotTooltip(int order) {
    static constexpr pp::CS k[6] = {pp::CS::GLYPH_SLOT_TOOLTIP1, pp::CS::GLYPH_SLOT_TOOLTIP2, pp::CS::GLYPH_SLOT_TOOLTIP3,
                                    pp::CS::GLYPH_SLOT_TOOLTIP4, pp::CS::GLYPH_SLOT_TOOLTIP5, pp::CS::GLYPH_SLOT_TOOLTIP6};
    return order >= 1 && order <= 6 ? pp::cs(k[order - 1]) : "";
}

// The glyph rows outlive any world screen (the session keeps a pointer): one copy for the program.
gbk::GlyphData g_glyphData;
bool g_glyphDataLoaded = false;

}  // namespace

// --- GgbUi -----------------------------------------------------------------------------------------------------------

WorldScreen::GgbUi::~GgbUi() {
    freeGlyphArt();
    freeBankArt();
    freeChannelArt();
}

void WorldScreen::GgbUi::freeGlyphArt() {
    for (pp::Texture* t : {&glyFrame, &glyParts, &glyLocked, &glyGlow, &glySparkle})
        if (t->pixels) pp::textureRelease(*t);
    for (int i = 0; i < 21; ++i) {
        if (rune[i].pixels) pp::textureRelease(rune[i]);
        runeTried[i] = false;
    }
    if (glyArtTried) PSP_LOGI(pp::tag::Gfx, "glyph_art_free kb=%u", static_cast<unsigned>(glyArtBytes / 1024u));
    glyArtTried = false;
    glyGlowTried = false;
    glyArtBytes = 0;
}

void WorldScreen::destroyGgbUi(GgbUi* p) { delete p; }

WorldScreen::GgbUi& WorldScreen::ggbUi() {
    if (!ggb_) {
        ggb_ = std::unique_ptr<GgbUi, void (*)(GgbUi*)>(new GgbUi, &WorldScreen::destroyGgbUi);
        GgbUi& g = *ggb_;
        // A map transfer makes a new world screen: the session's old events are taken as seen.
        g.useGlyphSeen = session_.useGlyphSeq();
        g.gbOpenSeen = session_.guildBankOpenSeq();
        g.gbTextSeen = session_.guildBankTextSeq();
        g.gbTextChangedSeen = session_.guildBankTextChangedSeq();
        g.gbLogSeen = session_.guildBankLogVersion();
        PSP_LOGI(pp::tag::Ctrl, "ggb_ui_new use_glyph=%u bank_open=%u", static_cast<unsigned>(g.useGlyphSeen),
                 static_cast<unsigned>(g.gbOpenSeen));
    }
    return *ggb_;
}

// drawWindows, every frame.
void WorldScreen::updateGlyphsGuildBank() {
    PROF_SCOPE("ggb.update");
    GgbUi& g = ggbUi();
    const uint32_t now = pp::logUptimeMs();
    const float dt = g.lastMs ? std::min(0.1f, static_cast<float>(now - g.lastMs) / 1000.0f) : 0.0f;
    g.lastMs = now;
    if (!g_glyphDataLoaded) {  // text.pak "glyph_data", once per program
        g_glyphDataLoaded = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/text.pak");
        pp::Pak pak;
        bool ok = false;
        if (pak.open(path)) {
            if (const pp::PakEntry* e = pak.find("glyph_data")) {
                if (auto* b = static_cast<const uint8_t*>(pak.load(e))) {
                    ok = gbk::readGlyphData(b, e->size, g_glyphData);
                    pp::pakFree(const_cast<uint8_t*>(b));
                }
            }
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "glyph_data ok=%d slots=%u props=%u spells=%u", ok ? 1 : 0,
                 static_cast<unsigned>(g_glyphData.slots.size()), static_cast<unsigned>(g_glyphData.props.size()),
                 static_cast<unsigned>(g_glyphData.spellGlyph.size()));
    }
    if (!session_.hasGlyphData() && !g_glyphData.props.empty()) session_.setGlyphData(&g_glyphData);
    session_.glyphUpdate();
    glyphUpdate(dt);
    guildBankUpdate();
    channelsUpdate();
}

bool WorldScreen::isGgbPopup(uint8_t w) const {
    return w == kWinGuildBankPopup || w == kWinGlyphConfirm || w == kWinChannelDaughter || w == kWinChannelDropDown ||
           w == kWinChannelPopup || w == kWinGuildBankTabPopup;
}

bool WorldScreen::ggbKeepsPopupArt() const {
    for (uint8_t w : {kWinGuildBankPopup, kWinGlyphConfirm, kWinChannelDaughter, kWinChannelPopup, kWinGuildBankTabPopup})
        if (windowOpen(w)) return true;
    return false;
}

bool WorldScreen::drawGgbWindow(UiRenderer& ui, uint8_t w) {
    switch (w) {
        case kWinGuildBank: return drawGuildBankFrame(ui);
        case kWinGuildBankPopup: return drawGuildBankPopup(ui);
        case kWinGuildBankTabPopup: return drawGuildBankTabPopup(ui);
        case kWinGlyphConfirm: return drawGlyphConfirm(ui);
        case kWinChannelDaughter: return drawChannelDaughter(ui);
        case kWinChannelDropDown: return drawChannelDropDown(ui);
        case kWinChannelPopup: return drawChannelPopup(ui);
        default: return false;
    }
}

bool WorldScreen::ggbAction(uint32_t act) {
    if (act >= kIdGlyphTab && act <= kIdGlyphNo) return glyphAction(act, false);
    if (act >= kIdChRow0 && act < kIdGbClose) return channelAction(act, false);
    if (act >= kIdGbClose && act <= 0x8FFF) return guildBankAction(act, false);
    if (ggb_ && ggb_->gbHeld.active && guildBankBagClick(act, false)) return true;  // a vault item put into a bag
    return false;
}

bool WorldScreen::ggbAltAction(uint32_t alt) {
    if (alt >= kIdGlyphTab && alt <= kIdGlyphNo) return glyphAction(alt, true);
    if (alt >= kIdChRow0 && alt < kIdGbClose) return channelAction(alt, true);
    if (alt >= kIdGbClose && alt <= 0x8FFF) return guildBankAction(alt, true);
    if (session_.guildBankOpen() && windowOpen(kWinGuildBank) && guildBankBagClick(alt, true)) return true;
    return false;
}

void WorldScreen::ggbWindowPad(pp::PadState& pad) {
    if (!ggb_) return;
    // A click outside the open channel list closes it (UIDropDownMenu: its own buttons are the only stops on it).
    if ((pad.pressed & PSP_CTRL_CROSS) && windowOpen(kWinChannelDropDown)) {
        auto r = winRect_.find(kWinChannelDropDown);
        const uint32_t f = winFocus_.focused();
        const bool inList = r != winRect_.end() && r->second.contains(winFocus_.cursorX(), winFocus_.cursorY());
        if (!inList && !(f >= kIdChRow0 && f < kIdChRow0 + 40)) ggbCloseDropDown();
    }
    if (!(pad.pressed & PSP_CTRL_SQUARE)) return;
    const uint32_t id = winFocus_.focused();
    bool taken = false;
    if (id >= kIdGbSlot0 && id < kIdGbSlot0 + gbk::kBankSlots && windowOpen(kWinGuildBank)) {
        taken = guildBankAction(id, false);
    } else if (ggb_->gbHeld.active) {
        taken = guildBankBagClick(id, false);
    }
    if (taken) pad.pressed &= ~PSP_CTRL_SQUARE;
}

bool WorldScreen::ggbEscape() {
    if (session_.glyphTargeting()) {  // SpellStopTargeting
        session_.cancelGlyphTargeting();
        return true;
    }
    if (ggb_ && ggb_->gbHeld.active) {  // ClearCursor
        ggb_->gbHeld = GgbUi::Held{};
        return true;
    }
    return false;
}

bool WorldScreen::ggbCloseDropDown() {
    if (!windowOpen(kWinChannelDropDown)) return false;
    closeWindow(kWinChannelDropDown);
    if (ggb_) ggb_->dropKind = 0;
    return true;
}

// After the windows: the glyph / the vault item on the cursor (the tooltips go through wotlkTip).
void WorldScreen::drawGgbOverlay(UiRenderer& ui) {
    if (!uiMode_ || settings_.isOpen()) return;
    uint32_t display = 0;
    if (session_.glyphTargeting()) {
        display = session_.protoItemDef(session_.glyphTargetingEntry()).displayInfoId;
    } else if (ggb_ && ggb_->gbHeld.active) {
        if (const gbk::BankItem* it = session_.guildBankItem(ggb_->gbHeld.tab, ggb_->gbHeld.slot))
            display = session_.protoItemDef(it->entry).displayInfoId;
    }
    if (display == 0) return;
    if (const pp::Texture* t = itemIcons().byDisplay(display, spellFrame_))
        ui.image(*t, Rect{winFocus_.cursorX() + 6.0f, winFocus_.cursorY() + 6.0f, 18.0f, 18.0f}, 0xE0FFFFFFu);
}

// --- GlyphFrame ------------------------------------------------------------------------------------------------------

bool WorldScreen::glyphTabShown() {
    const uint32_t level = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    return level >= static_cast<uint32_t>(kInscriptionLevel) && !talentPetView();
}

bool WorldScreen::glyphTabSelected() { return ggb_ && ggb_->glyphTab && glyphTabShown(); }

void WorldScreen::selectGlyphTab(bool on) {
    GgbUi& g = ggbUi();
    if (g.glyphTab == on) return;
    g.glyphTab = on;
    PSP_LOGI(pp::tag::Ctrl, "glyph_tab on=%d", on ? 1 : 0);
    if (on) {
        for (int i = 0; i < gbk::kGlyphSockets; ++i) g.elapsed[i] = g.tint[i] = 0.0f;  // GlyphFrame_OnShow
        winFocus_.setFocus(kIdGlyphSocket0);
    }
}

void WorldScreen::openGlyphFrame() {  // OpenGlyphFrame -> PlayerTalentFrame_OpenGlyphFrame(active group)
    if (!glyphTabShown()) return;
    if (!windowOpen(kWinTalents)) openTalentWindow();
    else openWindow(kWinTalents);  // to the top
    selectGlyphTab(true);
    if (!uiMode_) {
        setUiMode(true);
        suppress_ = ~0u;
    }
    winFocus_.setFocus(kIdGlyphSocket0);
}

void WorldScreen::loadGlyphArt() {
    GgbUi& g = ggbUi();
    if (g.glyArtTried) return;
    g.glyArtTried = true;
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    int ok = 0;
    if (pak.open(path)) {
        ok += ggbLoad(pak, "gly_frame", g.glyFrame, g.glyArtBytes);
        ok += ggbLoad(pak, "gly_parts", g.glyParts, g.glyArtBytes);
        ok += ggbLoad(pak, "gly_locked", g.glyLocked, g.glyArtBytes);
        ok += ggbLoad(pak, "gly_sparkle", g.glySparkle, g.glyArtBytes);
        pak.close();
    }
    PSP_LOGI(pp::tag::Gfx, "glyph_art ok=%d/4 kb=%u", ok, static_cast<unsigned>(g.glyArtBytes / 1024u));
}

const pp::Texture* WorldScreen::glyphRune(uint32_t rune) {
    GgbUi& g = ggbUi();
    if (rune > 20) rune = 0;
    if (!g.runeTried[rune]) {
        g.runeTried[rune] = true;
        char path[256], name[24];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        std::snprintf(name, sizeof(name), "gly_rune_%u", static_cast<unsigned>(rune));
        pp::Pak pak;
        if (pak.open(path)) {
            ggbLoad(pak, name, g.rune[rune], g.glyArtBytes);
            pak.close();
        }
    }
    return g.rune[rune].pixels ? &g.rune[rune] : nullptr;
}

// Every frame: USE_GLYPH, the GLYPH_* events (sounds, the glow), the timers, the art while the glyph tab shows.
void WorldScreen::glyphUpdate(float dt) {
    GgbUi& g = ggbUi();
    if (session_.useGlyphSeq() != g.useGlyphSeen) {
        g.useGlyphSeen = session_.useGlyphSeq();
        openGlyphFrame();
    }
    const bool shown = windowOpen(kWinTalents) && glyphTabSelected();
    pp::WorldSession::GlyphEvent ev;
    while (session_.takeGlyphEvent(ev)) {
        if (!shown) continue;  // GlyphFrame_OnEvent acts only while visible
        const gbk::GlyphSocket s = session_.glyphSocket(ev.socket);
        const bool minor = s.type == 2;
        playSoundByName(ev.kind == 2 ? (minor ? "Glyph_MinorDestroy" : "Glyph_MajorDestroy")
                                     : (minor ? "Glyph_MinorCreate" : "Glyph_MajorCreate"));
        g.glowT = 0.0f;  // GlyphFrame_PulseGlow
        if (ev.kind == 2) g.sparkle[ev.socket].on = false;  // GlyphFrame_StopSlotAnimation
        g.elapsed[ev.socket] = g.tint[ev.socket] = 0.0f;
        PSP_LOGI(pp::tag::Gfx, "glyph_glow kind=%u socket=%u", static_cast<unsigned>(ev.kind),
                 static_cast<unsigned>(ev.socket));
    }
    if (g.glowT >= 0.0f) {
        g.glowT += dt;
        if (g.glowT > 1.6f) {
            g.glowT = -1.0f;
            if (g.glyGlow.pixels) pp::textureRelease(g.glyGlow);
            g.glyGlowTried = false;
        }
    }
    if (!shown) {
        if (g.glyArtTried) g.freeGlyphArt();
        if (g.confirm && !windowOpen(kWinGlyphConfirm)) g.confirm = 0;
        return;
    }
    for (int i = 0; i < gbk::kGlyphSockets; ++i) {
        g.elapsed[i] += dt;
        g.tint[i] += dt;
        GgbUi::Sparkle& sp = g.sparkle[i];
        if (sp.on) {
            sp.t += dt;
            if (sp.t >= sp.dur) sp.on = false;
        }
        // GlyphFrame_OnUpdate: a filled socket with no sparkle playing starts one of a random size.
        if (!sp.on && session_.glyphSocket(i).glyph != 0) {
            const int size = std::rand() % 3;
            sp = GgbUi::Sparkle{0.0f, kSparkleDuration[size], size, true};
        }
    }
    if (g.confirm && !windowOpen(kWinGlyphConfirm)) g.confirm = 0;
}

float WorldScreen::drawGlyphTabButton(UiRenderer& ui, float x, float y, float w, float h, bool focusable) {
    const bool sel = glyphTabSelected();
    const Font& fs = kit_.font(kFontSmall);
    const float cap = 20.0f * kSX;
    const float ty = y - (sel ? 5.0f * kSY : 0.0f);
    const pp::Texture& t = tabArt_[sel ? 0 : 1];
    if (t.pixels) {
        const float tw = static_cast<float>(t.uw), th = static_cast<float>(t.uh), ct = tw * 20.0f / 128.0f;
        ui.image(t, Rect{x, ty, cap, h}, SrcRect{0, 0, ct, th});
        ui.image(t, Rect{x + cap, ty, w - 2 * cap, h}, SrcRect{ct, 0, tw - 2 * ct, th});
        ui.image(t, Rect{x + w - cap, ty, cap, h}, SrcRect{tw - ct, 0, ct, th});
    }
    const Rect hit{x + 4, ty + 2, w - 8, h - 4};
    const bool f = focusable && winFocus_.addStop(kIdGlyphTab, hit);
    const float lh = static_cast<float>(fs.lineHeight());
    ui.textShadow(fs, x + w * 0.5f, ty + (h - lh) * 0.5f - 1.0f, CLIENT_STR(GLYPHS), sel ? kColWhite : kColGold, Align::Center);
    if (f) ui.fill(hit, 0x30FFFFFFu);
    return w;
}

// GameTooltip:SetGlyph through the shared tooltip (wotlkTip, drawn after the windows).
void WorldScreen::buildGlyphTip(int socket, const Rect& anchorOnScreen) {
    const gbk::GlyphSocket s = session_.glyphSocket(socket);
    std::vector<WotlkTipLine> lines;
    // The glyph type line (own colour): the rare-quality blue 0070dd of the quality colours (upstream ui_colors.hpp
    // getQualityColor). The line order of the three variants is kept pending observation.
    constexpr uint32_t kGlyphBlue = 0xFFDD7000u;
    constexpr uint32_t kGreyTip = 0xFF808080u, kRedTip = 0xFF0000FFu, kGoldTip = 0xFF00D2FFu;
    const char* type = s.type == 2 ? CLIENT_STR(MINOR_GLYPH) : CLIENT_STR(MAJOR_GLYPH);
    if (s.glyph != 0 && s.spell != 0) {
        const std::string& name = spellDb_.name(s.spell);
        lines.emplace_back(name.empty() ? std::string(CLIENT_STR(UNKNOWN)) : name, kColWhite);
        lines.emplace_back(type, kGlyphBlue);
        const std::string desc = spellDescription(s.spell);
        if (!desc.empty()) lines.emplace_back(desc, kGoldTip);
        // GLYPH_SLOT_REMOVE_TOOLTIP - PSP: the button is Circle (no Shift).
        lines.emplace_back(OWN_STR(GLYPH_REMOVE_HINT), kGreyTip);
    } else if (!s.enabled) {
        lines.emplace_back(CLIENT_STR(GLYPH_LOCKED), kRedTip);
        lines.emplace_back(type, kGlyphBlue);
        if (s.order >= 1 && s.order <= 6) lines.emplace_back(slotTooltip(s.order), kGoldTip);  // GLYPH_SLOT_TOOLTIP%d
    } else {
        lines.emplace_back(CLIENT_STR(GLYPH_INACTIVE), kGreyTip);
        lines.emplace_back(type, kGlyphBlue);
        lines.emplace_back(CLIENT_STR(GLYPH_EMPTY_DESC), kGoldTip);
    }
    wotlkTip(anchorOnScreen, std::move(lines), kTipRight);
}

void WorldScreen::drawGlyphFrame(UiRenderer& ui, bool focusable) {
    PROF_SCOPE("glyphs.frame");
    GgbUi& g = ggbUi();
    loadGlyphArt();
    const Font& fn = kit_.font(kFontNormal);
    // GlyphFrameBackground (352 x 441, TOPLEFT): the frame with the portrait's hole.
    if (g.glyFrame.pixels) ui.image(g.glyFrame, Rect{PX(0), PY(0), 352 * kSX, 441 * kSY});
    else kit_.panel(ui, Rect{PX(20), PY(70), 300 * kSX, 340 * kSY});
    ui.textShadow(fn, PX(192), PY(18) - static_cast<float>(fn.lineHeight()) * 0.5f + 1.0f, CLIENT_STR(GLYPHS), kColGold,
                  Align::Center);  // (TALENT_SPEC_PRIMARY_GLYPH with two talent groups - PSP: one)
    // The close button of PlayerTalentFrame stays above GlyphFrame (its frame level + 1): drawn again.
    {
        const Rect cr{PX(340) - 9, PY(25) - 9, 18, 18};
        if (npcArt_.close.pixels) ui.image(npcArt_.close, cr);
    }
    const bool targeting = session_.glyphTargeting();
    const pp::Texture& parts = g.glyParts;
    const float su = parts.pixels ? static_cast<float>(parts.uw) / 256.0f : 1.0f;
    const float sv = parts.pixels ? static_cast<float>(parts.uh) / 128.0f : 1.0f;
    auto piece = [&](const Piece& p, float cx, float cy, float wU, float hU, uint32_t col, Blend bl = Blend::Alpha) {
        const Rect r{PX(cx) - wU * kSX * 0.5f, PY(cy) - hU * kSY * 0.5f, wU * kSX, hU * kSY};
        if (parts.pixels) ui.image(parts, r, SrcRect{p.x * su, p.y * sv, p.w * su, p.h * sv}, col, bl);
        return r;
    };
    int tipSocket = -1;
    Rect tipRect{};
    for (int i = 0; i < gbk::kGlyphSockets; ++i) {
        const gbk::GlyphSocket s = session_.glyphSocket(i);
        const bool major = s.type != 2;
        const float cx = kSocketX[i], cy = kSocketY[i];
        const float setU = major ? 108.0f : 86.0f;
        const bool hasGlyph = s.enabled && s.glyph != 0;
        const bool matches = targeting && s.enabled && !hasGlyph && session_.glyphMatchesSocket(i);
        // The button (72 x 72 template, 90 x 90 in the frame): the focus stop.
        const Rect hit{PX(cx) - 45 * kSX * 0.5f * 1.4f, PY(cy) - 45 * kSY * 0.5f * 1.4f, 63 * kSX, 63 * kSY};
        const uint32_t id = kIdGlyphSocket0 + static_cast<uint32_t>(i);
        const bool f = focusable && winFocus_.addStop(id, hit);
        // $parentSetting: alpha .6 .. 1 .. .6 over 6 s with a glyph (GlyphFrameGlyph_OnUpdate), else .6.
        float setA = 0.6f;
        if (hasGlyph) {
            const float e = std::fmod(g.elapsed[i], 6.0f);
            setA = e <= 2.0f ? 0.6f + 0.4f * e / 2.0f : e >= 4.0f ? 1.0f - 0.4f * (e - 4.0f) / 2.0f : 1.0f;
        }
        if (!s.enabled) {  // locked: UI-GlyphFrame-Locked (.1 .. .9), no background, rune, ring, shine
            const Rect r{PX(cx) - setU * kSX * 0.5f, PY(cy) - setU * kSY * 0.5f, setU * kSX, setU * kSY};
            if (g.glyLocked.pixels) ui.image(g.glyLocked, r, alphaWhite(setA));
            else ui.fill(Rect{r.x + r.w * 0.3f, r.y + r.h * 0.3f, r.w * 0.4f, r.h * 0.4f}, 0x80000000u);
        } else {
            piece(major ? kSetMajor : kSetMinor, cx, cy, setU, setU, alphaWhite(setA));
            // The tint of a socket the glyph on the cursor fits (1.6 s): the slot's art, alpha 1 -> .4 -> .4 -> 1.
            float bgA = 1.0f;
            bool slotArt = hasGlyph;
            if (matches) {
                slotArt = true;
                const float e = std::fmod(g.tint[i], 1.6f);
                bgA = e <= 0.6f ? 1.0f - 0.6f * e / 0.6f : e >= 0.8f ? 0.4f + 0.6f * (e - 0.8f) / 0.8f : 0.4f;
                const float hlA = f ? 0.4f : 0.4f * bgA;  // HIGHLIGHT_BASEALPHA
                piece(major ? kSetMajor : kSetMinor, cx, cy, setU, setU, alphaWhite(hlA), Blend::Add);
            } else if (f) {
                piece(major ? kSetMajor : kSetMinor, cx, cy, setU, setU, alphaWhite(0.25f), Blend::Add);  // OnEnter
            }
            const float bgU = major ? 70.0f : 64.0f;
            piece(slotArt ? bgSlot(i) : kBgEmpty, cx, cy, bgU, bgU, alphaWhite(bgA));
            if (hasGlyph) {  // $parentGlyph: the rune tinted by the type
                const Rect gr{PX(cx) - 53 * kSX * 0.5f, PY(cy) - 53 * kSY * 0.5f, 53 * kSX, 53 * kSY};
                if (const pp::Texture* rt = glyphRune(s.rune))
                    ui.image(*rt, gr, major ? abgr(1.0f, 0.25f, 0.0f) : abgr(0.0f, 0.25f, 1.0f));
            }
            // $parentRing (major 82 at 0,-1, minor 62 at 0,1) and $parentShine (16 at -9,12).
            if (major) piece(kRingMajor, cx, cy + 1.0f, 82.0f, 82.0f, 0xFFFFFFFFu);
            else piece(kRingMinor, cx, cy - 1.0f, 62.0f, 62.0f, 0xFFFFFFFFu);
            piece(major ? kShineMajor : kShineMinor, cx - 9.0f, cy - 12.0f, 16.0f, 16.0f, 0xFFFFFFFFu);
        }
        if (f) {
            tipSocket = i;
            tipRect = Rect{hit.x + ui.offsetX(), hit.y + ui.offsetY(), hit.w, hit.h};
        }
    }
    // The sparkles (GlyphSparkleTexture, ADD) moving along slotAnimations.
    if (g.glySparkle.pixels)
        for (int i = 0; i < gbk::kGlyphSockets; ++i) {
            const GgbUi::Sparkle& sp = g.sparkle[i];
            if (!sp.on || sp.dur <= 0.0f) continue;
            const float k = std::min(1.0f, sp.t / sp.dur);
            const float ux = kSparkle[i][0] + (kSparkle[i][2] - kSparkle[i][0]) * k;
            const float uy = kSparkle[i][1] + (kSparkle[i][3] - kSparkle[i][1]) * k;
            const float sz = kSparkleSize[sp.size];
            const float cx = 192.0f + ux, cy = 256.0f - uy;
            ui.image(g.glySparkle, Rect{PX(cx) - sz * kSX * 0.5f, PY(cy) - sz * kSY * 0.5f, sz * kSX, sz * kSY},
                     0xFFFFFFFFu, Blend::Add);
        }
    // GlyphFrameGlow (TOPLEFT -9,-38 of the frame): alpha up in .1 s, down in 1.5 s.
    if (g.glowT >= 0.0f) {
        if (!g.glyGlowTried) {
            g.glyGlowTried = true;
            char path[256];
            pp::fsPath(path, sizeof(path), "data/ui.pak");
            pp::Pak pak;
            uint32_t bytes = 0;
            if (pak.open(path)) {
                ggbLoad(pak, "gly_glow", g.glyGlow, bytes);
                pak.close();
            }
        }
        const float a = g.glowT <= 0.1f ? g.glowT / 0.1f : std::max(0.0f, 1.0f - (g.glowT - 0.1f) / 1.5f);
        if (g.glyGlow.pixels) ui.image(g.glyGlow, Rect{PX(-9), PY(38), 352 * kSX, 441 * kSY}, alphaWhite(a));
    }
    if (tipSocket >= 0 && uiMode_) buildGlyphTip(tipSocket, tipRect);
}

// StaticPopup CONFIRM_GLYPH_PLACEMENT (YES / NO: PlaceGlyphInSocket) and CONFIRM_REMOVE_GLYPH (YES / NO:
// RemoveGlyphFromSocket), hideOnEscape.
bool WorldScreen::drawGlyphConfirm(UiRenderer& ui) {
    GgbUi& g = ggbUi();
    if (!g.confirm) {
        closeWindow(kWinGlyphConfirm);
        return true;
    }
    StaticPopup p;
    p.text = g.confirmText.c_str();
    p.button1 = CLIENT_STR(YES);
    p.button1Id = kIdGlyphYes;
    p.button2 = CLIENT_STR(NO);
    p.button2Id = kIdGlyphNo;
    p.focusable = uiMode_ && !settings_.isOpen();
    drawStaticPopup(ui, p);
    return true;
}

bool WorldScreen::glyphAction(uint32_t act, bool alt) {
    GgbUi& g = ggbUi();
    if (act == kIdGlyphTab) {  // PlayerGlyphTab_OnClick / PlayerTalentTab_OnClick
        if (!alt) {
            selectGlyphTab(true);
            playSoundByName("igCharacterInfoTab");
        }
        return true;
    }
    if (act == kIdGlyphYes || act == kIdGlyphNo) {
        if (act == kIdGlyphYes && g.confirmSocket >= 0) {
            if (g.confirm == 1) session_.placeGlyph(g.confirmSocket);
            else if (g.confirm == 2) session_.removeGlyph(g.confirmSocket);
        }
        g.confirm = 0;
        closeWindow(kWinGlyphConfirm);
        return true;
    }
    if (act < kIdGlyphSocket0 || act >= kIdGlyphSocket0 + gbk::kGlyphSockets) return true;
    const int i = static_cast<int>(act - kIdGlyphSocket0);
    const gbk::GlyphSocket s = session_.glyphSocket(i);
    const std::string name = s.spell ? spellDb_.name(s.spell) : std::string();
    if (alt) {  // GlyphFrameGlyph_OnClick RightButton (PSP: without Shift) -> CONFIRM_REMOVE_GLYPH
        if (s.glyph == 0 || s.spell == 0) return true;
        g.confirmText = CLIENT_FMT(CONFIRM_REMOVE_GLYPH, name);
        g.confirm = 2;
        g.confirmSocket = i;
        openWindow(kWinGlyphConfirm);
        winFocus_.setFocus(kIdGlyphNo);
        return true;
    }
    if (!session_.glyphTargeting()) return true;  // PlaceGlyphInSocket without a glyph: nothing
    if (s.glyph != 0 && session_.glyphMatchesSocket(i)) {
        g.confirmText = CLIENT_STR(CONFIRM_GLYPH_PLACEMENT);
        g.confirm = 1;
        g.confirmSocket = i;
        openWindow(kWinGlyphConfirm);
        winFocus_.setFocus(kIdGlyphNo);
        return true;
    }
    session_.placeGlyph(i);
    return true;
}

}  // namespace wowee::ui::psp
