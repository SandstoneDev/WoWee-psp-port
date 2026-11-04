// A big WMO -> its own pak of chunks (include/rendering/psp/psp_bigwmo_format.hpp).
// Used by psp_model_conv's WmoBank for every WMO over --wmo-max-tris.
//
//  - groups and materials as WmoBank (upstream WMORenderer rules): texture1 -> 2 -> 3, blend 0 opaque /
//    1 alpha test / >= 2 blended, 0x01 unlit, 0x40 / 0x80 clamp, antiportal groups skipped; vertex light as WmoBank
//    (wmoVertexLight below, the same function WmoBank calls);
//  - the WMO is cut into cells (cellSize yards, local X/Y); a triangle belongs to the cell of its centroid; the
//    triangles of one group in one cell are split by a k-d tree into chunks of <= chunkTriangles; render triangles
//    are split to <= maxEdge yards first (the GE guard band: no vertex of a big triangle near the camera may land
//    past the 4096 px window), collision keeps the original triangles (the same k-d tree places them);
//  - a chunk: one strip per (texture, blend, flags) - batches merged by material -, s16 positions on the WMO's
//    lattice around the cell centre, u16 UV over the chunk's range, 5650 baked light;
//  - "l_<n>": the chunk's original triangles simplified (meshoptimizer, border locked, absolute error in yards)
//    when that saves >= 30 %; "s_<cell>": the cell's exterior groups simplified harder, untextured (the colour is
//    the light times the texture's mean colour) - what the city looks like from far outside;
//  - doodads: every MODD placement (all sets) with its MODR group, in the cell of its sphere centre ("dd_<cell>"),
//    the models from the map's doodads pak (the caller's ModelBank).
#pragma once

#include "pipeline/blp_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/psp/psp_bigwmo_format.hpp"
#include "rendering/psp/psp_wmo_landmarks.hpp"
#include "rendering/psp/psp_model_format.hpp"

#include "conv_common.hpp"
#include "model_bake.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::pspconv {

// Vertex light of a WMO group: interior groups (0x2000) max(MOCV, ambient, 0.15), exterior
// (0.5 + 0.5 sun) * max(MOCV, 0.25). WmoBank and the big-WMO bake both call this.
inline glm::vec3 wmoVertexLight(const pipeline::WMOVertex& v, bool interior, const glm::vec3& ambient) {
    const glm::vec3 mocv(v.color);
    if (interior) return glm::max(mocv, glm::max(ambient, glm::vec3(0.15f)));
    const glm::vec3 sun = glm::normalize(glm::vec3(0.35f, 0.25f, 0.9f));
    const float nl = glm::length(v.normal);
    const glm::vec3 n = nl > 1e-4f ? v.normal / nl : glm::vec3(0, 0, 1);
    return (0.5f + 0.5f * std::max(0.0f, glm::dot(n, sun))) * glm::max(mocv, glm::vec3(0.25f));
}

struct BigWmoOptions {
    float cellSize = 128.0f;       // yd
    uint32_t chunkTriangles = 768; // render triangles per chunk at most
    float maxEdge = 4.0f;          // yd: the "n_" split (guard band at near 1.0: 4 safe for cfg fov >= 66 - the default
                                   // 90 -, 3 for every fov; the GE guard-band rule, include psp_bigwmo_vis.hpp).
                                   // stays 4 = --wmo-max-edge (by design: the cfg fov
                                   // floor is 66; 3 yd would push mid-size WMOs into this path and grow "n_" ~1.8x).
    float baseEdge = 16.0f;        // yd: "c_" triangles longer than this are split too (tight chunk spheres:
                                   // fewer chunks fail the guard-band test, a smaller "n_" next to the camera)
    float maxChunkExtent = 40.0f;  // yd: a chunk of >= 64 triangles is split further when wider than this
    uint32_t maxNearTriangles = 6000;  // the "n_" of one chunk at most (a chunk is split further)
    bool indexed = true;           // indexed strips (kChunkIndexed)
    float lodRatio = 0.25f;        // "l_": target share of the original triangles
    float lodError = 0.35f;        // yd, absolute (drawn past wmolod 50 yd: ~2 px)
    float shellRatio = 0.015f;     // "s_": target share of a cell's triangles seen from outside
    float shellError = 8.0f;       // yd (drawn past wmoshell 160 yd; a capital city's route: 1 % / 10 yd lost towers)
};

// A doodad model from the map's ModelBank (psp_model_conv): its "m_" id (-1 none), sphere, particle effect.
struct BigWmoModelRef {
    int id = -1;
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    int effect = -1;
};

// What the doodads pak needs about a baked big WMO.
struct BigWmoBaked {
    bool ok = false;
    std::string pakRel;           // "bigwmo/<map>_<stem>.pak"
    glm::vec3 bmin{0.0f}, bmax{0.0f};  // local box
    struct Doodad {
        uint32_t index;
        uint16_t set;
        BigWmoModelRef model;
        glm::mat4 local;
    };
    std::vector<Doodad> doodads;  // every MODD placement with a model or an effect
    std::vector<std::pair<uint32_t, uint32_t>> sets;
    size_t triangles = 0, chunks = 0, bytes = 0;
};

class BigWmoBaker {
public:
    BigWmoBaker(std::string root, int texMax, BigWmoOptions opt) : root_(std::move(root)), texMax_(texMax), opt_(opt) {}

    // m: root and every group already loaded (WMOLoader). Writes <outDir>/<pakRel>.
    bool bake(const std::string& gamePath, const pipeline::WMOModel& m, const std::string& outDir,
              const std::string& pakRel, uint32_t stamp, const std::function<BigWmoModelRef(const std::string&)>& models,
              BigWmoBaked& out);

private:
    struct Mat {
        uint16_t texture = rendering::psp::kNoTexture;
        uint8_t blend = 0, flags = 0;
        glm::vec3 mean{1.0f};     // texture mean colour (shells)
        bool operator<(const Mat& o) const {
            return std::tie(texture, blend, flags) < std::tie(o.texture, o.blend, o.flags);
        }
    };
    struct Tri {
        uint32_t v[3];
        uint16_t mat;             // index into mats_
        uint8_t mopy;             // MOPY flags (collision)
        glm::vec3 centroid;
        uint32_t parent = 0;      // base: the original's index; near: the base triangle's index
        bool first = false;       // base: the first piece of its original (the original's chunk)
        bool outside = false;     // original: seen from outside (visibility pass)
    };
    struct GroupMesh {
        std::vector<pipeline::M2Vertex> verts;  // split vertices appended
        std::vector<glm::vec3> lit;
        std::vector<Tri> render, base, original;  // original: v index < originalVerts; base: split to baseEdge;
                                                  // render: base split to maxEdge (the "n_" meshes)
        size_t originalVerts = 0;
    };

    // ---- helpers ----
    static double lat(float v, double step) { return std::llround(static_cast<double>(v) / step); }
    int texture(const pipeline::WMOModel& m, const pipeline::WMOMaterial& mat, bool& alpha, glm::vec3& mean);
    glm::vec3 meanColour(const std::string& path);
    size_t markSeenOutside(std::vector<GroupMesh>& meshes, const glm::vec3& lo, const glm::vec3& hi);
    std::vector<uint8_t> buildMesh(const GroupMesh& gm, const std::vector<const Tri*>& tris, const glm::vec3& centre,
                                   double step, uint16_t cell, uint16_t flags, bool untextured,
                                   std::vector<uint16_t>* texList, uint32_t& triOut, uint32_t& clamps);

    std::string root_;
    int texMax_;
    BigWmoOptions opt_;
    std::vector<PakItem> textures_;
    TextureBank* tex_ = nullptr;
    std::vector<Mat> mats_;
    std::map<std::string, glm::vec3> means_;
    size_t statStripVerts_ = 0, statUniqueVerts_ = 0;  // all meshes: strip length vs distinct vertices (format choice)
};

inline glm::vec3 BigWmoBaker::meanColour(const std::string& path) {
    auto it = means_.find(path);
    if (it != means_.end()) return it->second;
    glm::vec3 c(0.6f);
    std::vector<uint8_t> bytes;
    if (readFile(root_ + "/" + lower(path), bytes)) {
        const pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
        if (!img.data.empty()) {
            glm::dvec3 s(0.0);
            const size_t n = static_cast<size_t>(img.width) * img.height;
            for (size_t i = 0; i < n; ++i) s += glm::dvec3(img.data[i * 4], img.data[i * 4 + 1], img.data[i * 4 + 2]);
            c = glm::vec3(s / (255.0 * static_cast<double>(std::max<size_t>(1, n))));
        }
    }
    means_.emplace(path, c);
    return c;
}

