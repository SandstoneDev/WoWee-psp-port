// PSP character (M7, animated since M9): the race/sex model from
// data/characters.pak (tools/psp/converter psp_char_conv) with the geosets and
// textures of the character's appearance.
//
// - Geosets: upstream core::bareCharacterGeosets (header-only) with the hair
//   scalp and facial-hair variants from the baked WAPP tables.
// - Body texture: composited here on the CPU into 128x128 ABGR8888 from the
//   WLAY layers the tables name (skin, face lower/upper, underwear), in
//   upstream's order; then swizzled for the GE. Hair and fixed textures are
//   WTEX entries.
// - Animation (M9): "n_<race>_<sex>" (WSKN) is drawn with GE hardware
//   skinning: per sub-batch up to 8 bone matrices (sceGuBoneMatrix) and u8
//   vertex weights; the pose is evaluated once per drawn frame on the CPU
//   (psp_skeleton.hpp, profiler zone "char.anim"). The API follows upstream
//   CharacterRenderer (playAnimation / getAnimationState / update: a one-shot
//   returns to Stand when it ends), so upstream's LocomotionFSM drives it
//   unchanged. Without "n_" (older pak, or animated = false) the static
//   Stand frame 0 model "c_" is drawn as before.
// - Equipment (M10c): upstream EntitySpawner::setOnlinePlayerEquipment -
//   geosets via applyEquipmentGeosets (psp_equipment.hpp) over the bare set,
//   and the ItemDisplayInfo region textures ("q_<n>" WLAY from items.pak)
//   composited after skin/face/underwear in upstream order (slot 0..18,
//   region 0..7). setEquipment() re-dresses without reloading the model.
//   A worn cloak's texture goes on geoset group 15 (upstream
//   setGroupTextureOverride(15)).
//   body equipment: geosets, body layers and the cloak by the
//   body rules (psp_body_rules.hpp), Show Helm / Show Cloak
//   (Equipment::hideHelm / hideCloak); a re-dress paints only the body
//   sections that changed (composeBody, log char_equip ... dirty= reads=).
// - Attachments: helmets, shoulders, weapons and shields are item
//   models ("a_<n>" in items.pak) on the model's attachment points
//   ("p_<race>_<sex>"), drawn on this frame's bone matrices after the body
//   (psp_attachments.hpp; upstream attachWeapon + the weapon matrix update).
// Other players/NPCs later: one CharacterModel per unit; the WSKN blob is
// read-only and could be shared per race/sex (per-instance state is the
// sequence, the time and the bone matrices).
#pragma once

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/psp/psp_attachments.hpp"
#include "rendering/psp/psp_equipment.hpp"
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_skeleton.hpp"
#include "rendering/psp/psp_terrain.hpp"  // TerrainCamera

#include <glm/glm.hpp>
#include <psptypes.h>

#include <cstdint>
#include <vector>

namespace wowee::rendering::psp {

struct CharacterLook {
    uint8_t race = 1;
    uint8_t sex = 0;          // 0 male, 1 female (model)
    uint8_t skin = 0, face = 0, hairStyle = 0, hairColor = 0;
    uint8_t facial = 0;
};

class CharacterModel {
public:
    CharacterModel() = default;
    ~CharacterModel();
    CharacterModel(const CharacterModel&) = delete;
    CharacterModel& operator=(const CharacterModel&) = delete;

    // Loads model, tables, textures; composites the body. False when the pak
    // has no model for this race/sex. animated: use "n_" when the pak has it.
    // equipment/items (M10c): what is worn and the baked items.pak; null or
    // an unopened bank = bare (as before).
    bool load(platform::psp::Pak& pak, const CharacterLook& look, bool animated = true,
              const Equipment* equipment = nullptr, ItemBank* items = nullptr);
    // M10c: new equipment on a loaded model - geosets and the body texture
    // are rebuilt (layers read from `pak`, the same characters.pak), the
    // model, skeleton and animation state are kept.
    void setEquipment(platform::psp::Pak& pak, const Equipment& equipment, ItemBank* items);
    const Equipment& equipment() const { return equipment_; }
    void clear();
    bool loaded() const { return static_cast<bool>(h_) || skinned_; }
    bool animated() const { return skinned_; }

    // Feet at `pos` (server coords), facing `yaw` (server orientation).
    // alpha < 1 fades the whole character (camera close to it, like the original client);
    // the opaque pass then blends with a fixed factor and writes no depth.
    void draw(const TerrainCamera& cam, const glm::vec3& pos, float yaw, uint32_t fogColorABGR,
              float alpha = 1.0f);
    // Height of the model in the stand pose (feet to top), 0 when not loaded.
    float height() const {
        if (skinned_) return skin_.h->standHeight;
        return h_ ? h_->center[2] + h_->half[2] : 0.0f;
    }

