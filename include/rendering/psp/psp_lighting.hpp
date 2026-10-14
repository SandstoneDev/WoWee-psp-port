// Lighting - upstream LightingManager's logic over data/light.pak.
//
// Upstream (src/rendering/lighting_manager.cpp) reads Light.dbc and its
// bands through the AssetManager and blends, per frame: the light volumes
// around the player (inner radius full weight, smoothstep to the outer,
// the top MAX_BLEND_VOLUMES = 2 normalised, ties by lightVolumeOrderedBefore),
// the LightParams of each (clear / rain / underwater), 18 colour + 6 float
// bands sampled at the visual hour (half-minute keys, midnight wrap), zone
// ambience overrides (zone_ambience.cpp, compiled here unchanged), fog pulled
// toward the sky's horizon colour (fogSkyBlend 0.7), fog distances scaled by
// 1/fogStrength (0.4), ambient+diffuse headroom, then an exponential blend
// toward the new values (rate 5/s).
//
// This class is that code with its inputs swapped: the bands come baked
// from the PC (psp_light_conv, psp_light_format.hpp) instead of the DBCs,
// and the frame time is passed in (upstream reads steady_clock). Pure logic
// (glm only, no GE): the converter compiles it too, for --verify against
// upstream's own LightingManager and --dump.
//
// PSP: no sky models (LightSkybox) - the skybox layers are not tracked;
// the sky is a gradient of the band colours (psp_lighting_ge).
#pragma once

#include "rendering/lighting_manager.hpp"  // upstream LightingParams (+ zone ambience)
#include "rendering/psp/psp_light_format.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace wowee::rendering::psp {

class Lighting {
public:
    // Copies the blob ("lights" entry of data/light.pak) and checks it.
    bool load(const void* blob, size_t size, const char** why = nullptr);
    void clear();
    bool loaded() const { return view_.header != nullptr; }
    uint32_t volumeCount() const { return loaded() ? view_.header->volumeCount : 0; }
    uint32_t paramsCount() const { return loaded() ? view_.header->paramsCount : 0; }
    uint32_t bytes() const { return static_cast<uint32_t>(blob_.size() * sizeof(uint32_t)); }
    const LightFileView& file() const { return view_; }

    // Upstream LightingManager::update. gameTimeHours < 0: the caller had no
    // clock (upstream then reads the local time; the PSP always passes one).
    void update(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId, float gameTimeHours, float dtSec,
                bool isRaining = false, bool isUnderwater = false);

    // The unsmoothed values update() blends toward (upstream's newParams),
    // for the converter's checks and dumps. `vol0`/`vol1`: the light ids
    // blended (0 = none); `inRange`: volumes with a weight.
    LightingParams sample(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId, float hours,
                          bool isRaining = false, bool isUnderwater = false, uint32_t* vol0 = nullptr,
                          uint32_t* vol1 = nullptr, uint32_t* inRange = nullptr) const;
    // One profile at one time (upstream sampleLightParams), by params index.
    LightingParams sampleProfile(uint32_t paramsIndex, uint16_t timeHalfMinutes) const;

    const LightingParams& params() const { return current_; }
    float timeOfDay() const { return timeOfDay_; }                 // 0..1
    float visualHours() const { return visualHours_; }             // upstream getVisualTimeOfDayHours
    uint32_t volume0() const { return vol0_; }
    uint32_t volume1() const { return vol1_; }
    uint32_t inRange() const { return inRange_; }
    // Snap to the target on the next update (world entry, teleport), instead
    // of upstream's one-second fade from the fallback colours.
    void snapNext() { snap_ = true; }

    // Upstream knobs (LightingManager setters), same defaults.
    float fogSkyBlend = 0.7f;
    float fogStrength = 0.4f;
    bool indoors = false;
    // PSP (wowee.cfg lightdefault, off = upstream): outside every volume use
    // the map's default light (origin, zero radii) instead of upstream's
    // hand-written fallback - the original client's rule.
    bool useMapDefault = false;
    // PSP (wowee.cfg lightzero, false = upstream exactly): upstream's volume
    // blend starts from `newParams = {}`, i.e. LightingParams' default
    // member values, not zero (its comment says "Zero-initialize"), which
    // pales every sky colour toward (0.5, 0.7, 1.0) and adds 100..1000 yd of
    // fog. The PC hides the sky under the sky model; the PSP's sky is these
    // colours. true = start from zero, as the comment means.
    bool zeroBlend = false;

private:
    struct Weighted {
        const LightFileVolume* volume = nullptr;
        float weight = 0.0f;
    };
    void findVolumes(const glm::vec3& pos, uint32_t mapId, std::vector<Weighted>& out) const;
    const LightFileVolume* mapDefault(uint32_t mapId) const;
    uint32_t selectParams(const LightFileVolume* v, bool isRaining, bool isUnderwater) const;
    glm::vec3 sampleColor(const LightFileBand& band, uint16_t t) const;
    float sampleFloat(const LightFileBand& band, uint16_t t) const;
    LightingParams target(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId, float timeOfDay,
                          float visualHours, bool isRaining, bool isUnderwater, uint32_t* vol0, uint32_t* vol1,
                          uint32_t* inRange) const;

    std::vector<uint32_t> blob_;  // 4-aligned copy
    LightFileView view_;
    LightingParams fallback_;
    LightingParams current_;
    float timeOfDay_ = 0.5f;
    float visualHours_ = 12.0f;
    uint32_t vol0_ = 0, vol1_ = 0, inRange_ = 0;
    bool snap_ = true;
    mutable std::vector<Weighted> scratch_;
};

} // namespace wowee::rendering::psp