inline int BigWmoBaker::texture(const pipeline::WMOModel& m, const pipeline::WMOMaterial& mat, bool& alpha,
                                glm::vec3& mean) {
    for (uint32_t off : {mat.texture1, mat.texture2, mat.texture3}) {
        uint32_t idx = UINT32_MAX;
        auto it = m.textureOffsetToIndex.find(off);
        if (it != m.textureOffsetToIndex.end()) idx = it->second;
        else if (off < m.textures.size()) idx = off;
        if (idx >= m.textures.size() || m.textures[idx].empty()) continue;
        const int id = tex_->get(m.textures[idx], alpha, mat.blendMode == 0);  // as WmoBank::textureOf
        if (id >= 0) {
            mean = meanColour(m.textures[idx]);
            return id;
        }
    }
    return -1;
}

// The visibility pass: the WMO's original triangles rendered orthographically into an id buffer (1 px ~ 0.5 yd, at
// most 2048 px) from 8 azimuths x 4 elevations (5, 25, 50, 75 degrees) and straight down; a triangle that wins a
// pixel in any view is seen from outside. Returns how many.
inline size_t BigWmoBaker::markSeenOutside(std::vector<GroupMesh>& meshes, const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 centre = (lo + hi) * 0.5f;
    const float radius = std::max(1.0f, glm::length(hi - lo) * 0.5f);
    const int res = std::clamp(static_cast<int>(radius * 2.0f / 0.5f), 256, 2048);
    std::vector<float> depth(static_cast<size_t>(res) * res);
    std::vector<uint64_t> id(static_cast<size_t>(res) * res);
    std::vector<glm::vec3> dirs;
    for (float elev : {5.0f, 25.0f, 50.0f, 75.0f})
        for (int a = 0; a < 8; ++a) {
            const float az = glm::radians(45.0f * a + (elev == 25.0f || elev == 75.0f ? 22.5f : 0.0f));
            const float el = glm::radians(elev);
            dirs.emplace_back(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el));  // towards the eye
        }
    dirs.emplace_back(0.0f, 0.0f, 1.0f);
    const float scale = static_cast<float>(res) / (2.0f * radius);
    for (const glm::vec3& d : dirs) {
        const glm::vec3 up = std::fabs(d.z) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
        const glm::vec3 u = glm::normalize(glm::cross(up, d)), v = glm::cross(d, u);
        std::fill(depth.begin(), depth.end(), -1e30f);
        std::fill(id.begin(), id.end(), ~uint64_t{0});
        for (size_t gi = 0; gi < meshes.size(); ++gi) {
            GroupMesh& gm = meshes[gi];
            for (size_t ti = 0; ti < gm.original.size(); ++ti) {
                const Tri& t = gm.original[ti];
                glm::vec3 s[3];
                for (int k = 0; k < 3; ++k) {
                    const glm::vec3 p = gm.verts[t.v[k]].position - centre;
                    s[k] = glm::vec3((glm::dot(p, u) + radius) * scale, (glm::dot(p, v) + radius) * scale, glm::dot(p, d));
                }
                const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[1].y - s[0].y) * (s[2].x - s[0].x);
                if (std::fabs(area) < 1e-12f) continue;
                const int x0 = std::max(0, static_cast<int>(std::floor(std::min({s[0].x, s[1].x, s[2].x}))));
                const int x1 = std::min(res - 1, static_cast<int>(std::ceil(std::max({s[0].x, s[1].x, s[2].x}))));
                const int y0 = std::max(0, static_cast<int>(std::floor(std::min({s[0].y, s[1].y, s[2].y}))));
                const int y1 = std::min(res - 1, static_cast<int>(std::ceil(std::max({s[0].y, s[1].y, s[2].y}))));
                const uint64_t key = (static_cast<uint64_t>(gi) << 32) | ti;
                bool any = false;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        const float w0 = ((s[1].x - px) * (s[2].y - py) - (s[1].y - py) * (s[2].x - px)) / area;
                        const float w1 = ((s[2].x - px) * (s[0].y - py) - (s[2].y - py) * (s[0].x - px)) / area;
                        const float w2 = 1.0f - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        any = true;
                        const float z = w0 * s[0].z + w1 * s[1].z + w2 * s[2].z;
                        const size_t o = static_cast<size_t>(y) * res + x;
                        if (z > depth[o]) {
                            depth[o] = z;
                            id[o] = key;
                        }
                    }
                // A triangle smaller than a pixel (no pixel centre inside): its nearest pixel, as a candidate.
                if (!any) {
                    const int x = std::clamp(static_cast<int>((s[0].x + s[1].x + s[2].x) / 3.0f), 0, res - 1);
                    const int y = std::clamp(static_cast<int>((s[0].y + s[1].y + s[2].y) / 3.0f), 0, res - 1);
                    const float z = (s[0].z + s[1].z + s[2].z) / 3.0f;
                    const size_t o = static_cast<size_t>(y) * res + x;
                    if (z > depth[o]) {
                        depth[o] = z;
                        id[o] = key;
                    }
                }
            }
        }
        for (uint64_t k : id)
            if (k != ~uint64_t{0}) meshes[k >> 32].original[k & 0xFFFFFFFFu].outside = true;
    }
    size_t n = 0;
    for (const GroupMesh& gm : meshes)
        for (const Tri& t : gm.original) n += t.outside ? 1u : 0u;
    return n;
}

