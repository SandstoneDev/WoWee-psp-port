// psp_minimap_format.hpp - minimap / world map / indoor (WMO) minimap / dungeon map records and the minimap math.
//
// Clean-room implementation of specs/world/minimap_format.md (v2), except kIndoorZoomYards (2.1) and
// wmoMinimapWalk (3.4), which follow the port's own rules described at them. Shared by the PC minimap converter and the
// EBOOT. Plain little-endian PODs with asserted sizes; namespace wowee::rendering::psp. Depends only on the
// upstream minimap view type (ui::MinimapView) and the standard headers below; no platform headers.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ui/minimap_projection.hpp"

namespace wowee {
namespace rendering {
namespace psp {

// ---------------------------------------------------------------------------------------------------------------
// 2.1 Constants
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMinimapVersion = 1;
constexpr uint32_t kWorldMapVersion = 1;
constexpr float kMinimapTileYards = 533.33333f;
constexpr uint32_t kWmoMinimapVersion = 1;
constexpr float kWmoMinimapYardsPerPixel = 0.5f;  // an indoor tile texture pixel covers 0.5 yard
constexpr float kWmoMinimapTileYards = 128.0f;  // indoor tile spacing (256 px)
// Indoor zoom levels 0..5, yards across the minimap (the port's own scale): 32-yard steps from 192 (about the
// closest outdoor view, 100 yd radius) down to 32.
constexpr float kIndoorZoomYards[6] = {192.0f, 160.0f, 128.0f, 96.0f, 64.0f, 32.0f};
constexpr uint32_t kDungeonMapVersion = 1;

// ---------------------------------------------------------------------------------------------------------------
// 2.3 Record layouts
// ---------------------------------------------------------------------------------------------------------------
struct MinimapHeader {
    char magic[4];  // "WMMP"
    uint32_t version;
    uint32_t mapId;  // Map.dbc ID
    char mapName[32];  // Map.dbc directory
    uint16_t tileSize;  // texels per tile edge
    uint16_t tileCount;
    uint32_t areaCount;
    uint32_t namesSize;
};

struct MinimapArea {
    uint32_t id;  // AreaTable.dbc ID
    uint32_t parent;
    uint32_t exploreFlag;
    uint32_t nameOffset;
};

struct WorldMapHeader {
    char magic[4];  // "WWMP"
    uint32_t version;
    uint32_t mapId;
    uint32_t zoneCount;
    uint32_t overlayCount;
    uint32_t exploreCount;
    uint32_t zmpCells;  // 0 or 128 x 128
    uint32_t namesSize;
    uint16_t baseTile;  // texels of one baked 256-pixel art tile
    uint16_t reserved;
};

struct WorldMapZone {
    uint32_t wmaId;
    uint32_t mapId;
    uint32_t areaId;
    uint32_t displayMapId;
    uint32_t parentWorldMapId;
    float locLeft;
    float locRight;
    float locTop;
    float locBottom;
    float virtualOffsetX;
    float virtualOffsetY;
    uint32_t folderOffset;
    uint32_t nameOffset;
    uint32_t firstExplore;
    uint16_t exploreCount;
    uint16_t firstOverlay;
    uint16_t overlayCount;
    uint16_t tileMask;  // bit n = w_<wma>_<n> baked
};

struct WorldMapOverlay {
    uint32_t areaIds[4];
    uint16_t texWidth;
    uint16_t texHeight;
    uint16_t offsetX;
    uint16_t offsetY;
    uint16_t hitLeft;
    uint16_t hitRight;
    uint16_t hitTop;
    uint16_t hitBottom;  // 1024 x 768 map pixels
    uint32_t baked;  // 1 = o_ entry exists
};

struct MinimapVert {
    float sx, sy;  // screen position
    float gx, gy;  // continuous tile coordinates
};

struct WmoMinimapHeader {
    char magic[4];  // "WMMI"
    uint32_t version;
    uint16_t instanceCount;
    uint16_t groupCount;
    uint32_t tileCount;
    uint32_t portalCount;
};

struct WmoMinimapInstance {
    float toWorld[12];  // WMO-local -> server, 3 x 4 column-major (element (r, c) at c * 3 + r)
    float toLocal[12];  // its inverse, same layout
    float bmin[3];  // world box
    float bmax[3];
    uint32_t uniqueId;  // placement id; 0xFFFFFFFF for a map's single global WMO
    uint16_t firstGroup;
    uint16_t groupCount;
};

struct WmoMinimapGroup {
    float bmin[3];  // group box, WMO-local
    float bmax[3];
    uint32_t flags;  // root group-info flags: 0x8 exterior, 0x40 exterior lit, 0x2000 indoor
    uint32_t wmoGroupId;
    uint32_t firstTile;
    uint32_t firstPortal;
    uint16_t tileCount;
    uint16_t portalCount;
};

struct WmoMinimapTile {
    uint16_t group;  // global group index
    uint8_t x, y;  // tile indices within its group
    uint16_t pxW, pxH;  // source texture pixel size (tile covers px * 0.5 yards)
    float x0, y0;  // local corner
    uint32_t baked;  // 1 = u_<tile index> exists
};

struct WmoMinimapPortal {
    uint16_t group;  // group on the other side, global index
    uint16_t reserved;
    float bmin[3];  // portal polygon box, WMO-local
    float bmax[3];
};

struct DungeonMapHeader {
    char magic[4];  // "WDMP"
    uint32_t version;
    uint32_t mapId;
    uint32_t wma;  // the map's WorldMapArea, 0 = none
    int32_t corpseMap;
    float corpseX;
    float corpseY;
    uint16_t floorCount;
    uint16_t chunkCount;
    uint8_t terrainLevel;  // 1: level 1 is the zone's terrain map
    uint8_t reserved[3];
    uint32_t terrainName;  // name offset, 0 = none
    uint32_t namesSize;
};

struct DungeonFloor {
    uint32_t id;  // DungeonMap.dbc ID
    uint16_t floor;  // floor index, 1..
    uint16_t tileMask;  // bit n = f_<wma>_<floor>_<n> baked
    float locLeft;  // server Y max
    float locRight;  // server Y min
    float locTop;  // server X max
    float locBottom;  // server X min
    uint32_t nameOffset;  // 0 = generic "floor n" label
};

struct DungeonChunk {
    uint32_t wmoGroupId;  // DungeonMapChunk.dbc WMO group id
    uint32_t dungeonMapId;  // -> DungeonFloor.id
    float minZ;
};

static_assert(sizeof(MinimapHeader) == 56, "MinimapHeader size");
static_assert(sizeof(MinimapArea) == 16, "MinimapArea size");
static_assert(sizeof(WorldMapHeader) == 36, "WorldMapHeader size");
static_assert(sizeof(WorldMapZone) == 64, "WorldMapZone size");
static_assert(sizeof(WorldMapOverlay) == 36, "WorldMapOverlay size");
static_assert(sizeof(MinimapVert) == 16, "MinimapVert size");
static_assert(sizeof(WmoMinimapHeader) == 20, "WmoMinimapHeader size");
static_assert(sizeof(WmoMinimapInstance) == 128, "WmoMinimapInstance size");
static_assert(sizeof(WmoMinimapGroup) == 44, "WmoMinimapGroup size");
static_assert(sizeof(WmoMinimapTile) == 20, "WmoMinimapTile size");
static_assert(sizeof(WmoMinimapPortal) == 28, "WmoMinimapPortal size");
static_assert(sizeof(DungeonMapHeader) == 44, "DungeonMapHeader size");
static_assert(sizeof(DungeonFloor) == 28, "DungeonFloor size");
static_assert(sizeof(DungeonChunk) == 12, "DungeonChunk size");

// ---------------------------------------------------------------------------------------------------------------
// 3.1 Coordinates
// ---------------------------------------------------------------------------------------------------------------
inline void minimapTileCoords(float serverX, float serverY, float& gx, float& gy) {
    gx = 32.0f - serverY / kMinimapTileYards;
    gy = 32.0f - serverX / kMinimapTileYards;
}

inline void minimapServerPos(float gx, float gy, float& serverX, float& serverY) {
    serverY = (32.0f - gx) * kMinimapTileYards;
    serverX = (32.0f - gy) * kMinimapTileYards;
}

using MinimapProj = ui::MinimapView;

inline void minimapDeltaToOffset(float dx, float dy, const MinimapProj& v, float& px, float& py) {
    if (v.viewRadius <= 0.0f) {
        px = 0.0f;
        py = 0.0f;
        return;
    }
    const float c = v.cosBearing, s = v.sinBearing;
    const float rx = -(dy * c - dx * s);
    const float ry = -(dy * s + dx * c);
    px = rx / v.viewRadius * v.mapRadius;
    py = ry / v.viewRadius * v.mapRadius;
}

inline void minimapOffsetToDelta(float px, float py, const MinimapProj& v, float& dx, float& dy) {
    if (v.mapRadius <= 0.0f) {
        dx = 0.0f;
        dy = 0.0f;
        return;
    }
    const float c = v.cosBearing, s = v.sinBearing;
    const float rx = px / v.mapRadius * v.viewRadius;
    const float ry = py / v.mapRadius * v.viewRadius;
    const float X = -rx * c - ry * s;
    const float Y = -rx * s + ry * c;
    dx = -Y;
    dy = X;
}

// ---------------------------------------------------------------------------------------------------------------
// 3.2 Disc polygon and tile clipping (Sutherland-Hodgman against one axis-aligned line at a time)
// ---------------------------------------------------------------------------------------------------------------
inline int minimapClipAxis(const MinimapVert* in, int n, MinimapVert* out, int axis, float limit, bool keepAbove) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const MinimapVert& a = in[i];
        const MinimapVert& b = in[(i + 1) % n];
        const float va = axis == 0 ? a.gx : a.gy;
        const float vb = axis == 0 ? b.gx : b.gy;
        const bool ia = keepAbove ? (va >= limit) : (va <= limit);
        const bool ib = keepAbove ? (vb >= limit) : (vb <= limit);
        if (ia) out[m++] = a;
        if (ia != ib) {
            const float t = (va - limit) / (va - vb);
            MinimapVert c;
            c.sx = a.sx + (b.sx - a.sx) * t;
            c.sy = a.sy + (b.sy - a.sy) * t;
            c.gx = a.gx + (b.gx - a.gx) * t;
            c.gy = a.gy + (b.gy - a.gy) * t;
            out[m++] = c;
        }
    }
    return m;
}

inline int minimapClipToTile(const MinimapVert* poly, int n, int tx, int ty, MinimapVert* out, MinimapVert* tmp) {
    const float x0 = static_cast<float>(tx), x1 = static_cast<float>(tx + 1);
    const float y0 = static_cast<float>(ty), y1 = static_cast<float>(ty + 1);
    int m = minimapClipAxis(poly, n, tmp, 0, x0, true);
    if (m < 3) return 0;
    m = minimapClipAxis(tmp, m, out, 0, x1, false);
    if (m < 3) return 0;
    m = minimapClipAxis(out, m, tmp, 1, y0, true);
    if (m < 3) return 0;
    m = minimapClipAxis(tmp, m, out, 1, y1, false);
    if (m < 3) return 0;
    return m;
}

inline int minimapDisc(float cx, float cy, float playerX, float playerY, const MinimapProj& v, int segments,
                       MinimapVert* out) {
    const float step = 6.28318530718f / static_cast<float>(segments);
    for (int k = 0; k < segments; ++k) {
        const float a = static_cast<float>(k) * step;
        const float ox = std::cos(a) * v.mapRadius;
        const float oy = std::sin(a) * v.mapRadius;
        float dx = 0.0f, dy = 0.0f;
        minimapOffsetToDelta(ox, oy, v, dx, dy);
        MinimapVert& o = out[k];
        o.sx = cx + ox;
        o.sy = cy + oy;
        minimapTileCoords(playerX + dx, playerY + dy, o.gx, o.gy);
    }
    return segments;
}

inline void minimapTileRange(const MinimapVert* poly, int n, int& x0, int& y0, int& x1, int& y1) {
    if (n <= 0) {  // empty polygon: an empty range (not specified; own choice)
        x0 = y0 = 0;
        x1 = y1 = -1;
        return;
    }
    float minX = poly[0].gx, maxX = poly[0].gx, minY = poly[0].gy, maxY = poly[0].gy;
    for (int i = 1; i < n; ++i) {
        minX = std::min(minX, poly[i].gx);
        maxX = std::max(maxX, poly[i].gx);
        minY = std::min(minY, poly[i].gy);
        maxY = std::max(maxY, poly[i].gy);
    }
    x0 = static_cast<int>(std::floor(minX));
    x1 = static_cast<int>(std::floor(maxX));
    y0 = static_cast<int>(std::floor(minY));
    y1 = static_cast<int>(std::floor(maxY));
}

// ---------------------------------------------------------------------------------------------------------------
// 3.3 wmmi view and placement matrices
// ---------------------------------------------------------------------------------------------------------------
struct WmoMinimapView {
    const WmoMinimapHeader* h = nullptr;
    const WmoMinimapInstance* inst = nullptr;
    const WmoMinimapGroup* groups = nullptr;
    const WmoMinimapTile* tiles = nullptr;
    const WmoMinimapPortal* portals = nullptr;

