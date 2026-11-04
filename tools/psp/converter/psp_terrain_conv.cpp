// psp_terrain_conv - bakes client ADT terrain into PSP "WTER" tiles.
//
//   psp_terrain_conv --extracted <dir> --map <map> --out <file.pak>
//                    [--tile X_Y]... [--around X Y R]
//
// Reads the loose files written by upstream's asset_extract (lowercase paths,
// forward slashes). Parsing is upstream's ADTLoader + TerrainMeshGenerator,
// unchanged; this tool only reduces the result for the PSP:
//   - the outer 9x9 height grid of every chunk (the 8x8 centre vertices are
//     dropped), picked by vertex position so the MCVT layout cannot be misread;
//   - per vertex: sun lighting from the height-field normal (grey RGB565);
//   - per chunk (version 2): a 64x64 texture baked from the chunk's texture
//     layers. Each Tileset texture is box-filtered to 16x16 and repeated 4x4
//     across the chunk (upstream's layer UV = pos * 4 / CHUNK_SIZE), blended
//     by the 64x64 alpha maps texel for texel, then median-cut to 256 colours:
//     swizzled T8 + a 256-entry 5650 CLUT, 4.5 KB per chunk.
//   - lighting - the original client's baked terrain shading,
//     which upstream's ADTLoader skips: MCSH (64x64 1-bit shadow map per
//     chunk, same grid as the alpha maps) darkens the chunk texture
//     (1 - strength x shadow, 3x3 soft edge; --mcsh-strength, default 0.45,
//     --no-mcsh), MCCV (145 BGRA vertex shading, 0x7F = 1.0) multiplies the
//     vertex light (--no-mccv). WTER header flags: bit 1 MCSH, bit 2 MCCV
//     baked (the PSP ignores them; the format is unchanged).
//   - a map whose WDT names a global WMO
//     (MPHD 0x1 - dungeons, no ADTs; upstream WDTInfo::isWMOOnly) gets a pak
//     with the "wmo_only" record alone (include/rendering/psp/psp_map_format.hpp):
//     "no terrain here" for the PSP, which then stands on the WMO's floors.
// Layout of a tile: include/rendering/psp/psp_terrain.hpp.

#include "pipeline/adt_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "pipeline/wdt_loader.hpp"           // global-WMO maps
#include "rendering/psp/psp_map_format.hpp"  // global-WMO maps

#include "conv_common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace wowee;
using namespace wowee::pspconv;

namespace {

constexpr float kTileSize = 533.33333f;
constexpr float kUnit = kTileSize / 16.0f / 8.0f;
constexpr int kTex = 64;         // chunk texture size
constexpr int kTileTex = 16;     // one texture repeat, box-filtered
constexpr int kRepeats = 4;      // repeats per chunk (upstream layer UV)

using TileTex = std::array<Rgb, kTileTex * kTileTex>;

// Lighting - MCSH / MCCV of every MCNK, in file order (the order
// ADTLoader numbers its chunks). Sub-chunk offsets are read like
// ADTLoader::parseMCNK reads MCVT/MCAL: relative to the MCNK payload, an
// optional 8-byte sub-chunk header skipped when its magic is there.
struct ChunkShade {
    bool hasShadow = false;
    std::array<uint8_t, 4096> shadow{};  // 0/1, [row * 64 + col] like the alpha maps
    bool hasMccv = false;
    std::array<Rgb, 145> mccv{};         // 1.0 = neutral (0x7F)
};
struct ShadeOptions {
    bool mcsh = true, mccv = true;
    float mcshStrength = 0.45f;
};

std::vector<ChunkShade> readShade(const std::vector<uint8_t>& adt, const ShadeOptions& opt) {
    auto u32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= adt.size()) std::memcpy(&v, adt.data() + o, 4);
        return v;
    };
    constexpr uint32_t kMCNK = 0x4D434E4B, kMCSH = 0x4D435348, kMCCV = 0x4D434356;
    std::vector<ChunkShade> out;
    size_t off = 0;
    while (off + 8 <= adt.size() && out.size() < 256) {
        const uint32_t magic = u32(off), size = u32(off + 4);
        if (off + 8 + size > adt.size()) break;
        if (magic == kMCNK && size >= 128) {
            const size_t d = off + 8;  // payload
            ChunkShade cs;
            const uint32_t flags = u32(d + 0x00);
            const uint32_t ofsShadow = u32(d + 0x2C), sizeShadow = u32(d + 0x30), ofsMccv = u32(d + 0x74);
            if (opt.mcsh && (flags & 0x1u) && ofsShadow > 0 && sizeShadow >= 512) {
                size_t o = d + ofsShadow;
                if (u32(o) == kMCSH) o += 8;
                if (o + 512 <= d + size) {
                    for (int row = 0; row < 64; ++row)
                        for (int col = 0; col < 64; ++col)
                            cs.shadow[row * 64 + col] = (adt[o + row * 8 + col / 8] >> (col % 8)) & 1u;
                    cs.hasShadow = true;
                }
            }
            if (opt.mccv && (flags & 0x40u) && ofsMccv > 0) {
                size_t o = d + ofsMccv;
                if (u32(o) == kMCCV) o += 8;
                if (o + 145 * 4 <= d + size) {
                    for (int i = 0; i < 145; ++i) {
                        const uint8_t* c = &adt[o + i * 4];  // B, G, R, A
                        cs.mccv[i] = Rgb{c[2] / 127.0f, c[1] / 127.0f, c[0] / 127.0f, 1.0f};
                    }
                    cs.hasMccv = true;
                }
            }
            out.push_back(cs);
        }
        off += 8 + size;
    }
    return out;
}

