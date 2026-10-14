// UIErrorsFrame on the world screen.
//
// FrameXML UIErrorsFrame.xml / .lua: a MessageFrame 512 x 60 at TOP (0, -122) of UIParent, ErrorFont
// (GameFontNormalLarge), displayDuration="5", insertMode="TOP", fadeDuration the UI.xsd default 3.0.
// UI_ERROR_MESSAGE -> AddMessage(msg, 1.0, 0.1, 0.1), UI_INFO_MESSAGE -> AddMessage(msg, 1.0, 1.0, 0.0).
// As upstream's MessageFrame (lua_engine.cpp lua_MessageFrame_AddMessage, widget_renderer.cpp, expireMessages):
// every message is a new line - equal messages are not merged, three refusals in a row are three lines; a new
// line goes on top and pushes the older ones down; the frame holds as many lines as its height fits (60 / 16 =
// 3), the one pushed out is gone; a line lives displayDuration in all and fades out over the last fadeDuration of
// it. A message too long for the frame wraps and takes as many of the rows as it needs.
//
// Inputs: combatError_ (every place that wrote the old single red line under the frames still writes it;
// it is taken into the stack once a frame - uiErrorsTick), WorldSession::takeUiError (refusals the session
// hears: upstream addUIError / raiseUiError), uiInfo() (the mode line of the PSP controls, yellow).
// PSP: the frame is 307 px wide (512 units at the 0.6 the other frames use) with its top at y 104 (under
// the unit frames and the debug lines - where the single line was); kFontNormal (13 px) for ErrorFont.
// Over the trainer / talent frames the stack is drawn after the windows (drawTrainerOverlay), as the
// single line was before (UIErrorsFrame is HIGH strata, the panels MEDIUM).
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kFrameTop = 104.0f;       // the old error line (ui_world_hud.cpp kErrorY)
constexpr float kFrameW = 512.0f * 0.6f;  // MessageFrame 512 units wide
constexpr size_t kRows = 3;               // 60 / ErrorFont 16
constexpr uint32_t kLifeMs = 5000;        // displayDuration="5": the whole life of a line
constexpr uint32_t kFadeMs = 3000;        // fadeDuration (the UI.xsd default, 3.0 s): the end of that life
constexpr uint32_t kRgbError = 0x001A1AFFu;  // (1.0, 0.1, 0.1) as 0x00BBGGRR
constexpr uint32_t kRgbInfo = 0x0000FFFFu;   // (1.0, 1.0, 0.0)

// Word wrap to the frame's width (UTF-8, the font's advances); a word wider than the frame breaks.
std::vector<std::string> wrapRows(const Font& f, const std::string& text, float maxW) {
    std::vector<std::string> rows;
    std::string cur;
    float w = 0.0f, breakW = 0.0f;
    size_t breakAt = 0;  // bytes of cur up to and including the last space
    const char* s = text.c_str();
    while (*s) {
        const char* p0 = s;
        const uint32_t cp = utf8Next(&s);
        const Glyph* g = f.find(cp);
        const float adv = g ? static_cast<float>(g->advance) : 0.0f;
        if (w + adv > maxW && !cur.empty()) {
            if (breakAt > 0) {
                rows.push_back(cur.substr(0, breakAt - 1));  // without the space it broke at
                cur.erase(0, breakAt);
                w -= breakW;
            } else {
                rows.push_back(cur);
                cur.clear();
                w = 0.0f;
            }
            breakAt = 0;
            breakW = 0.0f;
            if (cur.empty() && cp == ' ') continue;
        }
        cur.append(p0, static_cast<size_t>(s - p0));
        w += adv;
        if (cp == ' ') {
            breakAt = cur.size();
            breakW = w;
        }
    }
    if (!cur.empty()) rows.push_back(cur);
    return rows;
}

}  // namespace

// MessageFrame:AddMessage with insertMode TOP: the message's rows on top, the older rows pushed down, what
// no longer fits the three rows is gone.
void WorldScreen::uiErrorAdd(const std::string& text, uint32_t rgb) {
    if (text.empty()) return;
    const uint32_t now = pp::logUptimeMs();
    std::vector<std::string> rows = wrapRows(kit_.font(kFontNormal), text, kFrameW);
    if (rows.size() > kRows) rows.resize(kRows);
    std::vector<UiErrorRow> added;
    for (auto& r : rows) added.push_back(UiErrorRow{std::move(r), rgb & 0x00FFFFFFu, now});
    uiErrors_.insert(uiErrors_.begin(), added.begin(), added.end());
    if (uiErrors_.size() > kRows) uiErrors_.resize(kRows);
    static uint32_t logs = 0;
    if (++logs <= 60)
        PSP_LOGI(pp::tag::Gfx, "ui_error kind=%s len=%u rows=%u shown=%u", rgb == kRgbInfo ? "info" : "error",
                 static_cast<unsigned>(text.size()), static_cast<unsigned>(rows.size()),
                 static_cast<unsigned>(uiErrors_.size()));
}

void WorldScreen::uiError(const std::string& text) { uiErrorAdd(text, kRgbError); }

void WorldScreen::uiInfo(const std::string& text) { uiErrorAdd(text, kRgbInfo); }

// Once a frame (drawHudLines): the inputs into the stack, the rows whose fade ended out.
void WorldScreen::uiErrorsTick() {
    if (!combatError_.empty()) {
        uiError(combatError_);
        combatError_.clear();
    }
    std::string s;
    while (session_.takeUiError(s)) uiError(s);
    const uint32_t now = pp::logUptimeMs();
    uiErrors_.erase(std::remove_if(uiErrors_.begin(), uiErrors_.end(),
                                   [now](const UiErrorRow& r) { return now - r.bornMs >= kLifeMs; }),
                    uiErrors_.end());
}

bool WorldScreen::uiErrorsOverWindows() const {
    return windowOpen(kWinTrainer) || windowOpen(kWinTalents) ||
           windowOpen(kWinTradeSkill);  // TradeSkillFrame sits there too
}

float WorldScreen::uiErrorsBottom() const {
    const Font& f = kit_.font(kFontNormal);
    const float lh = static_cast<float>(f.lineHeight() > 0 ? f.lineHeight() : 15);
    return kFrameTop + lh * static_cast<float>(uiErrors_.size());
}

void WorldScreen::drawUiErrors(UiRenderer& ui) {
    const Font& f = kit_.font(kFontNormal);
    const float lh = static_cast<float>(f.lineHeight() > 0 ? f.lineHeight() : 15);
    const uint32_t now = pp::logUptimeMs();
    float y = kFrameTop;
    for (const UiErrorRow& r : uiErrors_) {
        const uint32_t age = now - r.bornMs;
        float a = 1.0f;  // upstream expireMessages: full alpha, then left / fade over the last fadeDuration
        if (age >= kLifeMs) a = 0.0f;
        else if (kLifeMs - age < kFadeMs) a = static_cast<float>(kLifeMs - age) / static_cast<float>(kFadeMs);
        const uint32_t alpha = static_cast<uint32_t>(a * 255.0f + 0.5f);
        if (alpha >= 8) ui.textShadow(f, pp::kScreenW * 0.5f, y, r.text.c_str(), (alpha << 24) | r.color, Align::Center);
        y += lh;
    }
}

}  // namespace wowee::ui::psp
