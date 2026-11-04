// psp_minimap_conv - minimap tiles, area names and the world map for the PSP
// -> data/minimap_<map>.pak (include/rendering/psp/psp_minimap_format.hpp).
//
//   psp_minimap_conv --extracted <dir> [--map <map>] [--around X Y R]... [--all]
//                    [--tile 128] [--worldmap near|all|none] [--wmtile 128]
//                    [--out <psp/bin/data/minimap_<map>.pak>] [--verify] [--dump <dir>]
//   psp_minimap_conv --extracted <dir> [--map <map>] [--around X Y R] --list-assets
//   psp_minimap_conv --selftest [--dump <dir>]
//
// What upstream reads at run time, baked on the PC:
//  - minimap tiles: Textures\Minimap\md5translate.trs -> "<Map>\map<x>_<y>" -> hashed BLP
//    (upstream Minimap::parseTRS / getOrLoadTileTexture), 256 px -> --tile texels (128),
//    T8 swizzled + mips on the same CLUT; only the tiles of --around boxes (tile X Y =
//    ADT file names, radius R tiles) or --all of the map;
//  - area names: AreaTable.dbc (id, parent, ExploreFlag, name in the client's locale column)
//    for the map's areas + every area a world map row references (minimap zone text,
//    upstream getAreaName / ZoneManager);
//  - world map: WorldMapArea / WorldMapOverlay / WorldMapTransforms rows exactly as
//    upstream world_map::DataRepository::loadZones builds them, the continent's ZMP
//    (Interface\WorldMap\<continent>.zmp) as uint16 area ids, and for --worldmap near
//    (continents + zones touching the --around boxes) / all the zone textures
//    Interface\WorldMap\<folder>\<folder><1..12>.blp and the overlays <texture><n>.blp
//    composited (upstream CompositeRenderer), scaled 256 -> --wmtile.
//
//  - "agrid", the MCNK area id of every chunk of the map's ADT tiles (the
//    tiles its WDT lists) - the area under the feet (zone music, the minimap's zone text) per map; was
//    data/audio.pak "a_<x>_<y>", one map's grids for every map (psp_worldmap_view.hpp AreaGridView).
//  - WorldMapTransforms moves a zone shown on another map (DisplayMapID:
//    a zone of map 530 on map 0, its islands on map 1) along the data's own axes
//    (PSP: upstream swaps them, readWorldMap); --verify checks those zones against the continent's ZMP.
//
//   psp_minimap_conv --extracted <dir> --world [--wmtile 128] [--out <psp/bin/data/worldmap.pak>]
//                    [--verify] [--dump <dir>]
// The world map's levels above the continents -> data/worldmap.pak
// (psp_worldmap_view.hpp WorldLevelView): WorldMapContinent.dbc, Interface\WorldMap\World\World<1..12>
// and each continent's <Folder>Highlight (the "World" level), Interface\WorldMap\Cosmic\Cosmic<1..12>
// and the cosmic buttons' highlights (the "Cosmic" level). --dump: both levels with every highlight
// added where the PSP draws it (world_level.png, cosmic_level.png).
//
// --verify     reads the pak back through the PSP's readers (psp_worldmap_view.hpp): headers,
//              every WTEX, the zone blocks being consecutive, the tiles of the boxes present,
//              upstream's projection bounds per continent. Exit code 3 on an error.
// --dump <dir> PNGs: every tile, the stitched region (north up), each baked world map zone
//              (base + every overlay, like a fully explored map), areas.tsv.
// --list-assets  the MPQ paths psp_mpq_extract must extract for these options.
// --selftest   synthetic data, no client files (runs without client data): tile/pixel math, the disc
//              clipping and rotation used by the PSP, and a pak round trip.
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/psp/psp_minimap_format.hpp"
#include "rendering/psp/psp_worldmap_view.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include "rendering/world_map/exploration_state.hpp"

#include "forest_bake.hpp"  // writePng, PakView, decodeWtex (+ conv_common.hpp)
#include "core/coordinates.hpp"  // adtToWorld (dungeon_minimap_bake.hpp)

#include <glm/glm.hpp>                   // dungeon_minimap_bake.hpp
#include <glm/gtc/matrix_transform.hpp>  // dungeon_minimap_bake.hpp

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace wowee;
using namespace wowee::pspconv;
namespace fmt = wowee::rendering::psp;
namespace wm = wowee::rendering::world_map;

namespace {

constexpr uint32_t kKindRaw = 3;

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0;
    std::vector<Rgb> px;
    Rgb& at(int x, int y) { return px[static_cast<size_t>(y) * w + x]; }
    const Rgb& at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

Image fromBlp(const pipeline::BLPImage& b) {
    Image img;
    img.w = b.width;
    img.h = b.height;
    img.px.resize(static_cast<size_t>(img.w) * img.h);
    for (size_t i = 0; i < img.px.size(); ++i) {
        const uint8_t* p = &b.data[i * 4];
        img.px[i] = Rgb{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    }
    return img;
}

bool loadBlp(const std::string& path, Image& out) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) return false;
    pipeline::BLPImage b = pipeline::BLPLoader::load(bytes);
    if (b.data.empty() || b.width <= 0 || b.height <= 0) return false;
    out = fromBlp(b);
    return true;
}

// Area-weighted resample (any ratio), colour weighted by alpha: no dark fringes.
Image resample(const Image& src, int w, int h) {
    Image dst;
    dst.w = w;
    dst.h = h;
    dst.px.assign(static_cast<size_t>(w) * h, Rgb{0, 0, 0, 0});
    const float sx = static_cast<float>(src.w) / w, sy = static_cast<float>(src.h) / h;
    for (int y = 0; y < h; ++y) {
        const float y0 = y * sy, y1 = (y + 1) * sy;
        for (int x = 0; x < w; ++x) {
            const float x0 = x * sx, x1 = (x + 1) * sx;
            float r = 0, g = 0, b = 0, a = 0, wsum = 0;
            for (int yy = static_cast<int>(y0); yy < src.h && yy < y1; ++yy) {
                const float wy = std::min(y1, yy + 1.0f) - std::max(y0, static_cast<float>(yy));
                if (wy <= 0) continue;
                for (int xx = static_cast<int>(x0); xx < src.w && xx < x1; ++xx) {
                    const float wx = std::min(x1, xx + 1.0f) - std::max(x0, static_cast<float>(xx));
                    if (wx <= 0) continue;
                    const Rgb& q = src.at(xx, yy);
                    const float wgt = wx * wy;
                    r += q.r * q.a * wgt;
                    g += q.g * q.a * wgt;
                    b += q.b * q.a * wgt;
                    a += q.a * wgt;
                    wsum += wgt;
                }
            }
            Rgb& o = dst.at(x, y);
            if (a > 1e-5f) o = Rgb{r / a, g / a, b / a, wsum > 0 ? a / wsum : 0.0f};
            else o = Rgb{0, 0, 0, 0};
        }
    }
    return dst;
}

// A channel-wise maximum over each source block (the continent highlights are outlines a
// texel wide at 512: an average would fade them at the PSP's scale, the maximum keeps them a texel wide).
Image maxPool(const Image& src, int w, int h) {
    Image dst;
    dst.w = w;
    dst.h = h;
    dst.px.assign(static_cast<size_t>(w) * h, Rgb{0, 0, 0, 1});
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int x0 = x * src.w / w, x1 = std::max(x0 + 1, (x + 1) * src.w / w);
            const int y0 = y * src.h / h, y1 = std::max(y0 + 1, (y + 1) * src.h / h);
            Rgb m{0, 0, 0, 0};
            for (int yy = y0; yy < y1 && yy < src.h; ++yy)
                for (int xx = x0; xx < x1 && xx < src.w; ++xx) {
                    const Rgb& q = src.at(xx, yy);
                    m.r = std::max(m.r, q.r);
                    m.g = std::max(m.g, q.g);
                    m.b = std::max(m.b, q.b);
                    m.a = std::max(m.a, q.a);
                }
            dst.at(x, y) = m;
        }
    return dst;
}

int pow2Ceil(int v, int minimum) {
    int p = minimum;
    while (p < v) p *= 2;
    return p;
}

uint8_t nearestIndex(const Rgb& c, const std::array<Rgb, 256>& pal) {
    int best = 0;
    float bestD = 1e9f;
    for (int i = 0; i < 256; ++i) {
        const float dr = c.r - pal[i].r, dg = c.g - pal[i].g, db = c.b - pal[i].b, da = c.a - pal[i].a;
        const float d = dr * dr + dg * dg + db * db + 2.0f * da * da;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return static_cast<uint8_t>(best);
}

// Content picture (cw x ch) -> WTEX T8, swizzled, CLUT 8888, power-of-two storage
// (rows >= 16 bytes, 8-row blocks, edge texels repeated into the padding so linear
// filtering never pulls the padding in), optional mips on the same CLUT (models' rule).
std::vector<uint8_t> encodeWtex(const Image& content, bool mips) {
    const int w = pow2Ceil(content.w, 16), h = pow2Ceil(content.h, 8);
    Image img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.at(x, y) = content.at(std::min(x, content.w - 1), std::min(y, content.h - 1));
    bool hasAlpha = false;
    for (const Rgb& p : content.px)
        if (p.a < 0.98f) {
            hasAlpha = true;
            break;
        }
    std::vector<uint8_t> idx;
    std::array<Rgb, 256> pal;
    quantize(img.px, idx, pal);
    std::vector<std::vector<uint8_t>> levels;
    if (mips) {
        Image m = img;
        while (levels.size() < 7 && m.w / 2 >= 16 && m.h / 2 >= 8) {
            m = resample(m, m.w / 2, m.h / 2);
            std::vector<uint8_t> mi(m.px.size());
            for (size_t i = 0; i < m.px.size(); ++i) mi[i] = nearestIndex(m.px[i], pal);
            levels.push_back(swizzle(mi, m.w, m.h));
        }
    }
    std::vector<uint8_t> blob = {'W', 'T', 'E', 'X'};
    blob.push_back(5);  // GU_PSM_T8
    blob.push_back(1);  // swizzled
    blob.push_back(static_cast<uint8_t>((hasAlpha ? 1 : 0) | 4 | (levels.empty() ? 0 : 8)));  // alpha|linear|mips
    blob.push_back(static_cast<uint8_t>(levels.size()));
    put<uint16_t>(blob, static_cast<uint16_t>(w));
    put<uint16_t>(blob, static_cast<uint16_t>(h));
    put<uint16_t>(blob, static_cast<uint16_t>(content.w));
    put<uint16_t>(blob, static_cast<uint16_t>(content.h));
    put<uint16_t>(blob, 256);
    put<uint16_t>(blob, 0);
    put<uint32_t>(blob, 0);  // 9-slice margins
    put<uint32_t>(blob, 32);
    put<uint32_t>(blob, 32 + 1024);
    for (const Rgb& p : pal) put<uint32_t>(blob, to8888(p));
    const std::vector<uint8_t> sw = swizzle(idx, w, h);
    blob.insert(blob.end(), sw.begin(), sw.end());
    for (const auto& l : levels) {
        alignTo(blob, 16);
        blob.insert(blob.end(), l.begin(), l.end());
    }
    return blob;
}

bool decodeContent(const uint8_t* b, uint32_t size, Image& out) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!decodeWtex(b, size, w, h, rgba)) return false;
    uint16_t uw = 0, uh = 0;
    std::memcpy(&uw, b + 12, 2);
    std::memcpy(&uh, b + 14, 2);
    if (uw == 0 || uh == 0 || uw > w || uh > h) return false;
    out.w = uw;
    out.h = uh;
    out.px.resize(static_cast<size_t>(uw) * uh);
    for (int y = 0; y < uh; ++y)
        for (int x = 0; x < uw; ++x) {
            const uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            out.at(x, y) = Rgb{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
        }
    return true;
}

std::vector<uint8_t> toRgba8(const Image& img) {
    std::vector<uint8_t> o(img.px.size() * 4);
    for (size_t i = 0; i < img.px.size(); ++i) {
        o[i * 4] = static_cast<uint8_t>(quant(img.px[i].r, 255));
        o[i * 4 + 1] = static_cast<uint8_t>(quant(img.px[i].g, 255));
        o[i * 4 + 2] = static_cast<uint8_t>(quant(img.px[i].b, 255));
        o[i * 4 + 3] = static_cast<uint8_t>(quant(img.px[i].a, 255));
    }
    return o;
}

