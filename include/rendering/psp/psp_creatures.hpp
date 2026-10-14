// PSP creatures and NPCs (M10b): the displays baked by tools/psp/converter
// psp_creature_conv into data/creatures.pak, drawn at the units the caller
// hands over (live: M10a's SMSG_UPDATE_OBJECT units; debug: our server's DB
// spawns baked per tile, "s_<map>_<x>_<y>", wowee.cfg npcstatic=1).
//
// Format: include/rendering/psp/psp_model_format.hpp (WMDL models like the
// characters, WCDI displays, WCSP spawns). A display is a model + the
// textures of its replaceable slots + a batch mask (geosets), all resolved on
// the PC by upstream EntitySpawner rules; nothing here knows DBCs.
//
// Creature displays: the pak holds every CreatureDisplayInfo row (~24 000 displays) and is
// read on demand - PakIndex (include/platform/psp/psp_pak_index.hpp): the fence of the sorted TOC in RAM
// (a few KB), a lookup = at most one 8 KB TOC block read. A display is looked up the first time a unit near
// the camera shows it, kept in a small pool while used; models, skins and textures are looked up, read and
// evicted as before (LRU within memoryBudgetKb). A recently used entry's place on the card is remembered in a
// small direct-mapped cache, so a re-load after an eviction reads no TOC. An unsorted pak of an older bake
// opens with Pak as before (its TOC in RAM).
//
// - Displays, models and textures load lazily, nearest units first, at most
//   loadsPerFrame reads and tocReadsPerFrame TOC block reads per frame (a unit
//   whose model or textures are not in yet is skipped, never drawn white).
// - Memory: models and textures not drawn for a while are freed, least
//   recently used first, while the total is above memoryBudgetKb (never one
//   used in the last 2 frames: the GE may still read it).
// - Per frame: distance + frustum cull, nearest maxDrawn kept, LOD 1 beyond
//   lodDistance; opaque pass grouped by display and LOD (state once, then a
//   matrix + draw per unit, like ModelRenderer), blended batches far to near.
// - Placement like upstream spawnOnlineCreature: feet at the server position,
//   yaw = server orientation (upstream: canonical + 90 deg, the same angle),
//   scale = unit scale x CreatureDisplayInfo scale x CreatureModelData scale.
// - Creature anim: a model with "n_<model>" (WSKN) is drawn with GE
//   hardware skinning (psp_skinned.hpp) and one animation state per unit,
//   driven like upstream's creature sync loop (Stand / Walk / Run / Swim / Fly
//   by movement, Death, state emotes) plus the combat hooks below; the pose
//   is evaluated only for the units drawn (profiler zone "npc.anim").
// - creature displays: a humanoid NPC's helm and shoulders (CreatureDisplayInfoExtra) and its main-hand
//   weapon (UNIT_VIRTUAL_ITEM_SLOT_ID, resolved by the caller) as item models from items.pak (ItemBank,
//   psp_attachments.hpp - the players' path, EntitySpawner's online rules) on the attachment points "p_<n>"
//   of its model, for the nearest maxEquipped NPCs within equipDistance; the cape (texture baked into the
//   display) on its cape batches.
#pragma once

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_pak_index.hpp"  // creature displays
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_skinned.hpp"  // Creature anim
#include "rendering/psp/psp_terrain.hpp"  // TerrainCamera

#include <pspgu.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace wowee::rendering::psp {

class ItemBank;               // psp_equipment.hpp (creature displays: NPC equipment)
class CharacterAttachments;   // psp_attachments.hpp

