// PC checks of a big-WMO pak (psp_model_conv --bigwmo-verify / --bigwmo-route,
// options below).
//  --bigwmo-verify <pak>   every record of "!wmo" against the TOC (offset, size), every chunk / LOD / shell mesh
//                          decoded (strip ranges, vertex counts, texture ids), collision meshes (collisionValid, as the
//                          PSP's install), doodad records, the box of a cell of doodads only;
//  --verify <doodads pak>  (verifyBigWmoIndex) its "wbig" record: size, placements, every
//                          bigwmo/*.pak it names next to the pak, each verified as above with the stamp and the doodads'
//                          models ("m_<id>" of the doodads pak);
//  --bigwmo-route <route> <outDir>
//                          the PSP's visibility and level-of-detail code (include/rendering/psp/psp_bigwmo_vis.hpp)
//                          on a camera route; per stop: camera group, visible groups, chunks / triangles / draws /
//                          binds / cells drawn, the cost model (measured on a PSP-3000: CPU 5.06 us per bind + 2.76 us per
//                          draw + 5.3 us per matrix, GE 0.49 us per triangle + 5.6 us per batch), and an offline
//                          z-buffer render (480 x 272, the PSP camera) of the full WMO against the culled set: pixels
//                          the culled set misses ("holes") are counted - the portals must not hide what is seen.
//                          Route file: "pak <bigwmo pak>", optional "wbig <doodads pak>" (then cameras are world /
//                          server coordinates of that placement), "cam x y z yawDeg pitchDeg", or "auto <n>" (the
//                          street groups' floors, four directions each, plus views from outside).
#pragma once

#include "rendering/psp/psp_bigwmo_format.hpp"
#include "rendering/psp/psp_bigwmo_vis.hpp"
#include "rendering/psp/psp_model_format.hpp"

#include "conv_common.hpp"
#include "forest_bake.hpp"  // writePng

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace wowee::pspconv {

namespace bw = wowee::rendering::psp;

struct PcPak {
    std::vector<uint8_t> bytes;
    std::map<std::string, std::pair<uint32_t, uint32_t>> toc;
    bool open(const std::string& path) {
        if (!readFile(path, bytes) || bytes.size() < 16 || std::memcmp(bytes.data(), "WPAK", 4) != 0) return false;
        uint32_t count, tocOff;
        std::memcpy(&count, &bytes[8], 4);
        std::memcpy(&tocOff, &bytes[12], 4);
        for (uint32_t i = 0; i < count; ++i) {
            const size_t o = tocOff + static_cast<size_t>(i) * 64;
            if (o + 64 > bytes.size()) return false;
            char nm[49] = {};
            std::memcpy(nm, &bytes[o], 48);
            uint32_t off, size;
            std::memcpy(&off, &bytes[o + 48], 4);
            std::memcpy(&size, &bytes[o + 52], 4);
            toc[nm] = {off, size};
        }
        return true;
    }
    const uint8_t* at(const bw::BigWmoBlobRef& r) const {
        return r.offset && r.offset + static_cast<uint64_t>(r.size) <= bytes.size() ? bytes.data() + r.offset : nullptr;
    }
};

// A decoded WBCK mesh: triangles in WMO-local yards, a colour each, its batches' textures.
struct PcMesh {
    std::vector<glm::vec3> pos;  // 3 per triangle
    std::vector<uint32_t> col;   // per triangle, RGB
    uint32_t batches = 0, triangles = 0;
    std::vector<uint16_t> texturePerBatch;
};

inline bool decodeMesh(const uint8_t* b, uint32_t size, const bw::BigWmoTables& t, PcMesh& out, std::string* err = nullptr) {
    out = PcMesh{};
    auto fail = [&](const char* why) {
        if (err) *err = why;
        return false;
    };
    if (!b || size < sizeof(bw::ChunkMeshHeader)) return fail("short");
    const auto* h = reinterpret_cast<const bw::ChunkMeshHeader*>(b);
    if (std::memcmp(h->magic, "WBCK", 4) != 0 || h->version != bw::kBigWmoVersion) return fail("magic");
    if (h->vertexOffset % 16 || h->vertexOffset + uint64_t{h->vertexCount} * 12u > size ||
        h->batchOffset + uint64_t{h->batchCount} * 12u > size || h->textureOffset + uint64_t{h->textureCount} * 2u > size)
        return fail("tables");
    const bool indexed = (h->flags & bw::kChunkIndexed) != 0;
    if (indexed && (h->indexOffset % 2 || h->indexOffset + uint64_t{h->indexCount} * 2u > size)) return fail("indices");
    const auto* ix = reinterpret_cast<const uint16_t*>(b + h->indexOffset);
    if (h->cell >= t.h->cellCount) return fail("cell");
    const bw::BigWmoCell& cell = t.cells[h->cell];
    const auto* v = reinterpret_cast<const bw::ModelVertex*>(b + h->vertexOffset);
    const auto* bt = reinterpret_cast<const bw::ModelBatch*>(b + h->batchOffset);
    const float step = t.h->step;
    for (uint32_t i = 0; i < h->batchCount; ++i) {
        const bw::ModelBatch& m = bt[i];
        if (m.indexStart + m.indexCount > (indexed ? h->indexCount : h->vertexCount)) return fail("strip range");
        if (m.texture != bw::kNoTexture && m.texture >= t.h->textureCount) return fail("texture id");
        out.texturePerBatch.push_back(m.texture);
        ++out.batches;
        for (uint32_t k = 2; k < m.indexCount; ++k) {
            auto at = [&](uint32_t s) -> const bw::ModelVertex* {
                const uint32_t vi = indexed ? ix[s] : s;
                return vi < h->vertexCount ? &v[vi] : nullptr;
            };
            const bw::ModelVertex* tv[3] = {at(m.indexStart + k - 2), at(m.indexStart + k - 1), at(m.indexStart + k)};
            if (!tv[0] || !tv[1] || !tv[2]) return fail("index range");
            glm::vec3 p[3];
            for (int j = 0; j < 3; ++j)
                p[j] = glm::vec3(cell.center[0] + tv[j]->x * step, cell.center[1] + tv[j]->y * step,
                                 cell.center[2] + tv[j]->z * step);
            if (glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) < 1e-6f) continue;  // degenerate join
            const uint16_t c = tv[0]->color;
            const uint32_t r = ((c & 31u) * 255u) / 31u, g = (((c >> 5) & 63u) * 255u) / 63u, bl = ((c >> 11) * 255u) / 31u;
            out.pos.insert(out.pos.end(), {p[0], p[1], p[2]});
            out.col.push_back(r | (g << 8) | (bl << 16));
            ++out.triangles;
        }
    }
    return true;
}

