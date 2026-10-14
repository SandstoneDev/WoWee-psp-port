// The particle systems of the world's models, offline format shared by
// tools/psp/converter/particle_bake.hpp (psp_model_conv, PC) and src/rendering/psp/psp_particles.cpp
// (PSP). Plain little-endian PODs, no platform headers.
//
// One pak entry "pidx", the FIRST record of the pak's TOC, in data/doodads_<map>.pak (the doodads and
// WMO doodads of the ADT tiles) and in data/gameobjects.pak (GameObjectDisplayInfo displays). Its first
// record lets the PSP read it without the pak's TOC (the doodads TOC of a whole continent is ~230 KB):
// pak header (16 B) -> TOC record 0 (64 B) -> this entry. Everything inside is addressed from the
// start of the entry, so the entry is self-contained and every other pak entry is unchanged
// (an older EBOOT never looks at "pidx"; a pak without it simply has no particles).
//
// Layout (offsets from the entry start, every section 4-aligned, the blobs 64-aligned):
//   ParticleIndexHeader
//   ParticleTileRec[tileCount]        doodads paks: the tiles that place emitters, sorted by (x, y)
//   ParticleDisplayRec[displayCount]  gameobjects.pak: displayId -> effect, sorted by displayId
//   ParticleEffectRec[effectCount]    effect n: a WSFX v2 blob (include/rendering/psp/psp_spellfx_format.hpp)
//   ParticleTextureRec[textureCount]  texture n: a WTEX blob (T8 + CLUT, the effects' "x_<n>" ids)
//   -- directoryBytes: what the PSP reads (and keeps) at open --
//   the blobs: placement lists (ParticlePlacement[count] per tile), effects, textures.
//
// An effect is one M2's particle emitters (the most populous kFxMaxEmitters), its ribbon emitters and,
// when upstream's classifier calls the model a spell effect (its mesh is then not in the WMDL "m_<n>"),
// its batches as WSFX groups; its ParticleClass picks upstream's per-model particle rules.
#pragma once

#include <cstdint>

