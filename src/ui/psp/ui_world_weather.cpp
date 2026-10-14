// The weather - upstream Renderer's weather sync
// (renderer.cpp, after the lighting update) over rendering::psp::WeatherFx (upstream rendering::Weather,
// src/rendering/psp/psp_particles.cpp):
//   - the server's SMSG_WEATHER is authoritative (psp_world_session.cpp reads it as upstream does);
//   - in zone 10 (a forest zone) no weather at all - its fog is the weather (upstream);
//   - no weather inside a WMO (upstream setEnabled(!insideWmo); PSP: a building above the camera - a ray up
//     against the WMO groups, ModelRenderer::raycastBuildings, 4 times a second);
//   - the server's rain picks the rain light profile (upstream isRaining -> LightingManager, here the PSP
//     Lighting through ui_world_lighting.cpp).
// PSP (wowee.cfg, read at the first world update; ui_world_particles.cpp makes the state):
//   - weathermap=1 (default): the first field of SMSG_WEATHER is what an AzerothCore / TrinityCore 3.3.5a realm
//     sends - its WeatherState (Weather.h: 0 fine, 1 fog, 3/4/5 rain, 6/7/8 snow, 22/41/42 sandstorm,
//     86 thunders, 90 black rain, 106 black snow); upstream reads it as its own 1 rain / 2 snow / 3 storm, which
//     on such a realm shows light rain as a storm and nothing else. weathermap=0 = upstream's reading (A/B);
//   - weatherzone=0 (default): when the realm says fine, fine - the original client shows only the server's
//     weather. weatherzone=1 = upstream's single-player zone table (Weather::updateZoneWeather) in that case;
//   - weather=0 off; weathermax (300 drops, upstream 2000) and weatherrange (25 yd box, upstream 100).
#include "ui/psp/ui_world_particles.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {

struct Decoded {
    rp::WeatherKind kind = rp::WeatherKind::None;
    bool dark = false;
    bool rain = false;  // the light's rain profile
};

Decoded decodeWeather(uint32_t type, int map) {
    Decoded d;
    if (map == 0) {  // upstream renderer.cpp: 1 rain, 2 snow, 3 storm (isRaining: type 1)
        if (type == 1) {
            d.kind = rp::WeatherKind::Rain;
            d.rain = true;
        } else if (type == 2) {
            d.kind = rp::WeatherKind::Snow;
        } else if (type == 3) {
            d.kind = rp::WeatherKind::Storm;
        }
        return d;
    }
    switch (type) {  // AzerothCore / TrinityCore WeatherState
        case 3:
        case 4:
        case 5:
            d.kind = rp::WeatherKind::Rain;
            d.rain = true;
            break;
        case 6:
        case 7:
        case 8:
            d.kind = rp::WeatherKind::Snow;
            break;
        case 22:  // WEATHER_TYPE_STORM is a sandstorm: upstream's storm motion, sand coloured
        case 41:
        case 42:
            d.kind = rp::WeatherKind::Sand;
            break;
        case 86:  // thunders: upstream's (rain) storm
            d.kind = rp::WeatherKind::Storm;
            d.rain = true;
            break;
        case 90:  // black rain
            d.kind = rp::WeatherKind::Rain;
            d.dark = true;
            d.rain = true;
            break;
        case 106:  // black snow
            d.kind = rp::WeatherKind::Snow;
            d.dark = true;
            break;
        default:  // 0 fine, 1 fog (no particles)
            break;
    }
    return d;
}

const char* kindName(rp::WeatherKind k) {
    switch (k) {
        case rp::WeatherKind::Rain: return "rain";
        case rp::WeatherKind::Snow: return "snow";
        case rp::WeatherKind::Storm: return "storm";
        case rp::WeatherKind::Sand: return "sand";
        default: return "none";
    }
}

