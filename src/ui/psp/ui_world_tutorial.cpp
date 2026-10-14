// Tutorials - the 3.3.5a client's TutorialFrame on the PSP.
//
// Logic = the client's FrameXML TutorialFrame.lua (TutorialFrame_NewTutorial,
// _Update, _AlertButton_OnClick, _Hide, _OnHide, Prev/Next, CheckNextPrev)
// over WorldSession's tutorial flags (psp_world_session_tutorial.cpp: the
// TUTORIAL_TRIGGER events, FlagTutorial, Get{Next,Prev}CompleatedTutorial);
// texts, layouts and art from data/tutorial.pak (build_tutorial_pak.py):
// the frame from UI-TUTORIAL-FRAME, the layout's images, keys and arrows at
// the DISPLAY_DATA offsets, TUTORIAL_TITLE<id> / TUTORIAL<id> (ruRU).
//
// PSP differences (marked "PSP:" below):
//  - keyboard / mouse sentences are swapped for a line with PSP button glyphs
//    (the one table in ui_tutorial_controls.hpp; wowee.cfg tutorialsubst=0
//    shows the original text), the mouse picture becomes glyphs, the key
//    pictures of "Movement" show the PSP buttons;
//  - the frame is a window of the stack (id 0xA0): Circle closes it, the
//    buttons need the cursor (SELECT); the frame is scaled to the 480x272
//    screen (the art was baked at that scale), its height is capped;
//  - the text scrolls a page per Cross on it (no scroll bar);
//  - the alert button sits bottom left (the action bar is bottom centre) and
//    its highlight pulses (no mouse-over on the PSP);
//  - no call-out glow around other frames (their PSP positions differ);
//  - "Menu -> Tutorials" = the options panel's "Show tutorials" / "Reset
//    tutorials" (Off also sends CMSG_TUTORIAL_CLEAR, like ClearTutorials).
// wowee.cfg: tutorials (1; the CVar showTutorials), tutorialauto (1 = open at
// once like the client; 0 = only the alert button), tutorialsubst (1),
// controls (1 current scheme, 2 = docs/PSP_CONTROLS.md; picks the table column).
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_tutorial_db.hpp"
#include "ui/psp/ui_tutorial_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

enum : uint32_t {
    kIdTutAlert = 0x6901,
    kIdTutPrev,
    kIdTutNext,
    kIdTutClose,
    kIdTutText,
    kIdTutMenu = 0x6908,  // "Tutorials" in the game menu window
    kIdTutOn,
    kIdTutOff,
    kIdTutReset,
    kIdTutMenuClose,
};

// Frame art entries (tutorial.pak), TutorialFrame.xml textures.
enum TutArt { kArtTop, kArtBottom, kArtLeft, kArtRight, kArtKey, kArtAlert, kArtAlertHl, kArtArrowU, kArtArrowD,
              kArtArrowL, kArtArrowR, kArtCount };
const char* const kArtNames[kArtCount] = {"f_top", "f_bottom", "f_left", "f_right", "f_key", "f_alert",
                                          "f_alert_hl", "f_arrow_u", "f_arrow_d", "f_arrow_l", "f_arrow_r"};

// TutorialFrame.lua / .xml sizes (original units).
constexpr float kTopH = 80.0f, kTileH = 10.0f, kBottomH = 30.0f, kFrameW = 364.0f;
constexpr float kMaxFrameH = 262.0f;  // PSP: screen 272
constexpr float kMouseW = 76.0f, kMouseH = 101.0f, kKeyW = 76.0f, kKeyH = 72.0f;
constexpr int kMaxImages = 3;
const Rect kAlertRect{4.0f, 272.0f - 31.0f - 2.0f, 50.0f, 31.0f};  // PSP: bottom left, clear of the action bar

struct TutItem {
    std::string text;
    int glyph = -1;
    float x = 0.0f;
};

}  // namespace

