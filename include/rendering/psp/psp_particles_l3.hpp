// EBOOT runtime for level-3 emitters and ribbons: emission, stepping, drawing (particle-runtime.md, RT-*).
// Clean-room implementation. No exceptions, no RTTI, no heap allocation per particle or per frame (one block at
// acquire()). GE output goes through FxGeOut; the PSP build maps it to sceGu (psp_particles_l3_gu.cpp), host
// tools record it.
//
// Integration (docs/PSP_PORT_ARCHITECTURE.md): this is the port's particle engine. Its pool, owners and
// ribbon lanes are shared with the older rules of psp_particles.cpp (upstream's and the port's, for blobs without
// the level-3 section and for wowee.cfg ptclevel3=0), which plug in through FxLegacyRules; the stand-ins of the
// clean side are replaced by the port's TerrainCamera, frameLight() and ge::sphereSafe. Additions are marked
// "Integration:".
#pragma once

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include "rendering/psp/psp_ge_limits.hpp"    // ge::sphereSafe
#include "rendering/psp/psp_lighting_ge.hpp"  // FrameLight, frameLight()
#include "rendering/psp/psp_particle_sim.hpp"
#include "rendering/psp/psp_spellfx_format.hpp"
#include "rendering/psp/psp_terrain.hpp"      // TerrainCamera

