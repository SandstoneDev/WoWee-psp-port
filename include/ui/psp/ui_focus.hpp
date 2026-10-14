// Controller navigation for the PSP UI: a geometric d-pad focus ring plus a
// nub-driven cursor, sharing one position (pattern from the Derma port,
// source-psp glua_panel.c nav_pick/nav_nearest_to).
//
// Immediate mode: every frame the screen declares its focusable widgets with
// addStop(); navigation for the next frame is resolved against that list, so
// no screen has to wire neighbours by hand.
#pragma once

#include "platform/psp/psp_input.hpp"
#include "ui/psp/ui_renderer.hpp"

#include <cstdint>

namespace wowee::ui::psp {

class FocusRing {
public:
    static constexpr int kMaxStops = 384;  // was 64; talent grid + action bar + auras, 5 bags;
    // the bank and the mailbox: 224 -> 384 - the bank, its bags and the 5 bags at once

    // Call once per frame before the screen draws: applies input to the
    // stops declared LAST frame, then clears the list.
    void update(const platform::psp::PadState& pad, float dtSec);

    // Declares a focusable widget. Returns true if it is focused this frame.
    bool addStop(uint32_t id, const Rect& r);
    // PSP (owner: "a click on the chat under a window still opens the chat"): an opaque area - a
    // window's rect, declared before the window's own stops. Under the cursor it hides everything declared
    // before it, as a window hides the mouse from what is under it; Cross there does nothing.
    void addBlocker(const Rect& r);

    uint32_t focused() const { return focused_; }
    // Movable windows: stops declared from now on are shifted by
    // (dx, dy) - the same offset the renderer draws them with.
    void setOffset(float dx, float dy) {
        ox_ = dx;
        oy_ = dy;
    }
    // Puts focus on `id` even if that widget has not been declared yet (a
    // screen that is about to appear): kept until it shows up, instead of
    // falling back to the stop nearest the cursor.
    void setFocus(uint32_t id);
    // The widget activated this frame, or 0. PSP (owner: "Cross the left mouse button, Circle the
    // right one"): activated() = Cross released on it, activatedAlt() = Circle released on it.
    uint32_t activated() const { return activated_; }
    uint32_t activatedAlt() const { return activatedAlt_; }
    // Stops whose centre lies under one of these rects (the windows drawn over the one being
    // declared now) are not declared - a widget covered by another window takes no focus.
    void setOccluders(const Rect* r, int n) {
        occluderCount_ = n < kMaxOccluders ? n : kMaxOccluders;
        for (int i = 0; i < occluderCount_; ++i) occluders_[i] = r[i];
    }
    bool isPressed(uint32_t id) const { return id == focused_ && pressHeld_; }

    float cursorX() const { return cx_; }
    float cursorY() const { return cy_; }
    bool cursorVisible() const { return cursorVisible_; }

private:
    struct Stop {
        uint32_t id;
        Rect r;
    };
    int pick(int cur, int dx, int dy) const;
    int nearestTo(float px, float py) const;
    int indexOf(uint32_t id) const;

    Stop stops_[kMaxStops];
    int count_ = 0;
    Stop prev_[kMaxStops];
    int prevCount_ = 0;

    uint32_t focused_ = 0;
    bool focusPending_ = false;
    uint32_t activated_ = 0, activatedAlt_ = 0;
    bool pressHeld_ = false;
    static constexpr int kMaxOccluders = 16;
    Rect occluders_[kMaxOccluders];
    int occluderCount_ = 0;
    float cx_ = 240.0f, cy_ = 136.0f;
    float ox_ = 0.0f, oy_ = 0.0f;
    float speed_ = 0.0f;
    bool cursorVisible_ = false;
};

} // namespace wowee::ui::psp
