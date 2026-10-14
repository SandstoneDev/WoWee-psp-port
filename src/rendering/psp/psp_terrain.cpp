#include "rendering/psp/psp_terrain.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/psp/psp_cpu_fast.hpp"

#include "platform/psp/psp_bigheap.hpp"  // pp::heapRoomFor
#include "platform/psp/psp_config.hpp"   // The terrain pool's keys
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "rendering/psp/psp_frustum.hpp"
#include "rendering/psp/psp_lighting_ge.hpp"  // Lighting

#include <pspgu.h>
#include <pspgum.h>
#include <psputils.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <vector>

namespace wowee::rendering::psp {

namespace pp = wowee::platform::psp;

namespace {
// Memory diet: the terrain pool - one block for the run (TerrainRenderer::poolInit).
TerrainPool g_pool;
}  // namespace
namespace prof = wowee::platform::psp::prof;

namespace {

constexpr int kGrid = 9;         // vertices per chunk side (outer MCVT grid)
// Skirts: every edge vertex again, dropped by kSkirtDepth, so the gap a
// T-junction leaves between chunks of different LOD is closed by a wall.
constexpr int kSkirtVerts = 4 * kGrid;
constexpr int kChunkVerts = kGrid * kGrid + kSkirtVerts;  // 117
constexpr int16_t kSkirtDepth = 8 * 32;                    // 8 yd in 1/32 yd

// Edge e (0: row 0, 1: row 8, 2: col 0, 3: col 8), position i along it.
inline int edgeVertex(int e, int i) {
    switch (e) {
        case 0: return i;
        case 1: return 8 * kGrid + i;
        case 2: return i * kGrid;
        default: return i * kGrid + 8;
    }
}
inline int skirtVertex(int e, int i) { return kGrid * kGrid + e * kGrid + i; }
constexpr int kChunkBytes = 340;
constexpr int kLocalStep = 1024; // s16 units per grid step

template <typename T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// PSP (hardware crash hw2: TerrainRenderer::loadTile+0x364, a store through the null vertex block of a failed
// memalign - DBE): no allocation on the tile path goes through operator new (its handler logs "oom new failed" and the
// next failure ends the game) - a tile that finds no memory is not loaded and comes again later (the streamer's retry).
template <typename T>
T* allocPod() {
    void* p = std::malloc(sizeof(T));
    return p ? new (p) T{} : nullptr;
}
template <typename T>
void freePod(T* p) {
    if (!p) return;
    p->~T();
    std::free(p);
}

} // namespace

TerrainRenderer::TerrainRenderer() {
    // No vector growth on the load / draw paths (operator new's handler is fatal): room up front.
    tiles_.reserve(16);
    near_.reserve(256);
    // Shared indexed triangle *strips* for LOD steps 1, 2, 4, 8 over the 9x9
    // grid (PSP hardware: the GE spent 12 ms/frame on ~135k terrain vertices
    // from indexed lists, 3 per triangle; a strip sends ~1.2). Rows and the
    // four skirt walls are joined by degenerate triangles; winding does not
    // matter, terrain is drawn with culling off.
    for (int l = 0; l < 4; ++l) {
        const int step = 1 << l;
        const int quads = (kGrid - 1) / step;
        std::vector<uint16_t> idx;
        auto run = [&](auto&& vertexA, auto&& vertexB) {
            // One strip: A0 B0 A1 B1 ... ; joined to the previous one by
            // repeating its last index and this run's first index.
            if (!idx.empty()) {
                idx.push_back(idx.back());
                idx.push_back(static_cast<uint16_t>(vertexA(0)));
            }
            for (int i = 0; i <= quads; ++i) {
                idx.push_back(static_cast<uint16_t>(vertexA(i)));
                idx.push_back(static_cast<uint16_t>(vertexB(i)));
            }
        };
        for (int r = 0; r < quads; ++r)
            run([&](int c) { return (r * step) * kGrid + c * step; },
                [&](int c) { return ((r + 1) * step) * kGrid + c * step; });
        for (int e = 0; e < 4; ++e)  // skirt walls at this LOD's edge spacing
            run([&](int i) { return edgeVertex(e, i * step); }, [&](int i) { return skirtVertex(e, i * step); });
        lodCounts_[l] = static_cast<int>(idx.size());
        lodTriangles_[l] = quads * quads * 2 + 4 * quads * 2;
        lodIndices_[l] = static_cast<uint16_t*>(memalign(16, idx.size() * sizeof(uint16_t)));
        std::memcpy(lodIndices_[l], idx.data(), idx.size() * sizeof(uint16_t));
        sceKernelDcacheWritebackRange(lodIndices_[l], idx.size() * sizeof(uint16_t));
    }
}

TerrainRenderer::~TerrainRenderer() {
    clear();
    for (auto* p : lodIndices_) std::free(p);
}

void TerrainRenderer::loadDetail(pp::Pak& pak, uint16_t id) {
    if (id == 0xFFFF) return;
    if (id >= details_.size()) {
        // The table grows through operator new - only with the room for it (a missing detail texture is
        // cosmetic: that chunk draws without its near detail pass).
        const size_t want = std::max<size_t>(id + 1u, details_.capacity() * 2u);
        if (want > details_.capacity() && !pp::heapRoomFor(static_cast<uint32_t>(want * sizeof(pp::Texture)), 64u)) return;
        details_.reserve(want);
        details_.resize(id + 1u);
    }
    if (details_[id].pixels) return;
    char name[16];
    std::snprintf(name, sizeof(name), "dt_%u", static_cast<unsigned>(id));
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob && !pp::textureFromBlob(blob, e->size, details_[id], true)) pp::pakFree(blob);
}

