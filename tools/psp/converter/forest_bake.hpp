// Forest LOD for the PSP: the far
// LOD 2 mesh of foliage models and their impostors, baked by psp_model_conv.
//
// On hardware, in a dense forest the GE
// spends ~10 ms on models (~11-16M vertices/s), most of it on far trees whose
// canopies are hundreds of alpha-tested leaf cards a few pixels large. Two
// cheaper stand-ins, both chosen per instance by distance on the PSP:
//   - LOD 2: every batch through meshopt_simplify; leaf-card batches (cards
//     are disconnected quads, which a topology-preserving simplifier cannot
//     merge) then drop their smallest cards and grow the kept ones about
//     their centre so the canopy keeps its coverage;
//   - impostor: `views` pictures of the model rendered here by a small
//     software rasterizer (orthographic, from the horizon, textures + alpha
//     test + the WMDL vertex light) into one T8 atlas with mips; the PSP draws
//     one camera-facing quad per far tree (format: psp_model_format.hpp WIMP).
// Also: --verify / --dump of a baked pak (PC check before deploying) and the
// synthetic-tree self test (no client data needed).
#pragma once

#include "model_bake.hpp"

#include <meshoptimizer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace wowee::pspconv {

// One M2 batch as ModelBank accepted it (texture resolved, blend mapped),
// before the near split: the source of LOD 2 and of the impostor pictures.
struct ForestBatch {
    std::vector<uint32_t> tris;   // source vertex indices, 3 per triangle
    fmt::ModelBatch ob{};         // texture, blend, flags (index range unused)
    std::string texName;          // game path of the texture, "" = white
    int uvSet = 0;
};

struct ForestOptions {
    float lod2Ratio = 0.3f;       // LOD 2 triangle target vs its source (0 = no LOD 2)
    float lod2Error = 0.04f;      // meshopt_simplify error, relative to the model size
    float lod2Keep = 0.7f;        // LOD 2 is written only when <= this x its source
    bool impostors = true;
    int impostorViews = 8;        // 4, 8 or 16 (atlas 4 columns)
    int impostorCell = 64;        // texels per view (power of two)
    int impostorSupersample = 4;  // rasterizer samples per texel side
    uint32_t impostorMinTris = 12;  // smaller models are cheaper than a quad + texture
    float impostorMinRadius = 2.0f; // model units: small bushes fade out before
};

// Same sun as WmdlBuilder::build (the WMDL vertex light).
inline glm::vec3 forestSun() { return glm::normalize(glm::vec3(0.35f, 0.25f, 0.9f)); }

inline float vertexLight(const pipeline::M2Vertex& v, bool unlit) {
    if (unlit) return 1.0f;
    const float nl = glm::length(v.normal);
    const glm::vec3 n = nl > 1e-4f ? v.normal / nl : glm::vec3(0, 0, 1);
    return std::min(1.0f, 0.5f + 0.5f * std::max(0.0f, glm::dot(n, forestSun())));
}

// ---------------------------------------------------------------------------
// LOD 2
// ---------------------------------------------------------------------------

struct Lod2Stats {
    size_t sourceTris = 0;
    size_t tris = 0;
    size_t cardsKept = 0, cardsTotal = 0;
};

