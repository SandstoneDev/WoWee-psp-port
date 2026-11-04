// The upstream reference (upstream_ref.hpp) - dresses
// characters through upstream's own functions (EntitySpawner,
// AppearanceComposer, the GameScreen rules in upstream_local.cpp) over the
// seam (seam/, seam_impl.cpp) and reads back what they asked the renderer for.
#include "upstream_ref.hpp"
#include "upstream_local.hpp"

#include "core/appearance_composer.hpp"
#include "core/character_paths.hpp"
#include "core/entity_spawner.hpp"
#include "core/geoset_rules.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <limits>

using namespace wowee;

namespace eqm {

namespace {
uint32_t spawnKey(uint8_t race, uint8_t sex, uint8_t hair, uint8_t facial) {
    return (uint32_t{race} << 24) | (uint32_t{sex} << 16) | (uint32_t{hair} << 8) | facial;
}

// EntitySpawner::spawnOnlinePlayer and setOnlinePlayerEquipment are private: upstream reaches them through
// its spawn queues (entity_spawner_processing.cpp, async loads), which hand over the same arguments. The
// matrix calls them directly - a member pointer named in an explicit instantiation is not access checked.
template <typename Tag>
struct Member {
    static typename Tag::type ptr;
};
template <typename Tag>
typename Tag::type Member<Tag>::ptr = nullptr;
template <typename Tag, typename Tag::type P>
struct Grab {
    struct Init {
        Init() { Member<Tag>::ptr = P; }
    };
    static Init init;
};
template <typename Tag, typename Tag::type P>
typename Grab<Tag, P>::Init Grab<Tag, P>::init;

struct SpawnOnlinePlayer {
    using type = void (core::EntitySpawner::*)(uint64_t, uint8_t, uint8_t, uint32_t, uint8_t, float, float, float,
                                               float);
};
struct SetOnlinePlayerEquipment {
    using type = void (core::EntitySpawner::*)(uint64_t, const std::array<uint32_t, 19>&,
                                               const std::array<uint8_t, 19>&);
};
}  // namespace

template struct Grab<SpawnOnlinePlayer, &core::EntitySpawner::spawnOnlinePlayer>;
template struct Grab<SetOnlinePlayerEquipment, &core::EntitySpawner::setOnlinePlayerEquipment>;

// NPCs: spawnOnlineCreature spawns from the model cache the async load fills (entity_spawner_processing.cpp,
// not compiled here) - the matrix loads the model the way that load does and puts it in the cache.
namespace {
struct SpawnOnlineCreature {
    using type = void (core::EntitySpawner::*)(uint64_t, uint32_t, float, float, float, float, float);
};
struct DisplayModelCache {
    using type = std::unordered_map<uint32_t, uint32_t> core::EntitySpawner::*;
};
}  // namespace
template struct Grab<SpawnOnlineCreature, &core::EntitySpawner::spawnOnlineCreature>;
template struct Grab<DisplayModelCache, &core::EntitySpawner::displayIdModelCache_>;

struct Upstream::Impl {
    pipeline::AssetManager assets;
    pipeline::DBCLayout layout;
    rendering::CharacterRenderer cr;
    rendering::AnimationController ac;
    rendering::Renderer renderer;
    game::GameHandler gh;
    game::GameServices services;
    std::unique_ptr<core::EntitySpawner> spawner;
    std::unique_ptr<core::AppearanceComposer> composer;
    ui::MatrixApp app;
    struct Spawned {
        uint64_t guid = 0;
        uint32_t instance = 0, modelId = 0;
        uint32_t localInstance = 0, localModelId = 0;
        uint32_t bytes = 0;
    };
    std::map<uint32_t, Spawned> spawned;
    uint64_t nextGuid = 0x0000700000000001ull;
    uint32_t nextLocalModel = 0x30000000u;
    uint64_t nextNpcGuid = 0x0000F13000000001ull;
    uint32_t nextNpcModel = 0x50000000u;

