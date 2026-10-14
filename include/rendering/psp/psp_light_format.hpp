// Lighting - "WLGT" blob in data/light.pak (entry "lights").
//
// Written by tools/psp/converter/psp_light_conv from the client's
// DBFilesClient/Light.dbc, LightParams.dbc, LightIntBand.dbc and
// LightFloatBand.dbc, read the way upstream's LightingManager reads them
// (src/rendering/lighting_manager.cpp: lightPositionToWorld, the /36 radii,
// lightBandSlot, the 0x00RRGGBB colours, half-minute time keys % 2880).
// Positions and radii are already in world (server) yards; colours stay the
// DBC's packed 0x00RRGGBB, float values stay raw (fog end in 1/36 yd, as
// upstream's sampleLightParams divides it itself).
//
// One header for the converter (PC) and the PSP. Little-endian, 4-aligned:
//   LightFileHeader
//   LightFileVolume  [volumeCount]   Light.dbc rows of the baked maps
//   LightFileParams  [paramsCount]   LightParams rows those volumes name
//   LightFileColorKey[colorKeyCount] every colour band's keys, pooled
//   LightFileFloatKey[floatKeyCount] every float band's keys, pooled
// A band is {first key, key count} into its pool (count 0 = the DBC has no
// row for that channel: upstream samples grey 0.5 / 1.0 then).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wowee::rendering::psp {

constexpr uint32_t kLightFileVersion = 1;
constexpr uint32_t kLightColorChannels = 18;  // LIGHT_INT_CHANNELS
constexpr uint32_t kLightFloatChannels = 6;   // LIGHT_FLOAT_CHANNELS
constexpr uint16_t kLightNoParams = 0xFFFF;

enum LightVolumeFlags : uint16_t {
    // Light.dbc's row at the map origin with zero radii: the original
    // client's per-map default light. Upstream never matches it (radius 0)
    // and falls back to its own constants; the PSP can use it (wowee.cfg
    // lightdefault=1).
    kLightVolumeMapDefault = 1u << 0,
};

struct LightFileHeader {
    char magic[4];            // "WLGT"
    uint32_t version;         // kLightFileVersion
    uint32_t volumeCount;
    uint32_t paramsCount;
    uint32_t colorKeyCount;
    uint32_t floatKeyCount;
    uint32_t mapCount;        // distinct map ids among the volumes (info)
    uint32_t reserved;
};
static_assert(sizeof(LightFileHeader) == 32, "WLGT header is 32 bytes");

struct LightFileVolume {
    uint32_t lightId;
    uint32_t mapId;
    float position[3];        // world yards (upstream lightPositionToWorld)
    float innerRadius;        // yards (DBC / LIGHT_COORD_UNITS_PER_YARD)
    float outerRadius;
    uint16_t params[3];       // clear, rain, underwater: index into the params table
    uint16_t flags;           // LightVolumeFlags
};
static_assert(sizeof(LightFileVolume) == 36, "WLGT volume is 36 bytes");

struct LightFileBand {
    uint32_t first;           // index into the key pool
    uint16_t count;           // 0..16
    uint16_t pad;
};
static_assert(sizeof(LightFileBand) == 8, "WLGT band is 8 bytes");

struct LightFileParams {
    uint32_t lightParamsId;
    uint32_t lightSkyboxId;   // LightParams field 2 (info: the PSP draws no sky model)
    LightFileBand color[kLightColorChannels];
    LightFileBand fl[kLightFloatChannels];
};
static_assert(sizeof(LightFileParams) == 8 + 24 * 8, "WLGT params are 200 bytes");

struct LightFileColorKey {
    uint16_t time;            // half-minutes since midnight, 0..2879
    uint16_t pad;
    uint32_t rgb;             // DBC 0x00RRGGBB
};
static_assert(sizeof(LightFileColorKey) == 8, "WLGT colour key is 8 bytes");

struct LightFileFloatKey {
    uint16_t time;
    uint16_t pad;
    float value;
};
static_assert(sizeof(LightFileFloatKey) == 8, "WLGT float key is 8 bytes");

// A parsed view into one blob (no copies; the blob must outlive it).
struct LightFileView {
    const LightFileHeader* header = nullptr;
    const LightFileVolume* volumes = nullptr;
    const LightFileParams* params = nullptr;
    const LightFileColorKey* colorKeys = nullptr;
    const LightFileFloatKey* floatKeys = nullptr;

    // Checks the magic, version, sizes and every index; false + reason on
    // a malformed blob (the PSP then keeps today's fixed look).
    bool parse(const void* data, size_t size, const char** why = nullptr) {
        auto fail = [&](const char* w) {
            if (why) *why = w;
            header = nullptr;
            return false;
        };
        if (!data || size < sizeof(LightFileHeader)) return fail("short");
        const auto* h = static_cast<const LightFileHeader*>(data);
        if (std::memcmp(h->magic, "WLGT", 4) != 0) return fail("magic");
        if (h->version != kLightFileVersion) return fail("version");
        const size_t need = sizeof(LightFileHeader) + size_t(h->volumeCount) * sizeof(LightFileVolume) +
                            size_t(h->paramsCount) * sizeof(LightFileParams) +
                            size_t(h->colorKeyCount) * sizeof(LightFileColorKey) +
                            size_t(h->floatKeyCount) * sizeof(LightFileFloatKey);
        if (h->volumeCount > 65535 || h->paramsCount >= kLightNoParams || size < need) return fail("size");
        const auto* p = static_cast<const uint8_t*>(data) + sizeof(LightFileHeader);
        volumes = reinterpret_cast<const LightFileVolume*>(p);
        p += size_t(h->volumeCount) * sizeof(LightFileVolume);
        params = reinterpret_cast<const LightFileParams*>(p);
        p += size_t(h->paramsCount) * sizeof(LightFileParams);
        colorKeys = reinterpret_cast<const LightFileColorKey*>(p);
        p += size_t(h->colorKeyCount) * sizeof(LightFileColorKey);
        floatKeys = reinterpret_cast<const LightFileFloatKey*>(p);
        for (uint32_t i = 0; i < h->volumeCount; ++i)
            for (uint16_t pi : volumes[i].params)
                if (pi != kLightNoParams && pi >= h->paramsCount) return fail("volume_params");
        for (uint32_t i = 0; i < h->paramsCount; ++i) {
            for (const LightFileBand& b : params[i].color)
                if (b.count > 16 || b.first > h->colorKeyCount || b.count > h->colorKeyCount - b.first) return fail("color_band");
            for (const LightFileBand& b : params[i].fl)
                if (b.count > 16 || b.first > h->floatKeyCount || b.count > h->floatKeyCount - b.first) return fail("float_band");
        }
        header = h;
        return true;
    }
};

} // namespace wowee::rendering::psp
