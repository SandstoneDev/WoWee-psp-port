#pragma once
// PC seam - upstream's AnimationController as the equipment code calls it (nothing recorded).
#include <cstdint>

namespace wowee {
namespace rendering {

class AnimationController {
public:
    void setRangedWeaponActive(bool active) { rangedWeaponActive = active; }
    template <typename... A>
    void setMounted(A&&...) {}
    // NPC emote state -> animation: the pose a spawned NPC starts in, not its clothes.
    static uint32_t getEmoteAnimByEmotesId(uint32_t) { return 0; }
    bool rangedWeaponActive = false;
};

}  // namespace rendering
}  // namespace wowee
