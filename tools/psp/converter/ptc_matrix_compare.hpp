// psp_particle_matrix --compare: the dense reference against the engine's code over the baked blob and upstream's
// rules, plus a picture. The reference and the engine side are ptc_clean_reference.hpp (built from the level-3
// specs, docs/PSP_PORT_ARCHITECTURE.md); here: upstream's rules (wowee.cfg ptclevel3=0), the pictures and
// the per-model statistics (TSV).
#pragma once

#include "conv_common.hpp"
#include "particle_bake.hpp"
#include "ptc_clean_reference.hpp"

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_model_classifier.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace wowee::pspconv::ptcmatrix {

namespace sim = wowee::rendering::psp::ptcsim;
namespace sfx = wowee::rendering::psp;

// --- the blob as the PSP reads it (copies of psp_particles.cpp's FxView / curve / trackMatrix) -----------------

struct Blob {
    std::vector<uint8_t> bytes;
    const uint8_t* base = nullptr;
    const sfx::SpellFxHeader* h = nullptr;
    const sfx::SpellFxTrack* tracks = nullptr;
    const sfx::SpellFxEmitter* em = nullptr;
    uint8_t cls = 0;
    void set() {
        base = bytes.data();
        h = reinterpret_cast<const sfx::SpellFxHeader*>(base);
        tracks = reinterpret_cast<const sfx::SpellFxTrack*>(base + h->trackOffset);
        em = reinterpret_cast<const sfx::SpellFxEmitter*>(base + h->emitterOffset);
        const sfx::SpellFxHeaderExt* x = sfx::spellFxExt(base);
        cls = x ? x->cls : static_cast<uint8_t>(sfx::kPtcSpell);
    }
};

struct FramePos {
    int k = 0;
    float f = 0.0f;
};
inline FramePos framePos(uint16_t frames, float pos) {
    FramePos fp;
    if (frames <= 1 || !(pos > 0.0f)) return fp;
    const float maxPos = static_cast<float>(frames - 1);
    if (pos >= maxPos) {
        fp.k = frames - 2;
        fp.f = 1.0f;
        return fp;
    }
    fp.k = static_cast<int>(pos);
    fp.f = pos - static_cast<float>(fp.k);
    return fp;
}
inline float curveAt(const uint8_t* base, uint32_t off, uint16_t frames, float pos, bool step = false) {
    const auto* v = reinterpret_cast<const float*>(base + off);
    if (frames <= 1) return v[0];
    const FramePos fp = framePos(frames, pos);
    if (step) return fp.f >= 1.0f ? v[fp.k + 1] : v[fp.k];
    return v[fp.k] + (v[fp.k + 1] - v[fp.k]) * fp.f;
}
inline float curvePos(const sfx::SpellFxHeader& h, float tMs) {
    return std::fmod(tMs, static_cast<float>(h.periodMs)) / static_cast<float>(h.periodMs) * static_cast<float>(h.frames - 1);
}
inline void trackMatrix(const uint8_t* base, const sfx::SpellFxTrack& t, float pos, sim::M34& out) {
    const FramePos fp = framePos(t.frames, pos);
    const int k1 = t.frames > 1 ? fp.k + 1 : fp.k;
    if (t.flags & sfx::kFxTrackMatrix) {
        const auto* keys = reinterpret_cast<const sfx::SpellFxMatrixKey*>(base + t.offset);
        for (int i = 0; i < 9; ++i) out.l[i] = (keys[fp.k].m[i] + (keys[k1].m[i] - keys[fp.k].m[i]) * fp.f) * t.scaleScale;
        for (int i = 0; i < 3; ++i) out.t[i] = (keys[fp.k].t[i] + (keys[k1].t[i] - keys[fp.k].t[i]) * fp.f) * t.translationScale;
        return;
    }
    const auto* keys = reinterpret_cast<const sfx::SpellFxKey*>(base + t.offset);
    const sfx::SpellFxKey& a = keys[fp.k];
    const sfx::SpellFxKey& b = keys[k1];
    float q[4], len = 0.0f;
    for (int i = 0; i < 4; ++i) {
        q[i] = a.q[i] + (b.q[i] - a.q[i]) * fp.f;
        len += q[i] * q[i];
    }
    const float inv = len > 1e-6f ? 1.0f / std::sqrt(len) : 0.0f;
    const float x = q[0] * inv, y = q[1] * inv, z = q[2] * inv, w = len > 1e-6f ? q[3] * inv : 1.0f;
    float s[3];
    for (int i = 0; i < 3; ++i) {
        s[i] = (a.s[i] + (b.s[i] - a.s[i]) * fp.f) * t.scaleScale;
        out.t[i] = (a.t[i] + (b.t[i] - a.t[i]) * fp.f) * t.translationScale;
    }
    out.l[0] = (1.0f - 2.0f * (y * y + z * z)) * s[0];
    out.l[1] = (2.0f * (x * y + w * z)) * s[0];
    out.l[2] = (2.0f * (x * z - w * y)) * s[0];
    out.l[3] = (2.0f * (x * y - w * z)) * s[1];
    out.l[4] = (1.0f - 2.0f * (x * x + z * z)) * s[1];
    out.l[5] = (2.0f * (y * z + w * x)) * s[1];
    out.l[6] = (2.0f * (x * z + w * y)) * s[2];
    out.l[7] = (2.0f * (y * z - w * x)) * s[2];
    out.l[8] = (1.0f - 2.0f * (x * x + y * y)) * s[2];
}

// --- one instance at the origin: upstream's rules (the reference and the engine side: ptc_clean_reference) -----

struct Snap {  // one particle at one frame, for the comparison and the pictures
    int serial, emitter;
    glm::vec3 pos, vel;
    float half[2], rgba[4];
    bool visible;
};

// Upstream's rules (ptclevel3=0, as psp_particles.cpp runs them) over the blob's fields without the level-3 section.
struct UpSim {
    const Blob* B = nullptr;
    struct P {
        glm::vec3 pos, vel;
        float life, maxLife;
        int emitter;
    };
    std::vector<P> parts;
    std::vector<float> acc;
    sim::Rng rng;
    void init(const Blob& b, uint32_t seed) {
        B = &b;
        acc.assign(b.h->emitterCount, 0.0f);
        rng.seed(seed);
    }
    void frame(float tMs, float dt) {
        const float pos = curvePos(*B->h, tMs);
        const bool firefly = B->cls & sfx::kPtcFirefly, flame = B->cls & sfx::kPtcFlame;
        for (int ei = 0; ei < B->h->emitterCount; ++ei) {
            const sfx::SpellFxEmitter& e = B->em[ei];
            auto cv = [&](int k) { return curveAt(B->base, e.curveOffset[k], e.curveFrames[k], pos); };
            float rate = cv(sfx::kFxCurveRate);
            const float life = cv(sfx::kFxCurveLife);
            if (flame && rate > 0.0f && life > 0.0f) rate = std::max(rate, 15.0f / std::max(life, 0.1f));
            if (rate <= 0.0f || life <= 0.0f) continue;
            acc[ei] += rate * dt;
            sim::M34 bone, world;
            if (e.track != sfx::kFxNone) trackMatrix(B->base, B->tracks[e.track], pos, bone);
            else sim::identity(bone);
            const float axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            sim::compose(axes, glm::vec3(0.0f), bone, world);
            while (acc[ei] >= 1.0f) {
                acc[ei] -= 1.0f;
                P p{};
                p.emitter = ei;
                p.maxLife = life;
                p.life = 0.0f;
                p.pos = sim::apply(world, glm::vec3(e.position[0], e.position[1], e.position[2]));
                const float speed = cv(sfx::kFxCurveSpeed), vr = cv(sfx::kFxCurveVRange), hr = cv(sfx::kFxCurveHRange);
                glm::vec3 dir(rng.sym() * hr, rng.sym() * hr, 1.0f + rng.sym() * vr);
                const float l2 = glm::dot(dir, dir);
                if (l2 > 1e-6f) dir *= 1.0f / std::sqrt(l2);
                p.vel = sim::rotate(world, dir) * speed;
                if (std::fabs(speed) < 0.01f)
                    p.vel = firefly ? sim::rotate(world, glm::vec3(rng.sym() * 0.6f, rng.sym() * 0.6f, rng.sym() * 0.3f))
                                    : sim::rotate(world, glm::vec3(rng.sym(), rng.sym(), -rng.u01() * 0.5f));
                parts.push_back(p);
            }
            if (acc[ei] > 2.0f) acc[ei] = 0.0f;
        }
        for (size_t i = 0; i < parts.size();) {
            P& p = parts[i];
            p.life += dt;
            if (p.life >= p.maxLife) {
                parts[i] = parts.back();
                parts.pop_back();
                continue;
            }
            const sfx::SpellFxEmitter& e = B->em[p.emitter];
            float g = curveAt(B->base, e.curveOffset[sfx::kFxCurveGravity], e.curveFrames[sfx::kFxCurveGravity], pos);
            if (g == 0.0f && !firefly)
                g = std::fabs(curveAt(B->base, e.curveOffset[sfx::kFxCurveSpeed], e.curveFrames[sfx::kFxCurveSpeed], pos)) > 0.1f ? 4.0f
                                                                                                                         : 1.5f;
            p.vel.z -= g * dt;
            p.pos += p.vel * dt;
            ++i;
        }
    }
    void snap(std::vector<Snap>& out) const {
        out.clear();
        const bool spell = B->cls & sfx::kPtcSpell, firefly = B->cls & sfx::kPtcFirefly, flame = B->cls & sfx::kPtcFlame;
        const bool generic = !spell && !firefly && !flame;
        for (const P& p : parts) {
            const sfx::SpellFxEmitter& e = B->em[p.emitter];
            const float ratio = std::clamp(p.life / std::max(p.maxLife, 0.001f), 0.0f, 1.0f);
            const float x = ratio * (sfx::kFxLifeKeys - 1);
            const int kk = std::min(static_cast<int>(x), sfx::kFxLifeKeys - 2);
            const float fr = x - static_cast<float>(kk);
            auto ch = [&](int s) {
                const float a0 = static_cast<float>((e.color[kk] >> s) & 0xFFu), a1 = static_cast<float>((e.color[kk + 1] >> s) & 0xFFu);
                return (a0 + (a1 - a0) * fr) / 255.0f;
            };
            float cr = ch(0), cg = ch(8), cb = ch(16), ca = ch(24);
            const float raw = e.scale[kk] + (e.scale[kk + 1] - e.scale[kk]) * fr;
            if (generic) {
                cr += (1.0f - cr) * 0.7f;
                cg += (1.0f - cg) * 0.7f;
                cb += (1.0f - cb) * 0.7f;
                if (raw > 2.0f) ca *= 0.02f;
                if (e.blend == sfx::kFxBlendAdd) ca *= 0.05f;
            }
            if (flame) {
                cr = std::max(cr, 0.5f);
                cg = std::max(cg, 0.26f);
                cb = std::max(cb, 0.09f);
                ca = std::max(ca, 0.3f);
            }
            float scale = raw;
            if (spell) scale = std::max(raw * 1.5f, 0.15f);
            else if (!firefly) {
                scale = std::min(raw, 1.5f);
                if (flame) scale = std::max(scale, 0.15f);
            }
            Snap s{};
            s.serial = -1;
            s.emitter = p.emitter;
            s.pos = p.pos;
            s.vel = p.vel;
            s.visible = true;
            s.half[0] = s.half[1] = 0.5f * scale * 0.47f;  // the PSP's upstream size (aSize x 500 / d px at 1080)
            s.rgba[0] = cr;
            s.rgba[1] = cg;
            s.rgba[2] = cb;
            s.rgba[3] = ca;
            out.push_back(s);
        }
    }
};

// --- pictures --------------------------------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;  // rgb
    void init(int W, int H, float g) {
        w = W;
        h = H;
        px.assign(static_cast<size_t>(W) * H * 3, g);
    }
    void disc(int x0, int y0, float cx, float cy, float rad, const float rgb[3], float a, int pw, int ph) {
        if (rad < 0.7f) rad = 0.7f;
        const int xa = std::max(0, static_cast<int>(cx - rad - 1)), xb = std::min(pw - 1, static_cast<int>(cx + rad + 1));
        const int ya = std::max(0, static_cast<int>(cy - rad - 1)), yb = std::min(ph - 1, static_cast<int>(cy + rad + 1));
        for (int y = ya; y <= yb; ++y)
            for (int x = xa; x <= xb; ++x) {
                const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) / rad;
                if (d >= 1.0f) continue;
                const float k = a * (1.0f - d * d);
                float* q = &px[(static_cast<size_t>(y0 + y) * w + (x0 + x)) * 3];
                for (int c = 0; c < 3; ++c) q[c] += rgb[c] * k;
            }
    }
    bool savePpm(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w * 3; ++x)
                row[x] = static_cast<uint8_t>(std::clamp(px[static_cast<size_t>(y) * w * 3 + x], 0.0f, 1.0f) * 255.0f + 0.5f);
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        return true;
    }
};