void TerrainRenderer::clear() {
    for (auto& d : details_)
        if (d.block) pp::textureRelease(d);
    details_.clear();
    for (Tile* t : tiles_) {
        if (t->windowed)
            for (Chunk& ch : t->chunks) texFree(*t, ch);
        pp::pakFree(t->blob);
        deleteTile(t);
    }
    tiles_.clear();
    near_.clear();
    stagedHead_ = stagedTable_ = false;  // world_flush: a head staged for a job that went with the flush
}

// The chunks of a new tile (was inline in loadTile). records: 256 x 340 bytes; texData: the blob's texture
// blocks or null (the window: textures come later per chunk row); detailTable: 256 u16 or null.
bool TerrainRenderer::buildTile(Tile& tile, const uint8_t* records, const TerrainLayout& layout, const uint8_t* texData,
                                const uint8_t* detailTable, pp::Pak& pak) {
    if (!tile.vertexBlock)  // A pool tile has its vertex block in its slot already
        tile.vertexBlock = static_cast<TerrainVertex*>(memalign(16, 256 * kChunkVerts * sizeof(TerrainVertex)));
    if (!tile.vertexBlock) {
        lastNoMem_ = true;
        return false;
    }
    for (int i = 0; i < 256; ++i) {
        const uint8_t* c = records + i * kChunkBytes;
        Chunk& ch = tile.chunks[i];
        ch.baseX = rd<float>(c + 0);
        ch.baseY = rd<float>(c + 4);
        ch.baseZ = rd<float>(c + 8);
        TerrainVertex* v = tile.vertexBlock + i * kChunkVerts;
        ch.tex = nullptr;
        ch.texState = 0;
        ch.clut = texData ? reinterpret_cast<const uint16_t*>(texData + i * layout.texBytes) : nullptr;
        ch.texels = texData ? texData + i * layout.texBytes + 512 : nullptr;
        ch.detail = 0xFFFF;
        if (detailTable) {
            ch.detail = rd<uint16_t>(detailTable + i * 2u);
            loadDetail(pak, ch.detail);
        }
        int16_t hmin = 32767, hmax = -32768;
        for (int r = 0; r < kGrid; ++r) {
            for (int col = 0; col < kGrid; ++col) {
                const int k = r * kGrid + col;
                const int16_t h = rd<int16_t>(c + 16 + k * 2);
                v[k].color = rd<uint16_t>(c + 16 + 162 + k * 2);
                v[k].u = static_cast<uint8_t>(col * 16);  // 0..128 across the chunk
                v[k].v = static_cast<uint8_t>(r * 16);
                v[k].x = static_cast<int16_t>(-r * kLocalStep);
                v[k].y = static_cast<int16_t>(-col * kLocalStep);
                v[k].z = h;
                if (h < hmin) hmin = h;
                if (h > hmax) hmax = h;
            }
        }
        for (int e = 0; e < 4; ++e) {
            for (int k = 0; k < kGrid; ++k) {
                TerrainVertex s = v[edgeVertex(e, k)];
                s.z = static_cast<int16_t>(s.z - kSkirtDepth < -32768 ? -32768 : s.z - kSkirtDepth);
                v[skirtVertex(e, k)] = s;
            }
        }
        ch.minZ = ch.baseZ + hmin / 32.0f;
        ch.maxZ = ch.baseZ + hmax / 32.0f;
        ch.verts = v;
        terrainChunkSphere(c, tile.unit, ch.cx, ch.cy, ch.rad);  // The texture window's distance
    }
    // Written once here, read by the GE from now on.
    sceKernelDcacheWritebackRange(tile.vertexBlock, 256 * kChunkVerts * sizeof(TerrainVertex));
    return true;
}

bool TerrainRenderer::loadTile(pp::Pak& pak, int tileX, int tileY) {
    lastNoMem_ = false;
    char name[32];
    std::snprintf(name, sizeof(name), "t_%d_%d", tileX, tileY);
    const pp::PakEntry* e = pak.find(name);
    if (!e) return false;
    if (!roomForTile()) return false;
    if (texWindow) return loadTileWindowed(pak, e, tileX, tileY);  // The head only
    auto* blob = static_cast<uint8_t*>(pak.load(e));
    if (!blob) {
        lastNoMem_ = true;  // No block for the 1.6 MB blob (pak_alloc_fail) or a failed read - again later
        return false;
    }
    TerrainLayout layout;
    if (!terrainLayout(blob, e->size, e->size, layout)) {
        PSP_LOGE(pp::tag::Gfx, "wter_bad name=%s size=%u", name, static_cast<unsigned>(e->size));
        pp::pakFree(blob);
        return false;
    }

    auto* tile = newTile();  // A pool slot, or ~15 KB not through operator new
    if (!tile) {
        lastNoMem_ = true;
        pp::pakFree(blob);
        return false;
    }
    tile->tileX = tileX;
    tile->tileY = tileY;
    tile->unit = rd<float>(blob + 16);
    tile->hasMips = layout.textured && layout.version >= 3;
    if (!buildTile(*tile, blob + kTerrainHeaderBytes, layout, layout.textured ? blob + layout.texBase : nullptr,
                   layout.detailOffset ? blob + layout.detailOffset : nullptr, pak)) {
        pp::pakFree(blob);
        deleteTile(tile);
        return false;
    }
    tile->blob = blob;  // textures stay here (pak.load wrote the D-cache back)
    tile->bytes = e->size + 256u * kChunkVerts * sizeof(TerrainVertex);
    tiles_.push_back(tile);
    return true;
}

