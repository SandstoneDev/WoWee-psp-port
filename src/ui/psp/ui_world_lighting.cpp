// Lighting in the world - upstream Renderer's use of LightingManager
// on the PSP.
//
// Upstream (renderer.cpp): every frame lightingManager->update(character
// position, map, zone, gameHandler->getGameTime(), raining, underwater), then
// the frame UBO takes lightDir / lightColor (diffuse) / ambientColor /
// fogColor / fogStart / fogEnd, and the sky pass takes the sky colours.
// Here the same update runs over data/light.pak (read at world entry, pak
// closed: the card's open-file limit), and its result goes to the GE
// (psp_lighting_ge): fog colour and range, a tint over the baked vertex
// light, the sky gradient, optional glow. wowee.cfg keys (read at world entry;
// the "Light" window changes them live and can save them):
//   lighting     1   0 = today's fixed look (no tint, legacy fog, flat sky)
//   sky          1   band gradient sky (0 = the flat colour)
//   fogbands     1   fog colour + range from the bands (0 = legacy 0.55..0.98 far)
//   lighttint    1   1 GE ambient over the vertex colour, 2 screen multiply, 0 off
//   glow         0   glow strength % (0 = off; reserves 18 KB of eDRAM at entry)
//   timeofday   -1   -1 = the realm clock, 0..23 = fixed hour (tests)
//   lightgain  100   % on the tint (the baked light is brighter than upstream's noon)
//   fogstrength 40   % upstream fogStrength (0.4: its default thinning)
//   lightzero    1   blend light volumes from zero (upstream: from the struct defaults)
//   lightdefault 1   outside every volume use the map's default light (original client; 0 = upstream fallback)
//   timeadvance  1   run the clock between server updates (upstream: frozen)
//   light2x      0   GU_FRAGMENT_2X: tints above 1.0 (untextured draws come out half)
//   skyfast      1   the sky lays colour + depth itself (no frame clear), 64-px
//                    column strips, grid kept while camera and light hold; 0 = as before
//   skybench     0   1..3 = 4 extra whole-screen passes per frame in the GE pass
//                    "sky.bench" (1 the skyfast sky, 2 the old frame clear, 3 a fast clear)
#include "ui/psp/ui_world_screen.hpp"

#include "core/local_time.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"  // frameSkipNextClear / frameCleared
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"

#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {

// Widget ids (range 0x6400-0x64FF).
enum : uint32_t {
    kIdLightOpen = 0x6400,
    kIdLightOn,
    kIdLightSky,
    kIdLightFog,
    kIdLightTint,
    kIdLightTime,
    kIdLightGlow,
    kIdLightGain,
    kIdLightFogK,
    kIdLightZero,
    kIdLightSave,
    kIdLightSkyFast,
    kIdLightSkyBench,
};

// whole-screen passes per frame of the sky bench (skybench 1..3).
constexpr int kSkyBenchPasses = 4;

// The converters' fixed sun (conv_common / model_bake / psp_terrain_conv:
// normalize(0.35, 0.25, 0.9)) on flat ground: 0.5 + 0.5 N.sun. The tint is
// the Light.dbc light on the same ground over this.
constexpr float kBakedFlat = 0.5f + 0.5f * 0.9022f;

// Upstream's fallback (LightingManager::update without a game time): the
// local clock through core::localTime. PSP: libc's time(), not sceRtc - a
// psprtc import next to libcglue's splits the stubs ("stubs out of order").
float clockHoursNow() {
    const std::tm t = core::localTime(std::time(nullptr));
    return static_cast<float>(t.tm_hour) + static_cast<float>(t.tm_min) / 60.0f;
}

uint32_t rgb24(const glm::vec3& c) {
    const uint32_t abgr = rp::abgrFrom(c);
    return ((abgr & 0xFF) << 16) | (abgr & 0xFF00) | ((abgr >> 16) & 0xFF);
}

}  // namespace

