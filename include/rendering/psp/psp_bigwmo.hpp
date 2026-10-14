// Big WMOs: a big WMO on the PSP - capitals and the other WMOs over the doodad bake's
// budget, baked by psp_model_conv into data/bigwmo/<map>_<stem>.pak
// (include/rendering/psp/psp_bigwmo_format.hpp).
//
// One BigWmo = one placed WMO (a "wbig" placement, or any matrix: the transports may place and move one). It owns:
//  - the pak's "!wmo" record (groups, portals, cells, chunk table: ~70-150 KB) and one file descriptor (read by the
//    caller's StreamLoader thread through pakRead, released with close());
//  - resident blobs: chunk meshes (full "c_", guard-band split "n_", far "l_"), cell shells "s_", chunk collision
//    "q_", textures "x_", cell doodad lists "dd_" - under one hard budget (bytes), least recently used evicted;
//  - per frame: the camera's group, the portal walk (psp_bigwmo_vis.hpp - the same code as the PC route check),
//    the level of detail per cell / chunk, the GE draw (opaque and alpha-tested sorted by texture, then blended
//    far to near), collision (floor / walls / camera ray in world space over the resident "q_").
// Main thread only. update() plans what should be resident and installs what the loader read; frees happen only
// there (after frameEnd's sceGuSync), never in draw.
#pragma once

#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_bigwmo_format.hpp"
#include "rendering/psp/psp_bigwmo_vis.hpp"
#include "rendering/psp/psp_terrain.hpp"  // TerrainCamera

#include <pspgu.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace wowee::rendering::psp {

struct BigWmoConfig {
    uint32_t budgetBytes = 3u << 20;  // wowee.cfg wmobudget (KB): meshes + shells + collision + textures
    float collisionYd = 40.0f;        // wmocolyd: "q_" of chunks within this (XY) of the player or the camera
    float doodadYd = 160.0f;          // wmodoodadyd: "dd_" of cells within this of the camera
    float prefetchSec = 1.5f;         // wmoprefetch: also plan from where the camera will be (velocity x this)
    uint32_t texLodBytes = 16384;     // wmotexlod: drop mip 0 of textures whose level 0 is this big (0 off)
    bool portals = true;              // wmoportals=0: every group visible (A/B)
    // wmovis: 2 walk + exterior seeds (default: the route's fewest holes), 4 walk-outdoor (kVisWalkOutdoor), 1 walk
    // only, 3 upstream as is.
    int visMode = PortalVisibility::kVisWalkExt;
    BigWmoLod lod;                    // wmolod / wmoshell / wmoview
};

struct BigWmoStats {
    int camGroup = -1;
    bool outside = true;
    uint32_t visibleGroups = 0, walked = 0;
    uint32_t chunks = 0, nearChunks = 0, lodChunks = 0, shellCells = 0, missing = 0;
    uint32_t triangles = 0, draws = 0, binds = 0, matrices = 0;
    uint32_t residentBytes = 0, textureBytes = 0, collisionBytes = 0, headerBytes = 0;
    uint32_t residentBlobs = 0, reads = 0, evictions = 0, overBudget = 0;
    uint32_t cpuUs = 0;
    // Chunks not drawn this frame because their sphere is not guard-band safe and their "n_" is not
    // resident (the unsplit "c_" / "l_" would break the GE rule); blobs that failed their check (never read again).
    uint32_t guardSkipped = 0, badBlobs = 0;
    // Draws of interior-group batches (their baked light, no day/night tint) / of the tinted ones.
    uint32_t interiorDraws = 0, tintedDraws = 0;
};

class BigWmo {
public:
    // A read the caller should perform (offset/size in this pak; `key` comes back in install()).
    struct Request {
        uint32_t offset, size, key;
    };

    BigWmo() = default;
    ~BigWmo() { close(); }
    BigWmo(const BigWmo&) = delete;
    BigWmo& operator=(const BigWmo&) = delete;

