// Offline doodad format shared by tools/psp/converter (psp_model_conv, PC)
// and src/rendering/psp/psp_models.cpp (PSP). Plain little-endian PODs, no
// platform headers: both sides include this file.
//
// One pak per map, "data/doodads_<map>.pak", entries:
//   "m_<n>"   WMDL model n       (static M2 at rest pose, one skin + low LOD)
//   "x_<n>"   WTEX texture n     (T8 swizzled + 256 x ABGR8888 CLUT, see
//                                 include/platform/psp/psp_texture.hpp)
//   "d_<x>_<y>" WDOD placements of ADT tile x_y (MDDF, pre-transformed; and
//             since M8 every group of each MODF building plus the building's
//             doodads, uniqueId = hash(building uniqueId, part) | 0x80000000)
//
// WMDL: ModelHeader, then (16-byte aligned, offsets from blob start)
//   ModelVertex[vertexCount], uint16_t indices[indexCount],
//   ModelBatch[batchCount[0] + batchCount[1] (+ lod2BatchCount with kModelLod2)]
//   (full detail first, then LOD 1, then the far LOD 2 of trees).
// A vertex position p (s16) is (p / 32768) * half + center in model space,
// a texture coordinate t (u16) is (t / 32768) * uvScale + uvMin: exactly what
// the GE computes with the model matrix and sceGuTexScale/sceGuTexOffset.
//
// WDOD: DoodadTileHeader, then DoodadPlacement[count].
#pragma once

#include <cstdint>