    bool attach(const void* blob, uint32_t size) {
        h = nullptr;
        inst = nullptr;
        groups = nullptr;
        tiles = nullptr;
        portals = nullptr;
        if (!blob || size < sizeof(WmoMinimapHeader)) return false;
        const uint8_t* b = static_cast<const uint8_t*>(blob);
        const WmoMinimapHeader* hh = reinterpret_cast<const WmoMinimapHeader*>(b);
        if (std::memcmp(hh->magic, "WMMI", 4) != 0) return false;
        if (hh->version != kWmoMinimapVersion) return false;
        const uint64_t ic = hh->instanceCount, gc = hh->groupCount, tc = hh->tileCount, pc = hh->portalCount;
        const uint64_t instOff = sizeof(WmoMinimapHeader);
        const uint64_t groupOff = instOff + ic * sizeof(WmoMinimapInstance);
        const uint64_t tileOff = groupOff + gc * sizeof(WmoMinimapGroup);
        const uint64_t portalOff = tileOff + tc * sizeof(WmoMinimapTile);
        const uint64_t end = portalOff + pc * sizeof(WmoMinimapPortal);
        if (end > size) return false;
        const WmoMinimapInstance* in = reinterpret_cast<const WmoMinimapInstance*>(b + instOff);
        const WmoMinimapGroup* gr = reinterpret_cast<const WmoMinimapGroup*>(b + groupOff);
        const WmoMinimapTile* ti = reinterpret_cast<const WmoMinimapTile*>(b + tileOff);
        const WmoMinimapPortal* po = reinterpret_cast<const WmoMinimapPortal*>(b + portalOff);
        for (uint64_t i = 0; i < ic; ++i)
            if (static_cast<uint64_t>(in[i].firstGroup) + in[i].groupCount > gc) return false;
        for (uint64_t i = 0; i < gc; ++i) {
            if (static_cast<uint64_t>(gr[i].firstTile) + gr[i].tileCount > tc) return false;
            if (static_cast<uint64_t>(gr[i].firstPortal) + gr[i].portalCount > pc) return false;
        }
        for (uint64_t i = 0; i < tc; ++i)
            if (ti[i].group >= gc || ti[i].pxW == 0 || ti[i].pxH == 0) return false;
        for (uint64_t i = 0; i < pc; ++i)
            if (po[i].group >= gc) return false;
        h = hh;
        inst = in;
        groups = gr;
        tiles = ti;
        portals = po;
        return true;
    }
};

inline void wmoMinimapApply(const float m[12], float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = m[0] * x + m[3] * y + m[6] * z + m[9];
    oy = m[1] * x + m[4] * y + m[7] * z + m[10];
    oz = m[2] * x + m[5] * y + m[8] * z + m[11];
}

// ---------------------------------------------------------------------------------------------------------------
// 3.4 Indoor group walk
// ---------------------------------------------------------------------------------------------------------------
// Which groups of one placed WMO show indoor minimap tiles: breadth first through the portals from the player's
// group `start`, as upstream WMORenderer::getVisibleGroupsViaPortals walks the portal graph from the camera's group.
// A portal is passed when its box meets the view box [bmin, bmax] (closed intervals on all three axes; upstream
// tests the portal against the camera frustum instead). Port's own rule: only interior groups (flag 0x2000, the
// indoor test's flag) are entered; groups more than maxDepth (clamped to 0..254) portals away are not reached.
// `seen` (the instance's groupCount bytes, zero on entry) gets 1 for every group reached, the start included.
// Returns the number of groups reached (0 for a start outside the instance). No allocation: during the walk
// seen[i] holds 1 + the group's distance in portals, and is put back to 1 at the end.
inline int wmoMinimapWalk(const WmoMinimapView& v, const WmoMinimapInstance& in, uint32_t start, const float bmin[3],
                          const float bmax[3], uint8_t* seen, int maxDepth = 254) {
    if (!v.h || !v.groups || !seen) return 0;
    const uint32_t first = in.firstGroup;
    const uint32_t count = in.groupCount;
    if (start < first || start >= first + count) return 0;
    maxDepth = std::clamp(maxDepth, 0, 254);
    seen[start - first] = 1;
    int reached = 1;
    for (int level = 1; level <= maxDepth; ++level) {
        bool grew = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (seen[i] != level) continue;
            const WmoMinimapGroup& G = v.groups[first + i];
            for (uint32_t k = 0; k < G.portalCount; ++k) {
                const WmoMinimapPortal& P = v.portals[G.firstPortal + k];
                const uint32_t o = P.group;
                if (o < first || o >= first + count) continue;
                if (seen[o - first]) continue;
                if (!(v.groups[o].flags & 0x2000u)) continue;
                bool overlap = true;
                for (int a = 0; a < 3; ++a)
                    if (P.bmax[a] < bmin[a] || P.bmin[a] > bmax[a]) overlap = false;
                if (!overlap) continue;
                seen[o - first] = static_cast<uint8_t>(level + 1);
                ++reached;
                grew = true;
            }
        }
        if (!grew) break;
    }
    for (uint32_t i = 0; i < count; ++i)
        if (seen[i]) seen[i] = 1;
    return reached;
}

