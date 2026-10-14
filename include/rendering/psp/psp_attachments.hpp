// Helmets, shoulders, weapons and shields drawn on character
// attachment points (upstream CharacterRenderer::attachWeapon + the weapon
// matrix update in CharacterRenderer::update:
//   weaponMat = charModelMat * boneMatrix * T(attachment offset) * localTransform).
//
// - Which item model goes where: equipmentAttachments() (psp_equipment.hpp,
//   upstream EntitySpawner / AppearanceComposer rules, platform-free so the
//   PC tool psp_equip_preview runs it too).
// - Where the attachment point is: "p_<race>_<sex>" (AttachPoint, baked by
//   psp_char_conv with upstream findAttachmentBone); the bone matrix is the
//   pose CharacterModel already evaluated for the GE this frame (model space,
//   before the vertex dequantisation - psp_skeleton.hpp evaluatePose), or the
//   baked Stand frame 0 matrix for the static model.
// - Item models ("a_<n>" WMDL) and their skins ("t_<n>" WTEX) come from
//   items.pak, are loaded when a character is (re)dressed - never in the draw
//   - and shared by every character through one cache: reference counted, the
//   unreferenced ones kept until the budget (wowee.cfg attachmax, KB) needs
//   the room, least recently used first. PSP: upstream loads a fresh model per
//   attach (a new model id each time); the PSP cannot afford duplicates.
// - Items are static: the item M2's Stand frame 0, no item animation, no
//   particles/ribbons of the item model itself. weapons: the glow of an item visual
//   (enchantment, the display's own; upstream attachWeaponEffect) is drawn by psp_weapon_fx.hpp from the weapon's
//   world matrix kept here (glows()).
// wowee.cfg: attachments=0 draws none (A/B), attachmax=<KB> cache budget.
// Profiler: zone "char.attach" (draw), "char.attach.load"; GE pass "char.attach".
// Log: attach_load display= slot= model= kb= ms=, attach_missing display= slot=,
// char_attach n= points= kb=.
#pragma once

#include "rendering/psp/psp_equipment.hpp"
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_skeleton.hpp"

#include <cstdint>
#include <vector>

namespace wowee::platform::psp {
struct Texture;  // item displays: psp_texture.hpp
}

namespace wowee::rendering::psp {

// wowee.cfg "attachments" (default 1); can be switched at run time.
bool attachmentsEnabled();
void setAttachmentsEnabled(bool on);
// Bytes of item models and textures in the shared cache.
uint32_t attachmentCacheBytes();

// Item displays: an item texture ("t_<n>" - a cloak's) from the
// same shared cache, reference counted like the models' skins: one copy for
// every character wearing it (was one per character, ~22 KB each - a crowd in
// cloaks). nullptr when the pak has none; give it back with releaseItemTexture.
const platform::psp::Texture* acquireItemTexture(ItemBank& items, uint16_t id);
void releaseItemTexture(const platform::psp::Texture* tex);
// Frees every cached model and texture nobody holds (the select screen's
// character is gone: the world starts with an empty cache).
void attachmentCacheTrim();

// ---- weapons: weapon glows (psp_weapon_fx.hpp) ----
// The world screen advances the frame number before the frame's characters are drawn; a weapon with an item
// visual drawn with the camera's position remembers its world matrix and that frame (glows() hands them over).
uint32_t attachmentFrame();
void attachmentNextFrame();

// One weapon with a glow drawn this frame.
struct WeaponGlow {
    float world[12];          // the weapon model's matrix (BoneMatrix layout: 3x4 column-major), server coordinates
    ItemVisualPoints points;  // its attachments 0..4: where the visual's effects sit (found = 0: all at the origin)
    uint16_t itemVisual = 0;  // ItemVisuals.dbc id
    uint8_t slot = 0;         // equipment slot (15 main hand, 16 off hand)
};
// ---- weapons (end) ----

class CharacterAttachments {
public:
    CharacterAttachments() = default;
    ~CharacterAttachments() { clear(); }
    CharacterAttachments(const CharacterAttachments&) = delete;
    CharacterAttachments& operator=(const CharacterAttachments&) = delete;