// One unit to draw, in server coordinates (what SMSG_UPDATE_OBJECT carries).
struct CreatureUnit {
    uint64_t guid = 0;
    uint32_t displayId = 0;      // UNIT_FIELD_DISPLAYID
    float x = 0, y = 0, z = 0;   // feet
    float orientation = 0;       // radians, 0 = +x (north), counter-clockwise
    float scale = 1.0f;          // OBJECT_FIELD_SCALE_X
    // Creature anim - what upstream's creature sync loop
    // (Application::syncRenderInstancesToEntities) reads per frame.
    bool moving = false;         // Entity::isActivelyMoving (server spline in progress)
    bool walking = false;        // WALKING move flag (SMSG_SPLINE_MOVE_SET_WALK_MODE)
    bool swimming = false;       // SWIMMING (SMSG_SPLINE_MOVE_START_SWIM)
    bool flying = false;         // FLYING (SMSG_SPLINE_MOVE_SET_FLYING)
    bool dead = false;           // isUnitCorpseState(health, maxHealth, dynamic flags)
    uint32_t emoteAnim = 0;      // UNIT_NPC_EMOTESTATE through Emotes.dbc (a looping state), 0 = none
    float speed = 0.0f;          // walk/run - spline ground speed, yd/s (0 = unknown)
    // Mounts. A mount under a player (WorldScreen, ui_world_mount.cpp):
    // kept before other units when maxDrawn cuts the list.
    bool mount = false;
    // The own mount: its animation is the caller's (upstream MountFSM through
    // playDriven / animationState), it is posed every frame (outside npcposes)
    // so the rider sits on this frame's seat.
    bool driven = false;
    // Upstream creates mount instances at scale 1.0: the CreatureDisplayInfo x
    // CreatureModelData scale of the display is not applied (wowee.cfg mountscale=1 applies it).
    bool rawScale = false;
    // creature displays: the main-hand weapon's ItemDisplayInfo id (upstream tryAttachCreatureVirtualWeapons:
    // UNIT_VIRTUAL_ITEM_SLOT_ID -> Item.dbc -> DisplayID), 0 = none / not resolved. Humanoid displays only.
    uint32_t mainHand = 0;
    // other players' equipment: the rest of the virtual items as the original client shows them -
    // UNIT_VIRTUAL_ITEM_SLOT_ID + 1 / + 2 (the off hand: a weapon or a shield; the ranged weapon) through Item.dbc,
    // the InventoryType of each (where the item hangs: Item.dbc, 13 one-hand when unknown) and the sheath state
    // (UNIT_FIELD_BYTES_2 byte 0 = SHEATH_STATE_UNARMED: in the sheaths, as a player's). Upstream: main hand only.
    uint32_t offHand = 0, ranged = 0;
    uint8_t mainHandInv = 13, offHandInv = 13, rangedInv = 15;
    bool sheathed = false;
    uint8_t sheathState = 1;  // batch 5 merge: the whole byte (0 put away, 1 melee, 2 ranged) for the client rule
};

struct CreatureStats {
    uint32_t units = 0;
    uint32_t drawn = 0;
    uint32_t culled = 0;
    uint32_t waiting = 0;        // visible but still loading (or no such display)
    uint32_t lowLod = 0;
    uint32_t batches = 0;
    uint32_t triangles = 0;
    uint32_t loads = 0;          // pak reads this frame
    uint32_t displays = 0, models = 0, textures = 0;  // resident
    uint32_t memoryKb = 0;
    // Creature anim.
    uint32_t skinned = 0;        // drawn with GE skinning this frame
    uint32_t posed = 0;          // of them, pose evaluated this frame (others reuse the last)
    uint32_t boneUploads = 0;    // sceGuBoneMatrix this frame
    uint32_t moving = 0;         // skinned units walking/running this frame
    uint32_t skins = 0;          // WSKN blobs resident
    uint32_t animStates = 0;     // per-unit animation states kept
    uint32_t skipped = 0;        // skinned units not drawn: bone upload / display list budget
    // creature displays.
    uint32_t tocReads = 0;       // TOC block reads this frame (PakIndex lookups that missed its cache)
    uint32_t equipped = 0;       // units drawn with item models this frame
    uint32_t heapHeld = 0;       // Frames whose loads waited for heapReserveKb (since open)
};

class CreatureRenderer {
public:
    CreatureRenderer() = default;
    ~CreatureRenderer();
    CreatureRenderer(const CreatureRenderer&) = delete;
    CreatureRenderer& operator=(const CreatureRenderer&) = delete;