// expectStamp - the "wbig" stamp of the doodads pak that lists it (else not checked); models - the
// "m_<id>" ids of that pak (every doodad record must name one; else not checked).
inline bool bigWmoVerify(const std::string& path, const uint32_t* expectStamp = nullptr,
                         const std::set<uint32_t>* models = nullptr) {
    PcPak pak;
    if (!pak.open(path)) {
        std::fprintf(stderr, "bigwmo-verify: cannot read %s\n", path.c_str());
        return false;
    }
    auto hdr = pak.toc.find("!wmo");
    if (hdr == pak.toc.end()) {
        std::fprintf(stderr, "bigwmo-verify: no !wmo\n");
        return false;
    }
    bw::BigWmoTables t;
    if (!t.bind(pak.bytes.data() + hdr->second.first, hdr->second.second)) {
        std::fprintf(stderr, "bigwmo-verify: !wmo tables do not bind\n");
        return false;
    }
    // TOC record 0 must be "!wmo" (the PSP reads it without a search).
    {
        uint32_t tocOff;
        std::memcpy(&tocOff, &pak.bytes[12], 4);
        if (std::strncmp(reinterpret_cast<const char*>(&pak.bytes[tocOff]), "!wmo", 48) != 0) {
            std::fprintf(stderr, "bigwmo-verify: TOC record 0 is not !wmo\n");
            return false;
        }
    }
    size_t errors = 0;
    auto check = [&](const bw::BigWmoBlobRef& r, const std::string& name, bool optional) {
        auto it = pak.toc.find(name);
        if (it == pak.toc.end()) {
            if (!optional || r.offset) {
                std::fprintf(stderr, "  %s: not in the TOC (ref %u+%u)\n", name.c_str(), r.offset, r.size);
                ++errors;
            }
            return;
        }
        if (it->second.first != r.offset || it->second.second != r.size) {
            std::fprintf(stderr, "  %s: TOC %u+%u, header %u+%u\n", name.c_str(), it->second.first, it->second.second,
                         r.offset, r.size);
            ++errors;
        }
    };
    size_t tris = 0, lodTris = 0, shellTris = 0, draws = 0, meshBytes = 0, lodBytes = 0, colBytes = 0, nearTris = 0;
    for (uint32_t c = 0; c < t.h->chunkCount; ++c) {
        const bw::BigWmoChunk& k = t.chunks[c];
        check(k.mesh, "c_" + std::to_string(c), false);
        check(k.collision, "q_" + std::to_string(c), true);
        check(t.chunkLod[c], "l_" + std::to_string(c), true);
        check(t.chunkNear[c], "n_" + std::to_string(c), true);
        if (t.chunkNear[c].size) {
            PcMesh nm;
            std::string why2;
            if (!decodeMesh(pak.at(t.chunkNear[c]), t.chunkNear[c].size, t, nm, &why2)) {
                std::fprintf(stderr, "  n_%u: %s\n", c, why2.c_str());
                ++errors;
            }
            nearTris += nm.triangles;
        }
        PcMesh m;
        std::string why;
        if (!decodeMesh(pak.at(k.mesh), k.mesh.size, t, m, &why)) {
            std::fprintf(stderr, "  c_%u: %s\n", c, why.c_str());
            ++errors;
            continue;
        }
        tris += m.triangles;
        draws += m.batches;
        meshBytes += k.mesh.size;
        if (t.chunkLod[c].size) {
            PcMesh l;
            if (!decodeMesh(pak.at(t.chunkLod[c]), t.chunkLod[c].size, t, l, &why)) {
                std::fprintf(stderr, "  l_%u: %s\n", c, why.c_str());
                ++errors;
            }
            lodTris += l.triangles;
            lodBytes += t.chunkLod[c].size;
        }
        if (k.collision.size) {
            // The PSP's install check (cells, lists, triangle indices, a grid of >= 1 cell).
            if (!bw::collisionValid(pak.at(k.collision), k.collision.size)) {
                std::fprintf(stderr, "  q_%u: bad WCOL\n", c);
                ++errors;
            }
            colBytes += k.collision.size;
        }
        if (k.cell >= t.h->cellCount || c < t.cells[k.cell].chunkStart ||
            c >= t.cells[k.cell].chunkStart + t.cells[k.cell].chunkCount) {
            std::fprintf(stderr, "  c_%u: not in its cell's range\n", c);
            ++errors;
        }
    }
    size_t doodads = 0;
    for (uint32_t i = 0; i < t.h->cellCount; ++i) {
        const bw::BigWmoCell& cl = t.cells[i];
        check(cl.shell, "s_" + std::to_string(i), true);
        check(cl.doodads, "dd_" + std::to_string(i), true);
        if (cl.shell.size) {
            PcMesh m;
            std::string why;
            if (!decodeMesh(pak.at(cl.shell), cl.shell.size, t, m, &why)) {
                std::fprintf(stderr, "  s_%u: %s\n", i, why.c_str());
                ++errors;
            }
            shellTris += m.triangles;
        }
        if (cl.doodads.size % sizeof(bw::BigWmoDoodad) || cl.doodads.size / sizeof(bw::BigWmoDoodad) != cl.doodadCount) {
            std::fprintf(stderr, "  dd_%u: size %u for %u doodads\n", i, cl.doodads.size, cl.doodadCount);
            ++errors;
        } else if (const uint8_t* db = pak.at(cl.doodads)) {
            // The records - group, model in the doodads pak; a cell of doodads only must have a box
            // that holds them (the PSP reads "dd_" by the distance of the cell box; the bake before the fix gave such
            // a cell the point box of the WMO's origin).
            size_t badGroup = 0, noModel = 0, outside = 0;
            for (uint32_t d = 0; d < cl.doodadCount; ++d) {
                bw::BigWmoDoodad r;
                std::memcpy(&r, db + d * sizeof(r), sizeof(r));
                if (r.group != 0xFFFF && r.group >= t.h->groupCount) ++badGroup;
                if (models && !models->count(r.model)) ++noModel;
                bool in = true;
                for (int a = 0; a < 3; ++a)
                    in = in && r.sphere[a] >= cl.bmin[a] - 0.01f && r.sphere[a] <= cl.bmax[a] + 0.01f;
                if (!cl.chunkCount && !in) ++outside;
            }
            if (badGroup || noModel || outside) {
                std::fprintf(stderr, "  dd_%u: %zu with a bad group, %zu with no model in the doodads pak, %zu outside the "
                             "box of a cell without chunks\n", i, badGroup, noModel, outside);
                ++errors;
            }
        }
        doodads += cl.doodadCount;
    }
    if (expectStamp && t.h->stamp != *expectStamp) {
        std::fprintf(stderr, "  stamp %08x, the doodads pak's wbig %08x (another bake: drawn without doodads)\n",
                     t.h->stamp, *expectStamp);
        ++errors;
    }
    for (uint32_t i = 0; i < t.h->textureCount; ++i) check(t.textures[i], "x_" + std::to_string(i), false);
    std::printf("bigwmo-verify %s: %u groups, %u cells, %u chunks, %u portals, %u textures, %zu doodads; tris %zu "
                "(header %u), near %zu, lod %zu, shells %zu, batches %zu; mesh %zu KB, lod %zu KB, collision %zu KB, header %u KB; "
                "step 1/%.0f, side %s; %zu errors\n",
                path.c_str(), t.h->groupCount, t.h->cellCount, t.h->chunkCount, t.h->portalCount, t.h->textureCount,
                doodads, tris, t.h->totalTriangles, nearTris, lodTris, shellTris, draws, meshBytes / 1024, lodBytes / 1024,
                colBytes / 1024, hdr->second.second / 1024, 1.0 / t.h->step,
                (t.h->flags & bw::kBigWmoSideFlipped) ? "flipped" : "as written", errors);
    // The decoder skips zero-area triangles (degenerate source slivers, split into many pieces: harbour docks 22
    // sources -> 169 pieces of 41 855); more than 1 % missing is an error.
    return errors == 0 && tris <= t.h->totalTriangles && tris * 100 >= static_cast<size_t>(t.h->totalTriangles) * 99;
}