    // Opens the pak (one descriptor), reads "!wmo" (TOC record 0). `matrix` = BigWmoPlacement::matrix (world = M *
    // local, 3x4 column-major). stamp: the doodads pak's "wbig" stamp (0 = unknown: no doodads).
    bool open(const char* absPath, const float matrix[12], uint32_t uniqueId, uint16_t doodadSet, uint32_t stamp);
    // Why the last open() failed - the file is missing or its "!wmo" is bad (for good), or a
    // descriptor / memory / read failed (worth another try later: at world entry 3-4 descriptors are free).
    enum OpenError : uint8_t { kOpenOk = 0, kOpenMissing, kOpenBad, kOpenRetry };
    OpenError openError() const { return openErr_; }
    // The sceIoOpen error of the last failed open / ensureFd (0x80020320 = no descriptor left).
    int fdError() const { return fdErr_; }
    void close();
    bool isOpen() const { return tables_.h != nullptr; }
    int fd() const { return fd_; }
    // Hardware 110-114: 3-4 free file descriptors at world entry. The descriptor goes back while nothing is read
    // (releaseFd, only with no read in flight) and comes back for the next reads (ensureFd: one open, 17-34 ms).
    bool ensureFd();
    void releaseFd();
    uint32_t fileId() const { return fileId_; }
    const char* name() const { return name_; }
    const BigWmoTables& tables() const { return tables_; }
    uint32_t uniqueId() const { return uniqueId_; }
    uint16_t doodadSet() const { return doodadSet_; }
    bool doodadsUsable() const { return doodadsOk_; }
    // wmo-transports: move it (world = M * local); the next draw / collision use it.
    void setPlacement(const float matrix[12]);
    const glm::mat4& placement() const { return M_; }
    // World box of the placed WMO.
    void worldBox(glm::vec3& lo, glm::vec3& hi) const;

    // Main thread, once per frame before draw: plans residency from the camera (and the player), evicts over the
    // budget, and returns the reads to issue (nearest first, at most maxRequests, none already in flight).
    void update(const glm::vec3& camWorld, const glm::vec3& playerWorld, float dtSec, uint32_t frame,
                std::vector<Request>& out, uint32_t maxRequests);
    // A read finished (blob from the loader, 64-byte aligned, D-cache written back; ownership taken; nullptr = the
    // read failed: the request may be issued again later).
    // A failed read is asked again only ~5 s later; a blob that fails its check never again (a
    // corrupt record would otherwise be read every frame).
    void install(uint32_t key, void* blob, uint32_t size);
    // A request handed out by update() but not issued (the loader's ring full, no descriptor): no
    // read failed, it is asked again by the next plan (no back-off).
    void cancel(uint32_t key);
    // Reads the caller has issued and not yet installed.
    uint32_t inFlight() const { return inFlight_; }

    // GE: opaque + alpha-tested, then blended. The terrain has set projection/view for this camera.
    void draw(const TerrainCamera& cam, uint32_t fogColorABGR);
    const BigWmoStats& stats() const { return stats_; }
    // The groups the last draw saw (bit per group) - the doodads' filter (ModelRenderer::groupFilter).
    const PortalVisibility& visibility() const { return vis_; }

    // Doodads: the resident "dd_" cells changed since the last call (the caller rebuilds its placement set).
    bool takeDoodadsChanged() {
        const bool c = doodadsChanged_;
        doodadsChanged_ = false;
        return c;
    }
    // Every resident doodad record (set 0 + the placement's set), WMO-local.
    template <typename Fn>
    void forDoodads(Fn&& fn) const {
        for (const Res& r : cellDoodads_) {
            if (!r.blob) continue;
            const auto* d = static_cast<const BigWmoDoodad*>(r.blob);
            for (uint32_t i = 0; i < r.size / sizeof(BigWmoDoodad); ++i)
                if (d[i].set == 0 || d[i].set == doodadSet_) fn(d[i]);
        }
    }

    // Collision in world space (resident "q_" only), the same rules as ModelRenderer for buildings (upstream
    // WMORenderer): floor = highest walkable (normal z >= cos 50) under (x, y) at or below probeZ; walls push a
    // cylinder r 0.5 h 2 standing on feetZ (stepped over below feetZ + 1); camera ray against everything.
    bool floorHeight(float x, float y, float probeZ, float& outZ) const;
    // The teleport landing hold (ui_teleport_hold.hpp): the collision ("q_") of every chunk whose box lies
    // within radiusYd (XY) of world (x, y) is resident - the floor a landing player needs. wanted / resident: counts.
    bool collisionReadyAt(float x, float y, float radiusYd, uint32_t& wanted, uint32_t& resident) const;
    void collideWalls(glm::vec3& to, float feetZ) const;
    float raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const;

