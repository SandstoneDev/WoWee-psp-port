// PSP terrain: ADT tiles baked offline by tools/psp/converter (psp_terrain_conv)
// into "WTER" blobs, drawn on the GE with per-chunk LOD and culling.
//
// Blob layout (little-endian):
//   header 32 B: "WTER" u16 version(1|2) u16 flags u16 tileX u16 tileY
//                u32 chunkCount(256) f32 unitSize u32 reserved[3]
//   chunk x256, row-major (chunk row 0..15, chunk col 0..15), 340 B each:
//     f32 baseX, baseY, baseZ  - server coords of vertex (row 0, col 0)
//     u16 holes, u16 flags
//     s16 h[81]   - (z - baseZ) * 32, vertex (r,c) at index r*9+c
//     u16 col[81] - RGB565 in GE 5650 order (R bits 0-4): v1 = albedo x light,
//                   v2 = light only (it modulates the chunk texture)
//   v2 (flags bit 0), after the chunk records, 256 x 4608 B, same order:
//     u16 clut[256] (5650), u8 texels[64*64] T8 swizzled - 64x64 per chunk
//     baked from the texture layers by psp_terrain_conv
//   v3: each chunk texture block also carries 32x32 and 16x16 mips (same
//     CLUT, swizzled) right after the 64x64 level: 5888 B per chunk
//   v4: then u16 detail[256]: "dt_<n>" WTEX of the chunk's dominant layer
//     (luminance around 0.5, last mip flat), drawn near the camera as a
//     modulate-2x pass repeated 4x per chunk; 0xFFFF = none
//   Vertex (r,c) sits at (baseX - r*unit, baseY - c*unit, baseZ + h/32):
//   server x points north, y west, z up; rows run south, columns run east.
//
// Rendering rules (see docs/PSP_PORT_ARCHITECTURE.md): camera-relative (GE
// matrices keep 24 of 32 float bits - world coordinates reach +-17000), s16
// positions are normalised by the GE (value/32768) so the model matrix
// carries the scale, vertex memory is never rewritten after submission.
#pragma once

