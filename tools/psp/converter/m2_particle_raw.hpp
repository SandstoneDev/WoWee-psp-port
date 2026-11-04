// Bounded, read-only reader of the particle and ribbon emitters of a version-264 M2 file, with track sampling.
// Clean-room implementation of cleanroom/specs/particles/m2-particle-records.md (REC-*). PC only (converter, tools).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace wowee {
namespace pspconv {
namespace m2raw {

constexpr uint32_t kParticleSize = 0x1DC;
constexpr uint32_t kRibbonSize = 0xB0;

enum class Kind : uint8_t { F32, U8, U16, Fixed16, Vec3, Vec2 };

struct Track {
    uint16_t interp = 0;
    int16_t gseq = 0;
    uint32_t tsCount = 0, tsOfs = 0, valCount = 0, valOfs = 0;
    Kind kind = Kind::F32;
    bool any = false;
};

struct FBlock {
    std::vector<uint16_t> ts;
    std::vector<float> v;
    int dim = 0;
    size_t keys() const { return dim ? v.size() / static_cast<size_t>(dim) : 0; }
};

struct Emitter {
    uint32_t base = 0;  // record offset in the file
    int32_t id = -1;
    uint32_t flags = 0;
    glm::vec3 position{0.0f};
    uint16_t bone = 0, texture = 0;
    std::string geometryModel, recursionModel;
    uint8_t blend = 0, type = 0;
    uint16_t colorIndex = 0;
    uint8_t particleType = 0, headOrTail = 0;
    int16_t tileRotation = 0;
    uint16_t rows = 1, cols = 1;
    Track speed, speedVar, vRange, hRange, gravity, life;
    float lifeVary = 0.0f;
    Track rate;
    float rateVary = 0.0f;
    Track areaL, areaW, zSource;
    FBlock color, alpha, scale;
    glm::vec2 scaleVary{0.0f};
    FBlock headCell, tailCell;
    float tailLength = 0.0f, twinkleSpeed = 0.0f, twinklePercent = 1.0f, twinkleMin = 1.0f, twinkleMax = 1.0f;
    float burst = 0.0f, drag = 0.0f, baseSpin = 0.0f, baseSpinVary = 0.0f, spin = 0.0f, spinVary = 0.0f;
    glm::vec3 tumbleMin{0.0f}, tumbleMax{0.0f};
    glm::vec3 wind{0.0f};
    float windTime = 0.0f;
    float followSpeed1 = 0.0f, followScale1 = 0.0f, followSpeed2 = 0.0f, followScale2 = 0.0f;
    std::vector<glm::vec3> spline;
    Track enabledIn;
};

struct Ribbon {
    uint32_t base = 0;
    int32_t id = -1;
    uint32_t bone = 0;
    glm::vec3 position{0.0f};
    std::vector<uint16_t> textures, materials;
    Track color, alpha, above, below;
    float edgesPerSecond = 0.0f, edgeLifetime = 0.0f, gravity = 0.0f;
    uint16_t rows = 1, cols = 1;
    Track texSlot, visibility;
    int16_t priority = 0;
};

struct Sequence {
    uint16_t id = 0, variation = 0;
    uint32_t duration = 0, flags = 0;
};

// Extra header tables the level-3 bake needs (m2-particle-records.md 3.1); not part of the callers' interface.
struct TextureDef {
    uint32_t type = 0, flags = 0;
    std::string name;
};
struct MaterialDef {
    uint16_t flags = 0, blend = 0;
};

class Model {
public:
    uint32_t version = 0;
    std::vector<uint32_t> globalSeq;
    std::vector<Sequence> sequences;
    std::vector<Emitter> emitters;
    std::vector<Ribbon> ribbons;
    // extra (bake): header bone count, texture table, material table
    uint32_t boneCount = 0;
    std::vector<TextureDef> textures;
    std::vector<MaterialDef> materials;

