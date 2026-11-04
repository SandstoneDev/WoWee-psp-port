// M2 -> PSP model baking shared by psp_model_conv (doodads), psp_char_conv
// (characters) and psp_creature_conv (creatures, M10b): the texture bank
// (BLP -> WTEX T8 + mips), the WMDL writer
// (include/rendering/psp/psp_model_format.hpp) and the stand-pose skinning.
#pragma once

#include "pipeline/blp_loader.hpp"
#include "texture_alpha_policy.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/psp/psp_model_format.hpp"

#include "conv_common.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace wowee::pspconv {

namespace fmt = wowee::rendering::psp;

constexpr uint32_t kKindTexture = 1;  // WTEX
constexpr uint32_t kKindModel = 5;
constexpr uint32_t kKindDoodads = 6;

class TextureBank {
public:
    TextureBank(std::string root, int texMax, std::vector<PakItem>& out)
        : root_(std::move(root)), texMax_(texMax), out_(out) {}

    // Returns the texture id, or -1 when the file is missing or unreadable.
    // Black textures were seen on the wood of a village inn. `opaque` - the material draws without
    // alpha (blend 0: no alpha test, no blending, upstream too), where many textures keep a specular mask in the
    // alpha (a capital city's trims: 56 % of a city support texture's 2x2 blocks at alpha 0, which the alpha-weighted
    // halving turned black). Such a use bakes the picture with alpha 1: the real colours, the whole CLUT for them.
    int get(const std::string& gamePath, bool& hasAlpha, bool opaque = false, bool foliage = false) {
        const std::string path = lower(gamePath);
        const std::string key = opaque ? path + (foliage ? "|foliage-opaque" : "|opaque") : path;
        auto it = ids_.find(key);
        if (it != ids_.end()) {
            hasAlpha = it->second.second;
            return it->second.first;
        }
        int id = -1;
        hasAlpha = false;
        std::vector<uint8_t> bytes;
        if (readFile(root_ + "/" + path, bytes)) {
            pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
            if (!img.data.empty() && img.width > 0 && img.height > 0) {
                prepareMaterialTextureAlpha(img, opaque, foliage);
                id = static_cast<int>(count_++);
                std::vector<uint8_t> blob = bake(img, hasAlpha, texMax_);
                bytes_ += blob.size();
                out_.push_back({"x_" + std::to_string(id), kKindTexture, std::move(blob)});
                if (std::getenv("PSPCONV_TEXLIST"))  // which x_<id> is which file (texture checks)
                    std::fprintf(stderr, "  texture x_%d %s alpha=%d\n", id, key.c_str(), hasAlpha ? 1 : 0);
            }
        }
        if (id < 0) std::fprintf(stderr, "  missing texture %s\n", path.c_str());
        ids_.emplace(key, std::make_pair(id, hasAlpha));
        return id;
    }
    // A picture made by the converter (RGBA8, power-of-two), registered under
    // `key` like a file: the same key returns the same id.
    // texMax > 0 overrides the bank's size limit (forest LOD impostor atlases
    // are 256 x 128: eight 64-texel views).
    int add(const std::string& key, const pipeline::BLPImage& img, int texMax = 0) {
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second.first;
        bool hasAlpha = false;
        const int id = static_cast<int>(count_++);
        std::vector<uint8_t> blob = bake(img, hasAlpha, texMax > 0 ? texMax : texMax_);
        bytes_ += blob.size();
        out_.push_back({"x_" + std::to_string(id), kKindTexture, std::move(blob)});
        ids_.emplace(key, std::make_pair(id, hasAlpha));
        return id;
    }
    size_t count() const { return count_; }
    size_t bytes() const { return bytes_; }

private:
    // Box filter by 2 on the chosen axes, alpha-weighted colour: no dark
    // fringes around leaves.
    static void halve(std::vector<Rgb>& px, int& w, int& h, bool hx, bool hy) {
        const int nw = hx ? w / 2 : w, nh = hy ? h / 2 : h;
        const int fx = w / nw, fy = h / nh;
        std::vector<Rgb> n(static_cast<size_t>(nw) * nh);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                Rgb s{0, 0, 0, 0}, u{0, 0, 0, 0};
                for (int yy = 0; yy < fy; ++yy) {
                    for (int xx = 0; xx < fx; ++xx) {
                        const Rgb& q = px[(y * fy + yy) * w + x * fx + xx];
                        s.r += q.r * q.a;
                        s.g += q.g * q.a;
                        s.b += q.b * q.a;
                        s.a += q.a;
                        u.r += q.r;
                        u.g += q.g;
                        u.b += q.b;
                    }
                }
                // A fully transparent block keeps its plain average (was black: 0 / 0).
                const float inv = s.a > 1e-4f ? 1.0f / s.a : 0.0f;
                const float k = 1.0f / static_cast<float>(fx * fy);
                n[y * nw + x] = s.a > 1e-4f ? Rgb{s.r * inv, s.g * inv, s.b * inv, s.a * k}
                                            : Rgb{u.r * k, u.g * k, u.b * k, 0.0f};
            }
        }
        px.swap(n);
        w = nw;
        h = nh;
    }

    static uint8_t nearest(const Rgb& c, const std::array<Rgb, 256>& pal) {
        int best = 0;
        float bestD = 1e9f;
        for (int i = 0; i < 256; ++i) {
            const float dr = c.r - pal[i].r, dg = c.g - pal[i].g, db = c.b - pal[i].b, da = c.a - pal[i].a;
            const float d = dr * dr + dg * dg + db * db + 2.0f * da * da;
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return static_cast<uint8_t>(best);
    }

    std::vector<uint8_t> bake(const pipeline::BLPImage& img, bool& hasAlpha, int texMax) {
        int w = img.width, h = img.height;
        std::vector<Rgb> px(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < px.size(); ++i) {
            const uint8_t* p = &img.data[i * 4];
            px[i] = Rgb{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
        }
        // Halve until it fits (BLPs are powers of two).
        while (w > texMax || h > texMax) halve(px, w, h, w > texMax, h > texMax);
        // GE storage: T8 rows >= 16 bytes, swizzle blocks 8 rows high.
        while (w < 16 || h < 8) {
            const int nw = w < 16 ? w * 2 : w, nh = h < 8 ? h * 2 : h;
            std::vector<Rgb> n(static_cast<size_t>(nw) * nh);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x) n[y * nw + x] = px[(y * h / nh) * w + x * w / nw];
            px.swap(n);
            w = nw;
            h = nh;
        }
        hasAlpha = false;
        for (const Rgb& p : px) {
            if (p.a < 0.98f) {
                hasAlpha = true;
                break;
            }
        }
        std::vector<uint8_t> idx;
        std::array<Rgb, 256> pal;
        quantize(px, idx, pal);

        std::vector<uint8_t> blob;
        blob.insert(blob.end(), {'W', 'T', 'E', 'X'});
        blob.push_back(5);  // GU_PSM_T8
        blob.push_back(1);  // swizzled
        // Mip levels share the CLUT: 16-byte rows and 8-row blocks at least.
        std::vector<std::vector<uint8_t>> mips;
        {
            std::vector<Rgb> m = px;
            int mw = w, mh = h;
            while (mips.size() < 7 && mw / 2 >= 16 && mh / 2 >= 8) {
                halve(m, mw, mh, true, true);
                std::vector<uint8_t> mi(m.size());
                for (size_t i = 0; i < m.size(); ++i) mi[i] = nearest(m[i], pal);
                mips.push_back(swizzle(mi, mw, mh));
            }
        }
        blob.push_back(static_cast<uint8_t>((hasAlpha ? 1 : 0) | 4 | (mips.empty() ? 0 : 8)));  // alpha|linear|mips
        blob.push_back(static_cast<uint8_t>(mips.size()));
        put<uint16_t>(blob, static_cast<uint16_t>(w));
        put<uint16_t>(blob, static_cast<uint16_t>(h));
        put<uint16_t>(blob, static_cast<uint16_t>(w));
        put<uint16_t>(blob, static_cast<uint16_t>(h));
        put<uint16_t>(blob, 256);
        put<uint16_t>(blob, 0);
        put<uint32_t>(blob, 0);    // 9-slice margins
        put<uint32_t>(blob, 32);   // CLUT offset
        put<uint32_t>(blob, 32 + 1024);
        for (const Rgb& p : pal) put<uint32_t>(blob, to8888(p));
        const std::vector<uint8_t> sw = swizzle(idx, w, h);
        blob.insert(blob.end(), sw.begin(), sw.end());
        for (const auto& mip : mips) {
            alignTo(blob, 16);
            blob.insert(blob.end(), mip.begin(), mip.end());
        }
        return blob;
    }

    std::string root_;
    int texMax_;
    std::vector<PakItem>& out_;
    std::map<std::string, std::pair<int, bool>> ids_;
    size_t count_ = 0;
    size_t bytes_ = 0;
};

