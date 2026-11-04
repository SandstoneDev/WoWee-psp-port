// psp_light_conv - the client's light tables for the PSP.
//
//   psp_light_conv --extracted <dir> --out <light.pak> [--map <id>]...   (default maps 0 and 1)
//   psp_light_conv --verify <light.pak> [--extracted <dir>]
//   psp_light_conv --dump <light.pak> --png <strip.png> [--map <id>]...
//
// Bake: DBFilesClient/Light.dbc, LightParams.dbc, LightIntBand.dbc and
// LightFloatBand.dbc, read exactly as upstream's LightingManager reads them
// (lighting_manager.cpp: loadLightDbc / loadLightParamsDbc /
// loadLightBandDbcs - the 3.3.5a field indices of dbc_layouts.json, which
// are also its fallbacks; lightPositionToWorld and the /36 radii,
// lightBandSlot for the one-based block index, 16 keys at most, time %
// 2880). Only the volumes of the chosen maps and the LightParams rows they
// name are kept. Output: include/rendering/psp/psp_light_format.hpp, entry
// "lights" (kind 3) of data/light.pak - the PSP reads it at world entry into
// RAM and closes the pak.
//
// --verify: checks the blob, prints the light at a few known places over the
// day with the PSP's own code (src/rendering/psp/psp_lighting.cpp, compiled
// here), and with --extracted also runs upstream's LightingManager (compiled
// here unchanged) at the same places and hours and compares: the PSP port
// must match it (lightzero=0, upstream's blend; exit code = mismatches).
// --dump: a PNG strip per volume - rows ambient, diffuse, fog, sky top,
// sky middle, band 1, band 2; columns = the day in 7.5 min steps.
#include "pipeline/asset_manager.hpp"
#include "core/coordinates.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/light_band_block.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/psp/psp_light_format.hpp"
#include "rendering/psp/psp_lighting.hpp"

#include "conv_common.hpp"
#include "forest_bake.hpp"  // PakView, writePng

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace wowee;
using namespace wowee::pspconv;
namespace fmt = wowee::rendering::psp;

