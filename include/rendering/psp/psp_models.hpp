// PSP static models: the M2 doodads baked by tools/psp/converter
// (psp_model_conv) into data/doodads_<map>.pak, drawn on the GE after the
// terrain with the same camera-relative view.
//
// Format: include/rendering/psp/psp_model_format.hpp. Blobs stay where
// Pak::load put them (64-byte aligned, D-cache written back): the GE reads
// vertices, indices and textures from there, nothing is copied or rewritten.
//
// Per frame: a 64-yard grid is culled first (distance + frustum per cell),
// then the doodads of visible cells: distance cull without sqrt (larger
// models and foliage reach further, ground clutter stays close), frustum,
// LOD 1 skin beyond a size-scaled distance, then two passes - opaque and alpha-tested, then blended batches
// with depth writes off - and a triangle cap as a safety net.
#pragma once

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_model_format.hpp"
#include "rendering/psp/psp_terrain.hpp"  // TerrainCamera

#include <pspgu.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace wowee::rendering::psp {

struct ModelStats {
    uint32_t instances = 0;
    uint32_t drawn = 0;
    uint32_t culled = 0;
    uint32_t lowLod = 0;
    uint32_t batches = 0;
    uint32_t triangles = 0;
    uint32_t models = 0;
    uint32_t textures = 0;
    uint32_t memoryKb = 0;
    uint32_t texLodDropped = 0;  // Textures loaded at half size (m2texlod)
    // Forest LOD (psp_impostors.cpp)
    uint32_t lod2 = 0;           // instances drawn with the far LOD 2 mesh
    uint32_t impostors = 0;      // instances drawn as impostor quads
    uint32_t impostorDraws = 0;  // GE draws of the impostor pass (one per model)
    uint32_t impostorModels = 0; // models with an impostor loaded
    uint32_t guardNear = 0;      // GE audit: split LOD 0 for the guard band past the 12-yd rule (guardlod)
};

// Forest LOD (docs/PSP_PORT_ARCHITECTURE.md): wowee.cfg keys
// treelod, treelod2dist, impostors, impostordist; the Debug window cycles them.
// Distances are yards from the instance's bounding sphere.
struct ForestLod {
    int treeLod = 2;                // 0 trees in full (no M2 low skin), 1 the first LOD rule (+ low skin), 2 + far LOD 2
    float lod2Distance = 50.0f;
    int impostors = 1;              // 0 off, 1 quads, 2 GE sprites, 3 quads tinted red (debug)
    float impostorDistance = 90.0f;
};

class ModelRenderer {
public:
    ModelRenderer() = default;
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    // Adds the doodads of tile "d_<x>_<y>" and loads the models/textures they
    // use (a doodad already added from a neighbour tile is skipped).
    bool loadTile(platform::psp::Pak& pak, int tileX, int tileY);
    // Game objects: placements the caller built, as pseudo-tile (tileX, tileY) - outside the 0..63
    // grid - in a WDOD blob of its own; unloadTile() drops it like a tile. Load the new set before dropping the old
    // one: the models both use stay resident.
    bool setPlacements(platform::psp::Pak& pak, int tileX, int tileY, const DoodadPlacement* p, uint32_t n);
    // transports: the placements of pseudo-tile (tileX, tileY) moved - the same count and
    // order setPlacements was given (moving transports: a ship's parts every frame). Copied over the tile's blob
    // (the instances point into it), the grid rebuilt now. False when the tile or the count differs.
    bool movePlacements(int tileX, int tileY, const DoodadPlacement* p, uint32_t n);
    // The model/texture tables at the pak's size up front (no reallocation under the grid's pointers).
    void reserveTables(const platform::psp::Pak& pak);
    bool vramOn = true;           // False = models stay in RAM (a second renderer must not reset eDRAM)
    const char* logTag = "m2";    // The grid log line's name
    void clear();

