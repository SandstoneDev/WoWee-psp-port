#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace wowee::ui::psp {
// Generic mask-based repeat. No PSP SDK dependency; never repeats activation buttons.
struct UiDpadRepeat {
    uint32_t direction = 0;
    float elapsed = 0, next = .32f;
    uint32_t step(uint32_t held, uint32_t pressed, float dt,
                  float delay = .32f, float interval = .09f, float fastInterval = .055f) {
        if (!held) { direction = 0; elapsed = 0; next = delay; return 0; }
        const uint32_t candidate = pressed ? pressed & (~pressed + 1u) :
            (held & direction) ? direction : held & (~held + 1u);
        if (candidate != direction || (pressed & candidate)) {
            direction = candidate; elapsed = 0; next = delay; return candidate;
        }
        if (!std::isfinite(dt) || dt <= 0) return 0;
        elapsed += std::min(dt, .1f);
        if (elapsed < next) return 0;
        next = elapsed + (elapsed > 1.2f ? fastInterval : interval);
        return direction;
    }
};
} // namespace wowee::ui::psp
