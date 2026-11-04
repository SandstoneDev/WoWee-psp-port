// Camera-only follow math; no game state or PSP SDK dependencies.
#pragma once
#include <algorithm>
#include <cmath>
namespace wowee::ui::psp {
struct SmartFollowCamera {
    static float movementBasis(float cameraYaw, float facing, bool autorun) {
        return autorun ? facing : cameraYaw;
    }
    static float follow(float yaw, float facing, float dt, bool enabled, bool manual, bool moving) {
        if (!enabled || manual || !moving || !(dt > 0)) return yaw;
        constexpr float tau = 6.28318530718f;
        float delta = std::remainder(facing - yaw, tau);
        // Exactly opposite has two equal arcs; rounding must not alternate them.
        if (std::abs(std::abs(delta)-tau*.5f)<.0001f) delta=tau*.5f;
        // The old camera's smooth follow rate, applied AFTER movement each frame.
        if (std::abs(delta) < .001f) return yaw + delta;
        return yaw + delta * (1.0f - std::exp(-1.6f * std::min(dt, .1f)));
    }
};
// Only a near-straight reverse gesture needs a stable basis: otherwise the
// camera swing changes the reverse movement heading again every frame.
class ReverseStickTurn {
public:
    float basis(float cameraYaw,float facing,bool autorun,bool smart,float x,float y,bool manual) {
        if (!smart || manual || autorun || y<.35f || std::abs(x)>.25f) held_=false;
        if (!held_ && smart && !manual && !autorun && y>.55f && std::abs(x)<.12f) {
            held_=true; basis_=cameraYaw;
        }
        return held_ ? basis_ : SmartFollowCamera::movementBasis(cameraYaw,facing,autorun);
    }
    void reset() { held_=false; }
private:
    bool held_=false;
    float basis_=0;
};
} // namespace wowee::ui::psp