// upstream CharacterRenderer regionFor(): where an overlay sits on the 256 atlas.
struct Region {
    int x, y, w, h;
    bool known;
};
inline Region regionFor(const std::string& p) {
    auto has = [&](const char* s) { return p.find(s) != std::string::npos; };
    if (has("faceupper") || has("facialupperhair") || has("scalpupperhair")) return {0, 160, 128, 32, true};
    if (has("facelower") || has("faciallowerhair") || has("scalplowerhair")) return {0, 192, 128, 64, true};
    if (has("pelvis")) return {128, 96, 128, 64, true};
    if (has("torso")) return {128, 0, 128, 64, true};
    if (has("armupper")) return {0, 0, 128, 64, true};
    if (has("armlower")) return {0, 64, 128, 64, true};
    if (has("hand")) return {0, 128, 128, 32, true};
    if (has("foot") || has("feet")) return {128, 224, 128, 32, true};
    if (has("legupper") || has("leg")) return {128, 160, 128, 64, true};
    return {0, 0, 0, 0, false};
}

// Area-average resample (alpha-weighted colour) to exactly dw x dh.
inline std::vector<Rgb> resample(const pipeline::BLPImage& img, int dw, int dh) {
    std::vector<Rgb> out(static_cast<size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * img.height / dh, y1 = std::max(y0 + 1, (y + 1) * img.height / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * img.width / dw, x1 = std::max(x0 + 1, (x + 1) * img.width / dw);
            Rgb s{0, 0, 0, 0}, u{0, 0, 0, 0};
            int n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = &img.data[(static_cast<size_t>(yy) * img.width + xx) * 4];
                    const float a = p[3] / 255.0f;
                    s.r += p[0] / 255.0f * a;
                    s.g += p[1] / 255.0f * a;
                    s.b += p[2] / 255.0f * a;
                    s.a += a;
                    u.r += p[0] / 255.0f;
                    u.g += p[1] / 255.0f;
                    u.b += p[2] / 255.0f;
                    ++n;
                }
            }
            // A fully transparent area keeps its plain average (was black: 0 / 0).
            const float inv = s.a > 1e-4f ? 1.0f / s.a : 0.0f;
            out[static_cast<size_t>(y) * dw + x] = s.a > 1e-4f ? Rgb{s.r * inv, s.g * inv, s.b * inv, s.a / n}
                                                               : Rgb{u.r / n, u.g / n, u.b / n, 0.0f};
        }
    }
    return out;
}