namespace {

std::string g_root;

bool loadDbc(const std::string& name, pipeline::DBCFile& dbc) {
    std::vector<uint8_t> bytes;
    if (!readFile(g_root + "/dbfilesclient/" + lower(name), bytes)) {
        std::fprintf(stderr, "missing %s/dbfilesclient/%s\n", g_root.c_str(), lower(name).c_str());
        return false;
    }
    if (!dbc.load(bytes)) {
        std::fprintf(stderr, "cannot parse %s\n", name.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

struct Band {
    uint32_t count = 0;
    uint16_t times[16] = {};
    uint32_t colors[16] = {};
    float values[16] = {};
};
struct Params {
    uint32_t id = 0, skybox = 0;
    Band color[fmt::kLightColorChannels];
    Band fl[fmt::kLightFloatChannels];
};

int bake(const std::string& out, const std::set<uint32_t>& maps) {
    pipeline::DBCFile light, params, intBand, floatBand;
    if (!loadDbc("Light.dbc", light) || !loadDbc("LightParams.dbc", params) ||
        !loadDbc("LightIntBand.dbc", intBand) || !loadDbc("LightFloatBand.dbc", floatBand))
        return 1;
    // LightParams: id -> profile (upstream loadLightParamsDbc: field 0 id, 2 skybox).
    std::map<uint32_t, Params> profiles;
    for (uint32_t i = 0; i < params.getRecordCount(); ++i) {
        Params p;
        p.id = params.getUInt32(i, 0);
        if (params.getFieldCount() > 2) p.skybox = params.getUInt32(i, 2);
        profiles[p.id] = p;
    }
    // Bands (upstream loadLightBandDbcs): field 0 block index, 1 count,
    // 2.. time keys, 18.. values.
    auto readBands = [&](pipeline::DBCFile& dbc, uint32_t channels, bool colour) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
            const rendering::LightBandSlot slot = rendering::lightBandSlot(dbc.getUInt32(i, 0), channels);
            if (!slot.valid) continue;
            auto it = profiles.find(slot.lightParamsId);
            if (it == profiles.end() || slot.channel >= channels) continue;
            Band& b = colour ? it->second.color[slot.channel] : it->second.fl[slot.channel];
            b.count = std::min<uint32_t>(dbc.getUInt32(i, 1), 16);
            for (uint32_t k = 0; k < b.count; ++k) {
                b.times[k] = static_cast<uint16_t>(dbc.getUInt32(i, 2 + k) % 2880u);
                if (colour) b.colors[k] = dbc.getUInt32(i, 18 + k);
                else b.values[k] = dbc.getFloat(i, 18 + k);
            }
            ++used;
        }
        return used;
    };
    const uint32_t intUsed = readBands(intBand, fmt::kLightColorChannels, true);
    const uint32_t floatUsed = readBands(floatBand, fmt::kLightFloatChannels, false);

    // Volumes of the chosen maps (upstream loadLightDbc: id 0, map 1, x/z/y
    // 2-4, inner 5, outer 6, params clear/rain/underwater 7-9).
    std::vector<fmt::LightFileVolume> volumes;
    std::vector<uint32_t> usedParams;  // LightParams ids, index = file index
    std::map<uint32_t, uint16_t> paramIndex;
    std::set<uint32_t> mapsSeen;
    uint32_t missingParams = 0, defaults = 0;
    auto indexOf = [&](uint32_t id) -> uint16_t {
        if (id == 0) return fmt::kLightNoParams;
        if (!profiles.count(id)) {
            ++missingParams;
            return fmt::kLightNoParams;
        }
        auto it = paramIndex.find(id);
        if (it != paramIndex.end()) return it->second;
        const uint16_t idx = static_cast<uint16_t>(usedParams.size());
        paramIndex[id] = idx;
        usedParams.push_back(id);
        return idx;
    };
    for (uint32_t i = 0; i < light.getRecordCount(); ++i) {
        const uint32_t mapId = light.getUInt32(i, 1);
        if (!maps.empty() && !maps.count(mapId)) continue;
        fmt::LightFileVolume v{};
        v.lightId = light.getUInt32(i, 0);
        v.mapId = mapId;
        const float dbcX = light.getFloat(i, 2), dbcZ = light.getFloat(i, 3), dbcY = light.getFloat(i, 4);
        const glm::vec3 p = rendering::lightPositionToWorld(dbcX, dbcY, dbcZ);
        v.position[0] = p.x;
        v.position[1] = p.y;
        v.position[2] = p.z;
        const float innerRaw = light.getFloat(i, 5), outerRaw = light.getFloat(i, 6);
        v.innerRadius = innerRaw / rendering::LIGHT_COORD_UNITS_PER_YARD;
        v.outerRadius = outerRaw / rendering::LIGHT_COORD_UNITS_PER_YARD;
        v.params[0] = indexOf(light.getUInt32(i, 7));
        v.params[1] = light.getFieldCount() > 8 ? indexOf(light.getUInt32(i, 8)) : fmt::kLightNoParams;
        v.params[2] = light.getFieldCount() > 9 ? indexOf(light.getUInt32(i, 9)) : fmt::kLightNoParams;
        if (dbcX == 0.0f && dbcY == 0.0f && dbcZ == 0.0f && innerRaw == 0.0f && outerRaw == 0.0f) {
            v.flags |= fmt::kLightVolumeMapDefault;
            ++defaults;
        }
        volumes.push_back(v);
        mapsSeen.insert(mapId);
        if (usedParams.size() >= fmt::kLightNoParams - 1) {
            std::fprintf(stderr, "too many LightParams rows\n");
            return 1;
        }
    }
    if (volumes.empty()) {
        std::fprintf(stderr, "no Light.dbc rows for the chosen maps\n");
        return 1;
    }

    std::vector<fmt::LightFileParams> fileParams;
    std::vector<fmt::LightFileColorKey> colorKeys;
    std::vector<fmt::LightFileFloatKey> floatKeys;
    for (uint32_t id : usedParams) {
        const Params& p = profiles[id];
        fmt::LightFileParams fp{};
        fp.lightParamsId = p.id;
        fp.lightSkyboxId = p.skybox;
        for (uint32_t c = 0; c < fmt::kLightColorChannels; ++c) {
            fp.color[c].first = static_cast<uint32_t>(colorKeys.size());
            fp.color[c].count = static_cast<uint16_t>(p.color[c].count);
            for (uint32_t k = 0; k < p.color[c].count; ++k)
                colorKeys.push_back({p.color[c].times[k], 0, p.color[c].colors[k] & 0x00FFFFFFu});
        }
        for (uint32_t c = 0; c < fmt::kLightFloatChannels; ++c) {
            fp.fl[c].first = static_cast<uint32_t>(floatKeys.size());
            fp.fl[c].count = static_cast<uint16_t>(p.fl[c].count);
            for (uint32_t k = 0; k < p.fl[c].count; ++k) floatKeys.push_back({p.fl[c].times[k], 0, p.fl[c].values[k]});
        }
        fileParams.push_back(fp);
    }

    fmt::LightFileHeader h{};
    std::memcpy(h.magic, "WLGT", 4);
    h.version = fmt::kLightFileVersion;
    h.volumeCount = static_cast<uint32_t>(volumes.size());
    h.paramsCount = static_cast<uint32_t>(fileParams.size());
    h.colorKeyCount = static_cast<uint32_t>(colorKeys.size());
    h.floatKeyCount = static_cast<uint32_t>(floatKeys.size());
    h.mapCount = static_cast<uint32_t>(mapsSeen.size());
    std::vector<uint8_t> blob;
    auto append = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        blob.insert(blob.end(), b, b + n);
    };
    append(&h, sizeof(h));
    append(volumes.data(), volumes.size() * sizeof(volumes[0]));
    append(fileParams.data(), fileParams.size() * sizeof(fileParams[0]));
    append(colorKeys.data(), colorKeys.size() * sizeof(colorKeys[0]));
    append(floatKeys.data(), floatKeys.size() * sizeof(floatKeys[0]));
    std::printf("Light.dbc %u rows, LightParams %u, int bands %u, float bands %u\n", light.getRecordCount(),
                params.getRecordCount(), intUsed, floatUsed);
    std::printf("kept: %zu volumes on %zu maps (%u map defaults), %zu params, %zu colour keys, %zu float keys, "
                "%zu bytes; params ids not in LightParams: %u\n",
                volumes.size(), mapsSeen.size(), defaults, fileParams.size(), colorKeys.size(), floatKeys.size(),
                blob.size(), missingParams);
    std::vector<PakItem> items;
    items.push_back({"lights", 3, std::move(blob)});
    return writePak(out, items) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Verify / dump
// ---------------------------------------------------------------------------

struct Place {
    const char* name;
    uint32_t map, zone;
    glm::vec3 pos;  // server coords (= upstream's render space)
};
// Known spots (server coordinates; zone = AreaTable zone for the ambience).
const Place kPlaces[] = {
    {"m0-start", 0, 12, {-8913.0f, -136.0f, 82.0f}},
    {"m0-village", 0, 12, {-9464.0f, 62.0f, 56.0f}},
    {"m0-capital", 0, 1519, {-8833.0f, 628.0f, 94.0f}},
    {"m0-start2", 0, 1, {-6240.0f, 331.0f, 383.0f}},
    {"m0-town", 0, 10, {-10560.0f, -1180.0f, 28.0f}},
    {"m1-capital", 1, 1637, {1630.0f, -4373.0f, 31.0f}},
};
const float kHours[] = {0.0f, 6.0f, 9.0f, 12.0f, 18.0f, 21.0f};

uint32_t rgb8(const glm::vec3& c) {
    auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

bool finite3(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool loadBlob(const std::string& pakPath, PakView& pak, fmt::Lighting& lighting) {
    if (!pak.open(pakPath)) {
        std::fprintf(stderr, "cannot read %s\n", pakPath.c_str());
        return false;
    }
    uint32_t size = 0;
    const uint8_t* b = pak.find("lights", size);
    if (!b) {
        std::fprintf(stderr, "%s: no \"lights\" entry\n", pakPath.c_str());
        return false;
    }
    const char* why = nullptr;
    if (!lighting.load(b, size, &why)) {
        std::fprintf(stderr, "%s: bad WLGT blob (%s)\n", pakPath.c_str(), why ? why : "?");
        return false;
    }
    return true;
}

}  // namespace

// Upstream LightingManager::initialize reads the DBCs through
// AssetManager::readFile: served from the extracted tree here (the other
// members are never touched; the manager only calls readFile).
namespace wowee::pipeline {
std::vector<uint8_t> AssetManager::readFile(const std::string& path) const {
    std::vector<uint8_t> bytes;
    wowee::pspconv::readFile(g_root + "/" + wowee::pspconv::lower(path), bytes);
    return bytes;
}
}  // namespace wowee::pipeline

namespace {

int compareWithUpstream(const fmt::Lighting& psp) {
    // The managers get an AssetManager in raw storage: its constructor lives
    // in asset_manager.cpp (MPQ/manifest code this tool does not link) and
    // readFile above uses no member.
    alignas(pipeline::AssetManager) unsigned char storage[sizeof(pipeline::AssetManager)] = {};
    auto* am = reinterpret_cast<pipeline::AssetManager*>(storage);
    struct Point {
        const Place* place;
        float hour;
        std::unique_ptr<rendering::LightingManager> up;
    };
    std::vector<Point> points;
    for (const Place& pl : kPlaces)
        for (float hour : kHours) {
            Point p{&pl, hour, std::make_unique<rendering::LightingManager>()};
            if (!p.up->initialize(am)) return 1;
            points.push_back(std::move(p));
        }
    // Upstream smooths by real time with dt clamped to 0.25 s: 10 rounds of
    // 250 ms leave exp(-12.5) of the start value (all points step together).
    for (int round = 0; round < 11; ++round) {
        for (Point& p : points) p.up->update(p.place->pos, p.place->map, p.place->zone, p.hour);
        if (round < 10) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    int mismatches = 0;
    for (const Point& p : points) {
        const rendering::LightingParams& a = p.up->getLightingParams();
        const rendering::LightingParams b = psp.sample(p.place->pos, p.place->map, p.place->zone, p.hour);
        auto diff3 = [](const glm::vec3& x, const glm::vec3& y) {
            const glm::vec3 d = glm::abs(x - y);
            return std::max({d.x, d.y, d.z});
        };
        const float dc = std::max({diff3(a.ambientColor, b.ambientColor), diff3(a.diffuseColor, b.diffuseColor),
                                   diff3(a.fogColor, b.fogColor), diff3(a.skyTopColor, b.skyTopColor),
                                   diff3(a.skyMiddleColor, b.skyMiddleColor), diff3(a.skyBand1Color, b.skyBand1Color),
                                   diff3(a.skyBand2Color, b.skyBand2Color)});
        const float dd = diff3(a.directionalDir, b.directionalDir);
        const float df = std::max(std::fabs(a.fogStart - b.fogStart), std::fabs(a.fogEnd - b.fogEnd));
        const bool bad = !(dc <= 0.004f) || !(dd <= 0.004f) || !(df <= 0.5f + 0.002f * std::fabs(a.fogEnd));
        if (bad) ++mismatches;
        std::printf("  upstream %-10s %05.2fh colour %.4f dir %.4f fog %.2f yd %s\n", p.place->name, p.hour, dc, dd, df,
                    bad ? "MISMATCH" : "ok");
    }
    std::printf("upstream comparison: %zu points, %d mismatches\n", points.size(), mismatches);
    return mismatches;
}

int verify(const std::string& pakPath) {
    PakView pak;
    fmt::Lighting lighting;
    if (!loadBlob(pakPath, pak, lighting)) return 1;
    const fmt::LightFileView& f = lighting.file();
    int errors = 0;
    uint32_t unsorted = 0, emptyBands = 0, defaults = 0, noClear = 0;
    std::set<uint32_t> maps;
    for (uint32_t i = 0; i < f.header->volumeCount; ++i) {
        const fmt::LightFileVolume& v = f.volumes[i];
        maps.insert(v.mapId);
        if (v.flags & fmt::kLightVolumeMapDefault) ++defaults;
        if (v.params[0] == fmt::kLightNoParams) ++noClear;
        if (!(v.outerRadius >= v.innerRadius) || !std::isfinite(v.position[0]) || !std::isfinite(v.position[1])) {
            if (++errors <= 20) std::fprintf(stderr, "verify: light %u: bad position/radii\n", v.lightId);
        }
    }
    for (uint32_t i = 0; i < f.header->paramsCount; ++i) {
        const fmt::LightFileParams& p = f.params[i];
        for (const fmt::LightFileBand& b : p.color) {
            if (!b.count) ++emptyBands;
            for (uint32_t k = 1; k < b.count; ++k)
                if (f.colorKeys[b.first + k].time < f.colorKeys[b.first + k - 1].time) ++unsorted;
        }
        for (const fmt::LightFileBand& b : p.fl) {
            if (!b.count) ++emptyBands;
            for (uint32_t k = 1; k < b.count; ++k)
                if (f.floatKeys[b.first + k].time < f.floatKeys[b.first + k - 1].time) ++unsorted;
        }
    }
    std::printf("%s: WLGT v%u, %u volumes on %zu maps (%u map defaults, %u without clear params), %u params, "
                "%u colour keys, %u float keys, %u bytes in RAM\n",
                pakPath.c_str(), f.header->version, f.header->volumeCount, maps.size(), defaults, noClear,
                f.header->paramsCount, f.header->colorKeyCount, f.header->floatKeyCount, lighting.bytes());
    std::printf("bands without keys %u (upstream samples grey 0.5 / 1.0), keys out of time order %u\n", emptyBands,
                unsorted);
    // The PSP's code at known places over the day (upstream blend, lightzero=0).
    for (const Place& pl : kPlaces) {
        for (float hour : kHours) {
            uint32_t v0 = 0, v1 = 0, n = 0;
            const rendering::LightingParams p = lighting.sample(pl.pos, pl.map, pl.zone, hour, false, false, &v0, &v1, &n);
            if (!finite3(p.ambientColor) || !finite3(p.diffuseColor) || !finite3(p.fogColor) ||
                !finite3(p.skyTopColor) || !std::isfinite(p.fogStart) || !std::isfinite(p.fogEnd)) {
                if (++errors <= 20) std::fprintf(stderr, "verify: %s %.0fh: non-finite light\n", pl.name, hour);
            }
            std::printf("  %-10s %05.2fh vol %u/%u (%u) amb %06X dif %06X fog %06X %.0f-%.0f sky %06X %06X %06X %06X\n",
                        pl.name, hour, v0, v1, n, rgb8(p.ambientColor), rgb8(p.diffuseColor), rgb8(p.fogColor),
                        p.fogStart, p.fogEnd, rgb8(p.skyTopColor), rgb8(p.skyMiddleColor), rgb8(p.skyBand1Color),
                        rgb8(p.skyBand2Color));
        }
    }
    // What the PSP shows by default (lightzero=1: the blend starts from zero).
    lighting.zeroBlend = true;
    for (const Place& pl : kPlaces) {
        for (float hour : {0.0f, 12.0f, 18.0f}) {
            const rendering::LightingParams p = lighting.sample(pl.pos, pl.map, pl.zone, hour);
            std::printf("  lightzero=1 %-10s %05.2fh amb %06X dif %06X fog %06X %.0f-%.0f sky %06X %06X %06X %06X\n",
                        pl.name, hour, rgb8(p.ambientColor), rgb8(p.diffuseColor), rgb8(p.fogColor), p.fogStart,
                        p.fogEnd, rgb8(p.skyTopColor), rgb8(p.skyMiddleColor), rgb8(p.skyBand1Color),
                        rgb8(p.skyBand2Color));
        }
    }
    lighting.zeroBlend = false;
    if (!g_root.empty()) errors += compareWithUpstream(lighting);
    std::printf("verify errors=%d\n", errors);
    return errors;
}

int dump(const std::string& pakPath, const std::string& pngPath, const std::set<uint32_t>& maps) {
    PakView pak;
    fmt::Lighting lighting;
    if (!loadBlob(pakPath, pak, lighting)) return 1;
    const fmt::LightFileView& f = lighting.file();
    constexpr int kCols = 192;  // 7.5 min per column
    constexpr int kRowH = 5, kRows = 7, kGap = 3;
    std::vector<uint32_t> list;
    for (uint32_t i = 0; i < f.header->volumeCount; ++i)
        if ((maps.empty() || maps.count(f.volumes[i].mapId)) && f.volumes[i].params[0] != fmt::kLightNoParams)
            list.push_back(i);
    if (list.empty()) {
        std::fprintf(stderr, "dump: no volumes\n");
        return 1;
    }
    const int w = kCols, h = static_cast<int>(list.size()) * (kRows * kRowH + kGap);
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    std::printf("dump %s: %zu volumes, top to bottom (rows: ambient, diffuse, fog, sky top, sky middle, band 1, "
                "band 2; columns: 00:00..24:00)\n",
                pngPath.c_str(), list.size());
    for (size_t li = 0; li < list.size(); ++li) {
        const fmt::LightFileVolume& v = f.volumes[list[li]];
        const int y0 = static_cast<int>(li) * (kRows * kRowH + kGap);
        for (int x = 0; x < kCols; ++x) {
            const uint16_t t = static_cast<uint16_t>(x * 2880 / kCols);
            const rendering::LightingParams p = lighting.sampleProfile(v.params[0], t);
            const glm::vec3 rows[kRows] = {p.ambientColor, p.diffuseColor, p.fogColor, p.skyTopColor,
                                           p.skyMiddleColor, p.skyBand1Color, p.skyBand2Color};
            for (int r = 0; r < kRows; ++r) {
                const uint32_t c = rgb8(rows[r]);
                for (int yy = 0; yy < kRowH; ++yy) {
                    uint8_t* px = &rgba[(static_cast<size_t>(y0 + r * kRowH + yy) * w + x) * 4];
                    px[0] = static_cast<uint8_t>(c >> 16);
                    px[1] = static_cast<uint8_t>(c >> 8);
                    px[2] = static_cast<uint8_t>(c);
                    px[3] = 255;
                }
            }
        }
        std::printf("  %3zu: light %u map %u at (%.0f, %.0f, %.0f) r %.0f..%.0f params %u%s\n", li, v.lightId, v.mapId,
                    v.position[0], v.position[1], v.position[2], v.innerRadius, v.outerRadius,
                    f.params[v.params[0]].lightParamsId, (v.flags & fmt::kLightVolumeMapDefault) ? " (map default)" : "");
    }
    return writePng(pngPath, w, h, rgba) ? 0 : 1;
}

}  // namespace

namespace {

// --selftest <dir>: synthetic Light*.dbc (no client data needed) ->
// <dir>/light.pak, then --verify with upstream's LightingManager over the
// same files, then a --dump strip. Volumes: a map default, a starting area and an
// overlapping village light (two-volume blend + smoothstep), a capital city with
// a rain variant, another capital on map 1 (+ a light naming a missing params id).
// Bands: 1..5 keys, first key after midnight (wrap), one channel without a
// row (grey fallback).
bool writeDbc(const std::string& path, uint32_t fields, const std::vector<std::vector<uint32_t>>& rows) {
    std::vector<uint8_t> b = {'W', 'D', 'B', 'C'};
    put<uint32_t>(b, static_cast<uint32_t>(rows.size()));
    put<uint32_t>(b, fields);
    put<uint32_t>(b, fields * 4);
    put<uint32_t>(b, 1);
    for (const auto& r : rows)
        for (uint32_t f = 0; f < fields; ++f) put<uint32_t>(b, f < r.size() ? r[f] : 0u);
    b.push_back(0);
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(o);
}

uint32_t fbits(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return u;
}

int selftest(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir + "/dbfilesclient", ec);
    const float zp = core::coords::ZEROPOINT;
    // World -> Light.dbc fields (inverse of lightPositionToWorld).
    auto lightRow = [&](uint32_t id, uint32_t map, glm::vec3 w, float inner, float outer, uint32_t clear,
                        uint32_t rain) {
        const float dbcY = (zp - w.x) * 36.0f, dbcX = (zp - w.y) * 36.0f, dbcZ = w.z * 36.0f;
        return std::vector<uint32_t>{id, map, fbits(dbcX), fbits(dbcZ), fbits(dbcY), fbits(inner * 36.0f),
                                     fbits(outer * 36.0f), clear, rain, 0, 0, 0, 0, 0, 0};
    };
    std::vector<std::vector<uint32_t>> light = {
        {1, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0},  // map default (origin, zero radii)
        lightRow(2, 0, {-8913.0f, -136.0f, 82.0f}, 150.0f, 400.0f, 1, 0),
        lightRow(3, 0, {-9464.0f, 62.0f, 56.0f}, 100.0f, 900.0f, 2, 0),
        lightRow(4, 0, {-8833.0f, 628.0f, 94.0f}, 300.0f, 600.0f, 3, 4),
        lightRow(5, 1, {1630.0f, -4373.0f, 31.0f}, 400.0f, 800.0f, 5, 0),
        lightRow(6, 1, {1630.0f, -4373.0f, 31.0f}, 400.0f, 800.0f, 99, 0),  // params id not in LightParams
    };
    std::vector<std::vector<uint32_t>> params;
    for (uint32_t id = 1; id <= 6; ++id) params.push_back({id, 0, id == 3 ? 7u : 0u, 0, 0, 0, 0, 0, 0, 0});
    std::vector<std::vector<uint32_t>> ib, fb;
    uint32_t seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 8) & 0xFFFFFFu;
    };
    for (uint32_t id = 1; id <= 6; ++id) {
        for (uint32_t ch = 0; ch < 18; ++ch) {
            if (id == 2 && ch == 6) continue;  // no row: grey fallback
            const uint32_t n = 1 + (id + ch) % 5;
            std::vector<uint32_t> r(34, 0);
            r[0] = (id - 1) * 18 + ch + 1;
            r[1] = n;
            const uint32_t start = (ch % 3) * 200;  // first key after midnight: wrap
            for (uint32_t k = 0; k < n; ++k) {
                r[2 + k] = start + k * (2400 / n);
                r[18 + k] = rnd();
            }
            ib.push_back(r);
        }
        for (uint32_t ch = 0; ch < 6; ++ch) {
            const uint32_t n = 1 + (id + ch) % 4;
            std::vector<uint32_t> r(34, 0);
            r[0] = (id - 1) * 6 + ch + 1;
            r[1] = n;
            for (uint32_t k = 0; k < n; ++k) {
                r[2 + k] = 100 + k * (2600 / n);
                const float kf = static_cast<float>(k);
                const float v = ch == 0 ? 6000.0f + 3000.0f * kf : ch == 1 ? 0.2f + 0.1f * kf : 0.5f * kf;
                r[18 + k] = fbits(v);
            }
            fb.push_back(r);
        }
    }
    const std::string d = dir + "/dbfilesclient/";
    if (!writeDbc(d + "light.dbc", 15, light) || !writeDbc(d + "lightparams.dbc", 10, params) ||
        !writeDbc(d + "lightintband.dbc", 34, ib) || !writeDbc(d + "lightfloatband.dbc", 34, fb)) {
        std::fprintf(stderr, "selftest: cannot write DBCs under %s\n", dir.c_str());
        return 1;
    }
    g_root = dir;
    const std::string pak = dir + "/light.pak";
    if (bake(pak, {0u, 1u}) != 0) return 1;
    int errors = verify(pak);
    if (dump(pak, dir + "/light_strip.png", {}) != 0) ++errors;
    std::printf("selftest %s errors=%d\n", errors ? "FAILED" : "ok", errors);
    return errors;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out, verifyPath, dumpPath, png;
    std::set<uint32_t> maps;
    bool allMaps = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) g_root = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--map" && i + 1 < argc) {
            const std::string m = argv[++i];
            if (m == "all") allMaps = true;
            else maps.insert(static_cast<uint32_t>(std::stoul(m)));
        } else if (a == "--verify" && i + 1 < argc) verifyPath = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == "--png" && i + 1 < argc) png = argv[++i];
        else if (a == "--selftest" && i + 1 < argc) return selftest(argv[++i]) == 0 ? 0 : 3;
    }
    if (!verifyPath.empty()) return verify(verifyPath) == 0 ? 0 : 3;
    if (!dumpPath.empty() && !png.empty()) return dump(dumpPath, png, maps);
    if (g_root.empty() || out.empty()) {
        std::fprintf(stderr,
                     "usage: psp_light_conv --extracted <dir> --out <light.pak> [--map <id>|all]...\n"
                     "       psp_light_conv --verify <light.pak> [--extracted <dir>]\n"
                     "       psp_light_conv --dump <light.pak> --png <strip.png> [--map <id>]...\n"
                     "       psp_light_conv --selftest <scratch dir>\n");
        return 2;
    }
    if (maps.empty() && !allMaps) maps = {0u, 1u};  // maps 0 + 1
    if (allMaps) maps.clear();
    return bake(out, maps);
}
