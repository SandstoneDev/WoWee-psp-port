// psp_spellfx_format.hpp - the WSFX effect blob and the spell visual table: layouts, enumerations, accessors and
// validation.
//
// Clean-room implementation of specs/world/spellfx_format.md (v2). Header only, <cstdint> only, no platform
// headers, plain little-endian PODs. The validator reads every field through byte copies (no unaligned loads, no
// type-punned pointers) and decides finiteness on bit patterns (no floating-point comparison with a NaN).
#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {
namespace psp {

// ---------------------------------------------------------------------------------------------------------------
// 2.1 Constants
// ---------------------------------------------------------------------------------------------------------------
constexpr uint16_t kSpellFxVersion = 1;
constexpr uint16_t kSpellFxVersion2 = 2;
constexpr uint16_t kFxNone = 0xFFFF;
constexpr int kFxMaxEmitters = 16;
constexpr int kFxSpellEmitters = 4;
constexpr int kFxMaxRibbons = 4;
constexpr int kFxLifeKeys = 5;
constexpr int kFxCurveCount = 6;
constexpr int kFxMaxBlockKeys = 256;
constexpr int kFxLifeCurves = 5;
constexpr int kFxL3Curves = 5;
constexpr int kFxMaxSplinePoints = 64;

// ---------------------------------------------------------------------------------------------------------------
// 2.2 Enumerations
// ---------------------------------------------------------------------------------------------------------------
enum SpellFxAttach : uint8_t {
    kFxAttachNone = 0,
    kFxAttachHand = 1,
    kFxAttachLeftHand = 2,
    kFxAttachChest = 5,
    kFxAttachHead = 11,
};

enum SpellFxFlags : uint8_t { kFxParticleDominant = 1, kFxHasRibbons = 2 };

enum ParticleClass : uint8_t {
    kPtcSpell = 1,
    kPtcFirefly = 2,
    kPtcFlame = 4,
    kPtcSmoke = 8,
    kPtcSpin = 16,
    kPtcItem = 32,
};

enum SpellFxExtFlags : uint8_t { kFxExtLevel3 = 1 };

enum SpellFxL3Curve : uint8_t {
    kFxCurveSpeedVar = 0,
    kFxCurveAreaL = 1,
    kFxCurveAreaW = 2,
    kFxCurveZSource = 3,
    kFxCurveEnabled = 4,
};

enum M2ParticleFlags : uint32_t {
    kM2PtcLit = 0x1,
    kM2PtcSort = 0x2,
    kM2PtcVelocityQuad = 0x4,
    kM2PtcNoFog = 0x8,
    kM2PtcLocal = 0x10,
    kM2PtcModelScale = 0x20,
    kM2PtcInheritVelocity = 0x40,
    kM2PtcSphereInward = 0x80,
    kM2PtcSphereUp = 0x100,
    kM2PtcRandomSpin = 0x200,
    kM2PtcTailGrows = 0x400,
    kM2PtcAlongPath = 0x800,
    kM2PtcXYQuad = 0x1000,
    kM2PtcGround = 0x2000,
    kM2PtcFollow = 0x4000,
    kM2PtcBurstOnce = 0x8000,
    kM2PtcRandomCell = 0x10000,
    kM2PtcHead = 0x20000,
    kM2PtcTail = 0x40000,
    kM2PtcScaleVaryXY = 0x80000,
};

enum SpellFxLifeCurve : uint8_t {
    kFxLifeColor = 0,
    kFxLifeAlpha = 1,
    kFxLifeScale = 2,
    kFxLifeHead = 3,
    kFxLifeTail = 4,
};

enum SpellFxL3RibbonCurve : uint8_t {
    kFxRibbonR = 0,
    kFxRibbonG = 1,
    kFxRibbonB = 2,
    kFxRibbonAlpha = 3,
    kFxRibbonAbove = 4,
    kFxRibbonBelow = 5,
    kFxRibbonVisible = 6,
    kFxRibbonSlot = 7,
    kFxRibbonCurves = 8,
};

enum SpellFxTrackFlags : uint16_t { kFxTrackBillboard = 1, kFxTrackMatrix = 2 };

enum SpellFxBlend : uint8_t { kFxBlendAlpha = 0, kFxBlendAdd = 1, kFxBlendAlphaKey = 2 };

enum SpellFxGroupFlags : uint8_t { kFxWrapU = 1, kFxWrapV = 2 };

enum SpellFxCurve : uint8_t {
    kFxCurveRate = 0,
    kFxCurveLife = 1,
    kFxCurveSpeed = 2,
    kFxCurveVRange = 3,
    kFxCurveHRange = 4,
    kFxCurveGravity = 5,
};

enum SpellFxEmitterFlags : uint8_t { kFxEmitRandomTile = 0x40, kFxEmitTiled = 0x80 };

// ---------------------------------------------------------------------------------------------------------------
// 2.3 Structures
// ---------------------------------------------------------------------------------------------------------------
struct SpellFxVisualTable {
    char magic[4];  // "WSFV"
    uint16_t version;  // 1
    uint16_t reserved;
    uint32_t count;
    uint32_t effectCount;
    uint32_t textureCount;
};

struct SpellFxVisual {
    uint32_t visual;  // SpellVisual.dbc ID
    uint16_t precast;  // effect index n of "e_<n>", or kFxNone
    uint16_t cast;
    uint16_t impact;
    uint16_t reserved;
};

struct SpellFxHeader {
    char magic[4];  // "WSFX"
    uint16_t version;
    uint8_t attach;  // SpellFxAttach
    uint8_t flags;  // SpellFxFlags
    uint32_t durationMs;
    uint32_t periodMs;
    uint16_t frames;
    uint16_t trackCount;
    uint16_t groupCount;
    uint16_t emitterCount;
    uint16_t textureCount;
    uint16_t sourceEmitters;
    float center[3];
    float half[3];
    float radius;
    uint32_t trackOffset;
    uint32_t groupOffset;
    uint32_t emitterOffset;
    uint32_t textureOffset;
    uint32_t poolOffset;
    uint32_t vertexOffset;
    uint32_t vertexCount;
    uint32_t size;
};

struct SpellFxHeaderExt {
    uint8_t cls;  // ParticleClass
    uint8_t extFlags;  // SpellFxExtFlags
    uint16_t ribbonCount;
    uint32_t ribbonOffset;
    float reach;
    uint32_t level3Offset;
};

struct SpellFxL3Emitter {
    uint32_t flags;  // M2 particle flags word, unchanged
    uint8_t type;  // 1 plane, 2 sphere, 3 spline
    uint8_t blend;  // M2 blending type 0..6
    uint8_t lut;
    uint8_t cellShift;
    uint16_t splineCount;
    uint16_t stepMask;
    float lifespanVary;  // 21 consecutive floats: lifespanVary .. scaleVary[1]
    float rateVary;
    float tailLength;
    float twinkleSpeed;
    float twinklePercent;
    float twinkleMin;
    float twinkleRange;
    float burst;
    float drag;
    float baseSpin;
    float baseSpinVary;
    float spin;
    float spinVary;
    float wind[3];
    float windTime;
    float followBase;
    float followSlope;
    float scaleVary[2];
    uint32_t lifeOffset[5];
    uint16_t lifeKeys[5];
    uint16_t reserved1;
    uint32_t curveOffset[5];
    uint16_t curveFrames[5];
    uint16_t splineLens;
    uint32_t splineOffset;
};

struct SpellFxL3Ribbon {
    uint8_t blend;
    uint8_t rows;
    uint8_t cols;
    uint8_t reserved;
    float edgesPerSecond;
    float edgeLifetime;
    float gravity;
    uint32_t curveOffset[8];
    uint16_t curveFrames[8];
};

struct SpellFxRibbon {
    uint16_t track;
    uint16_t texture;
    uint8_t blend;
    uint8_t flags;  // 0
    uint16_t reserved;
    float position[3];
    uint32_t color;  // ABGR8888
    float heightAbove;
    float heightBelow;
    float visibility;
    float edgesPerSecond;
    float edgeLifetime;
    float gravity;
};

struct SpellFxTrack {
    uint32_t offset;
    uint16_t frames;
    uint16_t flags;  // SpellFxTrackFlags
    float pivot[3];
    float translationScale;
    float scaleScale;
    uint16_t bone;
    uint16_t reserved;
};

struct SpellFxKey {
    int16_t q[4];
    int16_t t[3];
    int16_t s[3];
};

struct SpellFxMatrixKey {
    int16_t m[9];
    int16_t t[3];
};

struct SpellFxGroup {
    uint32_t vertexStart;
    uint16_t vertexCount;
    uint16_t texture;
    uint16_t track;
    uint8_t blend;
    uint8_t flags;  // SpellFxGroupFlags
    uint32_t color;  // ABGR8888
    uint32_t uvOffset;
    uint16_t uvFrames;
    uint16_t reserved;
    float uvMin[2];
    float uvScale[2];
};

struct SpellFxEmitter {
    uint16_t track;
    uint16_t texture;
    uint8_t blend;
    uint8_t flags;  // SpellFxEmitterFlags
    uint8_t cols;
    uint8_t rows;
    float position[3];
    uint32_t color[5];  // ABGR8888
    float scale[5];
    uint32_t curveOffset[6];
    uint16_t curveFrames[6];
};

struct SpellFxVertex {
    uint16_t u;
    uint16_t v;
    int16_t x;
    int16_t y;
    int16_t z;
};

static_assert(sizeof(SpellFxVisualTable) == 20, "SpellFxVisualTable size");
static_assert(sizeof(SpellFxVisual) == 12, "SpellFxVisual size");
static_assert(sizeof(SpellFxHeader) == 88, "SpellFxHeader size");
static_assert(sizeof(SpellFxHeaderExt) == 16, "SpellFxHeaderExt size");
static_assert(sizeof(SpellFxL3Emitter) == 164, "SpellFxL3Emitter size");
static_assert(sizeof(SpellFxL3Ribbon) == 64, "SpellFxL3Ribbon size");
static_assert(sizeof(SpellFxRibbon) == 48, "SpellFxRibbon size");
static_assert(sizeof(SpellFxTrack) == 32, "SpellFxTrack size");
static_assert(sizeof(SpellFxKey) == 20, "SpellFxKey size");
static_assert(sizeof(SpellFxMatrixKey) == 24, "SpellFxMatrixKey size");
static_assert(sizeof(SpellFxGroup) == 40, "SpellFxGroup size");
static_assert(sizeof(SpellFxEmitter) == 96, "SpellFxEmitter size");
static_assert(sizeof(SpellFxVertex) == 10, "SpellFxVertex size");

// ---------------------------------------------------------------------------------------------------------------
// Internal helpers (byte-wise loads: safe for any alignment, little-endian like the format and both targets)
// ---------------------------------------------------------------------------------------------------------------
namespace spellfx_detail {

template <typename T>
inline void load(T& out, const uint8_t* p) {
    uint8_t* d = reinterpret_cast<uint8_t*>(&out);
    for (unsigned i = 0; i < sizeof(T); ++i) d[i] = p[i];
}

inline uint32_t bitsOf(float f) {
    uint32_t u = 0;
    load(u, reinterpret_cast<const uint8_t*>(&f));
    return u;
}

// Fits(off, n, each): off <= size and, unless n is 0, (size - off) / each >= n (integer division, no overflow).
inline bool fits(uint64_t size, uint64_t off, uint64_t n, uint64_t each) {
    if (off > size) return false;
    if (n == 0) return true;
    return (size - off) / each >= n;
}

inline uint32_t roundUp4(uint32_t v) { return (v + 3u) & ~3u; }

}  // namespace spellfx_detail

// ---------------------------------------------------------------------------------------------------------------
// 3.4 Accessors
// ---------------------------------------------------------------------------------------------------------------
inline bool fxFinite(float f) {
    return (spellfx_detail::bitsOf(f) & 0x7F800000u) != 0x7F800000u;
}

inline int fxLifeDim(int curve) {
    return curve == kFxLifeColor ? 3 : (curve == kFxLifeScale ? 2 : 1);
}

inline uint32_t spellFxKeySize(const SpellFxTrack& t) {
    return (t.flags & kFxTrackMatrix) ? 24u : 20u;
}

inline const uint16_t* fxLifeTimes(const uint8_t* base, const SpellFxL3Emitter& c, int curve) {
    return reinterpret_cast<const uint16_t*>(base + c.lifeOffset[curve]);
}

inline const float* fxLifeValues(const uint8_t* base, const SpellFxL3Emitter& c, int curve) {
    return reinterpret_cast<const float*>(base + c.lifeOffset[curve] +
                                          spellfx_detail::roundUp4(2u * c.lifeKeys[curve]));
}

inline const SpellFxHeaderExt* spellFxExt(const void* data) {
    if (!data) return nullptr;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    uint16_t version = 0;
    spellfx_detail::load(version, b + 4);
    if (version < kSpellFxVersion2) return nullptr;
    return reinterpret_cast<const SpellFxHeaderExt*>(b + sizeof(SpellFxHeader));
}

inline const SpellFxL3Emitter* spellFxLevel3(const void* data) {
    if (!spellFxExt(data)) return nullptr;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    SpellFxHeaderExt ext;
    spellfx_detail::load(ext, b + sizeof(SpellFxHeader));
    if (!(ext.extFlags & kFxExtLevel3) || ext.level3Offset == 0) return nullptr;
    return reinterpret_cast<const SpellFxL3Emitter*>(b + ext.level3Offset);
}

inline const SpellFxL3Ribbon* spellFxLevel3Ribbons(const void* data) {
    const SpellFxL3Emitter* e = spellFxLevel3(data);
    if (!e) return nullptr;
    uint16_t emitterCount = 0;
    spellfx_detail::load(emitterCount, static_cast<const uint8_t*>(data) + 22);
    return reinterpret_cast<const SpellFxL3Ribbon*>(reinterpret_cast<const uint8_t*>(e) +
                                                    static_cast<uint32_t>(emitterCount) * sizeof(SpellFxL3Emitter));
}

// ---------------------------------------------------------------------------------------------------------------
// 3.5 Validation
// ---------------------------------------------------------------------------------------------------------------
namespace spellfx_detail {

inline bool finiteAt(const uint8_t* b, uint64_t off) {
    uint32_t u = 0;
    load(u, b + off);
    return (u & 0x7F800000u) != 0x7F800000u;
}

inline bool allFinite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!fxFinite(v[i])) return false;
    return true;
}

