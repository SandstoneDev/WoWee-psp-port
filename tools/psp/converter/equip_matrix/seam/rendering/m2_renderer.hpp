#pragma once
// PC seam - upstream's M2Renderer (doodads, game objects): not part of a character's
// equipment; every call answers "nothing" so the surrounding upstream code compiles and stays unused.
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

class M2Renderer {
public:
    template <typename... A> bool loadModel(A&&...) { return false; }
    template <typename... A> bool hasModel(A&&...) const { return false; }
    template <typename... A> uint32_t createInstance(A&&...) { return 0; }
    template <typename... A> void removeInstance(A&&...) {}
    template <typename... A> void setSkipCollision(A&&...) {}
    template <typename... A> void setModelPinned(A&&...) {}
    template <typename... A> void setInstanceIsGameObject(A&&...) {}
    template <typename... A> void setInstanceAnimation(A&&...) {}
    template <typename... A> void setInstanceAnimationFrozen(A&&...) {}
    template <typename... A> void setInstanceAnimationHeld(A&&...) {}
    template <typename... A> bool getInstanceBounds(A&&...) const { return false; }
    template <typename... A> bool hasAnimation(A&&...) const { return false; }
    template <typename... A> bool hasInstance(A&&...) const { return false; }
    template <typename... A> void setInstanceTransform(A&&...) {}
};

}  // namespace rendering
}  // namespace wowee