struct WorldScreen::TutorialUi {
    pp::TutorialDb db;
    bool dbOk = false;
    bool autoOpen = true, subst = true;
    int scheme = 0;  // 0 = controls=1, 1 = controls=2
    // TutorialFrame.lua state
    std::vector<uint32_t> queue;  // TUTORIALFRAME_QUEUE
    uint32_t lastId = 0;          // TUTORIAL_LAST_ID
    bool frameShown = false;
    uint32_t frameId = 0;         // TutorialFrame.id
    bool alertShown = false;
    // Layout of the shown tutorial (PSP px)
    float x = 0, y = 0, W = 0, H = 0, s = 0.72f;
    int tiles = 0;
    Rect text{};
    std::string title;
    std::vector<std::vector<TutItem>> lines;
    int scroll = 0;
    float lineH = 13.0f, glyphPx = 12.0f;
    const pp::TutorialRec* rec = nullptr;
    const TutorialControlsRow* row = nullptr;
    // Art (loaded on demand in update, released when nothing is shown)
    pp::Texture art[kArtCount];
    bool artTried = false;
    pp::Texture images[kMaxImages];
    int imageIndex[kMaxImages] = {-1, -1, -1};
    uint32_t imagesFor = 0;
    float pulse = 0.0f;
    uint32_t logs = 0, missingLogs = 0;

    void releaseImages() {
        for (auto& t : images) pp::textureRelease(t);
        for (int& i : imageIndex) i = -1;
        imagesFor = 0;
    }
    void releaseArt() {
        for (auto& t : art) pp::textureRelease(t);
        artTried = false;
        releaseImages();
    }
    ~TutorialUi() { releaseArt(); }
};

void WorldScreen::destroyTutorialUi(TutorialUi* p) { delete p; }

namespace {

// Splits one word into text runs and glyph tokens.
void wordPieces(const std::string& word, std::vector<TutItem>& out) {
    out.clear();
    std::string run;
    for (size_t i = 0; i < word.size();) {
        int len = 0;
        const int g = word[i] == '{' ? tutorialGlyphToken(word.c_str() + i, &len) : -1;
        if (g >= 0) {
            if (!run.empty()) out.push_back(TutItem{run, -1, 0.0f});
            run.clear();
            out.push_back(TutItem{std::string(), g, 0.0f});
            i += static_cast<size_t>(len);
        } else {
            run += word[i++];
        }
    }
    if (!run.empty()) out.push_back(TutItem{run, -1, 0.0f});
}

}  // namespace

// An inline glyph takes glyphPx + 3 px (1.5 px each side) - with the
// art cropped to its visible box the gaps are even; the font's space is 2 px.
static constexpr float kGlyphGap = 3.0f;