    // Releases the cache references of what is attached now.
    void clear();
    // Attaches `picks` to the points of `points` (unknown points are dropped,
    // like upstream attachWeapon when findAttachmentBone fails); loads what
    // the cache lacks from `items`.
    // item displays: essential = false (other players, NPCs):
    // a model the cache lacks is not loaded while the models and skins in use
    // pass wowee.cfg attachcap (KB, default 1536) - log attach_cap.
    void set(const std::vector<AttachPick>& picks, const std::vector<AttachPoint>& points, ItemBank& items,
             bool essential = true);
    size_t count() const { return entries_.size(); }
    // memory budget: attachcap's crowd queue - the last set()'s picks again under an
    // AttachDeferScope: every item model released, all counted as left off (attachCapSkipped), so the queue gives
    // them back when there is room (an NPC giving way to a nearer player).
    void giveWay(const std::vector<AttachPoint>& points, ItemBank& items);

    // Draws every attached model. charModel: the character's model matrix
    // relative to the camera (BoneMatrix layout, no dequantisation);
    // pose/poseCount: this frame's bone matrices of the skinned model (null
    // for the static one: the points' Stand matrices are used). The caller's
    // GE state: depth test on, depth writes on, fog set, blending off.
    void draw(const BoneMatrix& charModel, const BoneMatrix* pose, uint32_t poseCount, float alpha);
    // weapons: draw() in the world - camPos (server coordinates) turns the camera-relative
    // matrices into world ones, which the weapons with a glow keep for glows().
    void draw(const BoneMatrix& charModel, const BoneMatrix* pose, uint32_t poseCount, float alpha, const float camPos[3]);
    // The weapons with a glow drawn in this attachmentFrame(); returns how many were written (<= max).
    size_t glows(WeaponGlow* out, size_t max) const;

private:
    struct Entry {
        AttachPick pick;
        AttachPoint point;
        const void* model = nullptr;   // cached WMDL blob
        const void* skin = nullptr;    // cached platform::psp::Texture, or null
        std::vector<const void*> batchTextures;  // per LOD 0 batch: Texture or null (white)
        std::vector<const void*> held;           // cache references this entry holds (textures)
        // weapons: a weapon with a glow (pick.itemVisual) - its visual points (the model's "WIVP"
        // footer) and its last world matrix.
        ItemVisualPoints points{};
        float world[12] = {};
        uint32_t drawnFrame = ~0u;
    };
    static void release(std::vector<Entry>& entries);
    void drawPass(const Entry& e, bool blendPass, bool fade, uint32_t fixSrc, uint32_t fixDst);
    std::vector<Entry> entries_;
    std::vector<AttachPick> lastPicks_;  // memory budget: what the last set() was given (giveWay)
};

// Other players' equipment: attachcap's crowd policy. A character that is not
// essential (another player, an NPC) gets an item model the cache lacks only while the models and skins in use stay
// under attachcap (a model already in the cache is shared: always). Past it the nearest players come first:
// WorldScreen::syncRemotePlayers strips the farthest holder (an AttachDeferScope around its re-attach) and attaches
// the nearer one the cap left without (attachCapSkipped). While an AttachDeferScope lives, a non-essential set()
// attaches nothing and releases what it held: a new player's models are attached in a later frame (nearest first),
// the farthest gives way. PSP only (upstream loads every model of everyone).
class AttachDeferScope {
public:
    AttachDeferScope();
    ~AttachDeferScope();
    AttachDeferScope(const AttachDeferScope&) = delete;
    AttachDeferScope& operator=(const AttachDeferScope&) = delete;
};
// Picks with a model that a's last set() left off (the cap or a defer scope); 0 = all of them attached.
uint32_t attachCapSkipped(const CharacterAttachments& a);
// Item model / skin bytes some character holds now, and wowee.cfg attachcap in bytes.
uint32_t attachmentInUseBytes();
uint32_t attachmentCapBytes();
// Memory budget: the memory governor's cap (bytes; 0 = none) - while
// it is below attachcap, attachmentCapBytes() and the cap rule use it; the crowd queue sheds the farthest holders
// down to it (WorldScreen::remoteCrowdTurn). attachmentCacheTrim() frees what nobody holds.
void setAttachmentGovernorCap(uint32_t bytes);
uint32_t attachmentGovernorCap();

} // namespace wowee::rendering::psp