    // Another player as upstream spawns one (EntitySpawner::spawnOnlinePlayer): the race model, CharSections
    // skin, hair and the bare geosets. One per race / sex / hair / facial, reused.
    Spawned* spawn(uint8_t race, uint8_t sex, uint8_t hair, uint8_t facial) {
        const uint32_t key = spawnKey(race, sex, hair, facial);
        auto it = spawned.find(key);
        if (it != spawned.end()) return it->second.instance ? &it->second : nullptr;
        Spawned s;
        s.guid = nextGuid++;
        s.bytes = uint32_t{hair} << 16;  // skin 0, face 0, hair colour 0 (core::unpackAppearanceBytes)
        ((*spawner).*Member<SpawnOnlinePlayer>::ptr)(s.guid, race, sex, s.bytes, facial, 0.0f, 0.0f, 0.0f, 0.0f);
        s.instance = spawner->getPlayerInstanceId(s.guid);
        s.modelId = s.instance ? cr.getInstanceModelId(s.instance) : 0;
        it = spawned.emplace(key, s).first;
        return s.instance ? &it->second : nullptr;
    }

    // The own character: upstream's Application builds it from the race model through AppearanceComposer
    // (resolvePlayerTextures before the model loads, compositePlayerSkin after).
    bool localReady(Spawned& s, uint8_t race, uint8_t sex) {
        if (s.localInstance) return true;
        const pipeline::M2Model* base = cr.getModelData(s.modelId);
        if (!base) return false;
        pipeline::M2Model model = *base;
        const auto tex = composer->resolvePlayerTextures(model, static_cast<game::Race>(race),
                                                         sex ? game::Gender::FEMALE : game::Gender::MALE, s.bytes);
        s.localModelId = nextLocalModel++;
        if (!cr.loadModel(model, s.localModelId)) return false;
        s.localInstance = cr.createInstance(s.localModelId, glm::vec3(0.0f));
        if (!s.localInstance) return false;
        renderer.characterInstanceId = s.localInstance;
        composer->compositePlayerSkin(s.localModelId, tex);
        return true;
    }

    // What the renderer was asked for on `inst` since its record was reset. `capeSlot` >= 0: the cape is that
    // texture slot's override (own player), else the geoset group 15 override (other players).
    void collect(uint32_t inst, UpResult& out, int capeSlot) {
        out = UpResult{};
        auto it = cr.instances.find(inst);
        if (it == cr.instances.end()) return;
        const auto& in = it->second;
        out.ok = true;
        out.geosets.insert(in.geosets.begin(), in.geosets.end());
        for (const auto& c : cr.composites)
            if (c.withRegions) {
                out.regions.clear();
                for (const auto& r : c.regions)
                    out.regions.emplace_back(r.first, pipeline::AssetManager::normalize(r.second));
                out.composited = true;
            }
        if (capeSlot >= 0) {
            if (auto s = in.slotOverride.find(static_cast<uint16_t>(capeSlot)); s != in.slotOverride.end())
                out.capeTexture = cr.textureName(s->second);
        } else if (auto g = in.groupOverride.find(15); g != in.groupOverride.end()) {
            out.capeTexture = cr.textureName(g->second);
        }
        for (const auto& [point, a] : in.attached) {
            UpAttach u;
            u.model = a.model;
            u.texture = a.texture;
            u.textureApplied = a.textureApplied;
            bool identity = true;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) {
                    u.local[c * 4 + r] = a.local[c][r];
                    if (std::abs(a.local[c][r] - (c == r ? 1.0f : 0.0f)) > 1e-6f) identity = false;
                }
            u.hasLocal = !identity;
            out.attach[point] = u;
        }
        out.attachFailed = in.attachFailed;
    }
};

Upstream::Upstream() : impl_(std::make_unique<Impl>()) {}
Upstream::~Upstream() = default;

bool Upstream::init(const UpOptions& o, std::string& why) {
    Impl& m = *impl_;
    if (!m.assets.initialize(o.roots)) {
        why = "no data roots";
        return false;
    }
    // upstream Application: the expansion's DBC layout (the 3.3.5a profile).
    if (o.dbcLayoutJson.empty() || !m.layout.loadFromJson(o.dbcLayoutJson)) {
        why = "cannot read " + o.dbcLayoutJson;
        return false;
    }
    pipeline::setActiveDBCLayout(&m.layout);
    if (!m.assets.loadDBC("ItemDisplayInfo.dbc") || !m.assets.loadDBC("CharSections.dbc")) {
        why = "no ItemDisplayInfo.dbc / CharSections.dbc under the data roots";
        return false;
    }
    m.cr.setAssetManager(&m.assets);
    m.renderer.characterRenderer = &m.cr;
    m.renderer.animationController = &m.ac;
    m.services.renderer = &m.renderer;
    m.services.assetManager = &m.assets;
    m.spawner = std::make_unique<core::EntitySpawner>(&m.renderer, &m.assets, &m.gh, &m.services);
    m.spawner->initialize();
    m.composer = std::make_unique<core::AppearanceComposer>(&m.renderer, &m.assets, &m.gh, m.spawner.get());
    m.app.renderer = &m.renderer;
    m.app.assets = &m.assets;
    m.app.gameHandler = &m.gh;
    m.app.composer = m.composer.get();
    return true;
}