// The window's tile - the head (parked by the streamer, or read now) and the v4 detail table; no
// texture is read here (texSyncLoad / the streamer's runs).
bool TerrainRenderer::loadTileWindowed(pp::Pak& pak, const pp::PakEntry* e, int tileX, int tileY) {
    const uint32_t headBytes = std::min<uint32_t>(e->size, kTerrainHeadBytes);
    // The pool's head buffer - read there by the streamer's job (headStaged), else read into it now.
    uint8_t* const poolHeadBuf = g_pool.ready() ? g_pool.head() : nullptr;
    const bool staged = poolHeadBuf && stagedHead_ && stagedX_ == tileX && stagedY_ == tileY;
    const bool stagedTable = staged && stagedTable_;
    stagedHead_ = stagedTable_ = false;
    uint8_t* head = staged ? poolHeadBuf : nullptr;
    if (!head && poolHeadBuf) {
        if (!pak.readPart(e, 0, poolHeadBuf, headBytes)) {
            lastNoMem_ = true;  // a failed read (pak_read_fail): the streamer asks again later
            return false;
        }
        head = poolHeadBuf;
    }
    if (!head) head = static_cast<uint8_t*>(pp::pakPrefetchTake(pak.fileId(), e->offset));
    auto freeHead = [&]() {
        if (head != poolHeadBuf) std::free(head);
    };
    if (!head) {
        head = static_cast<uint8_t*>(std::malloc(headBytes));
        if (!head) {
            lastNoMem_ = true;
            return false;
        }
        if (!pak.readPart(e, 0, head, headBytes)) {
            std::free(head);
            lastNoMem_ = true;  // a failed read (pak_read_fail): the streamer asks again later
            return false;
        }
    }
    TerrainLayout layout;
    if (!terrainLayout(head, headBytes, e->size, layout)) {
        PSP_LOGE(pp::tag::Gfx, "wter_bad name=%s size=%u", e->name, static_cast<unsigned>(e->size));
        freeHead();
        return false;
    }
    uint8_t table[kTerrainDetailTableBytes];
    bool haveTable = false;
    if (layout.detailOffset) {
        if (stagedTable) {  // Read by the job after the head, into the pool's head buffer
            std::memcpy(table, poolHeadBuf + kTerrainHeadBytes, sizeof(table));
            haveTable = true;
        } else if (void* parked = pp::pakPrefetchTake(pak.fileId(), e->offset + layout.detailOffset)) {
            std::memcpy(table, parked, sizeof(table));
            std::free(parked);
            haveTable = true;
        } else {
            haveTable = pak.readPart(e, layout.detailOffset, table, sizeof(table));
        }
    }
    auto* tile = newTile();  // A pool slot, or ~15 KB not through operator new
    if (!tile) {
        lastNoMem_ = true;
        freeHead();
        return false;
    }
    tile->tileX = tileX;
    tile->tileY = tileY;
    tile->unit = rd<float>(head + 16);
    tile->hasMips = layout.textured && layout.version >= 3;
    if (!buildTile(*tile, head + kTerrainHeaderBytes, layout, nullptr, haveTable ? table : nullptr, pak)) {
        freeHead();
        deleteTile(tile);
        return false;
    }
    freeHead();
    tile->blob = nullptr;
    tile->windowed = layout.textured;  // an untextured tile has nothing to window
    tile->fileOffset = e->offset;
    tile->texBase = layout.texBase;
    tile->texBytes = layout.texBytes;
    tile->bytes = 256u * kChunkVerts * sizeof(TerrainVertex);
    tiles_.push_back(tile);
    return true;
}

// Tile streaming.
bool TerrainRenderer::hasTile(int tileX, int tileY) const {
    for (const Tile* t : tiles_)
        if (t->tileX == tileX && t->tileY == tileY) return true;
    return false;
}

// tiles_ grows through operator new only with the room for it (16 reserved: the 3x3 + those leaving).
bool TerrainRenderer::roomForTile() {
    if (tiles_.size() < tiles_.capacity()) return true;
    if (pp::heapRoomFor(static_cast<uint32_t>((tiles_.capacity() * 2u + 1u) * sizeof(Tile*)), 64u)) {
        tiles_.reserve(tiles_.capacity() * 2u + 1u);
        return true;
    }
    lastNoMem_ = true;
    return false;
}

TerrainRenderer::Tile* TerrainRenderer::findTile(int tileX, int tileY) {
    for (Tile* t : tiles_)
        if (t->tileX == tileX && t->tileY == tileY) return t;
    return nullptr;
}

uint32_t TerrainRenderer::unloadTile(int tileX, int tileY) {
    for (size_t i = 0; i < tiles_.size(); ++i) {
        Tile* t = tiles_[i];
        if (t->tileX != tileX || t->tileY != tileY) continue;
        const uint32_t bytes = t->bytes;
        tiles_.erase(tiles_.begin() + static_cast<std::ptrdiff_t>(i));
        near_.clear();  // held chunk pointers of the last frame
        if (t->windowed)
            for (Chunk& ch : t->chunks) texFree(*t, ch);
        pp::pakFree(t->blob);
        deleteTile(t);
        return bytes;
    }
    return 0;
}

void TerrainRenderer::missingDetails(const void* blobPtr, uint32_t size, std::vector<uint16_t>& out) const {
    const auto* blob = static_cast<const uint8_t*>(blobPtr);
    TerrainLayout layout;
    if (!blob || !terrainLayout(blob, size, size, layout) || !layout.detailOffset) return;
    missingDetailsTable(blob + layout.detailOffset, out);
}