void WorldScreen::initLighting() {
    LightingUi& L = light_;
    L.on = pp::configGetInt("lighting", 1);
    L.sky = pp::configGetInt("sky", 1);
    L.fogBands = pp::configGetInt("fogbands", 1);
    L.tint = std::clamp(pp::configGetInt("lighttint", 1), 0, 2);
    L.glow = std::clamp(pp::configGetInt("glow", 0), 0, 100);
    L.timeOfDay = std::clamp(pp::configGetInt("timeofday", -1), -1, 23);
    L.gain = std::clamp(pp::configGetInt("lightgain", 100), 50, 200);
    L.fogStrength = std::clamp(pp::configGetInt("fogstrength", 40), 0, 200);
    L.zero = pp::configGetInt("lightzero", 1);
    // On by default (the original client's rule). Off, Northshire (no
    // Light.dbc sphere) took upstream's daytime fallback: a blue sky and fog at
    // any hour - the "day on the PSP while it is night" problem.
    L.mapDefault = pp::configGetInt("lightdefault", 1);
    L.advance = pp::configGetInt("timeadvance", 1);
    L.twoX = pp::configGetInt("light2x", 0);
    L.moon = std::clamp(pp::configGetInt("moonlight", 60), 0, 100);
    L.skyFast = pp::configGetInt("skyfast", 1) != 0 ? 1 : 0;
    L.skyBench = std::clamp(pp::configGetInt("skybench", 0), 0, 3);
    rp::setDither3d(pp::configGetInt("dither3d", 0) != 0);  // GE dither on the lit 3D passes
    PSP_LOGI(pp::tag::Gfx, "dither3d on=%d", rp::dither3d() ? 1 : 0);
    initPortrait();  // the portrait's VRAM, with the glow's, before the models
    // Glow: its buffer comes off the top of eDRAM, so before the models fill it.
    if (L.glow > 0 && !rp::glowReserve()) {
        PSP_LOGW(pp::tag::Gfx, "glow_vram_missing (glow off)");
        L.glow = 0;
    }
    char path[256];
    pp::fsPath(path, sizeof(path), "data/light.pak");
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    pp::Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(pp::tag::Gfx, "light_missing path=data/light.pak (fixed look)");
    } else {
        const pp::PakEntry* e = pak.find("lights");
        void* blob = e ? pak.load(e) : nullptr;
        const char* why = nullptr;
        if (!blob || !L.light.load(blob, e->size, &why)) {
            PSP_LOGW(pp::tag::Gfx, "light_bad why=%s", why ? why : "no_entry");
        } else {
            PSP_LOGI(pp::tag::Gfx, "light_load ok=1 volumes=%u params=%u kb=%u ms=%u",
                     static_cast<unsigned>(L.light.volumeCount()), static_cast<unsigned>(L.light.paramsCount()),
                     static_cast<unsigned>(L.light.bytes() / 1024),
                     static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000));
        }
        if (blob) pp::pakFree(blob);
        pak.close();  // MFILE rule: nothing stays open
    }
    PSP_LOGI(pp::tag::Gfx,
             "light_cfg on=%d sky=%d fog=%d tint=%d glow=%d tod=%d gain=%d fogk=%d zero=%d mapdef=%d adv=%d moon=%d",
             L.on, L.sky, L.fogBands, L.tint, L.glow, L.timeOfDay, L.gain, L.fogStrength, L.zero, L.mapDefault,
             L.advance, L.moon);
    // the sky's A/B keys.
    PSP_LOGI(pp::tag::Gfx, "sky_cfg fast=%d bench=%d bench_passes=%d", L.skyFast, L.skyBench,
             L.skyBench > 0 ? kSkyBenchPasses : 0);
}

// black until the realm time is known (at most 2 s), then 0.4 s in.
float WorldScreen::worldFade() {
    const uint32_t now = pp::logUptimeMs();
    if (!worldEnterMs_) worldEnterMs_ = now;
    if (!worldLitMs_ && now - worldEnterMs_ >= 2000u) worldLitMs_ = now;  // no time packet: go on
    if (!worldLitMs_) return 1.0f;
    const float t = static_cast<float>(now - worldLitMs_) / 400.0f;
    return t >= 1.0f ? 0.0f : 1.0f - t;
}