namespace wowee {
namespace platform {
namespace psp {
struct Texture;
}  // namespace psp
}  // namespace platform

namespace rendering {
namespace psp {

// ---- views of a blob (section 2) -------------------------------------------------------------------------------
struct FxView {
    const SpellFxHeader* h = nullptr;
    const SpellFxTrack* tracks = nullptr;
    const SpellFxEmitter* emitters = nullptr;
    const SpellFxRibbon* ribbons = nullptr;
    int ribbonCount = 0;
    uint8_t cls = kPtcSpell;
    float reach = 0.0f;
    const uint8_t* base = nullptr;
    const SpellFxL3Emitter* level3 = nullptr;       // nullptr without the level-3 section
    const SpellFxL3Ribbon* level3Ribbons = nullptr;
    // Integration: the mesh part of the blob (the older draw path) and the port's setter for a blob that already
    // passed spellFxValid (a v1 blob gets the reach max(4, radius + |center|)).
    const SpellFxGroup* groups = nullptr;
    const uint16_t* textures = nullptr;
    const SpellFxVertex* verts = nullptr;
    void set(const void* blob);
};
// Validates the blob (spellFxValid) and fills the view. Returns false for an invalid blob.
bool fxViewFromBlob(const void* data, uint32_t size, FxView& out);

// texture lookup by id; nullptr result = texture missing (Integration: the port's texture type)
typedef const platform::psp::Texture* (*FxTexLookup)(void* user, uint16_t id);

struct FxInstance {
    const FxView* fx = nullptr;
    glm::vec3 pos{0.0f};                                   // model origin, world
    float axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};           // column-major rotation x scale
    float scale = 1.0f;
    float timeMs = 0.0f;                                   // animation clock
    float* acc = nullptr;                                  // kFxMaxEmitters accumulators owned by the user
    int16_t lanes[kFxMaxRibbons] = {-1, -1, -1, -1};       // ribbon lanes, -1 none
    uint8_t owner = 0xFF;                                  // pool owner id (< 128)
    bool mesh = false;
    float rate = 1.0f;                                     // density x LOD factor
    int cap = 32;                                          // live particles per emitter
    float camDist = 0.0f;                                  // camera distance, 0 = none
    float alpha = 1.0f;                                    // model alpha
    FxTexLookup tex = nullptr;
    void* texUser = nullptr;
    // Integration: the older rules' colour x alpha (wowee.cfg spellfxpremul for spell effects) and an output flag
    // (something of the instance went to the GE this frame).
    bool premultiply = true;
    bool drawn = false;
};

struct FxFrameStats {
    uint32_t emitted = 0;    // births of the call (all sub-steps)
    uint32_t live = 0;       // live particles of the user after the call (last sub-step)
    uint32_t drawCalls = 0;
    uint32_t vertices = 0;
    // Integration: the port's frame lines (spellfx_frame, particles_frame, weaponfx_frame).
    uint32_t sprites = 0, draws = 0, ribbonVerts = 0, meshDraws = 0, listFull = 0;
};

// ---- GE output -------------------------------------------------------------------------------------------------
struct FxVertex {  // GE vertex: f32 u, v; u32 colour ABGR8888; f32 x, y, z (camera-relative)
    float u, v;
    uint32_t color;
    float x, y, z;
};
static_assert(sizeof(FxVertex) == 24, "GE vertex is 24 bytes");

enum class FxPrim : uint8_t { Sprites, Triangles, TriangleStrip };
enum class FxFactor : uint8_t { Fixed, SrcAlpha, OneMinusSrcAlpha, DstColor, SrcColor };
struct FxDrawState {
    FxPrim prim = FxPrim::Sprites;
    FxFactor src = FxFactor::Fixed, dst = FxFactor::Fixed;
    uint32_t srcFix = 0xFFFFFFFFu, dstFix = 0;
    bool alphaTest = false;  // pass when alpha > alphaRef
    uint8_t alphaRef = 0;
    int32_t textureId = -1;          // -1 = untextured (white)
    const void* texture = nullptr;   // lookup result for textureId
};
class FxGeOut {
public:
    virtual ~FxGeOut() {}
    // writable space for up to maxVerts vertices of the next draw call (nullptr: draw nothing)
    virtual FxVertex* reserve(int maxVerts) = 0;
    // one draw call over the first `count` reserved vertices (count >= 1)
    virtual void submit(const FxDrawState& st, FxVertex* verts, int count) = 0;
};
// RT-DRAW-3 blend / alpha test of a level-3 blend value (prim left as Sprites)
FxDrawState fxBlendState(uint8_t blend);

// RT-WORLD-1: phase of a world instance (ms) from its unique id
float fxWorldPhaseMs(uint32_t uniqueId);

// Helpers for tools (particle-matrix-tool.md): the emitter's values (RT-EMIT-1, held per stepMask) and its emitter
// frame (RT-FRAME) for an instance at its current timeMs.
ptcsim::EmitterValues fxEmitterValuesAt(const FxView& fx, int e, float timeMs);
void fxEmitterFrameAt(const FxInstance& I, int e, ptcsim::M34& F);

// Birth observer (tools): called after each birth with the new particle's slot.
typedef void (*FxBirthFn)(void* user, int slot, uint8_t owner, uint8_t emitter);

struct FxPoolConfig {  // shares (wowee.cfg keys), read once at acquire
    int spellShare = 0;
    int worldShare = 256;
    int weaponShare = 96;
    int lanes = 32;  // Integration: ribbon lanes (wowee.cfg particleribbons), at most ParticleEngine::kMaxLanes
};

class ParticleEngine;

// Integration: the older (non-level-3) particle rules of the port - upstream's rules (wowee.cfg ptclevel3=0) and
// the port's (a blob without the level-3 section) - on the engine's pool, owners and lanes. psp_particles.cpp on
// the PSP; none on the PC tools. `level3Rules`: the level-3 rules are on (the older rules take only the instances
// without the section, in the same 0.1-s sub-steps) or off (they take every instance).
class FxLegacyRules {
public:
    virtual ~FxLegacyRules() {}
    virtual void step(ParticleEngine& e, FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt,
                      bool level3Rules, FxFrameStats& st) = 0;
    // GE state, the WSFX meshes and the older rules' sprites (before the level-3 draws) ...
    virtual void drawBefore(ParticleEngine& e, FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam,
                            int maxPx, bool level3Rules, FxFrameStats& st) = 0;
    // ... the older rules' ribbons and the GE state back (after them).
    virtual void drawAfter(ParticleEngine& e, FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam,
                           bool level3Rules, FxFrameStats& st) = 0;
};

// ---- the level-3 particle engine -------------------------------------------------------------------------------
class ParticleEngine {
public:
    static constexpr int kOwners = 128;
    static constexpr int kMaxLanes = 32;
    static constexpr int kRingEdges = 64;
    static constexpr int kMaxEmitters = 16;

    struct OwnerState {
        glm::vec3 prevOrigin{0.0f};
        glm::vec3 lastDisp{0.0f};
        glm::vec3 inherit{0.0f};
        float clock = 0.0f;
        uint16_t burstBits = 0;
        bool valid = false;
    };
    struct Edge {  // 32 bytes
        glm::vec3 bottom;
        float age;
        glm::vec3 top;
        uint32_t color;
    };
    struct Lane {
        float acc;
        bool started;
        bool used;
        int16_t first;  // ring index of the oldest edge
        int16_t count;
        glm::vec3 prevSpine, prevY;
        Edge ring[kRingEdges];
        const Edge& edge(int i) const { return ring[(first + i) % kRingEdges]; }  // 0 = oldest
    };

    ParticleEngine();
    ~ParticleEngine();
    ParticleEngine(const ParticleEngine&) = delete;
    ParticleEngine& operator=(const ParticleEngine&) = delete;

    bool acquire(const FxPoolConfig& cfg);
    void release();
    int capacity() const { return cap_; }

    void setOutput(FxGeOut* out) { out_ = out; }
    // Tools (particle-matrix-tool.md TOOL-2): one generator per emitter index; nullptr = the engine's own.
    void setEmitterRngs(ptcsim::Rng* rngs16) { emitterRngs_ = rngs16; }
    void setBirthObserver(FxBirthFn fn, void* user) {
        birthFn_ = fn;
        birthUser_ = user;
    }
    ptcsim::Rng& rng() { return rng_; }

