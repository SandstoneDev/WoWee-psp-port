// psp_glue_scene_conv - the character-select backgrounds of the original client
// (Interface\Glues\Models\UI_<Race>\UI_<Race>.m2) as pictures for the PSP
// (rendered here once per scene; the PSP draws the character over them).
//
//   psp_glue_scene_conv --extracted <dir> [--extracted <dir2>]... --out <dir> [--scene human]...
//                       [--ss 3] [--time 0] [--anchor-x 150] [--list-missing <file>]
//
// CharacterSelect is a ModelFFX: SetBackgroundModel (GlueParent.lua) loads the
// scene M2 named by GetSelectBackgroundModel, SetLighting sets sequence 0,
// camera 0, the race's fog (CharModelFogInfo) and directional lights
// (RaceLights); the character stands in it. On the PSP the scene is not drawn
// live: this tool renders it here, once per scene, through camera 0 with those
// lights and fog, into a 480 x 272 picture; build_glue_pak.py packs the
// pictures into glue.pak (`cs_<scene>`) with the camera (`cs_scenes`), and the
// PSP draws the character over the picture through the same camera. Why a
// picture and not the M2 on the GE: at 480 x 272 a still frame differs little from the live scene
// and looks better than the GE would draw it.
//
// The scene is posed and lit with upstream's rules where upstream has them:
// M2Loader (pipeline), m2_track sampling, CharacterRenderer::getBoneTransform
// (T(pivot) T R S T(-pivot), parents first), batch order (opaque first, then by
// priorityPlane / materialLayer), the batch colour x transparency gate, no back
// face culling, CharacterPreview::loadRacialBackdrop (attachment 0 = where the
// character stands, camera 0). Upstream does not draw these scenes through
// their own camera, lights or fog; those follow the client (GlueParent.lua
// SetLighting) and the M2 format (wowdev.wiki M2, Cameras): the stored FoV is
// diagonal, vertical = diagonal / sqrt(1 + aspect^2) on the 480 x 272 screen, spherical
// billboard bones, texture-coordinate combos, M2 point lights, blend modes.
// Not drawn: particles and ribbons (the PSP draws none either).
//
// Output per scene: <out>/cs_<scene>.png (480 x 272, RGB) and one line in
// <out>/cs_scenes.txt:
//   <scene> eye x y z target x y z fovy <rad> near <> far <> stand x y z yaw <rad> shiftx <px>
//   fog r g b far (far 0 = none) tint r g b
// (the character pass on the PSP uses the same camera, shift and fog; tint = its light).
#include "conv_common.hpp"

#include "pipeline/blp_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace wowee;
using namespace wowee::pspconv;