namespace detail {

inline uint32_t findRoot(std::vector<uint32_t>& p, uint32_t x) {
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

inline float triArea(const std::vector<pipeline::M2Vertex>& v, uint32_t a, uint32_t b, uint32_t c) {
    return 0.5f * glm::length(glm::cross(v[b].position - v[a].position, v[c].position - v[a].position));
}

// Leaf cards: keeps the largest connected pieces until `target` triangles,
// then grows the kept pieces about their area centre by sqrt(total/kept area)
// (<= 1.35) so the canopy keeps roughly its coverage. New vertices are
// appended to `verts` (the UVs stay: the texture stretches with the card).
inline void decimateCards(std::vector<uint32_t>& tris, size_t target, std::vector<pipeline::M2Vertex>& verts,
                          Lod2Stats& st) {
    const size_t n = tris.size() / 3;
    std::vector<uint32_t> parent(verts.size());
    std::iota(parent.begin(), parent.end(), 0u);
    for (size_t t = 0; t < n; ++t) {
        const uint32_t a = findRoot(parent, tris[t * 3]);
        for (int k = 1; k < 3; ++k) {
            const uint32_t b = findRoot(parent, tris[t * 3 + k]);
            if (a != b) parent[b] = a;
        }
    }
    struct Piece {
        std::vector<uint32_t> tris;  // triangle numbers
        float area = 0.0f;
        glm::vec3 centre{0.0f};
        uint32_t first = 0;
    };
    std::map<uint32_t, Piece> byRoot;
    for (size_t t = 0; t < n; ++t) {
        Piece& p = byRoot[findRoot(parent, tris[t * 3])];
        if (p.tris.empty()) p.first = static_cast<uint32_t>(t);
        const float a = triArea(verts, tris[t * 3], tris[t * 3 + 1], tris[t * 3 + 2]);
        p.tris.push_back(static_cast<uint32_t>(t));
        p.area += a;
        p.centre += a * (verts[tris[t * 3]].position + verts[tris[t * 3 + 1]].position +
                         verts[tris[t * 3 + 2]].position) / 3.0f;
    }
    std::vector<Piece> pieces;
    float total = 0.0f;
    for (auto& [root, p] : byRoot) {
        if (p.area > 1e-8f) p.centre /= p.area;
        else p.centre = verts[tris[p.tris[0] * 3]].position;
        total += p.area;
        pieces.push_back(std::move(p));
    }
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
        return a.area != b.area ? a.area > b.area : a.first < b.first;
    });
    size_t keep = 0, keptTris = 0;
    float keptArea = 0.0f;
    while (keep < pieces.size() && (keep == 0 || keptTris < target)) {
        keptTris += pieces[keep].tris.size();
        keptArea += pieces[keep].area;
        ++keep;
    }
    st.cardsTotal += pieces.size();
    st.cardsKept += keep;
    float grow = keptArea > 1e-8f ? std::sqrt(total / keptArea) : 1.0f;
    grow = std::clamp(grow, 1.0f, 1.35f);
    std::vector<uint32_t> out;
    out.reserve(keptTris * 3);
    for (size_t i = 0; i < keep; ++i) {
        const Piece& p = pieces[i];
        std::map<uint32_t, uint32_t> remap;
        for (uint32_t t : p.tris) {
            for (int k = 0; k < 3; ++k) {
                const uint32_t src = tris[t * 3 + k];
                if (grow <= 1.001f || verts.size() >= 65000) {
                    out.push_back(src);
                    continue;
                }
                auto it = remap.find(src);
                if (it == remap.end()) {
                    pipeline::M2Vertex v = verts[src];
                    v.position = p.centre + (v.position - p.centre) * grow;
                    verts.push_back(v);
                    it = remap.emplace(src, static_cast<uint32_t>(verts.size() - 1)).first;
                }
                out.push_back(it->second);
            }
        }
    }
    tris.swap(out);
}

} // namespace detail