    BigWmoConfig cfg;

private:
    struct Res {               // a resident blob
        void* blob = nullptr;
        uint32_t size = 0;
        uint32_t lastUse = 0;  // frame
        bool requested = false;
        bool wanted = false;
        float prio = 0.0f;     // distance: smaller first
        bool bad = false;          // failed its check - never read again
        uint32_t retryAtMs = 0;    // a failed read - not before this (pp::logUptimeMs)
    };
    struct Tex {
        platform::psp::Texture t;
        uint32_t bytes = 0;
        uint32_t lastUse = 0;
        bool requested = false, ok = false, wanted = false;
        float prio = 0.0f;
        bool bad = false;          // as Res
        uint32_t retryAtMs = 0;
    };
    // key = kind << 24 | index
    enum Kind : uint32_t { kMesh = 1, kNear, kLod, kCol, kShell, kTex, kDoodads };

    Res* resOf(uint32_t kind, uint32_t index);
    void want(uint32_t kind, uint32_t index, float prio, uint32_t frame);
    bool meshReady(const Res& r) const;  // resident and every texture it binds resident
    void freeRes(Res& r, uint32_t kind);
    int findGroup(const glm::vec3& local) const;

    BigWmoTables tables_;
    void* header_ = nullptr;
    uint32_t headerSize_ = 0;
    int fd_ = -1;
    uint32_t fileId_ = 0;
    OpenError openErr_ = kOpenOk;
    int fdErr_ = 0;
    uint32_t fdRetryAtMs_ = 0;     // ensureFd failed - no new sceIoOpen before this
    char name_[48] = {};
    char path_[128] = {};
    uint32_t uniqueId_ = 0;
    uint16_t doodadSet_ = 0;
    bool doodadsOk_ = false;
    glm::mat4 M_{1.0f}, Minv_{1.0f};
    glm::mat3 A_{1.0f}, Ai_{1.0f};
    std::vector<Res> mesh_, near_, lod_, col_, shell_, cellDoodads_;
    std::vector<Tex> tex_;
    std::vector<std::vector<uint32_t>> groupChunks_;  // chunk ids per group (camera group test)
    std::vector<uint32_t> colResident_;                // chunks with "q_" resident (collision queries)
    std::vector<uint8_t> cellShellMode_;               // hysteresis: 1 = drawing the shell
    std::vector<uint32_t> groupSeen_;                  // frame a group was last visible (interiors stay wanted)
    struct PlanItem {
        float prio;
        uint32_t bytes;
        uint32_t key;
    };
    std::vector<PlanItem> planScratch_;
    std::vector<uint32_t> reqQueue_;                   // keys to read, nearest first (the last plan)
    size_t reqNext_ = 0;
    bool planDirty_ = true;
    uint32_t curFrame_ = 0;                            // the caller's frame (update)
    glm::vec3 playerLocal_{0.0f};
    uint32_t residentBytes_ = 0, textureBytes_ = 0, collisionBytes_ = 0;
    uint32_t inFlight_ = 0;
    bool doodadsChanged_ = false;
    glm::vec3 lastCam_{1e9f}, prevCam_{0.0f}, vel_{0.0f};
    uint32_t planFrame_ = 0;
    PortalVisibility vis_;
    BigWmoStats stats_;
    struct DrawItem {
        const void* blob;   // WBCK
        uint16_t texture;   // sort key
        uint16_t cell;
        uint16_t batch;
        uint8_t blend;
        uint8_t interior;   // a chunk of an interior group (MOGP 0x2000) - its baked light only
        float dist;
    };
    std::vector<DrawItem> items_;
    std::vector<ScePspFMatrix4> cellMtx_;
    std::vector<uint32_t> cellMtxFrame_;
    uint32_t drawFrame_ = 0;
};

// FNV of (WMO uniqueId, part) | 0x80000000 - the ids psp_model_conv gives WMO parts (partId), so a big WMO's doodads
// never meet an MDDF uniqueId in ModelRenderer's dedupe.
inline uint32_t bigWmoPartId(uint32_t wmoUniqueId, uint32_t part) {
    uint32_t h = 2166136261u;
    for (uint32_t v : {wmoUniqueId, part})
        for (int b = 0; b < 4; ++b) h = (h ^ ((v >> (b * 8)) & 0xFFu)) * 16777619u;
    return h | 0x80000000u;
}

}  // namespace wowee::rendering::psp
