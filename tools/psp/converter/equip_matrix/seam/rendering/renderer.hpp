#pragma once
// PC seam for upstream's rendering::Renderer - only the
// accessors the equipment code reaches through (see character_renderer.hpp).
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

class CharacterRenderer;
class M2Renderer;
class WMORenderer;
class AnimationController;
class CameraController;
class VkContext;

class Renderer {
public:
    CharacterRenderer* getCharacterRenderer() const { return characterRenderer; }
    M2Renderer* getM2Renderer() const { return m2Renderer; }
    WMORenderer* getWMORenderer() const { return wmoRenderer; }
    AnimationController* getAnimationController() const { return animationController; }
    CameraController* getCameraController() const { return nullptr; }
    VkContext* getVkContext() const { return nullptr; }
    uint32_t getCharacterInstanceId() const { return characterInstanceId; }
    const glm::vec3& getCharacterPosition() const { return characterPosition; }
    float getCharacterYaw() const { return 0.0f; }

    CharacterRenderer* characterRenderer = nullptr;
    M2Renderer* m2Renderer = nullptr;
    WMORenderer* wmoRenderer = nullptr;
    AnimationController* animationController = nullptr;
    uint32_t characterInstanceId = 0;
    glm::vec3 characterPosition{0.0f};
};

}  // namespace rendering
}  // namespace wowee