namespace wowee::rendering::psp {

constexpr uint16_t kParticleIndexVersion = 1;

struct ParticleIndexHeader {
    char magic[4];             // "WPFX"
    uint16_t version;          // kParticleIndexVersion
    uint16_t flags;            // ParticleIndexFlags (0 in older paks)
    uint32_t tileCount, tileOffset;
    uint32_t displayCount, displayOffset;
    uint32_t effectCount, effectOffset;
    uint32_t textureCount, textureOffset;
    uint32_t directoryBytes;   // header + the four tables
    uint32_t size;             // the whole entry
    uint32_t placementCount;   // all tiles (stats)
    uint32_t reserved;
};
static_assert(sizeof(ParticleIndexHeader) == 56, "WPFX header is 56 bytes");

struct ParticleTileRec {
    int16_t x, y;              // ADT tile (the "d_<x>_<y>" of the same pak)
    uint32_t offset;           // ParticlePlacement[count]
    uint32_t count;
};
static_assert(sizeof(ParticleTileRec) == 12, "WPFX tile is 12 bytes");

struct ParticleDisplayRec {
    uint32_t displayId;        // GameObjectDisplayInfo ID
    uint16_t effect;
    uint16_t reserved;
};
static_assert(sizeof(ParticleDisplayRec) == 8, "WPFX display is 8 bytes");

enum ParticleEffectFlags : uint8_t {
    kPtcEffectMesh = 1u << 0,  // has WSFX groups (a spell-effect doodad's own mesh)
    kPtcEffectLevel3 = 1u << 1,  // its blob carries the WSFX level-3 section
};

// ParticleIndexHeader::flags (0 in older paks; older EBOOTs ignore it).
enum ParticleIndexFlags : uint16_t {
    kPtcIndexLevel3 = 1u << 0,  // the effects were baked with the level-3 section
};

struct ParticleEffectRec {
    uint32_t offset, size;     // the WSFX v2 blob
    float reach;               // = its SpellFxHeaderExt::reach (model space), readable without the blob
    uint8_t cls;               // = its SpellFxHeaderExt::cls
    uint8_t flags;             // ParticleEffectFlags
    uint8_t emitters, ribbons; // stats
};
static_assert(sizeof(ParticleEffectRec) == 16, "WPFX effect is 16 bytes");

struct ParticleTextureRec {
    uint32_t offset, size;     // the WTEX blob
};
static_assert(sizeof(ParticleTextureRec) == 8, "WPFX texture is 8 bytes");

// One placed emitter model: the doodad placement (upstream terrain_manager placementEuler / the WMO
// doodad's T * R * S, as WDOD's DoodadPlacement), the same uniqueId as its "d_" record (a doodad on a
// tile border is listed by both tiles; the PSP keeps the first).
struct ParticlePlacement {
    uint32_t uniqueId;
    uint16_t effect;
    uint16_t flags;            // 0
    float position[3];         // server coordinates
    float axes[9];             // column-major 3x3: rotation * scale
};
static_assert(sizeof(ParticlePlacement) == 56, "WPFX placement is 56 bytes");

// The directory (header + tables, `bytes` of it read) of an entry of `entrySize` bytes: every table in
// the directory, every record's data inside the entry. Shared by the converter's --verify and the PSP.
inline bool particleIndexValid(const void* dir, uint32_t bytes, uint32_t entrySize) {
    if (!dir || bytes < sizeof(ParticleIndexHeader)) return false;
    const auto* h = static_cast<const ParticleIndexHeader*>(dir);
    if (h->magic[0] != 'W' || h->magic[1] != 'P' || h->magic[2] != 'F' || h->magic[3] != 'X') return false;
    if (h->version != kParticleIndexVersion || h->size != entrySize || h->directoryBytes > bytes ||
        h->directoryBytes > entrySize)
        return false;
    auto table = [&](uint32_t off, uint32_t n, uint32_t each) {
        return !(off & 3u) && off <= h->directoryBytes && (n == 0 || (h->directoryBytes - off) / each >= n);
    };
    if (!table(h->tileOffset, h->tileCount, sizeof(ParticleTileRec)) ||
        !table(h->displayOffset, h->displayCount, sizeof(ParticleDisplayRec)) ||
        !table(h->effectOffset, h->effectCount, sizeof(ParticleEffectRec)) ||
        !table(h->textureOffset, h->textureCount, sizeof(ParticleTextureRec)))
        return false;
    if (h->effectCount > 0xFFFE || h->textureCount > 0xFFFE) return false;
    auto data = [&](uint32_t off, uint32_t size) {
        return off >= h->directoryBytes && off <= entrySize && size <= entrySize - off;
    };
    const auto* b = static_cast<const uint8_t*>(dir);
    const auto* tiles = reinterpret_cast<const ParticleTileRec*>(b + h->tileOffset);
    for (uint32_t i = 0; i < h->tileCount; ++i) {
        if (tiles[i].count > (1u << 20) || !data(tiles[i].offset, tiles[i].count * static_cast<uint32_t>(sizeof(ParticlePlacement))))
            return false;
        if (i && (tiles[i - 1].x > tiles[i].x || (tiles[i - 1].x == tiles[i].x && tiles[i - 1].y >= tiles[i].y)))
            return false;  // sorted, unique
    }
    const auto* disp = reinterpret_cast<const ParticleDisplayRec*>(b + h->displayOffset);
    for (uint32_t i = 0; i < h->displayCount; ++i) {
        if (disp[i].effect >= h->effectCount) return false;
        if (i && disp[i - 1].displayId >= disp[i].displayId) return false;
    }
    const auto* fx = reinterpret_cast<const ParticleEffectRec*>(b + h->effectOffset);
    for (uint32_t i = 0; i < h->effectCount; ++i) {
        uint32_t u;
        __builtin_memcpy(&u, &fx[i].reach, sizeof(u));
        if (!data(fx[i].offset, fx[i].size) || fx[i].size < 104 || (u & 0x7F800000u) == 0x7F800000u) return false;
    }
    const auto* tex = reinterpret_cast<const ParticleTextureRec*>(b + h->textureOffset);
    for (uint32_t i = 0; i < h->textureCount; ++i)
        if (!data(tex[i].offset, tex[i].size) || tex[i].size < 32) return false;
    return true;
}

} // namespace wowee::rendering::psp