// One WBCK blob from `tris` (render triangles of one group mesh): strips per material, lattice positions.
inline std::vector<uint8_t> BigWmoBaker::buildMesh(const GroupMesh& gm, const std::vector<const Tri*>& tris,
                                                   const glm::vec3& centre, double step, uint16_t cell, uint16_t flags,
                                                   bool untextured, std::vector<uint16_t>* texList, uint32_t& triOut,
                                                   uint32_t& clamps) {
    namespace fmt = rendering::psp;
    // Materials in order (untextured: one batch per blend class).
    std::map<std::tuple<uint16_t, uint8_t, uint8_t>, std::vector<const Tri*>> byMat;
    for (const Tri* t : tris) {
        const Mat& mt = mats_[t->mat];
        const uint16_t tx = untextured ? fmt::kNoTexture : mt.texture;
        // One type in the ?: (an enum against uint8_t: -Wextra).
        const uint8_t bl =
            untextured ? static_cast<uint8_t>(mt.blend == fmt::kBlendAlpha ? fmt::kBlendAlpha : fmt::kBlendOpaque) : mt.blend;
        const uint8_t fl = untextured ? static_cast<uint8_t>(mt.flags & fmt::kBatchUnlit) : mt.flags;
        byMat[{tx, bl, fl}].push_back(t);
    }
    // Chunk-local vertices: (source vertex, material) - a vertex shared by two materials is emitted per material
    // (an untextured shell colours it by its material's mean).
    struct LV {
        uint32_t src;
        uint16_t mat;
    };
    std::vector<LV> lverts;
    std::unordered_map<uint64_t, uint32_t> lmap;
    glm::vec2 tmin(1e30f), tmax(-1e30f);
    std::vector<std::pair<std::tuple<uint16_t, uint8_t, uint8_t>, std::vector<unsigned int>>> batches;
    for (auto& [key, list] : byMat) {
        std::vector<unsigned int> idx;
        for (const Tri* t : list) {
            for (int k = 0; k < 3; ++k) {
                const uint64_t lk = (static_cast<uint64_t>(t->v[k]) << 16) | t->mat;
                auto it = lmap.find(lk);
                uint32_t li;
                if (it != lmap.end()) {
                    li = it->second;
                } else {
                    li = static_cast<uint32_t>(lverts.size());
                    lverts.push_back({t->v[k], t->mat});
                    lmap.emplace(lk, li);
                    const glm::vec2 uv = gm.verts[t->v[k]].texCoords[0];
                    tmin = glm::min(tmin, uv);
                    tmax = glm::max(tmax, uv);
                }
                idx.push_back(li);
            }
        }
        batches.emplace_back(key, std::move(idx));
    }
    if (lverts.empty()) return {};
    if (untextured) {
        tmin = glm::vec2(0.0f);
        tmax = glm::vec2(1.0f);
    }
    const glm::vec2 trange = glm::max(tmax - tmin, glm::vec2(1e-4f));
    // Lattice positions around the cell centre.
    const double cx = lat(centre.x, step), cy = lat(centre.y, step), cz = lat(centre.z, step);
    std::vector<fmt::ModelVertex> verts(lverts.size());
    for (size_t i = 0; i < lverts.size(); ++i) {
        const pipeline::M2Vertex& v = gm.verts[lverts[i].src];
        fmt::ModelVertex& o = verts[i];
        const double q[3] = {lat(v.position.x, step) - cx, lat(v.position.y, step) - cy, lat(v.position.z, step) - cz};
        int16_t s[3];
        for (int a = 0; a < 3; ++a) {
            if (q[a] < -32767.0 || q[a] > 32767.0) ++clamps;
            s[a] = static_cast<int16_t>(std::clamp(q[a], -32767.0, 32767.0));
        }
        o.x = s[0];
        o.y = s[1];
        o.z = s[2];
        const glm::vec2 uv = v.texCoords[0];
        o.u = static_cast<uint16_t>(std::lround(std::clamp((uv.x - tmin.x) / trange.x, 0.0f, 1.0f) * 65535.0f));
        o.v = static_cast<uint16_t>(std::lround(std::clamp((uv.y - tmin.y) / trange.y, 0.0f, 1.0f) * 65535.0f));
        const Mat& mt = mats_[lverts[i].mat];
        glm::vec3 light = (mt.flags & fmt::kBatchUnlit) ? glm::vec3(1.0f) : glm::min(gm.lit[lverts[i].src], glm::vec3(1.0f));
        if (untextured) light = glm::min(light * mt.mean, glm::vec3(1.0f));
        o.color = to565(Rgb{light.r, light.g, light.b, 1.0f});
    }
    // Strips (as WmdlBuilder: meshoptimizer, joined by degenerate triangles inside a batch); indexed: the distinct
    // vertices once, the strip as u16 indices.
    std::vector<fmt::ModelBatch> outBatches;
    std::vector<fmt::ModelVertex> stripVerts;
    std::vector<uint16_t> stripIdx;
    const bool indexed = opt_.indexed && verts.size() <= 65535;
    std::vector<uint16_t> texs;
    triOut = 0;
    for (auto& [key, idx] : batches) {
        std::vector<unsigned int> tri = idx;
        meshopt_optimizeVertexCacheStrip(tri.data(), tri.data(), tri.size(), verts.size());
        std::vector<unsigned int> strip(meshopt_stripifyBound(tri.size()));
        strip.resize(meshopt_stripify(strip.data(), tri.data(), tri.size(), verts.size(), 0));
        if (!indexed && stripVerts.size() + strip.size() > 65535) {
            std::fprintf(stderr, "  bigwmo: chunk over 65535 strip vertices (batch dropped)\n");
            continue;
        }
        fmt::ModelBatch b{};
        b.indexStart = static_cast<uint32_t>(indexed ? stripIdx.size() : stripVerts.size());
        b.indexCount = static_cast<uint32_t>(strip.size());
        b.texture = std::get<0>(key);
        b.blend = std::get<1>(key);
        b.flags = std::get<2>(key);
        if (indexed)
            for (unsigned int i : strip) stripIdx.push_back(static_cast<uint16_t>(i));
        else
            for (unsigned int i : strip) stripVerts.push_back(verts[i]);
        statStripVerts_ += strip.size();
        outBatches.push_back(b);
        triOut += static_cast<uint32_t>(idx.size() / 3);
        if (b.texture != fmt::kNoTexture && std::find(texs.begin(), texs.end(), b.texture) == texs.end())
            texs.push_back(b.texture);
    }
    if (texList) *texList = texs;
    statUniqueVerts_ += verts.size();
    if (indexed) stripVerts = verts;
    fmt::ChunkMeshHeader h{};
    std::memcpy(h.magic, "WBCK", 4);
    h.version = fmt::kBigWmoVersion;
    h.flags = static_cast<uint16_t>(flags | (indexed ? fmt::kChunkIndexed : 0));
    h.vertexCount = static_cast<uint16_t>(stripVerts.size());
    h.batchCount = static_cast<uint16_t>(outBatches.size());
    h.textureCount = static_cast<uint16_t>(texs.size());
    h.cell = cell;
    h.uvMin[0] = tmin.x;
    h.uvMin[1] = tmin.y;
    h.uvScale[0] = trange.x * 32768.0f / 65535.0f;
    h.uvScale[1] = trange.y * 32768.0f / 65535.0f;
    h.triangles = triOut;
    std::vector<uint8_t> blob(sizeof(h));
    h.textureOffset = static_cast<uint32_t>(blob.size());
    for (uint16_t t : texs) put<uint16_t>(blob, t);
    alignTo(blob, 4);
    h.batchOffset = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(outBatches.data()),
                reinterpret_cast<const uint8_t*>(outBatches.data() + outBatches.size()));
    alignTo(blob, 16);
    h.vertexOffset = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(stripVerts.data()),
                reinterpret_cast<const uint8_t*>(stripVerts.data() + stripVerts.size()));
    if (indexed) {
        alignTo(blob, 4);
        h.indexOffset = static_cast<uint32_t>(blob.size());
        h.indexCount = static_cast<uint32_t>(stripIdx.size());
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(stripIdx.data()),
                    reinterpret_cast<const uint8_t*>(stripIdx.data() + stripIdx.size()));
    }
    std::memcpy(blob.data(), &h, sizeof(h));
    return blob;
}

