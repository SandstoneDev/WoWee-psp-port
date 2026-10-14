// World map / minimap - readers of the "mmhdr" / "wmhdr" entries of data/minimap_<map>.pak
// (include/rendering/psp/psp_minimap_format.hpp), shared by the PSP and by
// psp_minimap_conv --verify / --selftest so both read the pak the same way.
//
// WorldMapView::buildZones gives what upstream's world_map::DataRepository::loadZones
// builds from the DBCs - the std::vector<world_map::Zone> that upstream's own
// coordinate_projection / exploration_state / view_state_machine (linked unchanged)
// read. PSP: the DBC walk happens on the PC (the converter), not at run time.
#pragma once

#include "rendering/psp/psp_minimap_format.hpp"
#include "rendering/world_map/world_map_types.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::rendering::psp {

struct MinimapHeaderView {
    const MinimapHeader* h = nullptr;
    const MinimapArea* areas = nullptr;
    const char* names = nullptr;
    uint32_t namesSize = 0;

    bool attach(const void* blob, uint32_t size) {
        h = nullptr;
        if (!blob || size < sizeof(MinimapHeader)) return false;
        const auto* p = static_cast<const uint8_t*>(blob);
        const auto* hh = reinterpret_cast<const MinimapHeader*>(p);
        if (std::memcmp(hh->magic, "WMMP", 4) != 0 || hh->version != kMinimapVersion) return false;
        const uint64_t need = sizeof(MinimapHeader) + uint64_t{hh->areaCount} * sizeof(MinimapArea) + hh->namesSize;
        if (need > size || hh->namesSize == 0) return false;
        const auto* a = reinterpret_cast<const MinimapArea*>(p + sizeof(MinimapHeader));
        const char* n = reinterpret_cast<const char*>(a + hh->areaCount);
        if (n[hh->namesSize - 1] != '\0') return false;
        for (uint32_t i = 0; i < hh->areaCount; ++i) {
            if (a[i].nameOffset >= hh->namesSize) return false;
            if (i && a[i].id <= a[i - 1].id) return false;  // sorted, unique
        }
        h = hh;
        areas = a;
        names = n;
        namesSize = hh->namesSize;
        return true;
    }
    const MinimapArea* area(uint32_t id) const {
        if (!h) return nullptr;
        uint32_t lo = 0, hi = h->areaCount;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (areas[mid].id < id) lo = mid + 1;
            else hi = mid;
        }
        return lo < h->areaCount && areas[lo].id == id ? &areas[lo] : nullptr;
    }
    const char* name(uint32_t id) const {
        const MinimapArea* a = area(id);
        return a && a->nameOffset && names[a->nameOffset] ? names + a->nameOffset : nullptr;
    }
    // upstream DataRepository::exploreFlagByAreaId_
    void exploreFlags(std::unordered_map<uint32_t, uint32_t>& out) const {
        out.clear();
        if (!h) return;
        for (uint32_t i = 0; i < h->areaCount; ++i) out[areas[i].id] = areas[i].exploreFlag;
    }
};

struct WorldMapView {
    const WorldMapHeader* h = nullptr;
    const WorldMapZone* zones = nullptr;
    const WorldMapOverlay* overlays = nullptr;
    const uint32_t* explore = nullptr;
    const uint16_t* zmp = nullptr;
    const char* names = nullptr;

