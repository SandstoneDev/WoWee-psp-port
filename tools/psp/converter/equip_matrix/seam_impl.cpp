// The seam behind the upstream reference (upstream_ref.hpp). Built with
// equip_matrix/seam/ first on the include path, next to upstream's own
// src/core/entity_spawner.cpp, entity_spawner_player.cpp and
// appearance_composer.cpp (unchanged). This file holds:
//   1. the seam AssetManager (folders of psp_mpq_extract output);
//   2. the seam CharacterRenderer (records what upstream asks of it);
//   3. the EntitySpawner members that live in entity_spawner_processing.cpp
//      (the async spawn queues, transports, game objects - not compiled here):
//      loadWeaponM2 as upstream writes it, the rest never reached;
//   (upstream_ref.cpp dresses characters through upstream's functions).
#include "core/appearance_composer.hpp"
#include "core/character_paths.hpp"
#include "core/entity_spawner.hpp"
#include "core/geoset_rules.hpp"
#include "core/logger.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace wowee;

// ---------------------------------------------------------------------------
// 1. seam AssetManager

namespace wowee {
namespace pipeline {

std::string AssetManager::normalize(const std::string& path) {
    std::string s;
    s.reserve(path.size());
    for (char c : path) {
        if (c == '\\') c = '/';
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

bool AssetManager::initialize(const std::vector<std::string>& roots) {
    roots_ = roots;
    folders_.clear();
    dbcs_.clear();
    files_.clear();
    cacheBytes_ = 0;
    initialized_ = !roots_.empty();
    return initialized_;
}

namespace {
// One folder level, file names lower case. False when the folder is not there.
bool listFolder(const std::string& dir, std::unordered_set<std::string>& out) {
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileExA((dir + "\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.insert(AssetManager::normalize(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) out.insert(AssetManager::normalize(e.path().filename().string()));
    return true;
#endif
}
}  // namespace

const std::unordered_set<std::string>* AssetManager::listing(const std::string& norm, std::string& rootOut) const {
    const size_t slash = norm.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : norm.substr(0, slash + 1);
    auto it = folders_.find(folder);
    if (it == folders_.end()) {
        std::pair<std::string, std::unordered_set<std::string>> entry;
        for (const std::string& root : roots_) {
            std::unordered_set<std::string> names;
            if (listFolder(root + "/" + folder, names)) {
                entry.first = root;
                entry.second = std::move(names);
                break;
            }
        }
        it = folders_.emplace(folder, std::move(entry)).first;
    }
    rootOut = it->second.first;
    return it->second.first.empty() ? nullptr : &it->second.second;
}

bool AssetManager::fileExists(const std::string& path) const {
    if (!initialized_ || path.empty()) return false;
    const std::string norm = normalize(path);
    std::string root;
    const auto* names = listing(norm, root);
    if (!names) return false;
    const size_t slash = norm.rfind('/');
    return names->count(slash == std::string::npos ? norm : norm.substr(slash + 1)) > 0;
}

std::string AssetManager::resolveFile(const std::string& normalizedPath) const {
    const std::string norm = normalize(normalizedPath);
    std::string root;
    const auto* names = listing(norm, root);
    const size_t slash = norm.rfind('/');
    if (!names || !names->count(slash == std::string::npos ? norm : norm.substr(slash + 1))) return {};
    return root + "/" + norm;
}

std::vector<uint8_t> AssetManager::readFile(const std::string& path) const {
    const std::string norm = normalize(path);
    const bool model = norm.size() > 3 && norm.compare(norm.size() - 3, 3, ".m2") == 0;
    if (auto it = files_.find(norm); it != files_.end()) {
        if (model) lastM2_ = norm;
        return it->second;
    }
    const std::string fs = resolveFile(norm);
    std::vector<uint8_t> data;
    if (fs.empty()) return data;
    std::ifstream f(fs, std::ios::binary | std::ios::ate);
    if (!f) return data;
    const auto size = f.tellg();
    if (size <= 0) return data;
    data.resize(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), size);
    if (!f) data.clear();
    if (model && !data.empty()) lastM2_ = norm;
    // Item models are asked for once per race and sex: keep what was read (small files only).
    if (!data.empty() && data.size() <= 4u * 1024u * 1024u && cacheBytes_ + data.size() <= cacheLimit_) {
        cacheBytes_ += data.size();
        files_.emplace(norm, data);
    }
    return data;
}

BLPImage AssetManager::loadTexture(const std::string& path, bool keepCompressed) {
    const std::vector<uint8_t> data = readFile(path);
    if (data.empty()) return {};
    return BLPLoader::load(data, keepCompressed);
}

std::shared_ptr<DBCFile> AssetManager::loadDBC(const std::string& name) {
    if (auto it = dbcs_.find(name); it != dbcs_.end()) return it->second;
    std::shared_ptr<DBCFile> dbc;
    const std::vector<uint8_t> data = readFile("DBFilesClient\\" + name);
    if (!data.empty()) {
        auto d = std::make_shared<DBCFile>();
        if (d->load(data)) dbc = std::move(d);
    }
    dbcs_.emplace(name, dbc);
    return dbc;
}

}  // namespace pipeline

// ---------------------------------------------------------------------------
// 2. seam CharacterRenderer (the recorder)

namespace rendering {

bool CharacterRenderer::loadModel(const pipeline::M2Model& model, uint32_t id) {
    if (!model.isValid()) return false;
    models[id] = model;
    return true;
}

uint32_t CharacterRenderer::createInstance(uint32_t modelId, const glm::vec3&, const glm::vec3&, float) {
    if (!models.count(modelId)) return 0;
    const uint32_t id = nextInstanceId++;
    instances[id].modelId = modelId;
    return id;
}

const pipeline::M2Model* CharacterRenderer::getModelData(uint32_t modelId) const {
    auto it = models.find(modelId);
    return it != models.end() ? &it->second : nullptr;
}

const pipeline::M2Model* CharacterRenderer::getInstanceModelData(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    return it != instances.end() ? getModelData(it->second.modelId) : nullptr;
}

void CharacterRenderer::setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.geosets = geosets;
    it->second.geosetsSet = true;
}

void CharacterRenderer::setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, VkTexture* texture) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    if (texture) it->second.groupOverride[geosetGroup] = texture;
    else it->second.groupOverride.erase(geosetGroup);
}

void CharacterRenderer::setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, VkTexture* texture) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    if (texture) it->second.slotOverride[textureSlot] = texture;
    else it->second.slotOverride.erase(textureSlot);
}