    bool parse(const std::vector<uint8_t>& m2, const std::vector<uint8_t>* anim0 = nullptr) {
        *this = Model();
        if (m2.size() < 0x130) return false;
        if (std::memcmp(m2.data(), "MD20", 4) != 0) return false;
        Reader r{m2.data(), m2.size()};
        uint32_t ver = r.u32(0x004);
        if (ver < 264) return false;
        version = ver;
        m2_ = m2;
        if (anim0 && !anim0->empty()) {
            anim_ = *anim0;
            hasAnim_ = true;
        }
        const Reader f{m2_.data(), m2_.size()};

        uint32_t n = f.u32(0x014), o = f.u32(0x018);
        if (n < 4096 && f.inside(o, uint64_t(n) * 4)) {
            globalSeq.resize(n);
            for (uint32_t i = 0; i < n; ++i) globalSeq[i] = f.u32(o + 4 * i);
        }
        n = f.u32(0x01C);
        o = f.u32(0x020);
        if (n < 4096 && f.inside(o, uint64_t(n) * 0x40)) {
            sequences.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t b = o + 0x40 * i;
                sequences[i].id = f.u16(b + 0x00);
                sequences[i].variation = f.u16(b + 0x02);
                sequences[i].duration = f.u32(b + 0x04);
                sequences[i].flags = f.u32(b + 0x0C);
            }
        }
        boneCount = f.u32(0x02C);
        n = f.u32(0x050);
        o = f.u32(0x054);
        if (n < 4096 && f.inside(o, uint64_t(n) * 16)) {
            textures.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t b = o + 16 * i;
                textures[i].type = f.u32(b);
                textures[i].flags = f.u32(b + 4);
                textures[i].name = f.str(b + 8);
            }
        }
        n = f.u32(0x070);
        o = f.u32(0x074);
        if (n < 4096 && f.inside(o, uint64_t(n) * 4)) {
            materials.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                materials[i].flags = f.u16(o + 4 * i);
                materials[i].blend = f.u16(o + 4 * i + 2);
            }
        }
        n = f.u32(0x128);
        o = f.u32(0x12C);
        if (n < 256 && f.inside(o, uint64_t(n) * kParticleSize)) {
            emitters.resize(n);
            for (uint32_t i = 0; i < n; ++i) readEmitter(f, o + kParticleSize * i, emitters[i]);
        }
        n = f.u32(0x120);
        o = f.u32(0x124);
        if (n < 256 && f.inside(o, uint64_t(n) * kRibbonSize)) {
            ribbons.resize(n);
            for (uint32_t i = 0; i < n; ++i) readRibbon(f, o + kRibbonSize * i, ribbons[i]);
        }
        return true;
    }

    bool seqInAnim(int seq) const {
        return seq >= 0 && static_cast<size_t>(seq) < sequences.size() && !(sequences[seq].flags & 0x20u);
    }

    uint32_t keyCount(const Track& t, int seq) const {
        int64_t i = t.gseq >= 0 ? 0 : seq;
        if (t.tsCount == 0 || i < 0 || i >= int64_t(t.tsCount)) return 0;
        const Reader f{m2_.data(), m2_.size()};
        return f.u32(t.tsOfs + 8 * uint32_t(i));
    }

    bool sample(const Track& t, int seq, float tMs, float* out, int dim) const {
        int64_t i = t.gseq >= 0 ? 0 : seq;
        if (t.tsCount == 0 || t.valCount == 0) return false;
        if (i < 0 || i >= int64_t(t.tsCount) || i >= int64_t(t.valCount)) return false;
        const Reader f{m2_.data(), m2_.size()};
        uint32_t nT = f.u32(t.tsOfs + 8 * uint32_t(i)), oT = f.u32(t.tsOfs + 8 * uint32_t(i) + 4);
        uint32_t nV = f.u32(t.valOfs + 8 * uint32_t(i)), oV = f.u32(t.valOfs + 8 * uint32_t(i) + 4);
        if (nT == 0 || nV == 0) return false;
        bool external = t.gseq < 0 && seqInAnim(seq);
        if (external && !hasAnim_) return false;
        const Reader src = external ? Reader{anim_.data(), anim_.size()} : f;
        uint32_t n = nT < nV ? nT : nV;
        uint32_t stride = (t.interp == 2 || t.interp == 3) ? 3u : 1u;
        uint32_t es = elemSize(t.kind);
        if (!src.inside(oT, uint64_t(n) * 4)) return false;
        if (!src.inside(oV, uint64_t(n) * stride * es)) return false;

        float tau = tMs;
        if (t.gseq >= 0 && static_cast<size_t>(t.gseq) < globalSeq.size() && globalSeq[t.gseq] > 0)
            tau = std::fmod(tMs, static_cast<float>(globalSeq[t.gseq]));

        auto stamp = [&](uint32_t k) { return static_cast<float>(src.u32(oT + 4 * k)); };
        int kd = kindDim(t.kind);
        int w = dim < kd ? dim : kd;
        float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
        if (n == 1 || tau <= stamp(0)) {
            decode(src, oV, t.kind, a);
        } else if (tau >= stamp(n - 1)) {
            decode(src, oV + (n - 1) * stride * es, t.kind, a);
        } else {
            uint32_t k = 0;
            while (k + 1 < n && !(stamp(k + 1) > tau)) ++k;
            decode(src, oV + k * stride * es, t.kind, a);
            if (t.interp != 0) {
                float t0 = stamp(k), t1 = stamp(k + 1);
                float fr = t1 > t0 ? (tau - t0) / (t1 - t0) : 0.0f;
                decode(src, oV + (k + 1) * stride * es, t.kind, b);
                for (int c = 0; c < kd; ++c) a[c] = a[c] + (b[c] - a[c]) * fr;
            }
        }
        for (int c = 0; c < w; ++c) out[c] = a[c];
        return true;
    }

    float sampleF(const Track& t, int seq, float tMs, float def) const {
        float v[3];
        return sample(t, seq, tMs, v, 1) ? v[0] : def;
    }