namespace wowee::rendering::psp {

constexpr uint16_t kModelFormatVersion = 1;

enum ModelFlags : uint16_t {
    kModelFoliage = 1u << 0,       // tree/bush: drawn farther, alpha-tested leaves
    kModelGroundDetail = 1u << 1,  // grass/clutter: drawn close only
    kModelBuilding = 1u << 2,      // WMO group (M8): placed like a doodad, drawn far
    kModelNearSplit = 1u << 4,     // LOD 0 = big triangles split (drawn only near the camera),
                                   // LOD 1 = the original mesh
    kModelStrips = 1u << 3,        // non-indexed GU_TRIANGLE_STRIP: batch indexStart/indexCount
                                   // are a vertex range, indexCount (header) is 0
    kModelLod2 = 1u << 5,          // forest LOD: lod2BatchCount batches follow LOD 1, a
                                   // simplified far mesh (trees); older paks: never set, field 0
};

enum ModelBlend : uint8_t {
    kBlendOpaque = 0,
    kBlendAlphaKey = 1,   // alpha test ~0.4
    kBlendAlpha = 2,
    kBlendAdd = 3,
    kBlendModulate = 4,
};

enum BatchFlags : uint8_t {
    kBatchWrapU = 1u << 0,
    kBatchWrapV = 1u << 1,
    kBatchUnlit = 1u << 2,
};

constexpr uint16_t kNoTexture = 0xFFFF;
// Character slots filled at run time (M2 texture types 1/8 and 6).
constexpr uint16_t kTexBody = 0xFFF1;
constexpr uint16_t kTexHair = 0xFFF6;

struct ModelHeader {
    char magic[4];          // "WMDL"
    uint16_t version;
    uint16_t flags;         // ModelFlags
    uint16_t vertexCount;
    uint16_t lod2BatchCount; // with kModelLod2 (was reserved, 0 in older paks)
    uint32_t indexCount;
    uint16_t batchCount[2]; // [0] full detail, [1] low LOD (0 = none)
    float center[3];
    float half[3];          // per-axis half extent (never 0)
    float uvMin[2];
    float uvScale[2];
    float radius;           // bounding sphere radius around center, model space
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t batchOffset;
    uint32_t geosetOffset;  // 0, or u16 submesh id per batch (characters)
};
static_assert(sizeof(ModelHeader) == 80, "WMDL header is 80 bytes");

// GE vertex: GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT.
struct ModelVertex {
    uint16_t u, v;
    uint16_t color;         // 5650, baked sun light (texture modulates it)
    int16_t x, y, z;
};
static_assert(sizeof(ModelVertex) == 12, "GE u16 UV + 5650 + s16 xyz vertex is 12 bytes");

struct ModelBatch {
    uint32_t indexStart;
    uint32_t indexCount;
    uint16_t texture;       // "x_<n>" or kNoTexture (white)
    uint8_t blend;          // ModelBlend
    uint8_t flags;          // BatchFlags
};
static_assert(sizeof(ModelBatch) == 12, "WMDL batch is 12 bytes");

// Game objects were seen without a model (e.g. a mailbox): the game objects' displays
// (GameObjectDisplayInfo.dbc -> its M2) in data/gameobjects.pak, "gidx": a GameObjectIndexHeader and the records
// sorted by displayId; the models, textures and collision meshes are "m_" / "x_" / "k_" of the same pak.
// WMO displays (transports, some doors) are not baked yet (counted by the converter).
struct GameObjectIndexHeader {
    char magic[4];          // "WGOI"
    uint16_t version;       // kModelFormatVersion
    uint16_t reserved;
    uint32_t count;
    uint32_t reserved1;
};
static_assert(sizeof(GameObjectIndexHeader) == 16, "WGOI header is 16 bytes");

struct GameObjectDisplayRec {
    uint32_t displayId;
    uint16_t model;         // "m_<n>"
    uint16_t radiusCm;      // bounding sphere radius, model space, centimetres
    int16_t centerCm[3];    // bounding sphere centre, model space, centimetres
    uint16_t flags;         // 0
};
static_assert(sizeof(GameObjectDisplayRec) == 16, "WGOI record is 16 bytes");

struct DoodadTileHeader {
    char magic[4];          // "WDOD"
    uint16_t version;
    uint16_t reserved;
    uint32_t count;
    uint32_t reserved1;
};
static_assert(sizeof(DoodadTileHeader) == 16, "WDOD header is 16 bytes");

// Upstream placement (terrain_manager placementEuler + M2Instance model
// matrix) already applied: world = axes * modelPos + position, render/server
// coordinates. Doodads crossing tile borders appear in several tiles with
// the same uniqueId.
struct DoodadPlacement {
    uint32_t uniqueId;
    uint16_t model;         // "m_<n>"
    uint16_t reserved;
    float position[3];
    float axes[9];          // column-major 3x3: rotation * scale
    float sphere[4];        // world bounding sphere: center xyz, radius
};
static_assert(sizeof(DoodadPlacement) == 72, "WDOD placement is 72 bytes");

// Tree impostors (docs/PSP_PORT_ARCHITECTURE.md): "i_<n>" next to
// "m_<n>" in the doodads pak, only for foliage models. `views` pictures of the
// model rendered by the converter's software rasterizer (orthographic, from
// the horizon, textures + alpha + baked sun light like the WMDL vertices),
// in one WTEX atlas "x_<texture>" of cols x rows cells of cellSize texels.
// View i looks at the model from model-space direction
//   (cos(2*pi*i/views), sin(2*pi*i/views), 0)
// towards the vertical axis through (axis[0], axis[1]); in the picture +x is
// (-sin, cos, 0) and +y is model +z. A cell maps the rectangle
// [-halfWidth, +halfWidth] x [zMin, zMax] around that axis to its inner
// cellSize - 2 * margin texels (the margin is transparent: no bleeding
// between cells under bilinear filtering). The PSP draws one camera-facing
// quad per far instance with the nearest view. Older paks: no "i_" entries.
constexpr uint16_t kImpostorVersion = 1;

struct ImpostorHeader {
    char magic[4];          // "WIMP"
    uint16_t version;
    uint8_t views;          // around the vertical axis (8)
    uint8_t cols;           // atlas columns; rows = ceil(views / cols)
    uint16_t texture;       // "x_<n>" atlas
    uint16_t cellSize;      // texels per cell side (power of two)
    uint16_t margin;        // transparent texels on each side of a cell
    uint16_t reserved0;
    float axis[2];          // model-space x, y of the vertical axis
    float zMin, zMax;       // model-space height range
    float halfWidth;        // model-space half width around the axis (every view)
    uint32_t sourceTriangles;  // LOD 0 triangles it stands in for (stats)
    uint32_t reserved[3];
};
static_assert(sizeof(ImpostorHeader) == 52, "WIMP header is 52 bytes");

// Collision (M8+): "k_<n>" next to "m_<n>" in the doodads pak, model space.
// WMO groups: the group's own triangles (not the render split), wall
// blocking per upstream WMORenderer MOPY rule; M2: the model's bounding
// (collision) triangles, every one blocking. A 2D grid over the model's XY
// lists the triangles touching each cell (upstream buildCollisionGrid, 4 yd).
//   CollisionHeader, float xyz[vertexCount], CollisionTri[triangleCount],
//   uint32 cell[nx*ny][2] = {start, count} into uint32 list[listCount]
struct CollisionHeader {
    char magic[4];          // "WCOL"
    uint16_t version;
    uint16_t reserved;
    uint32_t vertexCount, triangleCount, listCount;
    float bmin[3], bmax[3];
    float cell;
    uint16_t nx, ny;
    uint32_t vertexOffset, triangleOffset, cellOffset, listOffset;
};
static_assert(sizeof(CollisionHeader) == 68, "WCOL header is 68 bytes");

enum CollisionTriFlags : uint8_t {
    kColBlocksWalls = 1u << 0,  // counts for wall sweeps (floors use every triangle)
};

struct CollisionTri {
    uint16_t a, b, c;
    uint8_t flags;
    uint8_t pad;
};
static_assert(sizeof(CollisionTri) == 8, "WCOL triangle is 8 bytes");

// Characters: "data/characters.pak" (psp_char_conv), entries
//   "c_<race>_<sex>"  WMDL, stand pose frame 0, geosetOffset set
//   "n_<race>_<sex>"  WSKN skinned model + skeleton + animations (M9, below)
//   "a_<race>_<sex>"  WAPP appearance tables (built from CharSections,
//                     CharHairGeosets, CharacterFacialHairStyles by upstream's
//                     resolveCharacterSections / geoset rules)
//   "l_<n>"           WLAY body-texture layer (skin, face, underwear)
//   "x_<n>"           WTEX (hair and fixed model textures)
// The body texture is composited on the PSP from layers (upstream
// CharacterRenderer::compositeTextures order and regions, at half size).
constexpr int kBodyTexSize = 128;   // upstream atlas is 256
constexpr uint16_t kNoLayer = 0xFFFF;
constexpr int kBodyLayers = 6;      // skin, face lower, face upper, underwear x3

struct AppearanceHeader {
    char magic[4];          // "WAPP"
    uint16_t version;
    uint8_t race, sex;
    uint8_t skins, faces, hairStyles, hairColors;
    uint8_t facials, reserved[3];
    uint32_t bodyOffset;    // u16 layers[skins][faces][kBodyLayers]
    uint32_t hairOffset;    // u16 texture[hairStyles][hairColors] ("x_<n>")
    uint32_t hairGeosetOffset;   // u16 [hairStyles]: scalp geoset (0 = default 1)
    uint32_t facialOffset;  // u16 [facials][3]: variants of groups 100, 200, 300
};
static_assert(sizeof(AppearanceHeader) == 32, "WAPP header is 32 bytes");

// Layer pixels: T8 linear (not swizzled) + ABGR8888 CLUT, placed at (x, y) of
// the kBodyTexSize canvas and alpha-blended over what is there.
struct LayerHeader {
    char magic[4];          // "WLAY"
    uint16_t w, h;
    uint16_t x, y;
    uint32_t reserved;
    // uint32_t clut[256]; uint8_t pixels[w * h];
};
static_assert(sizeof(LayerHeader) == 16, "WLAY header is 16 bytes");

// Animated characters (M9): "n_<race>_<sex>" WSKN in data/characters.pak, next
// to the static "c_" model (kept: the PSP falls back to it when "n_" is absent).
// GE hardware skinning: the GE blends up to 8 bone matrices per draw with
// per-vertex weights (sceGuBoneMatrix, GU_WEIGHTS(n) | GU_WEIGHT_8BIT). Each
// batch is split offline into sub-batches whose triangles use <= 8 bones; a
// sub-batch is one non-indexed GU_TRIANGLE_STRIP with its own palette
// (slot -> bone) and its own vertex format:
//   u8 weight[n] (+ pad to even), u16 u, v, u16 color 5650, s16 x, y, z
//   stride = skinVertexStride(n) = align2(n) + 12
// Weight 128 = 1.0 (GE 8-bit unsigned fixed point), the weights of a vertex sum
// to 128. Positions are in the bind pose, normalised like WMDL (center/half);
// the dequantisation is folded into the bone matrices on the PSP, so the GE
// computes  world * sum_j(w_j * bone_j * D) * p  with D = T(center) * S(half).
// Light is baked per vertex from the Stand pose (frame 0) normals.
//
// Blob (offsets from blob start, sections 4-aligned, vertices 16-aligned):
//   SkinHeader, SkinBone[boneCount], AnimSequence[sequenceCount],
//   ModelBatch[batchCount] (indexStart/indexCount = range of sub-batches),
//   u16 geoset[batchCount], SkinSubBatch[subBatchCount],
//   BoneTrack[sequenceCount][boneCount], int16 key[keyCount][4], vertices.
//
// Bones: only those the skinned vertices use plus their ancestors, in M2
// order (parents before children), parent index into this list. Upstream
// CharacterRenderer rules: local = T(pivot) * T * R * S * T(-pivot) sampled by
// rendering/m2_track_sampler.hpp, world = parent * local. Global sequences
// are sampled at time 0.
// Sequences: variation 0 of each wanted animation id (upstream playAnimation
// prefers variation 0), resampled at a fixed rate: frame k is at
// k * durationMs / (frameCount - 1), the last frame at the duration itself;
// the PSP interpolates linearly between frames (nlerp for rotations).
// Track of one bone in one sequence: kTrackNone (default value: identity,
// no translation, unit scale), a key index (constant), or
// key index | kTrackAnimated (frameCount keys). Keys: rotation x, y, z, w
// / 32767 (consecutive frames on one hemisphere); translation x, y, z *
// translationScale; scale x, y, z / kSkinScaleOne.
constexpr uint16_t kSkinFormatVersion = 1;
constexpr int kSkinMaxPalette = 8;          // GE bone matrices
constexpr float kSkinWeightOne = 128.0f;    // GU_WEIGHT_8BIT
constexpr float kSkinScaleOne = 4096.0f;
constexpr uint32_t kTrackNone = 0xFFFFFFFFu;
constexpr uint32_t kTrackAnimated = 0x80000000u;

struct SkinHeader {
    char magic[4];          // "WSKN"
    uint16_t version;
    uint16_t flags;         // 0
    uint16_t boneCount;
    uint16_t sequenceCount;
    uint16_t batchCount;
    uint16_t subBatchCount;
    float center[3];        // bind pose: position = center + half * (s16 / 32768)
    float half[3];
    float uvMin[2];
    float uvScale[2];
    float standHeight;      // top of the Stand pose, frame 0 (feet at z = 0)
    float radius;           // bind-pose bounding sphere around center
    float translationScale; // bone translation key * this = yards
    float sampleRate;       // frames per second the sequences were baked at (info)
    uint32_t boneOffset;
    uint32_t sequenceOffset;
    uint32_t batchOffset;
    uint32_t geosetOffset;
    uint32_t subBatchOffset;
    uint32_t trackOffset;
    uint32_t keyOffset;
    uint32_t keyCount;
    uint32_t vertexBytes;   // whole vertex area (for memory stats)
    uint32_t reserved[3];
};
static_assert(sizeof(SkinHeader) == 120, "WSKN header is 120 bytes");

struct SkinBone {
    int16_t parent;         // index in this list (< own index), -1 = root
    uint16_t m2Bone;        // source M2 bone (debugging)
    float pivot[3];
};
static_assert(sizeof(SkinBone) == 16, "WSKN bone is 16 bytes");

struct AnimSequence {
    uint16_t animId;        // rendering/animation/animation_ids.hpp
    uint16_t variation;
    uint32_t durationMs;
    uint16_t frameCount;    // >= 2
    uint16_t flags;         // M2 sequence flags (low 16 bits, info)
    float movingSpeed;      // M2 moving speed (info)
    // Variations of one animation id (idle fidgets): the M2 frequency
    // (probability x 0x7FFF that this variation plays next) and how many
    // times it repeats (replay min..max). 0/0/0 in paks baked before.
    int16_t frequency;
    uint16_t replayMin;
    uint16_t replayMax;
    uint16_t blendTimeMs;   // M2 blendTime: cross-fade into this sequence (0 in older paks)
};
static_assert(sizeof(AnimSequence) == 24, "WSKN sequence is 24 bytes");

struct BoneTrack {
    uint32_t rotation, translation, scale;  // kTrackNone / key index [| kTrackAnimated]
};
static_assert(sizeof(BoneTrack) == 12, "WSKN track is 12 bytes");

struct SkinSubBatch {
    uint32_t vertexOffset;  // bytes from blob start, 16-aligned
    uint16_t vertexCount;   // strip length
    uint8_t weights;        // n = 1..8: GU_WEIGHTS(n), palette slots 0..n-1
    uint8_t reserved;
    uint8_t palette[kSkinMaxPalette];  // slot -> SkinBone index
};
static_assert(sizeof(SkinSubBatch) == 16, "WSKN sub-batch is 16 bytes");

constexpr int skinVertexStride(int weights) { return ((weights + 1) & ~1) + 12; }

// Emotes (M9c), in characters.pak:
//   "o_<race>_<sex>"  WSKA: extra sequences for the same skeleton as n_ (a
//                     SkinHeader with magic "WSKA", batchCount = 0: bones,
//                     sequences, tracks, keys, no vertices). Loaded on the
//                     first emote only - the dances alone are larger than the
//                     whole locomotion set.
//   "emotes"          EmoteTable + EmoteRow[count]: Emotes.dbc (ID, AnimID,
//                     EmoteSpecProc != 0 = a state emote), what upstream
//                     EmoteRegistry reads for SMSG_EMOTE / UNIT_NPC_EMOTESTATE.
struct EmoteTable {
    char magic[4];          // "WEMO"
    uint32_t count;
};
struct EmoteRow {
    uint16_t emoteId;
    uint16_t animId;        // 0 = none (text only)
    uint8_t state;          // EmoteSpecProc != 0
    uint8_t reserved[3];
};
static_assert(sizeof(EmoteRow) == 8, "emote row is 8 bytes");

// Chat. "emotetext" in characters.pak (psp_char_conv): EmotesText.dbc
// + EmotesTextData.dbc, what upstream EmoteRegistry reads to turn /dance into
// CMSG_TEXT_EMOTE and SMSG_TEXT_EMOTE into a chat line. Layout:
//   EmoteTextHeader, EmoteTextRow[rowCount], EmoteTextAlias[aliasCount]
//   (sorted by command bytes), char pool[poolSize] (NUL-terminated UTF-8).
// Texts keep the client's "%s" placeholders (sender first, then target).
// Reader shared by the PSP and the converter's --verify:
// include/rendering/psp/psp_emote_text.hpp.
enum EmoteTextSlot : uint8_t {
    kEtOthersTarget,     // EmotesText field 3 ("%s bows before %s.")
    kEtSenderTarget,     // field 5 ("You bow before %s.")
    kEtOthersNoTarget,   // field 7 ("%s bows down graciously.")
    kEtSenderNoTarget,   // field 9 ("You bow down graciously.")
    kEtSlotCount
};
constexpr uint32_t kEmoteTextNone = 0xFFFFFFFFu;
struct EmoteTextHeader {
    char magic[4];          // "WETX"
    uint16_t version;       // 1
    uint16_t slots;         // kEtSlotCount
    uint32_t rowCount;
    uint32_t aliasCount;
    uint32_t poolSize;
};
struct EmoteTextRow {
    uint16_t textEmoteId;   // EmotesText ID: CMSG_TEXT_EMOTE / SMSG_TEXT_EMOTE
    uint16_t emoteId;       // EmoteRef (Emotes.dbc), 0 = text only
    uint32_t text[kEtSlotCount];  // pool offsets, kEmoteTextNone = none
};
struct EmoteTextAlias {
    uint32_t command;       // pool offset: lower-case command without '/', UTF-8
    uint16_t row;           // index into the rows
    uint8_t source;         // 0 = EmotesText name (upstream), 1 = GlobalStrings EMOTEn_CMDm (localized)
    uint8_t reserved;
};
static_assert(sizeof(EmoteTextHeader) == 20, "emote text header is 20 bytes");
static_assert(sizeof(EmoteTextRow) == 20, "emote text row is 20 bytes");
static_assert(sizeof(EmoteTextAlias) == 8, "emote text alias is 8 bytes");

// Creatures and NPCs (M10b): "data/creatures.pak" (psp_creature_conv), entries
//   "m_<n>"          WMDL, stand pose frame 0 like the characters, geosetOffset
//                    set; a batch texture is "x_<n>", kNoTexture, or
//                    kTexSlot + M2 texture type (a replaceable slot, filled per
//                    display: 1/8 skin, 6 hair, 11-13 creature skins)
//   "x_<n>"          WTEX
//   "d_<displayId>"  WCDI: model, size, slot textures and batch mask of one
//                    CreatureDisplayInfo row (upstream EntitySpawner rules)
//   "s_<map>_<x>_<y>" WCSP static spawns of ADT tile x_y from the world DB -
//                    a debug fallback until live units arrive (M10a)
constexpr uint16_t kTexSlot = 0xFFF0;  // + type; kTexBody / kTexHair are types 1 and 6
constexpr int kTexSlotCount = 16;

enum CreatureDisplayFlags : uint16_t {
    kCreatureHumanoid = 1u << 0,  // CreatureDisplayInfoExtra: character model + NPC skin
    kCreatureBaked = 1u << 1,     // skin is the BakedNpcTextures picture
};

struct CreatureDisplayHeader {
    char magic[4];          // "WCDI"
    uint16_t version;
    uint16_t model;         // "m_<n>"
    uint16_t flags;         // CreatureDisplayFlags
    uint16_t batchCount;    // model batchCount[0] + batchCount[1]
    float scale;            // CreatureDisplayInfo scale x CreatureModelData scale
                            // (the unit's own OBJECT_FIELD_SCALE_X multiplies it)
    uint16_t slotTexture[kTexSlotCount];  // per M2 texture type: "x_<n>" or kNoTexture (white)
    // uint8_t active[batchCount]: 1 = drawn, 0 = hidden (geosets)
};
static_assert(sizeof(CreatureDisplayHeader) == 48, "WCDI header is 48 bytes");

struct CreatureSpawnHeader {
    char magic[4];          // "WCSP"
    uint16_t version;
    uint16_t reserved;
    uint32_t count;
    uint32_t reserved1;
};
static_assert(sizeof(CreatureSpawnHeader) == 16, "WCSP header is 16 bytes");

// One creature row of the world DB (server coordinates and orientation).
struct CreatureSpawn {
    uint32_t guid;          // creature.guid (not the packed object guid)
    uint32_t displayId;     // chosen from creature_template_model
    float position[3];
    float orientation;
    float scale;            // creature_template_model.DisplayScale
    uint32_t entry;
};
static_assert(sizeof(CreatureSpawn) == 32, "WCSP spawn is 32 bytes");

// Attachment points of a character model (helmets, shoulders,
// weapons, shields), in characters.pak next to the model:
//   "p_<race>_<sex>"  WATP: AttachTable + AttachPoint[count]. Each M2
//                     attachment id resolved on the PC by upstream
//                     CharacterRenderer::findAttachmentBone (lookup table, then
//                     scan by id, then key bones 26/27 for the hands, bone 0 for
//                     the helm). The item model sits at  bone * T(position)
//                     (upstream getAttachmentTransform / the weapon matrix).
// `bone` indexes the WSKN bone list of "n_" (psp_char_conv keeps attachment
// bones in it); `stand` is the M2 bone's Stand frame 0 matrix, what the
// static "c_" model (and an old "n_" without the bone) uses.
constexpr uint16_t kNoAttachBone = 0xFFFF;

struct AttachTable {
    char magic[4];          // "WATP"
    uint16_t version;       // kModelFormatVersion
    uint16_t count;
    // AttachPoint points[count];
};
static_assert(sizeof(AttachTable) == 8, "WATP header is 8 bytes");

struct AttachPoint {
    uint16_t id;            // M2 attachment id (0 shield, 1/2 hands, 5/6 shoulders, 11 helm, 12 back, ...)
    uint16_t bone;          // WSKN bone index, kNoAttachBone when "n_" lacks it
    float position[3];      // model space, applied after the bone matrix
    float stand[12];        // BoneMatrix layout (psp_skeleton.hpp): 3x4 column-major
};
static_assert(sizeof(AttachPoint) == 64, "WATP point is 64 bytes");

// Mounts. "p_<n>" in data/creatures.pak next to "m_<n>" / "n_<n>"
// of a model some mount display uses (psp_creature_conv --mounts): the same
// WATP table, holding only the seat attachments upstream's
// AnimationController::applyMountPositioning probes (0 "MountMain", then
// 5, 6, 7, 8; their bones are kept in n_), plus one pseudo point
// kAttachRiderHeight whose position[2] is upstream's rider height guess from
// the model's tight vertex bounds (EntitySpawner: maxZ * 0.8, extent * 0.75,
// 1.8 - what a remote player's rider sits at, and the own mount's fallback).
constexpr uint16_t kAttachRiderHeight = 0xFFFE;
inline constexpr uint16_t kMountSeatAttachIds[] = {0, 5, 6, 7, 8};
// data/creatures.pak from the DBCs alone - every CreatureDisplayInfo row
// (~24 000), each model once, each texture file once; written sorted with the "~fence" record
// (tools/psp/converter conv_common.hpp SortedPakWriter) and read on demand through PakIndex
// (include/platform/psp/psp_pak_index.hpp): nothing per display is in RAM until a unit shows it. Additions
// (older paks and runtimes stay readable both ways):
//  - "p_<n>" for every model, not only mounts: the points findAttachmentBone resolves among
//    kCreaturePointIds - upstream applyMountPositioning's seat probe (0, 5, 6, 7, 8) and the equipment
//    points of a humanoid NPC (main hand 1, off hand 2, shoulders 5/6, helm 11) - their bones kept in "n_",
//    + kAttachRiderHeight. Without a DB a model is a mount when the server says so (UNIT_FIELD_MOUNTDISPLAYID):
//    any display can be one.
//  - WCDI flag kCreatureEquip: after active[batchCount], at the next multiple of 4, CreatureDisplayEquip -
//    the CreatureDisplayInfoExtra equipment of a humanoid NPC (helm / shoulders / cape drawn at run time
//    from items.pak, the original client's look; upstream's world path draws only the cape and the weapon).
//  - active[i] == kBatchCape: a cape batch (geoset group 15 or an M2 texture of type 2) of a display whose
//    cape is shown (upstream normalizeHumanoidClothingGeosets: hasRenderableCape) - drawn with the cape
//    texture (upstream setGroupTextureOverride(15) + the type-2 slots) when the runtime has it, else as 1.
// + the sheath points - NPC weapons go through the client's rule
// (equipmentAttachmentsBySheath: put away at 26 / 27 / 28 / 30..33 by Item.dbc SheatheType; 12 the back for
// wowee.cfg rangedback) - older paks lack them: a put-away NPC weapon is not drawn there (attach_missing nopoint).
inline constexpr uint16_t kCreaturePointIds[] = {0, 1, 2, 5, 6, 7, 8, 11, 12, 26, 27, 28, 30, 31, 32, 33};
// The vehicle seats - every VehicleSeat.AttachmentID of the 3.3.5a DBC not among kCreaturePointIds
// (3, 4, 9, 10, 13..16, 19..21). Baked into "p_" only (their bones are not added to "n_": a seat whose bone is not
// already in the skin sits at the Stand pose); an older pak lacks them - the passenger then sits at the seat's offset
// from the vehicle's feet + its rider height (WorldScreen::classBarSeatPos).
inline constexpr uint16_t kVehicleSeatPointIds[] = {3, 4, 9, 10, 13, 14, 15, 16, 19, 20, 21};
constexpr uint16_t kCreatureEquip = 1u << 2;  // CreatureDisplayFlags: a CreatureDisplayEquip follows the mask
constexpr uint8_t kBatchCape = 2;             // WCDI active[] value

struct CreatureDisplayEquip {
    char magic[4];          // "WCDE"
    uint8_t race, sex;      // CreatureDisplayInfoExtra DisplayRaceID / DisplaySexID
    uint16_t reserved;
    uint32_t item[11];      // ItemDisplayInfo ids in the DBC's slot order: 0 helm, 1 shoulder, 2 shirt,
                            // 3 chest, 4 belt, 5 legs, 6 feet, 7 wrist, 8 hands, 9 tabard, 10 cape
};
static_assert(sizeof(CreatureDisplayEquip) == 52, "WCDE block is 52 bytes");

// The equipment block of a WCDI blob (nullptr when the display has none or the blob is short).
inline const CreatureDisplayEquip* creatureDisplayEquip(const void* blob, uint32_t size) {
    if (!blob || size < sizeof(CreatureDisplayHeader)) return nullptr;
    const auto* h = static_cast<const CreatureDisplayHeader*>(blob);
    if (!(h->flags & kCreatureEquip)) return nullptr;
    const uint32_t off = (static_cast<uint32_t>(sizeof(CreatureDisplayHeader)) + h->batchCount + 3u) & ~3u;
    if (off + sizeof(CreatureDisplayEquip) > size) return nullptr;
    const auto* e = reinterpret_cast<const CreatureDisplayEquip*>(static_cast<const uint8_t*>(blob) + off);
    return (e->magic[0] == 'W' && e->magic[1] == 'C' && e->magic[2] == 'D' && e->magic[3] == 'E') ? e : nullptr;
}

// Creature anim. "n_<n>" in data/creatures.pak: WSKN of the model
// "m_<n>" (same index; the static WMDL stays for wowee.cfg npcanim=0 and older
// runtimes - a pak without "n_" draws as before). Skeleton, sub-batches and
// sequences exactly as the characters' n_ (above). Its batches are the WMDL's
// in the same order - LOD 0, then LOD 1 - so the WCDI batch mask and slot
// textures apply unchanged. With kSkinHasLod in SkinHeader::flags,
// SkinHeader::reserved[0] = the LOD 0 batch count (else every batch is LOD 0).
constexpr uint16_t kSkinHasLod = 1u << 0;

// Sounds. Footfall ($FSD) animation events of a WSKN / WSKA blob
// (psp_char_conv n_ / o_, psp_creature_conv n_; tools/psp/converter
// fsd_bake.hpp): with kSkinHasEvents in SkinHeader::flags,
// SkinHeader::reserved[1] = offset of a SkinEventTable appended after
// everything else (4-aligned):
//   SkinEventTable, uint16_t first[sequenceCount + 1], uint32_t timeMs[count]
// Sequence s owns timeMs[first[s] .. first[s + 1]) (ms from its start,
// ascending) - upstream M2Model::footstepEventTimes of the baked sequences.
// Older paks: flag clear, no table (the runtime falls back to upstream's
// generic biped beats for the own player).
constexpr uint16_t kSkinHasEvents = 1u << 1;

struct SkinEventTable {
    char magic[4];          // "WFSD"
    uint16_t sequenceCount; // == SkinHeader::sequenceCount
    uint16_t count;         // time stamps in all
};
static_assert(sizeof(SkinEventTable) == 8, "WFSD header is 8 bytes");

// The footfalls of sequence `seq` (nullptr, n = 0 when none). `blobSize` = the
// blob's bytes (pass 0xFFFFFFFF when unknown: the table is then only checked
// against itself).
inline const uint32_t* skinFootsteps(const SkinHeader* h, uint32_t blobSize, uint32_t seq, uint32_t& n) {
    n = 0;
    if (!h || !(h->flags & kSkinHasEvents) || seq >= h->sequenceCount) return nullptr;
    const uint32_t off = h->reserved[1];
    if ((off & 3u) || off < sizeof(SkinHeader) || off > (16u << 20) || off > blobSize ||
        blobSize - off < sizeof(SkinEventTable))
        return nullptr;
    const auto* base = reinterpret_cast<const uint8_t*>(h);
    const auto* t = reinterpret_cast<const SkinEventTable*>(base + off);
    if (t->magic[0] != 'W' || t->magic[1] != 'F' || t->magic[2] != 'S' || t->magic[3] != 'D' ||
        t->sequenceCount != h->sequenceCount)
        return nullptr;
    const uint32_t firstBytes = (static_cast<uint32_t>(t->sequenceCount) + 1u) * 2u;
    const uint32_t timesOff = off + static_cast<uint32_t>(sizeof(SkinEventTable)) + ((firstBytes + 3u) & ~3u);
    if (timesOff > blobSize || (blobSize - timesOff) / 4u < t->count) return nullptr;
    const auto* first = reinterpret_cast<const uint16_t*>(base + off + sizeof(SkinEventTable));
    if (first[t->sequenceCount] != t->count || first[seq] > first[seq + 1] || first[seq + 1] > t->count)
        return nullptr;
    n = static_cast<uint32_t>(first[seq + 1] - first[seq]);
    return n ? reinterpret_cast<const uint32_t*>(base + timesOff) + first[seq] : nullptr;
}

} // namespace wowee::rendering::psp
