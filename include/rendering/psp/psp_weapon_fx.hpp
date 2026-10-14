// Weapons: the glows on weapons - item visuals of enchantments and of the displays
// themselves.
//
// The game and upstream (AppearanceComposer::applyEnchantVisuals,
// pipeline::resolveEnchantItemVisuals, CharacterRenderer::attachWeaponEffect): SpellItemEnchantment.ItemVisual ->
// ItemVisuals.dbc, 5 effect slots -> ItemVisualEffects.dbc models; the effect of slot i is placed at the weapon
// model's attachment i (the weapon's matrix x T(offset)), plays its sequence 0 in a loop and follows the weapon.
// Which visual a weapon shows: psp_equipment.hpp equipmentAttachmentsBySheath (AttachPick::itemVisual).
// PSP:
//  - the data is items.pak's ("ivis", "ifx_<n>" WSFX, "ift_<n>" WTEX - weapon_fx_bake.hpp), read through the
//    world's ItemBank (its PakIndex keeps the file open: no descriptor of its own);
//  - the effects run on the shared particle engine (psp_particles.hpp): pool owners kWeaponOwnerBase.., share
//    wowee.cfg enchantsprites; upstream draws only the meshes of such a model (most enchantment glows are particles
//    only - nothing shows upstream), the PSP draws its particles, ribbons and meshes (look class kPtcItem);
//  - budgets: at most `maxInstances` effects at once (the own weapons first, then the nearest characters), within
//    `distance` yd, effects + textures in a cache of `cacheBytes` (least recently used out), one effect read a
//    frame; the sprites at most `maxPx` px.
// wowee.cfg: enchantfx (1; 0 = off, nothing allocated), enchantsprites (96), enchantfxinst (16), enchantfxdist (30),
// enchantfxkb (64), enchantfxpx (32).
// Log: weaponfx on ..., weaponfx_load ..., every 10 s while glows run: weaponfx_frame ...
#pragma once

#include "rendering/psp/psp_attachments.hpp"
#include "rendering/psp/psp_equipment.hpp"
#include "rendering/psp/psp_particles.hpp"
#include "rendering/psp/psp_terrain.hpp"  // TerrainCamera

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::platform::psp {
struct Texture;
}

namespace wowee::rendering::psp {

struct WeaponFxConfig {
    int maxInstances = 16;              // enchantfxinst: effects at once (a visual has up to 5)
    float distance = 30.0f;             // enchantfxdist: yd from the camera to the character
    uint32_t cacheBytes = 64u * 1024u;  // enchantfxkb
    int maxPx = 32;                     // enchantfxpx
};

class WeaponGlows {
public:
    WeaponGlows();  // in the .cpp: Loaded / Tex are complete there
    ~WeaponGlows();
    WeaponGlows(const WeaponGlows&) = delete;
    WeaponGlows& operator=(const WeaponGlows&) = delete;

    // items.pak's "ivis" (through the world's ItemBank, which must outlive this); false: none (an older pak) or no
    // memory for the particle pool - then nothing is kept.
    bool open(const WeaponFxConfig& cfg, ItemBank& items);
    void close();
    bool isOpen() const { return !table_.empty(); }

    // PLAYER_VISIBLE_ITEM_n_ENCHANTMENT -> the visual the client puts on the weapon (0 = none).
    uint16_t enchantVisual(uint32_t enchantField) const;

    // A glowing weapon of this frame (CharacterModel::weaponGlows) and whose it is.
    struct Request {
        uint64_t owner = 0;  // the character (its guid; the own player's too)
        WeaponGlow glow;
        float d2 = 0.0f;     // squared distance camera - character
        bool own = false;
    };
    // Per frame, after the characters were drawn: the instances (own first, then the nearest), particles, meshes,
    // ribbons, the GE pass.
    void frame(const std::vector<Request>& requests, const TerrainCamera& cam, float dtSec);

private:
    struct Loaded;
    struct Tex;
    struct Slot;
    struct Want {
        uint64_t key;
        uint16_t effect;
        float pos[3];
        float axes[9];
        float d2;
    };
    Loaded* effect(uint16_t id, bool allowLoad);
    const platform::psp::Texture* texture(uint16_t id);
    static const platform::psp::Texture* texFn(void* user, uint16_t id);
    bool makeRoom(uint32_t bytes);
    void releaseSlot(Slot& s);
    void log(uint32_t nowMs);

    WeaponFxConfig cfg_;
    ItemBank* items_ = nullptr;
    std::vector<uint8_t> table_;  // "ivis" (4-aligned copy)
    bool engine_ = false;
    std::unordered_map<uint16_t, std::unique_ptr<Loaded>> loaded_;
    std::unordered_map<uint16_t, std::unique_ptr<Tex>> textures_;
    std::unordered_set<uint16_t> broken_;
    // An effect the cache had no room for -> the frame it was tried; read again only
    // ~1 s later (was: one Memory Stick read every frame, the frame's one read taken from the other effects).
    std::unordered_map<uint16_t, uint32_t> noRoom_;
    bool readable(uint16_t id) const;
    uint32_t cacheBytes_ = 0;
    std::unique_ptr<Slot[]> slots_;
    std::vector<Want> wants_;          // reserved at open: no allocation in the frame
    std::vector<FxInstance> list_;
    uint32_t frame_ = 0;
    // Stats (the 10 s line).
    uint32_t nextLogMs_ = 0, lastActiveMs_ = 0, loads_ = 0, fails_ = 0, evicts_ = 0, logs_ = 0;
    uint32_t usSum_ = 0, usMax_ = 0, usFrames_ = 0, peakLive_ = 0, lastInstances_ = 0, lastRequests_ = 0;
    FxFrameStats last_;
};

} // namespace wowee::rendering::psp
