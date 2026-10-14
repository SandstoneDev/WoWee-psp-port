// One layer manager for everything drawn over the world - the order
// of drawing and of input, FrameXML's model (upstream widget_tree.hpp:
// FrameStrata WORLD..TOOLTIP, then frame level, then registration order).
//
// Owner's rule (batch 3, answers to PR #2): HUD -> windows -> dialogs ->
// cursor, one order for drawing and input. A client is anything that draws a
// group of UI: the FrameXML frames of one strata, the chat, the HUD text, the
// window stack, the settings menu, the cursor. Each registers with a strata
// and a level; draw() calls them bottom to top, hitTest() asks them top to
// bottom, so what is drawn on top is also what gets the click.
//
// Integration plan (docs/PSP_PORT_ARCHITECTURE.md): today only FrameXML
// registers (one client per strata it uses). Next, WorldScreen::draw
// registers its passes instead of calling them in a fixed order:
//   chat          LOW, level 0     (under the unit frames)
//   HUD text      MEDIUM, level 0  (names, combat text, debug lines)
//   unit frames   MEDIUM, level 10 (B's frames until FrameXML takes over)
//   windows       DIALOG, level = stack position (top window = highest)
//   settings      FULLSCREEN_DIALOG
//   tooltips      TOOLTIP
//   cursor        kStrataCursor (always last)
// Each client keeps its own GE pass / profiler zone inside draw().
#pragma once

#include "ui/psp/ui_renderer.hpp"

#include <cstdint>
#include <vector>

namespace wowee::ui::psp {

// FrameXML strata (fx_format.hpp FxStrata) + the cursor above everything.
enum UiStrata : uint8_t {
    kUiWorld = 0,
    kUiBackground,
    kUiLow,
    kUiMedium,
    kUiHigh,
    kUiDialog,
    kUiFullscreen,
    kUiFullscreenDialog,
    kUiTooltip,
    kUiCursor,
    kUiStrataCount
};

class UiLayers {
public:
    using DrawFn = void (*)(void* ctx, UiRenderer& ui, uint8_t strata);
    // Screen px. Returns true when the client has something under the point
    // (it then gets the pointer; clients below do not).
    using HitFn = bool (*)(void* ctx, float x, float y, uint8_t strata);

    struct Client {
        const char* name = "";
        uint8_t strata = kUiMedium;
        int16_t level = 0;
        bool visible = true;
        void* ctx = nullptr;
        DrawFn draw = nullptr;
        HitFn hit = nullptr;
        uint32_t order = 0;  // registration order (ties)
    };

    // Returns a handle (>= 0) for setVisible / setLevel / remove.
    int add(const Client& c);
    void remove(int handle);
    void setVisible(int handle, bool v);
    void setLevel(int handle, int16_t level);

    // Bottom to top.
    void draw(UiRenderer& ui);
    // Top to bottom; the handle of the client that took the point, or -1.
    int hitTest(float x, float y) const;
    uint32_t clientCount() const;

private:
    void sort();
    std::vector<Client> clients_;
    std::vector<int> order_;  // indices into clients_, drawing order
    std::vector<bool> alive_;
    bool dirty_ = true;
    uint32_t next_ = 0;
};

}  // namespace wowee::ui::psp