// "Curve list ok" for an (offset, frames) pair.
inline bool curveListOk(const uint8_t* b, uint32_t size, uint32_t off, uint16_t frames, uint16_t hdrFrames) {
    if (!(frames == 1 || frames == hdrFrames)) return false;
    if (off % 4u != 0) return false;
    if (!fits(size, off, frames, 4)) return false;
    for (uint32_t i = 0; i < frames; ++i)
        if (!finiteAt(b, static_cast<uint64_t>(off) + 4u * i)) return false;
    return true;
}

inline bool trackOk(uint16_t track, uint16_t trackCount) { return track == kFxNone || track < trackCount; }

inline bool level3EmitterOk(const uint8_t* b, uint32_t size, const SpellFxL3Emitter& c, uint16_t hdrFrames) {
    if (c.type < 1 || c.type > 3) return false;
    if (c.blend > 6) return false;
    if (c.cellShift > 8) return false;
    // the 21 floats lifespanVary .. scaleVary[1]
    const float floats21[21] = {c.lifespanVary, c.rateVary, c.tailLength, c.twinkleSpeed, c.twinklePercent,
                                c.twinkleMin, c.twinkleRange, c.burst, c.drag, c.baseSpin, c.baseSpinVary,
                                c.spin, c.spinVary, c.wind[0], c.wind[1], c.wind[2], c.windTime, c.followBase,
                                c.followSlope, c.scaleVary[0], c.scaleVary[1]};
    if (!allFinite(floats21, 21)) return false;

    // life curves
    for (int k = 0; k < kFxLifeCurves; ++k) {
        const uint32_t n = c.lifeKeys[k];
        if (n == 0) continue;
        if (n > static_cast<uint32_t>(kFxMaxBlockKeys)) return false;
        const uint32_t off = c.lifeOffset[k];
        if (off % 4u != 0) return false;
        const uint32_t t = roundUp4(2u * n);
        if (!fits(size, off, 1, t)) return false;
        const uint64_t valOff = static_cast<uint64_t>(off) + t;
        const uint64_t values = static_cast<uint64_t>(n) * static_cast<uint64_t>(fxLifeDim(k));
        if (!fits(size, valOff, values, 4)) return false;
        for (uint64_t i = 0; i < values; ++i)
            if (!finiteAt(b, valOff + 4u * i)) return false;
        uint16_t prev = 0;
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t stamp = 0;
            load(stamp, b + off + 2u * i);
            if (i > 0 && stamp < prev) return false;
            prev = stamp;
        }
    }

    // extra curves
    for (int k = 0; k < kFxL3Curves; ++k)
        if (!curveListOk(b, size, c.curveOffset[k], c.curveFrames[k], hdrFrames)) return false;

    // spline
    if (c.splineCount > kFxMaxSplinePoints) return false;
    if (c.splineCount > 0) {
        if (c.splineOffset % 4u != 0) return false;
        if (!fits(size, c.splineOffset, c.splineCount, 12)) return false;
        for (uint32_t i = 0; i < 3u * c.splineCount; ++i)
            if (!finiteAt(b, static_cast<uint64_t>(c.splineOffset) + 4u * i)) return false;
        if (c.splineCount % 3u != 1u) return false;
    }
    if (c.splineLens > 0) {
        const uint32_t segs = c.splineCount >= 7 ? (c.splineCount - 1u) / 3u : 0u;
        if (c.splineLens != segs) return false;
        const uint64_t lensOff = static_cast<uint64_t>(c.splineOffset) + 12u * static_cast<uint64_t>(c.splineCount);
        if (!fits(size, lensOff, segs, 4)) return false;
        for (uint32_t i = 0; i < segs; ++i) {
            float len = 0.0f;
            load(len, b + lensOff + 4u * i);
            if (!fxFinite(len)) return false;
            if (len < 0.0f) return false;
        }
    }
    return true;
}