// Mip chain check of a baked WTEX (what textureFromBlob accepts on the PSP).
bool wtexSane(const uint8_t* b, uint32_t size, int& levelsOut) {
    if (size < 32 || std::memcmp(b, "WTEX", 4) != 0 || b[4] != 5) return false;
    uint16_t w, h;
    uint32_t clutOff, pixOff;
    std::memcpy(&w, b + 8, 2);
    std::memcpy(&h, b + 10, 2);
    std::memcpy(&clutOff, b + 24, 4);
    std::memcpy(&pixOff, b + 28, 4);
    if (w > 512 || h > 512 || (w & (w - 1)) || (h & (h - 1)) || pixOff % 16 || clutOff % 16) return false;
    uint32_t off = pixOff + static_cast<uint32_t>(w) * h;
    if (off > size) return false;
    levelsOut = 1;
    if (b[6] & 8) {
        for (int l = 1; l <= b[7]; ++l) {
            const uint32_t lw = w >> l, lh = h >> l;
            off = (off + 15u) & ~15u;
            if (lw < 16 || lh < 8 || off + lw * lh > size) return false;
            off += lw * lh;
            ++levelsOut;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pak building (shared by the real bake and the selftest)
// ---------------------------------------------------------------------------

struct Names {
    std::vector<char> data{'\0'};  // offset 0 = ""
    std::map<std::string, uint32_t> seen;
    uint32_t add(const std::string& s) {
        if (s.empty()) return 0;
        auto it = seen.find(s);
        if (it != seen.end()) return it->second;
        const uint32_t off = static_cast<uint32_t>(data.size());
        data.insert(data.end(), s.begin(), s.end());
        data.push_back('\0');
        seen.emplace(s, off);
        return off;
    }
};

template <typename T>
void putStruct(std::vector<uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

struct AreaRow {
    uint32_t id = 0, mapId = 0, parent = 0, exploreFlag = 0;
    std::string name;
};

std::vector<uint8_t> buildMinimapHeader(uint32_t mapId, const std::string& mapName, int tileSize, int tileCount,
                                        std::vector<AreaRow> areas) {
    std::sort(areas.begin(), areas.end(), [](const AreaRow& a, const AreaRow& b) { return a.id < b.id; });
    areas.erase(std::unique(areas.begin(), areas.end(), [](const AreaRow& a, const AreaRow& b) { return a.id == b.id; }),
                areas.end());
    Names names;
    std::vector<fmt::MinimapArea> rows;
    for (const AreaRow& a : areas) rows.push_back({a.id, a.parent, a.exploreFlag, names.add(a.name)});
    fmt::MinimapHeader h{};
    std::memcpy(h.magic, "WMMP", 4);
    h.version = fmt::kMinimapVersion;
    h.mapId = mapId;
    std::snprintf(h.mapName, sizeof(h.mapName), "%s", mapName.c_str());
    h.tileSize = static_cast<uint16_t>(tileSize);
    h.tileCount = static_cast<uint16_t>(tileCount);
    h.areaCount = static_cast<uint32_t>(rows.size());
    h.namesSize = static_cast<uint32_t>(names.data.size());
    std::vector<uint8_t> out;
    putStruct(out, h);
    for (const auto& r : rows) putStruct(out, r);
    out.insert(out.end(), names.data.begin(), names.data.end());
    return out;
}

struct OverlayRow {
    fmt::WorldMapOverlay rec{};
    std::string texture;
};
struct ZoneRow {
    fmt::WorldMapZone rec{};
    std::string folder, name;
    std::vector<uint32_t> exploreBits;
    std::vector<OverlayRow> overlays;
    bool bake = false;
};

std::vector<uint8_t> buildWorldMapHeader(uint32_t mapId, int baseTile, std::vector<ZoneRow>& zones,
                                         const std::vector<uint16_t>& zmp) {
    Names names;
    std::vector<fmt::WorldMapOverlay> ovs;
    std::vector<uint32_t> explore;
    for (ZoneRow& z : zones) {
        z.rec.folderOffset = names.add(z.folder);
        z.rec.nameOffset = names.add(z.name);
        z.rec.firstExplore = static_cast<uint32_t>(explore.size());
        z.rec.exploreCount = static_cast<uint16_t>(z.exploreBits.size());
        explore.insert(explore.end(), z.exploreBits.begin(), z.exploreBits.end());
        z.rec.firstOverlay = static_cast<uint16_t>(ovs.size());
        z.rec.overlayCount = static_cast<uint16_t>(z.overlays.size());
        for (const OverlayRow& o : z.overlays) ovs.push_back(o.rec);
    }
    fmt::WorldMapHeader h{};
    std::memcpy(h.magic, "WWMP", 4);
    h.version = fmt::kWorldMapVersion;
    h.mapId = mapId;
    h.zoneCount = static_cast<uint32_t>(zones.size());
    h.overlayCount = static_cast<uint32_t>(ovs.size());
    h.exploreCount = static_cast<uint32_t>(explore.size());
    h.zmpCells = zmp.size() == 128u * 128u ? 128u * 128u : 0u;
    h.namesSize = static_cast<uint32_t>(names.data.size());
    h.baseTile = static_cast<uint16_t>(baseTile);
    std::vector<uint8_t> out;
    putStruct(out, h);
    for (const ZoneRow& z : zones) putStruct(out, z.rec);
    for (const auto& o : ovs) putStruct(out, o);
    for (uint32_t e : explore) put<uint32_t>(out, e);
    if (h.zmpCells)
        for (uint16_t a : zmp) put<uint16_t>(out, a);
    out.insert(out.end(), names.data.begin(), names.data.end());
    return out;
}

// A zone's world map art: 12 base tiles (upstream loadZoneTextures) and its overlays
// (loadOverlayTextures) - the pieces of an overlay composited into one picture first.
struct ZoneArt {
    Image base[12];
    bool haveBase[12] = {};
    std::vector<Image> overlays;  // index = overlay index in the zone (empty = none)
};

Image scaledForPsp(const Image& src, int baseTile) {
    const float s0 = static_cast<float>(baseTile) / 256.0f;
    const float s = std::min({s0, 512.0f / static_cast<float>(std::max(1, src.w)), 512.0f / std::max(1, src.h)});
    const int w = std::max(1, static_cast<int>(std::ceil(src.w * s))), h = std::max(1, static_cast<int>(std::ceil(src.h * s)));
    return resample(src, w, h);
}

void appendZoneArt(std::vector<PakItem>& items, const ZoneRow& z, const ZoneArt& art, int baseTile) {
    char name[48];
    for (int n = 0; n < 12; ++n) {
        if (!art.haveBase[n]) continue;
        fmt::worldMapTileName(name, sizeof(name), z.rec.wmaId, n);
        items.push_back({name, kKindTexture, encodeWtex(scaledForPsp(art.base[n], baseTile), false)});
    }
    for (size_t i = 0; i < art.overlays.size(); ++i) {
        if (art.overlays[i].w == 0) continue;
        fmt::worldMapOverlayName(name, sizeof(name), z.rec.wmaId, static_cast<int>(i));
        items.push_back({name, kKindTexture, encodeWtex(scaledForPsp(art.overlays[i], baseTile), false)});
    }
}

// "agrid" from the per-tile area ids (psp_worldmap_view.hpp AreaGridView).
// Tiles whose chunks are all area 0 are left out (areaAt answers 0 for them anyway).
std::vector<uint8_t> buildAreaGrid(uint32_t mapId, const std::map<std::pair<int, int>, std::array<uint16_t, 256>>& tiles,
                                   size_t* multiTiles = nullptr) {
    std::vector<std::pair<uint16_t, const std::array<uint16_t, 256>*>> keyed;
    for (const auto& [xy, ids] : tiles) {
        bool any = false;
        for (uint16_t a : ids) any = any || a != 0;
        if (any && xy.first >= 0 && xy.first < 256 && xy.second >= 0 && xy.second < 256)
            keyed.push_back({static_cast<uint16_t>(xy.first << 8 | xy.second), &ids});
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<fmt::AreaGridTile> table;
    std::vector<uint8_t> data;
    size_t multi = 0;
    for (const auto& [key, ids] : keyed) {
        std::vector<uint16_t> pal;
        uint8_t idx[256] = {};
        for (int c = 0; c < 256; ++c) {
            const uint16_t a = (*ids)[c];
            auto it = std::find(pal.begin(), pal.end(), a);
            if (it == pal.end()) {
                pal.push_back(a);
                it = pal.end() - 1;
            }
            idx[c] = static_cast<uint8_t>(it - pal.begin());
        }
        fmt::AreaGridTile t{};
        t.key = key;
        t.paletteCount = static_cast<uint16_t>(pal.size());
        t.offset = static_cast<uint32_t>(data.size());
        table.push_back(t);
        for (uint16_t a : pal) put<uint16_t>(data, a);
        if (pal.size() > 16) {
            data.insert(data.end(), idx, idx + 256);
        } else if (pal.size() > 1) {
            for (int c = 0; c < 256; c += 2) data.push_back(static_cast<uint8_t>(idx[c] | idx[c + 1] << 4));
        }
        if (pal.size() > 1) ++multi;
    }
    if (multiTiles) *multiTiles = multi;
    fmt::AreaGridHeader h{};
    std::memcpy(h.magic, "WAGR", 4);
    h.version = fmt::kAreaGridVersion;
    h.mapId = mapId;
    h.tileCount = static_cast<uint32_t>(table.size());
    std::vector<uint8_t> out;
    putStruct(out, h);
    for (const auto& t : table) putStruct(out, t);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// ---------------------------------------------------------------------------
// Verify / dump (the PSP's readers)
// ---------------------------------------------------------------------------

struct VerifyOpts {
    std::set<std::pair<int, int>> expectTiles;  // tiles of the boxes that md5translate knows
    std::string dumpDir;
    bool quiet = false;
};

int verifyDungeonRecords(PakView& pak, const std::string& dumpDir, bool quiet);  // dungeon_minimap_bake.hpp
int dmTilePixels(float extent);                                                       // dungeon_minimap_bake.hpp

int verifyPak(const std::string& path, const VerifyOpts& opt) {
    PakView pak;
    if (!pak.open(path)) {
        std::printf("verify: cannot read %s\n", path.c_str());
        return 1;
    }
    int errors = 0;
    auto bad = [&](const std::string& what) {
        if (++errors <= 40) std::printf("verify ERROR: %s\n", what.c_str());
    };
    const bool dump = !opt.dumpDir.empty();
    if (dump) std::filesystem::create_directories(opt.dumpDir);
    uint32_t size = 0;
    const uint8_t* mh = pak.find("mmhdr", size);
    fmt::MinimapHeaderView mv;
    if (!mh || !mv.attach(mh, size)) {
        bad("mmhdr missing or refused by MinimapHeaderView");
        return errors;
    }
    // Tiles.
    int tiles = 0, minX = 64, minY = 64, maxX = -1, maxY = -1;
    size_t tileBytes = 0;
    std::map<std::pair<int, int>, Image> decoded;
    for (const auto& [name, span] : pak.entries) {
        int x = 0, y = 0;
        if (std::sscanf(name.c_str(), "t_%d_%d", &x, &y) != 2) continue;
        ++tiles;
        tileBytes += span.second;
        int levels = 0;
        const uint8_t* b = pak.bytes.data() + span.first;
        Image img;
        if (!wtexSane(b, span.second, levels) || !decodeContent(b, span.second, img)) {
            bad("tile " + name + " is not a valid WTEX T8");
            continue;
        }
        if (img.w != mv.h->tileSize || img.h != mv.h->tileSize) bad("tile " + name + " size != header tileSize");
        if (levels < 2) bad("tile " + name + " has no mips");
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        if (dump) {
            writePng(opt.dumpDir + "/" + name + ".png", img.w, img.h, toRgba8(img));
            decoded[{x, y}] = std::move(img);
        }
    }
    if (tiles != mv.h->tileCount) bad("tileCount in mmhdr != t_ entries");
    for (const auto& [x, y] : opt.expectTiles) {
        char n[48];
        fmt::minimapTileName(n, sizeof(n), x, y);
        if (!pak.entries.count(n)) bad(std::string("tile of the boxes not baked: ") + n);
    }
    if (dump && !decoded.empty()) {  // the region stitched, north up (x east, y south)
        const int ts = mv.h->tileSize, W = (maxX - minX + 1) * ts, H = (maxY - minY + 1) * ts;
        if (W <= 8192 && H <= 8192) {
            Image m;
            m.w = W;
            m.h = H;
            m.px.assign(static_cast<size_t>(W) * H, Rgb{0.05f, 0.08f, 0.12f, 1.0f});  // upstream noData colour
            for (const auto& [k, img] : decoded)
                for (int y = 0; y < img.h; ++y)
                    for (int x = 0; x < img.w; ++x) m.at((k.first - minX) * ts + x, (k.second - minY) * ts + y) = img.at(x, y);
            writePng(opt.dumpDir + "/region.png", W, H, toRgba8(m));
        }
    }
    int named = 0;
    for (uint32_t i = 0; i < mv.h->areaCount; ++i)
        if (mv.areas[i].nameOffset) ++named;
    if (dump) {
        FILE* f = std::fopen((opt.dumpDir + "/areas.tsv").c_str(), "w");
        if (f) {
            std::fprintf(f, "# id\tparent\texploreFlag\tname\n");
            for (uint32_t i = 0; i < mv.h->areaCount; ++i)
                std::fprintf(f, "%u\t%u\t%u\t%s\n", mv.areas[i].id, mv.areas[i].parent, mv.areas[i].exploreFlag,
                             mv.names + mv.areas[i].nameOffset);
            std::fclose(f);
        }
    }
    // World map.
    const uint8_t* wh = pak.find("wmhdr", size);
    int zonesBaked = 0, overlaysBaked = 0;
    size_t wmBytes = 0;
    if (wh) {
        fmt::WorldMapView wv;
        if (!wv.attach(wh, size)) {
            bad("wmhdr refused by WorldMapView");
        } else {
            std::vector<wm::Zone> zones;
            wv.buildZones(zones);
            for (uint32_t zi = 0; zi < wv.h->zoneCount; ++zi) {
                const fmt::WorldMapZone& z = wv.zones[zi];
                // Consecutive block: the PSP reads [first entry, last entry end) in one go.
                uint32_t lo = ~0u, hi = 0, sum = 0, count = 0;
                char n[48];
                Image canvas;
                const int bt = wv.h->baseTile;
                if (dump && z.tileMask) {
                    canvas.w = bt * 4;
                    canvas.h = bt * 3;
                    canvas.px.assign(static_cast<size_t>(canvas.w) * canvas.h, Rgb{0.05f, 0.08f, 0.12f, 1.0f});
                }
                for (int t = 0; t < 12; ++t) {
                    if (!(z.tileMask & (1u << t))) continue;
                    fmt::worldMapTileName(n, sizeof(n), z.wmaId, t);
                    uint32_t es = 0;
                    const uint8_t* b = pak.find(n, es);
                    Image img;
                    if (!b || !decodeContent(b, es, img)) {
                        bad(std::string("world map tile missing/bad: ") + n);
                        continue;
                    }
                    const uint32_t off = static_cast<uint32_t>(b - pak.bytes.data());
                    lo = std::min(lo, off);
                    hi = std::max(hi, off + es);
                    sum += (es + 63u) & ~63u;
                    ++count;
                    wmBytes += es;
                    if (dump) {
                        const Image s = resample(img, bt, bt);
                        for (int y = 0; y < bt; ++y)
                            for (int x = 0; x < bt; ++x) canvas.at((t % 4) * bt + x, (t / 4) * bt + y) = s.at(x, y);
                    }
                }
                for (uint32_t k = 0; k < z.overlayCount; ++k) {
                    const fmt::WorldMapOverlay& o = wv.overlays[z.firstOverlay + k];
                    if (!o.baked) continue;
                    fmt::worldMapOverlayName(n, sizeof(n), z.wmaId, static_cast<int>(k));
                    uint32_t es = 0;
                    const uint8_t* b = pak.find(n, es);
                    Image img;
                    if (!b || !decodeContent(b, es, img)) {
                        bad(std::string("overlay missing/bad: ") + n);
                        continue;
                    }
                    const uint32_t off = static_cast<uint32_t>(b - pak.bytes.data());
                    lo = std::min(lo, off);
                    hi = std::max(hi, off + es);
                    sum += (es + 63u) & ~63u;
                    ++count;
                    ++overlaysBaked;
                    wmBytes += es;
                    if (dump && canvas.w) {  // alpha over, at the map's scale
                        const float s = bt / 256.0f;
                        const int dw = std::max(1, static_cast<int>(o.texWidth * s)), dh = std::max(1, static_cast<int>(o.texHeight * s));
                        const Image r = resample(img, dw, dh);
                        const int ox = static_cast<int>(o.offsetX * s), oy = static_cast<int>(o.offsetY * s);
                        for (int y = 0; y < dh; ++y)
                            for (int x = 0; x < dw; ++x) {
                                const int cx = ox + x, cy = oy + y;
                                if (cx < 0 || cy < 0 || cx >= canvas.w || cy >= canvas.h) continue;
                                const Rgb& q = r.at(x, y);
                                Rgb& d = canvas.at(cx, cy);
                                d = Rgb{d.r + (q.r - d.r) * q.a, d.g + (q.g - d.g) * q.a, d.b + (q.b - d.b) * q.a, 1.0f};
                            }
                    }
                }
                if (count) {
                    ++zonesBaked;
                    // Entries are 64-byte aligned one after another: the span equals the aligned sum.
                    if (((hi + 63u) & ~63u) - lo != sum) bad("zone " + std::to_string(z.wmaId) + " entries not consecutive");
                }
                if (dump && canvas.w)
                    writePng(opt.dumpDir + "/worldmap_" + std::to_string(z.wmaId) + "_" + wv.str(z.folderOffset) + ".png",
                             canvas.w, canvas.h, toRgba8(canvas));
            }
            // Upstream's projection over the rebuilt zones: every continent has bounds.
            int continents = 0;
            for (int i = 0; i < static_cast<int>(zones.size()); ++i) {
                if (zones[i].areaID != 0) continue;
                ++continents;
                float l, r, t, b;
                if (!wm::getContinentProjectionBounds(zones, i, l, r, t, b))
                    bad("continent " + zones[i].areaName + " has no projection bounds");
            }
            if (!opt.quiet)
                std::printf("verify worldmap: zones=%u continents=%d baked=%d overlays=%u baked=%d zmp=%d kb=%zu\n",
                            wv.h->zoneCount, continents, zonesBaked, wv.h->overlayCount, overlaysBaked,
                            wv.zmp ? 1 : 0, wmBytes / 1024);
            // A zone of another map shown on this continent (WorldMapArea DisplayMapID:
            // map 530's islands on map 1, a zone of map 530 on map 0) must sit where the client's own
            // ZMP draws its area - most of the area's cells inside its shifted box (readWorldMap's transform).
            if (wv.zmp) {
                constexpr float kWorld = 64.0f * fmt::kMinimapTileYards;
                int virt = 0, offBox = 0;
                for (uint32_t zi = 0; zi < wv.h->zoneCount; ++zi) {
                    const fmt::WorldMapZone& z = wv.zones[zi];
                    if (z.areaId == 0 || z.mapId == mv.h->mapId || z.displayMapId != mv.h->mapId) continue;
                    int cells = 0, inside = 0;
                    for (int row = 0; row < 128; ++row)
                        for (int col = 0; col < 128; ++col) {
                            if (wv.zmp[row * 128 + col] != z.areaId) continue;
                            ++cells;
                            const float sy = (0.5f - (static_cast<float>(col) + 0.5f) / 128.0f) * kWorld;  // server Y
                            const float sx = (0.5f - (static_cast<float>(row) + 0.5f) / 128.0f) * kWorld;  // server X
                            if (sy >= std::min(z.locLeft, z.locRight) && sy <= std::max(z.locLeft, z.locRight) &&
                                sx >= std::min(z.locTop, z.locBottom) && sx <= std::max(z.locTop, z.locBottom))
                                ++inside;
                        }
                    ++virt;
                    if (cells && inside * 2 < cells) {
                        ++offBox;
                        bad("virtual zone " + std::string(wv.str(z.folderOffset)) + " off its ZMP cells (" +
                            std::to_string(inside) + "/" + std::to_string(cells) + ")");
                    }
                    if (!opt.quiet)
                        std::printf("verify virtual zone %s (map %u on %u): offset X %.1f Y %.1f, ZMP cells inside %d/%d\n",
                                    wv.str(z.folderOffset), z.mapId, z.displayMapId, z.virtualOffsetX, z.virtualOffsetY,
                                    inside, cells);
                }
                if (!opt.quiet && virt) std::printf("verify virtual zones: %d, off their ZMP cells %d\n", virt, offBox);
            }
        }
    }
    // The area grid - the PSP's reader, every chunk of every tile, ids known to mmhdr.
    if (const uint8_t* ag = pak.find("agrid", size)) {
        fmt::AreaGridView av;
        if (!av.attach(ag, size)) {
            bad("agrid refused by AreaGridView");
        } else {
            if (av.h->mapId != mv.h->mapId) bad("agrid map != mmhdr map");
            uint32_t unknown = 0, multi = 0;
            std::set<uint32_t> ids;
            for (uint32_t i = 0; i < av.h->tileCount; ++i) {
                const int x = av.tiles[i].key >> 8, y = av.tiles[i].key & 0xFF;
                if (av.tiles[i].paletteCount > 1) ++multi;
                for (int c = 0; c < 256; ++c) {
                    const uint32_t a = av.areaAt(x, y, c);
                    if (a == 0) continue;
                    ids.insert(a);
                }
            }
            for (uint32_t a : ids)
                if (!mv.area(a)) ++unknown;
            if (!opt.quiet)
                std::printf("verify agrid: tiles=%u multi-area=%u areas=%zu not-in-mmhdr=%u kb=%u\n", av.h->tileCount, multi,
                            ids.size(), unknown, size / 1024u);
        }
    } else if (!opt.quiet) {
        std::printf("verify agrid: none (the area under the feet falls back to the server's zone)\n");
    }
    errors += verifyDungeonRecords(pak, opt.dumpDir, opt.quiet);  // "wmmi", "dmap"
    if (!opt.quiet)
        std::printf("verify minimap: map=%s(%u) tiles=%d (%d..%d x %d..%d) tile=%u kb=%zu areas=%u named=%d errors=%d\n",
                    mv.h->mapName, mv.h->mapId, tiles, minX, maxX, minY, maxY, mv.h->tileSize, tileBytes / 1024,
                    mv.h->areaCount, named, errors);
    return errors;
}

// ---------------------------------------------------------------------------
// Reading the client data
// ---------------------------------------------------------------------------

bool loadDbc(const std::string& root, const char* name, pipeline::DBCFile& dbc) {
    std::vector<uint8_t> bytes;
    return readFile(root + "/dbfilesclient/" + name, bytes) && dbc.load(bytes);
}

// upstream Minimap::parseTRS, keys lowercased ("<map>\map32_48" -> "<hash>").
std::map<std::string, std::string> parseTrs(const std::string& root) {
    std::map<std::string, std::string> out;
    std::vector<uint8_t> data;
    if (!readFile(root + "/textures/minimap/md5translate.trs", data)) return out;
    std::string content(data.begin(), data.end());
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) nl = content.size();
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("dir:", 0) == 0) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string key = line.substr(0, tab), hash = line.substr(tab + 1);
        if (key.size() > 4 && lower(key.substr(key.size() - 4)) == ".blp") key.resize(key.size() - 4);
        if (hash.size() > 4 && lower(hash.substr(hash.size() - 4)) == ".blp") hash.resize(hash.size() - 4);
        for (char& c : key)
            if (c == '/') c = '\\';
        std::string k = key;
        for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out[k] = hash;
    }
    return out;
}

std::string minimapTilePath(const std::string& root, const std::map<std::string, std::string>& trs,
                            const std::string& mapName, int x, int y, bool& known) {
    const std::string key = lower(mapName) + "\\map" + std::to_string(x) + "_" + std::to_string(y);
    std::string k = key;
    for (char& c : k)
        if (c == '/') c = '\\';
    auto it = trs.find(k);
    known = it != trs.end();
    if (known) return root + "/textures/minimap/" + lower(it->second) + ".blp";
    // Some extracts carry the plain names too.
    return root + "/textures/minimap/" + lower(mapName) + "/map" + std::to_string(x) + "_" + std::to_string(y) + ".blp";
}

int pickLocaleColumn(const pipeline::DBCFile& dbc, uint32_t first) {
    int best = 0, bestN = -1;
    for (int k = 0; k < 16 && first + k < dbc.getFieldCount(); ++k) {
        int n = 0;
        for (uint32_t i = 0; i < dbc.getRecordCount() && i < 4000; ++i)
            if (!dbc.getStringView(i, first + k).empty()) ++n;
        if (n > bestN) {
            best = k;
            bestN = n;
        }
    }
    return best;
}

uint32_t mapIdByName(const std::string& root, const std::string& mapName) {
    pipeline::DBCFile map;
    if (loadDbc(root, "map.dbc", map)) {
        for (uint32_t i = 0; i < map.getRecordCount(); ++i)
            if (lower(map.getString(i, 1)) == lower(mapName)) return map.getUInt32(i, 0);
    }
    const std::string m = lower(mapName);  // upstream folderToMapId
    if (m == "azeroth") return 0;
    if (m == "kalimdor") return 1;
    if (m == "expansion01") return 530;
    if (m == "northrend") return 571;
    return ~0u;
}

struct Box {
    int x0, y0, x1, y1;
};

// upstream DataRepository::loadZones, rows of the map (physical or display).
bool readWorldMap(const std::string& root, uint32_t mapId, std::vector<ZoneRow>& zones, std::set<uint32_t>& areaRefs,
                  const std::unordered_map<uint32_t, uint32_t>& exploreFlagByArea,
                  const std::unordered_map<uint32_t, std::vector<uint32_t>>& childBits,
                  const std::unordered_map<uint32_t, std::string>& areaNames) {
    pipeline::DBCFile wma, wmo, wmt;
    if (!loadDbc(root, "worldmaparea.dbc", wma)) {
        std::printf("worldmap: dbfilesclient/worldmaparea.dbc missing - no world map\n");
        return false;
    }
    struct Tr {
        uint32_t src, dst;
        float minX, minY, maxX, maxY, offX, offY;
    };
    std::vector<Tr> trs;
    if (loadDbc(root, "worldmaptransforms.dbc", wmt) && wmt.getFieldCount() >= 9)
        for (uint32_t i = 0; i < wmt.getRecordCount(); ++i)
            trs.push_back({wmt.getUInt32(i, 1), wmt.getUInt32(i, 6), wmt.getFloat(i, 2), wmt.getFloat(i, 3),
                           wmt.getFloat(i, 4), wmt.getFloat(i, 5), wmt.getFloat(i, 7), wmt.getFloat(i, 8)});
    for (uint32_t i = 0; i < wma.getRecordCount(); ++i) {
        const uint32_t recMap = wma.getUInt32(i, 1), displayMap = wma.getUInt32(i, 8);
        if (recMap != mapId && displayMap != mapId) continue;
        ZoneRow z;
        z.rec.wmaId = wma.getUInt32(i, 0);
        z.rec.mapId = recMap;
        z.rec.areaId = wma.getUInt32(i, 2);
        z.folder = wma.getString(i, 3);
        z.rec.locLeft = wma.getFloat(i, 4);
        z.rec.locRight = wma.getFloat(i, 5);
        z.rec.locTop = wma.getFloat(i, 6);
        z.rec.locBottom = wma.getFloat(i, 7);
        z.rec.displayMapId = displayMap;
        z.rec.parentWorldMapId = wma.getUInt32(i, 10);
        if (recMap != mapId && displayMap == mapId) {
            // PSP: upstream tests and shifts the swapped axes. WorldMapTransforms' region
            // X and offset X are the server X (north: WorldMapArea locTop / locBottom), Y the server Y (west:
            // locLeft / locRight) - the starting area of race 11 (530: X -3962, Y -11654) lies in the 530 -> 1 region
            // X -6933..533, Y -16000..-8000, and the client's own ZMP of map 1 draws its zone at the 530 bounds
            // + (10133, 17600) in (X, Y), the ZMP of map 0 another 530 zone at + (-2400, 2400). Upstream's test (the centre of
            // locLeft / locRight against region X) never matched: those zones kept their
            // map 530 coordinates on maps 1 / 0. virtualOffsetX / Y = server X / Y.
            const float cy = (z.rec.locLeft + z.rec.locRight) * 0.5f, cx = (z.rec.locTop + z.rec.locBottom) * 0.5f;
            for (const Tr& t : trs) {
                if (t.src != recMap || t.dst != displayMap || cx < std::min(t.minX, t.maxX) || cx > std::max(t.minX, t.maxX) ||
                    cy < std::min(t.minY, t.maxY) || cy > std::max(t.minY, t.maxY))
                    continue;
                z.rec.virtualOffsetX = t.offX;
                z.rec.virtualOffsetY = t.offY;
                z.rec.locTop += t.offX;
                z.rec.locBottom += t.offX;
                z.rec.locLeft += t.offY;
                z.rec.locRight += t.offY;
                break;
            }
        }
        if (auto e = exploreFlagByArea.find(z.rec.areaId); e != exploreFlagByArea.end()) z.exploreBits.push_back(e->second);
        if (auto c = childBits.find(z.rec.areaId); c != childBits.end())
            z.exploreBits.insert(z.exploreBits.end(), c->second.begin(), c->second.end());
        if (auto n = areaNames.find(z.rec.areaId); n != areaNames.end()) z.name = n->second;
        if (z.rec.areaId) areaRefs.insert(z.rec.areaId);
        zones.push_back(std::move(z));
    }
    // Continent bounds from children when missing (upstream: same loop).
    for (ZoneRow& c : zones) {
        if (c.rec.areaId != 0) continue;
        if (std::abs(c.rec.locLeft) > 0.001f || std::abs(c.rec.locRight) > 0.001f || std::abs(c.rec.locTop) > 0.001f ||
            std::abs(c.rec.locBottom) > 0.001f)
            continue;
        bool first = true;
        for (const ZoneRow& z : zones) {
            if (z.rec.areaId == 0) continue;
            if (std::abs(z.rec.locLeft - z.rec.locRight) < 0.001f || std::abs(z.rec.locTop - z.rec.locBottom) < 0.001f) continue;
            if (z.rec.parentWorldMapId != 0 && c.rec.wmaId != 0 && z.rec.parentWorldMapId != c.rec.wmaId) continue;
            if (first) {
                c.rec.locLeft = z.rec.locLeft;
                c.rec.locRight = z.rec.locRight;
                c.rec.locTop = z.rec.locTop;
                c.rec.locBottom = z.rec.locBottom;
                first = false;
            } else {
                c.rec.locLeft = std::max(c.rec.locLeft, z.rec.locLeft);
                c.rec.locRight = std::min(c.rec.locRight, z.rec.locRight);
                c.rec.locTop = std::max(c.rec.locTop, z.rec.locTop);
                c.rec.locBottom = std::min(c.rec.locBottom, z.rec.locBottom);
            }
        }
    }
    std::map<uint32_t, size_t> byWma;
    for (size_t i = 0; i < zones.size(); ++i) byWma[zones[i].rec.wmaId] = i;
    if (loadDbc(root, "worldmapoverlay.dbc", wmo)) {
        for (uint32_t i = 0; i < wmo.getRecordCount(); ++i) {
            auto it = byWma.find(wmo.getUInt32(i, 1));
            if (it == byWma.end()) continue;
            OverlayRow o;
            for (int a = 0; a < 4; ++a) {
                o.rec.areaIds[a] = wmo.getUInt32(i, 2 + a);
                if (o.rec.areaIds[a]) areaRefs.insert(o.rec.areaIds[a]);
            }
            o.texture = wmo.getString(i, 8);
            o.rec.texWidth = static_cast<uint16_t>(wmo.getUInt32(i, 9));
            o.rec.texHeight = static_cast<uint16_t>(wmo.getUInt32(i, 10));
            o.rec.offsetX = static_cast<uint16_t>(wmo.getUInt32(i, 11));
            o.rec.offsetY = static_cast<uint16_t>(wmo.getUInt32(i, 12));
            o.rec.hitLeft = static_cast<uint16_t>(wmo.getUInt32(i, 13));
            o.rec.hitRight = static_cast<uint16_t>(wmo.getUInt32(i, 14));
            o.rec.hitTop = static_cast<uint16_t>(wmo.getUInt32(i, 15));
            o.rec.hitBottom = static_cast<uint16_t>(wmo.getUInt32(i, 16));
            if (o.texture.empty() || o.rec.texWidth == 0 || o.rec.texHeight == 0) continue;
            zones[it->second].overlays.push_back(std::move(o));
        }
    }
    return true;
}

std::string wmPath(const std::string& root, const std::string& folder, const std::string& file) {
    return root + "/" + lower("interface/worldmap/" + folder + "/" + file + ".blp");
}

bool loadZoneArt(const std::string& root, ZoneRow& z, ZoneArt& art, std::vector<std::string>* missing) {
    int found = 0;
    for (int n = 0; n < 12; ++n) {
        const std::string p = wmPath(root, z.folder, z.folder + std::to_string(n + 1));
        if (loadBlp(p, art.base[n])) {
            art.haveBase[n] = true;
            z.rec.tileMask |= static_cast<uint16_t>(1u << n);
            ++found;
        } else if (missing) {
            missing->push_back(p);
        }
    }
    art.overlays.assign(z.overlays.size(), Image{});
    for (size_t i = 0; i < z.overlays.size(); ++i) {
        OverlayRow& o = z.overlays[i];
        const int cols = (o.rec.texWidth + 255) / 256, rows = (o.rec.texHeight + 255) / 256;
        Image comp;
        comp.w = o.rec.texWidth;
        comp.h = o.rec.texHeight;
        comp.px.assign(static_cast<size_t>(comp.w) * comp.h, Rgb{0, 0, 0, 0});
        int pieces = 0;
        for (int t = 0; t < cols * rows; ++t) {
            Image piece;
            const std::string p = wmPath(root, z.folder, o.texture + std::to_string(t + 1));
            if (!loadBlp(p, piece)) {
                if (missing) missing->push_back(p);
                continue;
            }
            const int col = t % cols, row = t / cols;
            // upstream: the last piece is only as wide as the overlay has left; the file is
            // padded to a power of two - only its top-left part is the piece.
            const int pw = std::min(256, o.rec.texWidth - col * 256), ph = std::min(256, o.rec.texHeight - row * 256);
            for (int y = 0; y < ph && y < piece.h; ++y)
                for (int x = 0; x < pw && x < piece.w; ++x) comp.at(col * 256 + x, row * 256 + y) = piece.at(x, y);
            ++pieces;
        }
        if (pieces) {
            art.overlays[i] = std::move(comp);
            o.rec.baked = 1;
        }
    }
    return found > 0;
}

// The tiles the map's WDT lists (MAIN flag 1 = the tile has an ADT), as the
// client loads them (and as tools/psp/bake_world.sh bakes them).
std::vector<std::pair<int, int>> wdtTiles(const std::string& root, const std::string& mapName) {
    std::vector<std::pair<int, int>> out;
    std::vector<uint8_t> d;
    if (!readFile(root + "/world/maps/" + lower(mapName) + "/" + lower(mapName) + ".wdt", d)) return out;
    for (size_t i = 0; i + 8 <= d.size();) {
        uint32_t size = 0;
        std::memcpy(&size, &d[i + 4], 4);
        if (std::memcmp(&d[i], "NIAM", 4) == 0) {  // "MAIN", stored reversed
            for (int t = 0; t < 4096 && i + 8 + static_cast<size_t>(t) * 8 + 4 <= d.size(); ++t) {
                uint32_t flags = 0;
                std::memcpy(&flags, &d[i + 8 + static_cast<size_t>(t) * 8], 4);
                if (flags & 1u) out.push_back({t % 64, t / 64});
            }
            break;
        }
        i += 8 + static_cast<size_t>(size);
    }
    return out;
}

// upstream ADTLoader::load / parseMCNK: the top-level MCNK chunks in file order (chunkIndex++ for each),
// areaId at offset 52 of the MCNK header - the grid upstream Renderer::getCurrentZoneId reads (and
// psp_audio_conv baked into "a_<x>_<y>").
bool adtAreaIds(const std::string& path, std::array<uint16_t, 256>& out) {
    std::vector<uint8_t> d;
    out.fill(0);
    if (!readFile(path, d) || d.empty()) return false;
    int n = 0;
    for (size_t i = 0; i + 8 <= d.size() && n < 256;) {
        uint32_t size = 0;
        std::memcpy(&size, &d[i + 4], 4);
        if (i + 8 + static_cast<size_t>(size) > d.size()) break;  // upstream: a chunk past the end stops the walk
        if (std::memcmp(&d[i], "KNCM", 4) == 0) {
            if (size >= 56) {
                uint32_t area = 0;
                std::memcpy(&area, &d[i + 8 + 52], 4);
                out[n] = static_cast<uint16_t>(area > 0xFFFFu ? 0u : area);
            }
            ++n;
        }
        i += 8 + static_cast<size_t>(size);
    }
    return n > 0;
}

// data/worldmap.pak - the "World" and "Cosmic" levels (psp_worldmap_view.hpp).
int bakeWorldLevels(const std::string& root, const std::string& out, int wmTile, bool doVerify, const std::string& dump,
                    bool listAssets);
int verifyWorldPak(const std::string& path, const std::string& dumpDir);

int bakeWorldLevels(const std::string& root, const std::string& out, int wmTile, bool doVerify, const std::string& dump,
                    bool listAssets) {
    if (listAssets) {
        std::printf("# psp_mpq_extract --prefix (read-only) for psp_minimap_conv --world\n"
                    "DBFilesClient\\WorldMapContinent.dbc\nDBFilesClient\\WorldMapArea.dbc\n"
                    "Interface\\WorldMap\\World\\\nInterface\\WorldMap\\Cosmic\\\n"
                    "Interface\\WorldMap\\Azeroth\\AzerothHighlight.blp\nInterface\\WorldMap\\Kalimdor\\KalimdorHighlight.blp\n"
                    "Interface\\WorldMap\\Northrend\\NorthrendHighlight.blp\n");
        return 0;
    }
    pipeline::DBCFile wmc, wma;
    if (!loadDbc(root, "worldmapcontinent.dbc", wmc) || wmc.getFieldCount() < 14) {
        std::fprintf(stderr, "world: dbfilesclient/worldmapcontinent.dbc missing or not 3.3.5a (14 fields)\n");
        return 1;
    }
    std::map<uint32_t, std::string> contFolder;  // map -> its continent row's folder (WorldMapArea, areaId 0)
    if (loadDbc(root, "worldmaparea.dbc", wma))
        for (uint32_t i = 0; i < wma.getRecordCount(); ++i)
            if (wma.getUInt32(i, 2) == 0 && !contFolder.count(wma.getUInt32(i, 1)))
                contFolder[wma.getUInt32(i, 1)] = wma.getString(i, 3);
    std::vector<fmt::WorldLevelContinent> conts;
    std::vector<PakItem> worldItems, cosmicItems, hlItems;
    for (uint32_t i = 0; i < wmc.getRecordCount() && conts.size() < 16; ++i) {
        fmt::WorldLevelContinent c{};
        c.id = wmc.getUInt32(i, 0);
        c.mapId = wmc.getUInt32(i, 1);
        c.left = static_cast<int32_t>(wmc.getUInt32(i, 2));
        c.right = static_cast<int32_t>(wmc.getUInt32(i, 3));
        c.top = static_cast<int32_t>(wmc.getUInt32(i, 4));
        c.bottom = static_cast<int32_t>(wmc.getUInt32(i, 5));
        c.offsetX = wmc.getFloat(i, 6);
        c.offsetY = wmc.getFloat(i, 7);
        c.scale = wmc.getFloat(i, 8);
        c.worldMapId = wmc.getUInt32(i, 13);
        const std::string folder = contFolder.count(c.mapId) ? contFolder[c.mapId] : std::string();
        std::snprintf(c.folder, sizeof(c.folder), "%s", folder.c_str());
        Image hl;
        if (!folder.empty() && loadBlp(wmPath(root, folder, folder + "Highlight"), hl)) {
            char n[48];
            fmt::worldLevelHighlightName(n, sizeof(n), false, c.mapId);
            const int side = std::max(16, std::min(512, hl.w * wmTile / 512));  // 512 -> 128 with --wmtile 128
            hlItems.push_back({n, kKindTexture, encodeWtex(maxPool(hl, side, side), false)});
            c.flags |= 1u;
        }
        std::printf("world: continent %u map %u (%s) tiles %d..%d x %d..%d offset %.3f, %.3f scale %.2f world %u%s\n", c.id,
                    c.mapId, folder.c_str(), c.left, c.right, c.top, c.bottom, c.offsetX, c.offsetY, c.scale, c.worldMapId,
                    (c.flags & 1u) ? ", highlight" : "");
        conts.push_back(c);
    }
    int found[2] = {0, 0};
    for (int lvl = 0; lvl < 2; ++lvl) {
        const std::string folder = lvl ? "Cosmic" : "World";
        for (int t = 0; t < 12; ++t) {
            Image img;
            if (!loadBlp(wmPath(root, folder, folder + std::to_string(t + 1)), img)) continue;
            char n[48];
            fmt::worldLevelTileName(n, sizeof(n), lvl == 1, t);
            (lvl ? cosmicItems : worldItems).push_back({n, kKindTexture, encodeWtex(scaledForPsp(img, wmTile), false)});
            ++found[lvl];
        }
    }
    // The cosmic buttons' highlights (the map 0 / map 530 buttons' HighlightTexture, 512 x 512 with alpha).
    for (const auto& [mapId, file] : {std::pair<uint32_t, const char*>{0, "Cosmic-Azeroth-Highlight"},
                                      std::pair<uint32_t, const char*>{530, "Cosmic-Outland-Highlight"}}) {
        Image hl;
        if (!loadBlp(wmPath(root, "Cosmic", file), hl)) continue;
        char n[48];
        fmt::worldLevelHighlightName(n, sizeof(n), true, mapId);
        const int side = std::max(16, std::min(512, hl.w * wmTile / 512));
        cosmicItems.push_back({n, kKindTexture, encodeWtex(resample(hl, side, side), false)});
    }
    fmt::WorldLevelHeader h{};
    std::memcpy(h.magic, "WWLD", 4);
    h.version = fmt::kWorldLevelVersion;
    h.continentCount = static_cast<uint32_t>(conts.size());
    h.baseTile = static_cast<uint16_t>(wmTile);
    h.flags = static_cast<uint16_t>((found[0] == 12 ? 1u : 0u) | (found[1] == 12 ? 2u : 0u));
    std::vector<uint8_t> hdr;
    putStruct(hdr, h);
    for (const auto& c : conts) putStruct(hdr, c);
    std::vector<PakItem> items;
    items.push_back({"wlhdr", kKindRaw, std::move(hdr)});
    for (auto* v : {&worldItems, &hlItems, &cosmicItems})
        for (auto& e : *v) items.push_back(std::move(e));
    std::printf("world: %zu continents, world tiles %d/12, cosmic tiles %d/12, %zu highlights, entries %zu\n", conts.size(),
                found[0], found[1], hlItems.size(), items.size());
    const std::filesystem::path parent = std::filesystem::path(out).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    if (!writePak(out, items)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    if (!doVerify && dump.empty()) return 0;
    const int errors = verifyWorldPak(out, dump);
    return errors && doVerify ? 3 : 0;
}

// The PSP's reader over worldmap.pak; --dump draws each level with all its highlights added (ADD) where the PSP
// draws them (world_level.png: the continent outlines should sit on the coasts of the World art).
int verifyWorldPak(const std::string& path, const std::string& dumpDir) {
    PakView pak;
    if (!pak.open(path)) {
        std::printf("verify: cannot read %s\n", path.c_str());
        return 1;
    }
    int errors = 0;
    auto bad = [&](const std::string& what) {
        if (++errors <= 40) std::printf("verify ERROR: %s\n", what.c_str());
    };
    uint32_t size = 0;
    const uint8_t* hb = pak.find("wlhdr", size);
    fmt::WorldLevelView wl;
    if (!hb || !wl.attach(hb, size)) {
        bad("wlhdr missing or refused by WorldLevelView");
        return errors;
    }
    const bool dump = !dumpDir.empty();
    if (dump) std::filesystem::create_directories(dumpDir);
    for (int lvl = 0; lvl < 2; ++lvl) {
        const int bt = wl.h->baseTile;
        Image canvas;
        canvas.w = bt * 4;
        canvas.h = bt * 3;
        canvas.px.assign(static_cast<size_t>(canvas.w) * canvas.h, Rgb{0, 0, 0, 1});
        uint32_t lo = ~0u, hi = 0, sum = 0, kb = 0;
        auto span = [&](const uint8_t* b, uint32_t es) {
            const uint32_t off = static_cast<uint32_t>(b - pak.bytes.data());
            lo = std::min(lo, off);
            hi = std::max(hi, off + es);
            sum += (es + 63u) & ~63u;
            kb += es;
        };
        int tiles = 0;
        for (int t = 0; t < 12; ++t) {
            char n[48];
            fmt::worldLevelTileName(n, sizeof(n), lvl == 1, t);
            uint32_t es = 0;
            const uint8_t* b = pak.find(n, es);
            Image img;
            if (!b || !decodeContent(b, es, img)) {
                bad(std::string("tile missing/bad: ") + n);
                continue;
            }
            span(b, es);
            ++tiles;
            const Image s = resample(img, bt, bt);
            for (int y = 0; y < bt; ++y)
                for (int x = 0; x < bt; ++x) canvas.at((t % 4) * bt + x, (t / 4) * bt + y) = s.at(x, y);
        }
        // The highlights, added where the PSP puts them (map units: 1002 x 668 of the 1024 x 768 art).
        const float u2px = bt / 256.0f;
        auto addOver = [&](const Image& hl, float x, float y, float w, float hgt, bool alpha) {
            const int dw = std::max(1, static_cast<int>(w * u2px)), dh = std::max(1, static_cast<int>(hgt * u2px));
            const Image r = resample(hl, dw, dh);
            const int ox = static_cast<int>(std::floor(x * u2px)), oy = static_cast<int>(std::floor(y * u2px));
            for (int yy = 0; yy < dh; ++yy)
                for (int xx = 0; xx < dw; ++xx) {
                    const int cx = ox + xx, cy = oy + yy;
                    if (cx < 0 || cy < 0 || cx >= canvas.w || cy >= canvas.h) continue;
                    const Rgb& q = r.at(xx, yy);
                    const float k = alpha ? q.a : 1.0f;
                    Rgb& d = canvas.at(cx, cy);
                    d = Rgb{std::min(1.0f, d.r + q.r * k), std::min(1.0f, d.g + q.g * k), std::min(1.0f, d.b + q.b * k), 1.0f};
                }
        };
        int hls = 0;
        if (lvl == 0) {
            for (uint32_t i = 0; i < wl.h->continentCount; ++i) {
                const fmt::WorldLevelContinent& c = wl.conts[i];
                if (!(c.flags & 1u)) continue;
                char n[48];
                fmt::worldLevelHighlightName(n, sizeof(n), false, c.mapId);
                uint32_t es = 0;
                const uint8_t* b = pak.find(n, es);
                Image img;
                if (!b || !decodeContent(b, es, img)) {
                    bad(std::string("highlight missing/bad: ") + n);
                    continue;
                }
                span(b, es);
                ++hls;
                float x, y, w, hh;
                fmt::worldLevelBox(c, x, y, w, hh);
                const float side = fmt::worldLevelHighlightSide(c);
                if (c.worldMapId == 1) addOver(img, x, y, side, side, false);
                if (!(x > -50.0f && y > -50.0f && x + w < 1050.0f && y + hh < 720.0f && c.worldMapId == 1) && c.worldMapId == 1)
                    bad("continent " + std::to_string(c.mapId) + " box off the world map");
            }
        } else {
            int nb = 0;
            const fmt::CosmicButton* cb = fmt::cosmicButtons(nb);
            for (int k = 0; k < nb; ++k) {
                char n[48];
                fmt::worldLevelHighlightName(n, sizeof(n), true, cb[k].mapId);
                uint32_t es = 0;
                const uint8_t* b = pak.find(n, es);
                Image img;
                if (!b || !decodeContent(b, es, img)) {
                    bad(std::string("cosmic highlight missing/bad: ") + n);
                    continue;
                }
                span(b, es);
                ++hls;
                addOver(img, cb[k].hlX, cb[k].hlY, cb[k].hlW, cb[k].hlH, true);
            }
        }
        if (tiles && ((hi + 63u) & ~63u) - lo != sum) bad(std::string(lvl ? "cosmic" : "world") + " entries not consecutive");
        std::printf("verify %s level: tiles=%d highlights=%d kb=%u (one read)\n", lvl ? "cosmic" : "world", tiles, hls, kb / 1024u);
        if (dump) writePng(dumpDir + (lvl ? "/cosmic_level.png" : "/world_level.png"), canvas.w, canvas.h, toRgba8(canvas));
    }
    std::printf("verify worldmap.pak: continents=%u flags=%u errors=%d\n", wl.h->continentCount, wl.h->flags, errors);
    return errors;
}

// ---------------------------------------------------------------------------
// Selftest (no client data)
// ---------------------------------------------------------------------------

float polyArea(const fmt::MinimapVert* p, int n) {
    float a = 0;
    for (int i = 0; i < n; ++i) {
        const auto& u = p[i];
        const auto& v = p[(i + 1) % n];
        a += u.sx * v.sy - v.sx * u.sy;
    }
    return std::abs(a) * 0.5f;
}

int selftest(const std::string& dumpDir) {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("selftest FAIL: %s\n", what);
            ++fails;
        }
    };
    auto near = [](float a, float b, float eps) { return std::abs(a - b) <= eps; };
    // 1. World -> tile / pixel: a starting area (server -8949.95, -132.49) is ADT tile 32_48 of map 0.
    float gx = 0, gy = 0;
    fmt::minimapTileCoords(-8949.95f, -132.49f, gx, gy);
    check(static_cast<int>(std::floor(gx)) == 32 && static_cast<int>(std::floor(gy)) == 48, "start point tile 32_48");
    check(near(gx - 32.0f, 0.2484f, 1e-3f) && near(gy - 48.0f, 0.7812f, 1e-3f), "start point tile fraction");
    const int px = static_cast<int>((gx - 32.0f) * 128.0f), py = static_cast<int>((gy - 48.0f) * 128.0f);
    check(px == 31 && py == 99, "start point pixel in a 128 tile (31, 99)");
    float sx = 0, sy = 0;
    fmt::minimapServerPos(gx, gy, sx, sy);
    check(near(sx, -8949.95f, 0.02f) && near(sy, -132.49f, 0.02f), "tile coords round trip");
    // 2. Projection (upstream minimap_projection.hpp): north up - north is up, west is left.
    fmt::MinimapProj v;
    v.viewRadius = 200.0f;
    v.mapRadius = 38.0f;
    float ox = 0, oy = 0;
    fmt::minimapDeltaToOffset(100.0f, 0.0f, v, ox, oy);  // +100 yd server X (north)
    check(near(ox, 0.0f, 1e-4f) && near(oy, -19.0f, 1e-3f), "north projects up");
    fmt::minimapDeltaToOffset(0.0f, 100.0f, v, ox, oy);  // +100 yd server Y (west)
    check(near(ox, -19.0f, 1e-3f) && near(oy, 0.0f, 1e-4f), "west projects left");
    // Rotating: bearing = camera yaw -> the camera's forward is straight up, at any yaw.
    for (int k = 0; k < 16; ++k) {
        const float yaw = -3.1f + k * 0.41f;
        v.cosBearing = std::cos(yaw);
        v.sinBearing = std::sin(yaw);
        fmt::minimapDeltaToOffset(std::cos(yaw) * 50.0f, std::sin(yaw) * 50.0f, v, ox, oy);
        check(near(ox, 0.0f, 1e-3f) && oy < -9.0f, "rotating: camera forward points up");
        // left of the camera (yaw + 90 deg, CCW) is screen left
        fmt::minimapDeltaToOffset(std::cos(yaw + 1.5708f) * 50.0f, std::sin(yaw + 1.5708f) * 50.0f, v, ox, oy);
        check(ox < -9.0f && near(oy, 0.0f, 1e-3f), "rotating: camera left is screen left");
        float dx = 0, dy = 0;
        fmt::minimapOffsetToDelta(7.0f, -12.0f, v, dx, dy);
        fmt::minimapDeltaToOffset(dx, dy, v, ox, oy);
        check(near(ox, 7.0f, 1e-3f) && near(oy, -12.0f, 1e-3f), "offset <-> delta inverse");
    }
    // 3. The disc clipped per tile covers the disc exactly, and every clipped vertex keeps
    //    the affine screen <-> tile mapping (what the GE interpolates).
    for (int rot = 0; rot < 2; ++rot) {
        for (float radius : {100.0f, 200.0f, 400.0f, 800.0f}) {
            fmt::MinimapProj p;
            p.viewRadius = radius;
            p.mapRadius = 38.0f;
            const float yaw = rot ? 0.7f : 0.0f;
            p.cosBearing = std::cos(yaw);
            p.sinBearing = std::sin(yaw);
            const float playerX = -8949.95f, playerY = -132.49f, cx = 428.0f, cy = 70.0f;
            fmt::MinimapVert disc[64], out[72], tmp[72];
            const int n = fmt::minimapDisc(cx, cy, playerX, playerY, p, 40, disc);
            int x0, y0, x1, y1;
            fmt::minimapTileRange(disc, n, x0, y0, x1, y1);
            float sum = 0;
            int pieces = 0;
            bool affine = true;
            for (int ty = y0; ty <= y1; ++ty)
                for (int tx = x0; tx <= x1; ++tx) {
                    const int m = fmt::minimapClipToTile(disc, n, tx, ty, out, tmp);
                    if (!m) continue;
                    ++pieces;
                    sum += polyArea(out, m);
                    for (int i = 0; i < m; ++i) {
                        if (out[i].gx < tx - 1e-3f || out[i].gx > tx + 1 + 1e-3f || out[i].gy < ty - 1e-3f ||
                            out[i].gy > ty + 1 + 1e-3f)
                            affine = false;
                        float ddx = 0, ddy = 0, egx = 0, egy = 0;
                        fmt::minimapOffsetToDelta(out[i].sx - cx, out[i].sy - cy, p, ddx, ddy);
                        fmt::minimapTileCoords(playerX + ddx, playerY + ddy, egx, egy);
                        if (!near(egx, out[i].gx, 2e-3f) || !near(egy, out[i].gy, 2e-3f)) affine = false;
                    }
                }
            const float whole = polyArea(disc, n);
            check(near(sum, whole, whole * 1e-3f), "clipped pieces sum to the disc");
            check(affine, "clipped vertices keep the screen <-> tile mapping");
            check(pieces >= 1 && pieces <= 16, "tile pieces in range");
            // The player's own point is at the centre of the disc.
            float pgx, pgy;
            fmt::minimapTileCoords(playerX, playerY, pgx, pgy);
            float cdx = 0, cdy = 0, cgx, cgy;
            fmt::minimapOffsetToDelta(0.0f, 0.0f, p, cdx, cdy);
            fmt::minimapTileCoords(playerX + cdx, playerY + cdy, cgx, cgy);
            check(near(cgx, pgx, 1e-4f) && near(cgy, pgy, 1e-4f), "disc centre = player");
        }
    }
    // 4. Pak round trip: 2 x 2 synthetic tiles, areas with Cyrillic names, a continent + a
    //    zone with 12 base tiles and an alpha overlay; read back through the PSP readers.
    const std::string dir = dumpDir.empty() ? std::filesystem::temp_directory_path().string() + "/psp_minimap_selftest"
                                            : dumpDir;
    std::filesystem::create_directories(dir);
    const std::string pakPath = dir + "/minimap_selftest.pak";
    std::vector<PakItem> items;
    const Rgb colors[4] = {{0.9f, 0.1f, 0.1f, 1}, {0.1f, 0.8f, 0.2f, 1}, {0.1f, 0.2f, 0.9f, 1}, {0.9f, 0.9f, 0.2f, 1}};
    std::vector<AreaRow> areas = {{12, 0, 0, 1, "Тестовая зона"}, {9, 0, 12, 2, "Тестовая область"},
                                  {87, 0, 12, 3, "Третья зона"}};
    items.push_back({"mmhdr", kKindRaw, buildMinimapHeader(0, "Azeroth", 128, 4, areas)});
    int ti = 0;
    for (int y = 48; y <= 49; ++y)
        for (int x = 32; x <= 33; ++x, ++ti) {
            Image t;
            t.w = t.h = 256;
            t.px.resize(256 * 256);
            for (int yy = 0; yy < 256; ++yy)
                for (int xx = 0; xx < 256; ++xx) {
                    Rgb c = colors[ti];
                    const float k = 0.75f + 0.25f * (xx / 255.0f);  // gradient east
                    t.at(xx, yy) = Rgb{c.r * k, c.g * k, c.b * k, 1};
                }
            char n[48];
            fmt::minimapTileName(n, sizeof(n), x, y);
            items.push_back({n, kKindTexture, encodeWtex(resample(t, 128, 128), true)});
        }
    std::vector<ZoneRow> zones(2);
    zones[0].rec.wmaId = 14;  // continent
    zones[0].folder = "Azeroth";
    zones[0].rec.locLeft = 16000;
    zones[0].rec.locRight = -22000;
    zones[0].rec.locTop = 18000;
    zones[0].rec.locBottom = -22000;
    zones[1].rec.wmaId = 30;
    zones[1].rec.areaId = 12;
    zones[1].rec.parentWorldMapId = 14;
    zones[1].folder = "Elwynn";
    zones[1].name = "Тестовая зона";
    zones[1].rec.locLeft = 1535.4f;
    zones[1].rec.locRight = -1935.4f;
    zones[1].rec.locTop = -7939.6f;
    zones[1].rec.locBottom = -10254.2f;
    zones[1].exploreBits = {1, 2, 3};
    OverlayRow ov;
    ov.rec.areaIds[0] = 9;  // Northshire
    ov.rec.texWidth = 300;
    ov.rec.texHeight = 200;
    ov.rec.offsetX = 500;
    ov.rec.offsetY = 100;
    ov.texture = "Northshire";
    zones[1].overlays.push_back(ov);
    std::vector<ZoneArt> arts(2);
    for (int zi = 0; zi < 2; ++zi) {
        for (int n = 0; n < 12; ++n) {
            Image b;
            b.w = b.h = 256;
            b.px.assign(256 * 256, Rgb{0.2f + 0.05f * n, 0.3f, zi ? 0.2f : 0.6f, 1});
            arts[zi].base[n] = b;
            arts[zi].haveBase[n] = true;
            zones[zi].rec.tileMask |= static_cast<uint16_t>(1u << n);
        }
    }
    Image o;
    o.w = 300;
    o.h = 200;
    o.px.assign(300 * 200, Rgb{1, 1, 1, 0});
    for (int y = 40; y < 160; ++y)
        for (int x = 60; x < 240; ++x) o.at(x, y) = Rgb{0.95f, 0.85f, 0.4f, 1};
    arts[1].overlays.push_back(o);
    zones[1].overlays[0].rec.baked = 1;
    std::vector<uint16_t> zmp(128 * 128, 0);
    // Zone 1's cells of the ZMP: the world box of zone 1 (as the client's ZMP would hold).
    for (int r = 0; r < 128; ++r)
        for (int c = 0; c < 128; ++c) {
            const float serverY = (0.5f - (c + 0.5f) / 128.0f) * 64.0f * fmt::kMinimapTileYards;
            const float serverX = (0.5f - (r + 0.5f) / 128.0f) * 64.0f * fmt::kMinimapTileYards;
            if (serverY <= 1535.4f && serverY >= -1935.4f && serverX <= -7939.6f && serverX >= -10254.2f)
                zmp[r * 128 + c] = 12;
        }
    items.push_back({"wmhdr", kKindRaw, buildWorldMapHeader(0, 128, zones, zmp)});
    for (int zi = 0; zi < 2; ++zi) appendZoneArt(items, zones[zi], arts[zi], 128);
    if (!writePak(pakPath, items)) {
        std::printf("selftest FAIL: cannot write %s\n", pakPath.c_str());
        return 1;
    }
    VerifyOpts vo;
    vo.expectTiles = {{32, 48}, {33, 48}, {32, 49}, {33, 49}};
    vo.dumpDir = dumpDir;
    check(verifyPak(pakPath, vo) == 0, "verify of the synthetic pak");
    PakView pak;
    check(pak.open(pakPath), "pak reopen");
    uint32_t size = 0;
    const uint8_t* mh = pak.find("mmhdr", size);
    fmt::MinimapHeaderView mv;
    check(mh && mv.attach(mh, size), "mmhdr attach");
    if (mv.h) {
        const char* nm = mv.name(9);
        check(nm && std::string(nm) == "Тестовая область", "area 9 name (UTF-8)");
        check(mv.name(5) == nullptr, "unknown area -> none");
        const fmt::MinimapArea* a = mv.area(87);
        check(a && a->parent == 12 && a->exploreFlag == 3, "area 87 row");
    }
    // Tile colours survive T8 (tile 33_48 = colour 1, its east edge brighter).
    const uint8_t* tb = pak.find("t_33_48", size);
    Image t;
    check(tb && decodeContent(tb, size, t), "tile 33_48 decodes");
    if (t.w == 128) {
        const Rgb w0 = t.at(2, 64), e0 = t.at(125, 64);
        check(near(w0.g, 0.8f * 0.755f, 0.05f) && near(e0.g, 0.8f * 0.995f, 0.05f) && w0.r < 0.2f, "tile colour after T8");
    }
    // World map: upstream's logic over the rebuilt zones.
    const uint8_t* wh = pak.find("wmhdr", size);
    fmt::WorldMapView wv;
    check(wh && wv.attach(wh, size), "wmhdr attach");
    if (wv.h) {
        std::vector<wm::Zone> z;
        wv.buildZones(z);
        check(z.size() == 2 && z[1].overlays.size() == 1 && z[1].exploreBits.size() == 3, "zones rebuilt");
        const glm::vec3 northshire(-8949.95f, -132.49f, 0.0f);  // server = render space
        check(wm::findZoneForPlayer(z, northshire, 0) == 1, "findZoneForPlayer -> zone 1");
        check(wm::findBestContinentForPlayer(z, northshire) == 0, "findBestContinentForPlayer -> map 0");
        const glm::vec2 uv = wm::renderPosToMapUV(northshire, z[1].bounds, false);
        check(uv.x > 0.40f && uv.x < 0.50f && uv.y > 0.40f && uv.y < 0.45f, "start point UV on the zone 1 map");
        check(wv.zmpArea(-132.49f, -8949.95f) == 12, "ZMP cell under the start point -> area 12");
        std::unordered_map<uint32_t, uint32_t> flags;
        mv.exploreFlags(flags);
        wm::ExplorationState ex;
        std::vector<uint32_t> mask(128, 0);
        mask[0] = 1u << 1;  // AreaBit 1 (area 12's flag): the zone, not the Northshire overlay (flag 2)
        ex.setServerMask(mask, true);
        ex.update(z, northshire, 1, flags, 0);
        check(ex.exploredOverlays().empty(), "bit 1 does not reveal the start area (its flag is 2)");
        mask[0] = 1u << 2;
        ex.setServerMask(mask, true);
        ex.update(z, northshire, 1, flags, 0);
        check(ex.exploredOverlays().count(0) == 1, "bit 2 reveals the start area overlay");
        check(ex.exploredZones().count(1) == 1, "zone 1 explored");
    }
    // The area grid round trip (one area, a few, many) through the PSP's reader.
    {
        std::map<std::pair<int, int>, std::array<uint16_t, 256>> tiles;
        std::array<uint16_t, 256> one{}, few{}, many{}, none{};
        one.fill(14);                                                  // one zone only
        for (int c = 0; c < 256; ++c) few[c] = c < 100 ? 1637 : 14;    // a city / its zone
        for (int c = 0; c < 256; ++c) many[c] = static_cast<uint16_t>(100 + c % 40);
        tiles[{40, 30}] = one;
        tiles[{41, 29}] = few;
        tiles[{5, 63}] = many;
        tiles[{6, 6}] = none;  // all zero: left out
        size_t multi = 0;
        const std::vector<uint8_t> ag = buildAreaGrid(1, tiles, &multi);
        fmt::AreaGridView av;
        check(av.attach(ag.data(), static_cast<uint32_t>(ag.size())), "agrid attach");
        check(av.h && av.h->tileCount == 3 && multi == 2, "agrid: 3 tiles, 2 with several areas");
        bool same = av.h != nullptr;
        for (int c = 0; same && c < 256; ++c)
            same = av.areaAt(40, 30, c) == 14 && av.areaAt(41, 29, c) == few[c] && av.areaAt(5, 63, c) == many[c] &&
                   av.areaAt(6, 6, c) == 0 && av.areaAt(40, 31, c) == 0;
        check(same, "agrid: every chunk of every tile reads back");
        check(av.areaAt(40, 30, 256) == 0 && av.areaAt(-1, 30, 0) == 0, "agrid: out of range -> 0");
        std::vector<uint8_t> broken = ag;
        broken[16 + 2] = 0;  // first tile's paletteCount 0
        broken[16 + 3] = 0;
        fmt::AreaGridView bv;
        check(!bv.attach(broken.data(), static_cast<uint32_t>(broken.size())), "agrid: a bad tile is refused");
    }
    // The world level math with the 3.3.5a WorldMapContinent rows. The continent maps' top-left
    // corners (WorldMapArea, tile = 32 - yards / 533.33) must land where Astrolabe measured the client's world map
    // (x / y offset in world-map yards, 47714.28 yards = 1002 units): map 0 18542.31 / 3585.57,
    // map 571 16020.94 / 454.25 - a check of the formula against the client, not against itself.
    {
        const float yardsPerUnit = 47714.28f / 1002.0f;
        fmt::WorldLevelContinent ek{}, nr{}, ka{};
        ek.mapId = 0, ek.left = 26, ek.right = 44, ek.top = 8, ek.bottom = 60, ek.offsetX = 16.875f, ek.offsetY = -1.5f,
        ek.scale = 0.7f;
        nr.mapId = 571, nr.left = 16, nr.right = 45, nr.top = 12, nr.bottom = 33, nr.offsetX = 1.8125f, nr.offsetY = -6.375f,
        nr.scale = 0.7f;
        ka.mapId = 1, ka.left = 16, ka.right = 46, ka.top = 9, ka.bottom = 52, ka.offsetX = -20.1875f, ka.offsetY = 3.3125f,
        ka.scale = 0.7f;
        float x = 0, y = 0;
        fmt::worldLevelPoint(ek, 32.0f - 18171.97f / fmt::kMinimapTileYards, 32.0f - 11176.34f / fmt::kMinimapTileYards, x, y);
        check(near(x * yardsPerUnit, 18542.31f, 15.0f) && near(y * yardsPerUnit, 3585.57f, 15.0f),
              "world level: the map 0 corner where the client puts it");
        fmt::worldLevelPoint(nr, 32.0f - 9217.15f / fmt::kMinimapTileYards, 32.0f - 10593.38f / fmt::kMinimapTileYards, x, y);
        check(near(x * yardsPerUnit, 16020.94f, 15.0f) && near(y * yardsPerUnit, 454.25f, 15.0f),
              "world level: the map 571 corner where the client puts it");
        // The three boxes are side by side on the 1002 x 668 map without overlapping (the hover test is the box).
        float kx, ky, kw, kh, ex, ey, ew, eh, nx, ny, nw, nh;
        fmt::worldLevelBox(ka, kx, ky, kw, kh);
        fmt::worldLevelBox(ek, ex, ey, ew, eh);
        fmt::worldLevelBox(nr, nx, ny, nw, nh);
        check(kx + kw < nx && nx + nw < ex && ex + ew < 1002.0f && kx > -5.0f && ny > 0.0f && ky + kh < 668.0f,
              "world level: maps 1 | 571 | 0, apart, on the map");
        check(near(fmt::worldLevelHighlightSide(ka), 16.0f * 0.7f * 44.0f, 0.01f), "world level: map 1's highlight side");
        int nb = 0;
        const fmt::CosmicButton* cb = fmt::cosmicButtons(nb);
        check(nb == 2 && near(cb[0].hlX, 23.0f, 0.01f) && near(cb[0].hlY, 34.5f, 0.01f) && near(cb[1].hlX, 103.0f, 0.01f) &&
                  near(cb[1].hlY, 10.5f, 0.01f),
              "cosmic buttons: the highlight rects of WorldMapFrame.xml");
    }
    {  // The WMO minimap's rules and the walk / level readers the PSP uses
        // the client's texture sizes of map 389's groups 6, 7, 9, 2 and another dungeon's group 6 (their BLPs)
        check(dmTilePixels(42.5f) == 128 && dmTilePixels(27.1f) == 64 && dmTilePixels(20.4f) == 64 &&
                  dmTilePixels(124.3f) == 256 && dmTilePixels(28.2f) == 64 && dmTilePixels(114.1f) == 256 &&
                  dmTilePixels(17.3f) == 64 && dmTilePixels(300.0f) == 256 && dmTilePixels(5.0f) == 32,
              "wmo minimap: clamp(nextpow2(size / 0.5), 32, 256) px");
        // three groups in a row, portals 0-1 (x = 10) and 1-2 (x = 20); group 2 exterior (0x8): not walked
        std::vector<uint8_t> b;
        fmt::WmoMinimapHeader h{};
        std::memcpy(h.magic, "WMMI", 4);
        h.version = fmt::kWmoMinimapVersion;
        h.instanceCount = 1;
        h.groupCount = 3;
        h.portalCount = 4;
        putStruct(b, h);
        fmt::WmoMinimapInstance in{};
        for (float* m : {in.toWorld, in.toLocal}) m[0] = m[4] = m[8] = 1.0f;
        in.groupCount = 3;
        putStruct(b, in);
        const uint32_t flags[3] = {0x2000u, 0x2000u, 0x8u};
        for (int g = 0; g < 3; ++g) {
            fmt::WmoMinimapGroup G{};
            G.bmin[0] = g * 10.0f;
            G.bmax[0] = g * 10.0f + 10.0f;
            G.bmax[1] = G.bmax[2] = 10.0f;
            G.flags = flags[g];
            G.wmoGroupId = 100u + static_cast<uint32_t>(g);
            G.firstPortal = g == 0 ? 0u : g == 1 ? 1u : 3u;
            G.portalCount = g == 1 ? 2 : 1;
            putStruct(b, G);
        }
        const int pg[4] = {1, 0, 2, 1};
        const float px[4] = {10.0f, 10.0f, 20.0f, 20.0f};
        for (int k = 0; k < 4; ++k) {
            fmt::WmoMinimapPortal P{};
            P.group = static_cast<uint16_t>(pg[k]);
            P.bmin[0] = P.bmax[0] = px[k];
            P.bmax[1] = P.bmax[2] = 5.0f;
            putStruct(b, P);
        }
        fmt::WmoMinimapView v;
        uint8_t seen[3] = {};
        const float lo[3] = {-50.0f, -50.0f, -50.0f}, hi[3] = {50.0f, 50.0f, 50.0f};
        const bool att = v.attach(b.data(), static_cast<uint32_t>(b.size()));
        const int walked = att ? fmt::wmoMinimapWalk(v, v.inst[0], 0, lo, hi, seen) : 0;
        check(att && walked == 2 && seen[0] && seen[1] && !seen[2], "wmo minimap: the portal walk keeps to interiors");
        uint8_t seen2[3] = {};
        const float lo2[3] = {0.0f, 0.0f, 0.0f}, hi2[3] = {5.0f, 5.0f, 5.0f};  // the box short of the portal at x = 10
        check(att && fmt::wmoMinimapWalk(v, v.inst[0], 0, lo2, hi2, seen2) == 1, "wmo minimap: a portal off the view box");
        // a dungeon map: a terrain level + 2 floors, group 101 on floor 2 above z 5
        std::vector<uint8_t> d;
        fmt::DungeonMapHeader dh{};
        std::memcpy(dh.magic, "WDMP", 4);
        dh.version = fmt::kDungeonMapVersion;
        dh.floorCount = 2;
        dh.chunkCount = 2;
        dh.terrainLevel = 1;
        dh.namesSize = 1;
        putStruct(d, dh);
        for (int f = 0; f < 2; ++f) {
            fmt::DungeonFloor F{};
            F.id = 10u + static_cast<uint32_t>(f);
            F.floor = static_cast<uint16_t>(f + 1);
            F.locLeft = F.locTop = 100.0f;
            putStruct(d, F);
        }
        putStruct(d, fmt::DungeonChunk{101, 10, -10000.0f});
        putStruct(d, fmt::DungeonChunk{101, 11, 5.0f});
        d.push_back(0);
        fmt::DungeonMapView dv;
        const bool datt = dv.attach(d.data(), static_cast<uint32_t>(d.size()));
        check(datt && dv.levels() == 3 && dv.floorOfLevel(1) == -1 && dv.floorOfLevel(3) == 1 &&
                  dv.playerLevel(101, 0, 0, 0.0f) == 2 && dv.playerLevel(101, 0, 0, 7.0f) == 3 && dv.playerLevel(0, 0, 0, 0) == 1,
              "dungeon map: levels, the terrain level, DungeonMapChunk by group and MinZ");
    }
    std::printf("selftest: %s (%d failures)%s%s\n", fails ? "FAILED" : "OK", fails, dumpDir.empty() ? "" : ", dump in ",
                dumpDir.c_str());
    if (dumpDir.empty()) std::filesystem::remove_all(dir);
    return fails ? 1 : 0;
}

#include "dungeon_minimap_bake.hpp"  // inside this namespace (uses the helpers above)

}  // namespace

int main(int argc, char** argv) {
    std::string root, out, dump, mapName = "Azeroth", worldmapMode = "near";
    std::vector<Box> boxes;
    bool all = false, doVerify = false, listAssets = false, selfTest = false;
    bool worldLevels = false;  // --world (data/worldmap.pak)
    int tileSize = 128, wmTile = 128;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) root = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else if (a == "--map" && i + 1 < argc) mapName = argv[++i];
        else if (a == "--tile" && i + 1 < argc) tileSize = std::atoi(argv[++i]);
        else if (a == "--wmtile" && i + 1 < argc) wmTile = std::atoi(argv[++i]);
        else if (a == "--worldmap" && i + 1 < argc) worldmapMode = argv[++i];
        else if (a == "--around" && i + 3 < argc) {
            const int x = std::atoi(argv[i + 1]), y = std::atoi(argv[i + 2]), r = std::atoi(argv[i + 3]);
            boxes.push_back({x - r, y - r, x + r, y + r});
            i += 3;
        } else if (a == "--all") all = true;
        else if (a == "--verify") doVerify = true;
        else if (a == "--list-assets") listAssets = true;
        else if (a == "--selftest") selfTest = true;
        else if (a == "--world") worldLevels = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (selfTest) return selftest(dump);
    if (worldLevels && !root.empty()) {  // --world: the world map's levels
        if (wmTile != 64 && wmTile != 128 && wmTile != 256) {
            std::fprintf(stderr, "--wmtile must be 64, 128 or 256\n");
            return 2;
        }
        return bakeWorldLevels(root, out.empty() ? std::string("psp/bin/data/worldmap.pak") : out, wmTile, doVerify, dump,
                               listAssets);
    }
    if (root.empty() || (boxes.empty() && !all)) {
        std::fprintf(stderr,
                     "usage: psp_minimap_conv --extracted <dir> [--map Azeroth] (--around X Y R ... | --all)\n"
                     "         [--tile 128] [--worldmap near|all|none] [--wmtile 128] [--out <pak>]\n"
                     "         [--verify] [--dump <dir>] [--list-assets]\n"
                     "       psp_minimap_conv --extracted <dir> --world [--wmtile 128] [--out <worldmap.pak>]\n"
                     "         [--verify] [--dump <dir>] [--list-assets]\n"
                     "       psp_minimap_conv --selftest [--dump <dir>]\n");
        return 2;
    }
    if (tileSize != 64 && tileSize != 128 && tileSize != 256) {
        std::fprintf(stderr, "--tile must be 64, 128 or 256\n");
        return 2;
    }
    if (wmTile != 64 && wmTile != 128 && wmTile != 256) {
        std::fprintf(stderr, "--wmtile must be 64, 128 or 256\n");
        return 2;
    }
    if (out.empty()) out = "psp/bin/data/minimap_" + lower(mapName) + ".pak";
    const uint32_t mapId = mapIdByName(root, mapName);
    if (mapId == ~0u) {
        std::fprintf(stderr, "unknown map %s (Map.dbc directory)\n", mapName.c_str());
        return 1;
    }
    const auto trs = parseTrs(root);
    const std::string prefix = lower(mapName) + "\\map";
    // Tiles to bake.
    std::set<std::pair<int, int>> want;
    if (all) {
        for (const auto& [k, h] : trs) {
            int x = 0, y = 0;
            if (k.rfind(prefix, 0) == 0 && std::sscanf(k.c_str() + prefix.size(), "%d_%d", &x, &y) == 2) want.insert({x, y});
        }
    }
    for (const Box& b : boxes)
        for (int y = std::max(0, b.y0); y <= std::min(63, b.y1); ++y)
            for (int x = std::max(0, b.x0); x <= std::min(63, b.x1); ++x) want.insert({x, y});
    // Areas (upstream ZoneManager / DataRepository columns; name in the client's locale).
    pipeline::DBCFile at;
    std::vector<AreaRow> allAreas;
    std::unordered_map<uint32_t, uint32_t> exploreFlagByArea;
    std::unordered_map<uint32_t, std::vector<uint32_t>> childBits;
    std::unordered_map<uint32_t, std::string> areaNames;
    if (loadDbc(root, "areatable.dbc", at) && at.getFieldCount() > 12) {
        const int loc = pickLocaleColumn(at, 11);
        for (uint32_t i = 0; i < at.getRecordCount(); ++i) {
            AreaRow r;
            r.id = at.getUInt32(i, 0);
            r.mapId = at.getUInt32(i, 1);
            r.parent = at.getUInt32(i, 2);
            r.exploreFlag = at.getUInt32(i, 3);
            r.name = at.getString(i, 11 + loc);
            if (r.name.empty()) r.name = at.getString(i, 11);
            if (r.id == 0) continue;
            exploreFlagByArea[r.id] = r.exploreFlag;
            if (r.parent) childBits[r.parent].push_back(r.exploreFlag);
            areaNames[r.id] = r.name;
            allAreas.push_back(std::move(r));
        }
        std::printf("areatable: %zu rows, locale column %d\n", allAreas.size(), loc);
    } else {
        std::printf("warning: dbfilesclient/areatable.dbc missing - no zone names\n");
    }
    // World map rows.
    std::vector<ZoneRow> zones;
    std::set<uint32_t> areaRefs;
    const bool haveWm = worldmapMode != "none" &&
                        readWorldMap(root, mapId, zones, areaRefs, exploreFlagByArea, childBits, areaNames);
    // Which zones get their textures: continents always, zones by the boxes (or all).
    float boxMinY = 1e9f, boxMaxY = -1e9f, boxMinX = 1e9f, boxMaxX = -1e9f;  // server Y / X
    for (const auto& [x, y] : want) {
        boxMinY = std::min(boxMinY, (31.0f - x) * fmt::kMinimapTileYards);
        boxMaxY = std::max(boxMaxY, (32.0f - x) * fmt::kMinimapTileYards);
        boxMinX = std::min(boxMinX, (31.0f - y) * fmt::kMinimapTileYards);
        boxMaxX = std::max(boxMaxX, (32.0f - y) * fmt::kMinimapTileYards);
    }
    for (ZoneRow& z : zones) {
        if (worldmapMode == "all" || z.rec.areaId == 0) {
            z.bake = true;
            continue;
        }
        const float y0 = std::min(z.rec.locLeft, z.rec.locRight), y1 = std::max(z.rec.locLeft, z.rec.locRight);
        const float x0 = std::min(z.rec.locTop, z.rec.locBottom), x1 = std::max(z.rec.locTop, z.rec.locBottom);
        z.bake = z.rec.mapId == mapId && y1 >= boxMinY && y0 <= boxMaxY && x1 >= boxMinX && x0 <= boxMaxX;
    }
    std::string continentFolder;
    for (const ZoneRow& z : zones)
        if (z.rec.areaId == 0 && z.rec.mapId == mapId && continentFolder.empty()) continentFolder = z.folder;
    if (continentFolder.empty()) continentFolder = mapName;
    if (listAssets) {
        std::printf("# psp_mpq_extract --prefix (read-only) for psp_minimap_conv --map %s\n", mapName.c_str());
        std::printf("DBFilesClient\\Map.dbc\nDBFilesClient\\AreaTable.dbc\nDBFilesClient\\WorldMapArea.dbc\n"
                    "DBFilesClient\\WorldMapOverlay.dbc\nDBFilesClient\\WorldMapTransforms.dbc\n"
                    "Textures\\Minimap\\md5translate.trs\n");
        if (trs.empty()) std::printf("Textures\\Minimap\\   (md5translate.trs not extracted yet: the whole folder)\n");
        for (const auto& [x, y] : want) {
            bool known = false;
            const std::string p = minimapTilePath(root, trs, mapName, x, y, known);
            if (known) std::printf("Textures\\Minimap\\%s\n", std::filesystem::path(p).filename().string().c_str());
        }
        std::printf("Interface\\WorldMap\\%s.zmp\n", continentFolder.c_str());
        for (const ZoneRow& z : zones)
            if (z.bake) std::printf("Interface\\WorldMap\\%s\\\n", z.folder.c_str());
        std::printf("Interface\\Minimap\\   (ring and arrow for build_ui_pak.py)\n");
        std::printf("World\\Maps\\%s\\   (the WDT and the ADTs: the area grid)\n", mapName.c_str());
        return 0;
    }
    // Areas kept: the map's own + every one a world map row references.
    std::vector<AreaRow> keep;
    for (const AreaRow& r : allAreas)
        if (r.mapId == mapId || areaRefs.count(r.id)) keep.push_back(r);
    std::vector<PakItem> items;
    // Tiles.
    int baked = 0, missingFile = 0, notInTrs = 0;
    std::vector<PakItem> tileItems;
    for (const auto& [x, y] : want) {
        bool known = false;
        const std::string path = minimapTilePath(root, trs, mapName, x, y, known);
        Image img;
        if (!loadBlp(path, img)) {
            if (known) {
                ++missingFile;
                std::printf("  missing %s (map%d_%d)\n", path.c_str(), x, y);
            } else {
                ++notInTrs;  // ocean / nothing there: upstream draws its noData colour
            }
            continue;
        }
        char n[48];
        fmt::minimapTileName(n, sizeof(n), x, y);
        tileItems.push_back({n, kKindTexture, encodeWtex(resample(img, tileSize, tileSize), true)});
        ++baked;
    }
    std::printf("minimap: map=%s(%u) tiles %d baked, %d not in md5translate, %d files missing, tile %d\n",
                mapName.c_str(), mapId, baked, notInTrs, missingFile, tileSize);
    // The area grid of every ADT tile the WDT lists in the boxes (--all: the map),
    // and the areas it names kept in mmhdr (the minimap's zone text looks them up there).
    std::map<std::pair<int, int>, std::array<uint16_t, 256>> areaTiles;
    {
        std::vector<std::pair<int, int>> adts = wdtTiles(root, mapName);
        if (adts.empty())  // no WDT: the boxes' tiles that have an ADT file
            for (const auto& xy : want) adts.push_back(xy);
        for (const auto& [x, y] : adts) {
            if (!all && !want.count({x, y})) continue;
            std::array<uint16_t, 256> ids{};
            const std::string adt = root + "/world/maps/" + lower(mapName) + "/" + lower(mapName) + "_" + std::to_string(x) +
                                    "_" + std::to_string(y) + ".adt";
            if (adtAreaIds(adt, ids)) areaTiles[{x, y}] = ids;
        }
        std::set<uint32_t> gridAreas;
        for (const auto& [xy, ids] : areaTiles)
            for (uint16_t a : ids)
                if (a) gridAreas.insert(a);
        for (const AreaRow& r : allAreas)  // an area of another map named by a chunk (buildMinimapHeader dedups)
            if (gridAreas.count(r.id) && r.mapId != mapId && !areaRefs.count(r.id)) keep.push_back(r);
    }
    items.push_back({"mmhdr", kKindRaw, buildMinimapHeader(mapId, mapName, tileSize, baked, keep)});
    items.insert(items.end(), std::make_move_iterator(tileItems.begin()), std::make_move_iterator(tileItems.end()));
    if (!areaTiles.empty()) {
        size_t multi = 0;
        std::vector<uint8_t> ag = buildAreaGrid(mapId, areaTiles, &multi);
        std::printf("agrid: %zu ADT tiles read, %zu with several areas, %zu KB\n", areaTiles.size(), multi, ag.size() / 1024);
        items.push_back({"agrid", kKindRaw, std::move(ag)});
    } else {
        std::printf("agrid: no ADT tiles under world/maps/%s - none baked\n", lower(mapName).c_str());
    }
    // World map.
    if (haveWm) {
        std::vector<uint16_t> zmp;
        std::vector<uint8_t> raw;
        if (readFile(root + "/" + lower("interface/worldmap/" + continentFolder + ".zmp"), raw) &&
            raw.size() == 128u * 128u * 4u) {
            zmp.resize(128 * 128);
            for (size_t i = 0; i < zmp.size(); ++i) {
                uint32_t v = 0;
                std::memcpy(&v, &raw[i * 4], 4);
                zmp[i] = static_cast<uint16_t>(v > 0xFFFF ? 0 : v);
            }
        } else {
            std::printf("  no %s.zmp: continent clicks fall back to the player's zone\n", continentFolder.c_str());
        }
        std::vector<ZoneArt> arts(zones.size());
        std::vector<std::string> missing;
        int zonesWithArt = 0;
        for (size_t i = 0; i < zones.size(); ++i)
            if (zones[i].bake && loadZoneArt(root, zones[i], arts[i], &missing)) ++zonesWithArt;
        items.push_back({"wmhdr", kKindRaw, buildWorldMapHeader(mapId, wmTile, zones, zmp)});
        for (size_t i = 0; i < zones.size(); ++i)
            if (zones[i].bake) appendZoneArt(items, zones[i], arts[i], wmTile);
        std::printf("worldmap: %zu zones, %d with art, %zu files missing%s\n", zones.size(), zonesWithArt, missing.size(),
                    missing.empty() ? "" : " (first below)");
        for (size_t i = 0; i < missing.size() && i < 10; ++i) std::printf("  missing %s\n", missing[i].c_str());
    }
    // A dungeon or raid (Map.dbc InstanceType 1 / 2) - its dungeon world map ("dmap" + floor art) and
    // the minimap of its WMOs ("wmmi" + "u_<n>"; a global-WMO dungeon has nothing else, a tiled one keeps its "t_").
    {
        DmFloorStats fs;
        if (worldmapMode != "none" && bakeDungeonMap(root, mapId, wmTile, items, fs)) {
            std::printf("dungeon map: wma=%u floors=%d chunks=%d terrain=%d art tiles=%d missing=%d\n", fs.wma, fs.floors,
                        fs.chunks, fs.terrain ? 1 : 0, fs.artTiles, fs.missing);
            DmStats ws;
            const bool have = bakeWmoMinimap(root, mapName, trs, items, ws);
            std::printf("wmo minimap: %s instances=%zu groups=%zu tiles=%zu baked=%zu empty=%zu missing=%zu size-off=%zu kb=%zu\n",
                        have ? "yes" : "none", ws.instances, ws.groups, ws.tiles, ws.baked, ws.empty, ws.missing, ws.sizeOff,
                        ws.bytes / 1024);
        }
    }
    std::filesystem::create_directories(std::filesystem::path(out).parent_path().empty()
                                            ? std::filesystem::path(".")
                                            : std::filesystem::path(out).parent_path());
    if (!writePak(out, items)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    if (!doVerify && dump.empty()) return 0;
    VerifyOpts vo;
    for (const auto& [x, y] : want) {
        bool known = false;
        const std::string p = minimapTilePath(root, trs, mapName, x, y, known);
        if (known && std::filesystem::exists(p)) vo.expectTiles.insert({x, y});
    }
    vo.dumpDir = dump;
    const int errors = verifyPak(out, vo);
    return errors && doVerify ? 3 : 0;
}
