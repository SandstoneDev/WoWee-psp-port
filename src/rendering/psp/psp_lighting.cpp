// Lighting - upstream LightingManager::update over the baked bands.
// Line for line with src/rendering/lighting_manager.cpp; every departure is
// marked PSP:. Pure logic: built for the PSP and by the converter (--verify).
#include "rendering/psp/psp_lighting.hpp"

#include "rendering/light_coords.hpp"
#include "rendering/light_headroom.hpp"
#include "rendering/light_volume_order.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::rendering::psp {

namespace {
constexpr uint16_t kHalfMinutesPerDay = 2880;  // upstream kHalfMinutesPerDay
constexpr size_t MAX_BLEND_VOLUMES = 2;         // upstream MAX_BLEND_VOLUMES

// Upstream dbcColorToVec3: red is the high byte (0x00RRGGBB).
glm::vec3 dbcColorToVec3(uint32_t c) {
    return glm::vec3(static_cast<float>((c >> 16) & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                     static_cast<float>(c & 0xFF) / 255.0f);
}

// Upstream channel indices (LightParamsProfile::ColorChannel / FloatChannel).
enum : uint32_t {
    kDiffuse = 0, kAmbient = 1, kSkyTop = 2, kSkyMiddle = 3, kSkyBand1 = 4, kSkyBand2 = 5, kFog = 7,
};
enum : uint32_t { kFogEnd = 0, kFogStartScalar = 1, kCloudDensity = 3, kFogDensity = 3 };
}  // namespace

bool Lighting::load(const void* blob, size_t size, const char** why) {
    clear();
    blob_.resize((size + 3) / 4);
    if (size) std::memcpy(blob_.data(), blob, size);
    if (!view_.parse(blob_.data(), size, why)) {
        blob_.clear();
        blob_.shrink_to_fit();
        return false;
    }
    // Upstream constructor: the fallback (a starting zone's daytime).
    fallback_.ambientColor = glm::vec3(0.5f, 0.5f, 0.6f);
    fallback_.diffuseColor = glm::vec3(1.0f, 0.95f, 0.85f);
    fallback_.directionalDir = glm::normalize(glm::vec3(0.3f, -0.7f, 0.6f));
    fallback_.fogColor = glm::vec3(0.6f, 0.7f, 0.85f);
    fallback_.fogStart = 300.0f;
    fallback_.fogEnd = 1500.0f;
    fallback_.skyTopColor = glm::vec3(0.4f, 0.6f, 0.9f);
    fallback_.skyMiddleColor = glm::vec3(0.6f, 0.75f, 0.95f);
    current_ = fallback_;
    snap_ = true;
    return true;
}

void Lighting::clear() {
    blob_.clear();
    blob_.shrink_to_fit();
    view_ = LightFileView{};
    current_ = LightingParams{};
    vol0_ = vol1_ = inRange_ = 0;
}

uint32_t Lighting::selectParams(const LightFileVolume* v, bool isRaining, bool isUnderwater) const {
    // Upstream selectLightParamsId (a missing variant falls back to clear).
    if (!v) return kLightNoParams;
    if (isUnderwater && v->params[2] != kLightNoParams) return v->params[2];
    if (isRaining && v->params[1] != kLightNoParams) return v->params[1];
    return v->params[0];
}

glm::vec3 Lighting::sampleColor(const LightFileBand& band, uint16_t t) const {
    // Upstream sampleColorBand.
    if (band.count == 0) return glm::vec3(0.5f);
    const LightFileColorKey* k = view_.colorKeys + band.first;
    if (band.count == 1) return dbcColorToVec3(k[0].rgb);
    uint32_t idx1 = band.count - 1u, idx2 = 0;
    for (uint32_t i = 0; i < band.count; ++i) {
        if (t < k[i].time) {
            idx2 = i;
            idx1 = i > 0 ? i - 1 : band.count - 1u;
            break;
        }
    }
    const uint16_t t1 = k[idx1].time, t2 = k[idx2].time;
    const uint16_t span = (t2 > t1) ? (t2 - t1) : (kHalfMinutesPerDay - t1 + t2);
    const uint16_t elapsed = (t >= t1) ? (t - t1) : (kHalfMinutesPerDay - t1 + t);
    float f = span > 0 ? static_cast<float>(elapsed) / static_cast<float>(span) : 0.0f;
    f = glm::clamp(f, 0.0f, 1.0f);
    return glm::mix(dbcColorToVec3(k[idx1].rgb), dbcColorToVec3(k[idx2].rgb), f);
}

float Lighting::sampleFloat(const LightFileBand& band, uint16_t t) const {
    // Upstream sampleFloatBand.
    if (band.count == 0) return 1.0f;
    const LightFileFloatKey* k = view_.floatKeys + band.first;
    if (band.count == 1) return k[0].value;
    uint32_t idx1 = band.count - 1u, idx2 = 0;
    for (uint32_t i = 0; i < band.count; ++i) {
        if (t < k[i].time) {
            idx2 = i;
            idx1 = i > 0 ? i - 1 : band.count - 1u;
            break;
        }
    }
    const uint16_t t1 = k[idx1].time, t2 = k[idx2].time;
    const uint16_t span = (t2 > t1) ? (t2 - t1) : (kHalfMinutesPerDay - t1 + t2);
    const uint16_t elapsed = (t >= t1) ? (t - t1) : (kHalfMinutesPerDay - t1 + t);
    float f = span > 0 ? static_cast<float>(elapsed) / static_cast<float>(span) : 0.0f;
    f = glm::clamp(f, 0.0f, 1.0f);
    return glm::mix(k[idx1].value, k[idx2].value, f);
}

LightingParams Lighting::sampleProfile(uint32_t paramsIndex, uint16_t t) const {
    // Upstream sampleLightParams.
    if (!loaded() || paramsIndex >= view_.header->paramsCount) return fallback_;
    const LightFileParams& p = view_.params[paramsIndex];
    LightingParams out;
    out.ambientColor = sampleColor(p.color[kAmbient], t);
    out.diffuseColor = sampleColor(p.color[kDiffuse], t);
    out.fogColor = sampleColor(p.color[kFog], t);
    out.skyTopColor = sampleColor(p.color[kSkyTop], t);
    out.skyMiddleColor = sampleColor(p.color[kSkyMiddle], t);
    out.skyBand1Color = sampleColor(p.color[kSkyBand1], t);
    out.skyBand2Color = sampleColor(p.color[kSkyBand2], t);
    out.fogEnd = sampleFloat(p.fl[kFogEnd], t) / LIGHT_COORD_UNITS_PER_YARD;
    out.fogStart = out.fogEnd * sampleFloat(p.fl[kFogStartScalar], t);
    out.fogDensity = sampleFloat(p.fl[kFogDensity], t);
    out.cloudDensity = sampleFloat(p.fl[kCloudDensity], t);
    // Sun direction from the time (upstream: sin/cos of the day angle).
    const float angle = (static_cast<float>(t) / static_cast<float>(kHalfMinutesPerDay)) * glm::two_pi<float>();
    out.directionalDir = glm::normalize(
        glm::vec3(std::sin(angle) * 0.6f, -0.6f + std::cos(angle) * 0.4f, std::cos(angle) * 0.6f));
    return out;
}

const LightFileVolume* Lighting::mapDefault(uint32_t mapId) const {
    for (uint32_t i = 0; i < view_.header->volumeCount; ++i) {
        const LightFileVolume& v = view_.volumes[i];
        if (v.mapId == mapId && (v.flags & kLightVolumeMapDefault)) return &v;
    }
    return nullptr;
}

void Lighting::findVolumes(const glm::vec3& pos, uint32_t mapId, std::vector<Weighted>& out) const {
    // Upstream findLightVolumes: inner radius = 1, smoothstep fade to the
    // outer, sorted by lightVolumeOrderedBefore, the first two normalised.
    out.clear();
    if (!loaded()) return;
    for (uint32_t i = 0; i < view_.header->volumeCount; ++i) {
        const LightFileVolume& v = view_.volumes[i];
        if (v.mapId != mapId) continue;
        const glm::vec3 d = pos - glm::vec3(v.position[0], v.position[1], v.position[2]);
        const float distSq = glm::dot(d, d);
        float weight = 0.0f;
        if (distSq <= v.innerRadius * v.innerRadius) {
            weight = 1.0f;
        } else if (distSq < v.outerRadius * v.outerRadius) {
            const float dist = std::sqrt(distSq);
            float t = (dist - v.innerRadius) / (v.outerRadius - v.innerRadius);
            t = glm::clamp(t, 0.0f, 1.0f);
            weight = 1.0f - (t * t * (3.0f - 2.0f * t));
        }
        if (weight > 0.0f) out.push_back({&v, weight});
    }
    std::sort(out.begin(), out.end(), [](const Weighted& a, const Weighted& b) {
        return lightVolumeOrderedBefore(a.weight, a.volume->outerRadius, a.volume->lightId, b.weight,
                                        b.volume->outerRadius, b.volume->lightId);
    });
    const size_t blended = std::min(out.size(), MAX_BLEND_VOLUMES);
    float total = 0.0f;
    for (size_t i = 0; i < blended; ++i) total += out[i].weight;
    if (total > 0.0f)
        for (size_t i = 0; i < blended; ++i) out[i].weight /= total;
}

LightingParams Lighting::target(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId, float timeOfDay,
                                float visualHours, bool isRaining, bool isUnderwater, uint32_t* vol0,
                                uint32_t* vol1, uint32_t* inRange) const {
    const float visualDayFraction = visualHours / 24.0f;
    const uint16_t timeHalfMinutes =
        static_cast<uint16_t>(visualDayFraction * static_cast<float>(kHalfMinutesPerDay)) % kHalfMinutesPerDay;

    findVolumes(playerPos, mapId, scratch_);
    // PSP (lightdefault=1): outside every volume, the map's default light
    // at full weight - the original client's rule; upstream has none.
    if (scratch_.empty() && useMapDefault) {
        if (const LightFileVolume* d = mapDefault(mapId)) scratch_.push_back({d, 1.0f});
    }
    if (vol0) *vol0 = scratch_.empty() ? 0 : scratch_[0].volume->lightId;
    if (vol1) *vol1 = scratch_.size() > 1 ? scratch_[1].volume->lightId : 0;
    if (inRange) *inRange = static_cast<uint32_t>(scratch_.size());

    LightingParams newParams;
    if (indoors) {
        newParams.ambientColor = glm::vec3(0.6f, 0.6f, 0.65f);
        newParams.diffuseColor = glm::vec3(0.3f, 0.3f, 0.35f);
        newParams.fogColor = glm::vec3(0.3f, 0.3f, 0.4f);
        newParams.fogStart = 50.0f;
        newParams.fogEnd = 300.0f;
    } else if (!scratch_.empty()) {
        // Upstream: `newParams = {}; // Zero-initialize` - which, LightingParams
        // having default member initialisers, starts the sums from those
        // defaults (ambient 0.4/0.4/0.5, sky top 0.5/0.7/1.0, fog 100..1000 yd)
        // rather than from zero. Kept as it is (the port follows upstream);
        // PSP: Lighting::zeroBlend (wowee.cfg lightzero=1) starts from zero,
        // as the comment meant.
        newParams = {};
        if (zeroBlend) {
            newParams.ambientColor = newParams.diffuseColor = newParams.fogColor = glm::vec3(0.0f);
            newParams.skyTopColor = newParams.skyMiddleColor = glm::vec3(0.0f);
            newParams.skyBand1Color = newParams.skyBand2Color = glm::vec3(0.0f);
            newParams.fogStart = newParams.fogEnd = newParams.fogDensity = 0.0f;
            newParams.cloudDensity = newParams.horizonGlow = 0.0f;
        }
        glm::vec3 blendedDir(0.0f);
        const size_t blendCount = std::min(scratch_.size(), MAX_BLEND_VOLUMES);
        for (size_t i = 0; i < blendCount; ++i) {
            const Weighted& wv = scratch_[i];
            const uint32_t pi = selectParams(wv.volume, isRaining, isUnderwater);
            if (pi == kLightNoParams) continue;  // upstream: profile not found
            const LightingParams p = sampleProfile(pi, timeHalfMinutes);
            newParams.ambientColor += p.ambientColor * wv.weight;
            newParams.diffuseColor += p.diffuseColor * wv.weight;
            newParams.fogColor += p.fogColor * wv.weight;
            newParams.skyTopColor += p.skyTopColor * wv.weight;
            newParams.skyMiddleColor += p.skyMiddleColor * wv.weight;
            newParams.skyBand1Color += p.skyBand1Color * wv.weight;
            newParams.skyBand2Color += p.skyBand2Color * wv.weight;
            newParams.fogStart += p.fogStart * wv.weight;
            newParams.fogEnd += p.fogEnd * wv.weight;
            newParams.fogDensity += p.fogDensity * wv.weight;
            newParams.cloudDensity += p.cloudDensity * wv.weight;
            newParams.horizonGlow += p.horizonGlow * wv.weight;
            blendedDir += p.directionalDir * wv.weight;
        }
        const float lenSq = glm::dot(blendedDir, blendedDir);
        if (lenSq > 1e-6f) {
            newParams.directionalDir = blendedDir * glm::inversesqrt(lenSq);
        } else {
            newParams.directionalDir = glm::vec3(0.3f, -0.7f, 0.6f);
        }
    } else {
        // No light volume: the fallback, animated by the time of day.
        newParams = fallback_;
        const float angle = timeOfDay * glm::two_pi<float>();
        newParams.directionalDir = glm::normalize(
            glm::vec3(std::sin(angle) * 0.6f, -0.6f + std::cos(angle) * 0.4f, std::cos(angle) * 0.6f));
        if (timeOfDay < 0.25f || timeOfDay > 0.75f) {
            const float nightness = (timeOfDay < 0.25f) ? (0.25f - timeOfDay) * 4.0f : (timeOfDay - 0.75f) * 4.0f;
            newParams.ambientColor *= (0.3f + 0.7f * (1.0f - nightness));
            newParams.diffuseColor *= (0.2f + 0.8f * (1.0f - nightness));
            newParams.ambientColor.b += nightness * 0.1f;
        }
    }

    if (!indoors) applyZoneAmbienceOverride(zoneId, newParams);

    // Fog toward the sky's horizon colour (upstream fogSkyBlend_).
    newParams.fogColor = glm::mix(newParams.fogColor, newParams.skyMiddleColor, glm::clamp(fogSkyBlend, 0.0f, 1.0f));

    // Fog distances by 1/strength; 0 = none (upstream fogStrength_).
    const float strength = glm::clamp(fogStrength, 0.0f, 2.0f);
    if (strength <= 0.001f) {
        newParams.fogStart = 1.0e6f;
        newParams.fogEnd = 1.0e6f + 1.0f;
    } else if (std::fabs(strength - 1.0f) > 0.001f) {
        const float scale = 1.0f / strength;
        newParams.fogStart *= scale;
        newParams.fogEnd *= scale;
    }

    applyLightHeadroom(newParams.ambientColor, newParams.diffuseColor);
    return newParams;
}

LightingParams Lighting::sample(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId, float hours,
                                bool isRaining, bool isUnderwater, uint32_t* vol0, uint32_t* vol1,
                                uint32_t* inRange) const {
    const float tod = std::fmod(hours / 24.0f, 1.0f);
    const float visual = resolveZoneVisualTimeHours(zoneId, indoors, tod * 24.0f);
    return target(playerPos, mapId, zoneId, tod, visual, isRaining, isUnderwater, vol0, vol1, inRange);
}

void Lighting::update(const glm::vec3& playerPos, uint32_t mapId, uint32_t zoneId, float gameTimeHours,
                      float dtSec, bool isRaining, bool isUnderwater) {
    if (!loaded()) return;
    // PSP: the caller always has a clock (server time, the fixed test hour
    // or the PSP's own); upstream reads the local time when it has none.
    if (gameTimeHours >= 0.0f) timeOfDay_ = std::fmod(gameTimeHours / 24.0f, 1.0f);
    visualHours_ = resolveZoneVisualTimeHours(zoneId, indoors, timeOfDay_ * 24.0f);

    const LightingParams newParams =
        target(playerPos, mapId, zoneId, timeOfDay_, visualHours_, isRaining, isUnderwater, &vol0_, &vol1_, &inRange_);

    // Upstream: smoothing by real time, dt clamped to 0.25 s, rate 5/s.
    // PSP: dt from the frame loop (upstream: steady_clock); snap on request.
    const float dt = glm::clamp(dtSec, 0.0f, 0.25f);
    const float blendFactor = snap_ ? 1.0f : 1.0f - std::exp(-dt * 5.0f);
    snap_ = false;
    current_.ambientColor = glm::mix(current_.ambientColor, newParams.ambientColor, blendFactor);
    current_.diffuseColor = glm::mix(current_.diffuseColor, newParams.diffuseColor, blendFactor);
    current_.fogColor = glm::mix(current_.fogColor, newParams.fogColor, blendFactor);
    current_.skyTopColor = glm::mix(current_.skyTopColor, newParams.skyTopColor, blendFactor);
    current_.skyMiddleColor = glm::mix(current_.skyMiddleColor, newParams.skyMiddleColor, blendFactor);
    current_.skyBand1Color = glm::mix(current_.skyBand1Color, newParams.skyBand1Color, blendFactor);
    current_.skyBand2Color = glm::mix(current_.skyBand2Color, newParams.skyBand2Color, blendFactor);
    current_.fogStart = glm::mix(current_.fogStart, newParams.fogStart, blendFactor);
    current_.fogEnd = glm::mix(current_.fogEnd, newParams.fogEnd, blendFactor);
    current_.fogDensity = glm::mix(current_.fogDensity, newParams.fogDensity, blendFactor);
    current_.cloudDensity = glm::mix(current_.cloudDensity, newParams.cloudDensity, blendFactor);
    current_.horizonGlow = glm::mix(current_.horizonGlow, newParams.horizonGlow, blendFactor);
    // PSP: a mix of opposite directions (snap across a half day) has no
    // length; normalize would divide by zero (FPU trap on the Allegrex).
    const glm::vec3 dir = glm::mix(current_.directionalDir, newParams.directionalDir, blendFactor);
    const float dirSq = glm::dot(dir, dir);
    current_.directionalDir = dirSq > 1e-8f ? dir * glm::inversesqrt(dirSq) : newParams.directionalDir;
}

} // namespace wowee::rendering::psp