void WorldScreen::updateLighting(float dtSec) {
    PROF_SCOPE("light");
    LightingUi& L = light_;
    rp::FrameLight& f = rp::frameLight();
    f = rp::FrameLight{};  // today's look unless the bands take over below
    // skyfast is fixed here for the frame (the "Light" window may flip it
    // later in it): the sky drawn in draw() lays every pixel's colour and depth, so frameBegin (right
    // after this update) skips its clear. Not with the fill test (its 1x1 scissor would leave the
    // rest of the screen to the last frame) - that one keeps the old way.
    L.skyFastFrame = L.skyFast != 0 && !fillTest_;
    if (L.skyFastFrame) pp::frameSkipNextClear();
    if (!L.on || !L.light.loaded()) return;

    // The hour: fixed (tests), the realm's clock (upstream getGameTime), or
    // the PSP's own before the server has said anything (upstream: the PC's
    // local time). PSP: the realm time runs on between packets (the original
    // client's clock does; upstream holds the last value).
    const float serverHours = session_.getGameTime();
    // The realm time came (or changed): no blend from the PSP
    // clock's hour, the light is right on the next frame.
    if (session_.gameTimeReceivedMs() != L.seenTimeMs) {
        L.seenTimeMs = session_.gameTimeReceivedMs();
        L.light.snapNext();
        if (!worldLitMs_) worldLitMs_ = pp::logUptimeMs();
    }
    if (L.timeOfDay >= 0) {
        L.hours = static_cast<float>(L.timeOfDay);
    } else if (serverHours >= 0.0f) {
        float h = serverHours;
        if (L.advance) h += static_cast<float>(pp::logUptimeMs() - session_.gameTimeReceivedMs()) / 3600000.0f;
        L.hours = std::fmod(h, 24.0f);
    } else {
        L.hours = clockHoursNow();
    }

    rp::Lighting& lt = L.light;
    lt.fogStrength = static_cast<float>(L.fogStrength) / 100.0f;
    lt.zeroBlend = L.zero != 0;
    lt.useMapDefault = L.mapDefault != 0;
    // PSP: no WMO interior test, weather or swimming yet - outdoor, clear, dry.
    // the server's rain -> the rain light profile (upstream isRaining -> LightingManager).
    lt.update(glm::vec3(playerX_, playerY_, playerZ_), session_.worldEntry().mapId, zoneId_, L.hours, dtSec,
              weatherRaining());
    const rendering::LightingParams& p = lt.params();

    // Tint: upstream's `ambient + diffuse * N.L` on flat ground over what the
    // converters baked there. PSP: the vertex light stays baked (no normals
    // in the vertex formats, see docs), only its level and colour follow.
    const glm::vec3 toSun = -p.directionalDir;
    // PSP (too dark, the wolves in the forest were not seen):
    // upstream's sun is under the horizon from 18:00 to 6:00, so N.L = 0 and
    // only the ambient was left (tint 0.39 at 19:46, less deeper in the night).
    // The client lights the night by the moon: the light stays above, its
    // colour is the night diffuse of Light.dbc (dark blue) - so |z|.
    // (Night on the PC, day on the PSP:) with the map's own Light.dbc
    // (lightdefault=1) the night colours are right, but the moon at full |z| (~0.95 at 3:00)
    // outshone upstream's low noon sun (z ~0.5): tint 0.57 at night, 0.58 at noon (emulator).
    // The moon gives moonlight % of it (60: ~0.42 at night by the same numbers).
    const float nDotL = toSun.z >= 0.0f ? toSun.z : -toSun.z * static_cast<float>(L.moon) / 100.0f;
    glm::vec3 tint = (p.ambientColor + p.diffuseColor * nDotL) * (static_cast<float>(L.gain) / 100.0f / kBakedFlat);
    tint = glm::max(tint, glm::vec3(0.0f));
    L.tintLevel = (tint.r + tint.g + tint.b) / 3.0f;
    if (L.tint == rp::kTintGeAmbient) {
        f.tint = rp::kTintGeAmbient;
        f.twoX = L.twoX != 0;
        f.ambientABGR = rp::abgrFrom(f.twoX ? tint * 0.5f : tint);
    } else if (L.tint == rp::kTintScreen) {
        f.tint = rp::kTintScreen;
        f.screenABGR = rp::abgrFrom(tint);
    }

    if (L.fogBands) {
        // PSP: the bands' fog is fitted inside our far plane (450 yd; the
        // PC draws much further): the end at most 0.98 far, the start keeps
        // its share of the end. A start past the end would invert the fog.
        const float farEnd = rendering::psp::TerrainCamera{}.farZ * 0.98f;
        // Finite checks first: a NaN compare traps on the Allegrex FPU.
        float end = p.fogEnd, start = p.fogStart;
        if (!std::isfinite(end) || end <= 1.0f) end = farEnd;
        if (!std::isfinite(start)) start = end * 0.55f;
        const float share = std::clamp(start / end, 0.0f, 0.95f);
        end = std::min(end, farEnd);
        f.fogBands = true;
        f.fogEnd = end;
        f.fogStart = end * share;
        glm::vec3 fog = p.fogColor;
        // Screen tint multiplies the fogged pixels too: pre-divide.
        if (f.tint == rp::kTintScreen) fog = fog / glm::max(tint, glm::vec3(0.05f));
        f.fogABGR = rp::abgrFrom(fog);
    }

    // Log: which lights, on a change (first 60) and a sample every 10 s.
    const uint32_t now = pp::logUptimeMs();
    if ((lt.volume0() != L.lastVol0 || lt.volume1() != L.lastVol1) && ++L.zoneLogs <= 60) {
        L.lastVol0 = lt.volume0();
        L.lastVol1 = lt.volume1();
        PSP_LOGI(pp::tag::Gfx, "light_zone vol=%u/%u n=%u zone=%u map=%u", static_cast<unsigned>(lt.volume0()),
                 static_cast<unsigned>(lt.volume1()), static_cast<unsigned>(lt.inRange()),
                 static_cast<unsigned>(zoneId_), static_cast<unsigned>(session_.worldEntry().mapId));
    }
    if (now >= L.nextLogMs) {
        L.nextLogMs = now + 10000;
        PSP_LOGI(pp::tag::Gfx, "light_frame hour=%.2f vis=%.2f amb=%06X dif=%06X fog=%06X %.0f-%.0f sky=%06X tint=%.2f",
                 static_cast<double>(L.hours), static_cast<double>(lt.visualHours()),
                 static_cast<unsigned>(rgb24(p.ambientColor)), static_cast<unsigned>(rgb24(p.diffuseColor)),
                 static_cast<unsigned>(rgb24(p.fogColor)), static_cast<double>(f.fogStart),
                 static_cast<double>(f.fogEnd), static_cast<unsigned>(rgb24(p.skyTopColor)),
                 static_cast<double>(L.tintLevel));
    }
}