// M2 blend mode -> PSP batch blend.
inline uint8_t mapBlend(uint16_t m2Blend) {
    switch (m2Blend) {
        case 0: return fmt::kBlendOpaque;
        case 1: return fmt::kBlendAlphaKey;
        case 2: return fmt::kBlendAlpha;
        case 5:
        case 6: return fmt::kBlendModulate;
        default: return fmt::kBlendAdd;
    }
}

inline bool loadM2(const std::vector<uint8_t>& m2, const std::string& skinPath, pipeline::M2Model& m) {
    m = pipeline::M2Loader::load(m2);
    if (m.version >= 264) {
        std::vector<uint8_t> skin;
        if (!readFile(skinPath, skin) || !pipeline::M2Loader::loadSkin(skin, m)) return false;
    }
    return m.isValid();
}

// upstream pipeline::animPathForM2 (m2_asset_loader.cpp, which needs the
// AssetManager): "<stem><animId:04>-<variation:02>.anim".
inline std::string animPath(const std::string& m2Path, uint32_t animId, uint32_t variation) {
    const std::string skin = pipeline::skinPathForM2(m2Path);
    char tail[32];
    std::snprintf(tail, sizeof(tail), "%04u-%02u.anim", animId, variation);
    return skin.substr(0, skin.size() - 7) + tail;
}

