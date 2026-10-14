// The world screen half of the world's particle emitters
// (the engine and the emitters: src/rendering/psp/psp_particles.cpp).
//
// Upstream M2Renderer::update runs the particles of every M2 instance with emitters within its render distance
// (doodads, WMO doodads, game objects alike) and renderM2Particles / renderM2Ribbons draw them. The PSP keeps
// that per model, from the doodads pak's and gameobjects.pak's "pidx" entry (psp_model_conv), for the nearest
// `particleinst` emitter models within `particledist` yd (+ their reach) - see WorldParticles.
//
// wowee.cfg (read at the first world update): particles (1; 0 = off, nothing allocated), particlesprites (256),
// particleinst (24), particledist (50), particlelod (20), particleemit (32), particlemodel (64), particlekb (192),
// particlepx (32), particledensity (100 = upstream), particleribbons (16 lanes), ribbons (1); gameobjects=0
// also drops the objects' particles. The weather: ui_world_weather.cpp.
#include "ui/psp/ui_world_particles.hpp"

#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <malloc.h>  // mallinfo
#include <new>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {

constexpr float kObjMoveYd = 16.0f;   // yd walked before the objects are looked at again
constexpr uint32_t kObjEveryMs = 500;

// The same map list as WorldScreen::loadTerrain (mapDirName).
const char* fxMapDir(uint32_t mapId) {
    switch (mapId) {
        case 0: return "azeroth";
        case 1: return "kalimdor";
        case 530: return "expansion01";
        case 571: return "northrend";
        default: break;
    }
    // any other map by Map.dbc (worldMapDir, as loadTerrain) - a dungeon's doodads pak has a "pidx"
    // now (its torches' emitters; was: no particles in a dungeon at all).
    static std::string other;
    other = worldMapDir(mapId);
    return other.empty() ? nullptr : other.c_str();
}

}  // namespace

void WorldScreen::destroyFxWorldUi(FxWorldUi* p) { delete p; }

void WorldScreen::updateParticles(float dtSec) {
    if (fxWorldState_ < 0) return;
    if (fxWorldState_ == 0) {
        const bool particles = pp::configGetInt("particles", 1) != 0;
        const bool weather = pp::configGetInt("weather", 1) != 0;
        if (!particles) PSP_LOGI(pp::tag::Gfx, "particles off (wowee.cfg particles=0)");
        if (!weather) PSP_LOGI(pp::tag::Gfx, "weather off (wowee.cfg weather=0)");
        if (!particles && !weather) {
            fxWorldState_ = -1;
            return;
        }
        auto* ui = new (std::nothrow) FxWorldUi;
        if (!ui) {
            fxWorldState_ = -1;
            return;
        }
        fxWorld_ = std::unique_ptr<FxWorldUi, void (*)(FxWorldUi*)>(ui, &WorldScreen::destroyFxWorldUi);
        fxWorldState_ = 1;
        ui->particlesOn = particles;
        ui->objectsOn = pp::configGetInt("gameobjects", 1) != 0;
        if (weather) {
            rp::WeatherConfig wc;
            wc.maxDrops = pp::configGetInt("weathermax", 300);
            wc.range = static_cast<float>(pp::configGetInt("weatherrange", 25));
            ui->weather.configure(wc);
            ui->weatherOn = true;
            ui->weatherMap = std::clamp(pp::configGetInt("weathermap", 1), 0, 1);
            ui->zoneFallback = std::clamp(pp::configGetInt("weatherzone", 0), 0, 1);
            PSP_LOGI(pp::tag::Gfx, "weather on max=%d range=%d map=%d zone=%d kb=%u", wc.maxDrops,
                     static_cast<int>(wc.range), ui->weatherMap, ui->zoneFallback,
                     static_cast<unsigned>((ui->weather.memoryBytes() + 1023u) / 1024u));
        }
    }
    FxWorldUi& u = *fxWorld_;
    // The world's emitters of this map (opened again after a map change).
    const uint32_t mapId = session_.worldEntry().mapId;
    // not while the memory governor has them cut (particlesGovernor).
    if (u.particlesOn && mapId != u.mapId && !particlesGovCut_) {
        u.mapId = mapId;
        u.world.close();
        u.goVersion = ~0u;
        char doodads[256] = {}, objects[256] = {};
        if (const char* dir = fxMapDir(mapId)) {
            char rel[64];
            std::snprintf(rel, sizeof(rel), "data/doodads_%s.pak", dir);
            pp::fsPath(doodads, sizeof(doodads), rel);
        }
        if (u.objectsOn) pp::fsPath(objects, sizeof(objects), "data/gameobjects.pak");
        rp::WorldFxConfig cfg;
        cfg.maxInstances = pp::configGetInt("particleinst", 24);
        cfg.distance = static_cast<float>(pp::configGetInt("particledist", 50));
        cfg.lodDistance = static_cast<float>(pp::configGetInt("particlelod", 20));
        cfg.emitterCap = pp::configGetInt("particleemit", 32);
        cfg.modelCap = pp::configGetInt("particlemodel", 64);
        cfg.cacheBytes = static_cast<uint32_t>(std::clamp(pp::configGetInt("particlekb", 192), 32, 4096)) * 1024u;
        cfg.maxPx = pp::configGetInt("particlepx", 32);
        cfg.density = static_cast<float>(std::clamp(pp::configGetInt("particledensity", 100), 5, 400)) / 100.0f;
        cfg.ribbons = pp::configGetInt("ribbons", 1) != 0;
        if (!u.world.open(cfg, doodads, objects))
            PSP_LOGW(pp::tag::Gfx, "particles_none map=%u (no \"pidx\" in data/doodads_<map>.pak or data/gameobjects.pak: "
                     "rebake)", static_cast<unsigned>(mapId));
    }
    // The game objects with particles (the session's objects in sight, as updateGameObjects places them).
    if (u.particlesOn && u.objectsOn && u.world.isOpen()) {
        const uint32_t now = pp::logUptimeMs();
        const uint32_t version = session_.gameObjectsVersion();
        const float moved = std::fabs(playerX_ - u.goX) + std::fabs(playerY_ - u.goY);
        if (now >= u.goNextMs && (version != u.goVersion || moved >= kObjMoveYd)) {
            u.goNextMs = now + kObjEveryMs;
            u.goVersion = version;
            u.goX = playerX_;
            u.goY = playerY_;
            const float range = static_cast<float>(pp::configGetInt("particledist", 50)) + 60.0f;
            u.objects.clear();
            for (const auto& go : session_.gameObjects()) {
                if (go.displayId == 0) continue;
                const float dx = go.pos.x - playerX_, dy = go.pos.y - playerY_;
                if (dx * dx + dy * dy > range * range) continue;
                // upstream: the model turned by the orientation around Z, scaled (EntitySpawner / M2Renderer instance).
                const float c = std::cos(go.orientation), s = std::sin(go.orientation), k = go.scale;
                rp::WorldFxObject o;
                o.uniqueId = static_cast<uint32_t>(go.guid & 0xFFFFFFFFu) ^ static_cast<uint32_t>(go.guid >> 32);
                o.displayId = go.displayId;
                o.position[0] = go.pos.x;
                o.position[1] = go.pos.y;
                o.position[2] = go.pos.z;
                const float axes[9] = {c * k, s * k, 0.0f, -s * k, c * k, 0.0f, 0.0f, 0.0f, k};  // column-major Rz * k
                std::copy(axes, axes + 9, o.axes);
                u.objects.push_back(o);
            }
            u.world.setObjects(u.objects);
        }
    }
    if (u.weatherOn) updateWeather(dtSec);
}

