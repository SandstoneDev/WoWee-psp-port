#include "ui/psp/ui_focus.hpp"

#include "platform/psp/psp_display.hpp"

#include <pspctrl.h>

#include <cmath>

namespace wowee::ui::psp {

namespace {
// Cursor tuning, from the Derma port's hardware-tried values.
constexpr float kSpeedMin = 220.0f;  // px/s at the edge of the dead zone
constexpr float kSpeedMax = 600.0f;
constexpr float kAccel = 500.0f;     // px/s^2 while held
constexpr uint32_t kActivate = PSP_CTRL_CROSS | PSP_CTRL_CIRCLE;
} // namespace

int FocusRing::indexOf(uint32_t id) const {
    if (id == 0) return -1;  // id 0 = a blocker, never a widget
    for (int i = 0; i < prevCount_; ++i)
        if (prev_[i].id == id) return i;
    return -1;
}

int FocusRing::nearestTo(float px, float py) const {
    int best = -1;
    float bestD = 1e30f;
    for (int i = 0; i < prevCount_; ++i) {
        if (prev_[i].id == 0) continue;  // a blocker
        const Rect& r = prev_[i].r;
        float dx = r.x + r.w * 0.5f - px, dy = r.y + r.h * 0.5f - py;
        float d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// Geometric pick: the candidate's centre must lie beyond the current rect in
// the pressed direction; candidates overlapping the current rect on the
// perpendicular axis win; then the smallest along + across distance.
int FocusRing::pick(int cur, int dx, int dy) const {
    if (cur < 0) return nearestTo(cx_, cy_);
    const Rect& c = prev_[cur].r;
    const float ccx = c.x + c.w * 0.5f, ccy = c.y + c.h * 0.5f;
    int best = -1;
    float bestScore = 1e30f;
    for (int i = 0; i < prevCount_; ++i) {
        if (i == cur || prev_[i].id == 0) continue;  // not a blocker
        const Rect& r = prev_[i].r;
        const float rcx = r.x + r.w * 0.5f, rcy = r.y + r.h * 0.5f;
        float along, across;
        bool overlap;
        if (dx != 0) {
            if (dx > 0 ? rcx <= c.x + c.w : rcx >= c.x) continue;
            along = std::fabs(rcx - ccx);
            across = std::fabs(rcy - ccy);
            overlap = r.y < c.y + c.h && r.y + r.h > c.y;
        } else {
            if (dy > 0 ? rcy <= c.y + c.h : rcy >= c.y) continue;
            along = std::fabs(rcy - ccy);
            across = std::fabs(rcx - ccx);
            overlap = r.x < c.x + c.w && r.x + r.w > c.x;
        }
        float score = along + across * 2.0f - (overlap ? 100000.0f : 0.0f);
        if (score < bestScore) { bestScore = score; best = i; }
    }
    return best;
}

void FocusRing::update(const platform::psp::PadState& pad, float dtSec) {
    // Last frame's declarations become the navigation set.
    for (int i = 0; i < count_; ++i) prev_[i] = stops_[i];
    prevCount_ = count_;
    count_ = 0;
    activated_ = 0;
    activatedAlt_ = 0;

    int cur = indexOf(focused_);
    if (cur >= 0) {
        if (focusPending_) {
            // The requested widget appeared: warp the cursor onto it too.
            focusPending_ = false;
            cx_ = prev_[cur].r.x + prev_[cur].r.w * 0.5f;
            cy_ = prev_[cur].r.y + prev_[cur].r.h * 0.5f;
        }
    } else if (focusPending_) {
        // Keep the requested focus until its widget is declared.
    } else if (prevCount_ > 0 && !cursorVisible_) {
        // Not while the free cursor is out - over empty space it focuses nothing (a mouse).
        cur = nearestTo(cx_, cy_);
        focused_ = cur >= 0 ? prev_[cur].id : 0;
    }

    // D-pad: jump focus and warp the cursor to the new widget's centre.
    int dx = 0, dy = 0;
    if (pad.pressed & PSP_CTRL_LEFT) dx = -1;
    else if (pad.pressed & PSP_CTRL_RIGHT) dx = 1;
    else if (pad.pressed & PSP_CTRL_UP) dy = -1;
    else if (pad.pressed & PSP_CTRL_DOWN) dy = 1;
    if (dx || dy) {
        int next = pick(cur, dx, dy);
        if (next >= 0) {
            cur = next;
            focused_ = prev_[cur].id;
            cx_ = prev_[cur].r.x + prev_[cur].r.w * 0.5f;
            cy_ = prev_[cur].r.y + prev_[cur].r.h * 0.5f;
        }
        cursorVisible_ = false;
    }

    // Nub: free cursor with acceleration; hovering a widget focuses it.
    if (pad.ax != 0.0f || pad.ay != 0.0f) {
        speed_ = speed_ <= 0.0f ? kSpeedMin : std::fmin(kSpeedMax, speed_ + kAccel * dtSec);
        cx_ += pad.ax * speed_ * dtSec;
        cy_ += pad.ay * speed_ * dtSec;
        cx_ = std::fmin(std::fmax(cx_, 0.0f), platform::psp::kScreenW - 1.0f);
        cy_ = std::fmin(std::fmax(cy_, 0.0f), platform::psp::kScreenH - 1.0f);
        cursorVisible_ = true;
        // layers - the last declared stop is drawn on top: it takes
        // the cursor where widgets overlap (input goes to the topmost).
        // A blocker (a window) on top takes it too - nothing is focused there; over empty space
        // nothing is focused either (was: the last widget kept the focus, and Cross clicked it).
        focused_ = 0;
        for (int i = prevCount_ - 1; i >= 0; --i) {
            if (prev_[i].r.contains(cx_, cy_)) {
                focused_ = prev_[i].id;
                break;
            }
        }
    } else {
        speed_ = 0.0f;
    }

    pressHeld_ = (pad.held & kActivate) != 0;
    if ((pad.released & PSP_CTRL_CROSS) && focused_) activated_ = focused_;
    if ((pad.released & PSP_CTRL_CIRCLE) && focused_) activatedAlt_ = focused_;
}

void FocusRing::addBlocker(const Rect& r) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    if (count_ < kMaxStops) stops_[count_++] = Stop{0, Rect{r.x + ox_, r.y + oy_, r.w, r.h}};
}

void FocusRing::setFocus(uint32_t id) {
    focused_ = id;
    focusPending_ = true;
}

bool FocusRing::addStop(uint32_t id, const Rect& r) {
    const Rect a{r.x + ox_, r.y + oy_, r.w, r.h};
    for (int i = 0; i < occluderCount_; ++i)  // under a window drawn over this one
        if (occluders_[i].contains(a.x + a.w * 0.5f, a.y + a.h * 0.5f)) return false;
    if (count_ < kMaxStops) stops_[count_++] = Stop{id, a};
    return id == focused_;
}

} // namespace wowee::ui::psp