// upstream CharacterRenderer::getBoneTransform: T(pivot)*T*R*S*T(-pivot).
inline glm::mat4 boneLocal(const pipeline::M2Model& m, const pipeline::M2Bone& b, int seq) {
    namespace tr = rendering::m2_track;
    const glm::vec3 t = tr::sampleVec3(b.translation, seq, 0.0f, 0.0f, m.globalSequenceDurations, glm::vec3(0.0f));
    const glm::quat q = tr::sampleQuat(b.rotation, seq, 0.0f, 0.0f, m.globalSequenceDurations);
    const glm::vec3 s = tr::sampleVec3(b.scale, seq, 0.0f, 0.0f, m.globalSequenceDurations, glm::vec3(1.0f));
    const glm::mat3 r = glm::mat3_cast(q);
    const glm::vec3 c0 = r[0] * s.x, c1 = r[1] * s.y, c2 = r[2] * s.z;
    const glm::vec3 tt = (b.pivot + t) - (c0 * b.pivot.x + c1 * b.pivot.y + c2 * b.pivot.z);
    glm::mat4 out;
    out[0] = glm::vec4(c0, 0.0f);
    out[1] = glm::vec4(c1, 0.0f);
    out[2] = glm::vec4(c2, 0.0f);
    out[3] = glm::vec4(tt, 1.0f);
    return out;
}

// Stand (animation 0, variation 0), frame 0, applied to the vertices
// (characters and creatures; M9 replaces it with run-time skinning).
inline bool poseStand(const std::string& root, const std::string& m2Path, const std::vector<uint8_t>& m2Bytes,
                      pipeline::M2Model& m) {
    int seq = -1;
    for (size_t i = 0; i < m.sequences.size(); ++i) {
        if (m.sequences[i].id == 0 && m.sequences[i].variationIndex == 0) {
            seq = static_cast<int>(i);
            break;
        }
    }
    if (seq < 0 || m.bones.empty()) return false;
    if (!(m.sequences[seq].flags & 0x20)) {
        std::vector<uint8_t> anim;
        if (!readFile(root + "/" + lower(animPath(m2Path, 0, 0)), anim)) return false;
        pipeline::M2Loader::loadAnimFile(m2Bytes, anim, static_cast<uint32_t>(seq), m);
    }
    std::vector<glm::mat4> bones(m.bones.size());
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const auto& b = m.bones[i];
        const glm::mat4 local = boneLocal(m, b, seq);
        bones[i] = (b.parentBone >= 0 && static_cast<size_t>(b.parentBone) < i) ? bones[b.parentBone] * local : local;
    }
    for (auto& v : m.vertices) {
        glm::vec3 p(0.0f), n(0.0f);
        float wsum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float w = v.boneWeights[k] / 255.0f;
            if (w <= 0.0f || v.boneIndices[k] >= bones.size()) continue;
            const glm::mat4& bm = bones[v.boneIndices[k]];
            p += w * glm::vec3(bm * glm::vec4(v.position, 1.0f));
            n += w * glm::mat3(bm) * v.normal;
            wsum += w;
        }
        if (wsum > 0.0f) {
            v.position = p / wsum;
            v.normal = n;
        }
    }
    return true;
}

struct WmdlInfo {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    uint32_t triangles = 0;   // LOD 0
    size_t vertices = 0;
    size_t batches[3] = {0, 0, 0};  // LOD 0, LOD 1, far LOD 2 (forest LOD)
    bool strips = false;
};