// upstream Weather::initializeZoneWeatherDefaults (zone, type, min, max, probability).
struct ZoneRow {
    uint32_t zone;
    rp::WeatherKind kind;
    float minI, maxI, prob;
};
constexpr rp::WeatherKind kWr = rp::WeatherKind::Rain, kWs = rp::WeatherKind::Snow;
constexpr ZoneRow kZoneWeather[] = {
    {11, kWr, 0.1f, 0.4f, 0.15f},   {8, kWr, 0.1f, 0.5f, 0.2f},     {33, kWr, 0.2f, 0.7f, 0.25f},  {44, kWr, 0.1f, 0.3f, 0.1f},
    {36, kWr, 0.1f, 0.4f, 0.15f},   {45, kWr, 0.1f, 0.3f, 0.1f},    {267, kWr, 0.2f, 0.5f, 0.2f},  {28, kWr, 0.1f, 0.3f, 0.1f},
    {139, kWr, 0.1f, 0.3f, 0.1f},   {1, kWs, 0.2f, 0.6f, 0.3f},     {51, kWs, 0.1f, 0.5f, 0.2f},   {41, kWs, 0.1f, 0.4f, 0.15f},
    {2817, kWs, 0.3f, 0.7f, 0.4f},  {67, kWs, 0.2f, 0.6f, 0.35f},   {65, kWs, 0.2f, 0.5f, 0.3f},   {394, kWs, 0.1f, 0.4f, 0.2f},
    {495, kWs, 0.3f, 0.8f, 0.5f},   {210, kWs, 0.2f, 0.5f, 0.25f},  {3537, kWs, 0.2f, 0.6f, 0.3f}, {4742, kWs, 0.2f, 0.5f, 0.3f},
    {15, kWr, 0.1f, 0.4f, 0.15f},   {16, kWr, 0.1f, 0.3f, 0.1f},    {148, kWr, 0.1f, 0.4f, 0.15f}, {331, kWr, 0.1f, 0.3f, 0.1f},
    {405, kWr, 0.1f, 0.3f, 0.1f},   {490, kWr, 0.1f, 0.4f, 0.15f},  {493, kWr, 0.1f, 0.3f, 0.1f},  {618, kWs, 0.2f, 0.6f, 0.3f},
    {3483, kWr, 0.1f, 0.3f, 0.1f},  {3521, kWr, 0.1f, 0.4f, 0.15f}, {3519, kWr, 0.1f, 0.3f, 0.1f},
};

float rand01(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
}

}  // namespace