// TriangleSplitter (model_bake.hpp) with 32-bit indices and no vertex limit: a big WMO group gains tens of thousands
// of midpoints; a 16-bit limit would stop splitting - and the guard band - silently.
class BigSplitter {
public:
    BigSplitter(std::vector<pipeline::M2Vertex>& v, std::vector<glm::vec3>& lit, float maxEdge)
        : v_(v), lit_(lit), max2_(maxEdge * maxEdge) {}
    void split(uint32_t a, uint32_t b, uint32_t c, std::vector<uint32_t>& out, int depth = 0) {
        const float ab = d2(a, b), bc = d2(b, c), ca = d2(c, a);
        const float longest = std::max({ab, bc, ca});
        if (longest <= max2_ || depth >= 16) {
            out.insert(out.end(), {a, b, c});
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
};

// MOGP sub-chunk payload of a group file (MODR, ...): empty when absent.
inline std::vector<uint8_t> wmoGroupSubChunk(const std::vector<uint8_t>& b, const char* fourcc) {
    std::vector<uint8_t> out;
    const uint32_t want = (static_cast<uint32_t>(fourcc[0]) << 24) | (static_cast<uint32_t>(fourcc[1]) << 16) |
                          (static_cast<uint32_t>(fourcc[2]) << 8) | static_cast<uint32_t>(fourcc[3]);
    size_t off = 0;
    while (off + 8 <= b.size()) {
        uint32_t id, sz;
        std::memcpy(&id, &b[off], 4);
        std::memcpy(&sz, &b[off + 4], 4);
        const size_t end = off + 8 + sz;
        if (end > b.size()) break;
        if (id == 0x4D4F4750u) {  // MOGP: header 68 bytes, then sub-chunks
            size_t so = off + 8 + 68;
            while (so + 8 <= end) {
                uint32_t sid, ssz;
                std::memcpy(&sid, &b[so], 4);
                std::memcpy(&ssz, &b[so + 4], 4);
                if (so + 8 + ssz > end) break;
                if (sid == want) {
                    out.assign(b.begin() + static_cast<std::ptrdiff_t>(so + 8),
                               b.begin() + static_cast<std::ptrdiff_t>(so + 8 + ssz));
                    return out;
                }
                so += 8 + ssz;
            }
        }
        off = end;
    }
    return out;
}

inline bool BigWmoBaker::bake(const std::string& gamePath, const pipeline::WMOModel& m, const std::string& outDir,
                              const std::string& pakRel, uint32_t stamp,
                              const std::function<BigWmoModelRef(const std::string&)>& models, BigWmoBaked& out) {
    namespace fmt = rendering::psp;
    out = BigWmoBaked{};
    out.pakRel = pakRel;
    textures_.clear();
    mats_.clear();
    TextureBank tex(root_, texMax_, textures_);
    tex_ = &tex;
    const std::string key = lower(gamePath);

    // Root bytes again: MOPT planes (upstream's WMOPortal keeps no plane - its fields read the plane's first
    // bytes) and the raw MOPR.
    std::vector<uint8_t> rootBytes;
    readFile(root_ + "/" + key, rootBytes);
    std::vector<fmt::BigWmoPortal> portals;
    std::vector<fmt::BigWmoPortalRef> refs;
    {
        size_t off = 0;
        while (off + 8 <= rootBytes.size()) {
            uint32_t id, sz;
            std::memcpy(&id, &rootBytes[off], 4);
            std::memcpy(&sz, &rootBytes[off + 4], 4);
            if (off + 8 + sz > rootBytes.size()) break;
            const uint8_t* p = &rootBytes[off + 8];
            if (id == 0x4D4F5054u) {  // MOPT: u16 start, u16 count, C4Plane (normal, distance)
                for (uint32_t i = 0; i + 20 <= sz; i += 20) {
                    fmt::BigWmoPortal pt{};
                    std::memcpy(&pt.vertexStart, p + i, 2);
                    std::memcpy(&pt.vertexCount, p + i + 2, 2);
                    std::memcpy(pt.plane, p + i + 4, 16);
                    portals.push_back(pt);
                }
            } else if (id == 0x4D4F5052u) {  // MOPR: u16 portal, u16 group, i16 side, u16 filler
                for (uint32_t i = 0; i + 8 <= sz; i += 8) {
                    fmt::BigWmoPortalRef r{};
                    std::memcpy(&r.portal, p + i, 2);
                    std::memcpy(&r.group, p + i + 2, 2);
                    std::memcpy(&r.side, p + i + 4, 2);
                    refs.push_back(r);
                }
            }
            off += 8 + sz;
        }
    }

    // ---- per group: vertices, light, materials, render (split) and original triangles ----
    const size_t ng = m.groups.size();
    std::vector<GroupMesh> meshes(ng);
    std::map<std::tuple<uint16_t, uint8_t, uint8_t>, uint16_t> matIds;
    std::vector<std::vector<uint16_t>> modr(ng);
    size_t srcTris = 0, splitTris = 0, baseTris = 0;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (size_t gi = 0; gi < ng; ++gi) {
        const pipeline::WMOGroup& g = m.groups[gi];
        // MODR from the group file.
        for (const std::string& cand : pipeline::wmoGroupCandidates(gamePath, static_cast<uint32_t>(gi))) {
            std::vector<uint8_t> gb;
            if (readFile(root_ + "/" + lower(cand), gb)) {
                const std::vector<uint8_t> r = wmoGroupSubChunk(gb, "MODR");
                for (size_t i = 0; i + 1 < r.size(); i += 2) modr[gi].push_back(static_cast<uint16_t>(r[i] | (r[i + 1] << 8)));
                break;
            }
        }
        if (g.vertices.empty() || g.indices.empty() || g.batches.empty()) continue;
        if (g.flags & fmt::kWmoGroupAntiportal) continue;  // upstream: antiportal geometry is never drawn
        GroupMesh& gm = meshes[gi];
        const bool interior = (g.flags & 0x2000u) != 0;
        gm.verts.resize(g.vertices.size());
        gm.lit.resize(g.vertices.size());
        for (size_t i = 0; i < g.vertices.size(); ++i) {
            pipeline::M2Vertex& o = gm.verts[i];
            o = pipeline::M2Vertex{};
            o.position = g.vertices[i].position;
            o.normal = g.vertices[i].normal;
            o.texCoords[0] = o.texCoords[1] = g.vertices[i].texCoord;
            gm.lit[i] = wmoVertexLight(g.vertices[i], interior, m.ambientColor);
            lo = glm::min(lo, o.position);
            hi = glm::max(hi, o.position);
        }
        gm.originalVerts = gm.verts.size();
        BigSplitter baseSplit(gm.verts, gm.lit, opt_.baseEdge), nearSplit(gm.verts, gm.lit, opt_.maxEdge);
        for (const auto& b : g.batches) {
            if (b.indexCount < 3 || b.startIndex + b.indexCount > g.indices.size()) continue;
            Mat mt;
            uint32_t matFlags = 0, blend = 0;
            bool alpha = false;
            if (b.materialId < m.materials.size()) {
                const auto& mat = m.materials[b.materialId];
                matFlags = mat.flags;
                blend = mat.blendMode;
                const int id = texture(m, mat, alpha, mt.mean);
                if (id >= 0) mt.texture = static_cast<uint16_t>(id);
            }
            mt.blend = blend == 0 ? fmt::kBlendOpaque : blend == 1 ? fmt::kBlendAlphaKey : fmt::kBlendAlpha;
            if (mt.blend == fmt::kBlendAlphaKey && !alpha) mt.blend = fmt::kBlendOpaque;
            mt.flags = static_cast<uint8_t>(((matFlags & 0x40u) ? 0 : fmt::kBatchWrapU) |
                                            ((matFlags & 0x80u) ? 0 : fmt::kBatchWrapV) |
                                            ((matFlags & 0x01u) ? fmt::kBatchUnlit : 0));
            const auto mk = std::make_tuple(mt.texture, mt.blend, mt.flags);
            auto it = matIds.find(mk);
            uint16_t matId;
            if (it != matIds.end()) {
                matId = it->second;
            } else {
                matId = static_cast<uint16_t>(mats_.size());
                mats_.push_back(mt);
                matIds.emplace(mk, matId);
            }
            for (uint32_t k = 0; k + 2 < b.indexCount; k += 3) {
                const uint32_t t = (b.startIndex + k) / 3;
                const uint16_t ia = g.indices[b.startIndex + k], ib = g.indices[b.startIndex + k + 1],
                               ic = g.indices[b.startIndex + k + 2];
                if (ia >= gm.originalVerts || ib >= gm.originalVerts || ic >= gm.originalVerts) continue;
                Tri o{{ia, ib, ic}, matId, static_cast<uint8_t>(t < g.triFlags.size() ? g.triFlags[t] : 0), {}};
                o.centroid = (gm.verts[ia].position + gm.verts[ib].position + gm.verts[ic].position) / 3.0f;
                gm.original.push_back(o);
                ++srcTris;
                std::vector<uint32_t> bp;
                baseSplit.split(ia, ib, ic, bp);
                for (size_t q = 0; q + 2 < bp.size(); q += 3) {
                    Tri bt{{bp[q], bp[q + 1], bp[q + 2]}, matId, o.mopy, {}, static_cast<uint32_t>(gm.original.size() - 1), q == 0};
                    bt.centroid = (gm.verts[bp[q]].position + gm.verts[bp[q + 1]].position + gm.verts[bp[q + 2]].position) / 3.0f;
                    gm.base.push_back(bt);
                    ++baseTris;
                    std::vector<uint32_t> sp;
                    nearSplit.split(bp[q], bp[q + 1], bp[q + 2], sp);
                    for (size_t w = 0; w + 2 < sp.size(); w += 3) {
                        Tri r{{sp[w], sp[w + 1], sp[w + 2]}, matId, o.mopy, {}, static_cast<uint32_t>(gm.base.size() - 1)};
                        r.centroid = (gm.verts[sp[w]].position + gm.verts[sp[w + 1]].position + gm.verts[sp[w + 2]].position) / 3.0f;
                        gm.render.push_back(r);
                        ++splitTris;
                    }
                }
            }
        }
    }
    if (srcTris == 0) return false;
    out.bmin = lo;
    out.bmax = hi;
    const size_t seenOutside = markSeenOutside(meshes, lo, hi);

    // ---- cells ----
    const float cs = opt_.cellSize;
    const glm::vec2 origin(std::floor(lo.x / cs) * cs, std::floor(lo.y / cs) * cs);
    const int cellsX = std::max(1, static_cast<int>(std::ceil((hi.x - origin.x) / cs)));
    const int cellsY = std::max(1, static_cast<int>(std::ceil((hi.y - origin.y) / cs)));
    auto cellOf = [&](const glm::vec3& p) {
        const int ix = std::clamp(static_cast<int>(std::floor((p.x - origin.x) / cs)), 0, cellsX - 1);
        const int iy = std::clamp(static_cast<int>(std::floor((p.y - origin.y) / cs)), 0, cellsY - 1);
        return iy * cellsX + ix;
    };
    // Cell boxes over the vertices of its triangles (centroid rule of the original triangle; the split ones lie
    // on it) - the lattice must hold them.
    std::vector<glm::vec3> clo(static_cast<size_t>(cellsX) * cellsY, glm::vec3(1e30f)),
        chi(static_cast<size_t>(cellsX) * cellsY, glm::vec3(-1e30f));
    for (const GroupMesh& gm : meshes)
        for (const Tri& t : gm.base) {
            const int c = cellOf(t.centroid);
            for (uint32_t v : t.v) {
                clo[c] = glm::min(clo[c], gm.verts[v].position);
                chi[c] = glm::max(chi[c], gm.verts[v].position);
            }
            // The original triangle goes with its first piece's chunk (far mesh "l_", shell "s_"): its corners too.
            if (t.first)
                for (uint32_t v : gm.original[t.parent].v) {
                    clo[c] = glm::min(clo[c], gm.verts[v].position);
                    chi[c] = glm::max(chi[c], gm.verts[v].position);
                }
        }
    float need = 1.0f;
    std::vector<glm::vec3> centres(clo.size(), glm::vec3(0.0f));
    for (size_t c = 0; c < clo.size(); ++c) {
        if (clo[c].x > chi[c].x) continue;
        centres[c] = (clo[c] + chi[c]) * 0.5f;
        need = std::max({need, chi[c].x - centres[c].x, chi[c].y - centres[c].y, chi[c].z - centres[c].z});
    }
    double step = 1.0 / 4096.0;
    while (need / step > 32000.0) step *= 2.0;
    for (glm::vec3& c : centres)
        for (int a = 0; a < 3; ++a) c[a] = static_cast<float>(lat(c[a], step) * step);

    // ---- chunks: group x cell, then a k-d split of the original triangles; the split ones follow their parent ----
    struct ChunkOut {
        uint16_t group;
        int cell;  // the grid index (iy * cellsX + ix) before renumbering - an uint16_t wrapped past
                   // 65535 grid cells (--bigwmo-cell small on a wide WMO) and mixed chunks of far cells
        std::vector<const Tri*> base, near, original;
    };
    std::vector<ChunkOut> chunks;
    for (size_t gi = 0; gi < ng; ++gi) {
        GroupMesh& gm = meshes[gi];
        if (gm.original.empty()) continue;
        std::vector<std::vector<const Tri*>> kids(gm.base.size());
        for (const Tri& t : gm.render) kids[t.parent].push_back(&t);
        // Base triangles by cell, and in a cell those seen from outside apart from the rest: a chunk is then wholly
        // outer surface or wholly interior, so the outside filter (kChunkSeenOutside) and the shells drop real work.
        std::map<int, std::vector<const Tri*>> oByCell;
        for (const Tri& t : gm.base) oByCell[cellOf(t.centroid) * 2 + (gm.original[t.parent].outside ? 1 : 0)].push_back(&t);
        for (auto& [cellKey, olist] : oByCell) {
            const int cell = cellKey >> 1;
            std::vector<std::vector<const Tri*>> work;
            work.push_back(olist);
            while (!work.empty()) {
                std::vector<const Tri*> nd = std::move(work.back());
                work.pop_back();
                glm::vec3 a(1e30f), b(-1e30f), va(1e30f), vb(-1e30f);
                size_t nearCount = 0;
                for (const Tri* t : nd) {
                    a = glm::min(a, t->centroid);
                    b = glm::max(b, t->centroid);
                    for (uint32_t v : t->v) {
                        va = glm::min(va, gm.verts[v].position);
                        vb = glm::max(vb, gm.verts[v].position);
                    }
                    nearCount += kids[static_cast<size_t>(t - gm.base.data())].size();
                }
                const glm::vec3 vext = vb - va;
                const bool wide = nd.size() >= 64 && std::max({vext.x, vext.y, vext.z}) > opt_.maxChunkExtent;
                if (nd.size() < 8 || (nd.size() <= opt_.chunkTriangles && !wide && nearCount <= opt_.maxNearTriangles)) {
                    ChunkOut co{static_cast<uint16_t>(gi), cell, std::move(nd), {}, {}};
                    for (const Tri* t : co.base) {
                        const size_t pi = static_cast<size_t>(t - gm.base.data());
                        co.near.insert(co.near.end(), kids[pi].begin(), kids[pi].end());
                        if (t->first) co.original.push_back(&gm.original[t->parent]);
                    }
                    chunks.push_back(std::move(co));
                    continue;
                }
                const glm::vec3 ext = b - a;
                const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
                std::vector<float> cs2;
                cs2.reserve(nd.size());
                for (const Tri* t : nd) cs2.push_back(t->centroid[axis]);
                std::nth_element(cs2.begin(), cs2.begin() + static_cast<std::ptrdiff_t>(cs2.size() / 2), cs2.end());
                const float split = cs2[cs2.size() / 2];
                std::vector<const Tri*> l, r;
                for (const Tri* t : nd) (t->centroid[axis] < split ? l : r).push_back(t);
                if (l.empty() || r.empty()) {  // all on one plane: halve by order
                    l.assign(nd.begin(), nd.begin() + static_cast<std::ptrdiff_t>(nd.size() / 2));
                    r.assign(nd.begin() + static_cast<std::ptrdiff_t>(nd.size() / 2), nd.end());
                }
                work.push_back(std::move(l));
                work.push_back(std::move(r));
            }
        }
    }
    std::stable_sort(chunks.begin(), chunks.end(), [](const ChunkOut& a, const ChunkOut& b) {
        return a.cell != b.cell ? a.cell < b.cell : a.group < b.group;
    });

    // Used cells, renumbered (only cells with chunks or doodads are written).
    std::map<int, uint16_t> cellIndex;
    for (const ChunkOut& c : chunks) cellIndex.emplace(c.cell, 0);

    // ---- doodads ----
    std::vector<uint16_t> doodadGroup(m.doodads.size(), 0xFFFF);
    for (size_t gi = 0; gi < ng; ++gi)
        for (uint16_t d : modr[gi])
            if (d < doodadGroup.size() && doodadGroup[d] == 0xFFFF) doodadGroup[d] = static_cast<uint16_t>(gi);
    std::map<int, std::vector<fmt::BigWmoDoodad>> doodadsByCell;
    std::vector<fmt::BigWmoDoodad> landmarks;
    for (const auto& set : m.doodadSets) out.sets.emplace_back(set.startIndex, set.count);
    size_t doodadTotal = 0, doodadNoModel = 0;
    for (size_t i = 0; i < m.doodads.size(); ++i) {
        const pipeline::WMODoodad& d = m.doodads[i];
        auto nameIt = m.doodadNames.find(d.nameIndex);
        if (nameIt == m.doodadNames.end()) continue;
        const BigWmoModelRef mr = models(nameIt->second);
        uint16_t set = 0xFFFF;
        for (size_t s = 0; s < out.sets.size(); ++s)
            if (i >= out.sets[s].first && i < out.sets[s].first + out.sets[s].second) {
                set = static_cast<uint16_t>(s);
                break;
            }
        const glm::quat q(d.rotation.w, d.rotation.x, d.rotation.y, d.rotation.z);
        const glm::mat4 local = glm::scale(glm::translate(glm::mat4(1.0f), d.position) * glm::mat4_cast(q), glm::vec3(d.scale));
        if (mr.id >= 0 || mr.effect >= 0) out.doodads.push_back({static_cast<uint32_t>(i), set, mr, local});
        if (mr.id < 0) {
            ++doodadNoModel;
            continue;
        }
        fmt::BigWmoDoodad o{};
        o.model = static_cast<uint16_t>(mr.id);
        o.group = doodadGroup[i];
        o.set = set;
        o.index = static_cast<uint32_t>(i);
        const glm::mat3 ax(local);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) o.axes[c * 3 + r] = ax[c][r];
        for (int a = 0; a < 3; ++a) o.position[a] = local[3][a];
        const glm::vec3 sc = glm::vec3(local[3]) + ax * mr.center;
        o.sphere[0] = sc.x;
        o.sphere[1] = sc.y;
        o.sphere[2] = sc.z;
        o.sphere[3] = mr.radius * std::max({glm::length(ax[0]), glm::length(ax[1]), glm::length(ax[2])});
        const int cell = cellOf(sc);
        doodadsByCell[cell].push_back(o);
        if (fmt::isWmoLandmark(nameIt->second) && landmarks.size() < 16) landmarks.push_back(o);
        cellIndex.emplace(cell, 0);
        ++doodadTotal;
    }
    // The renumbered cells are u16 in the format (BigWmoHeader::cellCount, BigWmoChunk::cell,
    // ChunkMeshHeader::cell), a chunk index 24 bits in the PSP's request key (psp_bigwmo.cpp kKeyShift).
    if (cellIndex.size() > 65535 || chunks.size() > 0xFFFFFFu) {
        std::fprintf(stderr, "  bigwmo %s: %zu cells, %zu chunks (at most 65535 / 16777215): a larger --bigwmo-cell\n",
                     key.c_str(), cellIndex.size(), chunks.size());
        return false;
    }
    uint16_t nextCell = 0;
    for (auto& [c, idx] : cellIndex) idx = nextCell++;

    // ---- blobs ----
    std::vector<PakItem> items;
    std::vector<fmt::BigWmoChunk> chunkRecs(chunks.size());
    std::vector<std::string> chunkLodName(chunks.size()), chunkNearName(chunks.size());
    std::vector<fmt::BigWmoCell> cellRecs(cellIndex.size());
    for (auto& [c, idx] : cellIndex) {
        fmt::BigWmoCell& cr = cellRecs[idx];
        cr.ix = static_cast<int16_t>(c % cellsX);
        cr.iy = static_cast<int16_t>(c / cellsX);
        for (int a = 0; a < 3; ++a) {
            cr.center[a] = centres[c][a];
            cr.bmin[a] = 1e30f;
            cr.bmax[a] = -1e30f;
        }
        cr.chunkStart = 0;
        cr.chunkCount = 0;
    }
    uint32_t clamps = 0;
    size_t meshBytes = 0, lodBytes = 0, colBytes = 0, lodTris = 0, totalTris = 0, shellTris = 0, shellBytes = 0;
    size_t nearTris = 0, nearBytes = 0, chunksSeenOutside = 0;
    for (size_t ci = 0; ci < chunks.size(); ++ci) {
        const ChunkOut& c = chunks[ci];
        const uint16_t cell = cellIndex[c.cell];
        fmt::BigWmoCell& cr = cellRecs[cell];
        if (cr.chunkCount == 0) cr.chunkStart = static_cast<uint32_t>(ci);
        ++cr.chunkCount;
        const GroupMesh& gm = meshes[c.group];
        const glm::vec3 centre = centres[c.cell];
        fmt::BigWmoChunk& rec = chunkRecs[ci];
        rec.group = c.group;
        rec.cell = cell;
        uint32_t seen = 0;
        for (const Tri* t : c.original) seen += t->outside ? 1u : 0u;
        rec.outsideTriangles = static_cast<uint16_t>(std::min<uint32_t>(seen, 65535));
        if (seen) {
            rec.flags = fmt::kChunkSeenOutside;
            ++chunksSeenOutside;
        }
        std::vector<uint16_t> texList;
        uint32_t tris = 0;
        std::vector<uint8_t> mesh = buildMesh(gm, c.base, centre, step, cell, 0, false, &texList, tris, clamps);
        rec.triangles = static_cast<uint16_t>(std::min<uint32_t>(tris, 65535));
        rec.textureCount = static_cast<uint16_t>(texList.size());
        totalTris += tris;
        meshBytes += mesh.size();
        // Box in lattice units around the cell centre, rounded outwards.
        glm::vec3 blo(1e30f), bhi(-1e30f);
        for (const Tri* t : c.base)
            for (uint32_t v : t->v) {
                blo = glm::min(blo, gm.verts[v].position);
                bhi = glm::max(bhi, gm.verts[v].position);
            }
        for (const Tri* t : c.original)  // the far mesh "l_" draws them whole
            for (uint32_t v : t->v) {
                blo = glm::min(blo, gm.verts[v].position);
                bhi = glm::max(bhi, gm.verts[v].position);
            }
        for (int a = 0; a < 3; ++a) {
            rec.bmin[a] = static_cast<int16_t>(std::clamp(std::floor((blo[a] - centre[a]) / step) - 1.0, -32767.0, 32767.0));
            rec.bmax[a] = static_cast<int16_t>(std::clamp(std::ceil((bhi[a] - centre[a]) / step) + 1.0, -32767.0, 32767.0));
            cr.bmin[a] = std::min(cr.bmin[a], blo[a]);
            cr.bmax[a] = std::max(cr.bmax[a], bhi[a]);
        }
        items.push_back({"c_" + std::to_string(ci), kKindModel, std::move(mesh)});
        // Guard band: the split triangles, when splitting changed anything.
        if (c.near.size() > c.base.size()) {
            uint32_t nt = 0;
            std::vector<uint8_t> nm = buildMesh(gm, c.near, centre, step, cell, fmt::kChunkNear, false, nullptr, nt, clamps);
            if (!nm.empty()) {
                rec.nearTriangles = static_cast<uint16_t>(std::min<uint32_t>(nt, 65535));
                nearTris += nt;
                nearBytes += nm.size();
                chunkNearName[ci] = "n_" + std::to_string(ci);
                items.push_back({chunkNearName[ci], kKindModel, std::move(nm)});
            }
        }
        // Collision: the original triangles, WMO-local floats, wall rule as WmoBank (MOPY 0x08, or 0x20 without
        // 0x04; a group with no such triangle blocks with all of them - decided per group).
        if (!c.original.empty()) {
            bool groupBlocks = false;
            for (const Tri& t : gm.original)
                if ((t.mopy & 0x08) || ((t.mopy & 0x20) && !(t.mopy & 0x04))) {
                    groupBlocks = true;
                    break;
                }
            std::vector<glm::vec3> cverts;
            std::unordered_map<uint32_t, uint16_t> cmap;
            std::vector<fmt::CollisionTri> ctris;
            for (const Tri* t : c.original) {
                uint16_t ids[3];
                for (int k = 0; k < 3; ++k) {
                    auto it = cmap.find(t->v[k]);
                    if (it == cmap.end()) {
                        it = cmap.emplace(t->v[k], static_cast<uint16_t>(cverts.size())).first;
                        cverts.push_back(gm.verts[t->v[k]].position);
                    }
                    ids[k] = it->second;
                }
                const bool blocks = !groupBlocks || (t->mopy & 0x08) || ((t->mopy & 0x20) && !(t->mopy & 0x04));
                ctris.push_back(fmt::CollisionTri{ids[0], ids[1], ids[2], static_cast<uint8_t>(blocks ? fmt::kColBlocksWalls : 0), 0});
            }
            std::vector<uint8_t> col = buildCollision(cverts, ctris);
            if (!col.empty()) {
                colBytes += col.size();
                items.push_back({"q_" + std::to_string(ci), kKindModel, std::move(col)});
            }
            // Far LOD: the original triangles per material, simplified with the chunk border locked.
            std::map<uint16_t, std::vector<unsigned int>> byMat;
            std::unordered_map<uint32_t, uint32_t> vm;
            std::vector<float> pos;
            std::vector<uint32_t> back;
            for (const Tri* t : c.original)
                for (int k = 0; k < 3; ++k) {
                    auto it = vm.find(t->v[k]);
                    if (it == vm.end()) {
                        it = vm.emplace(t->v[k], static_cast<uint32_t>(back.size())).first;
                        back.push_back(t->v[k]);
                        const glm::vec3& p = gm.verts[t->v[k]].position;
                        pos.insert(pos.end(), {p.x, p.y, p.z});
                    }
                    byMat[t->mat].push_back(it->second);
                }
            std::vector<Tri> lodTriStore;
            size_t lodCount = 0;
            for (auto& [mat, idx] : byMat) {
                std::vector<unsigned int> dst(idx.size());
                const size_t target = std::max<size_t>(3, static_cast<size_t>(idx.size() * opt_.lodRatio) / 3 * 3);
                const size_t n = meshopt_simplify(dst.data(), idx.data(), idx.size(), pos.data(), back.size(), 12, target,
                                                  opt_.lodError, meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute,
                                                  nullptr);
                for (size_t k = 0; k + 2 < n; k += 3) {
                    Tri t{{back[dst[k]], back[dst[k + 1]], back[dst[k + 2]]}, mat, 0, {}};
                    lodTriStore.push_back(t);
                }
                lodCount += n / 3;
            }
            if (lodCount > 0 && lodCount * 10 <= c.original.size() * 7) {
                std::vector<const Tri*> lp;
                for (const Tri& t : lodTriStore) lp.push_back(&t);
                uint32_t lt = 0;
                std::vector<uint8_t> lod = buildMesh(gm, lp, centre, step, cell, fmt::kChunkLod, false, nullptr, lt, clamps);
                if (!lod.empty()) {
                    rec.lodTriangles = static_cast<uint16_t>(std::min<uint32_t>(lt, 65535));
                    lodTris += lt;
                    lodBytes += lod.size();
                    chunkLodName[ci] = "l_" + std::to_string(ci);
                    items.push_back({chunkLodName[ci], kKindModel, std::move(lod)});
                }
            }
        }
    }
    // Shells: per cell, its original triangles seen from outside (every group) simplified, untextured.
    for (auto& [c, idx] : cellIndex) {
        // One mesh over the cell's groups: vertices merged by position (groups do not share indices).
        std::vector<float> pos;
        std::vector<std::pair<uint32_t, uint32_t>> src;  // (group, vertex) per merged vertex
        std::map<std::tuple<float, float, float>, uint32_t> byPos;
        std::vector<unsigned int> idxAll;
        std::vector<uint16_t> triMat;
        for (const ChunkOut& ch : chunks) {
            if (ch.cell != c) continue;
            const GroupMesh& gm = meshes[ch.group];
            for (const Tri* t : ch.original) {
                if (!t->outside) continue;
                for (int k = 0; k < 3; ++k) {
                    const glm::vec3& p = gm.verts[t->v[k]].position;
                    auto it = byPos.find({p.x, p.y, p.z});
                    if (it == byPos.end()) {
                        it = byPos.emplace(std::make_tuple(p.x, p.y, p.z), static_cast<uint32_t>(src.size())).first;
                        src.emplace_back(ch.group, t->v[k]);
                        pos.insert(pos.end(), {p.x, p.y, p.z});
                    }
                    idxAll.push_back(it->second);
                }
                triMat.push_back(t->mat);
            }
        }
        if (idxAll.size() < 3) continue;
        std::vector<unsigned int> dst(idxAll.size());
        const size_t target = std::max<size_t>(3, static_cast<size_t>(idxAll.size() * opt_.shellRatio) / 3 * 3);
        size_t n = meshopt_simplify(dst.data(), idxAll.data(), idxAll.size(), pos.data(), src.size(), 12, target,
                                    opt_.shellError, meshopt_SimplifyErrorAbsolute, nullptr);
        // WMO meshes are open and seamed: the edge collapse stops far above the target. Vertex clustering
        // (sloppy) goes the rest of the way within the same error (relative to the cell mesh's extent there).
        if (n > target + target / 2) {
            glm::vec3 a(1e30f), b(-1e30f);
            for (size_t k = 0; k < src.size(); ++k) {
                a = glm::min(a, glm::vec3(pos[k * 3], pos[k * 3 + 1], pos[k * 3 + 2]));
                b = glm::max(b, glm::vec3(pos[k * 3], pos[k * 3 + 1], pos[k * 3 + 2]));
            }
            const float extent = std::max(1.0f, std::max({b.x - a.x, b.y - a.y, b.z - a.z}));
            std::vector<unsigned int> d2(n);
            const size_t n2 = meshopt_simplifySloppy(d2.data(), dst.data(), n, pos.data(), src.size(), 12, target,
                                                     opt_.shellError / extent, nullptr);
            if (n2 >= 3) {
                std::copy(d2.begin(), d2.begin() + static_cast<std::ptrdiff_t>(n2), dst.begin());
                n = n2;
            }
        }
        if (n < 3) continue;
        // One GroupMesh of the merged vertices (colour = the first triangle's material that used the vertex).
        GroupMesh sm;
        sm.verts.resize(src.size());
        sm.lit.resize(src.size());
        std::vector<uint16_t> vmat(src.size(), 0xFFFF);
        for (size_t t = 0; t * 3 + 2 < idxAll.size(); ++t)
            for (int k = 0; k < 3; ++k)
                if (vmat[idxAll[t * 3 + k]] == 0xFFFF) vmat[idxAll[t * 3 + k]] = triMat[t];
        for (size_t v = 0; v < src.size(); ++v) {
            sm.verts[v] = meshes[src[v].first].verts[src[v].second];
            sm.lit[v] = meshes[src[v].first].lit[src[v].second];
        }
        std::vector<Tri> st;
        for (size_t k = 0; k + 2 < n; k += 3) st.push_back(Tri{{dst[k], dst[k + 1], dst[k + 2]}, vmat[dst[k]], 0, {}});
        std::vector<const Tri*> sp;
        for (const Tri& t : st) sp.push_back(&t);
        uint32_t stris = 0;
        std::vector<uint8_t> shell = buildMesh(sm, sp, centres[c], step, idx, fmt::kChunkShell, true, nullptr, stris, clamps);
        if (shell.empty()) continue;
        cellRecs[idx].shellTriangles = stris;
        shellTris += stris;
        shellBytes += shell.size();
        items.push_back({"s_" + std::to_string(idx), kKindModel, std::move(shell)});
    }
    for (auto& [c, list] : doodadsByCell) {
        const uint16_t idx = cellIndex[c];
        cellRecs[idx].doodadCount = static_cast<uint32_t>(list.size());
        std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(list.data()),
                                  reinterpret_cast<const uint8_t*>(list.data() + list.size()));
        items.push_back({"dd_" + std::to_string(idx), 3, std::move(blob)});
    }
    // A cell of doodads only (no chunk: a doodad over a gap of the grid, or one past the geometry
    // clamped into an edge cell without chunks) gets the box of its doodads' spheres (and a centre inside it) - it had
    // the point box of its centre, (0, 0, 0) = the WMO's origin, so the PSP (which reads "dd_" by the distance of the
    // cell box) never read it.
    for (auto& [c, list] : doodadsByCell) {
        fmt::BigWmoCell& cr = cellRecs[cellIndex[c]];
        if (cr.chunkCount) continue;
        for (const fmt::BigWmoDoodad& d : list)
            for (int a = 0; a < 3; ++a) {
                cr.bmin[a] = std::min(cr.bmin[a], d.sphere[a] - d.sphere[3]);
                cr.bmax[a] = std::max(cr.bmax[a], d.sphere[a] + d.sphere[3]);
            }
        for (int a = 0; a < 3; ++a) cr.center[a] = static_cast<float>(lat((cr.bmin[a] + cr.bmax[a]) * 0.5f, step) * step);
    }
    for (fmt::BigWmoCell& cr : cellRecs)
        if (cr.bmin[0] > cr.bmax[0])  // nothing in it (cannot happen: a cell is written for its chunks or doodads)
            for (int a = 0; a < 3; ++a) cr.bmin[a] = cr.bmax[a] = cr.center[a];