#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_ge_limits.hpp"        // ge::kWorldFovYDeg
#include "rendering/psp/psp_terrain_pool.hpp"    // the terrain's fixed pool (no newlib churn)
#include "rendering/psp/psp_terrain_window.hpp"  // the chunk-texture window

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace wowee::rendering::psp {

struct TerrainVertex {
    uint8_t u, v;        // chunk texture coords, 0..128 = 0..1 (GE 8-bit UV)
    uint16_t color;      // GE 5650
    int16_t x, y, z;     // local grid (r*-1024, c*-1024, h)
};
static_assert(sizeof(TerrainVertex) == 10, "GE u8 UV + 5650 + s16 xyz vertex is 10 bytes");

struct TerrainCamera {
    glm::vec3 pos{0.0f};      // server coords
    glm::vec3 forward{1, 0, 0};
    glm::vec3 up{0, 0, 1};
    // Vertical field of view of the world camera in degrees; the horizontal one
    // follows the aspect (84 deg at 480 x 272). The value is under clean-room
    // review (round 2: camera_fov). Was 60 (cfg "fov").
    float fovyDeg = ge::kWorldFovYDeg;
    // 1.0: the GE drops a whole triangle when a vertex projects past its
    // 4096 px space, and a vertex clipped at the near plane lands up to
    // (edge length / near) screen-widths out; with near 0.6 the building
    // walls vanished at the screen edges (seen on hardware). Walls stay out of
    // the near plane by the camera's wall clearance instead.
    float nearZ = 1.0f;
    float farZ = 450.0f;
};

struct TerrainStats {
    uint32_t tiles = 0;
    uint32_t chunksDrawn = 0;
    uint32_t chunksCulled = 0;
    uint32_t triangles = 0;
    uint32_t textureKb = 0;
    uint32_t detailChunks = 0;
};

class TerrainRenderer {
public:
    TerrainRenderer();
    ~TerrainRenderer();

    // Loads tile "t_<x>_<y>" from the pak (returns false when absent).
    bool loadTile(platform::psp::Pak& pak, int tileX, int tileY);
    void clear();
    int tileCount() const { return static_cast<int>(tiles_.size()); }

    // Height at a server-coords position from loaded tiles (bilinear on the
    // 9x9 grid), or false when outside every loaded chunk.
    bool heightAt(float x, float y, float& z) const;
    // The loaded chunk under (x, y): ADT tile and MCNK index (ADT order), for
    // the area id (M12 zone music). False off the loaded tiles.
    bool chunkAt(float x, float y, int& tileX, int& tileY, int& chunk) const;

    // Sets GE projection/view for this camera and draws the terrain.
    void draw(const TerrainCamera& cam, uint32_t fogColorABGR);
    const TerrainStats& stats() const { return stats_; }

    // Debug A/B for the profiler: 0 normal, 1 untextured, 2 not drawn,
    // 3 no detail pass.
    int debugMode = 0;
    float detailDistance = 60.0f;  // yd from the chunk's bounding sphere
    // Chunk-texture mip choice: false = GU_TEXTURE_AUTO (default: cheaper,
    // soft when looking along the ground), true = SLOPE by distance (sharp to
    // ~120 yd, ~+1 ms GE measured on hardware). wowee.cfg "terrainsharp".
    bool sharpFloor = false;
    // How far the sharp floor keeps mip level 0, in yards (slope = 0.00815 at
    // 120 yd). wowee.cfg "sharpdist", settings menu.
    float sharpDistance = 120.0f;
    // The near detail pass (v4 "dt_" textures). wowee.cfg "terraindetail".
    bool detail = true;

    // Tile streaming (src/ui/psp/ui_world_streaming.cpp).
    bool hasTile(int tileX, int tileY) const;
    // Frees the tile (vertex block + blob); returns the bytes it held, 0 when
    // not loaded. Main thread, between frames only (the GE reads both while
    // a frame is drawn; frameEnd waits for it with sceGuSync). Detail
    // textures stay: 14 of ~22 KB for the whole baked area.
    uint32_t unloadTile(int tileX, int tileY);
    // Detail texture ids ("dt_<n>") a "t_<x>_<y>" blob uses that are not
    // resident yet (what loadTile would read), without keeping the blob.
    void missingDetails(const void* blob, uint32_t size, std::vector<uint16_t>& out) const;
    uint32_t residentBytes() const;  // tiles only (blobs + vertex blocks)

    // ---- Memory diet: the chunk-texture window -----------------------------------------------------------
    // On (wowee.cfg terraintex=1 with the tile streamer; set before the first loadTile): loadTile keeps the tile's
    // head only (header + chunk records -> vertex block) and reads the v4 detail table; the chunk textures (1.5 MB of
    // the 1.6 MB blob) come in per chunk row near the player - texSyncLoad at world entry, then the streamer's reads
    // (texStep -> texInstall) - and go past texRule.dropYd. A chunk without its texture draws with its vertex colour
    // (beyond the far plane it is not drawn at all). Off: the whole blob stays resident, the textures inside it, as
    // before. Main thread only; frees only between frames (the streamer's update, after frameEnd's sceGuSync).
    bool texWindow = false;
    TexWindowRule texRule;
    struct TexRequest {
        int tileX = 0, tileY = 0;
        uint16_t first = 0, count = 0;  // chunk run (one chunk row)
        uint32_t offset = 0, size = 0;  // in the pak file: entry offset + texture base + first * texBytes
    };
    // World entry (the loading screen): every chunk texture the window wants for (px, py), read through `pak` now.
    // Returns the chunks installed.
    uint32_t texSyncLoad(platform::psp::Pak& pak, float px, float py);
    // Frees the textures past dropYd, then appends up to maxRuns runs to read (nearest first; marked requested).
    // True when more runs are wanted than were given.
    bool texStep(float px, float py, uint32_t maxRuns, std::vector<TexRequest>& out);
    // A run arrived (count * texBytes bytes): each chunk still wanted and without a texture gets a block of its own
    // (16-byte aligned, D-cache written back). The caller frees `blob`. Returns the chunks installed.
    uint32_t texInstall(int tileX, int tileY, uint16_t first, uint16_t count, const void* blob, uint32_t size);
    // A run's read failed: its chunks may be asked again.
    void texCancel(int tileX, int tileY, uint16_t first, uint16_t count);
    // The runs a tile not loaded yet wants for (px, py), from its head (header + chunk records) - the streamer reads
    // them with the tile's job, so a tile near the player (a teleport) shows textured at once. Offsets in the file.
    static void texRunsForHead(const uint8_t* head, uint32_t headSize, uint32_t entrySize, uint32_t entryOffset,
                               float px, float py, const TexWindowRule& rule, int tileX, int tileY,
                               std::vector<TexRequest>& out);
    // The v4 detail table (256 u16) of a tile not loaded yet: the "dt_" ids not resident (what loadTile would read).
    void missingDetailsTable(const uint8_t* table, std::vector<uint16_t>& out) const;
    struct TexStats {
        uint32_t tiles = 0, chunks = 0;        // windowed tiles, their textured chunks
        uint32_t resident = 0, requested = 0;  // chunks with a texture / with a read in flight
        uint32_t residentBytes = 0;
        uint32_t installs = 0, frees = 0, nomem = 0;  // since the renderer was made
    };
    TexStats texStats() const;
    uint32_t texNomemCount() const { return texNomem_; }  // chunks whose 6 KB block was not there (cheap: no walk)
    // ---- Memory diet: the terrain pool (psp_terrain_pool.hpp) ----
    // Taken once at boot (main_psp.cpp, after bigheapInit) with terraintex=1 and kept for the run: chunk textures,
    // tiles (record + vertex block), the job's head, the rows in flight - fixed slots, never newlib. Keys:
    // terraintexslots (0 = auto from terraintexyd), terraintileslots (12), terraintexreads (rows in flight, <= 8).
    // Log: terrain_pool. Without it (terraintex=0, or the block was not there) the old per-tile allocations.
    static bool poolInit();
    static bool pooled();
    static uint32_t poolTilesFree();
    // The job's head (header + chunk records) / detail table were read straight into the pool's head buffer
    // (StreamLoader::Request::dst = poolHead()): loadTile takes them from there.
    static uint8_t* poolHead();
    void headStaged(int tileX, int tileY) { stagedX_ = tileX; stagedY_ = tileY; stagedHead_ = true; stagedTable_ = false; }
    void tableStaged() { stagedTable_ = true; }
    // A row buffer of the pool (16 chunk textures) for a texture read in flight, or null (none free / no pool).
    static uint8_t* poolRunAlloc();
    static void poolRunFree(void* p);
    static uint32_t poolRunsFree();
    // world_flush: slots in use (texture / tile / row buffer); poolReset - every slot free again,
    // called with no tile in any renderer and no read in flight (returns the slots that were still out: lost ones).
    static void poolUsage(uint32_t& tex, uint32_t& tiles, uint32_t& runs);
    static uint32_t poolReset();
    uint32_t texPoolEvictions() const { return texPoolEvict_; }
    uint32_t texPoolFull() const { return texPoolFull_; }

    // loadTile's last failure was a short heap or a failed read - the tile may be loaded again later (the
    // streamer retries it, tile_terrain_retry) - not a missing / bad entry. On hardware it crashed here before
    // (loadTile+0x364: a store through the null vertex block of a failed memalign); no allocation on the tile path
    // goes through operator new now (its handler is fatal).
    bool lastLoadNoMemory() const { return lastNoMem_; }

private:
    struct Chunk {
        float baseX, baseY, baseZ;
        float minZ, maxZ;
        const TerrainVertex* verts;  // 117 (81 grid + 36 skirt), inside Tile::vertexBlock
        const uint16_t* clut;        // v2: 256 x 5650, inside Tile::blob (else null)
        const uint8_t* texels;       // v2: 64x64 T8 swizzled
        uint16_t detail;             // v4: detail texture id, 0xFFFF = none
        // The texture window - the chunk's own texture block (clut / texels point into it), and its sphere
        // as the draw culls it (centre x, y; radius).
        uint8_t* tex;
        uint8_t texState;            // 0 none, 1 read in flight, 2 resident
        float cx, cy, rad;
    };
    struct Tile {
        int tileX, tileY;
        float unit;
        bool hasMips;                // v3 (not "mips": a MIPS compiler macro)
        Chunk chunks[256];
        TerrainVertex* vertexBlock;  // 256 * 117, 16-byte aligned, written once
        void* blob;                  // pak entry, kept: the GE reads textures from it
        uint32_t bytes;              // blob + vertex block (streaming log / eviction)
        // The texture window - no blob kept; where the textures are in the pak file (texture reads).
        bool windowed;
        uint32_t fileOffset, texBase, texBytes;
        // The chunk lookup of heightAt/chunkAt, built on first
        // use (main thread). Regular tile = every chunk of row r has baseX ==
        // rowX[r] and of column c baseY == colY[c] (bitwise), unit > 0: the
        // scan's "first chunk with fr, fc in [0, 8]" is then the first row
        // passing times 16 plus the first column passing. Otherwise the scan.
        mutable int8_t lookup;       // 0 not built, 1 regular, -1 scan
        mutable float rowX[16], colY[16];
        mutable float loX, hiX, loY, hiY;  // no chunk passes outside (with a margin)
    };

    std::vector<Tile*> tiles_;
    std::vector<platform::psp::Texture> details_;  // "dt_<n>" by id (pixels == null: absent)
    std::vector<const Chunk*> near_;
    uint16_t* lodIndices_[4] = {nullptr, nullptr, nullptr, nullptr};
    int lodCounts_[4] = {0, 0, 0, 0};      // strip indices per LOD
    int lodTriangles_[4] = {0, 0, 0, 0};   // real triangles per LOD
    TerrainStats stats_;
    void loadDetail(platform::psp::Pak& pak, uint16_t id);
    // A new tile's chunks from its records (vertex block, bounds, spheres); textures from `texData` (the
    // blob's texture blocks, `layout.texBytes` each) or none (the window); detail ids from `detailTable` or none.
    // False: the vertex block could not be allocated (the tile is not built; lastNoMem_).
    bool buildTile(Tile& tile, const uint8_t* records, const TerrainLayout& layout, const uint8_t* texData,
                   const uint8_t* detailTable, platform::psp::Pak& pak);
    bool loadTileWindowed(platform::psp::Pak& pak, const platform::psp::PakEntry* e, int tileX, int tileY);
    Tile* findTile(int tileX, int tileY);
    bool roomForTile();
    void texFree(Tile& t, Chunk& ch);
    bool lastNoMem_ = false;
    // The pool's head buffer holds tile (stagedX_, stagedY_)'s head (and its v4 table).
    bool stagedHead_ = false, stagedTable_ = false;
    int stagedX_ = 0, stagedY_ = 0;
    uint32_t texPoolEvict_ = 0, texPoolFull_ = 0;
    Tile* newTile();
    void deleteTile(Tile* t);
    uint8_t* texBlock(const Tile& forTile, const Chunk& forChunk);  // a pool slot (evicting the farthest) or memalign
    void texRelease(uint8_t* block);
    float texPx_ = 0.0f, texPy_ = 0.0f;  // where the last texStep / texSyncLoad stood (texInstall's drop test)
    uint32_t texInstalls_ = 0, texFrees_ = 0, texNomem_ = 0;
    // The chunk of tile t the plain scan (scanChunk) picks for
    // (x, y) - its index and fr/fc - or -1; with g_cpuFast through the lookup.
    int findChunk(const Tile& t, float x, float y, float& fr, float& fc) const;
    static int scanChunk(const Tile& t, float x, float y, float& fr, float& fc);
    static void buildLookup(const Tile& t);
};

} // namespace wowee::rendering::psp