void CharacterRenderer::clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.slotOverride.erase(textureSlot);
}

void CharacterRenderer::setModelTexture(uint32_t modelId, uint32_t textureSlot, VkTexture* texture) {
    if (texture) modelTextures[modelId][textureSlot] = texture;
    else modelTextures[modelId].erase(textureSlot);
}

VkTexture* CharacterRenderer::token(const std::string& name) {
    auto it = byName_.find(name);
    if (it != byName_.end()) return it->second;
    names_.push_back(name);
    auto* t = reinterpret_cast<VkTexture*>(&names_.back());
    byName_.emplace(name, t);
    return t;
}

const std::string& CharacterRenderer::textureName(const VkTexture* t) const {
    static const std::string kWhite = "#white", kNone;
    if (!t) return kNone;
    if (reinterpret_cast<const char*>(t) == &whiteTag_) return kWhite;
    return *reinterpret_cast<const std::string*>(t);
}

// upstream CharacterRenderer::loadTexture: empty / whitespace -> white; a file
// that does not decode -> white; otherwise the texture (cached by path).
VkTexture* CharacterRenderer::loadTexture(const std::string& path) {
    bool blank = true;
    for (char c : path)
        if (c != ' ' && c != '\t' && c != '\0' && c != '\n') blank = false;
    if (path.empty() || blank) return white();
    const std::string key = pipeline::AssetManager::normalize(path);
    auto it = loads_.find(key);
    if (it == loads_.end()) {
        bool ok = false;
        if (assetManager && assetManager->isInitialized()) ok = assetManager->loadTexture(path).isValid();
        it = loads_.emplace(key, ok).first;
    }
    return it->second ? token(key) : white();
}

