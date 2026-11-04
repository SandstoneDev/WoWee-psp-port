#pragma once
// PC seam - upstream's WMORenderer (buildings, transports): not part of a character's
// equipment; every call answers "nothing" so the surrounding upstream code compiles and stays unused.
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { struct BLPImage; }
namespace rendering {

class WMORenderer {
public:
    struct DoodadTemplate {
        std::string m2Path;
        glm::mat4 localTransform{1.0f};
    };
    template <typename... A> bool loadModel(A&&...) { return false; }
    template <typename... A> bool loadModelIncremental(A&&...) { return false; }
    template <typename... A> bool isModelLoaded(A&&...) const { return false; }
    template <typename... A> uint32_t createInstance(A&&...) { return 0; }
    template <typename... A> void removeInstance(A&&...) {}
    template <typename... A> void setInstanceTransform(A&&...) {}
    template <typename... A> bool hasInstance(A&&...) const { return false; }
    template <typename... A> void addDoodadToInstance(A&&...) {}
    template <typename... A> const std::vector<DoodadTemplate>* getDoodadTemplates(A&&...) const { return nullptr; }
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>*) {}
};

}  // namespace rendering
}  // namespace wowee