    bool attach(const void* blob, uint32_t size) {
        h = nullptr;
        if (!blob || size < sizeof(WorldMapHeader)) return false;
        const auto* p = static_cast<const uint8_t*>(blob);
        const auto* hh = reinterpret_cast<const WorldMapHeader*>(p);
        if (std::memcmp(hh->magic, "WWMP", 4) != 0 || hh->version != kWorldMapVersion) return false;
        if (hh->zmpCells != 0 && hh->zmpCells != 128u * 128u) return false;
        uint64_t off = sizeof(WorldMapHeader);
        const uint64_t zoneOff = off;
        off += uint64_t{hh->zoneCount} * sizeof(WorldMapZone);
        const uint64_t ovOff = off;
        off += uint64_t{hh->overlayCount} * sizeof(WorldMapOverlay);
        const uint64_t exOff = off;
        off += uint64_t{hh->exploreCount} * 4u;
        const uint64_t zmpOff = off;
        off += uint64_t{hh->zmpCells} * 2u;
        const uint64_t namesOff = off;
        off += hh->namesSize;
        if (off > size || hh->namesSize == 0 || p[namesOff + hh->namesSize - 1] != 0) return false;
        const auto* z = reinterpret_cast<const WorldMapZone*>(p + zoneOff);
        for (uint32_t i = 0; i < hh->zoneCount; ++i) {
            if (z[i].folderOffset >= hh->namesSize || z[i].nameOffset >= hh->namesSize) return false;
            if (uint32_t{z[i].firstOverlay} + z[i].overlayCount > hh->overlayCount) return false;
            if (uint64_t{z[i].firstExplore} + z[i].exploreCount > hh->exploreCount) return false;
        }
        h = hh;
        zones = z;
        overlays = reinterpret_cast<const WorldMapOverlay*>(p + ovOff);
        explore = reinterpret_cast<const uint32_t*>(p + exOff);
        zmp = hh->zmpCells ? reinterpret_cast<const uint16_t*>(p + zmpOff) : nullptr;
        names = reinterpret_cast<const char*>(p + namesOff);
        return true;
    }
    const char* str(uint32_t off) const { return h && off < h->namesSize ? names + off : ""; }

    // upstream DataRepository::loadZones (zones_ + their overlays), same order as baked
    // (WorldMapArea.dbc file order), so an index here is an index there.
    void buildZones(std::vector<world_map::Zone>& out) const {
        out.clear();
        if (!h) return;
        out.reserve(h->zoneCount);
        for (uint32_t i = 0; i < h->zoneCount; ++i) {
            const WorldMapZone& s = zones[i];
            world_map::Zone z;
            z.wmaID = s.wmaId;
            z.mapID = s.mapId;
            z.areaID = s.areaId;
            z.areaName = str(s.folderOffset);
            z.bounds.locLeft = s.locLeft;
            z.bounds.locRight = s.locRight;
            z.bounds.locTop = s.locTop;
            z.bounds.locBottom = s.locBottom;
            z.displayMapID = s.displayMapId;
            z.parentWorldMapID = s.parentWorldMapId;
            // A zone of another map shown on this one (DisplayMapID) - its WorldMapTransforms
            // offset in server X / Y (psp_minimap_conv readWorldMap; the bounds above are already moved by it).
            z.virtualOffsetWowX = s.virtualOffsetX;
            z.virtualOffsetWowY = s.virtualOffsetY;
            z.exploreBits.assign(explore + s.firstExplore, explore + s.firstExplore + s.exploreCount);
            for (uint32_t k = 0; k < s.overlayCount; ++k) {
                const WorldMapOverlay& o = overlays[s.firstOverlay + k];
                world_map::OverlayEntry ov;
                for (int a = 0; a < 4; ++a) ov.areaIDs[a] = o.areaIds[a];
                ov.texWidth = o.texWidth;
                ov.texHeight = o.texHeight;
                ov.offsetX = o.offsetX;
                ov.offsetY = o.offsetY;
                ov.tileCols = (o.texWidth + 255) / 256;
                ov.tileRows = (o.texHeight + 255) / 256;
                ov.hitRectLeft = o.hitLeft;
                ov.hitRectRight = o.hitRight;
                ov.hitRectTop = o.hitTop;
                ov.hitRectBottom = o.hitBottom;
                z.overlays.push_back(std::move(ov));
            }
            out.push_back(std::move(z));
        }
    }