bool WorldScreen::lightingSkyOn() const { return light_.on && light_.sky && light_.light.loaded(); }

void WorldScreen::drawLightingSky(const rendering::psp::TerrainCamera& cam, uint32_t flatSkyABGR) {
    LightingUi& L = light_;
    const bool gradient = lightingSkyOn();
    if (!L.skyFastFrame) {
        // skyfast=0: as before - frameBegin cleared the frame, the flat sky was filled at the top
        // of draw(). Its own GE pass and CPU zone ("light" before).
        if (!gradient) return;
        PROF_GE_PASS("sky");
        PROF_SCOPE("sky");
        rp::drawSky(cam, rp::skyColorsFrom(L.light.params()));
        return;
    }
    // skyfast=1: frameBegin left the frame uncleared (unless the request
    // got lost - frameCleared()); every pixel's colour and depth come from here.
    PROF_SCOPE("sky");
    const rp::SkyColors colors = gradient ? rp::skyColorsFrom(L.light.params()) : rp::SkyColors{};
    bool ready = false;
    if (L.skyBench > 0) {
        ready = gradient && rp::skyFastPrepare(cam, colors);  // bench 1 draws these strips
        PROF_GE_PASS("sky.bench");
        rp::skyBench(L.skyBench, kSkyBenchPasses);
    }
    PROF_GE_PASS("sky");
    if (L.skyBench <= 0) ready = gradient && rp::skyFastPrepare(cam, colors);
    if (ready) {
        rp::skyFastDraw();
    } else if (gradient) {
        // A degenerate camera (straight up/down - the pitch limit keeps it away): the horizon colour.
        rp::skyClear(rp::abgrFrom(colors.horizon));
    } else {
        rp::skyClear(flatSkyABGR);  // sky=0 / lighting off: the flat sky is the clear (was clear + fill)
    }
    const uint32_t now = pp::logUptimeMs();
    if (now >= L.skyNextLogMs) {
        L.skyNextLogMs = now + 10000;
        const rp::SkyCacheStats st = rp::skyCacheStatsTake();
        PSP_LOGI(pp::tag::Gfx, "sky_frame fast=1 grad=%d cleared=%d grid_reused=%u grid_built=%u bench=%d",
                 gradient ? 1 : 0, pp::frameCleared() ? 1 : 0, static_cast<unsigned>(st.hits),
                 static_cast<unsigned>(st.misses), L.skyBench);
    }
}

void WorldScreen::drawLightingPost() {
    const bool tintPass = rp::frameLight().tint == rp::kTintScreen;
    const int glow = light_.on ? light_.glow : 0;
    if (!tintPass && glow <= 0) return;
    PROF_GE_PASS("glow");
    PROF_SCOPE("light");
    rp::drawPost(glow);
}

// --- "Light" window (Debug -> Light) ------------------------------------------