// Builds LOD 2 from `src` (the model's LOD 1 skin when it has one, else the
// original LOD 0) into `o`'s lod 2. Nothing is added when the result is not
// below opt.lod2Keep of the source. Returns the LOD 2 triangle count;
// `result` (self test) receives the LOD 2 batches.
inline size_t buildLod2(const std::vector<ForestBatch>& src, std::vector<pipeline::M2Vertex>& verts,
                        const ForestOptions& opt, WmdlBuilder& o, Lod2Stats& st,
                        std::vector<ForestBatch>* result = nullptr) {
    if (opt.lod2Ratio <= 0.0f || src.empty()) return 0;
    std::vector<std::vector<uint32_t>> outTris;
    size_t srcTris = 0, outCount = 0;
    for (const ForestBatch& b : src) {
        const size_t n = b.tris.size() / 3;
        srcTris += n;
        std::vector<uint32_t> tris = b.tris;
        const size_t target = std::max<size_t>(1, static_cast<size_t>(std::lround(n * opt.lod2Ratio)));
        if (n >= 8) {
            std::vector<unsigned int> dst(tris.size());
            float err = 0.0f;
            const size_t k = meshopt_simplify(dst.data(), tris.data(), tris.size(), &verts[0].position.x, verts.size(),
                                              sizeof(pipeline::M2Vertex), target * 3, opt.lod2Error, 0, &err);
            dst.resize(k);
            if (k >= 3) tris.assign(dst.begin(), dst.end());
            // Leaf cards (alpha-tested) the simplifier could not merge.
            if (b.ob.blend == fmt::kBlendAlphaKey && tris.size() / 3 > target + target / 4)
                detail::decimateCards(tris, target, verts, st);
        }
        outCount += tris.size() / 3;
        outTris.push_back(std::move(tris));
    }
    st.sourceTris += srcTris;
    if (outCount == 0 || static_cast<float>(outCount) > opt.lod2Keep * static_cast<float>(srcTris)) return 0;
    for (size_t i = 0; i < src.size(); ++i) {
        if (outTris[i].empty()) continue;
        std::vector<uint16_t> idx(outTris[i].begin(), outTris[i].end());
        if (!o.add(idx, 0, static_cast<uint32_t>(idx.size()), 2, src[i].uvSet, src[i].ob)) return 0;
        if (result) {
            result->push_back(src[i]);
            result->back().tris = outTris[i];
        }
    }
    st.tris += outCount;
    return outCount;
}

// ---------------------------------------------------------------------------
// Impostor
// ---------------------------------------------------------------------------

struct ImpostorResult {
    fmt::ImpostorHeader h{};      // texture id filled by the caller
    pipeline::BLPImage atlas;     // RGBA8, cols*cell x rows*cell
    float coverage[16] = {};      // covered fraction of each view's inner cell
};

class ImpostorBaker {
public:
    explicit ImpostorBaker(std::string root) : root_(std::move(root)) {}

    // Self test / converter-made pictures: registers an image under a key.
    void preload(const std::string& key, pipeline::BLPImage img) { cache_[lower(key)] = std::move(img); }