bool Upstream::online(uint8_t race, uint8_t sex, uint8_t hairStyle, uint8_t facial, const uint32_t display[19],
                      const uint8_t inv[19], UpResult& out) {
    Impl& m = *impl_;
    out = UpResult{};
    Impl::Spawned* s = m.spawn(race, sex, hairStyle, facial);
    if (!s) return false;
    std::array<uint32_t, 19> d{};
    std::array<uint8_t, 19> t{};
    for (int i = 0; i < 19; ++i) {
        d[i] = display[i];
        t[i] = inv[i];
    }
    m.cr.resetRecord(s->instance);
    ((*m.spawner).*Member<SetOnlinePlayerEquipment>::ptr)(s->guid, d, t);
    m.collect(s->instance, out, -1);
    return out.ok;
}

bool Upstream::local(uint8_t race, uint8_t sex, uint8_t hairStyle, uint8_t facial, const uint32_t display[19],
                     const uint8_t inv[19], bool sheathed, bool helmShown, bool cloakShown, UpResult& out) {
    Impl& m = *impl_;
    out = UpResult{};
    Impl::Spawned* s = m.spawn(race, sex, hairStyle, facial);
    if (!s || !m.localReady(*s, race, sex)) return false;
    m.renderer.characterInstanceId = s->localInstance;
    m.gh.hasActiveCharacter = true;
    m.gh.activeCharacter.race = static_cast<game::Race>(race);
    m.gh.activeCharacter.gender = sex ? game::Gender::FEMALE : game::Gender::MALE;
    m.gh.activeCharacter.useFemaleModel = false;
    m.gh.activeCharacter.appearanceBytes = s->bytes;
    m.gh.activeCharacter.facialFeatures = facial;
    m.gh.helmVisible = helmShown;
    m.gh.cloakVisible = cloakShown;
    m.gh.inventory = game::Inventory();
    for (int i = 0; i < 19; ++i) {
        if (!display[i]) continue;
        game::ItemDef item;
        item.itemId = 900000u + static_cast<uint32_t>(i);  // any non-zero id: the slot is worn
        item.displayInfoId = display[i];
        item.inventoryType = inv[i];
        m.gh.inventory.setEquipSlot(static_cast<game::EquipSlot>(i), item);
    }
    m.composer->setWeaponsSheathed(sheathed);
    m.cr.resetRecord(s->localInstance);
    m.composer->loadEquippedWeapons();
    ui::matrix_updateCharacterGeosets(m.app, m.composer.get(), m.gh.inventory);
    ui::matrix_updateCharacterTextures(m.app, m.composer.get(), m.gh.inventory);
    const uint32_t cloakSlot = m.composer->getCloakTextureSlotIndex();
    m.collect(s->localInstance, out, static_cast<int>(cloakSlot));
    return out.ok;
}

std::set<uint16_t> Upstream::modelGeosets(uint8_t race, uint8_t sex) {
    std::set<uint16_t> out;
    Impl::Spawned* s = impl_->spawn(race, sex, 1, 0);
    if (!s) return out;
    if (const auto* md = impl_->cr.getModelData(s->modelId))
        for (const auto& b : md->batches) out.insert(b.submeshId);
    return out;
}

bool Upstream::hasAttachment(uint8_t race, uint8_t sex, uint32_t point) {
    Impl::Spawned* s = impl_->spawn(race, sex, 1, 0);
    uint16_t bone = 0;
    glm::vec3 off(0.0f);
    return s && impl_->cr.findAttachmentBone(s->modelId, point, bone, off);
}