// Collects batches (index ranges over one shared M2 vertex array) and writes
// a WMDL blob: a vertex used with both UV sets is emitted once per set,
// positions/UVs are normalised to s16/u16, sun light is baked per vertex.
class WmdlBuilder {
public:
    // False when the model runs out of 16-bit vertex indices.
    bool add(const std::vector<uint16_t>& indices, uint32_t start, uint32_t count, int lod, int uvSet,
             fmt::ModelBatch ob, uint16_t geoset = 0) {
        ob.indexStart = static_cast<uint32_t>(indices_.size());
        ob.indexCount = count;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t src = indices[start + k];
            const uint32_t key = src * 2 + static_cast<uint32_t>(uvSet);
            auto it = vmap_.find(key);
            uint16_t vi;
            if (it != vmap_.end()) {
                vi = it->second;
            } else {
                if (verts_.size() >= 65535) {
                    indices_.resize(ob.indexStart);
                    return false;
                }
                vi = static_cast<uint16_t>(verts_.size());
                verts_.emplace_back(src, uvSet);
                vmap_.emplace(key, vi);
            }
            indices_.push_back(vi);
        }
        batches_[lod].push_back(ob);
        geosets_[lod].push_back(geoset);
        return true;
    }
    bool empty() const { return batches_[0].empty() || verts_.empty(); }
    size_t batchCount(int lod) const { return batches_[lod].size(); }
    size_t triangles(int lod) const {
        size_t n = 0;
        for (const auto& b : batches_[lod]) n += b.indexCount / 3;
        return n;
    }

    // withGeosets: append the per-batch submesh ids (characters).
    // litColors (WMO): per source vertex RGB light to bake instead of the sun
    // term from the normal (unlit batches still take white).
    std::vector<uint8_t> build(const std::vector<pipeline::M2Vertex>& source, uint16_t flags, bool withGeosets,
                               WmdlInfo& info, const std::vector<glm::vec3>* litColors = nullptr) const {
        glm::vec3 pmin(1e9f), pmax(-1e9f);
        glm::vec2 tmin(1e9f), tmax(-1e9f);
        for (auto [src, set] : verts_) {
            const auto& v = source[src];
            pmin = glm::min(pmin, v.position);
            pmax = glm::max(pmax, v.position);
            tmin = glm::min(tmin, v.texCoords[set]);
            tmax = glm::max(tmax, v.texCoords[set]);
        }
        const glm::vec3 center = (pmin + pmax) * 0.5f;
        const glm::vec3 half = glm::max((pmax - pmin) * 0.5f, glm::vec3(0.01f));
        const glm::vec2 trange = glm::max(tmax - tmin, glm::vec2(1e-4f));
        float radius = 0.0f;
        const glm::vec3 sun = glm::normalize(glm::vec3(0.35f, 0.25f, 0.9f));

        // Unlit is the only batch state in the vertex colour: a vertex shared by
        // lit and unlit batches stays unlit (rare).
        std::vector<bool> unlit(verts_.size(), false);
        for (int l = 0; l < kLods; ++l)
            for (const auto& b : batches_[l])
                if (b.flags & fmt::kBatchUnlit)
                    for (uint32_t k = 0; k < b.indexCount; ++k) unlit[indices_[b.indexStart + k]] = true;

        std::vector<fmt::ModelVertex> verts(verts_.size());
        for (size_t i = 0; i < verts_.size(); ++i) {
            const auto& v = source[verts_[i].first];
            const glm::vec2 uv = v.texCoords[verts_[i].second];
            const glm::vec3 p = (v.position - center) / half;
            radius = std::max(radius, glm::length(v.position - center));
            fmt::ModelVertex& ov = verts[i];
            ov.x = static_cast<int16_t>(std::lround(std::clamp(p.x, -1.0f, 1.0f) * 32767.0f));
            ov.y = static_cast<int16_t>(std::lround(std::clamp(p.y, -1.0f, 1.0f) * 32767.0f));
            ov.z = static_cast<int16_t>(std::lround(std::clamp(p.z, -1.0f, 1.0f) * 32767.0f));
            ov.u = static_cast<uint16_t>(std::lround(std::clamp((uv.x - tmin.x) / trange.x, 0.0f, 1.0f) * 65535.0f));
            ov.v = static_cast<uint16_t>(std::lround(std::clamp((uv.y - tmin.y) / trange.y, 0.0f, 1.0f) * 65535.0f));
            glm::vec3 light(1.0f);
            if (!unlit[i]) {
                if (litColors) {
                    light = glm::min((*litColors)[verts_[i].first], glm::vec3(1.0f));
                } else {
                    const float nl = glm::length(v.normal);
                    const glm::vec3 n = nl > 1e-4f ? v.normal / nl : glm::vec3(0, 0, 1);
                    light = glm::vec3(std::min(1.0f, 0.5f + 0.5f * std::max(0.0f, glm::dot(n, sun))));
                }
            }
            ov.color = to565(Rgb{light.r, light.g, light.b, 1.0f});
        }

        fmt::ModelHeader h{};
        std::memcpy(h.magic, "WMDL", 4);
        h.version = fmt::kModelFormatVersion;
        h.flags = flags;
        h.vertexCount = static_cast<uint16_t>(verts.size());
        h.indexCount = static_cast<uint32_t>(indices_.size());
        h.batchCount[0] = static_cast<uint16_t>(batches_[0].size());
        h.batchCount[1] = static_cast<uint16_t>(batches_[1].size());
        if (!batches_[2].empty()) {  // forest far LOD (older paks: flag clear, field 0)
            h.flags = static_cast<uint16_t>(h.flags | fmt::kModelLod2);
            h.lod2BatchCount = static_cast<uint16_t>(batches_[2].size());
        }
        for (int a = 0; a < 3; ++a) {
            h.center[a] = center[a];
            h.half[a] = half[a];
        }
        h.uvMin[0] = tmin.x;
        h.uvMin[1] = tmin.y;
        h.uvScale[0] = trange.x * 32768.0f / 65535.0f;
        h.uvScale[1] = trange.y * 32768.0f / 65535.0f;
        h.radius = radius;

        // Non-indexed triangle strips (SPP_EXP measurement, and on the PSP:
        // near a large building the GE was vertex bound - a 1x1 scissor did not change
        // the frame time). Indexed lists transform ~3 vertices per triangle,
        // meshoptimizer strips ~1.3-1.6; batches are joined by degenerate
        // triangles, winding does not matter (every model draws two-sided).
        std::vector<fmt::ModelBatch> outBatches[kLods] = {batches_[0], batches_[1], batches_[2]};
        std::vector<fmt::ModelVertex> stripVerts;
        bool strips = true;
        for (int l = 0; l < kLods && strips; ++l) {
            for (auto& b : outBatches[l]) {
                std::vector<unsigned int> tri(indices_.begin() + b.indexStart,
                                              indices_.begin() + b.indexStart + b.indexCount);
                meshopt_optimizeVertexCacheStrip(tri.data(), tri.data(), tri.size(), verts.size());
                std::vector<unsigned int> strip(meshopt_stripifyBound(tri.size()));
                strip.resize(meshopt_stripify(strip.data(), tri.data(), tri.size(), verts.size(), 0));
                if (stripVerts.size() + strip.size() > 65535) {
                    strips = false;
                    break;
                }
                b.indexStart = static_cast<uint32_t>(stripVerts.size());
                b.indexCount = static_cast<uint32_t>(strip.size());
                for (unsigned int i : strip) stripVerts.push_back(verts[i]);
            }
        }
        if (strips) {
            h.flags = static_cast<uint16_t>(h.flags | fmt::kModelStrips);
            h.vertexCount = static_cast<uint16_t>(stripVerts.size());
            h.indexCount = 0;
        } else {
            for (int l = 0; l < kLods; ++l) outBatches[l] = batches_[l];
        }
        const std::vector<fmt::ModelVertex>& vout = strips ? stripVerts : verts;

        std::vector<uint8_t> blob(sizeof(h));
        h.vertexOffset = static_cast<uint32_t>(blob.size());
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(vout.data()),
                    reinterpret_cast<const uint8_t*>(vout.data() + vout.size()));
        alignTo(blob, 16);
        h.indexOffset = static_cast<uint32_t>(blob.size());
        if (!strips)
            blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(indices_.data()),
                        reinterpret_cast<const uint8_t*>(indices_.data() + indices_.size()));
        alignTo(blob, 16);
        h.batchOffset = static_cast<uint32_t>(blob.size());
        for (int l = 0; l < kLods; ++l)
            blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(outBatches[l].data()),
                        reinterpret_cast<const uint8_t*>(outBatches[l].data() + outBatches[l].size()));
        if (withGeosets) {
            alignTo(blob, 4);
            h.geosetOffset = static_cast<uint32_t>(blob.size());
            for (int l = 0; l < kLods; ++l)
                for (uint16_t g : geosets_[l]) put<uint16_t>(blob, g);
        }
        std::memcpy(blob.data(), &h, sizeof(h));

        info.center = center;
        info.radius = radius;
        info.vertices = vout.size();
        info.strips = strips;
        info.triangles = 0;
        for (const auto& b : batches_[0]) info.triangles += b.indexCount / 3;
        info.batches[0] = batches_[0].size();
        info.batches[1] = batches_[1].size();
        info.batches[2] = batches_[2].size();
        return blob;
    }