// Word wrap with inline glyphs (PSP: the controls lines).
static void layoutTutorialText(const UiKit& kit, const Font& f, const std::string& text, float width, float glyphPx,
                               std::vector<std::vector<TutItem>>& lines) {
    // L / R / SELECT / START are words in the line (no gap, as the text around them).
    auto glyphW = [&](int g) {
        const char* label = UiKit::padLabel(static_cast<PadGlyph>(g));
        return label ? static_cast<float>(f.measure(label)) : glyphPx + kGlyphGap;
    };
    lines.clear();
    lines.emplace_back();
    float x = 0.0f;
    const float space = static_cast<float>(f.measure(" "));
    std::vector<TutItem> pieces;
    size_t i = 0;
    while (i <= text.size()) {
        if (i == text.size()) break;
        if (text[i] == '\n') {
            lines.emplace_back();
            x = 0.0f;
            ++i;
            continue;
        }
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        size_t e = i;
        while (e < text.size() && text[e] != ' ' && text[e] != '\n') ++e;
        wordPieces(text.substr(i, e - i), pieces);
        i = e;
        float w = 0.0f;
        for (const auto& p : pieces) w += p.glyph >= 0 ? glyphW(p.glyph) : static_cast<float>(f.measure(p.text.c_str()));
        if (x > 0.0f && x + space + w > width) {
            lines.emplace_back();
            x = 0.0f;
        }
        if (x > 0.0f) x += space;
        for (auto& p : pieces) {
            p.x = x;
            x += p.glyph >= 0 ? glyphW(p.glyph) : static_cast<float>(f.measure(p.text.c_str()));
            lines.back().push_back(std::move(p));
        }
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
}

// Draws a "{l}{up}"-style glyph string centred in r, glyphs of `px`.
static void drawGlyphString(const UiKit& kit, UiRenderer& ui, const char* s, const Rect& r, float px) {
    if (!s) return;
    int glyphs[8];
    int n = 0;
    for (const char* p = s; *p && n < 8;) {
        int len = 0;
        const int g = tutorialGlyphToken(p, &len);
        if (g < 0) {
            ++p;
            continue;
        }
        glyphs[n++] = g;
        p += len;
    }
    float total = n > 0 ? (n - 1) * 2.0f : 0.0f;
    for (int i = 0; i < n; ++i) total += kit.glyphWidth(static_cast<PadGlyph>(glyphs[i]), px);
    float gx = r.x + (r.w - total) * 0.5f;
    const float gy = r.y + (r.h - px) * 0.5f;
    for (int i = 0; i < n; ++i) {
        const float gw = kit.glyphWidth(static_cast<PadGlyph>(glyphs[i]), px);
        if (!kit.glyph(ui, static_cast<PadGlyph>(glyphs[i]), Rect{gx, gy, gw, px}))
            ui.text(kit.font(kFontSmall), gx, gy, kTutorialGlyphNames[glyphs[i]], kColGold);  // no pad art: its name
        gx += gw + 2.0f;
    }
}

// TutorialFrame_Update: flag it, lay the frame out, show it.
void WorldScreen::tutorialShow(uint32_t id) {
    TutorialUi& t = *tut_;
    session_.flagTutorial(id);
    t.frameId = id;
    t.rec = t.db.find(id);
    t.row = tutorialControls(id);
    const pp::TutorialTables& tb = t.db.tables();
    t.s = t.db.scale();
    const float s = t.s;
    // Frame size: top + tileHeight x 10 + bottom; PSP: capped to the screen.
    t.tiles = t.rec ? t.rec->tileHeight : 7;
    const int maxTiles = static_cast<int>((kMaxFrameH / s - kTopH - kBottomH) / kTileH);
    if (t.tiles > maxTiles) t.tiles = maxTiles;
    t.W = std::floor(kFrameW * s + 0.5f);
    t.H = std::floor((kTopH + t.tiles * kTileH + kBottomH) * s + 0.5f);
    // anchorData: LEFT / RIGHT of UIParent, offsets in original units (y up).
    const uint8_t anchor = t.rec ? t.rec->anchor : static_cast<uint8_t>(pp::kTutLeft);
    const float ax = t.rec ? t.rec->anchorX * s : 15.0f * s, ay = t.rec ? t.rec->anchorY * s : 30.0f * s;
    pp::tutorialPlace(anchor, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH), t.W, t.H, ax, -ay,
                      t.x, t.y);
    t.x = std::clamp(t.x, 2.0f, pp::kScreenW - t.W - 2.0f);
    t.y = std::clamp(t.y, 2.0f, pp::kScreenH - t.H - 2.0f);
    // textBox (TOPLEFT / BOTTOMRIGHT offsets).
    const float tl = t.rec ? t.rec->textLeft : 33, tt = t.rec ? t.rec->textTop : -75;
    const float tr = t.rec ? t.rec->textRight : -29, tbm = t.rec ? t.rec->textBottom : 35;
    t.text = Rect{t.x + tl * s, t.y - tt * s, 0, 0};
    t.text.w = std::max(40.0f, t.x + t.W + tr * s - t.text.x);
    t.text.h = std::max(t.lineH, t.y + t.H - tbm * s - t.text.y);
    // Texts: TUTORIAL_TITLE<id>, TUTORIAL<id> (PSP: controls line swapped in).
    const char* title = t.rec ? tb.str(t.rec->titleOff) : nullptr;
    const char* marked = t.rec ? tb.str(t.rec->textOff) : nullptr;
    t.title = title ? title : "";
    int keys = 0;
    const char* pspLine = t.row ? pp::ps(t.row->line[t.scheme]) : nullptr;
    const std::string body = pp::assembleTutorialText(marked, pspLine, t.subst, &keys);
    const Font& f = kit_.font(kFontSmall);
    t.lineH = static_cast<float>(f.lineHeight()) + 2.0f;
    // (Glyphs were uneven in the text:) one height for every button,
    // 2 px under the font line (13 px for font_small: ascent 10 + descent 3), centred
    // on it in draw. Was lineHeight + 1 with the art's own margins (10.5 px faces, 9 px L/R).
    t.glyphPx = static_cast<float>(f.lineHeight()) - 2.0f;
    layoutTutorialText(kit_, f, body, t.text.w - 2.0f, t.glyphPx, t.lines);
    t.scroll = 0;
    if (keys > 0 && !t.row && ++t.missingLogs <= 30)
        PSP_LOGW(pp::tag::Ctrl, "tutorial_subst_missing id=%u keys=%d", static_cast<unsigned>(id), keys);
    t.frameShown = true;
    openWindow(kWinTutorial);
    if (++t.logs <= 60)
        PSP_LOGI(pp::tag::Ctrl, "tutorial_show id=%u queue=%u lines=%u keys=%d subst=%d elems=%u",
                 static_cast<unsigned>(id), static_cast<unsigned>(t.queue.size()),
                 static_cast<unsigned>(t.lines.size()), keys, (t.subst && pspLine) ? 1 : 0,
                 static_cast<unsigned>(t.rec ? t.rec->elemCount : 0));
}

