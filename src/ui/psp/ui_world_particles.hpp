// The world screen's state for the world's particle emitters and the weather,
// shared by ui_world_particles.cpp and ui_world_weather.cpp. Made at the first
// world update when wowee.cfg particles or weather is on, freed with the world screen.
#pragma once

#include "rendering/psp/psp_particles.hpp"
#include "ui/psp/ui_world_screen.hpp"

#include <cstdint>
#include <vector>

namespace wowee::ui::psp {

struct WorldScreen::FxWorldUi {
    rendering::psp::WorldParticles world;
    rendering::psp::WeatherFx weather;
    bool particlesOn = false, weatherOn = false;
    uint32_t mapId = 0xFFFFFFFFu;  // the map whose doodads pak is open (reopened on a map change)
    // Game objects (WorldScreen::updateGameObjects' rules): rebuilt on a session change or 16 yd moved, 2 / s at most.
    bool objectsOn = true;
    uint32_t goVersion = ~0u, goNextMs = 0;
    float goX = 0.0f, goY = 0.0f;
    std::vector<rendering::psp::WorldFxObject> objects;
    // Weather (ui_world_weather.cpp).
    int weatherMap = 1;           // wowee.cfg weathermap: 1 the realm's WeatherState, 0 upstream's 1/2/3
    int zoneFallback = 0;         // wowee.cfg weatherzone: 1 upstream's zone table when the realm says fine
    uint32_t seenVersion = ~0u;
    rendering::psp::WeatherKind kind = rendering::psp::WeatherKind::None;
    float intensity = 0.0f;
    bool dark = false, indoors = false, duskwood = false;
    uint32_t indoorCheckMs = 0, nextLogMs = 0, stateLogs = 0, usLast = 0, usMax = 0;
    // upstream Weather::updateZoneWeather (weatherzone=1)
    uint32_t zoneCurrent = 0;
    float zoneTimer = 0.0f, zoneCycle = 0.0f, zoneTarget = 0.0f, zoneIntensity = 0.0f;
    rendering::psp::WeatherKind zoneKind = rendering::psp::WeatherKind::None;
    uint32_t rng = 0x6C8E9CF5u;
};

}  // namespace wowee::ui::psp