// ---------------------------------------------------------------------------------------------------------------
// 3.5 Dungeon map view
// ---------------------------------------------------------------------------------------------------------------
struct DungeonMapView {
    const DungeonMapHeader* h = nullptr;
    const DungeonFloor* floors = nullptr;
    const DungeonChunk* chunks = nullptr;
    const char* names = nullptr;

    bool attach(const void* blob, uint32_t size) {
        h = nullptr;
        floors = nullptr;
        chunks = nullptr;
        names = nullptr;
        if (!blob || size < sizeof(DungeonMapHeader)) return false;
        const uint8_t* b = static_cast<const uint8_t*>(blob);
        const DungeonMapHeader* hh = reinterpret_cast<const DungeonMapHeader*>(b);
        if (std::memcmp(hh->magic, "WDMP", 4) != 0) return false;
        if (hh->version != kDungeonMapVersion) return false;
        const uint64_t floorOff = sizeof(DungeonMapHeader);
        const uint64_t chunkOff = floorOff + static_cast<uint64_t>(hh->floorCount) * sizeof(DungeonFloor);
        const uint64_t namesOff = chunkOff + static_cast<uint64_t>(hh->chunkCount) * sizeof(DungeonChunk);
        if (namesOff + hh->namesSize > size) return false;
        if (hh->namesSize == 0) return false;
        const char* nm = reinterpret_cast<const char*>(b + namesOff);
        if (nm[hh->namesSize - 1] != '\0') return false;
        if (hh->terrainName >= hh->namesSize) return false;
        const DungeonFloor* fl = reinterpret_cast<const DungeonFloor*>(b + floorOff);
        for (uint32_t i = 0; i < hh->floorCount; ++i)
            if (fl[i].nameOffset >= hh->namesSize) return false;
        h = hh;
        floors = fl;
        chunks = reinterpret_cast<const DungeonChunk*>(b + chunkOff);
        names = nm;
        return true;
    }

