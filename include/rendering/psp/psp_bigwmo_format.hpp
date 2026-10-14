// Big WMOs (capitals, harbours, fortresses - every WMO over the doodad bake's
// triangle budget) as "a small world, not a model". Shared by the converter
// (tools/psp/converter/wmo_chunk_bake.hpp, psp_model_conv) and the PSP (src/rendering/psp/psp_bigwmo.cpp).
// Plain little-endian PODs, no platform headers.
//
// A big WMO is its own pak, data/bigwmo/<map>_<stem>.pak: WPAK v1 with the TOC sorted by name and the "~fence"
// record (PakIndex, include/platform/psp/psp_pak_index.hpp). Records:
//   "!wmo"      BigWmoHeader and its tables (TOC record 0: '!' sorts first) - the only record read by name; it
//               holds the offset and size of every other record, so the PSP never searches the TOC
//   "c_<n>"     chunk n, full detail (WBCK)
//   "c_<n>" holds the triangles split to --bigwmo-base-edge (16 yd: tight chunk spheres); "n_<n>" the same split
//               further to --bigwmo-edge (WBCK, optional: chunkNear[n].size 0 = nothing to split) - drawn instead of "c_"
//               while the chunk's bounding sphere is not guard-band safe (the GE guard-band rule)
//   "l_<n>"     chunk n, simplified far mesh (WBCK, optional: chunkLod[n].size 0 = none)
//   "q_<n>"     chunk n collision (WCOL, psp_model_format.hpp; WMO-local float, the unsplit triangles)
//   "s_<cell>"  far shell of a cell (WBCK, untextured): its triangles seen from outside (every group), simplified
//   "dd_<cell>" doodads of a cell (BigWmoDoodad[]), their models in the map's doodads pak
//   "x_<n>"     WTEX textures of the chunks (the WMO's own bank)
//
// The map's doodads pak holds "wbig" (BigWmoIndexHeader + BigWmoPlacement[]): where each big WMO stands. The
// big WMO's doodads use that pak's models ("m_<id>"), so both paks come from one psp_model_conv run: the stamp
// in "wbig" and in BigWmoHeader must match (else the PSP draws the WMO without its doodads).
//
// Geometry ("a common grid"): the WMO is cut into cells of cellSize x cellSize yards (local X/Y). A chunk is
// the triangles of ONE group inside ONE cell (centroid rule), split further to ~chunkTriangles. Positions are
// s16 on one lattice of `step` yards for the whole WMO: vertex local = cell.center + s16 * step, the cell centre
// a multiple of step. A vertex shared by two chunks or two cells is the same lattice point in both - the GE
// sees the same number, no cracks. The GE divides s16 by 32768: model matrix = placement * T(cell.center) *
// S(step * 32768). One matrix per cell per frame.
#pragma once

#include <cstdint>