namespace {

constexpr int kScreenW = 480, kScreenH = 272;

// ---------------------------------------------------------------------------
// GlueParent.lua (3.3.5a ruRU): RaceLights, CharModelFogInfo. A light is
// {enabled, omni, dirX, dirY, dirZ, ambI, ambR, ambG, ambB, dirI, dirR, dirG, dirB}.
// ---------------------------------------------------------------------------
struct LuaLight {
    float dir[3];
    float ambI, amb[3];
    float dirI, col[3];
};

struct SceneDef {
    const char* name;      // GetSelectBackgroundModel: "UI_" .. name
    float fog[4];          // r, g, b, far (far 0 = ClearFog)
    std::vector<LuaLight> lights;
};

const std::vector<SceneDef>& sceneDefs() {
    static const std::vector<SceneDef> defs = {
        {"Human", {0.8f, 0.65f, 0.73f, 222.0f},
         {{{0.0f, 0.0f, -1.0f}, 1.0f, {0.27f, 0.27f, 0.27f}, 1.0f, {0, 0, 0}},
          {{-0.45756075f, -0.58900136f, -0.66611975f}, 1.0f, {0, 0, 0}, 1.0f, {0.19882353f, 0.34921569f, 0.43588236f}},
          {{-0.64623469f, 0.57582057f, -0.50081086f}, 1.0f, {0, 0, 0}, 2.0f, {0.52196085f, 0.44f, 0.29764709f}}}},
        {"Orc", {0.5f, 0.5f, 0.5f, 270.0f},
         {{{0.0f, 0.0f, -1.0f}, 1.0f, {0.15f, 0.15f, 0.15f}, 1.0f, {0, 0, 0}},
          {{-0.74919f, 0.35208f, -0.56103f}, 1.0f, {0, 0, 0}, 1.0f, {0.44706f, 0.54510f, 0.73725f}},
          {{0.53162f, -0.84340f, 0.07780f}, 1.0f, {0, 0, 0}, 2.0f, {0.55f, 0.338625f, 0.148825f}}}},
        {"Dwarf", {0.85f, 0.88f, 1.0f, 500.0f},
         {{{0.0f, 0.0f, -1.0f}, 1.0f, {0.3f, 0.3f, 0.3f}, 0.0f, {0, 0, 0}},
          {{-0.88314f, 0.42916f, -0.18945f}, 1.0f, {0, 0, 0}, 2.0f, {0.44706f, 0.67451f, 0.760785f}}}},
        {"Tauren", {1.0f, 0.61f, 0.42f, 153.0f},
         {{{-0.48073f, 0.71827f, -0.50297f}, 1.0f, {0, 0, 0}, 2.0f, {0.65f, 0.397645f, 0.2727f}},
          {{-0.49767f, -0.78677f, 0.36513f}, 1.0f, {0, 0, 0}, 1.0f, {0.6f, 0.47059f, 0.32471f}}}},
        {"Scourge", {0.0f, 0.22f, 0.22f, 26.0f},
         {{{0.0f, 0.0f, -1.0f}, 1.0f, {0.2f, 0.2f, 0.2f}, 1.0f, {0, 0, 0}}}},
        {"NightElf", {0.25f, 0.22f, 0.55f, 611.0f},
         {{{0.0f, 0.0f, -1.0f}, 1.0f, {0.0902f, 0.0902f, 0.1702f}, 1.0f, {0, 0, 0}}}},
        {"Draenei", {0, 0, 0, 0},
         {{{0.61185f, 0.62942f, -0.47903f}, 1.0f, {0, 0, 0}, 1.0f, {0.56941f, 0.52f, 0.6f}},
          {{-0.64345f, -0.31052f, -0.69968f}, 1.0f, {0, 0, 0}, 1.0f, {0.60941f, 0.60392f, 0.7f}},
          {{-0.46481f, -0.14320f, 0.87376f}, 1.0f, {0, 0, 0}, 2.0f, {0.5835f, 0.48941f, 0.6f}}}},
        {"BloodElf", {0, 0, 0, 0},
         {{{-0.82249f, -0.54912f, -0.14822f}, 1.0f, {0, 0, 0}, 2.0f, {0.581175f, 0.50588f, 0.42588f}},
          {{0.0f, 0.0f, -1.0f}, 1.0f, {0.60392f, 0.6149f, 0.7f}, 1.0f, {0, 0, 0}},
          {{0.02575f, 0.86518f, -0.50081f}, 1.0f, {0, 0, 0}, 1.0f, {0.59137f, 0.51745f, 0.63471f}}}},
        {"DeathKnight", {0, 0, 0, 0},
         {{{0.0f, 0.0f, -1.0f}, 1.0f, {0.38824f, 0.66353f, 0.76941f}, 1.0f, {0, 0, 0}}}},
        {"CharacterSelect", {0.8f, 0.65f, 0.73f, 222.0f},
         {{{0.0f, 0.0f, -1.0f}, 1.0f, {0.15f, 0.15f, 0.15f}, 1.0f, {0, 0, 0}},
          {{-0.74919f, 0.35208f, -0.56103f}, 1.0f, {0, 0, 0}, 1.0f, {0.44706f, 0.54510f, 0.73725f}},
          {{0.53162f, -0.84340f, 0.07780f}, 1.0f, {0, 0, 0}, 2.0f, {0.55f, 0.338625f, 0.148825f}}}},
    };
    return defs;
}

// ---------------------------------------------------------------------------
// Raw M2 reads the upstream loader does not keep (texture-coordinate combos,
// lights, the camera's position / target tracks at sequence 0).
// ---------------------------------------------------------------------------
struct RawM2 {
    const std::vector<uint8_t>& d;
    uint32_t u32(size_t o) const {
        if (o + 4 > d.size()) return 0;
        uint32_t v;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    }
    int16_t s16(size_t o) const {
        if (o + 2 > d.size()) return 0;
        int16_t v;
        std::memcpy(&v, d.data() + o, 2);
        return v;
    }
    float f32(size_t o) const {
        if (o + 4 > d.size()) return 0.0f;
        float v;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    }
    glm::vec3 v3(size_t o) const { return {f32(o), f32(o + 4), f32(o + 8)}; }
    // M2Track (3.3.5a, 20 bytes): u16 interp, s16 globalSeq, M2Array<M2Array<u32>> times,
    // M2Array<M2Array<T>> values. The first key of sequence 0 (`ok` = there is one).
    size_t firstKey(size_t track, bool& ok) const {
        ok = false;
        const uint32_t nSeq = u32(track + 12), ofsSeq = u32(track + 16);
        if (nSeq == 0 || ofsSeq == 0) return 0;
        const uint32_t n = u32(ofsSeq), ofs = u32(ofsSeq + 4);
        if (n == 0 || ofs == 0 || ofs >= d.size()) return 0;
        ok = true;
        return ofs;
    }
    glm::vec3 trackVec3(size_t track, glm::vec3 def) const {
        bool ok;
        const size_t o = firstKey(track, ok);
        return ok ? v3(o) : def;
    }
    float trackFloat(size_t track, float def) const {
        bool ok;
        const size_t o = firstKey(track, ok);
        return ok ? f32(o) : def;
    }
};

struct PointLight {
    int bone = -1;
    glm::vec3 pos{0.0f};
    glm::vec3 amb{0.0f}, diff{0.0f};
    float attStart = 0.0f, attEnd = 0.0f;
    int type = 1;
};

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
struct Tex {
    int w = 0, h = 0;
    std::vector<float> rgba;  // 0..1
    bool wrapU = true, wrapV = true;
    bool valid() const { return w > 0; }
    glm::vec4 sample(float u, float v) const {
        if (!valid()) return glm::vec4(1.0f);
        auto wrap = [](float t, bool rep) {
            if (rep) return t - std::floor(t);
            return std::clamp(t, 0.0f, 1.0f);
        };
        u = wrap(u, wrapU) * w - 0.5f;
        v = wrap(v, wrapV) * h - 0.5f;
        const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
        const float fx = u - x0, fy = v - y0;
        auto at = [&](int x, int y) {
            x = wrapU ? ((x % w) + w) % w : std::clamp(x, 0, w - 1);
            y = wrapV ? ((y % h) + h) % h : std::clamp(y, 0, h - 1);
            const float* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            return glm::vec4(p[0], p[1], p[2], p[3]);
        };
        return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx), glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx),
                        fy);
    }
};

// The extracted trees, searched in order (the scenes take textures from all over the client:
// Environments\Stars, Spells, World\...; --list-missing names what is not there yet).
std::vector<std::string> gRoots;
std::vector<std::string> gMissing;

bool readAny(const std::string& gamePath, std::vector<uint8_t>& out) {
    for (const auto& r : gRoots)
        if (readFile(r + "/" + lower(gamePath), out)) return true;
    if (std::find(gMissing.begin(), gMissing.end(), gamePath) == gMissing.end()) gMissing.push_back(gamePath);
    return false;
}