uint16_t Upstream::hairScalp(uint8_t race, uint8_t sex, uint8_t hairStyle) {
    // entity_spawner_player.cpp selectHairScalpGeoset (anonymous there): the map's value when above 0, else 1.
    const auto& map = impl_->spawner->getHairGeosetMap();
    auto it = map.find(core::appearanceKey(race, sex, hairStyle));
    return it != map.end() && it->second > 0 ? it->second : 1;
}

std::array<uint16_t, 3> Upstream::facialGeosets(uint8_t race, uint8_t sex, uint8_t facial, bool& found) {
    const auto& map = impl_->spawner->getFacialHairGeosetMap();
    auto it = map.find(core::appearanceKey(race, sex, facial));
    found = it != map.end();
    if (!found) return {1, 1, 1};  // bareGeosetsFor: no row = the first variant of each group
    return {it->second.geoset100, it->second.geoset200, it->second.geoset300};
}

bool Upstream::fileExists(const std::string& path) { return impl_->assets.fileExists(path); }

std::vector<uint8_t> Upstream::readFile(const std::string& path) { return impl_->assets.readFile(path); }

bool Upstream::loadBlp(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h) {
    pipeline::BLPImage img = impl_->assets.loadTexture(path);
    if (!img.isValid() || img.data.empty()) return false;
    w = img.width;
    h = img.height;
    rgba = std::move(img.data);
    return rgba.size() >= static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
}

namespace {
// upstream CharacterRenderer::getBoneTransform (src/rendering/character_renderer.cpp, a member there):
// T(pivot) * T(trans) * R(rot) * S(scale) * T(-pivot) at one time of one sequence.
glm::mat4 boneTransform(const pipeline::M2Bone& bone, float animTime, float globalSeqTime, int sequenceIndex,
                        const std::vector<uint32_t>& globalSeqDurations) {
    namespace tr = rendering::m2_track;
    glm::vec3 translation =
        tr::sampleVec3(bone.translation, sequenceIndex, animTime, globalSeqTime, globalSeqDurations, glm::vec3(0.0f));
    glm::quat rotation = tr::sampleQuat(bone.rotation, sequenceIndex, animTime, globalSeqTime, globalSeqDurations);
    glm::vec3 scale =
        tr::sampleVec3(bone.scale, sequenceIndex, animTime, globalSeqTime, globalSeqDurations, glm::vec3(1.0f));
    glm::mat3 R = glm::mat3_cast(rotation);
    glm::vec3 c0 = R[0] * scale.x;
    glm::vec3 c1 = R[1] * scale.y;
    glm::vec3 c2 = R[2] * scale.z;
    glm::vec3 t = (bone.pivot + translation) - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z);
    glm::mat4 transform;
    transform[0] = glm::vec4(c0, 0.0f);
    transform[1] = glm::vec4(c1, 0.0f);
    transform[2] = glm::vec4(c2, 0.0f);
    transform[3] = glm::vec4(t, 1.0f);
    return transform;
}
}  // namespace