private:
    std::vector<uint8_t> m2_;
    std::vector<uint8_t> anim_;
    bool hasAnim_ = false;

    struct Reader {
        const uint8_t* d;
        size_t n;
        bool inside(uint64_t off, uint64_t len) const { return off <= n && len <= n - off; }
        uint8_t u8(uint64_t o) const { return inside(o, 1) ? d[o] : 0; }
        uint16_t u16(uint64_t o) const {
            if (!inside(o, 2)) return 0;
            return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
        }
        uint32_t u32(uint64_t o) const {
            if (!inside(o, 4)) return 0;
            return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24);
        }
        int16_t i16(uint64_t o) const { return static_cast<int16_t>(u16(o)); }
        int32_t i32(uint64_t o) const { return static_cast<int32_t>(u32(o)); }
        float f32(uint64_t o) const {
            uint32_t b = u32(o);
            float f;
            std::memcpy(&f, &b, 4);
            return f;
        }
        glm::vec3 v3(uint64_t o) const { return glm::vec3(f32(o), f32(o + 4), f32(o + 8)); }
        // M2Array<char> at o: 1..1024 bytes inside the file, cut at the first NUL
        std::string str(uint64_t o) const {
            uint32_t cnt = u32(o), ofs = u32(o + 4);
            if (cnt < 1 || cnt > 1024 || !inside(ofs, cnt)) return std::string();
            size_t len = 0;
            while (len < cnt && d[ofs + len] != 0) ++len;
            return std::string(reinterpret_cast<const char*>(d + ofs), len);
        }
    };

    static uint32_t elemSize(Kind k) {
        switch (k) {
            case Kind::U8: return 1;
            case Kind::U16:
            case Kind::Fixed16: return 2;
            case Kind::Vec3: return 12;
            case Kind::Vec2: return 8;
            default: return 4;
        }
    }
    static int kindDim(Kind k) { return k == Kind::Vec3 ? 3 : (k == Kind::Vec2 ? 2 : 1); }
    static void decode(const Reader& r, uint64_t o, Kind k, float* out) {
        switch (k) {
            case Kind::F32: out[0] = r.f32(o); break;
            case Kind::U8: out[0] = static_cast<float>(r.u8(o)); break;
            case Kind::U16: out[0] = static_cast<float>(r.u16(o)); break;
            case Kind::Fixed16: out[0] = static_cast<float>(r.i16(o)) / 32767.0f; break;
            case Kind::Vec3:
                out[0] = r.f32(o);
                out[1] = r.f32(o + 4);
                out[2] = r.f32(o + 8);
                break;
            case Kind::Vec2:
                out[0] = r.f32(o);
                out[1] = r.f32(o + 4);
                break;
        }
    }

    static Track readTrack(const Reader& f, uint64_t o, Kind kind) {
        Track t;
        t.kind = kind;
        t.interp = f.u16(o);
        t.gseq = f.i16(o + 2);
        uint32_t nT = f.u32(o + 4), oT = f.u32(o + 8), nV = f.u32(o + 12), oV = f.u32(o + 16);
        bool ok = nT <= 4096 && nV <= 4096 && f.inside(oT, uint64_t(nT) * 8) && f.inside(oV, uint64_t(nV) * 8);
        if (!ok) return t;  // no data
        t.tsCount = nT;
        t.tsOfs = oT;
        t.valCount = nV;
        t.valOfs = oV;
        uint32_t m = nT < nV ? nT : nV;
        for (uint32_t i = 0; i < m && !t.any; ++i)
            if (f.u32(oT + 8 * i) != 0 && f.u32(oV + 8 * i) != 0) t.any = true;
        return t;
    }

    // FBlock: timestamps (u16) then values; element types per curve
    static FBlock readBlock(const Reader& f, uint64_t o, Kind kind) {
        FBlock b;
        b.dim = kindDim(kind);
        uint32_t nT = f.u32(o), oT = f.u32(o + 4), nV = f.u32(o + 8), oV = f.u32(o + 12);
        if (nT > 1024 || nV > 1024) return b;
        if (f.inside(oT, uint64_t(nT) * 2)) {
            b.ts.resize(nT);
            for (uint32_t i = 0; i < nT; ++i) b.ts[i] = f.u16(oT + 2 * i);
        }
        uint32_t es = elemSize(kind);
        if (f.inside(oV, uint64_t(nV) * es)) {
            b.v.resize(size_t(nV) * b.dim);
            for (uint32_t i = 0; i < nV; ++i) decode(f, oV + uint64_t(i) * es, kind, &b.v[size_t(i) * b.dim]);
        }
        return b;
    }

    static void readEmitter(const Reader& f, uint32_t b, Emitter& e) {
        e.base = b;
        e.id = f.i32(b + 0x000);
        e.flags = f.u32(b + 0x004);
        e.position = f.v3(b + 0x008);
        e.bone = f.u16(b + 0x014);
        e.texture = f.u16(b + 0x016);
        e.geometryModel = f.str(b + 0x018);
        e.recursionModel = f.str(b + 0x020);
        e.blend = f.u8(b + 0x028);
        e.type = f.u8(b + 0x029);
        e.colorIndex = f.u16(b + 0x02A);
        e.particleType = f.u8(b + 0x02C);
        e.headOrTail = f.u8(b + 0x02D);
        e.tileRotation = f.i16(b + 0x02E);
        e.rows = f.u16(b + 0x030);
        e.cols = f.u16(b + 0x032);
        e.speed = readTrack(f, b + 0x034, Kind::F32);
        e.speedVar = readTrack(f, b + 0x048, Kind::F32);
        e.vRange = readTrack(f, b + 0x05C, Kind::F32);
        e.hRange = readTrack(f, b + 0x070, Kind::F32);
        e.gravity = readTrack(f, b + 0x084, Kind::F32);
        e.life = readTrack(f, b + 0x098, Kind::F32);
        e.lifeVary = f.f32(b + 0x0AC);
        e.rate = readTrack(f, b + 0x0B0, Kind::F32);
        e.rateVary = f.f32(b + 0x0C4);
        e.areaL = readTrack(f, b + 0x0C8, Kind::F32);
        e.areaW = readTrack(f, b + 0x0DC, Kind::F32);
        e.zSource = readTrack(f, b + 0x0F0, Kind::F32);
        e.color = readBlock(f, b + 0x104, Kind::Vec3);
        e.alpha = readBlock(f, b + 0x114, Kind::Fixed16);
        e.scale = readBlock(f, b + 0x124, Kind::Vec2);
        e.scaleVary = glm::vec2(f.f32(b + 0x134), f.f32(b + 0x138));
        e.headCell = readBlock(f, b + 0x13C, Kind::U16);
        e.tailCell = readBlock(f, b + 0x14C, Kind::U16);
        e.tailLength = f.f32(b + 0x15C);
        e.twinkleSpeed = f.f32(b + 0x160);
        e.twinklePercent = f.f32(b + 0x164);
        e.twinkleMin = f.f32(b + 0x168);
        e.twinkleMax = f.f32(b + 0x16C);
        e.burst = f.f32(b + 0x170);
        e.drag = f.f32(b + 0x174);
        e.baseSpin = f.f32(b + 0x178);
        e.baseSpinVary = f.f32(b + 0x17C);
        e.spin = f.f32(b + 0x180);
        e.spinVary = f.f32(b + 0x184);
        e.tumbleMin = f.v3(b + 0x188);
        e.tumbleMax = f.v3(b + 0x194);
        e.wind = f.v3(b + 0x1A0);
        e.windTime = f.f32(b + 0x1AC);
        e.followSpeed1 = f.f32(b + 0x1B0);
        e.followScale1 = f.f32(b + 0x1B4);
        e.followSpeed2 = f.f32(b + 0x1B8);
        e.followScale2 = f.f32(b + 0x1BC);
        uint32_t ns = f.u32(b + 0x1C0), os = f.u32(b + 0x1C4);
        if (ns < 4096 && f.inside(os, uint64_t(ns) * 12)) {
            e.spline.resize(ns);
            for (uint32_t i = 0; i < ns; ++i) e.spline[i] = f.v3(os + 12 * uint64_t(i));
        }
        e.enabledIn = readTrack(f, b + 0x1C8, Kind::U8);
    }

    static std::vector<uint16_t> readU16Array(const Reader& f, uint64_t o) {
        std::vector<uint16_t> v;
        uint32_t n = f.u32(o), ofs = f.u32(o + 4);
        if (n < 256 && f.inside(ofs, uint64_t(n) * 2)) {
            v.resize(n);
            for (uint32_t i = 0; i < n; ++i) v[i] = f.u16(ofs + 2 * uint64_t(i));
        }
        return v;
    }

    static void readRibbon(const Reader& f, uint32_t b, Ribbon& r) {
        r.base = b;
        r.id = f.i32(b + 0x00);
        r.bone = f.u32(b + 0x04);
        r.position = f.v3(b + 0x08);
        r.textures = readU16Array(f, b + 0x14);
        r.materials = readU16Array(f, b + 0x1C);
        r.color = readTrack(f, b + 0x24, Kind::Vec3);
        r.alpha = readTrack(f, b + 0x38, Kind::Fixed16);
        r.above = readTrack(f, b + 0x4C, Kind::F32);
        r.below = readTrack(f, b + 0x60, Kind::F32);
        r.edgesPerSecond = f.f32(b + 0x74);
        r.edgeLifetime = f.f32(b + 0x78);
        r.gravity = f.f32(b + 0x7C);
        r.rows = f.u16(b + 0x80);
        r.cols = f.u16(b + 0x82);
        r.texSlot = readTrack(f, b + 0x84, Kind::U16);
        r.visibility = readTrack(f, b + 0x98, Kind::U8);
        r.priority = f.i16(b + 0xAC);
    }
};