void TerrainRenderer::missingDetailsTable(const uint8_t* table, std::vector<uint16_t>& out) const {
    if (!table) return;
    for (uint32_t i = 0; i < 256; ++i) {
        const uint16_t id = rd<uint16_t>(table + i * 2u);
        if (id == 0xFFFF || (id < details_.size() && details_[id].pixels)) continue;
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
}

uint32_t TerrainRenderer::residentBytes() const {
    uint32_t b = 0;
    for (const Tile* t : tiles_) b += t->bytes;
    return b;
}

// ---- Memory diet: the chunk-texture window ------------------------------------------------------

// ---- Memory diet: the terrain pool -------------------------------------------------------------

bool TerrainRenderer::poolInit() {
    if (g_pool.ready()) return true;
    if (pp::configGetInt("terraintex", 1) == 0 || pp::configGetInt("stream", 1) == 0) {
        PSP_LOGI(pp::tag::Mem, "terrain_pool off (terraintex=0 or stream=0: whole tiles as before)");
        return false;
    }
    TexWindowRule rule = texWindowRule(TerrainCamera{}.farZ, 64.0f, 48.0f);
    if (const int keepYd = pp::configGetInt("terraintexyd", 0); keepYd > 0) rule.keepYd = static_cast<float>(keepYd);
    TerrainPoolLayout l;
    const int slots = pp::configGetInt("terraintexslots", 0);
    l.texSlots = slots > 0 ? static_cast<uint32_t>(std::clamp(slots, 64, 2304)) : terrainTexSlotsFor(rule.keepYd);
    l.texSlotBytes = 512u + 4096u + 1024u + 256u;  // the largest chunk texture (v3 / v4)
    l.tileSlots = static_cast<uint32_t>(std::clamp(pp::configGetInt("terraintileslots", 12), 4, 32));
    l.tileSlotBytes = TerrainPool::align16(static_cast<uint32_t>(sizeof(Tile))) +
                      static_cast<uint32_t>(256 * kChunkVerts * sizeof(TerrainVertex));
    l.headBytes = kTerrainHeadBytes + kTerrainDetailTableBytes;
    l.runSlots = static_cast<uint32_t>(std::clamp(pp::configGetInt("terraintexreads", 6), 1, 8));
    l.runSlotBytes = 16u * l.texSlotBytes;
    const size_t bytes = TerrainPool::bytesFor(l);
    void* mem = memalign(64, bytes);
    if (!mem || !g_pool.init(mem, bytes, l)) {
        std::free(mem);
        PSP_LOGE(pp::tag::Mem, "terrain_pool_fail kb=%u (per-tile allocations)", static_cast<unsigned>(bytes / 1024u));
        return false;
    }
    PSP_LOGI(pp::tag::Mem, "terrain_pool kb=%u tex_slots=%u tile_slots=%u tile_kb=%u runs=%u keep_yd=%.0f base=%p",
             static_cast<unsigned>(bytes / 1024u), static_cast<unsigned>(l.texSlots),
             static_cast<unsigned>(l.tileSlots), static_cast<unsigned>(l.tileSlotBytes / 1024u),
             static_cast<unsigned>(l.runSlots), rule.keepYd, mem);
    return true;
}

bool TerrainRenderer::pooled() { return g_pool.ready(); }
uint32_t TerrainRenderer::poolTilesFree() { return g_pool.tileFree(); }
uint8_t* TerrainRenderer::poolHead() { return g_pool.ready() ? g_pool.head() : nullptr; }
uint8_t* TerrainRenderer::poolRunAlloc() { return g_pool.ready() ? g_pool.allocRun() : nullptr; }
void TerrainRenderer::poolRunFree(void* p) {
    if (p && g_pool.ready()) g_pool.freeRun(p);
}
uint32_t TerrainRenderer::poolRunsFree() { return g_pool.ready() ? g_pool.runFree() : 0u; }
void TerrainRenderer::poolUsage(uint32_t& tex, uint32_t& tiles, uint32_t& runs) {
    tex = g_pool.ready() ? g_pool.texUsed() : 0u;
    tiles = g_pool.ready() ? g_pool.tileUsed() : 0u;
    runs = g_pool.ready() ? g_pool.runUsed() : 0u;
}
uint32_t TerrainRenderer::poolReset() {
    if (!g_pool.ready()) return 0;
    const uint32_t lost = g_pool.texUsed() + g_pool.tileUsed() + g_pool.runUsed();
    g_pool.resetFree();
    return lost;
}

TerrainRenderer::Tile* TerrainRenderer::newTile() {
    if (g_pool.ready()) {
        uint8_t* slot = g_pool.allocTile();
        if (!slot) return nullptr;  // every slot in use: the streamer evicts outside the window first
        Tile* t = new (slot) Tile{};
        t->vertexBlock = reinterpret_cast<TerrainVertex*>(slot + TerrainPool::align16(static_cast<uint32_t>(sizeof(Tile))));
        return t;
    }
    return allocPod<Tile>();
}

void TerrainRenderer::deleteTile(Tile* t) {
    if (!t) return;
    if (g_pool.ready() && g_pool.contains(t)) {
        t->~Tile();
        g_pool.freeTile(t);
        return;
    }
    std::free(t->vertexBlock);
    freePod(t);
}

// A chunk texture's block: a pool slot - none free: the farthest resident chunk's, when it lies farther than this one
// (the pool is sized for the window: that one is past keep) - else memalign without a pool.
uint8_t* TerrainRenderer::texBlock(const Tile& forTile, const Chunk& forChunk) {
    if (!g_pool.ready()) return static_cast<uint8_t*>(memalign(16, forTile.texBytes));
    if (uint8_t* b = g_pool.allocTex()) return b;
    const auto d2 = [&](const Chunk& c) { return (c.cx - texPx_) * (c.cx - texPx_) + (c.cy - texPy_) * (c.cy - texPy_); };
    const float mine = d2(forChunk);
    Tile* farTile = nullptr;
    Chunk* far = nullptr;
    float farD2 = mine;
    for (Tile* t : tiles_) {
        if (!t->windowed) continue;
        for (Chunk& c : t->chunks) {
            if (c.texState != 2) continue;
            const float d = d2(c);
            if (d > farD2) {
                farD2 = d;
                far = &c;
                farTile = t;
            }
        }
    }
    if (!far) {
        ++texPoolFull_;
        return nullptr;
    }
    ++texPoolEvict_;
    texFree(*farTile, *far);  // between frames (the streamer's update): the GE is done with it
    return g_pool.allocTex();
}

void TerrainRenderer::texRelease(uint8_t* block) {
    if (g_pool.ready() && g_pool.contains(block)) g_pool.freeTex(block);
    else std::free(block);
}

void TerrainRenderer::texFree(Tile& t, Chunk& ch) {
    if (ch.tex) {
        texRelease(ch.tex);
        ch.tex = nullptr;
        t.bytes -= std::min(t.bytes, t.texBytes);
        ++texFrees_;
    }
    ch.texState = 0;
    ch.clut = nullptr;
    ch.texels = nullptr;
}

uint32_t TerrainRenderer::texInstall(int tileX, int tileY, uint16_t first, uint16_t count, const void* blobPtr,
                                     uint32_t size) {
    Tile* t = findTile(tileX, tileY);
    if (!t || !t->windowed || !blobPtr || first >= 256u || count > 256u - first || size < count * t->texBytes) return 0;
    const auto* src = static_cast<const uint8_t*>(blobPtr);
    uint32_t installed = 0;
    for (uint32_t k = 0; k < count; ++k) {
        Chunk& ch = t->chunks[first + k];
        if (ch.texState == 2) continue;  // a duplicate read (the tile went and came back meanwhile)
        if (texWindowWant(ch.cx, ch.cy, ch.rad, texPx_, texPy_, texRule) < 0) {
            ch.texState = 0;  // the player went away while it was read
            continue;
        }
        auto* block = texBlock(*t, ch);  // A pool slot (or memalign without the pool)
        if (!block) {
            ch.texState = 0;  // asked again by a later step
            if (!g_pool.ready()) ++texNomem_;  // the heap was short (the pool full is texPoolFull_, no heap matter)
            continue;
        }
        std::memcpy(block, src + k * t->texBytes, t->texBytes);
        sceKernelDcacheWritebackRange(block, t->texBytes);
        ch.tex = block;
        ch.clut = reinterpret_cast<const uint16_t*>(block);
        ch.texels = block + 512;
        ch.texState = 2;
        t->bytes += t->texBytes;
        ++texInstalls_;
        ++installed;
    }
    return installed;
}

void TerrainRenderer::texCancel(int tileX, int tileY, uint16_t first, uint16_t count) {
    Tile* t = findTile(tileX, tileY);
    if (!t || first >= 256u) return;
    for (uint32_t k = 0; k < count && first + k < 256u; ++k)
        if (t->chunks[first + k].texState == 1) t->chunks[first + k].texState = 0;
}

bool TerrainRenderer::texStep(float px, float py, uint32_t maxRuns, std::vector<TexRequest>& out) {
    texPx_ = px;
    texPy_ = py;
    struct Pick {
        Tile* t;
        TexRun r;
    };
    std::vector<Pick> picks;
    std::vector<TexRun> runs;
    uint8_t need[256];
    float d2[256];
    for (Tile* t : tiles_) {
        if (!t->windowed) continue;
        bool any = false;
        for (int i = 0; i < 256; ++i) {
            Chunk& ch = t->chunks[i];
            const int w = texWindowWant(ch.cx, ch.cy, ch.rad, px, py, texRule);
            if (w < 0 && ch.texState == 2) texFree(*t, ch);
            need[i] = w > 0 && ch.texState == 0 ? 1 : 0;
            any = any || need[i];
            const float dx = ch.cx - px, dy = ch.cy - py;
            d2[i] = dx * dx + dy * dy;
        }
        if (!any) continue;
        runs.clear();
        texWindowRuns(need, d2, runs);
        for (const TexRun& r : runs) picks.push_back(Pick{t, r});
    }
    std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.r.d2 < b.r.d2; });
    const size_t n = std::min<size_t>(picks.size(), maxRuns);
    for (size_t k = 0; k < n; ++k) {
        const Pick& p = picks[k];
        TexRequest q;
        q.tileX = p.t->tileX;
        q.tileY = p.t->tileY;
        q.first = p.r.first;
        q.count = p.r.count;
        q.offset = p.t->fileOffset + p.t->texBase + p.r.first * p.t->texBytes;
        q.size = p.r.count * p.t->texBytes;
        for (uint32_t c = 0; c < p.r.count; ++c) p.t->chunks[p.r.first + c].texState = 1;
        out.push_back(q);
    }
    return picks.size() > n;
}