// TutorialFrame_AlertButton_OnClick.
void WorldScreen::tutorialAlertClick() {
    TutorialUi& t = *tut_;
    if (!t.queue.empty()) {
        const uint32_t id = t.queue.front();
        t.queue.erase(t.queue.begin());
        tutorialShow(id);
    } else if (t.lastId) {
        tutorialShow(t.lastId);
    }
}

// TutorialFrame_NewTutorial (TUTORIAL_TRIGGER).
void WorldScreen::tutorialNew(uint32_t id) {
    TutorialUi& t = *tut_;
    if (session_.isTutorialFlagged(id)) return;
    if (std::find(t.queue.begin(), t.queue.end(), id) != t.queue.end()) return;
    if (!t.db.find(id)) return;  // PSP: no text baked for it - nothing to show
    t.lastId = id;
    t.queue.push_back(id);
    if (!t.frameShown) {
        t.alertShown = true;
        // not InCombatLockdown(); PSP: wowee.cfg tutorialauto=0 leaves it to the alert button
        if (t.autoOpen && !session_.inCombat()) tutorialAlertClick();
    }
}

// TutorialFrame_OnHide (+ TutorialFrame_Hide's "next in the queue").
void WorldScreen::tutorialHidden(bool showNext) {
    TutorialUi& t = *tut_;
    t.frameShown = false;
    if (showNext && !t.queue.empty()) {
        tutorialAlertClick();
        return;
    }
    const uint32_t level = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    if (t.queue.empty() && level > 5) t.alertShown = false;
}