VkTexture* CharacterRenderer::compositeTextures(const std::vector<std::string>& layerPaths) {
    Composite c;
    if (!layerPaths.empty()) c.base = layerPaths.front();
    for (size_t i = 1; i < layerPaths.size(); ++i) c.baseLayers.push_back(layerPaths[i]);
    composites.push_back(std::move(c));
    return token("#composite:" + std::to_string(composites.size() - 1));
}

VkTexture* CharacterRenderer::compositeWithRegions(const std::string& basePath, const std::vector<std::string>& baseLayers,
                                                  const std::vector<std::pair<int, std::string>>& regionLayers) {
    Composite c;
    c.base = basePath;
    c.baseLayers = baseLayers;
    c.regions = regionLayers;
    c.withRegions = true;
    composites.push_back(std::move(c));
    return token("#composite:" + std::to_string(composites.size() - 1));
}

// upstream CharacterRenderer::findAttachmentBone (src/rendering/character_renderer.cpp), the same steps over the
// M2 the character was loaded from.
bool CharacterRenderer::findAttachmentBone(uint32_t modelId, uint32_t attachmentId, uint16_t& outBoneIndex,
                                           glm::vec3& outOffset) const {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) return false;
    const auto& model = modelIt->second;

    outBoneIndex = 0;
    outOffset = glm::vec3(0.0f);
    bool found = false;

    if (attachmentId < model.attachmentLookup.size()) {
        uint16_t attIdx = model.attachmentLookup[attachmentId];
        if (attIdx < model.attachments.size()) {
            outBoneIndex = model.attachments[attIdx].bone;
            outOffset = model.attachments[attIdx].position;
            found = true;
        }
    }
    if (!found) {
        for (const auto& att : model.attachments) {
            if (att.id == attachmentId) {
                outBoneIndex = att.bone;
                outOffset = att.position;
                found = true;
                break;
            }
        }
    }
    if (!found && (attachmentId == 1 || attachmentId == 2)) {
        int32_t targetKeyBone = (attachmentId == 1) ? 26 : 27;
        for (size_t i = 0; i < model.bones.size(); i++) {
            if (model.bones[i].keyBoneId == targetKeyBone) {
                outBoneIndex = static_cast<uint16_t>(i);
                outOffset = glm::vec3(0.0f);
                found = true;
                break;
            }
        }
    }
    if (!found && attachmentId == 11 && !model.bones.empty()) {
        outBoneIndex = 0;
        found = true;
    }
    if (found && outBoneIndex >= model.bones.size()) found = false;
    return found;
}

// upstream attachWeapon: no bone for the point -> false; the old model at the
// point goes; the model must load; the texture applies when it is not white.
bool CharacterRenderer::attachWeapon(uint32_t charInstanceId, uint32_t attachmentId, const pipeline::M2Model& weaponModel,
                                     uint32_t weaponModelId, const std::string& texturePath,
                                     const glm::mat4& localTransform) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) return false;
    uint16_t bone = 0;
    glm::vec3 offset(0.0f);
    if (!findAttachmentBone(charIt->second.modelId, attachmentId, bone, offset)) {
        charIt->second.attachFailed.emplace_back(attachmentId, 1u);
        return false;
    }
    detachWeapon(charInstanceId, attachmentId);
    if (!weaponModel.isValid()) {
        charIt->second.attachFailed.emplace_back(attachmentId, 2u);
        return false;
    }
    Attached a;
    // The file the model came from: the .m2 the equipment code read just before (see lastModelRead).
    a.model = assetManager && !assetManager->lastModelRead().empty()
                  ? assetManager->lastModelRead()
                  : pipeline::AssetManager::normalize(weaponModel.name);
    a.texture = texturePath.empty() ? std::string() : pipeline::AssetManager::normalize(texturePath);
    a.textureApplied = !texturePath.empty() && loadTexture(texturePath) != white();
    a.local = localTransform;
    a.modelId = weaponModelId;
    charIt->second.attached[attachmentId] = std::move(a);
    return true;
}