// Tileset textures reduced to one 16x16 repeat (area average = no shimmer).
class TextureTiles {
public:
    explicit TextureTiles(std::string root) : root_(std::move(root)) {}
    const TileTex& get(const std::string& gamePath) {
        auto it = cache_.find(gamePath);
        if (it != cache_.end()) return it->second;
        TileTex t;
        t.fill(Rgb{0.4f, 0.45f, 0.3f});
        std::vector<uint8_t> bytes;
        if (readFile(root_ + "/" + lower(gamePath), bytes)) {
            pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
            if (!img.data.empty() && img.width >= kTileTex && img.height >= kTileTex) {
                const int bw = img.width / kTileTex, bh = img.height / kTileTex;
                for (int ty = 0; ty < kTileTex; ++ty) {
                    for (int tx = 0; tx < kTileTex; ++tx) {
                        double r = 0, g = 0, b = 0;
                        for (int y = 0; y < bh; ++y) {
                            for (int x = 0; x < bw; ++x) {
                                const uint8_t* p = &img.data[((ty * bh + y) * img.width + tx * bw + x) * 4];
                                r += p[0];
                                g += p[1];
                                b += p[2];
                            }
                        }
                        const double n = 255.0 * bw * bh;
                        t[ty * kTileTex + tx] = Rgb{static_cast<float>(r / n), static_cast<float>(g / n),
                                                    static_cast<float>(b / n)};
                    }
                }
            }
        } else {
            std::fprintf(stderr, "  missing texture %s\n", gamePath.c_str());
        }
        return cache_.emplace(gamePath, t).first->second;
    }

private:
    std::string root_;
    std::map<std::string, TileTex> cache_;
};

// Near-camera detail textures (v4): the dominant layer's Tileset texture at
// full resolution, turned into luminance detail centred on 0.5 so a
// modulate-2x pass adds the texture's grain without changing the baked
// colour. WTEX T8 grey (CLUT i/255), 128x128 + mips 64 and 32; the last
// mip (16) is flat 0.5 grey so the detail fades out with distance by
// itself. Stored once per texture as "dt_<n>" (shared by all tiles).
class DetailBank {
public:
    DetailBank(std::string root, std::vector<PakItem>& out) : root_(std::move(root)), out_(out) {}