    const pipeline::BLPImage* image(const std::string& gamePath) {
        if (gamePath.empty()) return nullptr;
        const std::string key = lower(gamePath);
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            pipeline::BLPImage img;
            std::vector<uint8_t> bytes;
            if (readFile(root_ + "/" + key, bytes)) img = pipeline::BLPLoader::load(bytes);
            it = cache_.emplace(key, std::move(img)).first;
        }
        const pipeline::BLPImage& img = it->second;
        return (img.width > 0 && img.height > 0 && img.data.size() >= static_cast<size_t>(img.width) * img.height * 4)
                   ? &img : nullptr;
    }

    bool bake(const std::vector<ForestBatch>& batches, const std::vector<pipeline::M2Vertex>& verts,
              const ForestOptions& opt, ImpostorResult& out) {
        const int views = opt.impostorViews;
        const int cell = opt.impostorCell;
        const int cols = 4;
        const int rows = (views + cols - 1) / cols;
        if ((views != 4 && views != 8 && views != 16) || cell < 16 || (cell & (cell - 1))) return false;
        // Axis and extents over the vertices the batches use.
        glm::vec3 lo(1e30f), hi(-1e30f);
        size_t used = 0;
        for (const ForestBatch& b : batches) {
            if (skipBlend(b.ob.blend)) continue;
            for (uint32_t i : b.tris) {
                lo = glm::min(lo, verts[i].position);
                hi = glm::max(hi, verts[i].position);
                ++used;
            }
        }
        if (!used || hi.z - lo.z < 1e-3f) return false;
        const glm::vec2 axis((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        float halfW = 0.0f;
        for (const ForestBatch& b : batches) {
            if (skipBlend(b.ob.blend)) continue;
            for (uint32_t i : b.tris) halfW = std::max(halfW, glm::length(glm::vec2(verts[i].position) - axis));
        }
        if (halfW < 1e-3f) return false;

        const int margin = std::max(1, cell / 32);
        const int inner = cell - 2 * margin;
        const int ss = std::max(1, opt.impostorSupersample);
        const int S = inner * ss;
        const int aw = cols * cell, ah = rows * cell;
        std::vector<Rgb> atlas(static_cast<size_t>(aw) * ah, Rgb{0, 0, 0, 0});
        std::vector<Rgb> canvas(static_cast<size_t>(S) * S);
        std::vector<float> depth(static_cast<size_t>(S) * S);
        constexpr float kPi = 3.14159265358979f;
        for (int view = 0; view < views; ++view) {
            const float phi = 2.0f * kPi * static_cast<float>(view) / static_cast<float>(views);
            const glm::vec3 dir(std::cos(phi), std::sin(phi), 0.0f);
            const glm::vec3 right(-std::sin(phi), std::cos(phi), 0.0f);
            std::fill(canvas.begin(), canvas.end(), Rgb{0, 0, 0, 0});
            std::fill(depth.begin(), depth.end(), -1e30f);
            for (const ForestBatch& b : batches) {
                if (skipBlend(b.ob.blend)) continue;
                const pipeline::BLPImage* img = image(b.texName);
                const bool unlit = (b.ob.flags & fmt::kBatchUnlit) != 0;
                const bool keyed = b.ob.blend == fmt::kBlendAlphaKey || b.ob.blend == fmt::kBlendAlpha;
                for (size_t t = 0; t + 2 < b.tris.size(); t += 3) {
                    Sv s[3];
                    for (int k = 0; k < 3; ++k) {
                        const pipeline::M2Vertex& v = verts[b.tris[t + k]];
                        const glm::vec3 p = v.position - glm::vec3(axis, 0.0f);
                        s[k].x = (glm::dot(p, right) + halfW) / (2.0f * halfW) * S;
                        s[k].y = (hi.z - v.position.z) / (hi.z - lo.z) * S;
                        s[k].z = glm::dot(p, dir);
                        s[k].uv = v.texCoords[b.uvSet];
                        s[k].light = vertexLight(v, unlit);
                    }
                    raster(s, img, keyed, (b.ob.flags & fmt::kBatchWrapU) != 0, (b.ob.flags & fmt::kBatchWrapV) != 0,
                           S, canvas, depth);
                }
            }
            // Box filter to the cell (colour weighted by coverage).
            const int cx0 = (view % cols) * cell + margin, cy0 = (view / cols) * cell + margin;
            size_t covered = 0;
            for (int y = 0; y < inner; ++y) {
                for (int x = 0; x < inner; ++x) {
                    Rgb acc{0, 0, 0, 0};
                    for (int yy = 0; yy < ss; ++yy) {
                        for (int xx = 0; xx < ss; ++xx) {
                            const Rgb& q = canvas[static_cast<size_t>(y * ss + yy) * S + x * ss + xx];
                            acc.r += q.r * q.a;
                            acc.g += q.g * q.a;
                            acc.b += q.b * q.a;
                            acc.a += q.a;
                        }
                    }
                    const float inv = acc.a > 1e-6f ? 1.0f / acc.a : 0.0f;
                    const float a = acc.a / static_cast<float>(ss * ss);
                    atlas[static_cast<size_t>(cy0 + y) * aw + cx0 + x] = Rgb{acc.r * inv, acc.g * inv, acc.b * inv, a};
                    if (a >= 0.5f) ++covered;
                }
            }
            if (view < 16) out.coverage[view] = static_cast<float>(covered) / static_cast<float>(inner * inner);
        }
        dilate(atlas, aw, ah, 8);

        out.atlas = pipeline::BLPImage{};
        out.atlas.width = aw;
        out.atlas.height = ah;
        out.atlas.data.resize(static_cast<size_t>(aw) * ah * 4);
        for (size_t i = 0; i < atlas.size(); ++i) {
            out.atlas.data[i * 4 + 0] = static_cast<uint8_t>(quant(atlas[i].r, 255));
            out.atlas.data[i * 4 + 1] = static_cast<uint8_t>(quant(atlas[i].g, 255));
            out.atlas.data[i * 4 + 2] = static_cast<uint8_t>(quant(atlas[i].b, 255));
            out.atlas.data[i * 4 + 3] = static_cast<uint8_t>(quant(atlas[i].a, 255));
        }
        fmt::ImpostorHeader& h = out.h;
        h = fmt::ImpostorHeader{};
        std::memcpy(h.magic, "WIMP", 4);
        h.version = fmt::kImpostorVersion;
        h.views = static_cast<uint8_t>(views);
        h.cols = static_cast<uint8_t>(cols);
        h.texture = fmt::kNoTexture;
        h.cellSize = static_cast<uint16_t>(cell);
        h.margin = static_cast<uint16_t>(margin);
        h.axis[0] = axis.x;
        h.axis[1] = axis.y;
        h.zMin = lo.z;
        h.zMax = hi.z;
        h.halfWidth = halfW;
        for (const ForestBatch& b : batches) h.sourceTriangles += static_cast<uint32_t>(b.tris.size() / 3);
        return true;
    }

private:
    struct Sv {
        float x, y, z;
        glm::vec2 uv;
        float light;
    };

    // Additive / modulated batches (glows) are left out of the picture.
    static bool skipBlend(uint8_t blend) { return blend == fmt::kBlendAdd || blend == fmt::kBlendModulate; }

    static Rgb sample(const pipeline::BLPImage* img, glm::vec2 uv, bool wrapU, bool wrapV) {
        if (!img) return Rgb{1, 1, 1, 1};
        const int w = img->width, h = img->height;
        auto fix = [](int i, int n, bool wrap) { return wrap ? ((i % n) + n) % n : std::clamp(i, 0, n - 1); };
        const float fx = uv.x * w - 0.5f, fy = uv.y * h - 0.5f;
        if (!std::isfinite(fx) || !std::isfinite(fy)) return Rgb{0, 0, 0, 0};
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - x0, ty = fy - y0;
        Rgb out{0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            const int xi = fix(x0 + (k & 1), w, wrapU), yi = fix(y0 + (k >> 1), h, wrapV);
            const float wt = ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? ty : 1.0f - ty);
            const uint8_t* p = &img->data[(static_cast<size_t>(yi) * w + xi) * 4];
            out.r += wt * p[0] / 255.0f;
            out.g += wt * p[1] / 255.0f;
            out.b += wt * p[2] / 255.0f;
            out.a += wt * p[3] / 255.0f;
        }
        return out;
    }

    // Two-sided, z-buffered (larger z = nearer the viewer), affine UVs
    // (orthographic), alpha test 0.4 like the GE batches (upstream m2.frag).
    static void raster(const Sv (&s)[3], const pipeline::BLPImage* img, bool keyed, bool wrapU, bool wrapV, int S,
                       std::vector<Rgb>& canvas, std::vector<float>& depth) {
        const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
        if (std::fabs(area) < 1e-9f) return;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({s[0].x, s[1].x, s[2].x}))));
        const int x1 = std::min(S - 1, static_cast<int>(std::ceil(std::max({s[0].x, s[1].x, s[2].x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({s[0].y, s[1].y, s[2].y}))));
        const int y1 = std::min(S - 1, static_cast<int>(std::ceil(std::max({s[0].y, s[1].y, s[2].y}))));
        const float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y) {
            const float py = y + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f;
                const float w0 = ((s[1].x - px) * (s[2].y - py) - (s[2].x - px) * (s[1].y - py)) * inv;
                const float w1 = ((s[2].x - px) * (s[0].y - py) - (s[0].x - px) * (s[2].y - py)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float z = w0 * s[0].z + w1 * s[1].z + w2 * s[2].z;
                const size_t i = static_cast<size_t>(y) * S + x;
                if (z <= depth[i]) continue;
                const glm::vec2 uv = w0 * s[0].uv + w1 * s[1].uv + w2 * s[2].uv;
                const Rgb t = sample(img, uv, wrapU, wrapV);
                if (keyed && t.a <= 0.4f) continue;
                const float l = w0 * s[0].light + w1 * s[1].light + w2 * s[2].light;
                depth[i] = z;
                canvas[i] = Rgb{t.r * l, t.g * l, t.b * l, 1.0f};
            }
        }
    }

    // Transparent texels take the colour of covered neighbours: bilinear
    // filtering and the mips then blend leaf colour, not black, at edges.
    static void dilate(std::vector<Rgb>& px, int w, int h, int passes) {
        for (int p = 0; p < passes; ++p) {
            std::vector<Rgb> next = px;
            std::vector<uint8_t> filled(px.size(), 0);
            for (size_t i = 0; i < px.size(); ++i) filled[i] = px[i].a > 0.0f || px[i].r + px[i].g + px[i].b > 0.0f;
            bool any = false;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (filled[i]) continue;
                    Rgb acc{0, 0, 0, 0};
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                            const size_t j = static_cast<size_t>(yy) * w + xx;
                            if (!filled[j]) continue;
                            acc.r += px[j].r;
                            acc.g += px[j].g;
                            acc.b += px[j].b;
                            ++n;
                        }
                    }
                    if (n) {
                        next[i] = Rgb{acc.r / n, acc.g / n, acc.b / n, 0.0f};
                        any = true;
                    }
                }
            }
            px.swap(next);
            if (!any) break;
        }
    }

    std::string root_;
    std::map<std::string, pipeline::BLPImage> cache_;
};