void CharacterRenderer::detachWeapon(uint32_t charInstanceId, uint32_t attachmentId) {
    auto it = instances.find(charInstanceId);
    if (it != instances.end()) it->second.attached.erase(attachmentId);
}

void CharacterRenderer::resetRecord(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    Instance& in = it->second;
    in.geosets.clear();
    in.geosetsSet = false;
    in.slotOverride.clear();
    in.groupOverride.clear();
    in.attached.clear();
    in.attachFailed.clear();
    composites.clear();
}

}  // namespace rendering

// ---------------------------------------------------------------------------
// 3. EntitySpawner members from entity_spawner_processing.cpp

namespace core {

// upstream entity_spawner_processing.cpp, as written there.
bool EntitySpawner::loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel) {
    return pipeline::loadM2WithSkin(*assetManager_, m2Path, outModel);
}

}  // namespace core
}  // namespace wowee

// The spawn queues, transports, mounts and game objects (entity_spawner_processing.cpp) are not
// part of dressing a character and are not compiled here; nothing in the matrix calls them.
namespace {
[[noreturn]] void notInMatrix(const char* what) {
    std::fprintf(stderr, "equip-matrix: upstream %s reached (not compiled into the matrix)\n", what);
    std::abort();
}
}  // namespace

namespace wowee {
namespace core {
#define EQM_TRAP(sig, name) sig { notInMatrix(name); }
EQM_TRAP(void EntitySpawner::processAsyncCreatureResults(bool), "processAsyncCreatureResults")
EQM_TRAP(void EntitySpawner::processAsyncNpcCompositeResults(bool), "processAsyncNpcCompositeResults")
EQM_TRAP(void EntitySpawner::processCreatureSpawnQueue(bool), "processCreatureSpawnQueue")
EQM_TRAP(void EntitySpawner::processPlayerSpawnQueue(), "processPlayerSpawnQueue")
EQM_TRAP(void EntitySpawner::processAsyncEquipmentResults(), "processAsyncEquipmentResults")
EQM_TRAP(void EntitySpawner::processDeferredEquipmentQueue(), "processDeferredEquipmentQueue")
EQM_TRAP(void EntitySpawner::processAsyncGameObjectResults(), "processAsyncGameObjectResults")
EQM_TRAP(void EntitySpawner::processPendingWmoUploads(), "processPendingWmoUploads")
EQM_TRAP(void EntitySpawner::processGameObjectSpawnQueue(), "processGameObjectSpawnQueue")
EQM_TRAP(void EntitySpawner::processPendingTransportRegistrations(), "processPendingTransportRegistrations")
EQM_TRAP(void EntitySpawner::processPendingTransportDoodads(), "processPendingTransportDoodads")
EQM_TRAP(void EntitySpawner::processPendingMount(), "processPendingMount")
EQM_TRAP(void EntitySpawner::processPendingRemotePlayerMounts(), "processPendingRemotePlayerMounts")
EQM_TRAP(void EntitySpawner::despawnCreature(uint64_t), "despawnCreature")
EQM_TRAP(void EntitySpawner::despawnGameObject(uint64_t), "despawnGameObject")
EQM_TRAP(void EntitySpawner::applyGameObjectAnimationPolicy(uint64_t, uint32_t, uint32_t), "applyGameObjectAnimationPolicy")
EQM_TRAP(void EntitySpawner::applyGameObjectState(uint64_t, uint8_t), "applyGameObjectState")
EQM_TRAP(void EntitySpawner::onGameObjectInfoReceived(uint32_t), "onGameObjectInfoReceived")
#undef EQM_TRAP
}  // namespace core
}  // namespace wowee