void TerrainRenderer::texRunsForHead(const uint8_t* head, uint32_t headSize, uint32_t entrySize, uint32_t entryOffset,
                                     float px, float py, const TexWindowRule& rule, int tileX, int tileY,
                                     std::vector<TexRequest>& out) {
    TerrainLayout layout;
    if (!terrainLayout(head, headSize, entrySize, layout) || !layout.textured || headSize < kTerrainHeadBytes) return;
    const float unit = rd<float>(head + 16);
    uint8_t need[256];
    float d2[256];
    bool any = false;
    for (int i = 0; i < 256; ++i) {
        float cx = 0, cy = 0, r = 0;
        terrainChunkSphere(head + kTerrainHeaderBytes + i * kChunkBytes, unit, cx, cy, r);
        need[i] = texWindowWant(cx, cy, r, px, py, rule) > 0 ? 1 : 0;
        any = any || need[i];
        d2[i] = (cx - px) * (cx - px) + (cy - py) * (cy - py);
    }
    if (!any) return;
    std::vector<TexRun> runs;
    texWindowRuns(need, d2, runs);
    texSortRuns(runs);
    for (const TexRun& r : runs) {
        TexRequest q;
        q.tileX = tileX;
        q.tileY = tileY;
        q.first = r.first;
        q.count = r.count;
        q.offset = entryOffset + layout.texBase + r.first * layout.texBytes;
        q.size = r.count * layout.texBytes;
        out.push_back(q);
    }
}

