// The on-device autopilot's band and summary - ui_autotest_overlay.hpp.
//
// The band: top centre, between the player frame and the minimap (x 92..388), the client's yellow UIErrorsFrame line
// (kFontNormal, UI_INFO_MESSAGE 1.0 / 1.0 / 0.0 - ui_world_errors.cpp) on a dark strip, word-wrapped to four rows:
// "Test 3/14: <place> - look: textured ground, NPCs walking"; under it the step counter in the small font and,
// the first 20 s and while paused, the START+SELECT line. It takes no input (the tester may grab the pad any time).
// The summary: a panel in the middle with a row per block (PASS green / FAIL red, its caption, minutes), the report's
// name, and "✕ close" - it stays until Cross.
#include "ui/psp/ui_autotest_overlay.hpp"

#include "platform/psp/psp_autotest.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_strings.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr uint32_t kYellow = 0xFF00FFFFu;  // UI_INFO_MESSAGE (1.0, 1.0, 0.0) as 0xAABBGGRR
constexpr uint32_t kStrip = 0xA0000000u;
constexpr float kBandX = 92.0f, kBandW = 296.0f;  // between the player frame and the minimap
constexpr size_t kMaxRows = 4;

// Word wrap to `maxW` with the font's advances (UTF-8); a word wider than the row breaks.
std::vector<std::string> wrap(const Font& f, const std::string& text, float maxW, size_t maxRows) {
    std::vector<std::string> rows;
    std::string cur;
    float w = 0.0f, breakW = 0.0f;
    size_t breakAt = 0;
    const char* s = text.c_str();
    while (*s) {
        const char* p0 = s;
        const uint32_t cp = utf8Next(&s);
        const Glyph* g = f.find(cp);
        const float adv = g ? static_cast<float>(g->advance) : 0.0f;
        if (w + adv > maxW && !cur.empty()) {
            if (breakAt > 0) {
                rows.push_back(cur.substr(0, breakAt - 1));
                cur.erase(0, breakAt);
                w -= breakW;
            } else {
                rows.push_back(cur);
                cur.clear();
                w = 0.0f;
            }
            breakAt = 0;
            breakW = 0.0f;
            if (rows.size() >= maxRows) return rows;
            if (cur.empty() && cp == ' ') continue;
        }
        cur.append(p0, static_cast<size_t>(s - p0));
        w += adv;
        if (cp == ' ') {
            breakAt = cur.size();
            breakW = w;
        }
    }
    if (!cur.empty() && rows.size() < maxRows) rows.push_back(cur);
    return rows;
}

float lineH(const Font& f, float fallback) { return f.lineHeight() > 0 ? static_cast<float>(f.lineHeight()) : fallback; }

void drawBand(UiRenderer& ui, const UiKit& kit, const pp::AutotestView& v) {
    const Font& big = kit.font(kFontNormal);
    const Font& small = kit.font(kFontSmall);
    std::string text;
    if (v.blockCaption[0]) {
        text = OWN_FMT(AT_BLOCK, v.block, v.blocks, v.blockCaption);
        if (v.lookCaption[0]) text += std::string(" - ") + OWN_FMT(AT_LOOK, v.lookCaption);
    } else {
        text = OWN_STR(AT_STARTING);
    }
    const std::vector<std::string> rows = wrap(big, text, kBandW - 8.0f, kMaxRows);
    const float bh = lineH(big, 15.0f), sh = lineH(small, 11.0f);
    const int secs = static_cast<int>(v.elapsedMs / 1000u);
    const std::string counter = OWN_FMT(AT_STEP, v.step, v.steps, v.stepVerb, secs / 60, secs % 60);
    const char* hint = v.paused ? OWN_STR(AT_PAUSED) : (v.pauseHint ? OWN_STR(AT_PAUSE_HINT) : nullptr);
    const float h = 4.0f + bh * static_cast<float>(rows.size()) + sh + (hint ? sh + 2.0f : 0.0f) + 4.0f;
    ui.fill(Rect{kBandX, 0.0f, kBandW, h}, kStrip);
    float y = 3.0f;
    const float cx = kBandX + kBandW * 0.5f;
    for (const std::string& r : rows) {
        ui.textShadow(big, cx, y, r.c_str(), kYellow, Align::Center);
        y += bh;
    }
    ui.text(small, cx, y, counter.c_str(), kColGrey, Align::Center);
    y += sh + 2.0f;
    if (hint) ui.textShadow(small, cx, y, hint, v.paused ? kColRed : kColGold, Align::Center);
}