    // Opens the pak (creature displays: a sorted pak through PakIndex, nothing else read; an older pak
    // through Pak, its TOC in RAM).
    bool open(const char* absPath);
    void close();
    bool isOpen() const { return index_.isOpen() || pak_.isOpen(); }
    int pakFd() const { return index_.isOpen() ? index_.fd() : pak_.fd(); }  // shared with the tile streamer
    // memory budget: without the DB spawns (npcstatic=0) only the main thread reads the pak - a
    // rare pak (Pak::setRare): its descriptor goes back after idleMs without a read (releaseIdleFd, every frame) and
    // to an open that found none (pakFdRelieve).
    void setRareFd(bool on) {
        index_.setRare(on);
        pak_.setRare(on);
    }
    // Memory diet (wowee.cfg npcarena): models, skins, textures, displays from the world arena's high
    // end (the NPC cache's LRU churn of 100-900 KB blobs stays out of newlib).
    void setWorldArena(bool on) {
        index_.setWorldArena(on);
        pak_.setWorldArena(on);
    }
    void releaseIdleFd(uint32_t nowMs, uint32_t idleMs) {
        index_.releaseIfIdle(nowMs, idleMs);
        pak_.releaseIfIdle(nowMs, idleMs);
    }
    uint32_t pakFileId() const { return index_.isOpen() ? index_.fileId() : pak_.fileId(); }
    // RAM the open pak's index holds (PakIndex: fence + block cache; Pak: its TOC), for the log.
    uint32_t indexRamBytes() const;

    // The units to draw from now on (copied; call again whenever they move
    // or change - every frame is fine). Units with an unknown display id are
    // counted as waiting and skipped.
    // world_flush after a far teleport: every model, texture and skin of the cache freed (the
    // units' animation states unbound, their item models released) - the units in range load theirs again as at world
    // entry. The displays' records and the pak index stay. Returns the bytes the cache held.
    uint32_t releaseModels();
    // near_starve_flush: only what no unit drew for idleFrames frames. Bytes freed.
    uint32_t releaseUnused(uint32_t idleFrames);
    void setUnits(const CreatureUnit* units, size_t count);
    size_t unitCount() const { return units_.size(); }

    // Loads what the nearest maxDrawn units within viewDistance of t need,
    // with no per-frame budget (world entry, behind a loading screen), so the
    // first frames do not pop units in one by one. Returns the pak reads.
    uint32_t prefetch(float x, float y, float z);

    // Debug / fallback (our server, wowee.cfg npcstatic=1): appends the world DB spawns baked
    // for ADT tile x_y of the map ("s_<map>_<x>_<y>"). Returns how many.
    size_t appendStaticSpawns(uint32_t mapId, int tileX, int tileY, std::vector<CreatureUnit>& out);
    // creature displays: where tile x_y's spawn record is in the pak (the tile streamer reads it ahead).
    bool spawnEntry(uint32_t mapId, int tileX, int tileY, uint32_t& offset, uint32_t& size);

    // The terrain has set projection/view for this camera already.
    void draw(const TerrainCamera& cam, uint32_t fogColorABGR);
    const CreatureStats& stats() const { return stats_; }

    float viewDistance = 70.0f;     // yards, plus the unit's radius
    float lodDistance = 18.0f;      // LOD 1 (simplified / lower skin) beyond this
    uint32_t maxDrawn = 24;         // nearest units kept (an NPC is ~2.3k triangles near, ~1.1k far)
    uint32_t loadsPerFrame = 2;     // pak reads per frame (~1-3 ms each on the Memory Stick)
    uint32_t tocReadsPerFrame = 2;  // creature displays: TOC block reads (8 KB) per frame for lookups
    uint32_t memoryBudgetKb = 6144; // models + textures resident (owner, was 3072)
    // A model / skin / texture is read only while the heap gives its size plus this many KB in one block
    // (npcheapkb; 0 = no check) - npcbudget alone let the NPCs take the heap's last MB ("oom new failed"), and a read
    // that found no memory marked the model failed for good (an NPC never drawn again).
    // Memory diet: one block of its size AND this many KB free in total after it (pp::heapRoomFor;
    // the margin is never contiguous), 1536 by default (was 768 in one block).
    uint32_t heapReserveKb = 1536;
    // The world-entry prefetch keeps this many KB free in total (npcprefetchkb) - the next consumers of the
    // entry come first (hardware hw4: 2.4 MB read down to the margin, then "oom new failed").
    uint32_t prefetchReserveKb = 3072;

    // creature displays: NPC equipment (helm, shoulders, main-hand weapon) from items.pak - see the top.
    void setItemBank(ItemBank* items) { items_ = items; }
    // memory budget: attachcap's crowd queue (WorldScreen::remoteCrowdTurn, psp_attach_crowd.hpp) -
    // the NPCs holding item models or left without them, the give-way and the turn. While the queue runs (it asked
    // for the holders in the last ~2 s) an NPC the cap left without models waits for its turn (attachRetry) instead
    // of trying again every 120 frames.
    struct AttachHolder {
        uint64_t guid;
        float dist;       // yd from (x, y, z)
        uint32_t count;   // item models attached
        uint32_t capped;  // picks left off
    };
    void attachHolders(float x, float y, float z, std::vector<AttachHolder>& out);
    bool attachYield(uint64_t guid);  // its item models released (counted as left off)
    bool attachRetry(uint64_t guid);  // dressed again at the next update (one a frame, nearest first)
    bool equipOn = true;            // wowee.cfg npcequip
    float equipDistance = 25.0f;    // yards (npcequipdist)
    uint32_t maxEquipped = 8;       // nearest NPCs dressed (npcequipmax)