uint32_t TerrainRenderer::texSyncLoad(pp::Pak& pak, float px, float py) {
    std::vector<TexRequest> reqs;
    texStep(px, py, 0xFFFFFFFFu, reqs);
    uint32_t installed = 0;
    void* buf = nullptr;
    uint32_t bufSize = 0;
    uint8_t* const run = poolRunAlloc();  // A pool row buffer (16 chunks), no heap block
    if (run) {
        buf = run;
        bufSize = g_pool.layout().runSlotBytes;
    }
    for (const TexRequest& q : reqs) {
        char name[32];
        std::snprintf(name, sizeof(name), "t_%d_%d", q.tileX, q.tileY);
        const pp::PakEntry* e = pak.find(name);
        const Tile* t = findTile(q.tileX, q.tileY);
        if (q.size > bufSize) {
            if (buf != run) std::free(buf);
            buf = std::malloc(q.size);
            bufSize = buf ? q.size : 0u;
        }
        // The run straight from the card (the head was taken from the parked blobs: readPart reads the file).
        const bool ok = e && t && buf && q.offset >= e->offset &&
                        pak.readPart(e, q.offset - e->offset, buf, q.size);
        if (ok) installed += texInstall(q.tileX, q.tileY, q.first, q.count, buf, q.size);
        else texCancel(q.tileX, q.tileY, q.first, q.count);
    }
    if (buf != run) std::free(buf);
    poolRunFree(run);
    return installed;
}

TerrainRenderer::TexStats TerrainRenderer::texStats() const {
    TexStats s;
    for (const Tile* t : tiles_) {
        if (!t->windowed) continue;
        ++s.tiles;
        for (const Chunk& ch : t->chunks) {
            ++s.chunks;
            if (ch.texState == 2) {
                ++s.resident;
                s.residentBytes += t->texBytes;
            } else if (ch.texState == 1) {
                ++s.requested;
            }
        }
    }
    s.installs = texInstalls_;
    s.frees = texFrees_;
    s.nomem = texNomem_;
    return s;
}

// heightAt/chunkAt scanned every chunk of every
// loaded tile with two FPU divides each (9-16 tiles x 256 chunks, ~0.1-0.7 ms
// a call on the PSP's 28-cycle div.s), and the world asks 30-50 times a frame
// (feet, camera march, camera floor, target circle, moving NPCs). The scan is
// kept (cpufast=0, irregular tiles, non-finite input); the lookup returns the
// same chunk with the same fr/fc: see Tile::lookup.
int TerrainRenderer::scanChunk(const Tile& t, float x, float y, float& fr, float& fc) {
    const float u = t.unit;
    for (int i = 0; i < 256; ++i) {
        const Chunk& ch = t.chunks[i];
        fr = (ch.baseX - x) / u;
        fc = (ch.baseY - y) / u;
        if (fr < 0 || fc < 0 || fr > 8 || fc > 8) continue;
        g_cpuStats.heightTests += static_cast<uint32_t>(i + 1);
        return i;
    }
    g_cpuStats.heightTests += 256u;
    return -1;
}

namespace {
bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
} // namespace

void TerrainRenderer::buildLookup(const Tile& t) {
    t.lookup = -1;  // scan unless the tile proves regular
    const float u = t.unit;
    if (!(u > 0.0f) || !std::isfinite(u)) return;
    for (int r = 0; r < 16; ++r) t.rowX[r] = t.chunks[r * 16].baseX;
    for (int c = 0; c < 16; ++c) t.colY[c] = t.chunks[c].baseY;
    float minX = t.rowX[0], maxX = t.rowX[0], minY = t.colY[0], maxY = t.colY[0];
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 16; ++c) {
            const Chunk& ch = t.chunks[r * 16 + c];
            if (!std::isfinite(ch.baseX) || !std::isfinite(ch.baseY) || !sameBits(ch.baseX, t.rowX[r]) ||
                !sameBits(ch.baseY, t.colY[c]))
                return;
        }
        minX = std::min(minX, t.rowX[r]);
        maxX = std::max(maxX, t.rowX[r]);
    }
    for (int c = 0; c < 16; ++c) {
        minY = std::min(minY, t.colY[c]);
        maxY = std::max(maxY, t.colY[c]);
    }
    // A chunk can only pass with base - x in [-slop, 8u + slop] (fr = (base -
    // x) / u in [0, 8], -0 included); one more yard covers the float rounding
    // of base - x at world coordinates (|x| < 2^15: ulp <= 0.004).
    const float slop = u * 0.0625f, reach = u * 8.0f + slop;
    t.loX = minX - reach - 1.0f;
    t.hiX = maxX + slop + 1.0f;
    t.loY = minY - reach - 1.0f;
    t.hiY = maxY + slop + 1.0f;
    t.lookup = 1;
}