bool loadTex(const std::string& gamePath, uint32_t flags, Tex& t) {
    std::vector<uint8_t> bytes;
    if (!readAny(gamePath, bytes)) return false;
    const pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
    if (!img.isValid() || img.data.empty()) return false;
    t.w = img.width;
    t.h = img.height;
    t.rgba.resize(static_cast<size_t>(t.w) * t.h * 4);
    for (size_t i = 0; i < t.rgba.size(); ++i) t.rgba[i] = img.data[i] / 255.0f;
    t.wrapU = (flags & 1u) != 0;
    t.wrapV = (flags & 2u) != 0;
    return true;
}

// ---------------------------------------------------------------------------
// Camera and framebuffer
// ---------------------------------------------------------------------------
struct Camera {
    glm::vec3 eye{0.0f}, target{0.0f};
    float fovy = 0.6f;  // radians
    float nearZ = 0.1f, farZ = 1000.0f;
    float shiftX = 0.0f;  // pixels on the 480 x 272 screen: the picture moves right by this
    glm::mat4 view{1.0f};
    float aspect = static_cast<float>(kScreenW) / kScreenH;
};

struct ClipVert {
    glm::vec4 clip;      // clip-space position
    glm::vec2 uv[2];
    glm::vec3 light;     // lit vertex colour (before the batch colour)
    float fog = 0.0f;    // 0 = none, 1 = all fog
};

struct Frame {
    int w, h, ss;
    std::vector<glm::vec3> color;
    std::vector<float> depth;
    Frame(int ss_) : w(kScreenW * ss_), h(kScreenH * ss_), ss(ss_), color(w * h, glm::vec3(0.0f)), depth(w * h, 1.0f) {}
};

struct DrawState {
    const Tex* tex[2] = {nullptr, nullptr};
    int texCount = 1;
    int uvSet[2] = {0, 0};
    glm::mat3 uvMat[2] = {glm::mat3(1.0f), glm::mat3(1.0f)};
    glm::vec4 color{1.0f};     // batch colour x transparency
    uint16_t blend = 0;
    bool depthTest = true, depthWrite = true;
    glm::vec3 fogColor{0.0f};
    bool fogged = false;
};

glm::vec3 fogTarget(const DrawState& s) {
    switch (s.blend) {
        case 3: case 4: case 7: return glm::vec3(0.0f);   // additive: fog fades the light out
        case 5: return glm::vec3(1.0f);                    // modulate: fades to no change
        case 6: return glm::vec3(0.5f);                    // modulate 2x: neutral grey
        default: return s.fogColor;
    }
}

void shadeAndBlend(Frame& f, int idx, float z, const DrawState& s, glm::vec2 uv0, glm::vec2 uv1, glm::vec3 light,
                   float fog) {
    if (s.depthTest && z > f.depth[idx]) return;
    const glm::vec2 uvs[2] = {uv0, uv1};
    glm::vec4 c(1.0f);
    for (int t = 0; t < s.texCount; ++t) {
        glm::vec3 uvh = s.uvMat[t] * glm::vec3(uvs[s.uvSet[t] & 1], 1.0f);
        const glm::vec4 tc = s.tex[t] ? s.tex[t]->sample(uvh.x, uvh.y) : glm::vec4(1.0f);
        if (t == 0) c = tc;
        else c = glm::vec4(glm::vec3(c) * glm::vec3(tc) * 2.0f, c.a);  // second layer: Mod2x combiner
    }
    c *= s.color;
    c = glm::vec4(glm::vec3(c) * light, c.a);
    c = glm::vec4(glm::min(glm::vec3(c), glm::vec3(1.0f)), c.a);
    if (s.fogged && fog > 0.0f) c = glm::vec4(glm::mix(glm::vec3(c), fogTarget(s), fog), c.a);
    glm::vec3& dst = f.color[idx];
    switch (s.blend) {
        case 0: dst = glm::vec3(c); break;
        case 1:
            if (c.a < 224.0f / 255.0f) return;  // alpha key
            dst = glm::vec3(c);
            break;
        case 2:
            if (c.a < 1.0f / 255.0f) return;
            dst = glm::mix(dst, glm::vec3(c), c.a);
            break;
        case 3: dst += glm::vec3(c); break;                     // NoAlphaAdd
        case 4: dst += glm::vec3(c) * c.a; break;               // Add
        case 5: dst *= glm::vec3(c); break;                     // Mod
        case 6: dst = dst * glm::vec3(c) * 2.0f; break;         // Mod2x
        case 7: dst = glm::vec3(c) + dst * (1.0f - c.a); break; // BlendAdd
        default: dst = glm::mix(dst, glm::vec3(c), c.a); break;
    }
    dst = glm::min(dst, glm::vec3(1.0f));
    if (s.depthWrite && s.blend <= 1) f.depth[idx] = z;
}

ClipVert lerpV(const ClipVert& a, const ClipVert& b, float t) {
    ClipVert r;
    r.clip = glm::mix(a.clip, b.clip, t);
    r.uv[0] = glm::mix(a.uv[0], b.uv[0], t);
    r.uv[1] = glm::mix(a.uv[1], b.uv[1], t);
    r.light = glm::mix(a.light, b.light, t);
    r.fog = a.fog + (b.fog - a.fog) * t;
    return r;
}

// Sutherland-Hodgman against w > near (clip.z >= -clip.w for an OpenGL projection).
std::vector<ClipVert> clipNear(const std::vector<ClipVert>& in) {
    std::vector<ClipVert> out;
    auto inside = [](const ClipVert& v) { return v.clip.z >= -v.clip.w; };
    for (size_t i = 0; i < in.size(); ++i) {
        const ClipVert& a = in[i];
        const ClipVert& b = in[(i + 1) % in.size()];
        const bool ia = inside(a), ib = inside(b);
        if (ia) out.push_back(a);
        if (ia != ib) {
            const float da = a.clip.z + a.clip.w, db = b.clip.z + b.clip.w;
            out.push_back(lerpV(a, b, da / (da - db)));
        }
    }
    return out;
}