    // Creature anim. Units whose display's model has "n_<model>"
    // (WSKN) are drawn skinned with per-unit animation (upstream creature
    // sync: Stand / Walk / Run / Swim / Fly by movement, Death, state emotes)
    // unless animate is off (wowee.cfg npcanim=0: the static Stand model).
    // Pose: every frame within animDistance, every 2nd frame to twice that,
    // every 4th beyond (upstream CharacterRenderer boneInterval 45/90 yd).
    // update() once per frame before draw(): dt for the animation clocks.
    void update(float dtSec);
    void setAnimate(bool on);
    bool animate() const { return animate_; }
    float animDistance = 45.0f;
    uint32_t maxPoses = 12;  // pose evaluations per frame (0 = every due unit), longest-waiting first
    uint32_t maxBoneUploads = 2000;  // sceGuBoneMatrix per frame (13 list words each); farther units skipped
    // Combat hooks (task B wires them to its events; no-ops for unknown guids
    // or units without an animated model; an unseen unit is ignored):
    // one-shot, then back to the state emote or Stand (upstream playAnimation
    // with oneShotReturnAnim). False when the model lacks the animation.
    bool playOneShot(uint64_t guid, uint32_t animId);
    // Upstream npcSwingCallback: the first of meleeAnimChain(Generic) the
    // model has (ATTACK_1H, 2H, 2H_LOOSE, UNARMED), else ATTACK_UNARMED.
    bool playAttack(uint64_t guid);
    // Upstream hitReactionCallback for NPCs: COMBAT_WOUND (or another
    // reaction id: DODGE, BLOCK, COMBAT_CRITICAL...) only when the model has it.
    bool playWound(uint64_t guid, uint32_t animId = 9 /* anim::COMBAT_WOUND */);
    // Upstream npcDeathCallback / npcRespawnCallback. The per-frame unit flag
    // (CreatureUnit::dead from the update fields) does the same; this is for
    // events that come before the fields.
    void setDead(uint64_t guid, bool dead);
    // Upstream emoteAnimCallback, creature branch: animId from Emotes.dbc;
    // state = loop and keep across movement (UNIT_NPC_EMOTESTATE), else a
    // one-shot that resumes the kept state. 0 + state clears it.
    void playEmote(uint64_t guid, uint32_t animId, bool state);
    // Upstream readiness: ready stance while in combat (READY_1H, else
    // READY_UNARMED...) instead of Stand when idle; false = back to Stand.
    void setCombatReady(uint64_t guid, bool ready);
    bool hasAnimation(uint64_t guid, uint32_t animId) const;