int TerrainRenderer::findChunk(const Tile& t, float x, float y, float& fr, float& fc) const {
    if (!g_cpuFast || !std::isfinite(x) || !std::isfinite(y)) return scanChunk(t, x, y, fr, fc);
    if (t.lookup == 0) buildLookup(t);
    if (t.lookup < 0) return scanChunk(t, x, y, fr, fc);
    if (x < t.loX || x > t.hiX || y < t.loY || y > t.hiY) return -1;
    // Separable on a regular tile: chunk (r, c) passes iff row r's fr and
    // column c's fc are in [0, 8]; the scan's first passing index r * 16 + c
    // is the first passing row with the first passing column. Same divides
    // as the scan on the same operands, so the same fr/fc bits.
    const float u = t.unit, slop = u * 0.0625f, reach = u * 8.0f + slop;
    int row = -1, col = -1;
    for (int r = 0; r < 16 && row < 0; ++r) {
        const float d = t.rowX[r] - x;
        if (d < -slop || d > reach) continue;
        ++g_cpuStats.heightTests;
        const float f = d / u;
        if (f < 0 || f > 8) continue;
        row = r;
        fr = f;
    }
    if (row < 0) return -1;
    for (int c = 0; c < 16 && col < 0; ++c) {
        const float d = t.colY[c] - y;
        if (d < -slop || d > reach) continue;
        ++g_cpuStats.heightTests;
        const float f = d / u;
        if (f < 0 || f > 8) continue;
        col = c;
        fc = f;
    }
    return col < 0 ? -1 : row * 16 + col;
}

bool TerrainRenderer::heightAt(float x, float y, float& z) const {
    ++g_cpuStats.heightCalls;
    for (const Tile* t : tiles_) {
        float fr = 0, fc = 0;
        const int i = findChunk(*t, x, y, fr, fc);  // was the scan inline
        if (i < 0) continue;
        const Chunk& ch = t->chunks[i];
        const int r = fr >= 8 ? 7 : static_cast<int>(fr);
        const int c = fc >= 8 ? 7 : static_cast<int>(fc);
        const float ar = fr - r, ac = fc - c;
        auto hz = [&](int rr, int cc) { return ch.verts[rr * kGrid + cc].z / 32.0f; };
        const float h = (hz(r, c) * (1 - ac) + hz(r, c + 1) * ac) * (1 - ar) +
                        (hz(r + 1, c) * (1 - ac) + hz(r + 1, c + 1) * ac) * ar;
        z = ch.baseZ + h;
        return true;
    }
    return false;
}

bool TerrainRenderer::chunkAt(float x, float y, int& tileX, int& tileY, int& chunk) const {
    ++g_cpuStats.heightCalls;
    for (const Tile* t : tiles_) {
        float fr = 0, fc = 0;
        const int i = findChunk(*t, x, y, fr, fc);  // was the scan inline
        if (i < 0) continue;
        tileX = t->tileX;
        tileY = t->tileY;
        chunk = i;
        return true;
    }
    return false;
}