void WorldScreen::updateTutorial(float dtSec) {
    if (!tut_) {
        tut_ = std::unique_ptr<TutorialUi, void (*)(TutorialUi*)>(new TutorialUi(), &destroyTutorialUi);
        TutorialUi& t = *tut_;
        session_.setShowTutorials(pp::configGetInt("tutorials", 1) != 0);
        t.autoOpen = pp::configGetInt("tutorialauto", 1) != 0;
        t.subst = pp::configGetInt("tutorialsubst", 1) != 0;
        t.scheme = pp::configGetInt("controls", 2) == 2 ? 1 : 0;  // default = ui_world_controls.cpp (2)
        t.dbOk = t.db.open();
        PSP_LOGI(pp::tag::Ctrl, "tutorial_cfg on=%d auto=%d subst=%d scheme=%d pak=%d", session_.showTutorials() ? 1 : 0,
                 t.autoOpen ? 1 : 0, t.subst ? 1 : 0, t.scheme + 1, t.dbOk ? 1 : 0);
    }
    PROF_SCOPE("tutorial");
    TutorialUi& t = *tut_;
    t.pulse += dtSec;
    if (t.pulse > 1000.0f) t.pulse = 0.0f;
    uint32_t id = 0;
    while (session_.takeTutorialTrigger(id))
        if (t.dbOk && session_.showTutorials()) tutorialNew(id);  // (login triggers may predate the cfg)
    // Circle closed it through the window stack: TutorialFrame_Hide.
    if (t.frameShown && !windowOpen(kWinTutorial)) {
        playSoundByName("igMainMenuClose");
        tutorialHidden(true);
    }
    // Art on demand, here (the GE is idle between frames), never in draw.
    const bool visible = t.frameShown || t.alertShown;
    if (visible && !t.artTried) {
        t.artTried = true;
        const int n = t.db.loadTextures(kArtNames, t.art, kArtCount);
        PSP_LOGI(pp::tag::Ctrl, "tutorial_art ok=%d missing=%d", n, kArtCount - n);
    }
    if (t.frameShown && t.imagesFor != t.frameId) {
        t.releaseImages();
        t.imagesFor = t.frameId;
        const char* names[kMaxImages] = {};
        char buf[kMaxImages][16];
        int k = 0;
        if (t.rec) {
            const auto& elems = t.db.tables().elems;
            for (uint32_t i = 0; i < t.rec->elemCount && k < kMaxImages; ++i) {
                const pp::TutorialElem& e = elems[t.rec->firstElem + i];
                if (e.type != pp::kTutElemImage) continue;
                std::snprintf(buf[k], sizeof(buf[k]), "timg_%u", static_cast<unsigned>(e.index));
                names[k] = buf[k];
                t.imageIndex[k++] = e.index;
            }
        }
        if (k) t.db.loadTextures(names, t.images, k);
    }
    if (!visible && t.artTried) t.releaseArt();
}

bool WorldScreen::tutorialWindowAction(uint32_t act) {
    if (!tut_ || act < 0x6900 || act > 0x69FF) return false;
    TutorialUi& t = *tut_;
    auto saveShow = [&](bool on) {
        session_.setShowTutorials(on);
        pp::configSet("tutorials", on ? "1" : "0");
        pp::configSave();
    };
    switch (act) {
        case kIdTutAlert: tutorialAlertClick(); break;
        case kIdTutPrev: {  // TutorialFramePrevButton_OnClick
            playSoundByName("igMainMenuOptionCheckBoxOn");
            if (uint32_t p = session_.getPrevCompletedTutorial(t.frameId, t.db.ids())) tutorialShow(p);
            break;
        }
        case kIdTutNext: {  // TutorialFrameNextButton_OnClick
            playSoundByName("igMainMenuOptionCheckBoxOn");
            if (uint32_t n = session_.getNextCompletedTutorial(t.frameId, t.db.ids())) tutorialShow(n);
            else if (!t.queue.empty()) tutorialAlertClick();
            break;
        }
        case kIdTutClose:  // TutorialFrame_Hide
            playSoundByName("igMainMenuClose");
            closeWindow(kWinTutorial);
            tutorialHidden(true);
            break;
        case kIdTutText: {  // PSP: a page further, back to the top after the end
            const int visible = std::max(1, static_cast<int>(t.text.h / t.lineH));
            t.scroll = t.scroll + visible >= static_cast<int>(t.lines.size()) ? 0 : t.scroll + visible - 1;
            break;
        }
        case kIdTutMenu:
            if (windowOpen(kWinTutorialMenu)) closeWindow(kWinTutorialMenu);
            else openWindow(kWinTutorialMenu);
            break;
        case kIdTutOn:
            saveShow(true);
            PSP_LOGI(pp::tag::Ctrl, "tutorial_show_set on=1");
            break;
        case kIdTutOff:  // PSP: "don't show" = ClearTutorials (every id seen on the server)
            saveShow(false);
            session_.clearTutorials();
            t.queue.clear();
            if (t.frameShown) closeWindow(kWinTutorial);
            t.frameShown = false;
            t.alertShown = false;
            PSP_LOGI(pp::tag::Ctrl, "tutorial_show_set on=0");
            break;
        case kIdTutReset:  // the options panel's ResetTutorials button
            playSoundByName("gsTitleOptionOK");
            session_.resetTutorials();
            t.queue.clear();
            t.lastId = 0;
            if (!session_.showTutorials()) saveShow(true);
            for (uint32_t id : {42u, 2u, 3u, 1u}) session_.triggerTutorial(id);
            break;
        case kIdTutMenuClose: closeWindow(kWinTutorialMenu); break;
        default: return false;
    }
    return true;
}