private:
    // LOD 0, LOD 1 and the forest's far LOD 2; characters and creatures use
    // the first two only, their blobs are unchanged.
    static constexpr int kLods = 3;
    std::vector<fmt::ModelBatch> batches_[kLods];
    std::vector<uint16_t> geosets_[kLods];
    std::vector<uint16_t> indices_;
    std::vector<std::pair<uint32_t, int>> verts_;  // (source vertex, UV set)
    std::unordered_map<uint32_t, uint16_t> vmap_;  // source * 2 + set -> out index
};

// The GE drops a whole triangle when a vertex lands far outside the screen
// (on hardware: building walls vanished at the screen edges next to the
// camera). Large building triangles are split at their longest edge until
// every edge is <= maxEdge; midpoints are shared, so the mesh stays closed.
// Where it stops short - kMaxDepth halvings, or the u16 vertex ceiling (kMaxVertices) - the
// triangle stays longer than maxEdge (the GE may drop it next to the camera): counted, the callers log it per
// model / WMO group and in the bake's summary (was: silent). kMaxDepth 8 -> 16 (as BigSplitter, wmo_chunk_bake.hpp):
// 8 halvings left real triangles long (test tiles 30_49 / 32_39 / 32_48: a tree canopy model 512 of them up to
// 6.1 > 5, a tree facade model 256 up to 5.1; +768 near-LOD triangles, +14 KB of the pak); the vertex ceiling stays the
// limit.
class TriangleSplitter {
public:
    static constexpr int kMaxDepth = 16;
    static constexpr size_t kMaxVertices = 65000;