void TerrainRenderer::draw(const TerrainCamera& cam, uint32_t fogColorABGR) {
    PROF_SCOPE("terrain");
    PROF_GE_PASS("terrain");
    stats_ = TerrainStats{};
    stats_.tiles = static_cast<uint32_t>(tiles_.size());

    // Camera-relative: the view holds rotation only, every chunk is
    // translated by (base - camera) on the CPU.
    const float aspect = static_cast<float>(pp::kScreenW) / pp::kScreenH;
    const glm::mat4 proj = glm::perspective(glm::radians(cam.fovyDeg), aspect, cam.nearZ, cam.farZ);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), cam.forward, cam.up);
    const Frustum fr = makeFrustum(proj * view);

    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(cam.fovyDeg, aspect, cam.nearZ, cam.farZ);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    ScePspFVector3 eye = {0, 0, 0};
    ScePspFVector3 center = {cam.forward.x, cam.forward.y, cam.forward.z};
    ScePspFVector3 up = {cam.up.x, cam.up.y, cam.up.z};
    sceGumLookAt(&eye, &center, &up);
    sceGumUpdateMatrix();  // projection + view now; chunks set GU_MODEL directly
    if (debugMode == 2) return;  // debug: no terrain (profiler A/B)

    sceGuEnable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR_MIPMAP_NEAREST, GU_LINEAR);  // v3 tiles have mips (level 0 only otherwise)
    // Mip level by distance, not by screen compression: GU_TEXTURE_AUTO picked
    // small mips right under the feet when looking along the ground (PSP build
    // 34). SLOPE: level = log2(slope * depth). Chunk texture = 64 texels per
    // 33.3 yd (1.92/yd); a yard covers ~235.6 / depth px (272 px, fovy 60), so
    // one texel per pixel at depth 1 / (1.92 / 235.6) -> slope 0.00815: level 0
    // to ~120 yd, 1 to ~245, then 2.
    // Default (owner): GU_TEXTURE_AUTO - softer at grazing angles
    // but ~1 ms of GE cheaper; sharpFloor switches to SLOPE.
    // sharpDistance moves where level 0 ends (settings menu; 120 yd = the original rule).
    if (sharpFloor) {
        sceGuTexLevelMode(GU_TEXTURE_SLOPE, 0.0f);
        sceGuTexSlope(0.00815f * 120.0f / std::max(sharpDistance, 10.0f));
    } else {
        sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
    }
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuClutMode(GU_PSM_5650, 0, 0xFF, 0);
    bool texOn = false;
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    lightBegin(cam, fogColorABGR);  // Lighting (fog from the bands, light tint)
    near_.clear();

    for (const Tile* t : tiles_) {
        const float u = t->unit;
        const float half = u * 4.0f;
        for (const Chunk& ch : t->chunks) {
            const glm::vec3 center(ch.baseX - half - cam.pos.x, ch.baseY - half - cam.pos.y,
                                   (ch.minZ + ch.maxZ) * 0.5f - cam.pos.z);
            const float radius = half * 1.4143f + (ch.maxZ - ch.minZ) * 0.5f;
            if (fr.sphereOutside(center, radius)) {
                ++stats_.chunksCulled;
                continue;
            }
            const float dist = glm::length(center);
            // Hardware: ~2.3k triangles cost ~70 us of GE wait at 60 fps,
            // so full detail reaches much further; the old 70/150/280 made the
            // relief visibly change as the camera zoomed out.
            const int lod = dist < 200.0f ? 0 : dist < 340.0f ? 1 : 2;

            // Model matrix sent directly (no gum stack work per chunk):
            // s16 positions arrive as value/32768, grid step 1024 -> unit yd,
            // heights in 1/32 yd.
            ScePspFMatrix4 m;
            m.x = {u * 32.0f, 0.0f, 0.0f, 0.0f};
            m.y = {0.0f, u * 32.0f, 0.0f, 0.0f};
            m.z = {0.0f, 0.0f, 1024.0f, 0.0f};
            m.w = {ch.baseX - cam.pos.x, ch.baseY - cam.pos.y, ch.baseZ - cam.pos.z, 1.0f};
            sceGuSetMatrix(GU_MODEL, &m);
            if (ch.texels && debugMode != 1) {
                if (!texOn) {
                    sceGuEnable(GU_TEXTURE_2D);
                    texOn = true;
                }
                sceGuClutLoad(256 / 8, ch.clut);
                if (t->hasMips) {
                    sceGuTexMode(GU_PSM_T8, 2, 0, 1);
                    sceGuTexImage(0, 64, 64, 64, ch.texels);
                    sceGuTexImage(1, 32, 32, 32, ch.texels + 4096);
                    sceGuTexImage(2, 16, 16, 16, ch.texels + 5120);
                } else {
                    sceGuTexMode(GU_PSM_T8, 0, 0, 1);
                    sceGuTexImage(0, 64, 64, 64, ch.texels);
                }
                stats_.textureKb += 4;
            } else if (texOn) {
                sceGuDisable(GU_TEXTURE_2D);
                texOn = false;
            }
            prof::count(prof::kDraws, 1);
            prof::count(prof::kVertices, static_cast<uint32_t>(lodCounts_[lod]));
            prof::count(prof::kTriangles, static_cast<uint32_t>(lodTriangles_[lod]));
            sceGuDrawArray(GU_TRIANGLE_STRIP,
                           GU_INDEX_16BIT | GU_TEXTURE_8BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D,
                           lodCounts_[lod], lodIndices_[lod], ch.verts);
            ++stats_.chunksDrawn;
            stats_.triangles += static_cast<uint32_t>(lodTriangles_[lod]);
            if (detail && lod == 0 && ch.detail != 0xFFFF && dist - radius < detailDistance) near_.push_back(&ch);
        }
    }
    lightEnd();  // Lighting

    // Detail pass (v4): the same strips again for the near chunks, the
    // dominant layer's luminance detail repeated 4x per chunk, blended as
    // dst * src * 2 (0.5 grey = unchanged). Replace, not modulate: the vertex
    // light is already in the frame buffer. Depth test only (same vertices,
    // GEQUAL passes), no depth writes, no fog (it would tint the multiplier).
    if (!near_.empty() && debugMode == 0) {
        PROF_GE_PASS("terrain.detail");
        PROF_SCOPE("terrain.detail");
        pp::textureResetBinding();
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0);
        sceGuDepthMask(GU_TRUE);
        sceGuTexScale(4.0f, 4.0f);
        for (const Chunk* ch : near_) {
            const Tile* t = nullptr;
            for (const Tile* tt : tiles_)
                if (ch >= tt->chunks && ch < tt->chunks + 256) t = tt;
            const pp::Texture* tex = ch->detail < details_.size() && details_[ch->detail].pixels ? &details_[ch->detail] : nullptr;
            if (!t || !tex) continue;
            pp::textureBind(tex);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            // Detail: 128 texels x 4 repeats per 33.3 yd = 15.4/yd -> slope
            // 0.0652: full detail to ~15 yd, then 64/32, flat grey from ~60 yd.
            sceGuTexLevelMode(GU_TEXTURE_SLOPE, 0.0f);
            sceGuTexSlope(0.0652f);
            sceGuTexWrap(GU_REPEAT, GU_REPEAT);
            const float u = t->unit;
            ScePspFMatrix4 m;
            m.x = {u * 32.0f, 0.0f, 0.0f, 0.0f};
            m.y = {0.0f, u * 32.0f, 0.0f, 0.0f};
            m.z = {0.0f, 0.0f, 1024.0f, 0.0f};
            m.w = {ch->baseX - cam.pos.x, ch->baseY - cam.pos.y, ch->baseZ - cam.pos.z, 1.0f};
            sceGuSetMatrix(GU_MODEL, &m);
            prof::count(prof::kDraws, 1);
            prof::count(prof::kVertices, static_cast<uint32_t>(lodCounts_[0]));
            sceGuDrawArray(GU_TRIANGLE_STRIP,
                           GU_INDEX_16BIT | GU_TEXTURE_8BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D,
                           lodCounts_[0], lodIndices_[0], ch->verts);
        }
        stats_.detailChunks = static_cast<uint32_t>(near_.size());
        sceGuDepthMask(GU_FALSE);
        sceGuDisable(GU_BLEND);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
        pp::textureResetBinding();
    }
    sceGuDisable(GU_TEXTURE_2D);
}

} // namespace wowee::rendering::psp