    // Mounts (docs/pending/mounts_spellfx.md).
    struct SequenceInfo {  // a baked sequence, what upstream's mount discovery reads off M2Sequence
        uint32_t id = 0;
        uint16_t variation = 0;
        uint16_t flags = 0;
        uint32_t durationMs = 0;
        float movingSpeed = 0.0f;
        int16_t frequency = 0;
        uint16_t replayMax = 0;
    };
    // Is the display in the pak at all (creature displays: a lookup the first time).
    bool hasDisplay(uint32_t displayId) { return findDisplay(displayId, true) >= 0; }
    // Loads everything the display draws with, plus its seat table, now (no
    // per-frame budget: a mount appears at once, like upstream's synchronous
    // processPendingMount). True when resident; bytes = its model + textures.
    bool preload(uint32_t displayId, uint32_t* bytes = nullptr);
    // The sequences of the display's skinned model (false: not resident / static).
    bool displaySequences(uint32_t displayId, std::vector<SequenceInfo>& out) const;
    bool displayHasAnimation(uint32_t displayId, uint32_t animId) const;
    // A driven unit: play now (or as soon as the unit is first drawn).
    void playDriven(uint64_t guid, uint32_t animId, bool loop);
    // upstream CharacterRenderer::getAnimationState for a unit.
    bool animationState(uint64_t guid, uint32_t& animId, float& timeMs, float& durationMs) const;
    // After draw(): where the unit's seat is this frame, server coordinates -
    // upstream getAttachmentTransform(mount, 0 / 5 / 6 / 7 / 8, first found):
    // feet + yaw x scale x (bone x attachment offset). False: no seat point
    // (or the unit was not drawn).
    bool seatPoint(uint64_t guid, glm::vec3& out) const;
    // The display's seat table: seatZ = attachment 0's offset z (-1 none),
    // riderHeight = the baked tight-bounds guess. False without "p_".
    bool mountHeights(uint32_t displayId, float& seatZ, float& riderHeight);
    // The scale the unit is drawn at (unit x display, or the unit's own with rawScale).
    float drawScale(uint64_t guid) const;
    // class bars: a vehicle seat - where attachment `attachId` of the unit's model is this frame (server
    // coordinates, the seatPoint math; attachId < 0 = the model's origin), plus the model's rider height guess
    // (kAttachRiderHeight, 1.8 without) and the unit's yaw. Loads the model's "p_" table on first use. False: the unit
    // was not drawn / not resident, or the model lacks that point (an older creatures.pak: the vehicle seat points
    // came with the class bars - the caller then uses the seat's offset from the unit's feet).
    bool attachPoint(uint64_t guid, int32_t attachId, glm::vec3& out, float* riderHeight = nullptr, float* yaw = nullptr);
    // game object use: a whole record of the open pak by name (e.g. "goanim"), one read; false when
    // the pak lacks it.
    bool readRecord(const char* name, std::vector<uint8_t>& out);

private:
    struct Model {
        void* blob = nullptr;
        const ModelHeader* h = nullptr;
        const ModelVertex* verts = nullptr;
        const uint16_t* indices = nullptr;
        const ModelBatch* batches = nullptr;
        uint32_t bytes = 0;
        uint32_t lastUsed = 0;
        bool failed = false;
    };
    struct Tex {
        platform::psp::Texture t;
        uint32_t bytes = 0;
        uint32_t lastUsed = 0;
        bool loaded = false;
        bool failed = false;
    };
    struct Display {
        uint32_t id = 0;
        uint32_t lastUsed = 0;      // creature displays: frame (the pool evicts the least recent)
        bool loaded = false, failed = false;
        bool texturesKnown = false; // `textures` filled (needs the model once)
        CreatureDisplayHeader h{};
        std::vector<uint8_t> active;
        std::vector<uint16_t> textures;  // "x_<n>" the shown batches need (model + slots)
        uint8_t skinMode = 0;       // 0 undecided, 1 skinned ("n_"), 2 static ("m_")
        bool hasEquip = false;      // creature displays: CreatureDisplayEquip read
        CreatureDisplayEquip equip{};
    };
    struct Unit {
        CreatureUnit u;
        int32_t display = -1;       // slot in displays_, -1 = not looked up, -2 = not in the pak
    };
    struct AnimUnit;  // Creature anim (below)
public:
    // The target's portrait - the client's SetPortraitTexture. The
    // unit is drawn again, after draw(), into a 64 x 64 GU_PSM_5650 VRAM
    // target (vramRel, VRAM-relative) with a camera on the front of its head.
    // Only a unit drawn this frame; false = not drawn (keep the old picture).
    bool drawPortrait(uint64_t guid, uint32_t vramRel, const TerrainCamera& cam);
    // PSP (owner: "the portrait freezes when I run away from the unit"): the portrait's unit (target /
    // conversation NPC) stays in the frame's list - animated and posed - when the camera culls it; then it is
    // not drawn in the world (portraitOnly), only into the portrait. 0 = none.
    void setPortraitUnit(uint64_t guid) { portraitUnit_ = guid; }
    // The target's outline (owner's reference: a glowing rim in the
    // reaction colour around the selected model). After draw(): a slightly
    // bigger copy of its opaque batches in one colour, no depth writes, then
    // the model again on top - only the rim outside the silhouette remains.
    bool drawOutline(uint64_t guid, uint32_t colorABGR, const TerrainCamera& cam, uint32_t fogColorABGR);

private:
    struct Visible {
        const Unit* unit;
        Display* d;
        float dist;
        uint8_t lod;
        ScePspFMatrix4 mtx;
        AnimUnit* anim = nullptr;   // Skinned (bone matrices in anim->ge)
        bool hull = false;          // The outline pass (opaque batches, one colour)
        bool skipped = false;       // Over the frame's bone upload / list budget
        bool portraitOnly = false;  // Culled in the world, kept for the portrait
    };
    uint64_t portraitUnit_ = 0;     // setPortraitUnit