    uint16_t get(const std::string& gamePath) {
        const std::string key = lower(gamePath);
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second;
        uint16_t id = 0xFFFF;
        std::vector<uint8_t> bytes;
        if (readFile(root_ + "/" + key, bytes)) {
            const pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
            if (!img.data.empty() && img.width >= 16 && img.height >= 16) {
                id = static_cast<uint16_t>(count_++);
                out_.push_back({"dt_" + std::to_string(id), 1, bake(img)});
            }
        }
        ids_.emplace(key, id);
        return id;
    }
    size_t count() const { return count_; }

private:
    static std::vector<float> down(const std::vector<float>& src, int sw, int sh, int dw, int dh) {
        std::vector<float> out(static_cast<size_t>(dw) * dh);
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
                const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
                double acc = 0;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx) acc += src[static_cast<size_t>(yy) * sw + xx];
                out[static_cast<size_t>(y) * dw + x] = static_cast<float>(acc / ((x1 - x0) * (y1 - y0)));
            }
        }
        return out;
    }

    std::vector<uint8_t> bake(const pipeline::BLPImage& img) {
        std::vector<float> lum(static_cast<size_t>(img.width) * img.height);
        double sum = 0;
        for (size_t i = 0; i < lum.size(); ++i) {
            const uint8_t* q = &img.data[i * 4];
            lum[i] = (0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2]) / 255.0f;
            sum += lum[i];
        }
        const float avg = std::max(1e-3f, static_cast<float>(sum / lum.size()));
        constexpr int kSize = 128;
        std::vector<float> lvl = down(lum, img.width, img.height, kSize, kSize);
        std::vector<uint8_t> blob;
        blob.insert(blob.end(), {'W', 'T', 'E', 'X'});
        blob.push_back(5);                    // GU_PSM_T8
        blob.push_back(1);                    // swizzled
        blob.push_back(static_cast<uint8_t>(4 | 8));  // linear | mipmaps
        blob.push_back(3);                    // extra levels: 64, 32, 16 (flat)
        for (uint16_t v : {uint16_t(kSize), uint16_t(kSize), uint16_t(kSize), uint16_t(kSize), uint16_t(256),
                           uint16_t(0)})
            put<uint16_t>(blob, v);
        put<uint32_t>(blob, 0);
        put<uint32_t>(blob, 32);
        put<uint32_t>(blob, 32 + 1024);
        for (int i = 0; i < 256; ++i) {
            const uint32_t g = static_cast<uint32_t>(i);
            put<uint32_t>(blob, 0xFF000000u | (g << 16) | (g << 8) | g);
        }
        auto emit = [&](const std::vector<float>& v, int size, bool flat) {
            std::vector<uint8_t> idx(v.size());
            for (size_t i = 0; i < v.size(); ++i) {
                const float d = flat ? 0.5f : std::clamp(0.5f * v[i] / avg, 0.0f, 1.0f);
                idx[i] = static_cast<uint8_t>(std::lround(d * 255.0f));
            }
            alignTo(blob, 16);
            const std::vector<uint8_t> sw = swizzle(idx, size, size);
            blob.insert(blob.end(), sw.begin(), sw.end());
        };
        emit(lvl, kSize, false);
        std::vector<float> l64 = down(lvl, kSize, kSize, 64, 64);
        emit(l64, 64, false);
        std::vector<float> l32 = down(l64, 64, 64, 32, 32);
        emit(l32, 32, false);
        emit(std::vector<float>(16 * 16, 0.5f), 16, true);
        return blob;
    }

    std::string root_;
    std::vector<PakItem>& out_;
    std::map<std::string, uint16_t> ids_;
    size_t count_ = 0;
};