    void step(FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt, FxFrameStats& stats);
    void draw(FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam, int maxPx, FxFrameStats& stats);

    int allocLane();
    void freeLane(int lane);
    void freeOwner(uint8_t owner);
    // Owner `removed` is freed; owners in (removed, hi) move down by one and take their particles and state.
    void shiftOwnersDown(uint8_t removed, uint8_t hi);

    // inspection (tests, tools)
    const ptcsim::Particle* particles() const { return pool_; }
    const OwnerState& ownerState(int owner) const { return owners_[owner]; }
    const Lane* lane(int i) const { return (lanes_ && i >= 0 && i < laneCount_) ? &lanes_[i] : nullptr; }
    int liveCount(uint8_t lo, uint8_t hi) const;

    // Integration (psp_particles.cpp, PSP only): the reference-counted acquire of the users (spell effects, the
    // world's emitters, the weapons' glows) with the wowee.cfg shares, the shares and the rules switch.
    bool acquireShared();
    void releaseShared();
    bool ready() const { return pool_ != nullptr; }
    int spellShare() const { return shares_.spellShare; }
    int worldShare() const { return shares_.worldShare; }
    int weaponShare() const { return shares_.weaponShare; }
    uint32_t memoryBytes() const { return memoryBytes_; }
    bool level3Rules() const { return level3Rules_; }
    void setLevel3Rules(bool on) { level3Rules_ = on; }
    void setLegacy(FxLegacyRules* legacy) { legacy_ = legacy; }
    // Integration: the storage the older rules work on.
    ptcsim::Particle* pool() { return pool_; }
    int popSlot();
    void freeSlot(int slot);
    Lane* laneAt(int i) { return (lanes_ && i >= 0 && i < laneCount_) ? &lanes_[i] : nullptr; }
    int laneCount() const { return laneCount_; }

private:
    struct EmitCache {  // per (instance, emitter) values of the current (sub-)step
        float life, gravity;
        float ox, oy, oz;  // emitter world origin (RT-STEP-5)
    };

    void subStep(FxInstance* in, int n, uint8_t lo, uint8_t hi, int share, float dt, FxFrameStats& stats);
    void emitInstance(FxInstance& I, int idx, int share, float dt, FxFrameStats& stats);
    void updateRibbon(FxInstance& I, int r, Lane& L, float dt);
    // Integration: the level-3 part of draw() (buckets, emitters, ribbons).
    void drawLevel3(FxInstance* in, int n, uint8_t lo, uint8_t hi, const TerrainCamera& cam, int maxPx, FxFrameStats& stats);
    void drawEmitter(FxInstance& I, int e, const TerrainCamera& cam, float focal, int maxPx, FxFrameStats& stats);
    void drawRibbon(FxInstance& I, int r, const Lane& L, const TerrainCamera& cam, float focal, FxFrameStats& stats);
    ptcsim::Rng& emitterRng(int e) { return emitterRngs_ ? emitterRngs_[e] : rng_; }
    // Integration: an instance the level-3 rules run (the section is there and the rules are on).
    bool level3Instance(const FxInstance& I) const { return level3Rules_ && I.fx && I.fx->h && I.fx->level3; }

    void* block_ = nullptr;
    ptcsim::Particle* pool_ = nullptr;
    uint16_t* freeStack_ = nullptr;
    int freeTop_ = 0;
    int cap_ = 0;
    Lane* lanes_ = nullptr;
    int laneCount_ = 0;  // Integration: FxPoolConfig::lanes
    EmitCache* cache_ = nullptr;   // [kOwners][kMaxEmitters]
    uint16_t* bucketNext_ = nullptr;
    OwnerState owners_[kOwners];
    float ownerAcc_[kOwners][kMaxEmitters];  // used when an instance has no accumulator array
    uint16_t counts_[kOwners][kMaxEmitters];
    uint16_t bucketHead_[kOwners][kMaxEmitters];
    uint16_t bucketTail_[kOwners][kMaxEmitters];
    int16_t ownerInst_[kOwners];
    int userLive_ = 0;
    FxGeOut* out_ = nullptr;
    ptcsim::Rng rng_;
    ptcsim::Rng* emitterRngs_ = nullptr;
    FxBirthFn birthFn_ = nullptr;
    void* birthUser_ = nullptr;
    // Integration: the PSP's shared use (psp_particles.cpp).
    bool level3Rules_ = true;
    FxLegacyRules* legacy_ = nullptr;
    FxPoolConfig shares_;
    int refs_ = 0;
    uint32_t memoryBytes_ = 0;
};

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