// the memory governor's first step - the world's
// emitters closed (their effects / textures cache, the directories, the descriptor, the pool's share), opened again
// when it gives the step back (the next updateParticles, as after a map change). PSP only. KB freed (heap in use
// before - after; 0 when there was nothing open). Main thread, in update(): the GE is done with the last frame.
uint32_t WorldScreen::particlesGovernor(bool cut) {
    particlesGovCut_ = cut;
    if (!fxWorld_ || !fxWorld_->particlesOn) return 0;
    FxWorldUi& u = *fxWorld_;
    if (!cut) {
        u.mapId = 0xFFFFFFFFu;  // reopened by the next updateParticles
        u.goVersion = ~0u;
        PSP_LOGI(pp::tag::Mem, "particles_governor on (reopened next update)");
        return 0;
    }
    const uint32_t before = pp::heapUsedBytes();
    const bool wasOpen = u.world.isOpen();
    u.world.close();
    u.mapId = 0xFFFFFFFFu;
    const uint32_t after = pp::heapUsedBytes();
    const uint32_t freedKb = before > after ? (before - after) / 1024u : 0u;
    PSP_LOGI(pp::tag::Mem, "particles_governor off was_open=%d freed_kb=%u heap_kb=%u", wasOpen ? 1 : 0,
             static_cast<unsigned>(freedKb), static_cast<unsigned>(after / 1024u));
    return freedKb;
}

// world_flush: the world's emitters closed - updateParticles opens the new place's
// (unless the governor holds them cut). KB freed.
uint32_t WorldScreen::particlesFlush() {
    if (!fxWorld_ || !fxWorld_->particlesOn) return 0;
    FxWorldUi& u = *fxWorld_;
    const uint32_t before = pp::heapUsedBytes();
    u.world.close();
    u.mapId = 0xFFFFFFFFu;  // reopened by the next updateParticles
    u.goVersion = ~0u;
    const uint32_t after = pp::heapUsedBytes();
    return before > after ? (before - after) / 1024u : 0u;
}

void WorldScreen::drawParticles(const rendering::psp::TerrainCamera& cam) {
    if (!fxWorld_ || !fxWorld_->particlesOn) return;
    fxWorld_->world.frame(cam, lastDt_);
}

}  // namespace wowee::ui::psp