    const char* str(uint32_t off) const {
        if (!h || off == 0 || off >= h->namesSize) return nullptr;
        return names + off;
    }

    int levels() const {
        if (!h) return 0;
        return static_cast<int>(h->floorCount) + (h->terrainLevel ? 1 : 0);
    }

    int floorOfLevel(int level) const {
        if (!h) return -1;
        const int idx = h->terrainLevel ? level - 2 : level - 1;  // terrain level 1 gives -2 + 1 = -1
        if (idx < 0 || idx >= static_cast<int>(h->floorCount)) return -1;
        return idx;
    }

    int playerLevel(uint32_t wmoGroupId, float x, float y, float z) const {
        if (!h) return 0;
        if (wmoGroupId != 0) {
            int bestFloor = -1;
            float bestZ = 0.0f;
            for (uint32_t i = 0; i < h->chunkCount; ++i) {
                const DungeonChunk& c = chunks[i];
                if (c.wmoGroupId != wmoGroupId || !(c.minZ <= z)) continue;
                const int f = floorById(c.dungeonMapId);
                if (f < 0) continue;
                if (bestFloor < 0 || c.minZ >= bestZ) {  // on equal minZ the later chunk wins
                    bestFloor = f;
                    bestZ = c.minZ;
                }
            }
            if (bestFloor >= 0) return (h->terrainLevel ? 2 : 1) + bestFloor;
        }
        if (h->terrainLevel) return 1;
        for (uint32_t i = 0; i < h->floorCount; ++i) {
            const DungeonFloor& f = floors[i];
            if (f.locRight <= y && y <= f.locLeft && f.locBottom <= x && x <= f.locTop)
                return 1 + static_cast<int>(i);
        }
        return 1;
    }

private:
    // index of the floor with that id (the last one in table order if several share it), or -1
    int floorById(uint32_t id) const {
        int r = -1;
        for (uint32_t i = 0; i < h->floorCount; ++i)
            if (floors[i].id == id) r = static_cast<int>(i);
        return r;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// 3.6 Entry names
// ---------------------------------------------------------------------------------------------------------------
inline void dungeonFloorTileName(char* out, size_t n, uint32_t wma, int floor, int tile) {
    std::snprintf(out, n, "f_%lu_%d_%d", static_cast<unsigned long>(wma), floor, tile);
}

inline void wmoMinimapTileName(char* out, size_t n, uint32_t tile) {
    std::snprintf(out, n, "u_%lu", static_cast<unsigned long>(tile));
}

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