// REC-LUT: lookup-table eligibility
inline bool lutEligible(const Emitter& e) {
    size_t c = e.color.ts.size(), a = e.alpha.ts.size(), s = e.scale.ts.size();
    size_t h = e.headCell.ts.size(), t = e.tailCell.ts.size();
    if (c < 1 || c > 3 || a < 1 || a > 3 || s < 1 || s > 3) return false;
    if (h > 4 || t > 4) return false;
    if (h == 4 && e.headCell.ts[1] != e.headCell.ts[2]) return false;
    if (t == 4 && e.tailCell.ts[1] != e.tailCell.ts[2]) return false;
    if (!(e.lifeVary == 0.0f)) return false;
    if (h == 0 && (e.flags & 0x10000u) && uint32_t(e.rows) * uint32_t(e.cols) > 1u) return false;
    if (!(e.scaleVary.x == 0.0f && e.scaleVary.y == 0.0f)) return false;
    const std::vector<uint16_t>* curves[5] = {&e.color.ts, &e.alpha.ts, &e.scale.ts, &e.headCell.ts, &e.tailCell.ts};
    bool have = false;
    uint16_t mid = 0;
    for (const auto* ts : curves) {
        if (ts->size() == 3 || ts->size() == 4) {
            if (!have) {
                mid = (*ts)[1];
                have = true;
            } else if ((*ts)[1] != mid) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace m2raw
}  // namespace pspconv
}  // namespace wowee