// The item model as the renderer shows it at the start of its Stand animation (an attached model plays
// Stand from time 0): upstream's loader, external Stand keyframes (loadExternalAnimations), the bone
// transforms, vertex skinning over the four weights. Bounds over the vertices of the batches the PSP bake
// keeps (skin 00 level 0, not geoset groups 15/17/18 - CharacterRenderer leaves those undrawn without a
// geoset filter).
Upstream::M2Info Upstream::m2Info(const std::string& path) {
    M2Info info;
    pipeline::M2Model m;
    if (!pipeline::loadM2WithSkin(impl_->assets, path, m)) return info;
    info.ok = true;
    info.vertices = static_cast<uint32_t>(m.vertices.size());
    int seq = -1;
    for (size_t i = 0; i < m.sequences.size(); ++i)
        if (m.sequences[i].id == 0 && m.sequences[i].variationIndex == 0) {
            seq = static_cast<int>(i);
            break;
        }
    if (seq >= 0 && !m.bones.empty()) {
        const std::vector<uint8_t> bytes = impl_->assets.readFile(path);
        pipeline::loadExternalAnimations(impl_->assets, path, bytes, m, {0});
        std::vector<glm::mat4> bones(m.bones.size());
        for (size_t i = 0; i < m.bones.size(); ++i) {
            const auto& b = m.bones[i];
            const glm::mat4 local = boneTransform(b, 0.0f, 0.0f, seq, m.globalSequenceDurations);
            bones[i] = (b.parentBone >= 0 && static_cast<size_t>(b.parentBone) < i) ? bones[b.parentBone] * local : local;
        }
        for (auto& v : m.vertices) {
            glm::vec3 p(0.0f);
            float wsum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float w = v.boneWeights[k] / 255.0f;
                if (w <= 0.0f || v.boneIndices[k] >= bones.size()) continue;
                p += w * glm::vec3(bones[v.boneIndices[k]] * glm::vec4(v.position, 1.0f));
                wsum += w;
            }
            if (wsum > 0.0f) v.position = p / wsum;
        }
        info.posed = true;
    }
    for (int k = 0; k < 3; ++k) {
        info.bmin[k] = std::numeric_limits<float>::max();
        info.bmax[k] = -std::numeric_limits<float>::max();
    }
    for (const auto& b : m.batches) {
        const uint16_t grp = b.submeshId / 100;
        if (b.submeshLevel != 0 || b.indexCount < 3 || grp == 15 || grp == 17 || grp == 18) continue;
        if (b.indexStart + b.indexCount > m.indices.size()) continue;
        ++info.batches;
        info.triangles += b.indexCount / 3;
        for (uint32_t i = b.indexStart; i < b.indexStart + (b.indexCount / 3) * 3; ++i) {
            const uint32_t vi = m.indices[i];
            if (vi >= m.vertices.size()) continue;
            for (int k = 0; k < 3; ++k) {
                info.bmin[k] = std::min(info.bmin[k], m.vertices[vi].position[k]);
                info.bmax[k] = std::max(info.bmax[k], m.vertices[vi].position[k]);
            }
        }
    }
    if (!info.batches) info.ok = false;
    return info;
}

bool Upstream::npc(uint32_t displayId, uint32_t virtualItem, UpResult& out, std::string& modelPath) {
    Impl& m = *impl_;
    out = UpResult{};
    const std::string path = m.spawner->getModelPathForDisplayId(displayId);
    modelPath = pipeline::AssetManager::normalize(path);
    if (path.empty()) return false;
    // entity_spawner_processing.cpp processCreatureSpawnQueue, the async load: the M2, its name, the .skin.
    const std::vector<uint8_t> m2Data = m.assets.readFile(path);
    if (m2Data.empty()) return false;
    pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
    if (model.name.empty()) model.name = path;
    if (model.vertices.empty()) return false;
    if (model.version >= 264) {
        const std::vector<uint8_t> skinData = m.assets.readFile(pipeline::skinPathForM2(path));
        if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, model);
    }
    const uint32_t modelId = m.nextNpcModel++;
    if (!m.cr.loadModel(model, modelId)) return false;
    ((*m.spawner).*Member<DisplayModelCache>::ptr)[displayId] = modelId;
    // The unit the server describes: its display, and the weapon in UNIT_VIRTUAL_ITEM_SLOT_ID (field 56, the
    // first candidate tryAttachCreatureVirtualWeapons reads).
    const uint64_t guid = m.nextNpcGuid++;
    auto unit = std::make_shared<game::Unit>(guid);
    unit->setDisplayId(displayId);
    if (virtualItem) unit->setField(56, virtualItem);
    m.gh.entityManager.addEntity(guid, unit);
    ((*m.spawner).*Member<SpawnOnlineCreature>::ptr)(guid, displayId, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const uint32_t inst = m.spawner->getCreatureInstanceId(guid);
    m.gh.entityManager.removeEntity(guid);
    if (!inst) return false;
    m.collect(inst, out, -1);
    const auto& in = m.cr.instances[inst];
    out.geosetsSet = in.geosetsSet;
    const pipeline::M2Model& md = m.cr.models[modelId];
    for (const auto& b : md.batches) out.modelGeosets.insert(b.submeshId);
    for (size_t ti = 0; ti < md.textures.size(); ++ti) {
        if (md.textures[ti].type != 1) continue;
        if (auto s = in.slotOverride.find(static_cast<uint16_t>(ti)); s != in.slotOverride.end()) {
            out.skinTexture = m.cr.textureName(s->second);
        } else if (auto mt = m.cr.modelTextures.find(modelId); mt != m.cr.modelTextures.end()) {
            if (auto t = mt->second.find(static_cast<uint32_t>(ti)); t != mt->second.end())
                out.skinTexture = m.cr.textureName(t->second);
        }
        break;
    }
    return true;
}


}  // namespace eqm