// ---------------------------------------------------------------------------
// PC check of a baked pak: --verify / --dump
// ---------------------------------------------------------------------------

// PNG, RGBA8, stored (uncompressed) deflate blocks: no zlib dependency.
inline bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    uint32_t crcTable[256];
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
    auto be32 = [](std::vector<uint8_t>& o, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<uint8_t>(v >> s));
    };
    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(file, static_cast<uint32_t>(data.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        uint32_t c = 0xFFFFFFFFu;
        for (uint8_t b : td) c = crcTable[(c ^ b) & 0xFF] ^ (c >> 8);
        file.insert(file.end(), td.begin(), td.end());
        be32(file, c ^ 0xFFFFFFFFu);
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, static_cast<uint32_t>(w));
    be32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", ihdr);
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<size_t>(y) * w * 4, rgba.begin() + static_cast<size_t>(y + 1) * w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t off = 0; off < raw.size() || off == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>((~n) >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (n == 0) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    be32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(f);
}

struct PakView {
    std::vector<uint8_t> bytes;
    std::map<std::string, std::pair<uint32_t, uint32_t>> entries;  // name -> (offset, size)

    bool open(const std::string& path) {
        if (!readFile(path, bytes) || bytes.size() < 16 || std::memcmp(bytes.data(), "WPAK", 4) != 0) return false;
        uint32_t count, toc;
        std::memcpy(&count, &bytes[8], 4);
        std::memcpy(&toc, &bytes[12], 4);
        if (toc + static_cast<size_t>(count) * 64 > bytes.size()) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = &bytes[toc + i * 64];
            char name[49] = {};
            std::memcpy(name, e, 48);
            uint32_t off, size;
            std::memcpy(&off, e + 48, 4);
            std::memcpy(&size, e + 52, 4);
            if (static_cast<size_t>(off) + size > bytes.size()) return false;
            entries[name] = {off, size};
        }
        return true;
    }
    const uint8_t* find(const std::string& name, uint32_t& size) const {
        auto it = entries.find(name);
        if (it == entries.end()) return nullptr;
        size = it->second.second;
        return bytes.data() + it->second.first;
    }
};