// --verify <doodads pak>: its "wbig" record (none: nothing to check) - the size for its count, the
// stamp, every placement (pak name, matrix, box, one per uniqueId), and every bigwmo/*.pak it names: present next to
// the doodads pak (data/ on the card), verified (bigWmoVerify) with this stamp and this pak's models. A bigwmo/
// pak of this map that no placement names is reported (a leftover of an older bake; not an error). Returns errors.
inline int verifyBigWmoIndex(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;  // verifyPak said it already
    char hdr[16];
    if (!f.read(hdr, 16) || std::memcmp(hdr, "WPAK", 4) != 0) return 0;
    uint32_t count, tocOff;
    std::memcpy(&count, hdr + 8, 4);
    std::memcpy(&tocOff, hdr + 12, 4);
    std::vector<char> toc(static_cast<size_t>(count) * 64);
    f.seekg(tocOff);
    if (!f.read(toc.data(), static_cast<std::streamsize>(toc.size()))) return 0;
    std::set<uint32_t> models;
    uint32_t wOff = 0, wSize = 0;
    bool have = false;
    for (uint32_t i = 0; i < count; ++i) {
        char nm[49] = {};
        std::memcpy(nm, &toc[static_cast<size_t>(i) * 64], 48);
        if (nm[0] == 'm' && nm[1] == '_' && nm[2] >= '0' && nm[2] <= '9') models.insert(static_cast<uint32_t>(std::strtoul(nm + 2, nullptr, 10)));
        if (std::strcmp(nm, "wbig") == 0) {
            std::memcpy(&wOff, &toc[static_cast<size_t>(i) * 64 + 48], 4);
            std::memcpy(&wSize, &toc[static_cast<size_t>(i) * 64 + 52], 4);
            have = true;
        }
    }
    if (!have) return 0;
    int errors = 0;
    auto bad = [&](const char* fmtStr, const std::string& a) {
        if (++errors <= 40) std::fprintf(stderr, fmtStr, a.c_str());
    };
    std::vector<uint8_t> rec(wSize);
    f.seekg(wOff);
    bw::BigWmoIndexHeader bh{};
    if (wSize < sizeof(bh) || !f.read(reinterpret_cast<char*>(rec.data()), static_cast<std::streamsize>(rec.size()))) {
        std::fprintf(stderr, "verify: wbig: short (%u bytes)\n", wSize);
        return 1;
    }
    std::memcpy(&bh, rec.data(), sizeof(bh));
    if (std::memcmp(bh.magic, "WBGI", 4) != 0 || bh.version != bw::kBigWmoVersion ||
        sizeof(bh) + static_cast<size_t>(bh.count) * sizeof(bw::BigWmoPlacement) != wSize) {
        std::fprintf(stderr, "verify: wbig: header (magic, version %u, %u placements in %u bytes)\n", bh.version, bh.count, wSize);
        return 1;
    }
    if (bh.stamp == 0) bad("verify: wbig: stamp 0 (the PSP draws the big WMOs without doodads)%s\n", "");
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    std::set<std::string> paks;
    std::set<uint32_t> uids;
    for (uint32_t i = 0; i < bh.count; ++i) {
        bw::BigWmoPlacement pl;
        std::memcpy(&pl, rec.data() + sizeof(bh) + static_cast<size_t>(i) * sizeof(pl), sizeof(pl));
        const size_t len = strnlen(pl.pak, sizeof(pl.pak));
        const std::string name(pl.pak, len);
        if (len == sizeof(pl.pak) || name.rfind("bigwmo/", 0) != 0 || name.size() < 12 ||
            name.compare(name.size() - 4, 4, ".pak") != 0) {
            bad("verify: wbig: placement pak name '%s'\n", name);
            continue;
        }
        if (!uids.insert(pl.uniqueId).second) bad("verify: wbig: uniqueId twice (%s)\n", name);
        bool finite = true;
        for (float v : pl.matrix) finite = finite && std::isfinite(v);
        for (int a = 0; a < 3; ++a) finite = finite && std::isfinite(pl.bmin[a]) && std::isfinite(pl.bmax[a]) && pl.bmin[a] <= pl.bmax[a];
        if (!finite) bad("verify: wbig: matrix / box of %s\n", name);
        paks.insert(name);
    }
    size_t okPaks = 0;
    for (const std::string& name : paks) {
        const std::filesystem::path file = dir / name;
        if (!std::filesystem::exists(file)) {
            bad("verify: wbig: %s missing next to the doodads pak (data/ on the card)\n", name);
            continue;
        }
        if (!bigWmoVerify(file.string(), &bh.stamp, &models)) bad("verify: wbig: %s failed (above)\n", name);
        else ++okPaks;
    }
    // Leftovers: bigwmo/<map>_*.pak of this map that no placement names.
    size_t leftovers = 0;
    std::error_code ec;
    std::string prefix;
    if (!paks.empty()) {
        const std::string first = *paks.begin();
        prefix = first.substr(7, first.find('_', 7) == std::string::npos ? 0 : first.find('_', 7) - 7 + 1);
    }
    if (!prefix.empty())
        for (const auto& de : std::filesystem::directory_iterator(dir / "bigwmo", ec)) {
            const std::string fn = de.path().filename().string();
            if (fn.rfind(prefix, 0) == 0 && de.path().extension() == ".pak" && !paks.count("bigwmo/" + fn)) {
                std::printf("verify: wbig: bigwmo/%s is not named by this pak (an older bake's? not copied to the card)\n",
                            fn.c_str());
                ++leftovers;
            }
        }
    std::printf("verify: wbig %u placements, stamp %08x, %zu big-WMO paks (%zu ok), %zu not named; %d errors\n", bh.count,
                bh.stamp, paks.size(), okPaks, leftovers, errors);
    return errors;
}