    TriangleSplitter(std::vector<pipeline::M2Vertex>& v, std::vector<glm::vec3>& lit, float maxEdge)
        : v_(v), lit_(lit), max2_(maxEdge * maxEdge) {}

    // Triangles left longer than maxEdge since construction / the last resetStops().
    size_t depthStops() const { return depthStops_; }      // kMaxDepth halvings were not enough
    size_t ceilingStops() const { return ceilingStops_; }  // the vertex ceiling was reached
    float longestLeft() const { return std::sqrt(longestLeft2_); }
    void resetStops() {
        depthStops_ = ceilingStops_ = 0;
        longestLeft2_ = 0.0f;
    }

    void split(uint32_t a, uint32_t b, uint32_t c, std::vector<uint16_t>& out, int depth = 0) {
        const float ab = d2(a, b), bc = d2(b, c), ca = d2(c, a);
        const float longest = std::max({ab, bc, ca});
        if (longest <= max2_ || depth >= kMaxDepth || v_.size() >= kMaxVertices) {
            if (longest > max2_) {  // stopped short
                ++(v_.size() >= kMaxVertices ? ceilingStops_ : depthStops_);
                longestLeft2_ = std::max(longestLeft2_, longest);
            }
            out.push_back(static_cast<uint16_t>(a));
            out.push_back(static_cast<uint16_t>(b));
            out.push_back(static_cast<uint16_t>(c));
            return;
        }
        if (longest == ab) {
            const uint32_t m = mid(a, b);
            split(a, m, c, out, depth + 1);
            split(m, b, c, out, depth + 1);
        } else if (longest == bc) {
            const uint32_t m = mid(b, c);
            split(a, b, m, out, depth + 1);
            split(a, m, c, out, depth + 1);
        } else {
            const uint32_t m = mid(c, a);
            split(a, b, m, out, depth + 1);
            split(m, b, c, out, depth + 1);
        }
    }

private:
    float d2(uint32_t a, uint32_t b) const {
        const glm::vec3 d = v_[a].position - v_[b].position;
        return glm::dot(d, d);
    }
    uint32_t mid(uint32_t a, uint32_t b) {
        const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
        auto it = mids_.find(key);
        if (it != mids_.end()) return it->second;
        pipeline::M2Vertex m = v_[a];
        const pipeline::M2Vertex& q = v_[b];
        m.position = (m.position + q.position) * 0.5f;
        m.normal = m.normal + q.normal;
        m.texCoords[0] = (m.texCoords[0] + q.texCoords[0]) * 0.5f;
        m.texCoords[1] = (m.texCoords[1] + q.texCoords[1]) * 0.5f;
        v_.push_back(m);
        lit_.push_back((lit_[a] + lit_[b]) * 0.5f);
        const uint32_t id = static_cast<uint32_t>(v_.size() - 1);
        mids_.emplace(key, id);
        return id;
    }