void WorldScreen::drawTutorialAlert(UiRenderer& ui) {
    if (!tut_ || !tut_->alertShown) return;
    TutorialUi& t = *tut_;
    const Rect r = kAlertRect;
    const bool focused = uiMode_ && !settings_.isOpen() && winFocus_.addStop(kIdTutAlert, r);
    if (t.art[kArtAlert].pixels) ui.image(t.art[kArtAlert], r);
    else kit_.panel(ui, r);
    // PSP: no mouse-over - the highlight pulses while tutorials wait, full when focused.
    const float a = focused ? 1.0f : (t.queue.empty() ? 0.0f : 0.5f + 0.5f * std::sin(t.pulse * 5.2f));
    if (a > 0.02f) {
        const Rect h{r.x + (r.w - r.h * 0.72f) * 0.5f + 1.0f, r.y + r.h * 0.14f - 2.0f, r.h * 0.72f, r.h * 0.7f};
        const uint32_t col = (static_cast<uint32_t>(a * 255.0f) << 24) | 0x00FFFFFFu;
        if (t.art[kArtAlertHl].pixels) ui.image(t.art[kArtAlertHl], h, col, Blend::Add);
    }
    if (!uiMode_) {  // "SELECT to reach it" (the word, right-aligned to the corner)
        const float gw = kit_.glyphWidth(kPadSelect, 14.0f);
        kit_.glyph(ui, kPadSelect, Rect{r.x + r.w + 8.0f - gw, r.y - 6.0f, gw, 14.0f});
    }
}