void rasterTri(Frame& f, const Camera& cam, const ClipVert& v0, const ClipVert& v1, const ClipVert& v2,
               const DrawState& s) {
    std::vector<ClipVert> poly = clipNear({v0, v1, v2});
    if (poly.size() < 3) return;
    struct SV {
        float x, y, z, invw;
        glm::vec2 uv0, uv1;
        glm::vec3 light;
        float fog;
    };
    std::vector<SV> sv;
    for (const ClipVert& v : poly) {
        const float iw = 1.0f / v.clip.w;
        SV o;
        o.x = (v.clip.x * iw * 0.5f + 0.5f) * f.w + cam.shiftX * f.ss;
        o.y = (1.0f - (v.clip.y * iw * 0.5f + 0.5f)) * f.h;
        o.z = v.clip.z * iw;
        o.invw = iw;
        o.uv0 = v.uv[0] * iw;
        o.uv1 = v.uv[1] * iw;
        o.light = v.light * iw;
        o.fog = v.fog * iw;
        sv.push_back(o);
    }
    for (size_t k = 1; k + 1 < sv.size(); ++k) {
        const SV& a = sv[0];
        const SV& b = sv[k];
        const SV& c = sv[k + 1];
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-8f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(f.w - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(f.h - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        if (x0 > x1 || y0 > y1) continue;
        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y) {
            const float py = y + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f;
                float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
                float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = w0 * a.z + w1 * b.z + w2 * c.z;
                if (z < -1.0f || z > 1.0f) continue;
                const float iw = w0 * a.invw + w1 * b.invw + w2 * c.invw;
                if (iw <= 0) continue;
                const float rw = 1.0f / iw;
                const glm::vec2 uv0 = (a.uv0 * w0 + b.uv0 * w1 + c.uv0 * w2) * rw;
                const glm::vec2 uv1 = (a.uv1 * w0 + b.uv1 * w1 + c.uv1 * w2) * rw;
                const glm::vec3 light = (a.light * w0 + b.light * w1 + c.light * w2) * rw;
                const float fog = (a.fog * w0 + b.fog * w1 + c.fog * w2) * rw;
                shadeAndBlend(f, y * f.w + x, z, s, uv0, uv1, light, fog);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Posing (upstream CharacterRenderer::getBoneTransform / calculateBoneMatrices)
// ---------------------------------------------------------------------------
glm::mat4 boneLocal(const pipeline::M2Bone& bone, float t, const std::vector<uint32_t>& gsd) {
    namespace tr = rendering::m2_track;
    const glm::vec3 translation = tr::sampleVec3(bone.translation, 0, t, t, gsd, glm::vec3(0.0f));
    const glm::quat rotation = tr::sampleQuat(bone.rotation, 0, t, t, gsd);
    const glm::vec3 scale = tr::sampleVec3(bone.scale, 0, t, t, gsd, glm::vec3(1.0f));
    const glm::mat3 R = glm::mat3_cast(rotation);
    const glm::vec3 c0 = R[0] * scale.x, c1 = R[1] * scale.y, c2 = R[2] * scale.z;
    const glm::vec3 tt = (bone.pivot + translation) - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z);
    glm::mat4 m;
    m[0] = glm::vec4(c0, 0.0f);
    m[1] = glm::vec4(c1, 0.0f);
    m[2] = glm::vec4(c2, 0.0f);
    m[3] = glm::vec4(tt, 1.0f);
    return m;
}

std::vector<glm::mat4> poseBones(const pipeline::M2Model& m, float t, const Camera& cam, int& billboards) {
    std::vector<glm::mat4> out(m.bones.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const auto& b = m.bones[i];
        const glm::mat4 local = boneLocal(b, t, m.globalSequenceDurations);
        glm::mat4 world = (b.parentBone >= 0 && static_cast<size_t>(b.parentBone) < i) ? out[b.parentBone] * local
                                                                                       : local;
        // M2 bone flag 0x8: spherical billboard - the bone's +X turns to the camera, +Z stays up
        // on the screen (the cards are authored in the YZ plane facing +X).
        if (b.flags & 0x8u) {
            ++billboards;
            const glm::vec3 p = glm::vec3(world * glm::vec4(b.pivot, 1.0f));
            const glm::vec3 sx(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                               glm::length(glm::vec3(world[2])));
            glm::vec3 X = glm::normalize(cam.eye - p);
            glm::vec3 camUp = glm::normalize(glm::vec3(glm::inverse(cam.view) * glm::vec4(0, 1, 0, 0)));
            glm::vec3 Z = glm::normalize(camUp - X * glm::dot(camUp, X));
            glm::vec3 Y = glm::cross(Z, X);
            glm::mat4 r(1.0f);
            r[0] = glm::vec4(X * sx.x, 0.0f);
            r[1] = glm::vec4(Y * sx.y, 0.0f);
            r[2] = glm::vec4(Z * sx.z, 0.0f);
            r[3] = glm::vec4(p - glm::mat3(r) * b.pivot, 1.0f);
            world = r;
        }
        out[i] = world;
    }
    return out;
}

// M2 texture transform: T(translation) T(0.5) R S T(-0.5) on (u, v).
glm::mat3 uvTransform(const pipeline::M2Model& m, uint16_t animIndex, float t) {
    namespace tr = rendering::m2_track;
    if (animIndex == 0xFFFF || animIndex >= m.textureTransformLookup.size()) return glm::mat3(1.0f);
    const uint16_t ti = m.textureTransformLookup[animIndex];
    if (ti == 0xFFFF || ti >= m.textureTransforms.size()) return glm::mat3(1.0f);
    const auto& tt = m.textureTransforms[ti];
    const auto& gsd = m.globalSequenceDurations;
    const glm::vec3 tr3 = tr::sampleVec3(tt.translation, 0, t, t, gsd, glm::vec3(0.0f));
    const glm::quat q = tr::sampleQuat(tt.rotation, 0, t, t, gsd);
    const glm::vec3 sc = tr::sampleVec3(tt.scale, 0, t, t, gsd, glm::vec3(1.0f));
    const glm::mat3 r3 = glm::mat3_cast(q);
    glm::mat3 R(1.0f);  // the 2D part of the rotation (about z)
    R[0][0] = r3[0][0];
    R[0][1] = r3[0][1];
    R[1][0] = r3[1][0];
    R[1][1] = r3[1][1];
    glm::mat3 S(1.0f);
    S[0][0] = sc.x;
    S[1][1] = sc.y;
    glm::mat3 T(1.0f), C(1.0f), Ci(1.0f);
    T[2] = glm::vec3(tr3.x, tr3.y, 1.0f);
    C[2] = glm::vec3(0.5f, 0.5f, 1.0f);
    Ci[2] = glm::vec3(-0.5f, -0.5f, 1.0f);
    return T * C * R * S * Ci;
}

struct Options {
    std::string out, listMissing;
    std::vector<std::string> scenes;
    int ss = 3;
    float timeMs = 0.0f;
    float anchorX = 150.0f;  // PSP: the character's feet land on this screen x (left of the character list)
};

std::string lowerName(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool renderScene(const Options& o, const SceneDef& def, FILE* meta) {
    const std::string folder = "interface/glues/models/ui_" + lowerName(def.name);
    const std::string m2Path = folder + "/ui_" + lowerName(def.name) + ".m2";
    std::vector<uint8_t> m2Bytes, skinBytes;
    if (!readAny(m2Path, m2Bytes)) {
        std::fprintf(stderr, "%s: missing %s\n", def.name, m2Path.c_str());
        return false;
    }
    pipeline::M2Model m = pipeline::M2Loader::load(m2Bytes);
    if (!readAny(folder + "/ui_" + lowerName(def.name) + "00.skin", skinBytes) ||
        !pipeline::M2Loader::loadSkin(skinBytes, m)) {
        std::fprintf(stderr, "%s: no skin\n", def.name);
        return false;
    }
    if (m.cameras.empty()) {
        std::fprintf(stderr, "%s: no camera\n", def.name);
        return false;
    }
    const RawM2 raw{m2Bytes};
    // Texture-coordinate combos (header 0x88 / 0x8C): batch.textureUnit -> UV set (-1 = environment map).
    std::vector<int16_t> texCombos;
    {
        const uint32_t n = raw.u32(0x88), ofs = raw.u32(0x8C);
        for (uint32_t i = 0; i < n && i < 4096; ++i) texCombos.push_back(raw.s16(ofs + i * 2));
    }
    // Lights (header 0x108 / 0x10C), 3.3.5a M2Light: u16 type, s16 bone, vec3 position, then 7 tracks
    // (ambient colour, ambient intensity, diffuse colour, diffuse intensity, attenuation start / end,
    // visibility).
    std::vector<PointLight> plights;
    {
        const uint32_t n = raw.u32(0x108), ofs = raw.u32(0x10C);
        for (uint32_t i = 0; i < n && i < 64; ++i) {
            const size_t b = ofs + i * 156;
            PointLight l;
            l.type = static_cast<uint16_t>(raw.u32(b) & 0xFFFF);
            l.bone = raw.s16(b + 2);
            l.pos = raw.v3(b + 4);
            const size_t tr0 = b + 16;
            l.amb = raw.trackVec3(tr0, glm::vec3(0.0f)) * raw.trackFloat(tr0 + 20, 0.0f);
            l.diff = raw.trackVec3(tr0 + 40, glm::vec3(0.0f)) * raw.trackFloat(tr0 + 60, 0.0f);
            l.attStart = raw.trackFloat(tr0 + 80, 0.0f);
            l.attEnd = raw.trackFloat(tr0 + 100, 0.0f);
            bool vis = true;
            bool ok;
            const size_t vk = raw.firstKey(tr0 + 120, ok);
            if (ok && vk < m2Bytes.size()) vis = m2Bytes[vk] != 0;
            if (vis) plights.push_back(l);
        }
    }

    // Camera 0 (SetCamera(0)): the portrait / scene camera.
    const pipeline::M2Camera* mc = &m.cameras[0];
    for (const auto& c : m.cameras)
        if (c.type == 0) {
            mc = &c;
            break;
        }
    Camera cam;
    cam.eye = mc->positionBase;
    cam.target = mc->targetBase;
    // The M2 camera's FoV is diagonal (wowdev.wiki M2, "Camera field of view"): vertical = dfov / sqrt(1 + aspect^2),
    // aspect of the rect the picture is shown on - the PSP screen, 480 x 272 (0.49 x dfov).
    cam.fovy = mc->fov / std::sqrt(1.0f + cam.aspect * cam.aspect);
    cam.nearZ = mc->nearClip > 0.01f ? mc->nearClip : 0.1f;
    cam.farZ = mc->farClip > cam.nearZ ? mc->farClip : 1000.0f;
    cam.view = glm::lookAt(cam.eye, cam.target, glm::vec3(0.0f, 0.0f, 1.0f));

    int billboards = 0;
    std::vector<glm::mat4> bones = poseBones(m, o.timeMs, cam, billboards);

    // Attachment 0: where the character stands (upstream loadRacialBackdrop).
    glm::vec3 stand = cam.target;
    for (const auto& a : m.attachments)
        if (a.id == 0) {
            stand = a.position;
            if (a.bone < bones.size()) stand = glm::vec3(bones[a.bone] * glm::vec4(a.position, 1.0f));
            break;
        }
    const glm::mat4 proj = glm::perspective(cam.fovy, cam.aspect, cam.nearZ, cam.farZ);
    {
        // PSP: the picture is shifted sideways so that the character's feet stand at anchorX, left of
        // the character list (the original has the list over the scene's right side as well, on a
        // 4:3 / 16:10 screen the scene's centre is free).
        const glm::vec4 c = proj * cam.view * glm::vec4(stand, 1.0f);
        const float sx = (c.x / c.w * 0.5f + 0.5f) * kScreenW;
        cam.shiftX = o.anchorX - sx;
    }

    // Lights: RaceLights (AddLight: the background set) + the model's own point lights.
    glm::vec3 ambient(0.0f);
    std::vector<std::pair<glm::vec3, glm::vec3>> dirs;  // direction the light travels, colour
    for (const auto& l : def.lights) {
        ambient += l.ambI * glm::vec3(l.amb[0], l.amb[1], l.amb[2]);
        const glm::vec3 col = l.dirI * glm::vec3(l.col[0], l.col[1], l.col[2]);
        if (col != glm::vec3(0.0f)) dirs.push_back({glm::normalize(glm::vec3(l.dir[0], l.dir[1], l.dir[2])), col});
    }
    for (auto& l : plights) {
        if (l.bone >= 0 && static_cast<size_t>(l.bone) < bones.size())
            l.pos = glm::vec3(bones[l.bone] * glm::vec4(l.pos, 1.0f));
    }

    // Vertices posed (skinning by the vertex bone indices, weights / 255).
    std::vector<glm::vec3> pos(m.vertices.size()), nrm(m.vertices.size());
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        const auto& v = m.vertices[i];
        glm::vec3 p(0.0f), n(0.0f);
        float wsum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float w = v.boneWeights[k] / 255.0f;
            if (w <= 0.0f || v.boneIndices[k] >= bones.size()) continue;
            const glm::mat4& b = bones[v.boneIndices[k]];
            p += w * glm::vec3(b * glm::vec4(v.position, 1.0f));
            n += w * glm::mat3(b) * v.normal;
            wsum += w;
        }
        if (wsum <= 0.0f) {
            p = v.position;
            n = v.normal;
        }
        pos[i] = p;
        const float nl = glm::length(n);
        nrm[i] = nl > 1e-5f ? n / nl : glm::vec3(0, 0, 1);
    }

    // Textures by M2 texture slot (type 0 = file name in the model).
    std::map<size_t, std::unique_ptr<Tex>> texCache;
    auto texFor = [&](size_t slot) -> const Tex* {
        if (slot >= m.textures.size()) return nullptr;
        auto it = texCache.find(slot);
        if (it != texCache.end()) return it->second.get();
        auto t = std::make_unique<Tex>();
        const auto& mt = m.textures[slot];
        if (mt.type != 0 || mt.filename.empty() || !loadTex(mt.filename, mt.flags, *t)) {
            std::fprintf(stderr, "%s: texture slot %zu type %u '%s' not loaded\n", def.name, slot, mt.type,
                         mt.filename.c_str());
            t.reset();
        }
        const Tex* r = t.get();
        texCache[slot] = std::move(t);
        return r;
    };

    // Batch order: opaque first (pass 0), then the rest; each pass by (priorityPlane, materialLayer)
    // - upstream CharacterRenderer (sortedBatchIndices + two passes).
    std::vector<size_t> order(m.batches.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto& ba = m.batches[a];
        const auto& bb = m.batches[b];
        if (ba.priorityPlane != bb.priorityPlane) return ba.priorityPlane < bb.priorityPlane;
        return ba.materialLayer < bb.materialLayer;
    });

    Frame frame(o.ss);
    // Clear: the fog colour (the scenes are closed by their sky; this only shows through holes).
    const glm::vec3 clearCol = def.fog[3] > 0.0f ? glm::vec3(def.fog[0], def.fog[1], def.fog[2]) : glm::vec3(0.0f);
    std::fill(frame.color.begin(), frame.color.end(), clearCol);
    const glm::mat4 vp = proj * cam.view;
    namespace tr = rendering::m2_track;
    int drawn = 0, skippedAlpha = 0, envMapped = 0;
    std::map<int, int> blendCount;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t bi : order) {
            const auto& b = m.batches[bi];
            const pipeline::M2Material mat =
                b.materialIndex < m.materials.size() ? m.materials[b.materialIndex] : pipeline::M2Material{0, 0};
            if ((pass == 0) != (mat.blendMode == 0)) continue;
            // Batch colour x transparency (upstream evalBatchColorAlpha x evalBatchTextureWeight).
            float alpha = 1.0f;
            glm::vec3 rgb(1.0f);
            if (b.colorIndex != 0xFFFF && b.colorIndex < m.colorAlphaTracks.size()) {
                alpha *= tr::sampleFloat(m.colorAlphaTracks[b.colorIndex], 0, o.timeMs, o.timeMs,
                                         m.globalSequenceDurations, b.colorIndex < m.colorAlphas.size()
                                                                         ? m.colorAlphas[b.colorIndex]
                                                                         : 1.0f);
                if (b.colorIndex < m.colorRGB.size()) rgb = m.colorRGB[b.colorIndex];
            }
            if (b.transparencyIndex < m.textureWeightLookup.size()) {
                const uint16_t ti = m.textureWeightLookup[b.transparencyIndex];
                if (ti != 0xFFFF && ti < m.textureWeightTracks.size())
                    alpha *= tr::sampleFloat(m.textureWeightTracks[ti], 0, o.timeMs, o.timeMs,
                                             m.globalSequenceDurations, 1.0f);
            }
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            if (alpha <= 0.01f) {
                ++skippedAlpha;
                continue;
            }
            DrawState s;
            s.blend = mat.blendMode;
            s.color = glm::vec4(rgb, alpha);
            s.depthTest = (mat.flags & 0x08u) == 0;
            s.depthWrite = (mat.flags & 0x10u) == 0;
            s.fogged = def.fog[3] > 0.0f && (mat.flags & 0x02u) == 0;
            s.fogColor = glm::vec3(def.fog[0], def.fog[1], def.fog[2]);
            const bool unlit = (mat.flags & 0x01u) != 0;
            s.texCount = std::clamp<int>(b.textureCount, 1, 2);
            bool env = false;
            for (int t = 0; t < s.texCount; ++t) {
                const size_t lk = static_cast<size_t>(b.textureIndex) + t;
                s.tex[t] = lk < m.textureLookup.size() ? texFor(m.textureLookup[lk]) : nullptr;
                int combo = 0;
                if (static_cast<size_t>(b.textureUnit) + t < texCombos.size()) combo = texCombos[b.textureUnit + t];
                if (combo < 0) env = true;
                s.uvSet[t] = combo > 0 ? 1 : 0;
                s.uvMat[t] = uvTransform(m, static_cast<uint16_t>(b.textureAnimIndex + (t ? 1 : 0)), o.timeMs);
                if (t == 1 && b.textureAnimIndex == 0xFFFF) s.uvMat[t] = glm::mat3(1.0f);
            }
            if (env) ++envMapped;
            ++blendCount[s.blend];
            ++drawn;
            const uint32_t end = std::min<uint32_t>(b.indexStart + b.indexCount, static_cast<uint32_t>(m.indices.size()));
            for (uint32_t k = b.indexStart; k + 2 < end; k += 3) {
                ClipVert cv[3];
                for (int j = 0; j < 3; ++j) {
                    const uint16_t vi = m.indices[k + j];
                    if (vi >= pos.size()) goto nextTri;
                    const glm::vec3& p = pos[vi];
                    cv[j].clip = vp * glm::vec4(p, 1.0f);
                    cv[j].uv[0] = m.vertices[vi].texCoords[0];
                    cv[j].uv[1] = m.vertices[vi].texCoords[1];
                    if (env) {  // sphere map from the view-space normal
                        const glm::vec3 vn = glm::normalize(glm::mat3(cam.view) * nrm[vi]);
                        cv[j].uv[0] = cv[j].uv[1] = glm::vec2(vn.x * 0.5f + 0.5f, -vn.y * 0.5f + 0.5f);
                    }
                    if (unlit) {
                        cv[j].light = glm::vec3(1.0f);
                    } else {
                        glm::vec3 l = ambient;
                        for (const auto& d : dirs) l += d.second * std::max(0.0f, glm::dot(nrm[vi], -d.first));
                        for (const auto& pl : plights) {
                            glm::vec3 to = pl.pos - p;
                            const float dist = glm::length(to);
                            float att = 1.0f;
                            if (pl.attEnd > pl.attStart && dist > pl.attStart)
                                att = std::clamp(1.0f - (dist - pl.attStart) / (pl.attEnd - pl.attStart), 0.0f, 1.0f);
                            if (dist > 1e-4f) to /= dist;
                            l += pl.amb * att + pl.diff * att * std::max(0.0f, glm::dot(nrm[vi], to));
                        }
                        cv[j].light = glm::min(l, glm::vec3(1.0f));
                    }
                    const float d = glm::length(p - cam.eye);
                    cv[j].fog = def.fog[3] > 0.0f ? std::clamp(d / def.fog[3], 0.0f, 1.0f) : 0.0f;
                }
                rasterTri(frame, cam, cv[0], cv[1], cv[2], s);
            nextTri:;
            }
        }
    }

    // Down-sample (box) and write.
    std::vector<uint8_t> rgb(kScreenW * kScreenH * 3);
    for (int y = 0; y < kScreenH; ++y)
        for (int x = 0; x < kScreenW; ++x) {
            glm::vec3 acc(0.0f);
            for (int sy = 0; sy < o.ss; ++sy)
                for (int sx = 0; sx < o.ss; ++sx) acc += frame.color[(y * o.ss + sy) * frame.w + x * o.ss + sx];
            acc /= static_cast<float>(o.ss * o.ss);
            for (int c = 0; c < 3; ++c)
                rgb[(y * kScreenW + x) * 3 + c] = static_cast<uint8_t>(std::clamp(acc[c], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    const std::string png = o.out + "/cs_" + lowerName(def.name) + ".png";
    stbi_write_png(png.c_str(), kScreenW, kScreenH, 3, rgb.data(), kScreenW * 3);

    // Where the character's feet land, and how many pixels a yard is there (the PSP check).
    const glm::vec4 sc = vp * glm::vec4(stand, 1.0f);
    const float feetX = (sc.x / sc.w * 0.5f + 0.5f) * kScreenW + cam.shiftX;
    const float feetY = (1.0f - (sc.y / sc.w * 0.5f + 0.5f)) * kScreenH;
    const glm::vec4 hc = vp * glm::vec4(stand + glm::vec3(0, 0, 2.0f), 1.0f);
    const float headY = (1.0f - (hc.y / hc.w * 0.5f + 0.5f)) * kScreenH;
    const glm::vec3 toCam = cam.eye - stand;
    const float yaw = std::atan2(toCam.y, toCam.x);
    // The character's light (SetLighting adds the same RaceLights with AddCharacterLight): the PSP
    // character keeps its baked vertex light, so one colour stands for them - the mean light over the
    // side the camera sees (normals toward it and up to 90 degrees off, tilted up), point lights at the
    // chest, each clamped to 1 per channel as the client's vertex colour is.
    glm::vec3 tint(0.0f);
    {
        glm::vec3 f(toCam.x, toCam.y, 0.0f);
        f = glm::length(f) > 1e-4f ? glm::normalize(f) : glm::vec3(1, 0, 0);
        const glm::vec3 side(-f.y, f.x, 0.0f);
        const glm::vec3 chest = stand + glm::vec3(0, 0, 1.0f);
        const float angles[] = {0.0f, 0.785f, -0.785f, 1.571f, -1.571f};
        for (float a : angles) {
            const glm::vec3 nf = glm::normalize(f * std::cos(a) + side * std::sin(a) + glm::vec3(0, 0, 0.4f));
            glm::vec3 l = ambient;
            for (const auto& d : dirs) l += d.second * std::max(0.0f, glm::dot(nf, -d.first));
            for (const auto& pl : plights) {
                glm::vec3 to = pl.pos - chest;
                const float dist = glm::length(to);
                float att = 1.0f;
                if (pl.attEnd > pl.attStart && dist > pl.attStart)
                    att = std::clamp(1.0f - (dist - pl.attStart) / (pl.attEnd - pl.attStart), 0.0f, 1.0f);
                if (dist > 1e-4f) to /= dist;
                l += pl.amb * att + pl.diff * att * std::max(0.0f, glm::dot(nf, to));
            }
            tint += glm::clamp(l, glm::vec3(0.0f), glm::vec3(1.0f));
        }
        tint /= static_cast<float>(sizeof(angles) / sizeof(angles[0]));
    }
    std::printf("%-15s verts %5zu tris %6zu batches %3zu (drawn %3d, alpha 0: %d, env %d) bones %3zu (billboard %d) "
                "tex %2zu lights %zu particles %zu ribbons %zu cams %zu\n",
                def.name, m.vertices.size(), m.indices.size() / 3, m.batches.size(), drawn, skippedAlpha, envMapped,
                m.bones.size(), billboards, m.textures.size(), plights.size(), m.particleEmitters.size(),
                m.ribbonEmitters.size(), m.cameras.size());
    std::printf("    camera fov %.3f (fovy %.1f deg) near %.2f far %.1f eye (%.1f %.1f %.1f) target (%.1f %.1f %.1f)\n",
                mc->fov, glm::degrees(cam.fovy), cam.nearZ, cam.farZ, cam.eye.x, cam.eye.y, cam.eye.z, cam.target.x,
                cam.target.y, cam.target.z);
    std::printf("    stand (%.2f %.2f %.2f) feet on screen (%.0f, %.0f), 2 yd = %.0f px, shift %.0f, "
                "character light (%.2f %.2f %.2f), blends:",
                stand.x, stand.y, stand.z, feetX, feetY, feetY - headY, cam.shiftX, tint.x, tint.y, tint.z);
    for (auto& [k, v] : blendCount) std::printf(" %d:%d", k, v);
    std::printf("\n");
    if (meta) {
        std::fprintf(meta, "%s eye %.4f %.4f %.4f target %.4f %.4f %.4f fovy %.5f near %.4f far %.2f stand %.4f %.4f %.4f "
                           "yaw %.5f shiftx %.2f fog %.4f %.4f %.4f %.2f tint %.4f %.4f %.4f",
                     lowerName(def.name).c_str(), cam.eye.x, cam.eye.y, cam.eye.z, cam.target.x, cam.target.y,
                     cam.target.z, cam.fovy, cam.nearZ, cam.farZ, stand.x, stand.y, stand.z, yaw, cam.shiftX, def.fog[0],
                     def.fog[1], def.fog[2], def.fog[3], tint.x, tint.y, tint.z);
        std::fprintf(meta, "\n");
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--extracted") gRoots.push_back(next());
        else if (a == "--list-missing") o.listMissing = next();
        else if (a == "--out") o.out = next();
        else if (a == "--scene") o.scenes.push_back(lowerName(next()));
        else if (a == "--ss") o.ss = std::clamp(std::atoi(next().c_str()), 1, 8);
        else if (a == "--time") o.timeMs = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--anchor-x") o.anchorX = static_cast<float>(std::atof(next().c_str()));
        else {
            std::fprintf(stderr, "usage: psp_glue_scene_conv --extracted <dir> [--extracted <dir>]... --out <dir> "
                                 "[--scene name]... [--ss 3] [--time ms] [--anchor-x 150] [--list-missing <file>]\n");
            return 2;
        }
    }
    if (gRoots.empty() || o.out.empty()) {
        std::fprintf(stderr, "--extracted and --out are required\n");
        return 2;
    }
    FILE* meta = std::fopen((o.out + "/cs_scenes.txt").c_str(), o.scenes.empty() ? "wb" : "ab");
    int ok = 0, total = 0;
    for (const auto& def : sceneDefs()) {
        if (!o.scenes.empty() &&
            std::find(o.scenes.begin(), o.scenes.end(), lowerName(def.name)) == o.scenes.end())
            continue;
        ++total;
        if (renderScene(o, def, meta)) ++ok;
    }
    if (meta) std::fclose(meta);
    if (!o.listMissing.empty()) {
        // For psp_mpq_extract --names (exact game paths, backslashes as in the model).
        FILE* f = std::fopen(o.listMissing.c_str(), "wb");
        for (const auto& m : gMissing)
            if (f) std::fprintf(f, "%s\n", m.c_str());
        if (f) std::fclose(f);
        std::printf("%zu missing files listed in %s\n", gMissing.size(), o.listMissing.c_str());
    }
    std::printf("%d / %d scenes\n", ok, total);
    return ok == total ? 0 : 1;
}