    std::vector<pipeline::M2Vertex>& v_;
    std::vector<glm::vec3>& lit_;
    float max2_;
    std::unordered_map<uint64_t, uint32_t> mids_;
    size_t depthStops_ = 0, ceilingStops_ = 0;  // split-short counters
    float longestLeft2_ = 0.0f;
};


// Collision mesh "k_<n>" (include/rendering/psp/psp_model_format.hpp WCOL):
// model-space triangles + a 2D grid over XY (upstream 4 yd cells, <= 64x64).
inline std::vector<uint8_t> buildCollision(const std::vector<glm::vec3>& verts,
                                           const std::vector<fmt::CollisionTri>& tris) {
    std::vector<uint8_t> blob;
    if (verts.empty() || tris.empty() || verts.size() > 65535) return blob;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& v : verts) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    float cell = 4.0f;
    const float ext = std::max(hi.x - lo.x, hi.y - lo.y);
    if (ext / cell > 64.0f) cell = ext / 64.0f;
    const int nx = std::max(1, static_cast<int>(std::ceil((hi.x - lo.x) / cell)));
    const int ny = std::max(1, static_cast<int>(std::ceil((hi.y - lo.y) / cell)));
    std::vector<std::vector<uint32_t>> cells(static_cast<size_t>(nx) * ny);
    for (uint32_t t = 0; t < tris.size(); ++t) {
        const glm::vec3& a = verts[tris[t].a];
        const glm::vec3& b = verts[tris[t].b];
        const glm::vec3& c = verts[tris[t].c];
        const int x0 = std::clamp(static_cast<int>((std::min({a.x, b.x, c.x}) - lo.x) / cell), 0, nx - 1);
        const int x1 = std::clamp(static_cast<int>((std::max({a.x, b.x, c.x}) - lo.x) / cell), 0, nx - 1);
        const int y0 = std::clamp(static_cast<int>((std::min({a.y, b.y, c.y}) - lo.y) / cell), 0, ny - 1);
        const int y1 = std::clamp(static_cast<int>((std::max({a.y, b.y, c.y}) - lo.y) / cell), 0, ny - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) cells[static_cast<size_t>(y) * nx + x].push_back(t);
    }
    fmt::CollisionHeader h{};
    std::memcpy(h.magic, "WCOL", 4);
    h.version = fmt::kModelFormatVersion;
    h.vertexCount = static_cast<uint32_t>(verts.size());
    h.triangleCount = static_cast<uint32_t>(tris.size());
    for (int a = 0; a < 3; ++a) {
        h.bmin[a] = lo[a];
        h.bmax[a] = hi[a];
    }
    h.cell = cell;
    h.nx = static_cast<uint16_t>(nx);
    h.ny = static_cast<uint16_t>(ny);
    blob.resize(sizeof(h));
    alignTo(blob, 16);
    h.vertexOffset = static_cast<uint32_t>(blob.size());
    for (const auto& v : verts)
        for (int a = 0; a < 3; ++a) put<float>(blob, v[a]);
    alignTo(blob, 4);
    h.triangleOffset = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(tris.data()),
                reinterpret_cast<const uint8_t*>(tris.data() + tris.size()));
    alignTo(blob, 4);
    h.cellOffset = static_cast<uint32_t>(blob.size());
    uint32_t start = 0;
    for (const auto& c : cells) {
        put<uint32_t>(blob, start);
        put<uint32_t>(blob, static_cast<uint32_t>(c.size()));
        start += static_cast<uint32_t>(c.size());
    }
    h.listOffset = static_cast<uint32_t>(blob.size());
    for (const auto& c : cells)
        for (uint32_t t : c) put<uint32_t>(blob, t);
    h.listCount = start;
    std::memcpy(blob.data(), &h, sizeof(h));
    return blob;
}

} // namespace wowee::pspconv