bool WorldScreen::drawTutorialWindow(UiRenderer& ui, uint8_t w) {
    if (!tut_ || (w != kWinTutorial && w != kWinTutorialMenu)) return false;
    TutorialUi& t = *tut_;
    PROF_SCOPE("tutorial");
    if (w == kWinTutorialMenu) {
        const Rect r{(pp::kScreenW - 236) * 0.5f, 136 - 112 * 0.5f, 236, 112};  // centred (was x 6)
        kit_.panel(ui, r);
        ui.textShadow(kit_.font(kFontNormal), r.x + r.w * 0.5f, r.y + 12, OWN_STR(TUT_MENU), kColGold, Align::Center);
        const float c0 = r.x + 12, bw = (r.w - 28) * 0.5f, c1 = c0 + bw + 4;
        ui.textShadow(kit_.font(kFontSmall), r.x + r.w * 0.5f, r.y + 27,
                      session_.showTutorials() ? OWN_STR(TUT_NOW_SHOWN) : OWN_STR(TUT_NOW_HIDDEN), kColWhite,
                      Align::Center);
        float y = r.y + 40;
        kit_.button(ui, winFocus_, kIdTutOn, Rect{c0, y, bw, 20}, OWN_STR(PORT_ON), !session_.showTutorials());
        kit_.button(ui, winFocus_, kIdTutOff, Rect{c1, y, bw, 20}, OWN_STR(PORT_OFF), session_.showTutorials());
        y += 23;
        const char* reset = t.db.tables().uiString("RESET_TUTORIALS");
        kit_.button(ui, winFocus_, kIdTutReset, Rect{c0, y, r.w - 24, 20}, reset ? reset : CLIENT_STR(RESET_TUTORIALS));
        y += 23;
        kit_.button(ui, winFocus_, kIdTutMenuClose, Rect{c0, y, r.w - 24, 20}, CLIENT_STR(CLOSE));
        return true;
    }
    if (!t.frameShown) return true;
    const float s = t.s;
    const Rect frame{t.x, t.y, t.W, t.H};
    auto place = [&](uint8_t align, float w, float h, int16_t dx, int16_t dy) {
        float px = 0, py = 0;
        pp::tutorialPlace(align, t.W, t.H, w, h, dx * s, -dy * s, px, py);
        return Rect{t.x + px, t.y + py, w, h};
    };
    // BACKGROUND: black (TutorialFrame.xml $parentBackground).
    ui.fill(Rect{t.x + 13 * s, t.y + 35 * s, t.W - 16 * s, t.H - 40 * s}, 0xFF000000u);
    // BORDER: top, the side strips, bottom (PSP: one stretched strip per side).
    const float sideTop = t.y + kTopH * s, sideH = t.tiles * kTileH * s;
    if (t.art[kArtTop].pixels) {
        ui.image(t.art[kArtTop], Rect{t.x, t.y, t.W, kTopH * s});
        if (t.art[kArtLeft].pixels) ui.image(t.art[kArtLeft], Rect{t.x + 6 * s, sideTop, 11 * s, sideH});
        if (t.art[kArtRight].pixels) ui.image(t.art[kArtRight], Rect{t.x + t.W - 8 * s, sideTop, 7 * s, sideH});
        if (t.art[kArtBottom].pixels)
            ui.image(t.art[kArtBottom], Rect{t.x + 6 * s, sideTop + sideH, t.W - 7 * s, kBottomH * s});
    } else {
        kit_.panel(ui, frame);  // no frame art baked: a plain panel
    }
    // ARTWORK: images, keys, arrows, mouse (PSP: glyphs) at the layout's points.
    if (t.rec) {
        const auto& elems = t.db.tables().elems;
        int img = 0;
        for (uint32_t i = 0; i < t.rec->elemCount; ++i) {
            const pp::TutorialElem& e = elems[t.rec->firstElem + i];
            if (e.type == pp::kTutElemImage) {
                const int k = img++;
                if (k >= kMaxImages || !t.images[k].pixels) continue;
                const pp::Texture& tex = t.images[k];
                ui.image(tex, place(e.align, tex.uw, tex.uh, e.x, e.y));
            } else if (e.type == pp::kTutElemArrow && e.index < 4) {
                const pp::Texture& tex = t.art[kArtArrowU + e.index];
                if (!tex.pixels) continue;
                const float k = e.scalePct / 50.0f;  // baked at the layouts' 0.5
                ui.image(tex, place(e.align, tex.uw * k, tex.uh * k, e.x, e.y));
            } else if (e.type == pp::kTutElemKey) {
                const char* g = e.index < 5 ? kTutorialKeyGlyph[t.scheme][e.index] : nullptr;
                if (!g) continue;  // PSP: no button for that key in this scheme
                const Rect kr = place(e.align, kKeyW * s, kKeyH * s, e.x, e.y);
                // PSP buttons only - no keyboard key art under the glyph.
                drawGlyphString(kit_, ui, g, kr, kr.h * 0.6f);
            } else if (e.type == pp::kTutElemMouse) {
                const char* g = t.row ? t.row->mouse[t.scheme] : nullptr;
                const float k = e.scalePct / 100.0f;
                drawGlyphString(kit_, ui, g, place(e.align, kMouseW * s * k, kMouseH * s * k, e.x, e.y),
                                std::floor(24.0f * k));
            }
        }
    }
    // Title (GameFontHighlight, gold), TOP +15,-17. The box is
    // the frame art's title bar, x 72..334 of 364 - it started at 30 and a long
    // title (TUTORIAL_TITLE of a long one) scrolled over the "?" icon (x 0..70).
    ui.textMarquee(kit_.font(kFontNormal), Rect{t.x + 74 * s, t.y + 12 * s, 258 * s, 16}, t.title.c_str(),
                   kColGold);
    // Text (GameFontNormal, white) in the textBox; PSP: Cross on it pages.
    const Font& f = kit_.font(kFontSmall);
    const int visible = std::max(1, static_cast<int>(t.text.h / t.lineH));
    const bool more = static_cast<int>(t.lines.size()) > visible;
    if (more) winFocus_.addStop(kIdTutText, t.text);
    ui.pushClip(t.text);
    for (int i = t.scroll; i < static_cast<int>(t.lines.size()) && i < t.scroll + visible; ++i) {
        const float ly = t.text.y + (i - t.scroll) * t.lineH;
        for (const TutItem& it : t.lines[i]) {
            const float lx = t.text.x + it.x;
            if (it.glyph < 0) ui.text(f, lx, ly, it.text.c_str(), kColWhite);
            else if (const char* label = UiKit::padLabel(static_cast<PadGlyph>(it.glyph)))
                ui.text(f, lx, ly, label, kColGold);  // the button's name, on the text's baseline
            else if (!kit_.glyphFill(ui, static_cast<PadGlyph>(it.glyph),
                                     Rect{lx + kGlyphGap * 0.5f, ly + (f.lineHeight() - t.glyphPx) * 0.5f,
                                          kit_.glyphWidth(static_cast<PadGlyph>(it.glyph), t.glyphPx), t.glyphPx}))
                ui.text(f, lx, ly, kTutorialGlyphNames[it.glyph], kColGold);  // no pad art: its name
        }
    }
    ui.popClip();
    if (more) {
        const bool atEnd = t.scroll + visible >= static_cast<int>(t.lines.size());
        const float mx = t.text.x + t.text.w - 34.0f, my = t.text.y + t.text.h + 1.0f;
        ui.text(f, mx, my, atEnd ? OWN_STR(TUT_TEXT_TOP) : OWN_STR(TUT_TEXT_MORE), kColGrey);
        if (uiMode_) kit_.glyph(ui, kPadCross, Rect{mx - 13.0f, my, 12.0f, 12.0f});
    }
    // Buttons: Prev (bottom left), Next, Close (bottom right) - TutorialFrame_CheckNextPrevButtons.
    const pp::TutorialTables& tb = t.db.tables();
    const char* prev = tb.uiString("PREV");
    const char* next = tb.uiString("NEXT");
    const char* close = tb.uiString("CLOSE");
    const bool canPrev = session_.getPrevCompletedTutorial(t.frameId, t.db.ids()) != 0;
    const bool canNext = session_.getNextCompletedTutorial(t.frameId, t.db.ids()) != 0 || !t.queue.empty();
    const float bh = 18.0f, by = t.y + t.H - bh - 4.0f;
    kit_.button(ui, winFocus_, kIdTutPrev, Rect{t.x + 22 * s, by, 62, bh}, prev ? prev : CLIENT_STR(PREV), canPrev);
    kit_.button(ui, winFocus_, kIdTutClose, Rect{t.x + t.W - 7 * s - 74, by, 74, bh}, close ? close : CLIENT_STR(CLOSE));
    kit_.button(ui, winFocus_, kIdTutNext, Rect{t.x + t.W - 7 * s - 74 - 66, by, 62, bh}, next ? next : CLIENT_STR(NEXT),
                canNext);
    // PSP: in the game the window's buttons need the cursor.
    if (!uiMode_) {
        const float hx = t.x + t.W * 0.5f - 10.0f, hy = by - 13.0f;
        const float gw = kit_.glyphWidth(kPadSelect, 12.0f);  // "SELECT" is text now
        if (kit_.glyph(ui, kPadSelect, Rect{hx - gw - 3.0f, hy, gw, 12.0f}))
            ui.text(f, hx, hy, OWN_STR(TUT_SELECT_WINDOW), kColGrey);
    }
    return true;
}

void WorldScreen::drawTutorialMenuButton(UiRenderer& ui, const Rect& button, bool focusable) {
    // A button of the GameMenuFrame column (was the left corner of the title row).
    drawGameMenuButton(ui, kIdTutMenu, button, OWN_STR(TUT_MENU), focusable);
}

} // namespace wowee::ui::psp
