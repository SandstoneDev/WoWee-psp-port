#pragma once
// PC seam for upstream's rendering::CharacterRenderer.
//
// Upstream's equipment code (EntitySpawner, AppearanceComposer) runs
// unchanged; this class stands where the Vulkan renderer stands and records
// what that code asks of it: the geosets it activates, the body composite
// (base, face/underwear layers, equipment region layers in order), the
// textures put on slots and geoset groups, and the models attached to each
// attachment point. Textures are tokens: a VkTexture* here is an index into
// textureNames (never dereferenced). attachWeapon keeps upstream's rule for
// whether the point exists on the character (findAttachmentBone, copied from
// src/rendering/character_renderer.cpp) and whether the model loads.
#include "pipeline/blp_loader.hpp"
#include "pipeline/m2_loader.hpp"

#include <deque>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkTexture;  // never defined: a token
class Camera;

class CharacterRenderer {
public:
    struct Attached {
        std::string model;       // M2Model::name (the path loadM2WithSkin read)
        std::string texture;     // texturePath given to attachWeapon
        bool textureApplied = false;  // the path loaded (upstream: not the white texture)
        glm::mat4 local{1.0f};
        uint32_t modelId = 0;
    };
    struct Composite {
        std::string base;
        std::vector<std::string> baseLayers;
        std::vector<std::pair<int, std::string>> regions;  // empty for compositeTextures
        bool withRegions = false;
    };
    struct Instance {
        uint32_t modelId = 0;
        std::unordered_set<uint16_t> geosets;
        bool geosetsSet = false;
        std::map<uint16_t, VkTexture*> slotOverride;   // texture slot -> token
        std::map<uint16_t, VkTexture*> groupOverride;  // geoset group -> token
        std::map<uint32_t, Attached> attached;         // attachment point -> model
        std::vector<std::pair<uint32_t, uint32_t>> attachFailed;  // (point, why: 1 no bone, 2 model)
        bool visible = true;
    };

    CharacterRenderer() = default;

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }

    // --- what the equipment code calls -------------------------------------
    bool loadModel(const pipeline::M2Model& model, uint32_t id);
    uint32_t createInstance(uint32_t modelId, const glm::vec3& position, const glm::vec3& rotation = glm::vec3(0.0f),
                            float scale = 1.0f);
    void removeInstance(uint32_t instanceId) { instances.erase(instanceId); }
    [[nodiscard]] const pipeline::M2Model* getModelData(uint32_t modelId) const;
    [[nodiscard]] const pipeline::M2Model* getInstanceModelData(uint32_t instanceId) const;
    [[nodiscard]] uint32_t getInstanceModelId(uint32_t instanceId) const {
        auto it = instances.find(instanceId);
        return it != instances.end() ? it->second.modelId : 0;
    }
    void setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets);
    void setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, VkTexture* texture);
    void setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, VkTexture* texture);
    void clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot);
    void setModelTexture(uint32_t modelId, uint32_t textureSlot, VkTexture* texture);
    VkTexture* loadTexture(const std::string& path);
    VkTexture* compositeTextures(const std::vector<std::string>& layerPaths);
    VkTexture* compositeWithRegions(const std::string& basePath, const std::vector<std::string>& baseLayers,
                                   const std::vector<std::pair<int, std::string>>& regionLayers);
    void clearCompositeCache() {}
    bool attachWeapon(uint32_t charInstanceId, uint32_t attachmentId, const pipeline::M2Model& weaponModel,
                      uint32_t weaponModelId, const std::string& texturePath,
                      const glm::mat4& localTransform = glm::mat4(1.0f));
    void detachWeapon(uint32_t charInstanceId, uint32_t attachmentId);
    bool attachWeaponEffect(uint32_t, uint32_t, uint32_t, const pipeline::M2Model&, uint32_t) { return false; }
    void detachWeaponEffects(uint32_t, uint32_t) {}
    void unloadModelIfUnused(uint32_t) {}

    // --- the rest of the surface (animation, placement): nothing to record --
    void playAnimation(uint32_t, uint32_t, bool = true, uint32_t = 0) {}
    void setArmAnimations(uint32_t, uint32_t, uint32_t) {}
    [[nodiscard]] bool hasAnimation(uint32_t, uint32_t) const { return true; }
    bool getAnimationState(uint32_t, uint32_t&, float&, float&) const { return false; }
    bool getAnimationSequences(uint32_t, std::vector<pipeline::M2Sequence>&) const { return false; }
    void startFadeIn(uint32_t, float) {}
    void setInstanceOpacity(uint32_t, float) {}
    void setInstancePosition(uint32_t, const glm::vec3&) {}
    void setInstanceRotation(uint32_t, const glm::vec3&) {}
    void moveInstanceTo(uint32_t, const glm::vec3&, float) {}
    void setInstanceVisible(uint32_t instanceId, bool visible) {
        if (auto it = instances.find(instanceId); it != instances.end()) it->second.visible = visible;
    }
    void setDrawSkinExtra(uint32_t, bool) {}
    void setInstanceSceneModel(uint32_t, bool) {}
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>*) {}
    bool getInstanceBounds(uint32_t, glm::vec3& c, float& r) const {
        c = glm::vec3(0.0f);
        r = 1.0f;
        return true;
    }
    bool getInstanceFootZ(uint32_t, float& z) const {
        z = 0.0f;
        return true;
    }
    bool getInstancePosition(uint32_t, glm::vec3& p) const {
        p = glm::vec3(0.0f);
        return true;
    }
    bool getInstanceHeight(uint32_t, float& h) const {
        h = 2.0f;
        return true;
    }
    bool getInstanceKeyBonePivotZ(uint32_t, int32_t, float&) const { return false; }
    bool getInstanceModelName(uint32_t, std::string&) const { return false; }
    bool getAttachmentTransform(uint32_t, uint32_t, glm::mat4&) { return false; }
    [[nodiscard]] size_t getInstanceCount() const { return instances.size(); }

    // --- matrix side --------------------------------------------------------
    // upstream CharacterRenderer::findAttachmentBone (private there), copied.
    bool findAttachmentBone(uint32_t modelId, uint32_t attachmentId, uint16_t& outBoneIndex, glm::vec3& outOffset) const;
    const std::string& textureName(const VkTexture* t) const;
    VkTexture* white() const { return const_cast<VkTexture*>(reinterpret_cast<const VkTexture*>(&whiteTag_)); }
    // Forget what was recorded on an instance (geosets, overrides, attachments) - one dress per run.
    void resetRecord(uint32_t instanceId);

    pipeline::AssetManager* assetManager = nullptr;
    std::unordered_map<uint32_t, pipeline::M2Model> models;
    std::unordered_map<uint32_t, Instance> instances;
    std::vector<Composite> composites;  // token index = position + 1 in the composite tokens
    std::unordered_map<uint32_t, std::map<uint32_t, VkTexture*>> modelTextures;  // setModelTexture: model -> slot
    uint32_t nextInstanceId = 1;

private:
    VkTexture* token(const std::string& name);
    char whiteTag_ = 0;
    std::deque<std::string> names_;  // token -> name (stable addresses)
    std::unordered_map<std::string, VkTexture*> byName_;
    std::unordered_map<std::string, bool> loads_;  // path -> the BLP decodes
};

}  // namespace rendering
}  // namespace wowee