// ---- route ----------------------------------------------------------------------------------------------------
struct RouteRaster {
    static constexpr int W = 480, H = 272;
    float farZ = 450.0f;
    std::vector<float> depth;
    std::vector<int32_t> id;
    std::vector<uint32_t> rgb;
    void clear() {
        depth.assign(W * H, 1e30f);
        id.assign(W * H, -1);
        rgb.assign(W * H, 0x403020u);
    }
    // One triangle (local points) through toClip, near-clipped, z-buffered.
    void tri(const glm::mat4& toClip, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, int32_t tid,
             uint32_t colour) {
        {
            const glm::vec3 n = glm::cross(b - a, c - a);
            const float nl = glm::length(n);
            const float sh = nl > 1e-9f ? 0.35f + 0.65f * std::fabs(glm::dot(n / nl, glm::normalize(glm::vec3(0.4f, 0.3f, 0.85f)))) : 1.0f;
            auto ch = [&](int s2) { return static_cast<uint32_t>(((colour >> s2) & 255u) * sh) << s2; };
            colour = ch(0) | ch(8) | ch(16);
        }
        glm::vec4 in[3] = {toClip * glm::vec4(a, 1.0f), toClip * glm::vec4(b, 1.0f), toClip * glm::vec4(c, 1.0f)};
        glm::vec4 poly[6];
        int n = 0;
        constexpr float kNear = 1.0f;  // w = view depth with glm::perspective; the PSP near plane
        for (int i = 0; i < 3; ++i) {
            const glm::vec4& p = in[i];
            const glm::vec4& q = in[(i + 1) % 3];
            const bool pin = p.w >= kNear, qin = q.w >= kNear;
            if (pin) poly[n++] = p;
            if (pin != qin) poly[n++] = p + (q - p) * ((kNear - p.w) / (q.w - p.w));
        }
        if (n < 3) return;
        glm::vec3 s[6];
        for (int i = 0; i < n; ++i)
            s[i] = glm::vec3((poly[i].x / poly[i].w * 0.5f + 0.5f) * W, (0.5f - poly[i].y / poly[i].w * 0.5f) * H, poly[i].w);
        for (int i = 1; i + 1 < n; ++i) fill(s[0], s[i], s[i + 1], tid, colour);
    }
    void fill(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, int32_t tid, uint32_t colour) {
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-9f) return;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        const float ia = 1.0f / a.z, ib = 1.0f / b.z, ic = 1.0f / c.z;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float invz = w0 * ia + w1 * ib + w2 * ic;
                const float z = 1.0f / invz;
                if (z > farZ) continue;  // the far plane (the PSP draws nothing past it either)
                const size_t o = static_cast<size_t>(y) * W + x;
                if (z < depth[o]) {
                    depth[o] = z;
                    id[o] = tid;
                    rgb[o] = colour;
                }
            }
    }
};

struct RouteStop {
    glm::vec3 eye;    // world (or local without wbig)
    float yaw, pitch; // degrees: yaw 0 = +X (north), counter-clockwise; pitch up positive
    glm::vec3 feet{0.0f};  // the player (world); "cam" stops: below the eye
    bool hasFeet = false;
};