bool bakeTile(const std::string& root, const std::string& map, int tx, int ty, TextureTiles& texTiles,
              DetailBank& details, const ShadeOptions& shadeOpt,  // lighting
              std::vector<uint8_t>& blob) {
    const std::string adtPath = root + "/world/maps/" + lower(map) + "/" + lower(map) + "_" +
                                std::to_string(tx) + "_" + std::to_string(ty) + ".adt";
    std::vector<uint8_t> bytes;
    if (!readFile(adtPath, bytes)) return false;
    pipeline::ADTTerrain terrain = pipeline::ADTLoader::load(bytes);
    terrain.coord.x = tx;  // load() leaves the tile coordinate to the caller
    terrain.coord.y = ty;
    pipeline::TerrainMesh mesh = pipeline::TerrainMeshGenerator::generate(terrain);
    const std::vector<ChunkShade> shade = readShade(bytes, shadeOpt);  // lighting
    int shadowChunks = 0, mccvChunks = 0;
    for (const ChunkShade& cs : shade) {
        shadowChunks += cs.hasShadow ? 1 : 0;
        mccvChunks += cs.hasMccv ? 1 : 0;
    }

    blob.clear();
    blob.insert(blob.end(), {'W', 'T', 'E', 'R'});
    put<uint16_t>(blob, 4);  // v3: chunk texture mips; v4: + per-chunk detail texture id
    // flags: bit 0 = per-chunk textures follow the chunk records;
    // bit 1 = MCSH, bit 2 = MCCV baked in (info only)
    put<uint16_t>(blob, static_cast<uint16_t>(1u | (shadowChunks ? 2u : 0u) | (mccvChunks ? 4u : 0u)));
    put<uint16_t>(blob, static_cast<uint16_t>(tx));
    put<uint16_t>(blob, static_cast<uint16_t>(ty));
    put<uint32_t>(blob, 256);
    put<float>(blob, kUnit);
    for (int i = 0; i < 3; ++i) put<uint32_t>(blob, 0);

    std::vector<uint8_t> textures;
    std::vector<uint16_t> detailIds;
    textures.reserve(256 * (512 + kTex * kTex));
    const glm::vec3 sun = glm::normalize(glm::vec3(0.35f, 0.25f, 0.9f));
    int badChunks = 0;
    for (int i = 0; i < 256; ++i) {
        const pipeline::ChunkMesh& cm = mesh.chunks[i];
        float grid[81];
        bool have[81] = {};
        float baseX = -1e30f, baseY = -1e30f;
        for (const auto& v : cm.vertices) {
            baseX = std::max(baseX, v.position[0]);
            baseY = std::max(baseY, v.position[1]);
        }
        for (const auto& v : cm.vertices) {
            const float fr = (baseX - v.position[0]) / kUnit, fc = (baseY - v.position[1]) / kUnit;
            const int r = static_cast<int>(std::lround(fr)), c = static_cast<int>(std::lround(fc));
            if (std::fabs(fr - r) > 0.05f || std::fabs(fc - c) > 0.05f) continue;  // centre vertex
            if (r < 0 || r > 8 || c < 0 || c > 8) continue;
            grid[r * 9 + c] = v.position[2];
            have[r * 9 + c] = true;
        }
        int missing = 0;
        for (int k = 0; k < 81; ++k)
            if (!have[k]) { grid[k] = 0.0f; ++missing; }
        if (missing) ++badChunks;
        if (cm.vertices.empty()) {  // chunk absent: keep a flat stub
            baseX = (32 - ty) * kTileSize - (i / 16) * 8 * kUnit;
            baseY = (32 - tx) * kTileSize - (i % 16) * 8 * kUnit;
        }
        const float baseZ = *std::min_element(grid, grid + 81);

        put<float>(blob, baseX);
        put<float>(blob, baseY);
        put<float>(blob, baseZ);
        put<uint16_t>(blob, cm.vertices.empty() ? 0xFFFFu : terrain.chunks[i].holes);  // preserve cave/city entrances
        put<uint16_t>(blob, 0);
        for (int k = 0; k < 81; ++k) {
            const float h = std::clamp((grid[k] - baseZ) * 32.0f, -32768.0f, 32767.0f);
            put<int16_t>(blob, static_cast<int16_t>(std::lround(h)));
        }
        for (int r = 0; r < 9; ++r) {
            for (int c = 0; c < 9; ++c) {
                // Height-field normal in server space (rows run -x, columns -y).
                auto hz = [&](int rr, int cc) {
                    rr = std::clamp(rr, 0, 8);
                    cc = std::clamp(cc, 0, 8);
                    return grid[rr * 9 + cc];
                };
                const float dzdx = (hz(r - 1, c) - hz(r + 1, c)) / (2 * kUnit);
                const float dzdy = (hz(r, c - 1) - hz(r, c + 1)) / (2 * kUnit);
                const glm::vec3 n = glm::normalize(glm::vec3(-dzdx, -dzdy, 1.0f));
                // Modulates the texture on the GE: keep <= 1.0.
                const float light = std::min(1.0f, 0.5f + 0.5f * std::max(0.0f, glm::dot(n, sun)));
                // Lighting - MCCV vertex shading (outer vertex r, c = r * 17 + c).
                if (i < static_cast<int>(shade.size()) && shade[i].hasMccv) {
                    const Rgb& v = shade[i].mccv[r * 17 + c];
                    put<uint16_t>(blob, to565(Rgb{std::min(1.0f, light * v.r), std::min(1.0f, light * v.g),
                                                  std::min(1.0f, light * v.b)}));
                    continue;
                }
                put<uint16_t>(blob, to565(Rgb{light, light, light}));
            }
        }

        // Chunk texture: texel (col, row) covers the same fraction of the chunk
        // as alpha map texel [row * 64 + col].
        std::vector<Rgb> px(kTex * kTex, Rgb{0.4f, 0.45f, 0.3f});
        const size_t nl = std::min<size_t>(cm.layers.size(), 4);
        const TileTex* layerTex[4] = {};
        for (size_t l = 0; l < nl; ++l) {
            const uint32_t id = cm.layers[l].textureId;
            if (id < mesh.textures.size()) layerTex[l] = &texTiles.get(mesh.textures[id]);
        }
        double cover[4] = {0, 0, 0, 0};  // final weight of each layer, for the detail texture
        for (int row = 0; row < kTex; ++row) {
            for (int col = 0; col < kTex; ++col) {
                float w[4] = {1, 0, 0, 0};
                for (size_t l = 1; l < nl; ++l) {
                    const auto& a = cm.layers[l].alphaData;
                    if (!layerTex[l] || a.size() < 4096) continue;
                    const float t = a[row * 64 + col] / 255.0f;
                    for (size_t k = 0; k < l; ++k) w[k] *= 1.0f - t;
                    w[l] = t;
                }
                for (size_t l = 0; l < nl; ++l) cover[l] += w[l];
                const int u = (col * kRepeats * kTileTex / kTex) % kTileTex;
                const int v = (row * kRepeats * kTileTex / kTex) % kTileTex;
                Rgb out = layerTex[0] ? (*layerTex[0])[v * kTileTex + u] : Rgb{0.4f, 0.45f, 0.3f};
                for (size_t l = 1; l < nl; ++l) {
                    const auto& a = cm.layers[l].alphaData;
                    if (!layerTex[l] || a.size() < 4096) continue;
                    const float t = a[row * 64 + col] / 255.0f;
                    const Rgb s = (*layerTex[l])[v * kTileTex + u];
                    out.r += (s.r - out.r) * t;
                    out.g += (s.g - out.g) * t;
                    out.b += (s.b - out.b) * t;
                }
                px[row * kTex + col] = out;
            }
        }
        // Lighting - MCSH, softened over 3x3 (the client filters it).
        if (i < static_cast<int>(shade.size()) && shade[i].hasShadow && shadeOpt.mcshStrength > 0.0f) {
            const auto& sh = shade[i].shadow;
            for (int row = 0; row < kTex; ++row) {
                for (int col = 0; col < kTex; ++col) {
                    int sum = 0, n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int rr = row + dy, cc = col + dx;
                            if (rr < 0 || cc < 0 || rr >= 64 || cc >= 64) continue;
                            sum += sh[rr * 64 + cc];
                            ++n;
                        }
                    const float k = 1.0f - shadeOpt.mcshStrength * static_cast<float>(sum) / static_cast<float>(n);
                    Rgb& q = px[row * kTex + col];
                    q.r *= k;
                    q.g *= k;
                    q.b *= k;
                }
            }
        }
        size_t dominant = 0;
        for (size_t l = 1; l < nl; ++l)
            if (cover[l] > cover[dominant]) dominant = l;
        uint16_t detail = 0xFFFF;
        if (nl > 0 && cm.layers[dominant].textureId < mesh.textures.size())
            detail = details.get(mesh.textures[cm.layers[dominant].textureId]);
        detailIds.push_back(detail);
        std::vector<uint8_t> idx;
        std::array<Rgb, 256> pal;
        quantize(px, idx, pal);
        for (const Rgb& p : pal) put<uint16_t>(textures, to565(p));
        const std::vector<uint8_t> sw = swizzle(idx, kTex, kTex);
        textures.insert(textures.end(), sw.begin(), sw.end());
        // v3 mips 32x32 and 16x16 on the same CLUT (PSP: the GE spent
        // 13 ms/frame on terrain - distant chunks thrashed the texture cache,
        // as the doodads did before their mips).
        std::vector<Rgb> m = px;
        for (int size = kTex / 2; size >= 16; size /= 2) {
            std::vector<Rgb> n(static_cast<size_t>(size) * size);
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const int w = size * 2;
                    const Rgb& a = m[(2 * y) * w + 2 * x];
                    const Rgb& b = m[(2 * y) * w + 2 * x + 1];
                    const Rgb& c = m[(2 * y + 1) * w + 2 * x];
                    const Rgb& d = m[(2 * y + 1) * w + 2 * x + 1];
                    n[y * size + x] = Rgb{(a.r + b.r + c.r + d.r) / 4, (a.g + b.g + c.g + d.g) / 4,
                                          (a.b + b.b + c.b + d.b) / 4, 1.0f};
                }
            }
            m.swap(n);
            std::vector<uint8_t> mi(m.size());
            for (size_t i = 0; i < m.size(); ++i) {
                int best = 0;
                float bestD = 1e9f;
                for (int k = 0; k < 256; ++k) {
                    const float dr = m[i].r - pal[k].r, dg = m[i].g - pal[k].g, db = m[i].b - pal[k].b;
                    const float dd = dr * dr + dg * dg + db * db;
                    if (dd < bestD) {
                        bestD = dd;
                        best = k;
                    }
                }
                mi[i] = static_cast<uint8_t>(best);
            }
            const std::vector<uint8_t> ms = swizzle(mi, size, size);
            textures.insert(textures.end(), ms.begin(), ms.end());
        }
    }
    blob.insert(blob.end(), textures.begin(), textures.end());
    for (uint16_t d : detailIds) put<uint16_t>(blob, d);  // v4
    std::printf("  tile %d_%d: %zu textures, chunks with gaps %d, mcsh %d mccv %d, %zu bytes\n", tx, ty,
                mesh.textures.size(), badChunks, shadowChunks, mccvChunks, blob.size());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string root, map = "Azeroth", out;
    std::vector<std::pair<int, int>> tiles;
    ShadeOptions shadeOpt;  // lighting
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-mcsh") shadeOpt.mcsh = false;  // lighting
        else if (a == "--no-mccv") shadeOpt.mccv = false;
        else if (a == "--mcsh-strength" && i + 1 < argc) shadeOpt.mcshStrength = std::clamp(std::stof(argv[++i]), 0.0f, 1.0f);
        else if (a == "--extracted" && i + 1 < argc) root = argv[++i];
        else if (a == "--map" && i + 1 < argc) map = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--tile" && i + 1 < argc) {
            int x, y;
            if (std::sscanf(argv[++i], "%d_%d", &x, &y) == 2) tiles.emplace_back(x, y);
        } else if (a == "--around" && i + 3 < argc) {
            int x = std::atoi(argv[++i]), y = std::atoi(argv[++i]), r = std::atoi(argv[++i]);
            for (int dx = -r; dx <= r; ++dx)
                for (int dy = -r; dy <= r; ++dy) tiles.emplace_back(x + dx, y + dy);
        }
    }
    if (root.empty() || out.empty() || tiles.empty()) {
        std::fprintf(stderr, "usage: psp_terrain_conv --extracted <dir> --map <Name> --out <pak> "
                             "(--tile X_Y | --around X Y R)... [--no-mcsh] [--no-mccv] [--mcsh-strength 0.45]\n");
        return 2;
    }
    // A global-WMO map has no terrain - the marker record alone (upstream
    // WorldLoader::loadMapGeometry reads the WDT first and loads no ADT then).
    {
        std::vector<uint8_t> wdtBytes;
        pipeline::WDTInfo wdt;
        if (readFile(root + "/world/maps/" + lower(map) + "/" + lower(map) + ".wdt", wdtBytes))
            wdt = pipeline::parseWDT(wdtBytes);
        if (wdt.isWMOOnly() && !wdt.rootWMOPath.empty()) {
            namespace rp = wowee::rendering::psp;
            rp::WmoOnlyHeader h{};
            std::memcpy(h.magic, "WWMO", 4);
            h.version = rp::kWmoOnlyVersion;
            h.mphdFlags = wdt.mphdFlags;
            for (int a = 0; a < 3; ++a) h.rotation[a] = wdt.rotation[a];
            h.doodadSet = wdt.doodadSet;
            std::snprintf(h.wmo, sizeof(h.wmo), "%s", lower(wdt.rootWMOPath).c_str());
            std::vector<uint8_t> rec(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
            std::printf("map %s: global wmo %s (MPHD 0x%x) - no terrain, \"wmo_only\" record only\n", map.c_str(),
                        wdt.rootWMOPath.c_str(), static_cast<unsigned>(wdt.mphdFlags));
            return writePak(out, {PakItem{"wmo_only", 3, std::move(rec)}}) ? 0 : 1;
        }
    }
    TextureTiles texTiles(root);
    std::vector<PakItem> entries, detailItems;
    DetailBank details(root, detailItems);
    for (auto [x, y] : tiles) {
        std::vector<uint8_t> blob;
        if (!bakeTile(root, map, x, y, texTiles, details, shadeOpt, blob)) {
            std::printf("  tile %d_%d: no ADT, skipped\n", x, y);
            continue;
        }
        entries.push_back({"t_" + std::to_string(x) + "_" + std::to_string(y), 4, std::move(blob)});
    }
    if (entries.empty()) return 1;
    std::printf("detail textures %zu\n", details.count());
    for (auto& d : detailItems) entries.push_back(std::move(d));
    return writePak(out, entries) ? 0 : 1;
}