    // creature displays: displays on demand. `lookup`: false = only what is known (no card access).
    int32_t findDisplay(uint32_t displayId, bool lookup);
    int32_t findDisplay(uint32_t displayId) const;
    bool loadDisplay(Display& d);
    bool loadModel(uint16_t id);
    bool loadTexture(uint16_t id);
    // Loads what the display still misses, within the frame's budget; true
    // when everything is resident.
    bool ensure(Display& d, uint32_t& budget);
    const platform::psp::Texture* texture(const Display& d, uint16_t batchTexture, uint8_t active = 1) const;
    void setBatchState(const Display& d, const ModelBatch& b, uint8_t active = 1);
    void drawBatch(const Model& m, const ModelBatch& b);
    void drawUnit(const Visible& v, bool blendPass);
    void evict();
    void evictDisplays();

    // creature displays: the pak (sorted: index_; older: pak_) and the place of an entry on the card.
    enum EntryKind : uint8_t { kEntryDisplay = 0, kEntryModel, kEntrySkin, kEntryPoints, kEntryTexture };
    bool findEntry(EntryKind kind, uint32_t id, platform::psp::PakEntry& out);
    bool tocBudgetLeft() const;
    void* loadEntry(const platform::psp::PakEntry& e);
    // Whether an entry of `size` bytes may be read now - the heap gives it plus heapReserveKb in one block.
    // No: the read waits (the display / model / skin / texture is NOT marked failed) and this frame reads no more.
    bool roomFor(uint32_t size);
    uint32_t heapHeldFrame_ = 0xFFFFFFFFu;  // the frame (frame_) whose reads stopped for want of heap
    bool prefetching_ = false;              // Inside prefetch() (prefetchReserveKb)
    struct EntrySlot {
        uint32_t key = 0xFFFFFFFFu;  // kind << 24 | id
        uint32_t offset = 0, size = 0;
    };
    static constexpr uint32_t kEntryCacheSlots = 512;   // 6 KB
    static constexpr uint32_t kDisplayPool = 128;       // displays kept (slots reserved at open, ~18 KB)
    std::vector<EntrySlot> entryCache_;
    platform::psp::PakIndex index_;
    platform::psp::Pak pak_;
    uint32_t frameTocReads0_ = 0;   // index_.blockReads() at the start of this frame's budgeted work
    bool budgetToc_ = false;        // draw(): TOC reads count against tocReadsPerFrame
    uint32_t lookups_ = 0, lookupMisses_ = 0;

    std::vector<Display> displays_;          // the pool: slots, capacity reserved (Display* stay valid)
    std::vector<int32_t> freeDisplays_;
    std::unordered_map<uint32_t, int32_t> displayIdx_;  // displayId -> slot, -2 = not in the pak
    std::unordered_map<uint16_t, Model> models_;        // resident (and failed) only
    std::unordered_map<uint16_t, Tex> textures_;
    std::vector<Unit> units_;
    std::vector<Visible> visible_;
    std::vector<std::pair<float, Unit*>> near_;  // creature displays: the frame's candidates, nearest first
    std::vector<uint32_t> order_;
    uint32_t frame_ = 0;
    uint32_t memoryBytes_ = 0;
    uint32_t nextLogMs_ = 0;
    uint32_t displayEvictions_ = 0;
    CreatureStats stats_;