    // upstream WorldMapFacade::zoneAtMapPoint's ZMP step: continent-map world
    // coordinates -> the AreaTable id of that cell (0 = none).
    uint32_t zmpArea(float serverY, float serverX) const {
        if (!zmp) return 0;
        constexpr float kWorldSize = 64.0f * kMinimapTileYards;
        const float zx = 0.5f - serverY / kWorldSize;  // upstream: 0.5 - wowX / size (wowX = render.y = serverY)
        const float zy = 0.5f - serverX / kWorldSize;
        if (zx < 0.0f || zx > 1.0f || zy < 0.0f || zy > 1.0f) return 0;
        const int col = std::clamp(static_cast<int>(zx * 128.0f), 0, 127);
        const int row = std::clamp(static_cast<int>(zy * 128.0f), 0, 127);
        return zmp[row * 128 + col];
    }
};

// Pak entry names (<= 47 chars) - one place for the converter and the PSP.
inline void minimapTileName(char* out, size_t n, int x, int y) { std::snprintf(out, n, "t_%d_%d", x, y); }
inline void worldMapTileName(char* out, size_t n, uint32_t wma, int tile) {
    std::snprintf(out, n, "w_%u_%d", static_cast<unsigned>(wma), tile);
}
inline void worldMapOverlayName(char* out, size_t n, uint32_t wma, int idx) {
    std::snprintf(out, n, "o_%u_%d", static_cast<unsigned>(wma), idx);
}

// ---------------------------------------------------------------------------
// The area under the feet of every map, and the world map's
// "World" / "Cosmic" levels.
// ---------------------------------------------------------------------------

// "agrid" entry of data/minimap_<map>.pak: the MCNK areaId of every chunk of the map's ADT tiles
// (upstream Renderer::getCurrentZoneId step 1, ADTLoader chunk order). Was: data/audio.pak
// "a_<x>_<y>" - the grids of one map (psp_audio_conv --map <map>) used on every map, every one of
// them in RAM (512 B a tile). Per tile: a palette of the distinct area ids, then the chunk indices -
// none (one area), 4 bits (<= 16 areas, two a byte, low nibble first) or 8 bits.
// Layout: AreaGridHeader, AreaGridTile[tileCount] (ascending key), the tiles' data.
constexpr uint32_t kAreaGridVersion = 1;
struct AreaGridHeader {
    char magic[4];       // "WAGR"
    uint32_t version;    // kAreaGridVersion
    uint32_t mapId;
    uint32_t tileCount;
};
static_assert(sizeof(AreaGridHeader) == 16, "area grid header");
struct AreaGridTile {
    uint16_t key;           // x << 8 | y (ADT file name map_<x>_<y>)
    uint16_t paletteCount;  // 1..256
    uint32_t offset;        // from the start of the data (after the tile table)
};
static_assert(sizeof(AreaGridTile) == 8, "area grid tile");
inline uint32_t areaGridIndexBytes(uint32_t paletteCount) {
    return paletteCount <= 1 ? 0u : paletteCount <= 16 ? 128u : 256u;
}

struct AreaGridView {
    const AreaGridHeader* h = nullptr;
    const AreaGridTile* tiles = nullptr;
    const uint8_t* data = nullptr;
    uint32_t dataSize = 0;

