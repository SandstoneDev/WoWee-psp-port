#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace wowee::platform::psp {
constexpr int frameTargetFps(int cap) { return cap <= 30 ? 30 : 60; }
constexpr int frameIntervalForTarget(int cap) { return frameTargetFps(cap) == 30 ? 2 : 1; }
// Feed work before the presentation wait, never capped frame time. Thresholds
// leave headroom for the selected 30 or 60 FPS presentation deadline.
class FrameBudget {
public:
    bool sample(uint32_t workUs, float dt, bool enabled, int targetFps = 30) {
        const int old = level_;
        const int target = frameTargetFps(targetFps);
        if (target != targetFps_) {
            targetFps_=target; averageUs_=0; bad_=good_=age_=severe_=0;
        }
        if (!enabled) {
            level_=0; active_=false; averageUs_=0; bad_=good_=age_=severe_=0;
            return old!=level_;
        }
        if (!active_) { active_=true; level_=1; }
        if (!workUs || !(dt>0) || !std::isfinite(dt)) return old!=level_;
        dt=std::min(dt,.1f); // a single card/dialog stall cannot collapse quality
        const float work=static_cast<float>(std::min<uint32_t>(workUs,60000u));
        averageUs_=averageUs_>0 ? averageUs_+(work-averageUs_)*(1-std::exp(-dt/.35f)) : work;
        age_+=dt;
        const float scale=30.f/static_cast<float>(targetFps_);
        severe_ = static_cast<float>(workUs) > 65000.f*scale ? severe_+dt : 0;
        // Three or more overloaded frames: towns need a much smaller working set.
        // A single card stall still cannot engage the emergency tier.
        if (severe_ >= .25f && level_ < 5) { level_=5; bad_=good_=age_=0; }
        // At 30 FPS the deadline is 33.3 ms (16.7 at 60). The old 29 ms
        // average cut line / 28 ms restore line could keep a capped, healthy
        // 29.5 ms scene permanently reduced. Reserve ~10% for a restore trial,
        // then cut back promptly if that extra detail overloads the scene.
        if (work>32000.f*scale || averageUs_>31000.f*scale) {
            bad_+=dt; good_=std::max(0.f,good_-dt*2.f);
            if (bad_>=.30f && age_>=.5f && level_<4) { ++level_; bad_=good_=0; age_=0; }
        } else if (work<31500.f*scale && averageUs_<30000.f*scale) {
            good_+=dt; bad_=0;
            if (good_>=3 && age_>=3 && level_>0) { --level_; good_=0; age_=0; }
        } else { bad_=0; good_=std::max(0.f,good_-dt*.5f); }
        return old!=level_;
    }
    int level() const { return level_; }
    int targetFps() const { return targetFps_; }
    float workUs() const { return averageUs_; }
    float distance() const { constexpr float v[]={1,.9f,.75f,.6f,.5f,.3f}; return v[level_]; }
    float terrain() const { constexpr float v[]={1,.85f,.7f,.55f,.4f,.25f}; return v[level_]; }
    float cosmetic() const { constexpr float v[]={1,.7f,.4f,.2f,.1f,0.f}; return v[level_]; }
    float spells() const { constexpr float v[]={1,.9f,.75f,.55f,.4f,.3f}; return v[level_]; }
    uint32_t crowdLimit(uint32_t requested, uint32_t floor) const {
        if (level_==5) return std::min<uint32_t>(requested,2u);
        const float scale=level_<2 ? 1.f : level_==2 ? .8f : level_==3 ? .6f : .4f;
        return std::min(requested,std::max(floor,static_cast<uint32_t>(requested*scale)));
    }
private:
    int level_=0;
    int targetFps_=30;
    bool active_=false;
    float averageUs_=0, bad_=0, good_=0, age_=0, severe_=0;
};
} // namespace wowee::platform::psp