void drawSummary(UiRenderer& ui, const UiKit& kit, const pp::AutotestView& v) {
    const Font& big = kit.font(kFontNormal);
    const Font& small = kit.font(kFontSmall);
    const float sh = lineH(small, 11.0f) + 1.0f, bh = lineH(big, 15.0f);
    const int n = v.resultCount;
    const int perCol = n > 16 ? (n + 1) / 2 : n;
    const int cols = n > 16 ? 2 : 1;
    const float colW = cols == 2 ? 220.0f : 300.0f;
    const float w = colW * static_cast<float>(cols) + 20.0f;
    const float h = 12.0f + bh + 4.0f + sh * static_cast<float>(perCol > 0 ? perCol : 1) + 6.0f + sh * 2.0f + 8.0f;
    const Rect r{(pp::kScreenW - w) * 0.5f, (pp::kScreenH - h) * 0.5f, w, h};
    kit.panel(ui, r);
    float y = r.y + 8.0f;
    const std::string title = OWN_FMT(AT_SUMMARY, v.passCount, v.failCount);
    ui.textShadow(big, r.x + w * 0.5f, y, title.c_str(), v.failCount ? kColRed : kColGold, Align::Center);
    y += bh + 4.0f;
    for (int i = 0; i < n; ++i) {
        const int col = i / (perCol > 0 ? perCol : 1), row = i % (perCol > 0 ? perCol : 1);
        const float x = r.x + 10.0f + static_cast<float>(col) * colW;
        const float ry = y + static_cast<float>(row) * sh;
        const bool pass = v.resultPass(i);
        ui.text(small, x, ry, pass ? OWN_STR(AT_PASS) : OWN_STR(AT_FAIL), pass ? kColGreen : kColRed);
        char mins[16];
        const unsigned s = v.resultMs(i) / 1000u;
        std::snprintf(mins, sizeof(mins), "%u:%02u", s / 60u, s % 60u);
        ui.text(small, x + colW - 14.0f, ry, mins, kColGrey, Align::Right);
        ui.pushClip(Rect{x + 62.0f, ry, colW - 62.0f - 50.0f, sh});
        ui.text(small, x + 62.0f, ry, v.resultCaption(i), kColWhite);
        ui.popClip();
    }
    y += sh * static_cast<float>(perCol > 0 ? perCol : 1) + 6.0f;
    ui.text(small, r.x + w * 0.5f, y, OWN_STR(AT_REPORT), kColGrey, Align::Center);
    y += sh;
    const char* closeText = OWN_STR(AT_CLOSE);
    const float tw = static_cast<float>(small.measure(closeText));
    const float gw = sh;
    const float x0 = r.x + (w - (gw + 4.0f + tw)) * 0.5f;
    if (!kit.glyphFill(ui, kPadCross, Rect{x0, y, gw, gw}))
        ui.text(small, x0, y, "X", kColWhite);
    ui.text(small, x0 + gw + 4.0f, y, closeText, kColWhite);
}

// PSP (public safe): the autopilot refused - one line in the band's place for 30 s.
void drawRefused(UiRenderer& ui, const UiKit& kit) {
    const Font& big = kit.font(kFontNormal);
    const float bh = lineH(big, 15.0f);
    ui.fill(Rect{kBandX, 0.0f, kBandW, bh + 6.0f}, kStrip);
    ui.textShadow(big, kBandX + kBandW * 0.5f, 3.0f, OWN_STR(AT_REFUSED_PUBLIC), kColRed, Align::Center);
}

}  // namespace

void drawAutotestOverlay(UiRenderer& ui, const UiKit& kit) {
    const pp::AutotestView* v = pp::autotestView();
    if (!v) {
        if (pp::autotestRefusedNotice()) {
            ui.beginFrame();
            drawRefused(ui, kit);
            ui.endFrame();
        }
        return;
    }
    ui.beginFrame();
    if (v->finished) drawSummary(ui, kit, *v);
    else drawBand(ui, kit, *v);
    ui.endFrame();
}

}  // namespace wowee::ui::psp