    // Animation, upstream CharacterRenderer semantics. playAnimation picks
    // variation 0 of the id (else the first sequence of the model) and
    // restarts at time 0; update advances the time, wraps looping sequences
    // and returns a finished one-shot to Stand.
    void playAnimation(uint32_t animId, bool loop);
    bool hasAnimation(uint32_t animId) const { return skinned_ && skin_.findSequence(animId) >= 0; }
    // M9c emotes: sequences outside n_ ("o_<race>_<sex>_<anim>" in this pak,
    // one per animation), loaded when played. The pak must stay open while
    // the model lives (WorldScreen::charPak_).
    void setEmotePak(platform::psp::Pak* pak) { emotePak_ = pak; }
    bool canPlay(uint32_t animId);
    bool getAnimationState(uint32_t& animId, float& timeMs, float& durationMs) const;
    void update(float dtSec);
    // AnimCapabilityProbe's locomotion part over the baked sequences.
    // Upstream AnimCapabilityProbe::probe with "has" = the pak baked it
    // (the probe itself takes the desktop Renderer).
    rendering::AnimCapabilitySet capabilities() const;

    uint32_t triangles() const { return triangles_; }
    uint32_t batchesDrawn() const { return batchesDrawn_; }
    uint32_t memoryKb() const { return memoryBytes_ / 1024u; }
    uint32_t boneUploads() const { return boneUploads_; }

    // Which upstream path places the weapons - the own player
    // (AppearanceComposer, sheathe state) or another player
    // (EntitySpawner::setOnlinePlayerEquipment). Set before load(); kept by
    // clear(). setWeaponsSheathed re-attaches without reloading (upstream
    // loadEquippedWeapons after toggleWeaponsSheathed); upstream starts drawn.
    void setLocalPlayer(bool local) { localPlayer_ = local; }
    void setWeaponsSheathed(bool sheathed);
    bool weaponsSheathed() const { return sheathed_; }
    size_t attachmentCount() const { return attach_.count(); }

    // ---- weapons ----
    // The placement by the unit's sheath state (psp_equipment.hpp equipmentAttachmentsBySheath)
    // (SheathState: 0 put away, 1 melee drawn, 2 ranged drawn) for every player, own or not - with
    // sheathClient (wowee.cfg sheathclient, default 1); 0 = the upstream rules above (A/B).
    // setWeaponsSheathed(b) is setSheathState(b ? 0 : 1).
    void setSheathState(uint8_t state);
    uint8_t sheathState() const { return sheathState_; }
    static bool sheathClient;
    static bool rangedOnBack;  // wowee.cfg rangedback (default 0 = the client: the ranged weapon only for a shot)
    // The weapons with a glow drawn this frame (psp_attachments.hpp WeaponGlow); returns how many (<= max).
    size_t weaponGlows(WeaponGlow* out, size_t max) const { return attach_.glows(out, max); }
    // ---- weapons (end) ----

private:
    const platform::psp::Texture* texture(uint16_t id) const;
    void drawPass(bool blendPass, float alpha);
    void setBatchState(const ModelBatch& b, const platform::psp::Texture* tex, bool fade, uint32_t fixSrc,
                       uint32_t fixDst);
    void evaluatePose();
    bool loadExtra(uint32_t animId);
    const SkinView& animView() const { return extraActive_ ? extra_ : skin_; }
    // Geosets, textures of the shown batches and the body composite for
    // look_ + equipment_ (load and setEquipment).
    void dress(platform::psp::Pak& pak, ItemBank* items);
    // body equipment: the body texture for `dress` (the body rules' sections and layers,
    // psp_body_rules.hpp). Only the sections whose layers changed since the last composite are painted again
    // (the client composites its dirty sections only, too): an item without body texture (weapon, helm, cloak,
    // ring, the helm/cloak switches) reads nothing; one piece of armour reads the skin and its own layers.
    // Returns the sections painted (bit per section; 0xFF: the whole texture).
    uint32_t composeBody(platform::psp::Pak& pak, ItemBank* items, const body::Dress& dress);

    void* modelBlob_ = nullptr;
    const ModelHeader* h_ = nullptr;       // static model ("c_")
    const ModelVertex* verts_ = nullptr;
    const uint16_t* indices_ = nullptr;
    const ModelBatch* batches_ = nullptr;  // static or skinned batches
    uint32_t batchCount_ = 0;
    std::vector<uint8_t> active_;          // per batch: geoset shown
    std::vector<platform::psp::Texture> textures_;  // "x_<n>" by id (pixels == null: absent)
    platform::psp::Texture body_;
    void* bodyPixels_ = nullptr;
    uint16_t hairTexture_ = kNoTexture;
    // Appearance kept for re-dressing (M10c).
    CharacterLook look_;
    Equipment equipment_;
    const uint16_t* geo_ = nullptr;        // per batch: geoset id
    uint16_t bodyLayers_[kBodyLayers];
    uint16_t hairScalp_ = 1, facial_[3] = {1, 1, 1};
    bool texFlush_ = false;
    // M10c: worn cloak's texture on geoset group 15 (items.pak "t_"); item displays: held in the
    // shared item texture cache (psp_attachments.hpp acquireItemTexture), one copy for everyone wearing it.
    const platform::psp::Texture* cape_ = nullptr;
    // body equipment: what the body texture shows now, per section (valid with bodyPixels_), and which base
    // layer is the torso / pelvis underwear (1 / 2, from its WLAY rectangle; 0 = skin, face, other).
    body::Paste bodyPaste_[body::kSections];
    bool bodyPasteValid_ = false;
    uint8_t baseUnderwear_[kBodyLayers] = {};
    uint32_t triangles_ = 0;
    uint32_t batchesDrawn_ = 0;
    uint32_t memoryBytes_ = 0;