void WorldScreen::updateWeather(float dtSec) {
    FxWorldUi& u = *fxWorld_;
    const float dt = std::clamp(dtSec, 0.0f, 0.25f);
    const uint32_t type = session_.getWeatherType();
    const float serverIntensity = session_.getWeatherIntensity();
    const Decoded d = decodeWeather(type, u.weatherMap);
    rp::WeatherKind kind = d.kind;
    float intensity = serverIntensity;
    bool dark = d.dark;
    const char* source = "server";
    // upstream Weather::updateZoneWeather: a roll per zone (and per 3-8 min cycle), eased in and out (~7 s).
    auto zoneWeather = [&u](uint32_t zoneId, float step) {
        const ZoneRow* row = nullptr;
        for (const ZoneRow& z : kZoneWeather)
            if (z.zone == zoneId) row = &z;
        auto roll = [&](bool newZone) {
            if (!row) {
                u.zoneTarget = 0.0f;
                return;
            }
            if (rand01(u.rng) < row->prob) {
                u.zoneKind = row->kind;
                u.zoneTarget = row->minI + (row->maxI - row->minI) * rand01(u.rng);
                u.zoneCycle = 180.0f + rand01(u.rng) * 300.0f;
            } else {
                u.zoneTarget = 0.0f;
                u.zoneCycle = newZone ? 120.0f + rand01(u.rng) * 180.0f : 180.0f + rand01(u.rng) * 300.0f;
            }
        };
        if (zoneId != u.zoneCurrent) {
            u.zoneCurrent = zoneId;
            u.zoneTimer = 0.0f;
            roll(true);
        }
        const float ease = 0.15f * step;
        if (u.zoneIntensity < u.zoneTarget) u.zoneIntensity = std::min(u.zoneIntensity + ease, u.zoneTarget);
        else if (u.zoneIntensity > u.zoneTarget) u.zoneIntensity = std::max(u.zoneIntensity - ease, u.zoneTarget);
        if (u.zoneIntensity <= 0.01f && u.zoneTarget <= 0.01f) u.zoneKind = rp::WeatherKind::None;
        u.zoneTimer += step;
        if (u.zoneCycle > 0.0f && u.zoneTimer >= u.zoneCycle) {
            u.zoneTimer = 0.0f;
            if (row) roll(false);
        }
    };
    if (zoneId_ == 10) {  // upstream: this zone's fog is its weather - no rain streaks
        kind = rp::WeatherKind::None;
        intensity = 0.0f;
        source = "duskwood";
    } else if (type == 0 && u.zoneFallback) {  // upstream: no server weather -> the zone table
        zoneWeather(zoneId_, dt);
        kind = u.zoneKind;
        intensity = u.zoneIntensity;
        dark = false;
        source = "zone";
    }
    // upstream setEnabled(!insideWmo). PSP: a building above the camera (last frame's), 4 times a second.
    const uint32_t now = pp::logUptimeMs();
    if (now >= u.indoorCheckMs) {
        u.indoorCheckMs = now + 250u;
        constexpr float kUp = 40.0f;
        u.indoors = models_.raycastBuildings(camPosForHud_, glm::vec3(0.0f, 0.0f, 1.0f), kUp) < kUp;
    }
    u.weather.set(kind, intensity, dark);
    u.weather.setEnabled(!u.indoors);
    if (kind != u.kind || std::fabs(intensity - u.intensity) > 0.05f || session_.weatherVersion() != u.seenVersion) {
        if (++u.stateLogs <= 40)
            PSP_LOGI(pp::tag::Gfx, "weather_state type=%u kind=%s intensity=%.2f dark=%d source=%s zone=%u indoors=%d",
                     static_cast<unsigned>(type), kindName(kind), static_cast<double>(intensity), dark ? 1 : 0, source,
                     static_cast<unsigned>(zoneId_), u.indoors ? 1 : 0);
        u.seenVersion = session_.weatherVersion();
        u.kind = kind;
        u.intensity = intensity;
        u.dark = dark;
    }
}

void WorldScreen::drawWeather(const rendering::psp::TerrainCamera& cam) {
    if (!fxWorld_ || !fxWorld_->weatherOn) return;
    FxWorldUi& u = *fxWorld_;
    uint32_t us = 0;
    u.weather.frame(cam, lastDt_, us);
    u.usLast = us;
    u.usMax = std::max(u.usMax, us);
    const uint32_t now = pp::logUptimeMs();
    if (now >= u.nextLogMs && u.weather.kind() != rp::WeatherKind::None) {
        u.nextLogMs = now + 10000u;
        PSP_LOGI(pp::tag::Gfx, "weather_frame kind=%s intensity=%.2f drops=%d us=%u us_max=%u indoors=%d",
                 kindName(u.weather.kind()), static_cast<double>(u.weather.intensity()), u.weather.drops(),
                 static_cast<unsigned>(us), static_cast<unsigned>(u.usMax), u.indoors ? 1 : 0);
        u.usMax = 0;
    }
}

// upstream GameHandler::isRaining (the server's rain, intensity > 0.05) -> the light's rain profile.
bool WorldScreen::weatherRaining() const {
    const int map = fxWorld_ ? fxWorld_->weatherMap : 1;
    return decodeWeather(session_.getWeatherType(), map).rain && session_.getWeatherIntensity() > 0.05f;
}

}  // namespace wowee::ui::psp