    // Creature anim.
    struct Skin {
        void* blob = nullptr;
        SkinView view;
        uint32_t lod0Batches = 0;   // batches [0, lod0) LOD 0, the rest LOD 1
        uint32_t bytes = 0;
        uint32_t lastUsed = 0;
        bool failed = false;
    };
    struct AnimUnit {
        SkinAnimState s;
        int32_t skin = -1;          // model index whose WSKN the state runs on (-1 = not bound)
        uint32_t lastSeen = 0;      // frame
        uint32_t lastPose = 0;      // frame the pose was evaluated
        bool poseValid = false;
        bool wasMoving = false, wasWalking = false, wasSwimming = false, wasFlying = false;
        bool dead = false;          // from the unit flag or setDead
        bool deadEvent = false;     // setDead(true) seen (kept until the flag agrees or respawn)
        bool ready = false;         // setCombatReady
        bool dirty = true;          // the pose changed since it was last evaluated
        uint32_t emote = 0;         // kept state emote anim (upstream creatureActiveEmotes)
        uint32_t fieldEmote = 0;    // last CreatureUnit::emoteAnim seen (a change applies it)
        uint32_t loopAnim = 0;      // what the unit loops now (a one-shot resumes it)
        float lastX = 0, lastY = 0, lastZ = 0;
        std::vector<BoneMatrix> scratch;
        std::vector<ScePspFMatrix4> ge;
        // Mounts - a driven unit's request not yet applied (no state bound yet).
        uint32_t drivenAnim = 0;
        bool drivenLoop = true, drivenPending = false;
        // creature displays: NPC equipment - the item models on this unit (null: none / out of range).
        std::shared_ptr<CharacterAttachments> attach;
        uint64_t attachKey = 0;     // display id << 32 | main hand: what `attach` was dressed for
        // other players' gear: the helm attached and its HelmetGeosetVisData hides the hair (the scalp batches out,
        // the bald cap in, as a player's: applyEquipmentGeosets); the frame attachcap last left models off.
        bool helmHidesHair = false;
        uint32_t capRetryFrame = 0;
        bool retryNow = false;      // memory budget: the crowd queue's turn (attachRetry)
    };
    uint32_t crowdQueueFrame_ = 0;  // memory budget: the frame the crowd queue last asked (attachHolders)
    bool useSkin(const Display& d) const;
    bool loadSkin(uint16_t id);
    Skin* skinOf(int32_t model);
    const Skin* skinOf(int32_t model) const;
    AnimUnit* animFor(uint64_t guid);
    const AnimUnit* animFor(uint64_t guid) const;
    void bindAnim(AnimUnit& a, const Unit& un, const Skin& sk);
    void driveAnim(AnimUnit& a, const Unit& un, const Skin& sk);
    uint32_t idleAnim(const AnimUnit& a, const Unit& un, const Skin& sk) const;
    void drawSkinnedUnit(const Visible& v, bool blendPass);
    // Mounts - "p_<n>" points (creature displays: of every model), loaded on first use.
    struct Seats {
        bool loaded = false;
        std::vector<AttachPoint> points;
    };
    std::unordered_map<uint16_t, Seats> seats_;
    const Seats* seatsOf(uint16_t model);
    const Unit* unitOf(uint64_t guid) const;
    float unitScale(const CreatureUnit& u, const Display& d) const { return u.rawScale ? u.scale : u.scale * d.h.scale; }
    std::unordered_map<uint16_t, Skin> skins_;
    std::unordered_map<uint64_t, AnimUnit> anims_;
    SkinDrawer skinDrawer_;
    std::vector<const Visible*> posing_;
    bool animate_ = true;
    float frameDt_ = 0.0f;
    uint32_t skinLogs_ = 0;
    // creature displays: NPC equipment.
    ItemBank* items_ = nullptr;
    void updateEquipment();
    void drawEquipment(const TerrainCamera& cam);
    uint32_t equipLogs_ = 0;
    // walk/run - PSP: Walk/Run/Swim/Fly play at the unit's spline
    // speed over the sequence's M2 movingSpeed (x scale), clamped 0.5..2, as
    // the original client does (upstream plays them at 1.0: a patrol walking at
    // 2.5 yd/s strode in place). wowee.cfg npcanimspeed=0 = upstream.
public:
    bool moveAnimRate = true;
private:
    float moveRate(const AnimUnit& a, const Unit& un, const Skin& sk) const;

public:
    // Sounds (src/rendering/psp/psp_footsteps.cpp). Animated units
    // within maxDist of (x, y, z) (server coords): position, the clock of the
    // sequence in play and its footfall ($FSD) times. Read-only; the caller
    // detects the crossings (upstream FootstepDriver) and plays the sound.
    struct FootstepProbe {
        uint64_t guid;
        uint32_t displayId;
        float x, y, z;
        float dist;
        uint32_t animId;
        float timeMs, durationMs;
        const uint32_t* events;  // nullptr when the model has none
        uint32_t eventCount;
        bool moving;             // on foot: moving, not swimming, flying or dead
    };
    uint32_t footstepProbes(float x, float y, float z, float maxDist, FootstepProbe* out, uint32_t max) const;
};

} // namespace wowee::rendering::psp