void WorldScreen::drawLightingDebugButton(UiRenderer& ui, float x, float y) {
    kit_.button(ui, winFocus_, kIdLightOpen, Rect{x, y, 54, 20}, OWN_STR(LIGHT_TITLE));
}

bool WorldScreen::lightingWindowAction(uint32_t act) {
    if (act < kIdLightOpen || act > kIdLightSkyBench) return false;  // + kIdLightSky*
    LightingUi& L = light_;
    auto logSet = [](const char* key, int v) { PSP_LOGI(pp::tag::Ctrl, "debug_set %s=%d", key, v); };
    switch (act) {
        case kIdLightOpen:
            if (windowOpen(kWinLight)) closeWindow(kWinLight);
            else openWindow(kWinLight);
            break;
        case kIdLightOn:
            L.on = !L.on;
            L.light.snapNext();
            logSet("lighting", L.on);
            break;
        case kIdLightSky:
            L.sky = !L.sky;
            logSet("sky", L.sky);
            break;
        case kIdLightFog:
            L.fogBands = !L.fogBands;
            logSet("fogbands", L.fogBands);
            break;
        case kIdLightTint:
            L.tint = (L.tint + 1) % 3;
            logSet("lighttint", L.tint);
            break;
        case kIdLightTime:
            L.timeOfDay = L.timeOfDay < 0 ? 0 : (L.timeOfDay >= 21 ? -1 : L.timeOfDay + 3);
            L.light.snapNext();
            logSet("timeofday", L.timeOfDay);
            break;
        case kIdLightGlow:
            L.glow = L.glow <= 0 ? 20 : L.glow < 40 ? L.glow + 10 : 0;
            if (L.glow > 0 && !rp::glowAvailable() && !rp::glowReserve()) {
                PSP_LOGW(pp::tag::Gfx, "glow_vram_missing (set glow=20 in wowee.cfg and re-enter the world)");
                L.glow = 0;
            }
            logSet("glow", L.glow);
            break;
        case kIdLightGain:
            L.gain = L.gain >= 150 ? 70 : L.gain + 10;
            logSet("lightgain", L.gain);
            break;
        case kIdLightFogK:
            L.fogStrength = L.fogStrength < 40 ? 40 : L.fogStrength < 70 ? 70 : L.fogStrength < 100 ? 100 : 20;
            logSet("fogstrength", L.fogStrength);
            break;
        case kIdLightZero:
            L.zero = !L.zero;
            L.light.snapNext();
            logSet("lightzero", L.zero);
            break;
        case kIdLightSkyFast:  // from the next frame (updateLighting latches it)
            L.skyFast = !L.skyFast;
            logSet("skyfast", L.skyFast);
            break;
        case kIdLightSkyBench:  // off -> sky x4 -> clear x4 -> fast clear x4 -> off
            L.skyBench = (L.skyBench + 1) % 4;
            logSet("skybench", L.skyBench);
            break;
        case kIdLightSave: {
            const struct {
                const char* key;
                int value;
            } keys[] = {{"lighting", L.on},        {"sky", L.sky},           {"fogbands", L.fogBands},
                        {"lighttint", L.tint},     {"glow", L.glow},         {"timeofday", L.timeOfDay},
                        {"lightgain", L.gain},     {"fogstrength", L.fogStrength}, {"lightzero", L.zero},
                        {"skyfast", L.skyFast}};  // skybench: a test, never saved
            for (const auto& k : keys) pp::configSet(k.key, std::to_string(k.value));
            int written = 0;
            const bool ok = pp::configSave(&written);
            PSP_LOGI(pp::tag::Gfx, "light_save ok=%d keys=%d", ok ? 1 : 0, written);
            break;
        }
        default: break;
    }
    return true;
}