    // ---- "!wmo" ----
    std::vector<fmt::BigWmoGroup> groups(ng);
    std::vector<uint16_t> groupChunks(ng, 0);
    for (const ChunkOut& c : chunks) ++groupChunks[c.group];
    for (size_t gi = 0; gi < ng; ++gi) {
        const pipeline::WMOGroup& g = m.groups[gi];
        fmt::BigWmoGroup& o = groups[gi];
        o.flags = g.flags;
        for (int a = 0; a < 3; ++a) {
            o.bmin[a] = g.boundingBoxMin[a];
            o.bmax[a] = g.boundingBoxMax[a];
        }
        o.portalStart = g.portalStart;
        o.portalCount = static_cast<uint16_t>(std::min<uint32_t>(g.portalCount, refs.size() > g.portalStart ? refs.size() - g.portalStart : 0));
        o.chunkCount = groupChunks[gi];
        o.doodadRefs = static_cast<uint16_t>(std::min<size_t>(modr[gi].size(), 65535));
        o.triangles = static_cast<uint32_t>(meshes[gi].original.size());
    }
    // Portal vertices (MOPV, upstream parses them) and the measured MOPR side convention.
    std::vector<float> pverts;
    for (const glm::vec3& v : m.portalVertices) pverts.insert(pverts.end(), {v.x, v.y, v.z});
    size_t sideAgree = 0, sideDisagree = 0;
    for (size_t gi = 0; gi < ng; ++gi) {
        const GroupMesh& gm = meshes[gi];
        if (gm.original.empty()) continue;
        glm::vec3 cen(0.0f);
        for (size_t v = 0; v < gm.originalVerts; ++v) cen += gm.verts[v].position;
        cen /= static_cast<float>(std::max<size_t>(1, gm.originalVerts));
        for (uint32_t i = 0; i < groups[gi].portalCount; ++i) {
            const fmt::BigWmoPortalRef& r = refs[groups[gi].portalStart + i];
            if (r.portal >= portals.size()) continue;
            const fmt::BigWmoPortal& p = portals[r.portal];
            if (p.vertexCount < 3 || p.vertexStart + p.vertexCount > m.portalVertices.size()) continue;
            const glm::vec3 n(p.plane[0], p.plane[1], p.plane[2]);
            const float d = glm::dot(n, cen) - glm::dot(n, m.portalVertices[p.vertexStart]);
            if (std::fabs(d) < 0.5f) continue;
            ((d > 0) == (r.side > 0) ? sideAgree : sideDisagree)++;
        }
    }
    std::vector<fmt::BigWmoBlobRef> chunkLod(chunks.size()), chunkNear(chunks.size()), texRefs(textures_.size());