struct View {
    glm::vec3 eye, center;
    float focal;
};

inline void drawPanel(Image& im, int x0, int y0, int pw, int ph, const View& v, const std::vector<Snap>& s) {
    const glm::vec3 fwd = glm::normalize(v.center - v.eye);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
    const glm::vec3 up = glm::cross(right, fwd);
    for (const Snap& p : s) {
        if (!p.visible) continue;
        const glm::vec3 d = p.pos - v.eye;
        const float z = glm::dot(d, fwd);
        if (z < 0.1f) continue;
        const float sx = pw * 0.5f + glm::dot(d, right) / z * v.focal, sy = ph * 0.5f - glm::dot(d, up) / z * v.focal;
        const float rad = std::max(std::fabs(p.half[0]), std::fabs(p.half[1])) / z * v.focal;
        const float rgb[3] = {p.rgba[0], p.rgba[1], p.rgba[2]};
        im.disc(x0, y0, sx, sy, rad, rgb, std::clamp(p.rgba[3], 0.0f, 1.0f) * 0.6f, pw, ph);
    }
    // frame + the emitter origin cross
    for (int x = 0; x < pw; ++x)
        for (int c = 0; c < 3; ++c) {
            im.px[(static_cast<size_t>(y0) * im.w + x0 + x) * 3 + c] = 0.35f;
            im.px[(static_cast<size_t>(y0 + ph - 1) * im.w + x0 + x) * 3 + c] = 0.35f;
        }
    for (int y = 0; y < ph; ++y)
        for (int c = 0; c < 3; ++c) {
            im.px[(static_cast<size_t>(y0 + y) * im.w + x0) * 3 + c] = 0.35f;
            im.px[(static_cast<size_t>(y0 + y) * im.w + x0 + pw - 1) * 3 + c] = 0.35f;
        }
    const glm::vec3 d = -v.eye;
    const float z = glm::dot(d, fwd);
    if (z > 0.1f) {
        const int ox = static_cast<int>(pw * 0.5f + glm::dot(d, right) / z * v.focal);
        const int oy = static_cast<int>(ph * 0.5f - glm::dot(d, up) / z * v.focal);
        for (int k = -4; k <= 4; ++k)
            for (int c = 0; c < 3; ++c) {
                if (ox + k > 0 && ox + k < pw && oy > 0 && oy < ph)
                    im.px[(static_cast<size_t>(y0 + oy) * im.w + x0 + ox + k) * 3 + c] = c == 1 ? 0.8f : 0.2f;
                if (oy + k > 0 && oy + k < ph && ox > 0 && ox < pw)
                    im.px[(static_cast<size_t>(y0 + oy + k) * im.w + x0 + ox) * 3 + c] = c == 1 ? 0.8f : 0.2f;
            }
    }
}

struct CompareStats {
    std::string model;
    int emitters = 0, refEmitters = 0;
    size_t refBorn = 0, pspBorn = 0, upBorn = 0, matched = 0;
    double posErrMax = 0, posErrSum = 0, velErrMax = 0, halfErrMax = 0, rgbaErrMax = 0, visDiff = 0;
    double refHalfMean = 0, upHalfMean = 0, refVz = 0, upVz = 0, refHoriz = 0, upHoriz = 0, refLive = 0, upLive = 0;
    size_t frames = 0, refN = 0, upN = 0;
    std::string why;
};

}  // namespace wowee::pspconv::ptcmatrix