    // M9 skinning.
    bool skinned_ = false;
    SkinView skin_;
    int sequence_ = -1;
    uint32_t animId_ = 0;
    float animTimeMs_ = 0.0f;
    bool animLoop_ = true;
    bool poseDirty_ = true;
    // Variations (idle fidgets): repeats left of the current one, and the
    // generator that picks the next by frequency, like the original client.
    uint16_t repeatsLeft_ = 0;
    // M9c: the emote sequence in play (one WSKA blob cached).
    platform::psp::Pak* emotePak_ = nullptr;
    void* extraBlob_ = nullptr;
    SkinView extra_;
    bool extraActive_ = false;
    // Cross-fade from the sequence that was playing (M2 blendTime, like the
    // original client; upstream CharacterRenderer switches instantly).
    int blendFromSeq_ = -1;
    bool blendFromExtra_ = false;
    float blendFromTimeMs_ = 0.0f;
    float blendElapsedMs_ = 0.0f;
    float blendMs_ = 0.0f;
    void startBlend(int newSeq, bool newExtra);
public:
    // Cross-fade length scale, % (wowee.cfg animblend; 0 = instant like upstream).
    static int blendPercent;
private:
    uint32_t rng_ = 0x2545F491u;
    void nextVariation();
    std::vector<BoneMatrix> pose_;
    ScePspFMatrix4* geBones_ = nullptr;    // 16-aligned, one per bone
    int slotBone_[kSkinMaxPalette];        // GE bone slot -> bone, this frame
    uint32_t boneUploads_ = 0;

    // Attachment points ("p_<race>_<sex>") and what hangs on them.
    void attachEquipment(ItemBank* items);
    std::vector<AttachPoint> attachPoints_;
    CharacterAttachments attach_;
    ItemBank* attachItems_ = nullptr;
    bool localPlayer_ = false;
    bool sheathed_ = false;
    uint8_t sheathState_ = 1;  // weapons: SheathState (upstream starts drawn)

public:
    // Sounds (src/rendering/psp/psp_footsteps.cpp). The clock of the
    // sequence in play (getAnimationState) plus its footfall ($FSD) times in
    // ms (psp_model_format.hpp SkinEventTable; events = nullptr, count = 0
    // when the pak has none). False when not animated.
    bool footstepClock(uint32_t& animId, float& timeMs, float& durationMs, const uint32_t*& events,
                       uint32_t& count) const;

    // Spell effects (src/rendering/psp/psp_spellfx.cpp): model-space position (feet at the
    // origin, before the yaw) of M2 attachment `id` on the pose last evaluated - upstream
    // CharacterRenderer::getAttachmentTransform's translation; the static model uses the point's Stand
    // matrix. False without "p_<race>_<sex>" or without that point.
    bool attachmentPosition(uint16_t id, float out[3]) const;

    // other players' equipment: attachcap's crowd policy (psp_attachments.hpp AttachDeferScope) - the item
    // models attached again for what is worn (WorldScreen::syncRemotePlayers: a deferred dress, a nearer player's
    // turn, the farthest giving way), and how many picks with a model the last attach left off.
    void refreshAttachments();
    uint32_t attachmentsCapped() const;
    // dressup: a second view of a loaded model - the DressUpFrame's model (DressUpModel:
    // SetUnit("player")). It shares `base`'s model blob (skeleton, sequences, batches - read-only) and the
    // textures `base` holds (copies without their owning block), so it costs only its own pose, geosets, body
    // composite (64 KB) and the attachments (the shared item cache) - not a second 0.7-1.5 MB "n_" / 0.1-0.25 MB
    // "c_" model. `base` must stay loaded and must not be loaded again while the view lives: its owner clears the
    // view first (WorldScreen::loadCharacter). `pak` = the characters.pak the base came from (the body layers, a
    // texture the base has not loaded). Equipment and attachments as load(); setLocalPlayer /
    // setWeaponsSheathed before it as for load(). clear() leaves the base's blob and textures alone.
    bool loadView(const CharacterModel& base, platform::psp::Pak& pak, const Equipment& equipment, ItemBank* items);
    bool isView() const { return view_; }
    // dressup: draw() in the Stand pose at `standMs` (the caller's clock) instead of the animation in play -
    // the character window's PlayerModel stands idle while the character runs, sits, rides or dances. The
    // animation in play is untouched (its pose is evaluated again right after). Opaque, no fade.
    void drawStand(const TerrainCamera& cam, const glm::vec3& pos, float yaw, uint32_t fogColorABGR, float standMs);

private:
    bool view_ = false;  // dressup: modelBlob_ and the block-less textures_ belong to the base model
};

} // namespace wowee::rendering::psp