    fmt::BigWmoHeader h{};
    std::memcpy(h.magic, "WBWO", 4);
    h.version = fmt::kBigWmoVersion;
    h.flags = sideDisagree > sideAgree ? fmt::kBigWmoSideFlipped : 0;
    h.stamp = stamp;
    h.groupCount = static_cast<uint16_t>(ng);
    h.cellCount = static_cast<uint16_t>(cellRecs.size());
    h.chunkCount = static_cast<uint32_t>(chunks.size());
    h.portalCount = static_cast<uint16_t>(portals.size());
    h.portalRefCount = static_cast<uint16_t>(refs.size());
    h.portalVertexCount = static_cast<uint32_t>(m.portalVertices.size());
    h.textureCount = static_cast<uint16_t>(textures_.size());
    h.cellsX = static_cast<uint16_t>(cellsX);
    h.cellsY = static_cast<uint16_t>(cellsY);
    for (int a = 0; a < 3; ++a) {
        h.bmin[a] = lo[a];
        h.bmax[a] = hi[a];
        h.ambient[a] = m.ambientColor[a];
    }
    h.step = static_cast<float>(step);
    h.cellSize = cs;
    h.cellOrigin[0] = origin.x;
    h.cellOrigin[1] = origin.y;
    h.totalTriangles = static_cast<uint32_t>(totalTris);
    h.doodadTotal = static_cast<uint32_t>(doodadTotal);
    auto headerBlob = [&]() {
        std::vector<uint8_t> b(sizeof(h));
        auto table = [&](uint32_t& off, const void* p, size_t bytes) {
            alignTo(b, 4);
            off = static_cast<uint32_t>(b.size());
            b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + bytes);
        };
        table(h.groupOffset, groups.data(), groups.size() * sizeof(fmt::BigWmoGroup));
        table(h.portalOffset, portals.data(), portals.size() * sizeof(fmt::BigWmoPortal));
        table(h.portalRefOffset, refs.data(), refs.size() * sizeof(fmt::BigWmoPortalRef));
        table(h.portalVertexOffset, pverts.data(), pverts.size() * sizeof(float));
        table(h.cellOffset, cellRecs.data(), cellRecs.size() * sizeof(fmt::BigWmoCell));
        table(h.chunkOffset, chunkRecs.data(), chunkRecs.size() * sizeof(fmt::BigWmoChunk));
        table(h.chunkLodOffset, chunkLod.data(), chunkLod.size() * sizeof(fmt::BigWmoBlobRef));
        table(h.textureOffset, texRefs.data(), texRefs.size() * sizeof(fmt::BigWmoBlobRef));
        table(h.chunkNearOffset, chunkNear.data(), chunkNear.size() * sizeof(fmt::BigWmoBlobRef));
        if (!landmarks.empty()) {
            alignTo(b, 4);
            h.reserved = static_cast<uint32_t>(b.size());
            const uint32_t count=static_cast<uint32_t>(landmarks.size());
            const auto* cp=reinterpret_cast<const uint8_t*>(&count);
            b.insert(b.end(),cp,cp+sizeof(count));
            const auto* dp=reinterpret_cast<const uint8_t*>(landmarks.data());
            b.insert(b.end(),dp,dp+landmarks.size()*sizeof(fmt::BigWmoDoodad));
        }
        std::memcpy(b.data(), &h, sizeof(h));
        return b;
    };
    for (auto& t : textures_) items.push_back(std::move(t));
    items.push_back({"!wmo", 3, headerBlob()});

    // Layout exactly as SortedPakWriter::finish (stable sort by name, 64-byte aligned), so the header can carry
    // every record's offset; then the header is rebuilt with them (same size) and written.
    auto layout = [&]() {
        std::vector<size_t> order(items.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].name < items[b].name; });
        std::map<std::string, fmt::BigWmoBlobRef> at;
        uint64_t off = (16 + 64ull * (items.size() + 1) + 63) & ~uint64_t(63);
        for (size_t i : order) {
            at[items[i].name] = fmt::BigWmoBlobRef{static_cast<uint32_t>(off), static_cast<uint32_t>(items[i].blob.size())};
            off = (off + items[i].blob.size() + 63) & ~uint64_t(63);
        }
        return at;
    };
    const auto at = layout();
    auto ref = [&](const std::string& nm) {
        auto it = at.find(nm);
        return it == at.end() ? fmt::BigWmoBlobRef{0, 0} : it->second;
    };
    for (size_t ci = 0; ci < chunks.size(); ++ci) {
        chunkRecs[ci].mesh = ref("c_" + std::to_string(ci));
        chunkRecs[ci].collision = ref("q_" + std::to_string(ci));
        chunkLod[ci] = chunkLodName[ci].empty() ? fmt::BigWmoBlobRef{0, 0} : ref(chunkLodName[ci]);
        chunkNear[ci] = chunkNearName[ci].empty() ? fmt::BigWmoBlobRef{0, 0} : ref(chunkNearName[ci]);
    }
    for (size_t i = 0; i < cellRecs.size(); ++i) {
        cellRecs[i].shell = ref("s_" + std::to_string(i));
        cellRecs[i].doodads = ref("dd_" + std::to_string(i));
    }
    for (size_t i = 0; i < texRefs.size(); ++i) texRefs[i] = ref("x_" + std::to_string(i));
    const std::vector<uint8_t> finalHeader = headerBlob();
    for (PakItem& it : items)
        if (it.name == "!wmo") {
            if (it.blob.size() != finalHeader.size()) return false;
            it.blob = finalHeader;
        }
    size_t bytes = 0;
    for (const PakItem& it : items) bytes += it.blob.size();
    if (!writePakSorted(outDir + "/" + pakRel, items)) return false;

    out.ok = true;
    out.triangles = totalTris;
    out.chunks = chunks.size();
    out.bytes = bytes;
    std::printf("bigwmo %s -> %s: %zu groups, %zu portals (%zu refs, side %zu agree / %zu not), %zu cells of %.0f yd "
                "(%dx%d), step 1/%.0f yd, %zu chunks\n",
                key.c_str(), pakRel.c_str(), ng, portals.size(), refs.size(), sideAgree, sideDisagree, cellRecs.size(),
                cs, cellsX, cellsY, 1.0 / step, chunks.size());
    std::printf("  source tris %zu, chunks %zu (split to %.1f yd: %zu), near %.1f yd: %zu; in the near meshes %zu, lod "
                "%zu, shells %zu; bytes mesh %zu KB, "
                "near %zu KB, lod %zu KB, collision %zu KB, shells %zu KB, textures %zu (%zu KB); doodads %zu (%zu "
                "without a model), clamps %u\n",
                srcTris, totalTris, opt_.baseEdge, baseTris, opt_.maxEdge, splitTris, nearTris, lodTris, shellTris, meshBytes / 1024, nearBytes / 1024,
                lodBytes / 1024, colBytes / 1024, shellBytes / 1024, textures_.size(), tex.bytes() / 1024, doodadTotal,
                doodadNoModel, clamps);
    std::printf("  seen from outside: %zu of %zu source triangles, %zu of %zu chunks\n", seenOutside, srcTris,
                chunksSeenOutside, chunks.size());
    std::printf("  vertices: strips %zu, distinct %zu (%.2f strip vertices per distinct)\n", statStripVerts_,
                statUniqueVerts_, static_cast<double>(statStripVerts_) / static_cast<double>(std::max<size_t>(1, statUniqueVerts_)));
    tex_ = nullptr;
    return true;
}

}  // namespace wowee::pspconv