namespace wowee::rendering::psp {

constexpr uint16_t kBigWmoVersion = 1;

// ---- doodads pak: "wbig" -------------------------------------------------------------------------------------
struct BigWmoIndexHeader {
    char magic[4];          // "WBGI"
    uint16_t version;       // kBigWmoVersion
    uint16_t count;         // BigWmoPlacement records
    uint32_t stamp;         // = BigWmoHeader::stamp of every pak listed
    uint32_t reserved;
};
static_assert(sizeof(BigWmoIndexHeader) == 16, "WBGI header is 16 bytes");

struct BigWmoPlacement {
    uint32_t uniqueId;      // MODF uniqueId (one placement however many ADTs list it)
    uint16_t doodadSet;     // MODF doodad set (set 0 is always added, as upstream terrain_manager)
    uint16_t flags;         // 0
    int16_t tileX, tileY;   // the first ADT tile that placed it (logs)
    char pak[44];           // "bigwmo/<map>_<stem>.pak", relative to data/
    float matrix[12];       // world = M * local: columns 0..2 rotation * scale (upstream placementEuler), col 3 position
    float bmin[3], bmax[3]; // world box of the WMO
};
static_assert(sizeof(BigWmoPlacement) == 128, "WBGI placement is 128 bytes");

// ---- the big WMO pak: "!wmo" ---------------------------------------------------------------------------------
// BigWmoHeader, then (offsets from the record start, 4-aligned):
//   BigWmoGroup[groupCount], BigWmoPortal[portalCount], BigWmoPortalRef[portalRefCount],
//   float portalVertex[portalVertexCount][3], BigWmoCell[cellCount], BigWmoChunk[chunkCount],
//   BigWmoBlobRef chunkLod[chunkCount], BigWmoBlobRef texture[textureCount], BigWmoBlobRef chunkNear[chunkCount]
struct BigWmoHeader {
    char magic[4];          // "WBWO"
    uint16_t version;       // kBigWmoVersion
    uint16_t flags;         // 0
    uint32_t stamp;         // the "wbig" stamp of the doodads pak baked in the same run
    uint16_t groupCount;
    uint16_t cellCount;
    uint32_t chunkCount;
    uint16_t portalCount;
    uint16_t portalRefCount;
    uint32_t portalVertexCount;
    uint16_t textureCount;
    uint16_t cellsX, cellsY;  // cell grid (cell index = iy * cellsX + ix is NOT used: cells are a list)
    uint16_t reserved0;
    float bmin[3], bmax[3]; // local box (MOHD)
    float step;             // yards per s16 unit (power-of-two fraction)
    float cellSize;         // yards
    float cellOrigin[2];    // local X/Y of cell (0, 0)'s lower corner
    float ambient[3];       // MOHD ambient (baked into the vertices already; info)
    uint32_t totalTriangles;   // full detail, every chunk
    uint32_t groupOffset, portalOffset, portalRefOffset, portalVertexOffset;
    uint32_t cellOffset, chunkOffset, chunkLodOffset, textureOffset;
    uint32_t doodadTotal;   // BigWmoDoodad records in all "dd_" records
    uint32_t chunkNearOffset;  // BigWmoBlobRef chunkNear[chunkCount] ("n_"), after chunkLod
    uint32_t reserved;
};
static_assert(sizeof(BigWmoHeader) == 136, "WBWO header is 136 bytes");

enum BigWmoHeaderFlags : uint16_t {
    // The bake measured the MOPR side sign against each group's own vertices (the group lies on `side` of the
    // portal plane through the portal's vertices along the MOPT normal); set when most refs say the opposite.
    kBigWmoSideFlipped = 1u << 0,
};

// MOGP flags the PSP reads (wowdev WMO, the 3.3.5a client).
enum BigWmoGroupFlags : uint32_t {
    kWmoGroupHasBsp = 0x1u,
    kWmoGroupExterior = 0x8u,       // outdoors: drawn by frustum when the camera is outside / outdoors
    kWmoGroupExteriorLit = 0x40u,
    kWmoGroupUnreachable = 0x80u,
    kWmoGroupHasDoodads = 0x800u,   // MODR
    kWmoGroupInterior = 0x2000u,    // indoors: seen through portals only
    kWmoGroupAlwaysDraw = 0x10000u, // drawn whatever the portals say (frustum only)
    kWmoGroupAntiportal = 0x4000000u,  // never drawn (upstream skips it too)
};

struct BigWmoGroup {
    uint32_t flags;         // MOGP flags
    float bmin[3], bmax[3]; // MOGP box, local
    uint16_t portalStart;   // into the portal refs (MOGP portalStart/Count)
    uint16_t portalCount;
    uint16_t chunkCount;    // its chunks (spread over its cells; chunk.group names it)
    uint16_t doodadRefs;    // MODR entries (info)
    uint32_t triangles;     // full detail (info)
};
static_assert(sizeof(BigWmoGroup) == 40, "WBWO group is 40 bytes");

struct BigWmoPortal {
    uint16_t vertexStart;   // into portalVertex
    uint16_t vertexCount;
    float plane[4];         // MOPT plane: normal xyz, d (dot(n, p) + d)
};
static_assert(sizeof(BigWmoPortal) == 20, "WBWO portal is 20 bytes");

struct BigWmoPortalRef {
    uint16_t portal;
    uint16_t group;         // the group on the other side
    int16_t side;           // MOPR side (+1 / -1): the side of the plane this ref's own group is on
    uint16_t reserved;
};
static_assert(sizeof(BigWmoPortalRef) == 8, "WBWO portal ref is 8 bytes");

struct BigWmoBlobRef {
    uint32_t offset;        // in the pak file, 0 = none
    uint32_t size;
};
static_assert(sizeof(BigWmoBlobRef) == 8, "WBWO blob ref is 8 bytes");

struct BigWmoCell {
    int16_t ix, iy;         // grid position
    float center[3];        // local, a multiple of step: the chunks' vertex origin
    float bmin[3], bmax[3]; // local box of everything in it (chunks of every group)
    uint32_t chunkStart;    // its chunks: chunkStart .. chunkStart + chunkCount - 1 (sorted by cell, then group)
    uint32_t chunkCount;
    BigWmoBlobRef shell;    // "s_<cell>" (exterior groups simplified, untextured), or 0
    BigWmoBlobRef doodads;  // "dd_<cell>", or 0
    uint32_t shellTriangles;
    uint32_t doodadCount;
};
static_assert(sizeof(BigWmoCell) == 72, "WBWO cell is 72 bytes");

struct BigWmoChunk {
    BigWmoBlobRef mesh;     // "c_<n>" full detail (WBCK)
    BigWmoBlobRef collision;  // "q_<n>" (WCOL), or 0 (no triangles)
    int16_t bmin[3], bmax[3];  // local box, lattice units from the cell centre (rounded outwards)
    uint16_t group;
    uint16_t cell;
    uint16_t triangles;     // full detail
    uint16_t lodTriangles;  // the "l_" mesh (0 = none)
    uint16_t textureCount;  // textures the full mesh binds (listed in its WBCK)
    uint16_t nearTriangles; // the "n_" mesh (0 = none: no triangle longer than the bake's edge)
    uint16_t flags;         // BigWmoChunkFlags
    uint16_t outsideTriangles;  // of its source triangles, seen from outside (the bake's visibility pass)
};
static_assert(sizeof(BigWmoChunk) == 44, "WBWO chunk is 44 bytes");

enum BigWmoChunkFlags : uint16_t {
    // Some of its triangles are seen from outside the WMO: the bake renders the whole WMO (orthographic id buffer)
    // from 33 directions around and above it. With the camera in no group (outside the WMO) the PSP draws only
    // such chunks - what the client shows from outside is the WMO's outer surface (and what its openings let
    // through, as far as those views see into them).
    kChunkSeenOutside = 1u << 0,
};

// ---- WBCK: one chunk / LOD / shell mesh ----------------------------------------------------------------------
// ChunkMeshHeader, u16 texture[textureCount] (distinct "x_" ids its batches use, 4-aligned), ModelBatch[batchCount]
// (psp_model_format.hpp), ModelVertex[vertexCount] (16-aligned: u16 u, v; 5650 baked light; s16 x, y, z on the WMO
// lattice around the cell centre), with kChunkIndexed u16 index[indexCount] after them. A batch is one
// GU_TRIANGLE_STRIP (its triangles joined by degenerate ones): indexStart/indexCount = a range of the index array
// (kChunkIndexed: the distinct vertices once, ~1 per triangle, the strip ~2 indices per triangle - a third less
// memory than the vertices themselves in strip order, the same GE work), else a range of the vertex array.
// UV = u16 / 32768 * uvScale + uvMin (sceGuTexScale / sceGuTexOffset per chunk, as WMDL).
struct ChunkMeshHeader {
    char magic[4];          // "WBCK"
    uint16_t version;       // kBigWmoVersion
    uint16_t flags;         // kChunkShell
    uint16_t vertexCount;
    uint16_t batchCount;
    uint16_t textureCount;
    uint16_t cell;
    float uvMin[2];
    float uvScale[2];
    uint32_t textureOffset, batchOffset, vertexOffset;
    uint32_t triangles;
    uint32_t indexOffset;   // kChunkIndexed: u16 index[indexCount] (2-aligned)
    uint32_t indexCount;
};
static_assert(sizeof(ChunkMeshHeader) == 56, "WBCK header is 56 bytes");

enum ChunkMeshFlags : uint16_t {
    kChunkShell = 1u << 0,  // far shell: untextured, colour = light x the texture's mean colour
    kChunkLod = 1u << 1,    // the simplified far mesh of a chunk
    kChunkNear = 1u << 2,   // the guard-band split of a chunk (drawn next to the camera)
    kChunkIndexed = 1u << 3,  // batches index an u16 index array (indexed strips)
};

// ---- "dd_<cell>": the WMO's doodads in that cell -------------------------------------------------------------
// MODD placements of every doodad set (the PSP keeps set 0 + the placement's set, as upstream), in WMO-local
// space; model = "m_<id>" of the doodads pak ("wbig" stamp). group = the first group whose MODR lists it
// (0xFFFF: none) - the doodad is drawn while that group is visible (the client draws a group's MODR doodads).
struct BigWmoDoodad {
    uint16_t model;
    uint16_t group;
    uint16_t set;           // MODS index the doodad belongs to
    uint16_t flags;         // 0
    uint32_t index;         // MODD index (particles / uniqueId: partId(wmo uid, 0x20000 + index))
    float axes[9];          // local rotation * scale (column-major), as DoodadPlacement
    float position[3];      // local
    float sphere[4];        // local bounding sphere centre, radius
};
static_assert(sizeof(BigWmoDoodad) == 76, "WBWO doodad is 76 bytes");

}  // namespace wowee::rendering::psp