    // The terrain has set projection/view for this camera already.
    void draw(const TerrainCamera& cam, uint32_t fogColorABGR);
    // game object use: the placement `uniqueId` drawn again additively (the client's mouseover
    // highlight of an interactive object); after draw(). False: not drawn.
    bool drawHighlight(uint32_t uniqueId, const TerrainCamera& cam, uint32_t fogColorABGR, uint32_t addABGR);
    const ModelStats& stats() const { return stats_; }

    // Collision (M8+, upstream WMORenderer/M2Renderer rules).
    // Highest walkable floor (normal z >= cos 50 deg) under (x, y) at or below
    // probeZ, from WMO groups and M2 collision meshes.
    bool floorHeight(float x, float y, float probeZ, float& outZ) const;
    // Pushes `to` out of blocking walls: a cylinder of radius 0.5, height 2
    // standing on feetZ; geometry lower than feetZ + 1 is stepped over.
    void collideWalls(glm::vec3& to, float feetZ) const;
    uint32_t collisionKb() const { return collisionBytes_ / 1024u; }
    // Camera ray against buildings only (upstream raycasts WMOs, not M2s):
    // distance to the first hit along the unit `dir`, or maxDist.
    float raycastBuildings(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const;

    ForestLod forest;
    void loadForestConfig();        // wowee.cfg -> forest (psp_impostors.cpp)
    void logForestStats() const;    // "m2_forest ..." (world_frame cadence)

    float viewDistance = 160.0f;    // base, scaled per model size
    // big WMOs: a placement with reserved != 0 is a big WMO's doodad (ui_world_bigwmo.cpp), drawn only
    // while bit (reserved - 1) of groupFilter is set - its WMO group is visible through the portals (the client
    // draws a group's MODR doodads with the group). nullptr = no filter; collision ignores it.
    const uint32_t* groupFilter = nullptr;
    uint32_t groupFilterBits = 0;
    bool groupHidden(uint16_t reserved) const {
        if (!groupFilter || !reserved) return false;
        const uint32_t g = reserved - 1u;
        return g >= groupFilterBits || !((groupFilter[g >> 5] >> (g & 31u)) & 1u);
    }
    uint32_t texLodBytes = 16384;   // m2texlod - drop mip 0 of textures whose level 0 is this big (0 off)
    uint32_t triangleCap = 150000;  // safety net only (60k cut far trees with strips + near LODs)
    // GE audit: near-split models (kModelNearSplit) also draw their split LOD 0 when their
    // bounding sphere may reach past the GE's 4096-px window or the near plane (ge::sphereSafe) - the unsplit
    // mesh's long triangles are dropped whole there (a long wall 12+ yd beside the camera). cfg guardlod
    // (1; 0 = the 12-yd rule alone, as before). drawFast only (cpufast=0 keeps the older pass).
    int guardLod = 1;

private:
    struct Model {
        void* blob = nullptr;
        const ModelHeader* h = nullptr;
        const ModelVertex* verts = nullptr;
        const uint16_t* indices = nullptr;
        const ModelBatch* batches = nullptr;
        void* colBlob = nullptr;                 // "k_<n>" WCOL, or null
        const CollisionHeader* col = nullptr;
        void* impBlob = nullptr;                 // "i_<n>" WIMP, or null
        const ImpostorHeader* imp = nullptr;
        uint32_t instances = 0;                  // placements using it (VRAM priority)
        bool tried = false;
        uint8_t blendMask = 0;  // frame CPU: bit n = LOD n has blended batches (fills padding)
        bool bad = false;       // failed its checks (broken data, not a short heap)
    };
    struct Tex {
        platform::psp::Texture t;
        uint32_t bytes = 0;  // What it holds (less than the entry after textureDropTopMip)
        bool tried = false;
        bool ok = false;
        bool missing = false;  // A drawn batch found it not loaded (tex_missing, retry queued)
        bool logged = false;   // tex_missing said once per texture
        uint8_t retries = 0;   // retries handed out (takeMissingTextures), at most kTexRetries
    };
    static constexpr uint8_t kTexRetries = 5;
    // Static per-doodad data, grouped by grid cell (hardware: the
    // flat loop over ~4.7k doodads cost 5 ms of CPU even at 40 yd).
    struct Inst {
        float x, y, z, r;         // world bounding sphere
        float reach;              // view-distance factor (size, foliage, clutter)
        const DoodadPlacement* p;
        const Model* m;
    };
    struct Cell {
        float x, y, z, r;         // sphere around all its doodads' spheres
        float maxReach;
        uint32_t first, count;    // range in insts_
    };
    struct Visible {
        const DoodadPlacement* p;
        const Model* m;
        float dist;
        uint8_t lod;
        ScePspFMatrix4 mtx;  // camera-relative model matrix, built once per frame
    };

    const Model* model(platform::psp::Pak& pak, uint16_t id);
    const platform::psp::Texture* texture(uint16_t id) const;
    // allowLod: m2texlod may halve it; impostor atlases never (their cell grid).
    bool loadTexture(platform::psp::Pak& pak, uint16_t id, bool allowLod = true);
    void drawModel(const Visible& v, bool blendPass);
    // PSP (an emulator run: black squares in a village): false = the batch's texture is not loaded (its
    // read failed for want of memory) - the batch is skipped, not drawn untextured (black); the texture is queued for
    // a retry (takeMissingTextures) and logged once (tex_missing model= tex=).
    bool setBatchState(const Model& m, const ModelBatch& b);
    void drawBatch(const Model& m, const ModelBatch& b);
    void moveToVram();
    void buildGrid();
    // Forest LOD (psp_impostors.cpp)
    uint8_t pickLod(const Inst& in, float dist) const;
    bool pickImpostor(const Inst& in, float dist);
    void loadImpostor(platform::psp::Pak& pak, uint16_t id, Model& m);
    void drawImpostors(const TerrainCamera& cam);

    std::vector<Model> models_;
    std::vector<Tex> textures_;
    std::vector<void*> tileBlobs_;
    std::vector<const DoodadPlacement*> instances_;
    std::unordered_set<uint32_t> seen_;
    std::vector<Inst> insts_;
    std::vector<Cell> cells_;
    bool gridDirty_ = false;
    std::vector<Visible> visible_;
    std::vector<uint32_t> order_;  // opaque pass: visible_ indices grouped by model and LOD
    std::vector<const Inst*> impostorList_;  // This frame's far trees
    uint32_t memoryBytes_ = 0;
    uint32_t collisionBytes_ = 0;
    std::vector<uint16_t> texMissing_;  // Queued for a retry (takeMissingTextures)
    uint32_t texMissingLogged_ = 0;
    ModelStats stats_;

    // Tile streaming (src/ui/psp/ui_world_streaming.cpp). Everything
    // below runs on the main thread between frames (update), never in draw:
    // frameEnd's sceGuSync has finished the GE's reads of the last frame.
public:
    // Grows the model/texture tables to every id the pak holds, once, so no
    // later load reallocates them (Inst pointers stay valid between grid
    // rebuilds), and switches eDRAM placement to incremental (streamVramStep,
    // vramFree on eviction) instead of vramReset + refill at each grid build.
    void streamBegin(const platform::psp::Pak& pak);
    bool hasTile(int tileX, int tileY) const;
    bool modelKnown(uint16_t id) const;    // loaded or tried: model() would not read it
    bool textureKnown(uint16_t id) const;  // likewise for loadTexture()
    // Drops tile "d_<x>_<y>": instances are rebuilt from the other loaded
    // tiles by loadTile's rules (a doodad on a border stays while any tile
    // holding it is loaded - upstream TerrainManager::unloadTile drops it
    // with the tile that placed it), models no instance uses are freed with
    // their collision mesh and eDRAM copy, textures no resident model uses
    // are freed. The pak only gives entry sizes. Returns the bytes freed.
    uint32_t unloadTile(const platform::psp::Pak& pak, int tileX, int tileY);
    // A model / texture whose load failed (model() and loadTexture() mark it
    // tried before reading - a short heap or a failed read) may be read again: the next tile or set that places it
    // asks again (was: never again in this renderer - one short heap left a transport without its hull / deck for
    // the session). unloadTile() calls it; a renderer that never unloads may call it before placing again.
    void forgetFailed();
    // The textures drawn batches found missing since the last call (at most `max`), made loadable
    // again (tried = false) - the tile streamer reads them with a top-up job and installs them with retryTextures.
    uint32_t takeMissingTextures(std::vector<uint16_t>& out, uint32_t max);
    // Loads the given textures that are still not resident (parked blobs first: the streamer read them ahead).
    uint32_t retryTextures(platform::psp::Pak& pak, const std::vector<uint16_t>& ids);
    uint32_t texMissingCount() const { return texMissingLogged_; }  // distinct textures found missing (session)
    uint32_t texMissingPending() const { return static_cast<uint32_t>(texMissing_.size()); }
    // Rebuilds the cull/collision grid now if a load or unload changed the
    // instances (draw() would do it lazily, but collision queries read it).
    void flushGrid();
    // Copies the vertices of the most used models not in eDRAM yet, up to
    // maxBytes (at least one model); returns the bytes copied, 0 when nothing
    // is left or nothing fits.
    uint32_t streamVramStep(uint32_t maxBytes);
    uint32_t memoryKb() const { return memoryBytes_ / 1024u; }

    // ---- memory budget (psp_tile_budget.hpp) --------------------
    // A tile by budget: its placements nearest (x, y) first; before each model not resident yet the heap must still
    // give reserveKb + that model (TileBudgetMeter: one probe per 512 KB read). When it does not, the placements left
    // are deferred: kept with the tile (hasTile() is true), placed by resumeTile() when there is room - the tile
    // streamer's top-up jobs read their models ahead, nearest first. parkedOnly: a model the loader thread did not
    // read ahead (pakPrefetchPut) is deferred too - never read from the card here (the streamer's install). A budget
    // of reserveKb 0 without parkedOnly is the plain loadTile. PSP only (the client loads a whole ADT's doodads).
    struct LoadBudget {
        float x = 0.0f, y = 0.0f;  // server coordinates: where the player stands
        uint32_t reserveKb = 0;    // KB the heap must still give besides the model (0 = no probe)
        bool parkedOnly = false;
        bool nearRule = false;     // Doodads in view - budgetNearProbe (the guard block may be used)
        uint32_t stepKb = 512;     // KB read between two probes (TileBudgetMeter::stepBytes)
    };
    // The heap probe (bytes in one block now?) - a volatile malloc / free; host tests put their own here.
    using BudgetProbe = bool (*)(uint32_t bytes);
    static BudgetProbe budgetProbe;
    static BudgetProbe budgetNearProbe;  // Without the guard block (LoadBudget::nearRule)
    bool loadTile(platform::psp::Pak& pak, int tileX, int tileY, const LoadBudget& budget);
    // The tile's deferred placements by the same rules; returns how many were placed.
    uint32_t resumeTile(platform::psp::Pak& pak, int tileX, int tileY, const LoadBudget& budget);
    uint32_t deferredCount(int tileX, int tileY) const;
    uint32_t deferredTotal() const;
    // PSP (an emulator run: a village never "full" - 2 tiles kept deferred placements on a 41 MB
    // heap): the tile's deferred placements the player could SEE from (x, y) - nearer than viewDistance x the size
    // factor of the cull (max(1, r / 12) x 1.6: the largest a model's flags give, the model is not read yet). The
    // ones farther are not on the screen: deferring them costs nothing visible.
    uint32_t deferredInView(int tileX, int tileY, float x, float y) const;
    // The models the tile's deferred placements need that are not resident (nor failed), nearest (x, y) first,
    // until their m_ + k_ + i_ bytes pass maxBytes (at least one).
    void deferredModels(const platform::psp::Pak& pak, int tileX, int tileY, float x, float y, uint32_t maxBytes,
                        std::vector<uint16_t>& out) const;
    struct BudgetStats {
        uint32_t stops = 0;     // calls the heap stopped
        uint32_t deferred = 0;  // placements deferred by those calls
        uint32_t resumed = 0;   // deferred placements placed later
        uint32_t probes = 0;    // heap probes
    };
    const BudgetStats& budgetStats() const { return budgetStats_; }
    // ---- memory budget (end) ----

private:
    // memory budget: a tile's placements not placed yet (indices into its "d_" blob; flag per placement).
    struct Deferred {
        uint32_t key = 0;
        std::vector<uint32_t> idx;
        std::vector<bool> flag;
    };
    std::vector<Deferred> deferred_;
    BudgetStats budgetStats_;
    Deferred* deferredOf(uint32_t key);
    const Deferred* deferredOf(uint32_t key) const;
    const void* tileBlob(uint32_t key) const;
    // Places p[order[k]] by the budget; what it may not read now goes to `left`. Returns how many were placed.
    uint32_t placeBudgeted(platform::psp::Pak& pak, const DoodadPlacement* p, const std::vector<uint32_t>& order,
                           const LoadBudget& budget, std::vector<uint32_t>& left);
    uint32_t modelNeedBytes(const platform::psp::Pak& pak, uint16_t id, bool& parked) const;

    std::vector<uint32_t> tileKeys_;  // parallel to tileBlobs_: (x << 16) | y
    bool streamVram_ = false;
    bool vramFull_ = false;
    bool inVram(const Model& m) const;

    // Camera vs props. PSP: the camera ray against the collision
    // meshes (k_<n>) of props - tree trunks, rocks, lamp posts - whose
    // collision is at least minHeight yards tall (fences, crates and bushes
    // stay see-through). Upstream's camera tests WMOs only
    // (CameraController "Camera collision": raycastBoundingBoxes); the original
    // client stops it at doodads too (hardware: "the camera goes through
    // trees"). Same contract as raycastBuildings.
public:
    float raycastProps(const glm::vec3& origin, const glm::vec3& dir, float maxDist, float minHeight) const;

    // Frame CPU (wowee.cfg cpufast):
    // the same results with less frame CPU. Collision queries walk only the
    // cull grid's cells near the query (the cells on their 64-yd lattice,
    // conservative cell tests, doodads in the same order as the old scan of
    // insts_) and remember the last answers for the same input; the M2 pass
    // skips plane tests inside wholly visible cells, sorts 16-byte records
    // (same permutation), skips models without blended batches in the
    // blended pass and sends most opaque draws without a stall syscall.
private:
    struct VisFast {
        const Model* m;
        float dist;
        uint32_t mi;  // its matrix in mtxFast_
        uint8_t lod;
    };
    void drawOld(const TerrainCamera& cam, uint32_t fogColorABGR);   // the older pass (cpufast=0)
    void drawFast(const TerrainCamera& cam, uint32_t fogColorABGR);
    void drawBatchNoStall(const Model& m, const ModelBatch& b, bool flush);
    // Doodads (in insts_ order) of the cells whose lattice slots overlap the
    // box grown by gridSpill_, that cellOk accepts.
    template <class CellOk, class Fn>
    void forGrid(float x0, float y0, float x1, float y1, CellOk&& cellOk, Fn&& fn) const;
    bool rayGridOk(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const;
    std::vector<VisFast> visFast_;
    std::vector<ScePspFMatrix4> mtxFast_;
    struct ImpKey {
        const Model* m;
        const Inst* in;
    };
    std::vector<ImpKey> impKeys_;
    float gridMinX_ = 0.0f, gridMinY_ = 0.0f;
    int gridNx_ = 0, gridNy_ = 0, gridSpill_ = 0;
    std::vector<uint16_t> lattice_;  // gridNx_ * gridNy_: index into cells_ + 1 (0 empty); empty = no lattice
    uint32_t gridGen_ = 1;           // bumped by every grid rebuild / clear (memo key)
    struct FloorMemo {
        uint32_t key[3];
        uint32_t gen = 0;
        float z;
        bool found;
    };
    struct RayMemo {
        uint32_t key[9];
        uint32_t gen = 0;
        float best;
    };
    mutable FloorMemo floorMemo_[4];
    mutable RayMemo rayMemo_[6];
    mutable uint8_t floorMemoNext_ = 0, rayMemoNext_ = 0;
};

} // namespace wowee::rendering::psp