// WTEX T8 swizzled level 0 -> RGBA8 (the converter's own layout, model_bake.hpp).
inline bool decodeWtex(const uint8_t* b, uint32_t size, int& w, int& h, std::vector<uint8_t>& rgba) {
    if (size < 32 || std::memcmp(b, "WTEX", 4) != 0 || b[4] != 5) return false;
    uint16_t tw, th;
    uint32_t clutOff, pixOff;
    std::memcpy(&tw, b + 8, 2);
    std::memcpy(&th, b + 10, 2);
    std::memcpy(&clutOff, b + 24, 4);
    std::memcpy(&pixOff, b + 28, 4);
    if (clutOff + 1024u > size || pixOff + static_cast<uint32_t>(tw) * th > size || tw % 16 || th % 8) return false;
    w = tw;
    h = th;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    const uint8_t* sw = b + pixOff;
    size_t o = 0;
    for (int by = 0; by < h / 8; ++by)
        for (int bx = 0; bx < w / 16; ++bx)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 16; ++x) {
                    const uint8_t idx = sw[o++];
                    uint32_t c;
                    std::memcpy(&c, b + clutOff + idx * 4u, 4);
                    uint8_t* d = &rgba[(static_cast<size_t>(by * 8 + y) * w + bx * 16 + x) * 4];
                    d[0] = c & 0xFF;
                    d[1] = (c >> 8) & 0xFF;
                    d[2] = (c >> 16) & 0xFF;
                    d[3] = (c >> 24) & 0xFF;
                }
    return true;
}