bool WorldScreen::drawLightingWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinLight) return false;
    const LightingUi& L = light_;
    const Rect r{8, 6, 312, 32 + 6 * 26 + 20};  // + the sky row
    kit_.panel(ui, r);
    ui.textShadow(kit_.font(kFontNormal), r.x + r.w * 0.5f, r.y + 12, OWN_STR(LIGHT_TITLE), kColGold, Align::Center);
    const float c0 = r.x + 12, c1 = r.x + r.w * 0.5f + 2, bw = r.w * 0.5f - 14, bh = 22;
    float y = r.y + 32;
    // The port's own strings (ps_b8_port.inc LIGHT_*).
    std::string line = OWN_FMT(LIGHT_ON_FMT, !L.light.loaded() ? OWN_STR(LIGHT_NO_PAK)
                                             : L.on            ? "Light.dbc"
                                                               : OWN_STR(LIGHT_AS_BEFORE));
    kit_.button(ui, winFocus_, kIdLightOn, Rect{c0, y, bw, bh}, line.c_str(), L.light.loaded());
    line = OWN_FMT(LIGHT_SKY_FMT, L.sky ? OWN_STR(LIGHT_GRADIENT) : OWN_STR(LIGHT_COLOR));
    kit_.button(ui, winFocus_, kIdLightSky, Rect{c1, y, bw, bh}, line.c_str());
    y += 26;
    line = OWN_FMT(LIGHT_FOG_FMT, L.fogBands ? OWN_STR(LIGHT_BANDS) : OWN_STR(LIGHT_AS_BEFORE));
    kit_.button(ui, winFocus_, kIdLightFog, Rect{c0, y, bw, bh}, line.c_str());
    line = OWN_FMT(LIGHT_TINT_FMT, L.tint == 1 ? "GE" : L.tint == 2 ? OWN_STR(LIGHT_SCREEN) : OWN_STR(PORT_OFF_LC));
    kit_.button(ui, winFocus_, kIdLightTint, Rect{c1, y, bw, bh}, line.c_str());
    y += 26;
    if (L.timeOfDay < 0) line = OWN_STR(LIGHT_TIME_SERVER);
    else line = OWN_FMT(LIGHT_TIME_FMT, L.timeOfDay);
    kit_.button(ui, winFocus_, kIdLightTime, Rect{c0, y, bw, bh}, line.c_str());
    if (L.glow > 0) line = OWN_FMT(LIGHT_GLOW_FMT, L.glow);
    else line = OWN_STR(LIGHT_GLOW_OFF);
    kit_.button(ui, winFocus_, kIdLightGlow, Rect{c1, y, bw, bh}, line.c_str());
    y += 26;
    line = OWN_FMT(LIGHT_GAIN_FMT, L.gain);
    kit_.button(ui, winFocus_, kIdLightGain, Rect{c0, y, bw, bh}, line.c_str());
    line = OWN_FMT(LIGHT_FOGK_FMT, static_cast<double>(L.fogStrength) / 100.0);
    kit_.button(ui, winFocus_, kIdLightFogK, Rect{c1, y, bw, bh}, line.c_str());
    y += 26;
    line = OWN_FMT(LIGHT_BLEND_FMT, L.zero ? OWN_STR(LIGHT_FROM_ZERO) : OWN_STR(LIGHT_UPSTREAM));
    kit_.button(ui, winFocus_, kIdLightZero, Rect{c0, y, bw, bh}, line.c_str());
    kit_.button(ui, winFocus_, kIdLightSave, Rect{c1, y, bw, bh}, OWN_STR(LIGHT_SAVE));
    y += 26;
    // the sky's A/B (skyfast) and its bench (skybench).
    line = OWN_FMT(LIGHT_SKYGE_FMT, L.skyFast ? OWN_STR(LIGHT_FAST) : OWN_STR(LIGHT_AS_BEFORE));
    kit_.button(ui, winFocus_, kIdLightSkyFast, Rect{c0, y, bw, bh}, line.c_str());
    static const pp::PS kBench[4] = {pp::PS::PORT_OFF_LC, pp::PS::LIGHT_BENCH_SKY, pp::PS::LIGHT_BENCH_CLEAR,
                                     pp::PS::LIGHT_BENCH_FAST};
    line = OWN_FMT(LIGHT_BENCH_FMT, pp::ps(kBench[std::clamp(L.skyBench, 0, 3)]));
    kit_.button(ui, winFocus_, kIdLightSkyBench, Rect{c1, y, bw, bh}, line.c_str());
    y += 26;
    const rp::FrameLight& f = rp::frameLight();
    const int hh = static_cast<int>(L.hours), mm = static_cast<int>((L.hours - static_cast<float>(hh)) * 60.0f);
    line = OWN_FMT(LIGHT_STATUS_FMT, hh, mm, static_cast<unsigned>(zoneId_), static_cast<unsigned>(L.light.volume0()),
                   static_cast<unsigned>(L.light.volume1()), static_cast<double>(f.fogStart),
                   static_cast<double>(f.fogEnd), static_cast<double>(L.tintLevel));
    ui.textShadow(kit_.font(kFontSmall), c0, y + 2, line.c_str(), kColGrey);
    return true;
}

} // namespace wowee::ui::psp