inline bool bigWmoRoute(const std::string& routePath, const std::string& outDir, const std::string& /*root*/) {
    std::ifstream rf(routePath);
    if (!rf) {
        std::fprintf(stderr, "bigwmo-route: cannot read %s\n", routePath.c_str());
        return false;
    }
    std::string pakPath, wbigPath;
    std::vector<RouteStop> stops;
    int autoN = 0, onlyGroup = -1, visRule = 0;
    bool boxFallback = false, autoAll = false;
    bw::BigWmoLod lod;
    for (std::string line; std::getline(rf, line);) {
        std::istringstream is(line);
        std::string k;
        if (!(is >> k) || k[0] == '#') continue;
        if (k == "pak") is >> pakPath;
        else if (k == "wbig") is >> wbigPath;
        else if (k == "auto") is >> autoN;
        else if (k == "autoall") {  // the same over every floor (not only the open-sky streets)
            is >> autoN;
            autoAll = true;
        }
        else if (k == "lod") is >> lod.lodDistance >> lod.shellDistance >> lod.viewDistance;
        else if (k == "only") is >> onlyGroup;  // the reference picture: this group alone (what is it?)
        else if (k == "rule") is >> visRule;    // the camera group / walk variant
        else if (k == "boxfallback") is >> boxFallback;  // no floor under the camera: the smallest box (1) or outside (0)
        else if (k == "cam") {
            RouteStop s{};
            is >> s.eye.x >> s.eye.y >> s.eye.z >> s.yaw >> s.pitch;
            stops.push_back(s);
        }
    }
    PcPak pak;
    if (!pak.open(pakPath)) {
        std::fprintf(stderr, "bigwmo-route: cannot read pak %s\n", pakPath.c_str());
        return false;
    }
    auto hdr = pak.toc.find("!wmo");
    bw::BigWmoTables t;
    if (hdr == pak.toc.end() || !t.bind(pak.bytes.data() + hdr->second.first, hdr->second.second)) {
        std::fprintf(stderr, "bigwmo-route: bad !wmo\n");
        return false;
    }
    // Placement: world = M * local (identity without wbig).
    glm::mat4 M(1.0f);
    if (!wbigPath.empty()) {
        PcPak dp;
        if (dp.open(wbigPath)) {
            auto it = dp.toc.find("wbig");
            if (it != dp.toc.end() && it->second.second >= sizeof(bw::BigWmoIndexHeader) &&
                it->second.first + uint64_t{it->second.second} <= dp.bytes.size()) {  // in the file
                const uint8_t* b = dp.bytes.data() + it->second.first;
                const auto* bh = reinterpret_cast<const bw::BigWmoIndexHeader*>(b);
                const auto* pl = reinterpret_cast<const bw::BigWmoPlacement*>(b + sizeof(*bh));
                // The placement whose pak has the route pak's file name (was substr(find("bigwmo")):
                // std::out_of_range for a path without "bigwmo", and substr(7) of a short name); the record's bounds.
                auto baseName = [](const std::string& s) {
                    const size_t at = s.find_last_of("/\\");
                    return at == std::string::npos ? s : s.substr(at + 1);
                };
                const std::string want = baseName(pakPath);
                const uint32_t n = std::memcmp(bh->magic, "WBGI", 4) == 0
                                       ? std::min<uint32_t>(bh->count, static_cast<uint32_t>((it->second.second - sizeof(*bh)) /
                                                                                            sizeof(bw::BigWmoPlacement)))
                                       : 0u;
                for (uint32_t i = 0; i < n; ++i) {
                    if (baseName(std::string(pl[i].pak, strnlen(pl[i].pak, sizeof(pl[i].pak)))) != want) continue;
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 3; ++r) M[c][r] = pl[i].matrix[c * 3 + r];
                    std::printf("route: placement uid %u of %.*s, stamp wbig %08x pak %08x\n", pl[i].uniqueId,
                                static_cast<int>(strnlen(pl[i].pak, sizeof(pl[i].pak))), pl[i].pak, bh->stamp, t.h->stamp);
                    break;
                }
            }
        }
    }
    const glm::mat4 Minv = glm::inverse(M);
    // Decode every chunk / LOD / shell once.
    std::vector<PcMesh> full(t.h->chunkCount), far(t.h->chunkCount), nearM(t.h->chunkCount), shell(t.h->cellCount);
    for (uint32_t c = 0; c < t.h->chunkCount; ++c) {
        decodeMesh(pak.at(t.chunks[c].mesh), t.chunks[c].mesh.size, t, full[c]);
        if (t.chunkNear[c].size) decodeMesh(pak.at(t.chunkNear[c]), t.chunkNear[c].size, t, nearM[c]);
        if (t.chunkLod[c].size) decodeMesh(pak.at(t.chunkLod[c]), t.chunkLod[c].size, t, far[c]);
    }
    for (uint32_t i = 0; i < t.h->cellCount; ++i)
        if (t.cells[i].shell.size) decodeMesh(pak.at(t.cells[i].shell), t.cells[i].shell.size, t, shell[i]);
    // Group -> chunks with collision (camera group test).
    std::vector<std::vector<uint32_t>> groupChunks(t.h->groupCount);
    for (uint32_t c = 0; c < t.h->chunkCount; ++c) groupChunks[t.chunks[c].group].push_back(c);
    auto floorBelow = [&](uint32_t g, const glm::vec3& p, float& z) {
        bool found = false;
        float best = -1e30f;
        for (uint32_t c : groupChunks[g]) {
            const bw::BigWmoChunk& k = t.chunks[c];
            if (!k.collision.size) continue;
            float cz;
            if (bw::collisionFloorBelow(reinterpret_cast<const bw::CollisionHeader*>(pak.at(k.collision)), k.collision.size, p,
                                        cz) &&
                cz > best) {
                best = cz;
                found = true;
            }
        }
        if (found) z = best;
        return found;
    };
    auto haveGeometry = [](uint32_t) { return true; };
    // Auto route: the big exterior-lit indoor groups (streets) - camera 3 yd over their floor at the box centre,
    // 4 directions; and 4 views from outside at 300 yd.
    if (autoN > 0) {
        // Street stops: a grid over the WMO; at each point every surface straight below the top (all groups'
        // collision); the topmost must be walkable (normal z >= cos 50) and belong to an exterior-lit group (0x40:
        // streets and squares) - open sky above, where a player walks. The player stands there, the camera 5 yd
        // behind and 2 yd over the head (the PSP's third person), four directions.
        struct Hit {
            float z;
            uint32_t group;
            float nz;
        };
        auto hitsAt = [&](float x, float y, std::vector<Hit>& out) {
            out.clear();
            for (uint32_t c = 0; c < t.h->chunkCount; ++c) {
                const bw::BigWmoChunk& k = t.chunks[c];
                if (!k.collision.size) continue;
                const auto* ch = reinterpret_cast<const bw::CollisionHeader*>(pak.at(k.collision));
                if (!ch || x < ch->bmin[0] || y < ch->bmin[1] || x > ch->bmax[0] || y > ch->bmax[1]) continue;
                const auto* cb = reinterpret_cast<const uint8_t*>(ch);
                const float* v = reinterpret_cast<const float*>(cb + ch->vertexOffset);
                const auto* tri = reinterpret_cast<const bw::CollisionTri*>(cb + ch->triangleOffset);
                const uint32_t* cells = reinterpret_cast<const uint32_t*>(cb + ch->cellOffset);
                const uint32_t* list = reinterpret_cast<const uint32_t*>(cb + ch->listOffset);
                const int cx = std::min<int>(ch->nx - 1, std::max(0, static_cast<int>((x - ch->bmin[0]) / ch->cell)));
                const int cy = std::min<int>(ch->ny - 1, std::max(0, static_cast<int>((y - ch->bmin[1]) / ch->cell)));
                const uint32_t* cl = cells + (static_cast<uint32_t>(cy) * ch->nx + static_cast<uint32_t>(cx)) * 2u;
                for (uint32_t i = 0; i < cl[1]; ++i) {
                    const bw::CollisionTri& tr = tri[list[cl[0] + i]];
                    const glm::vec3 A(v[tr.a * 3], v[tr.a * 3 + 1], v[tr.a * 3 + 2]);
                    const glm::vec3 B(v[tr.b * 3], v[tr.b * 3 + 1], v[tr.b * 3 + 2]);
                    const glm::vec3 C(v[tr.c * 3], v[tr.c * 3 + 1], v[tr.c * 3 + 2]);
                    const float d = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y);
                    if (std::fabs(d) < 1e-9f) continue;
                    const float l0 = ((B.y - C.y) * (x - C.x) + (C.x - B.x) * (y - C.y)) / d;
                    const float l1 = ((C.y - A.y) * (x - C.x) + (A.x - C.x) * (y - C.y)) / d;
                    const float l2 = 1.0f - l0 - l1;
                    if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                    const glm::vec3 n = glm::normalize(glm::cross(B - A, C - A));
                    out.push_back(Hit{l0 * A.z + l1 * B.z + l2 * C.z, k.group, std::fabs(n.z)});
                }
            }
            std::sort(out.begin(), out.end(), [](const Hit& p, const Hit& q) { return p.z > q.z; });
        };
        // Ray against every chunk's collision (the camera collision of the PSP, which has the resident ones).
        auto rayAll = [&](const glm::vec3& o, const glm::vec3& d, float maxD) {
            float best = maxD;
            const glm::vec3 e = o + d * maxD;
            const glm::vec3 lo = glm::min(o, e), hi = glm::max(o, e);
            for (uint32_t c = 0; c < t.h->chunkCount; ++c) {
                const bw::BigWmoChunk& k = t.chunks[c];
                if (!k.collision.size) continue;
                const auto* ch = reinterpret_cast<const bw::CollisionHeader*>(pak.at(k.collision));
                if (!ch || hi.x < ch->bmin[0] || hi.y < ch->bmin[1] || hi.z < ch->bmin[2] || lo.x > ch->bmax[0] ||
                    lo.y > ch->bmax[1] || lo.z > ch->bmax[2])
                    continue;
                const auto* cb = reinterpret_cast<const uint8_t*>(ch);
                const float* v = reinterpret_cast<const float*>(cb + ch->vertexOffset);
                const auto* tri = reinterpret_cast<const bw::CollisionTri*>(cb + ch->triangleOffset);
                for (uint32_t i = 0; i < ch->triangleCount; ++i) {
                    const glm::vec3 A(v[tri[i].a * 3], v[tri[i].a * 3 + 1], v[tri[i].a * 3 + 2]);
                    const glm::vec3 B(v[tri[i].b * 3], v[tri[i].b * 3 + 1], v[tri[i].b * 3 + 2]);
                    const glm::vec3 C(v[tri[i].c * 3], v[tri[i].c * 3 + 1], v[tri[i].c * 3 + 2]);
                    const glm::vec3 e1 = B - A, e2 = C - A, pv = glm::cross(d, e2);
                    const float det = glm::dot(e1, pv);
                    if (std::fabs(det) < 1e-9f) continue;
                    const float inv = 1.0f / det;
                    const glm::vec3 tv = o - A;
                    const float u = glm::dot(tv, pv) * inv;
                    if (u < 0.0f || u > 1.0f) continue;
                    const glm::vec3 qv = glm::cross(tv, e1);
                    const float w = glm::dot(d, qv) * inv;
                    if (w < 0.0f || u + w > 1.0f) continue;
                    const float tt = glm::dot(e2, qv) * inv;
                    if (tt > 0.0f && tt < best) best = tt;
                }
            }
            return best;
        };
        std::vector<glm::vec3> streetsAt;
        std::vector<Hit> hits;
        const float spacing = 20.0f;
        for (float y = t.h->bmin[1] + spacing * 0.5f; y < t.h->bmax[1]; y += spacing)
            for (float x = t.h->bmin[0] + spacing * 0.5f; x < t.h->bmax[0]; x += spacing) {
                hitsAt(x, y, hits);
                if (autoAll) {  // every walkable floor with 4 yd of room above it (indoor cities)
                    for (size_t h = 0; h < hits.size(); ++h)
                        if (hits[h].nz >= 0.643f && (h == 0 || hits[h - 1].z - hits[h].z >= 4.0f))
                            streetsAt.emplace_back(x, y, hits[h].z);
                    continue;
                }
                if (hits.empty() || hits[0].nz < 0.643f) continue;
                if (!(t.groups[hits[0].group].flags & bw::kWmoGroupExteriorLit)) continue;
                streetsAt.emplace_back(x, y, hits[0].z);
            }
        // Spread autoN of them over the list.
        const size_t want = static_cast<size_t>(autoN);
        for (size_t i = 0; i < want && !streetsAt.empty(); ++i) {
            const glm::vec3 feet = streetsAt[(i * streetsAt.size()) / want];
            for (int d = 0; d < 4; ++d) {
                const float yaw = glm::radians(90.0f * d + 30.0f);
                const glm::vec3 fwd(std::cos(yaw), std::sin(yaw), 0.0f);
                // The PSP camera (WorldScreen::cameraDistance): from the head back along the view, stopped 1.6 yd
                // before the first WMO triangle (the camera ray against the buildings).
                const glm::vec3 head = feet + glm::vec3(0.0f, 0.0f, 2.2f);
                glm::vec3 back = -fwd * 5.0f + glm::vec3(0.0f, 0.0f, 1.8f);
                const float want = glm::length(back);
                back /= want;
                const float hit = rayAll(head, back, want);
                const float dist = hit >= want ? want : std::max(0.5f, hit - 1.6f);
                const glm::vec3 local = head + back * dist;
                const glm::vec3 world = glm::vec3(M * glm::vec4(local, 1.0f));
                stops.push_back(RouteStop{world, 90.0f * d + 30.0f, -12.0f, glm::vec3(M * glm::vec4(feet, 1.0f)), true});
            }
        }
        std::printf("route: %zu street points (grid %.0f yd), %zu stops\n", streetsAt.size(), spacing, stops.size());
        const glm::vec3 c = (glm::vec3(t.h->bmin[0], t.h->bmin[1], t.h->bmin[2]) + glm::vec3(t.h->bmax[0], t.h->bmax[1], t.h->bmax[2])) * 0.5f;
        const float r = 0.5f * glm::length(glm::vec2(t.h->bmax[0] - t.h->bmin[0], t.h->bmax[1] - t.h->bmin[1])) + 60.0f;
        for (int d = 0; d < 4; ++d) {
            const float a = glm::radians(45.0f + 90.0f * d);
            const glm::vec3 local(c.x + std::cos(a) * r, c.y + std::sin(a) * r, t.h->bmax[2] * 0.5f + 20.0f);
            const glm::vec3 world = glm::vec3(M * glm::vec4(local, 1.0f));
            const glm::vec3 to = glm::vec3(M * glm::vec4(c, 1.0f)) - world;
            stops.push_back(RouteStop{world, glm::degrees(std::atan2(to.y, to.x)), -5.0f});
        }
    }
    std::filesystem::create_directories(outDir);
    // The world camera's projection (ge::kWorldFovYDeg, the PSP's 480 x 272).
    const float aspect = 480.0f / 272.0f;
    const glm::mat4 proj = glm::perspective(glm::radians(bw::ge::kWorldFovYDeg), aspect, 1.0f, lod.viewDistance);
    bw::PortalVisibility vis;
    RouteRaster ref, cul;
    ref.farZ = cul.farZ = lod.viewDistance;
    struct Sum {
        double tris = 0, draws = 0, binds = 0, mats = 0, ge = 0, cpu = 0, holes = 0, near = 0, lod = 0, shell = 0;
        double drawnKb = 0, texKb = 0, colKb = 0, maxResKb = 0;
        double maxTris = 0, maxGe = 0, maxCpu = 0, maxHoles = 0;
        size_t n = 0;
    } sum;
    std::ofstream csv(outDir + "/route.csv");
    csv << "stop,x,y,z,yaw,pitch,camGroup,outside,visibleGroups,walked,chunks,nearChunks,lodChunks,shellCells,tris,draws,binds,"
           "matrices,ge_ms,cpu_ms,hole_px,ref_px,drawn_kb,texture_kb,collision_kb\n";
    const int side = (t.h->flags & bw::kBigWmoSideFlipped) ? -1 : 1;
    for (size_t si = 0; si < stops.size(); ++si) {
        const RouteStop& s = stops[si];
        const float yaw = glm::radians(s.yaw), pit = glm::radians(s.pitch);
        const glm::vec3 fwd(std::cos(yaw) * std::cos(pit), std::sin(yaw) * std::cos(pit), std::sin(pit));
        const glm::mat4 view = glm::lookAt(s.eye, s.eye + fwd, glm::vec3(0, 0, 1));
        const glm::mat4 toClip = proj * view * M;
        const glm::vec3 local = glm::vec3(Minv * glm::vec4(s.eye, 1.0f));
        const int rule = visRule ? visRule : bw::PortalVisibility::kVisWalkExt;  // the PSP's default (wmovis 2)
        const bool upstreamRule = rule == bw::PortalVisibility::kVisUpstream;
        auto surfaceBelow = [&](uint32_t g, const glm::vec3& p, float& z) {
            bool found = false;
            float best = -1e30f;
            for (uint32_t c : groupChunks[g]) {
                const bw::BigWmoChunk& k = t.chunks[c];
                if (!k.collision.size) continue;
                float cz;
                if (bw::collisionFloorBelow(reinterpret_cast<const bw::CollisionHeader*>(pak.at(k.collision)), k.collision.size,
                                            p, cz, 0.0f) &&
                    cz > best) {
                    best = cz;
                    found = true;
                }
            }
            if (found) z = best;
            return found;
        };
        const int camGroup = rule == bw::PortalVisibility::kVisWalkOutdoor
                                 ? bw::findBigWmoGroupBySurface(t, local, surfaceBelow, haveGeometry)
                                 : bw::findBigWmoGroup(t, local, floorBelow, haveGeometry, boxFallback, upstreamRule);
        // The player's group seeds a walk too (upstream: the viewer position); he stands on a floor.
        const glm::vec3 feetLocal = s.hasFeet ? glm::vec3(Minv * glm::vec4(s.feet, 1.0f)) + glm::vec3(0.0f, 0.0f, 0.5f) : local;
        const int playerGroup = rule == bw::PortalVisibility::kVisWalkOutdoor
                                    ? camGroup  // walk-outdoor: the camera's group only
                                    : bw::findBigWmoGroup(t, feetLocal, floorBelow, haveGeometry, boxFallback, upstreamRule);
        if (camGroup < 0 && std::getenv("BIGWMO_ROUTE_DEBUG")) {  // which boxes hold it, which have a floor under it
            for (uint32_t g = 0; g < t.h->groupCount; ++g) {
                const bw::BigWmoGroup& gr = t.groups[g];
                const float m = 0.25f;
                if (local.x < gr.bmin[0] - m || local.y < gr.bmin[1] - m || local.z < gr.bmin[2] - m ||
                    local.x > gr.bmax[0] + m || local.y > gr.bmax[1] + m || local.z > gr.bmax[2] + m)
                    continue;
                float z = 0.0f;
                const bool f = floorBelow(g, local, z);
                std::printf("  debug stop %zu: in box of group %u flags %08x, floor %d z %.1f (cam z %.1f)\n", si, g,
                            gr.flags, f ? 1 : 0, z, local.z);
            }
        }
        vis.compute(t, local, toClip, camGroup, playerGroup, side, rule);
        // Reference: every chunk, full detail, every group (antiportal excluded).
        ref.clear();
        cul.clear();
        for (uint32_t c = 0; c < t.h->chunkCount; ++c) {
            if (t.groups[t.chunks[c].group].flags & bw::kWmoGroupAntiportal) continue;
            if (onlyGroup >= 0 && t.chunks[c].group != static_cast<uint32_t>(onlyGroup)) continue;
            const PcMesh& m = full[c];
            for (uint32_t i = 0; i < m.triangles; ++i)
                ref.tri(toClip, m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2], static_cast<int32_t>(c), m.col[i]);
        }
        // The PSP's choice.
        uint32_t chunks = 0, lodChunks = 0, shellCells = 0, nearChunks = 0, unsafeNoNear = 0;
        const glm::mat4 viewM = view * M;  // local -> camera (x right, y up, -z forward)
        double tris = 0, draws = 0, trisNear = 0, trisLod = 0, trisShell = 0;
        double drawnBytes = 0;  // the meshes drawn (they must be resident), their textures below, collision near
        std::set<uint16_t> binds;
        std::set<uint32_t> mats;
        for (uint32_t ci = 0; ci < t.h->cellCount; ++ci) {
            const bw::BigWmoCell& cl = t.cells[ci];
            const float cd = bw::boxDistance(local, glm::vec3(cl.bmin[0], cl.bmin[1], cl.bmin[2]),
                                             glm::vec3(cl.bmax[0], cl.bmax[1], cl.bmax[2]));
            if (cd > lod.viewDistance) continue;
            // The shell stands for the cell's exterior groups past shellDistance (inside or outside); outside, the
            // interiors of such a cell are not drawn either (bw::BigWmoLod).
            const bool useShell = cd > lod.shellDistance && cl.shell.size;
            if (useShell) {
                const PcMesh& m = shell[ci];
                drawnBytes += cl.shell.size;
                ++shellCells;
                tris += m.triangles;
                trisShell += m.triangles;
                draws += m.batches;
                mats.insert(ci);
                for (uint32_t i = 0; i < m.triangles; ++i)
                    cul.tri(toClip, m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2], -2, m.col[i]);
                if (vis.outside) continue;
            }
            for (uint32_t c = cl.chunkStart; c < cl.chunkStart + cl.chunkCount; ++c) {
                const bw::BigWmoChunk& k = t.chunks[c];
                if (!vis.chunkVisible(k)) continue;
                if (useShell && (k.flags & bw::kChunkSeenOutside)) continue;  // in the shell
                glm::vec3 lo, hi;
                t.chunkBox(c, lo, hi);
                const float d = bw::boxDistance(local, lo, hi);
                if (d > lod.viewDistance) continue;
                const bw::NdcRect r = bw::projectBox(toClip, lo, hi).intersect(vis.rects[k.group]);
                if (r.empty()) continue;
                // Guard band: the unsplit mesh only when its sphere is safe.
                const glm::vec3 cc = (lo + hi) * 0.5f;
                const float rr = glm::length(hi - lo) * 0.5f;
                const glm::vec4 cv = viewM * glm::vec4(cc, 1.0f);
                const bool safe =
                    bw::wmoSphereSafe(cv.x, cv.y, -cv.z, rr, 1.0f, bw::wmoFocalPx(bw::ge::kWorldFovYDeg));
                const bool useNear = !safe && t.chunkNear[c].size;
                if (!safe && !t.chunkNear[c].size && t.chunks[c].triangles) ++unsafeNoNear;
                const bool useLod = !useNear && safe && d > lod.lodDistance && t.chunkLod[c].size;
                const PcMesh& m = useNear ? nearM[c] : useLod ? far[c] : full[c];
                drawnBytes += useNear ? t.chunkNear[c].size : useLod ? t.chunkLod[c].size : t.chunks[c].mesh.size;
                ++chunks;
                if (useLod) ++lodChunks;
                if (useNear) ++nearChunks;
                tris += m.triangles;
                if (useNear) trisNear += m.triangles;
                if (useLod) trisLod += m.triangles;
                draws += m.batches;
                mats.insert(ci);
                for (uint16_t tx : m.texturePerBatch) binds.insert(tx);
                for (uint32_t i = 0; i < m.triangles; ++i)
                    cul.tri(toClip, m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2], static_cast<int32_t>(c), m.col[i]);
            }
        }
        // Holes: the reference shows a chunk the culled picture does not (a pixel left to the sky or to something
        // further). Shell cells and far meshes change the picture by design - only chunks drawn in full count.
        size_t holes = 0, refPx = 0;
        for (size_t o = 0; o < ref.id.size(); ++o) {
            if (ref.id[o] < 0) continue;
            ++refPx;
            // A shell pixel (-2) is the far level of detail by design (its shape is simplified): not a hole.
            if (cul.id[o] == -1 || (cul.id[o] >= 0 && cul.depth[o] > ref.depth[o] * 1.02f + 0.5f)) ++holes;
        }
        double texBytes = 0, colBytes = 0;
        for (uint16_t tx : binds)
            if (tx < t.h->textureCount) texBytes += t.textures[tx].size;
        for (uint32_t c = 0; c < t.h->chunkCount; ++c) {  // collision within 40 yd (XY) of the eye: wmocolyd
            glm::vec3 lo, hi;
            t.chunkBox(c, lo, hi);
            const float dx = std::max({lo.x - local.x, local.x - hi.x, 0.0f}), dy = std::max({lo.y - local.y, local.y - hi.y, 0.0f});
            if (dx * dx + dy * dy < 40.0f * 40.0f) colBytes += t.chunks[c].collision.size;
        }
        sum.drawnKb += drawnBytes / 1024.0;
        sum.texKb += texBytes / 1024.0;
        sum.colKb += colBytes / 1024.0;
        sum.maxResKb = std::max(sum.maxResKb, (drawnBytes + texBytes + colBytes) / 1024.0);
        // Binds: the PSP sorts the opaque draws by texture, so one bind per texture used (plus the shells' none).
        const double ge = 0.49e-3 * tris + 5.6e-3 * draws;
        const double cpu = 5.06e-3 * static_cast<double>(binds.size()) + 2.76e-3 * draws + 5.3e-3 * static_cast<double>(mats.size());
        csv << si << "," << s.eye.x << "," << s.eye.y << "," << s.eye.z << "," << s.yaw << "," << s.pitch << "," << camGroup
            << "," << (vis.outside ? 1 : 0) << "," << vis.visibleGroups << "," << vis.walked << "," << chunks << "," << nearChunks << "," << lodChunks
            << "," << shellCells << "," << tris << "," << draws << "," << binds.size() << "," << mats.size() << "," << ge
            << "," << cpu << "," << holes << "," << refPx << "," << drawnBytes / 1024.0 << "," << texBytes / 1024.0 << ","
            << colBytes / 1024.0 << "\n";
        sum.tris += tris;
        sum.near += trisNear;
        sum.lod += trisLod;
        sum.shell += trisShell;
        sum.draws += draws;
        sum.binds += static_cast<double>(binds.size());
        sum.mats += static_cast<double>(mats.size());
        sum.ge += ge;
        sum.cpu += cpu;
        const double holeShare = refPx ? static_cast<double>(holes) / static_cast<double>(refPx) : 0.0;
        sum.holes += holeShare;
        sum.maxTris = std::max(sum.maxTris, tris);
        sum.maxGe = std::max(sum.maxGe, ge);
        sum.maxCpu = std::max(sum.maxCpu, cpu);
        sum.maxHoles = std::max(sum.maxHoles, holeShare);
        ++sum.n;
        // + unsafe chunks without "n_" (their "c_" is already split to the edge: drawn as is).
        std::printf("stop %3zu (%.0f %.0f %.0f yaw %.0f): group %d%s vis %u/%u walked %u, chunks %u (lod %u, shells %u), "
                    "near %u (unsafe without n_ %u), tris %.0f, draws %.0f, binds %zu, mats %zu, GE %.1f ms CPU %.1f ms, "
                    "holes %zu of %zu px\n",
                    si, s.eye.x, s.eye.y, s.eye.z, s.yaw, camGroup, vis.outside ? " outside" : "", vis.visibleGroups,
                    t.h->groupCount, vis.walked, chunks, lodChunks, shellCells, nearChunks, unsafeNoNear, tris, draws,
                    binds.size(), mats.size(), ge, cpu, holes, refPx);
        if (si % 4 == 0 || holeShare > 0.01) {  // pictures: reference | culled | holes in red
            std::vector<uint8_t> img(static_cast<size_t>(RouteRaster::W) * 3 * RouteRaster::H * 4);
            for (int y = 0; y < RouteRaster::H; ++y)
                for (int x = 0; x < RouteRaster::W; ++x) {
                    const size_t o = static_cast<size_t>(y) * RouteRaster::W + x;
                    auto put = [&](int panel, uint32_t c) {
                        const size_t d = (static_cast<size_t>(y) * RouteRaster::W * 3 + panel * RouteRaster::W + x) * 4;
                        img[d] = c & 255;
                        img[d + 1] = (c >> 8) & 255;
                        img[d + 2] = (c >> 16) & 255;
                        img[d + 3] = 255;
                    };
                    put(0, ref.rgb[o]);
                    put(1, cul.rgb[o]);
                    const bool hole = ref.id[o] >= 0 && (cul.id[o] == -1 || (cul.id[o] >= 0 && cul.depth[o] > ref.depth[o] * 1.02f + 0.5f));
                    put(2, hole ? 0x0000FFu : (cul.id[o] == -1 ? 0x202020u : (cul.id[o] == -2 ? 0x808000u : 0x606060u)));
                }
            char nm[64];
            std::snprintf(nm, sizeof(nm), "/stop_%03zu.png", si);
            writePng(outDir + nm, RouteRaster::W * 3, RouteRaster::H, img);
        }
    }
    if (sum.n)
        std::printf("route: %zu stops; mean tris %.0f (max %.0f; of them near meshes %.0f, far meshes %.0f, shells %.0f), "
                    "draws %.0f, binds %.0f, matrices %.0f; GE %.1f ms (max %.1f), CPU %.1f ms (max %.1f); holes mean %.3f %% "
                    "(max %.3f %%)\n",
                    sum.n, sum.tris / sum.n, sum.maxTris, sum.near / sum.n, sum.lod / sum.n, sum.shell / sum.n,
                    sum.draws / sum.n, sum.binds / sum.n, sum.mats / sum.n, sum.ge / sum.n, sum.maxGe, sum.cpu / sum.n,
                    sum.maxCpu, 100.0 * sum.holes / sum.n, 100.0 * sum.maxHoles);
    if (sum.n)
        std::printf("route: resident at least (mean) drawn meshes %.0f KB + textures %.0f KB + collision (40 yd) %.0f KB; "
                    "max %.0f KB, + header %.0f KB\n",
                    sum.drawnKb / sum.n, sum.texKb / sum.n, sum.colKb / sum.n, sum.maxResKb, hdr->second.second / 1024.0);
    return true;
}

}  // namespace wowee::pspconv