inline bool level3RibbonOk(const uint8_t* b, uint32_t size, const SpellFxL3Ribbon& r, uint16_t hdrFrames) {
    if (r.blend > 6) return false;
    if (!fxFinite(r.edgesPerSecond) || !fxFinite(r.edgeLifetime) || !fxFinite(r.gravity)) return false;
    if (r.edgeLifetime < 0.25f || r.edgeLifetime > 100.0f) return false;
    if (r.edgesPerSecond < 0.0f || r.edgesPerSecond > 1000.0f) return false;
    for (int k = 0; k < kFxRibbonCurves; ++k)
        if (!curveListOk(b, size, r.curveOffset[k], r.curveFrames[k], hdrFrames)) return false;
    return true;
}

}  // namespace spellfx_detail

inline bool spellFxValid(const void* data, uint32_t size) {
    using namespace spellfx_detail;
    // 1. header
    if (!data) return false;
    if (size < sizeof(SpellFxHeader)) return false;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    SpellFxHeader h;
    load(h, b);
    if (h.magic[0] != 'W' || h.magic[1] != 'S' || h.magic[2] != 'F' || h.magic[3] != 'X') return false;
    if (h.version != kSpellFxVersion && h.version != kSpellFxVersion2) return false;
    if (h.size != size) return false;
    if (h.periodMs == 0 || h.frames == 0) return false;
    if (h.emitterCount > kFxMaxEmitters) return false;
    if (!allFinite(h.center, 3) || !allFinite(h.half, 3) || !fxFinite(h.radius)) return false;

    // 2. version 2 extension, ribbons and the level-3 section
    if (h.version == kSpellFxVersion2) {
        if (size < sizeof(SpellFxHeader) + sizeof(SpellFxHeaderExt)) return false;
        SpellFxHeaderExt x;
        load(x, b + sizeof(SpellFxHeader));
        if (x.ribbonCount > kFxMaxRibbons) return false;
        if (x.ribbonOffset % 4u != 0) return false;
        if (!fits(size, x.ribbonOffset, x.ribbonCount, sizeof(SpellFxRibbon))) return false;
        if (!fxFinite(x.reach) || x.reach < 0.0f || x.reach > 100000.0f) return false;
        for (uint32_t i = 0; i < x.ribbonCount; ++i) {
            SpellFxRibbon r;
            load(r, b + x.ribbonOffset + i * sizeof(SpellFxRibbon));
            if (!trackOk(r.track, h.trackCount)) return false;
            if (!allFinite(r.position, 3)) return false;
            if (!fxFinite(r.heightAbove) || !fxFinite(r.heightBelow) || !fxFinite(r.visibility) ||
                !fxFinite(r.edgesPerSecond) || !fxFinite(r.edgeLifetime) || !fxFinite(r.gravity))
                return false;
            if (r.edgesPerSecond < 0.0f || r.edgesPerSecond > 1000.0f) return false;
            if (!(r.edgeLifetime > 0.0f) || r.edgeLifetime > 100.0f) return false;
            if (r.heightAbove < -1000.0f || r.heightAbove > 1000.0f) return false;
            if (r.heightBelow < -1000.0f || r.heightBelow > 1000.0f) return false;
        }
        if (x.extFlags & kFxExtLevel3) {
            if (x.level3Offset == 0 || x.level3Offset % 4u != 0) return false;
            if (!fits(size, x.level3Offset, h.emitterCount, sizeof(SpellFxL3Emitter))) return false;
            const uint64_t ribOff =
                static_cast<uint64_t>(x.level3Offset) + static_cast<uint64_t>(h.emitterCount) * sizeof(SpellFxL3Emitter);
            if (!fits(size, ribOff, x.ribbonCount, sizeof(SpellFxL3Ribbon))) return false;
            for (uint32_t i = 0; i < h.emitterCount; ++i) {
                SpellFxL3Emitter c;
                load(c, b + x.level3Offset + i * sizeof(SpellFxL3Emitter));
                if (!level3EmitterOk(b, size, c, h.frames)) return false;
            }
            for (uint32_t i = 0; i < x.ribbonCount; ++i) {
                SpellFxL3Ribbon r;
                load(r, b + ribOff + i * sizeof(SpellFxL3Ribbon));
                if (!level3RibbonOk(b, size, r, h.frames)) return false;
            }
        }
    }

    // 3. tables
    if (!fits(size, h.trackOffset, h.trackCount, sizeof(SpellFxTrack))) return false;
    if (!fits(size, h.groupOffset, h.groupCount, sizeof(SpellFxGroup))) return false;
    if (!fits(size, h.emitterOffset, h.emitterCount, sizeof(SpellFxEmitter))) return false;
    if (!fits(size, h.textureOffset, h.textureCount, 2)) return false;
    if (!fits(size, h.vertexOffset, h.vertexCount, sizeof(SpellFxVertex))) return false;
    if (h.vertexOffset % 16u != 0) return false;
    if (h.trackOffset % 4u != 0 || h.groupOffset % 4u != 0 || h.emitterOffset % 4u != 0) return false;

    // 4. tracks
    for (uint32_t i = 0; i < h.trackCount; ++i) {
        SpellFxTrack t;
        load(t, b + h.trackOffset + i * sizeof(SpellFxTrack));
        if (!(t.frames == 1 || t.frames == h.frames)) return false;
        if (t.offset % 2u != 0) return false;
        if (!fits(size, t.offset, t.frames, spellFxKeySize(t))) return false;
        if (!allFinite(t.pivot, 3) || !fxFinite(t.translationScale) || !fxFinite(t.scaleScale)) return false;
    }

    // 5. groups
    for (uint32_t i = 0; i < h.groupCount; ++i) {
        SpellFxGroup g;
        load(g, b + h.groupOffset + i * sizeof(SpellFxGroup));
        if (g.vertexCount < 3) return false;
        if (g.vertexStart > h.vertexCount || h.vertexCount - g.vertexStart < g.vertexCount) return false;
        if (!trackOk(g.track, h.trackCount)) return false;
        if (g.uvOffset != 0) {
            if (!(g.uvFrames == 1 || g.uvFrames == h.frames)) return false;
            if (g.uvOffset % 4u != 0) return false;
            if (!fits(size, g.uvOffset, g.uvFrames, 8)) return false;
            for (uint32_t k = 0; k < 2u * g.uvFrames; ++k)
                if (!finiteAt(b, static_cast<uint64_t>(g.uvOffset) + 4u * k)) return false;
        }
        if (!allFinite(g.uvMin, 2) || !allFinite(g.uvScale, 2)) return false;
    }

    // 6. emitters
    for (uint32_t i = 0; i < h.emitterCount; ++i) {
        SpellFxEmitter e;
        load(e, b + h.emitterOffset + i * sizeof(SpellFxEmitter));
        if (!trackOk(e.track, h.trackCount)) return false;
        if (!allFinite(e.position, 3) || !allFinite(e.scale, 5)) return false;
        for (int k = 0; k < kFxCurveCount; ++k)
            if (!curveListOk(b, size, e.curveOffset[k], e.curveFrames[k], h.frames)) return false;
    }
    return true;
}

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