    bool attach(const void* blob, uint32_t size) {
        h = nullptr;
        if (!blob || size < sizeof(AreaGridHeader)) return false;
        const auto* p = static_cast<const uint8_t*>(blob);
        const auto* hh = reinterpret_cast<const AreaGridHeader*>(p);
        if (std::memcmp(hh->magic, "WAGR", 4) != 0 || hh->version != kAreaGridVersion || hh->tileCount > 64u * 64u)
            return false;
        const uint64_t tableEnd = sizeof(AreaGridHeader) + uint64_t{hh->tileCount} * sizeof(AreaGridTile);
        if (tableEnd > size) return false;
        const auto* t = reinterpret_cast<const AreaGridTile*>(p + sizeof(AreaGridHeader));
        const uint32_t ds = size - static_cast<uint32_t>(tableEnd);
        for (uint32_t i = 0; i < hh->tileCount; ++i) {
            if (t[i].paletteCount == 0 || t[i].paletteCount > 256) return false;
            if (i && t[i].key <= t[i - 1].key) return false;  // sorted, unique
            const uint64_t end = uint64_t{t[i].offset} + t[i].paletteCount * 2u + areaGridIndexBytes(t[i].paletteCount);
            if (end > ds) return false;
        }
        h = hh;
        tiles = t;
        data = p + tableEnd;
        dataSize = ds;
        return true;
    }
    // The chunk's area id (chunk = upstream ADTLoader's index, file order), 0 = no tile / no area.
    uint32_t areaAt(int x, int y, int chunk) const {
        if (!h || x < 0 || y < 0 || x > 255 || y > 255 || chunk < 0 || chunk >= 256) return 0;
        const uint16_t key = static_cast<uint16_t>(x << 8 | y);
        uint32_t lo = 0, hi = h->tileCount;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (tiles[mid].key < key) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= h->tileCount || tiles[lo].key != key) return 0;
        const AreaGridTile& t = tiles[lo];
        const uint8_t* pal = data + t.offset;
        const uint8_t* ix = pal + t.paletteCount * 2u;
        uint32_t k = 0;
        if (t.paletteCount > 16) k = ix[chunk];
        else if (t.paletteCount > 1) k = (ix[chunk >> 1] >> ((chunk & 1) * 4)) & 0xFu;
        if (k >= t.paletteCount) return 0;
        return static_cast<uint32_t>(pal[k * 2]) | static_cast<uint32_t>(pal[k * 2 + 1]) << 8;
    }
};

// data/worldmap.pak (psp_minimap_conv --world): what the client shows above the continents.
//   "wlhdr"        WorldLevelHeader, WorldLevelContinent[continentCount] (WorldMapContinent.dbc rows)
//   "wt_<n>"       tile n (0..11, 4 x 3) of Interface\WorldMap\World\World<n+1> (WorldMapFrame_Update: no
//                  map file at continent 0 -> "World"), scaled to baseTile texels, WTEX T8
//   "wh_<map>"     Interface\WorldMap\<Folder>\<Folder>Highlight of a continent on that map (WorldMapHighlight,
//                  ADD), the whole 512 x 512 texture, WTEX T8
//   "ct_<n>"       Interface\WorldMap\Cosmic\Cosmic<n+1> (continent WORLDMAP_COSMIC_ID)
//   "ch_<map>"     the cosmic map buttons' HighlightTexture: the highlight of map 0's button,
//                  the highlight of map 530's button, WTEX T8 with alpha
// The world level's entries (wt_, wh_) and the cosmic level's (ct_, ch_) are consecutive: one read each.
constexpr uint32_t kWorldLevelVersion = 1;
struct WorldLevelHeader {
    char magic[4];            // "WWLD"
    uint32_t version;         // kWorldLevelVersion
    uint32_t continentCount;
    uint16_t baseTile;        // texels of a baked 256 px tile (128)
    uint16_t flags;           // bit 0: world tiles baked, bit 1: cosmic tiles baked
};
static_assert(sizeof(WorldLevelHeader) == 16, "world level header");
// WorldMapContinent.dbc (3.3.5a, 14 fields): the continent's ADT tile box (inclusive) and where the client
// puts it on the world map: offset and scale in tiles. worldMapId 1 = on the continents' world map (maps 1,
// 0 and 571), 0 = not (map 530 - the cosmic map). taxiMin / taxiMax are not used here.
struct WorldLevelContinent {
    uint32_t id, mapId;
    int32_t left, right, top, bottom;
    float offsetX, offsetY, scale;
    uint32_t worldMapId;
    uint32_t flags;           // bit 0: "wh_<mapId>" baked
    char folder[20];          // Interface\WorldMap\<folder> (the continent's WorldMapArea row, areaId 0)
};
static_assert(sizeof(WorldLevelContinent) == 64, "world level continent");

