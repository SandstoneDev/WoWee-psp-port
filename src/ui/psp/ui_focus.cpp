#include "ui/psp/ui_focus.hpp"

#include "platform/psp/psp_display.hpp"

#include <pspctrl.h>

#include <cmath>
#include <algorithm>

namespace wowee::ui::psp {

namespace {
// Cursor tuning, from the Derma port's hardware-tried values.
constexpr float kSpeedMin = 220.0f;  // px/s at the edge of the dead zone
constexpr float kSpeedMax = 600.0f;
constexpr float kAccel = 500.0f;     // px/s^2 while held
} // namespace

int FocusRing::indexOf(uint32_t id) const {
    if (id == 0) return -1;  // id 0 = a blocker, never a widget
    for (int i = 0; i < prevCount_; ++i)
        if (prev_[i].id == id) return i;
    return -1;
}

bool FocusRing::navigable(int i) const {
    return prev_[i].id && (!modalGroup_ || prev_[i].group == modalGroup_) &&
           (!panelNavigation_ || prev_[i].group == navGroup_);
}

void FocusRing::focusGroup(uint8_t group) {
    const int old=indexOf(focused_);
    const uint32_t oldKind=focused_&0xFFFF0000u;
    if(old>=0 && oldKind!=0x00FE0000u && oldKind!=0x00FD0000u && oldKind!=0x00FB0000u && oldKind!=0x00FA0000u)remembered_[prev_[old].group]=focused_;
    navGroup_ = group;
    cursorVisible_ = false;
    groupPending_ = true;
    setFocus(remembered_[group]);
    groupWait_=remembered_[group]?1:0;
}

void FocusRing::cycleGroup(int direction) {
    if (modalGroup_) return;
    uint8_t groups[256]; int n = 0;
    for (int i = 0; i < prevCount_; ++i) {
        if (!prev_[i].id || !prev_[i].group) continue;
        bool seen = false;
        for (int j = 0; j < n; ++j) seen |= groups[j] == prev_[i].group;
        if (!seen) groups[n++] = prev_[i].group;
    }
    if (n < 2) return;
    int current = 0;
    for (int i = 0; i < n; ++i) if (groups[i] == navGroup_) current = i;
    focusGroup(groups[(current + direction + n) % n]);
}

bool FocusRing::focusedRect(Rect& out) const {
    for (int i = count_ - 1; i >= 0; --i)
        if (stops_[i].id == focused_ && (!modalGroup_ || stops_[i].group == modalGroup_)) { out = stops_[i].r; return true; }
    return false;
}

int FocusRing::nearestTo(float px, float py) const {
    int best = -1;
    float bestD = 1e30f;
    for (int i = 0; i < prevCount_; ++i) {
        if (!navigable(i)) continue;
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
        if (i == cur || !navigable(i)) continue;
        const Rect& r = prev_[i].r;
        const float rcx = r.x + r.w * 0.5f, rcy = r.y + r.h * 0.5f;
        float along, across;
        bool overlap;
        if (dx != 0) {
            if (dx > 0 ? rcx <= ccx + .5f : rcx >= ccx - .5f) continue;
            along = std::fabs(rcx - ccx);
            across = std::fabs(rcy - ccy);
            overlap = r.y < c.y + c.h && r.y + r.h > c.y;
        } else {
            if (dy > 0 ? rcy <= ccy + .5f : rcy >= ccy - .5f) continue;
            along = std::fabs(rcy - ccy);
            across = std::fabs(rcx - ccx);
            overlap = r.x < c.x + c.w && r.x + r.w > c.x;
        }
        float score = along + across * (overlap ? 1.0f : 3.0f);
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
    emptyAltActivated_ = false;

    if (enhanced_ && panelNavigation_ && (pad.held & PSP_CTRL_LTRIGGER)) {
        if (pad.pressed & PSP_CTRL_LEFT) cycleGroup(-1);
        else if (pad.pressed & PSP_CTRL_RIGHT) cycleGroup(1);
    }
    int cur = indexOf(focused_);
    if (modalGroup_ && ((cur < 0 && !cursorVisible_) || (cur >= 0 && prev_[cur].group != modalGroup_))) focusGroup(modalGroup_);
    if (groupPending_) {
        cur = indexOf(focused_);
        if ((cur < 0 || prev_[cur].group != navGroup_) && groupWait_) {
            --groupWait_; // a covered panel needs one draw to redeclare its saved control
        } else if (cur < 0 || prev_[cur].group != navGroup_) {
            cur = -1;
            // Declaration order supplies a stable starting control, rather
            // than whichever button happens to be closest to the old cursor.
            for(int i=0;i<prevCount_;++i)if(navigable(i)){cur=i;break;}
            if (cur >= 0) { focused_ = prev_[cur].id; focusPending_ = true; }
        }
        if (cur >= 0) groupPending_ = false;
    }
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

    // Repeat only directional input. L+left/right changes panels, never activates widgets.
    uint32_t directions = pad.held & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_UP | PSP_CTRL_DOWN);
    if ((directions & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) == (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) directions &= ~(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
    if ((directions & (PSP_CTRL_UP | PSP_CTRL_DOWN)) == (PSP_CTRL_UP | PSP_CTRL_DOWN)) directions &= ~(PSP_CTRL_UP | PSP_CTRL_DOWN);
    if (enhanced_ && panelNavigation_ && (pad.held & PSP_CTRL_LTRIGGER)) directions &= ~(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
    const uint32_t nav = enhanced_ ? repeat_.step(directions, pad.pressed & directions, dtSec) : pad.pressed & directions;
    analogDelay_ = std::max(0.0f, analogDelay_ - std::clamp(dtSec, 0.0f, .1f));
    // D-pad: jump focus and warp the cursor to the new widget's centre.
    int dx = 0, dy = 0;
    if (nav & PSP_CTRL_LEFT) dx = -1;
    else if (nav & PSP_CTRL_RIGHT) dx = 1;
    else if (nav & PSP_CTRL_UP) dy = -1;
    else if (nav & PSP_CTRL_DOWN) dy = 1;
    if (dx || dy) {
        int next = pick(cur, dx, dy);
        if (next >= 0) {
            cur = next;
            focused_ = prev_[cur].id;
            cx_ = prev_[cur].r.x + prev_[cur].r.w * 0.5f;
            cy_ = prev_[cur].r.y + prev_[cur].r.h * 0.5f;
        }
        cursorVisible_ = false;
        analogDelay_ = .2f;
    }

    // Nub: free cursor with acceleration; hovering a widget focuses it.
    if (!nav && analogDelay_ <= 0 && (std::abs(pad.ax) > .15f || std::abs(pad.ay) > .15f)) {
        speed_ = speed_ <= 0.0f ? kSpeedMin : std::fmin(kSpeedMax, speed_ + kAccel * dtSec);
        cx_ += pad.ax * speed_ * dtSec;
        cy_ += pad.ay * speed_ * dtSec;
        cx_ = std::fmin(std::fmax(cx_, 0.0f), platform::psp::kScreenW - 1.0f);
        cy_ = std::fmin(std::fmax(cy_, 0.0f), platform::psp::kScreenH - 1.0f);
        cursorVisible_ = true;
        focusPending_ = groupPending_ = false;
        // layers - the last declared stop is drawn on top: it takes
        // the cursor where widgets overlap (input goes to the topmost).
        // A blocker (a window) on top takes it too - nothing is focused there; over empty space
        // nothing is focused either (was: the last widget kept the focus, and Cross clicked it).
        focused_ = 0;
        for (int i = prevCount_ - 1; i >= 0; --i) {
            if ((!modalGroup_ || prev_[i].group == modalGroup_) && prev_[i].r.contains(cx_, cy_)) {
                focused_ = prev_[i].id;
                break;
            }
        }
    } else {
        speed_ = 0.0f;
    }

    int selected = indexOf(focused_);
    if(selected>=0 && !navigable(selected)) { focused_=0;selected=-1;cancelPresses(); }
    if (selected >= 0) {
        navGroup_ = prev_[selected].group;
        const uint32_t kind=focused_&0xFFFF0000u;
        if(kind!=0x00FE0000u && kind!=0x00FD0000u && kind!=0x00FB0000u && kind!=0x00FA0000u)remembered_[navGroup_] = focused_;
    }
    // A release may only activate the same widget on which this click began.
    if (pad.pressed & PSP_CTRL_CROSS) clickDown_ = focused_;
    if (pad.pressed & PSP_CTRL_CIRCLE) {
        altDown_ = focused_;
        altEmptyHeld_ = focused_ == 0;
        altStartX_ = cx_; altStartY_ = cy_;
    }
    const float altDx = cx_ - altStartX_, altDy = cy_ - altStartY_;
    if (focused_ || altDx*altDx + altDy*altDy > 64.0f) altEmptyHeld_ = false;
    if (clickDown_ != focused_ || selected < 0) clickDown_ = 0;
    if (altDown_ != focused_ || selected < 0) altDown_ = 0;
    if (pad.released & PSP_CTRL_CROSS) { if (clickDown_) activated_ = clickDown_; clickDown_ = 0; }
    if (pad.released & PSP_CTRL_CIRCLE) {
        if (altDown_) activatedAlt_ = altDown_;
        emptyAltActivated_ = altEmptyHeld_; altEmptyHeld_ = false; altDown_ = 0;
    }
    pressHeld_ = ((pad.held & PSP_CTRL_CROSS) && clickDown_) || ((pad.held & PSP_CTRL_CIRCLE) && altDown_);
}

void FocusRing::addBlocker(const Rect& r) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    if (count_ < kMaxStops-2) stops_[count_++] = Stop{0, Rect{r.x + ox_, r.y + oy_, r.w, r.h}, group_};
}

void FocusRing::setFocus(uint32_t id) {
    focused_ = id;
    focusPending_ = id != 0;
    clickDown_ = altDown_ = 0;
    altEmptyHeld_ = false;
}

bool FocusRing::addStop(uint32_t id, const Rect& r) {
    const Rect a{r.x + ox_, r.y + oy_, r.w, r.h};
    for (int i = 0; i < occluderCount_; ++i)  // under a window drawn over this one
        if (occluders_[i].contains(a.x + a.w * 0.5f, a.y + a.h * 0.5f)) return false;
    const uint32_t kind=id&0xFFFF0000u;
    const bool chrome=kind==0x00FE0000u || kind==0x00FD0000u || kind==0x00FB0000u || kind==0x00FA0000u;
    if (count_ < (chrome?kMaxStops:kMaxStops-2)) stops_[count_++] = Stop{id, a, group_};
    return id == focused_;
}

} // namespace wowee::ui::psp
