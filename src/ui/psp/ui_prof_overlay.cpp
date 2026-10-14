#include "ui/psp/ui_prof_overlay.hpp"

#include "platform/psp/psp_prof.hpp"

#include <algorithm>
#include <cstdio>

namespace wowee::ui::psp {

namespace prof = wowee::platform::psp::prof;

void drawProfOverlay(UiRenderer& ui, const UiKit& kit) {
    PROF_SCOPE("prof.overlay");
    const prof::Summary& s = prof::summary();
    const Font& f = kit.font(kFontSmall);
    constexpr float kX = 6, kW = 300, kLine = 11;
    float y = 40;
    ui.beginFrame();
    ui.fill(Rect{kX - 4, y - 3, kW + 8, 224}, 0xB0000000u);
    char line[128];

    std::snprintf(line, sizeof(line), "PROF %.1f fps  %.2f ms  CPU %.0f%%", s.fps, s.frameMs, s.cpuLoad * 100.0f);
    ui.text(f, kX, y, line, kColGold);
    y += kLine + 1;

    // Frame bar over two vblanks (33.3 ms): CPU build | GE tail | vblank wait.
    const float scale = kW / 33.33f;
    float x = kX;
    auto seg = [&](float ms, uint32_t abgr) {
        const float w = std::min(ms * scale, kX + kW - x);
        if (w > 0) ui.fill(Rect{x, y, w, 6}, abgr);
        x += w;
    };
    seg(s.buildMs, 0xFF40A0FFu);
    seg(s.geTailMs, 0xFF4040FFu);
    seg(s.vblankMs, 0xFF808080u);
    ui.fill(Rect{kX + 16.67f * scale, y - 1, 1, 8}, 0xFFFFFFFFu);
    y += 8;
    std::snprintf(line, sizeof(line), "cpu %.2f  ge tail %.2f  vbl %.2f  ge busy %.2f", s.buildMs, s.geTailMs,
                  s.vblankMs, s.geBusyMs);
    ui.text(f, kX, y, line, kColWhite);
    y += kLine;

    // GE passes, wrapped: the line must stay inside the panel.
    int len = std::snprintf(line, sizeof(line), "GE:");
    int onLine = 0;
    for (int i = 0; i < s.passCount; ++i) {
        if (s.passMs[i] < 0.005f) continue;
        if (onLine == 4) {
            ui.text(f, kX, y, line, 0xFFFFC080u);
            y += kLine;
            len = std::snprintf(line, sizeof(line), "   ");
            onLine = 0;
        }
        len += std::snprintf(line + len, sizeof(line) - len, " %s %.2f", s.passName[i], s.passMs[i]);
        ++onLine;
    }
    ui.text(f, kX, y, line, 0xFFFFC080u);
    y += kLine + 2;

    int order[prof::kMaxZones];
    for (int i = 0; i < s.zoneCount; ++i) order[i] = i;
    std::sort(order, order + s.zoneCount, [&](int a, int b) { return s.zoneSelfMs[a] > s.zoneSelfMs[b]; });
    ui.text(f, kX, y, "zone             self   incl  calls", kColGrey);
    y += kLine;
    for (int k = 0; k < s.zoneCount && k < 9; ++k) {
        const int i = order[k];
        std::snprintf(line, sizeof(line), "%-15s %6.2f %6.2f %5.0f", s.zoneName[i], s.zoneSelfMs[i], s.zoneInclMs[i],
                      s.zoneCalls[i]);
        ui.text(f, kX, y, line, kColWhite);
        y += kLine;
    }
    y += 2;
    std::snprintf(line, sizeof(line), "draws %.0f  tris %.0f  verts %.0f  binds %.0f", s.counters[prof::kDraws],
                  s.counters[prof::kTriangles], s.counters[prof::kVertices], s.counters[prof::kTexBinds]);
    ui.text(f, kX, y, line, kColGreen);
    y += kLine;
    std::snprintf(line, sizeof(line), "heap %u KB  free block %u KB", static_cast<unsigned>(s.heapUsedKb),
                  static_cast<unsigned>(s.heapFreeBlockKb));
    ui.text(f, kX, y, line, kColGrey);
    y += kLine;
    if (prof::capturing()) {
        std::snprintf(line, sizeof(line), "CAPTURE: %u frames left", static_cast<unsigned>(s.captureFramesLeft));
        ui.text(f, kX, y, line, kColRed);
    } else {
        ui.text(f, kX, y, "SELECT: hide   L+R+SELECT: capture 300 frames", kColGrey);
    }
    ui.endFrame();
}

} // namespace wowee::ui::psp