// Where a continent's tile coordinates land on the world map, in its 1002 x 668 map units (the client's
// WorldMapDetailFrame): 16 units a tile x scale around the map's centre (tile 32 = 501, 334). Derived from the
// DBC and checked twice: each <Folder>Highlight lies on the World art's coast within 1 unit
// when anchored at (left, top) with a side of max(right - left + 1, bottom - top + 1) tiles, and the continent
// maps' corners land where Astrolabe's measured world-map offsets put them (maps 0 and 571 to
// 0.1 unit). The client's own GetPlayerMapPosition on the world map is this, which the PSP has no code of.
constexpr float kWorldLevelUnitsPerTile = 16.0f;
inline void worldLevelPoint(const WorldLevelContinent& c, float tileX, float tileY, float& mapX, float& mapY) {
    mapX = 501.0f + kWorldLevelUnitsPerTile * ((tileX - 32.0f) * c.scale + c.offsetX);
    mapY = 334.0f + kWorldLevelUnitsPerTile * ((tileY - 32.0f) * c.scale + c.offsetY);
}
// The continent's box (its tiles) and its highlight's square, in map units.
inline void worldLevelBox(const WorldLevelContinent& c, float& x, float& y, float& w, float& h) {
    worldLevelPoint(c, static_cast<float>(c.left), static_cast<float>(c.top), x, y);
    w = kWorldLevelUnitsPerTile * c.scale * static_cast<float>(c.right - c.left + 1);
    h = kWorldLevelUnitsPerTile * c.scale * static_cast<float>(c.bottom - c.top + 1);
}
inline float worldLevelHighlightSide(const WorldLevelContinent& c) {
    return kWorldLevelUnitsPerTile * c.scale * static_cast<float>(std::max(c.right - c.left + 1, c.bottom - c.top + 1));
}

struct WorldLevelView {
    const WorldLevelHeader* h = nullptr;
    const WorldLevelContinent* conts = nullptr;
    bool attach(const void* blob, uint32_t size) {
        h = nullptr;
        if (!blob || size < sizeof(WorldLevelHeader)) return false;
        const auto* hh = static_cast<const WorldLevelHeader*>(blob);
        if (std::memcmp(hh->magic, "WWLD", 4) != 0 || hh->version != kWorldLevelVersion || hh->continentCount > 16)
            return false;
        if (sizeof(WorldLevelHeader) + uint64_t{hh->continentCount} * sizeof(WorldLevelContinent) > size) return false;
        h = hh;
        conts = reinterpret_cast<const WorldLevelContinent*>(static_cast<const uint8_t*>(blob) + sizeof(WorldLevelHeader));
        return true;
    }
    const WorldLevelContinent* byMap(uint32_t mapId) const {
        for (uint32_t i = 0; h && i < h->continentCount; ++i)
            if (conts[i].mapId == mapId) return &conts[i];
        return nullptr;
    }
};

inline void worldLevelTileName(char* out, size_t n, bool cosmic, int tile) {
    std::snprintf(out, n, cosmic ? "ct_%d" : "wt_%d", tile);
}
inline void worldLevelHighlightName(char* out, size_t n, bool cosmic, uint32_t mapId) {
    std::snprintf(out, n, cosmic ? "ch_%u" : "wh_%u", static_cast<unsigned>(mapId));
}

// The cosmic map's buttons (WorldMapFrame.xml: the buttons of map 530 and map 0, children of WorldMapButton,
// 1002 x 668 units, y down): the button rect (OnClick SetMapZoom) and its HighlightTexture's rect (CENTER of
// the button + the XML offset, y up there).
struct CosmicButton {
    uint32_t mapId;          // SetMapZoom(0) = the continents' world map, SetMapZoom(3) = map 530
    float x, y, size;        // the button
    float hlX, hlY, hlW, hlH;
};
inline const CosmicButton* cosmicButtons(int& count) {
    static const CosmicButton k[2] = {
        {530, 115.0f, 90.0f, 320.0f, 115.0f + 160.0f + 176.0f - 428.0f, 90.0f + 160.0f + 87.0f - 302.5f, 856.0f, 605.0f},
        {0, 593.0f, 255.0f, 366.0f, 593.0f + 183.0f - 224.0f - 449.0f, 255.0f + 183.0f - 104.0f - 323.5f, 898.0f, 647.0f},
    };
    count = 2;
    return k;
}

}  // namespace wowee::rendering::psp