// Checks every WMDL (header, batch ranges incl. LOD 2, texture references)
// and every WIMP (header, atlas texture size) of a doodads pak; prints a
// summary line and, with dumpDir, writes each impostor atlas as a PNG.
// Returns the number of errors.
inline int verifyPak(const std::string& path, const std::string& dumpDir) {
    PakView pak;
    if (!pak.open(path)) {
        std::fprintf(stderr, "verify: cannot read %s\n", path.c_str());
        return 1;
    }
    int errors = 0;
    if (!dumpDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dumpDir, ec);
    }
    auto bad = [&](const std::string& name, const char* why) {
        if (++errors <= 40) std::fprintf(stderr, "verify: %s: %s\n", name.c_str(), why);
    };
    size_t models = 0, foliage = 0, withLod2 = 0, impostors = 0, atlasBytes = 0, wimpBytes = 0;
    uint64_t trisLod[3] = {0, 0, 0}, foliageLod0 = 0, foliageLod2 = 0;
    for (const auto& [name, loc] : pak.entries) {
        if (name.rfind("m_", 0) != 0) continue;
        uint32_t size = 0;
        const uint8_t* b = pak.find(name, size);
        fmt::ModelHeader h;
        if (size < sizeof(h)) {
            bad(name, "short");
            continue;
        }
        std::memcpy(&h, b, sizeof(h));
        if (std::memcmp(h.magic, "WMDL", 4) != 0 || h.version != fmt::kModelFormatVersion) {
            bad(name, "magic/version");
            continue;
        }
        ++models;
        const bool lod2 = (h.flags & fmt::kModelLod2) != 0;
        const uint32_t counts[3] = {h.batchCount[0], h.batchCount[1], lod2 ? h.lod2BatchCount : 0u};
        const uint32_t nb = counts[0] + counts[1] + counts[2];
        if (lod2 && !h.lod2BatchCount) bad(name, "kModelLod2 without batches");
        if (!lod2 && h.lod2BatchCount) bad(name, "LOD 2 batches without kModelLod2");
        if (h.vertexOffset % 16 || h.vertexOffset + h.vertexCount * sizeof(fmt::ModelVertex) > size ||
            h.indexOffset + h.indexCount * 2u > size || h.batchOffset + nb * sizeof(fmt::ModelBatch) > size) {
            bad(name, "section out of range");
            continue;
        }
        const bool strips = (h.flags & fmt::kModelStrips) != 0;
        const uint32_t limit = strips ? h.vertexCount : h.indexCount;
        uint32_t bi = 0;
        uint64_t t[3] = {0, 0, 0};
        for (int l = 0; l < 3; ++l) {
            for (uint32_t k = 0; k < counts[l]; ++k, ++bi) {
                fmt::ModelBatch mb;
                std::memcpy(&mb, b + h.batchOffset + bi * sizeof(mb), sizeof(mb));
                if (mb.indexStart + mb.indexCount > limit) bad(name, "batch range");
                if (mb.texture != fmt::kNoTexture && !pak.entries.count("x_" + std::to_string(mb.texture)))
                    bad(name, "missing texture");
                t[l] += strips ? (mb.indexCount > 2 ? mb.indexCount - 2 : 0) : mb.indexCount / 3;
            }
        }
        for (int l = 0; l < 3; ++l) trisLod[l] += t[l];
        if (h.flags & fmt::kModelFoliage) {
            ++foliage;
            if (lod2) {
                ++withLod2;
                foliageLod0 += t[0];
                foliageLod2 += t[2];
            }
        }
    }
    for (const auto& [name, loc] : pak.entries) {
        if (name.rfind("i_", 0) != 0) continue;
        uint32_t size = 0;
        const uint8_t* b = pak.find(name, size);
        fmt::ImpostorHeader h;
        if (size < sizeof(h)) {
            bad(name, "short");
            continue;
        }
        std::memcpy(&h, b, sizeof(h));
        wimpBytes += size;
        const std::string model = "m_" + name.substr(2);
        uint32_t msize = 0;
        const uint8_t* mb = pak.find(model, msize);
        fmt::ModelHeader mh{};
        if (mb && msize >= sizeof(mh)) std::memcpy(&mh, mb, sizeof(mh));
        const int rows = h.cols ? (h.views + h.cols - 1) / h.cols : 0;
        if (std::memcmp(h.magic, "WIMP", 4) != 0 || h.version != fmt::kImpostorVersion) bad(name, "magic/version");
        else if (!mb || !(mh.flags & fmt::kModelFoliage)) bad(name, "no foliage model m_<n>");
        else if ((h.views != 4 && h.views != 8 && h.views != 16) || h.cols != 4 || h.cellSize < 16 ||
                 (h.cellSize & (h.cellSize - 1)) || h.margin * 2 >= h.cellSize)
            bad(name, "views/cols/cell");
        else if (!std::isfinite(h.zMin) || !std::isfinite(h.zMax) || !std::isfinite(h.halfWidth) ||
                 !std::isfinite(h.axis[0]) || !std::isfinite(h.axis[1]) || h.zMax <= h.zMin || h.halfWidth <= 0.0f)
            bad(name, "extents");
        else {
            uint32_t ts = 0;
            const uint8_t* tb = pak.find("x_" + std::to_string(h.texture), ts);
            int w = 0, hh = 0;
            std::vector<uint8_t> rgba;
            if (!tb) bad(name, "missing atlas texture");
            else if (!decodeWtex(tb, ts, w, hh, rgba)) bad(name, "atlas is not WTEX T8");
            else if (w != h.cols * h.cellSize || hh != rows * h.cellSize) bad(name, "atlas size != cols*cell x rows*cell");
            else {
                ++impostors;
                atlasBytes += ts;
                if (!dumpDir.empty()) {
                    const std::string png = dumpDir + "/impostor_" + name.substr(2) + ".png";
                    if (writePng(png, w, hh, rgba)) std::printf("  dump %s (%dx%d, %u views)\n", png.c_str(), w, hh, h.views);
                }
            }
        }
    }
    std::printf("verify %s: models=%zu foliage=%zu lod2=%zu impostors=%zu errors=%d\n", path.c_str(), models, foliage,
                withLod2, impostors, errors);
    std::printf("  strip tris (incl. degenerate joins) lod0=%llu lod1=%llu lod2=%llu; foliage with lod2: lod0=%llu"
                " -> lod2=%llu (%.0f%%)\n",
                static_cast<unsigned long long>(trisLod[0]), static_cast<unsigned long long>(trisLod[1]),
                static_cast<unsigned long long>(trisLod[2]), static_cast<unsigned long long>(foliageLod0),
                static_cast<unsigned long long>(foliageLod2),
                foliageLod0 ? 100.0 * static_cast<double>(foliageLod2) / static_cast<double>(foliageLod0) : 0.0);
    std::printf("  impostor atlases %zu KB, headers %zu B, pak %zu KB\n", atlasBytes / 1024, wimpBytes,
                pak.bytes.size() / 1024);
    return errors;
}

} // namespace wowee::pspconv
