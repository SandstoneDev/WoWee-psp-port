#pragma once
// The upstream reference for psp_equip_preview --matrix.
//
// Runs upstream WoWee's own equipment code on the PC - compiled unchanged from
// src/core/entity_spawner.cpp, entity_spawner_player.cpp,
// appearance_composer.cpp, helm_visual.cpp, pipeline/item_textures.cpp,
// m2_asset_loader.cpp, char_sections.cpp, m2_loader.cpp, ... (the seam/
// headers stand in for the Vulkan renderer, the network GameHandler and the
// manifest AssetManager and record what that code asks of them) - and hands
// back what a character wearing the given items shows:
//   - other players: EntitySpawner::spawnOnlinePlayer + setOnlinePlayerEquipment;
//   - the own player: AppearanceComposer::loadEquippedWeapons (helm, weapons,
//     held / sheathed) and GameScreen::updateCharacterGeosets /
//     updateCharacterTextures (their text cut out of src/ui/game_screen_hud.cpp
//     by CMake when the converter is configured - upstream_local.cpp; the UI
//     file they live in cannot be compiled alone);
//   - NPCs: EntitySpawner's humanoid path (geosets, cape, skin, virtual weapon).
// This header has no upstream types in it: psp_equip_preview.cpp (built with
// the PSP headers) includes it.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace eqm {

struct UpAttach {
    std::string model;          // M2 path the upstream code loaded (lower case, '/')
    std::string texture;        // texture path it gave attachWeapon ("" none)
    bool textureApplied = false;  // that texture loads (upstream: not the white texture)
    bool hasLocal = false;      // a local transform other than identity
    float local[16] = {};       // glm::mat4, column major
};

struct UpResult {
    bool ok = false;
    std::set<uint16_t> geosets;                       // the last setActiveGeosets
    std::vector<std::pair<int, std::string>> regions;  // compositeWithRegions regionLayers, in order
    bool composited = false;
    std::string capeTexture;                          // what group 15 / type 2 got ("" none)
    std::map<uint32_t, UpAttach> attach;              // attachment point -> model, at the end
    std::vector<std::pair<uint32_t, uint32_t>> attachFailed;  // (point, 1 = no bone on the character, 2 = model)
    // NPCs: whether a geoset filter was set (without one the renderer draws every batch but groups 15/17/18),
    // the model's submesh ids, and the skin (texture type 1: the instance override, else the model texture).
    bool geosetsSet = false;
    std::set<uint16_t> modelGeosets;
    std::string skinTexture;
};

struct UpOptions {
    std::vector<std::string> roots;  // extracted trees (mirrors first)
    std::string dbcLayoutJson;       // Data/expansions/wotlk/dbc_layouts.json
};

class Upstream {
public:
    Upstream();
    ~Upstream();
    bool init(const UpOptions& o, std::string& why);

    // Another player (EntitySpawner online path). display/inv: 19 visible slots.
    bool online(uint8_t race, uint8_t sex, uint8_t hairStyle, uint8_t facial, const uint32_t display[19],
                const uint8_t inv[19], UpResult& out);
    // The own player: AppearanceComposer weapons + helm (sheathed or held) and
    // the GameScreen geoset / texture rules (upstream_local.cpp).
    bool local(uint8_t race, uint8_t sex, uint8_t hairStyle, uint8_t facial, const uint32_t display[19],
               const uint8_t inv[19], bool sheathed, bool helmShown, bool cloakShown, UpResult& out);
    // An NPC (CreatureDisplayInfo id) as EntitySpawner spawns one: the model
    // loaded the way the async creature load does it, then spawnOnlineCreature
    // (skins, humanoid overrides, normalizeHumanoidClothingGeosets, the
    // virtual weapon). `virtualItem` = UNIT_VIRTUAL_ITEM_SLOT_ID (an Item.dbc
    // entry) or 0.
    bool npc(uint32_t creatureDisplayId, uint32_t virtualItem, UpResult& out, std::string& modelPath);

    // The character model upstream loads for race/sex: its submesh ids, and
    // whether an attachment point resolves to a bone (findAttachmentBone).
    std::set<uint16_t> modelGeosets(uint8_t race, uint8_t sex);
    bool hasAttachment(uint8_t race, uint8_t sex, uint32_t point);
    // Hair scalp / facial geosets upstream picks (CharHairGeosets, CharacterFacialHairStyles).
    uint16_t hairScalp(uint8_t race, uint8_t sex, uint8_t hairStyle);
    std::array<uint16_t, 3> facialGeosets(uint8_t race, uint8_t sex, uint8_t facial, bool& found);

    // Data access for the comparisons (the same tree upstream reads).
    bool fileExists(const std::string& path);
    std::vector<uint8_t> readFile(const std::string& path);
    // RGBA8 of a BLP (empty on failure).
    bool loadBlp(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h);
    // An M2 + skin as upstream loads it, at the start of its Stand animation:
    // triangles and bounds of the skin-0 batches the PSP bake keeps (submesh
    // level 0, not geoset groups 15/17/18).
    struct M2Info {
        bool ok = false;
        bool posed = false;  // Stand frame 0 applied (else bind pose)
        uint32_t vertices = 0, triangles = 0, batches = 0;
        float bmin[3] = {}, bmax[3] = {};
    };
    M2Info m2Info(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eqm
