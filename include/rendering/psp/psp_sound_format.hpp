// Sounds. data/sounds.pak (tools/psp/converter psp_sound_conv):
// the sound effects upstream's UiSoundManager, FootstepManager,
// CombatSoundManager, ActivitySoundManager and MovementSoundManager load from
// the MPQs, plus the SoundEntries.dbc rows whose files were baked (FrameXML's
// PlaySound names, playSound3D by id), plus the footstep surface of every
// terrain cell (upstream FootstepDriver::resolveFootstepSurface).
//
// Entries (WPAK, include/platform/psp/psp_pak.hpp):
//   "s_<hash>"        SoundClipHeader + s16 PCM (mono, 22050 Hz by default).
//                     <hash> = soundPathHash(game path), 8 lowercase hex
//                     digits: a WPAK name holds 47 characters, a game path up
//                     to ~110. The converter refuses colliding paths.
//   "entries"         SoundEntriesHeader + SoundEntryRow[rowCount] (DBC order)
//                     + uint32 fileHash[fileCount] (the rows' baked files).
//   "g_<map>_<x>_<y>" SurfaceGridHeader + nibbles of ADT tile x_y of map id
//                     <map>: 256 chunks x 8 x 8 cells (one cell = one terrain
//                     unit, 4.17 yd) + float water[256].
// Plain little-endian PODs, no platform headers: the converter and the PSP
// both include this file.
#pragma once

#include <cstdint>

namespace wowee::rendering::psp {

constexpr uint16_t kSoundFormatVersion = 1;

// FNV-1a over the path lowercased with '/' for '\\' (the extracted tree's
// spelling, conv_common.hpp lower()). Same function on both sides.
inline uint32_t soundPathHash(const char* path) {
    uint32_t h = 2166136261u;
    for (const char* p = path; p && *p; ++p) {
        char c = *p;
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    return h;
}

// SoundEntries Name (FrameXML PlaySound("igQuestFailed")): case-insensitive,
// like upstream UiSoundManager (upper-cased keys) - the same hash as a path.
inline uint32_t soundNameHash(const char* name) { return soundPathHash(name); }

struct SoundClipHeader {
    char magic[4];          // "WSND"
    uint16_t version;       // kSoundFormatVersion
    uint8_t channels;       // 1 (2 only with --stereo)
    uint8_t reserved;
    uint32_t rate;          // Hz
    uint32_t frames;        // s16 frames after the header
};
static_assert(sizeof(SoundClipHeader) == 16, "WSND header is 16 bytes");

struct SoundEntriesHeader {
    char magic[4];          // "WSEN"
    uint16_t version;
    uint16_t reserved;
    uint32_t rowCount;
    uint32_t fileCount;
    uint32_t rowOffset;     // SoundEntryRow[rowCount]
    uint32_t fileOffset;    // uint32 soundPathHash[fileCount]
};
static_assert(sizeof(SoundEntriesHeader) == 24, "WSEN header is 24 bytes");

// One SoundEntries.dbc row (3.3.5a: 0 ID, 1 SoundType, 2 Name, 3..12 File,
// 13..22 Freq, 23 DirectoryBase, 24 Volume, 25 Flags, 26 MinDistance,
// 27 DistanceCutoff), only its files that are in the pak, in DBC order.
struct SoundEntryRow {
    uint32_t id;
    uint32_t nameHash;      // soundNameHash(Name)
    uint16_t firstFile;     // into the file hash list
    uint8_t fileCount;
    uint8_t soundType;
    float volume;           // DBC Volume (0..1)
    float minDistance;      // DBC MinDistance (yd)
    float distanceCutoff;   // DBC DistanceCutoff (yd)
    uint32_t flags;
};
static_assert(sizeof(SoundEntryRow) == 28, "WSEN row is 28 bytes");

// upstream audio::FootstepSurface order (footstep_manager.hpp).
enum SoundSurface : uint8_t {
    kSurfStone = 0,
    kSurfDirt,
    kSurfGrass,
    kSurfWood,
    kSurfMetal,
    kSurfWater,
    kSurfSnow,
    kSurfCount
};
constexpr uint8_t kSurfCellWater = 0x8;  // nibble bit: an MH2O layer covers the cell

struct SurfaceGridHeader {
    char magic[4];          // "WSRF"
    uint16_t version;
    uint16_t reserved;
    // uint8_t cells[256 * 32]: chunk c (pipeline::ADTTerrain order), cell
    //   (row, col) = (fracY, fracX) of TerrainMeshGenerator::chunkFractionsAt
    //   floored, index n = c * 64 + row * 8 + col, byte n / 2, low nibble for
    //   even n: SoundSurface of the dominant layer at the cell centre
    //   (upstream TerrainManager::getDominantTextureAt + the token rules of
    //   FootstepDriver) | kSurfCellWater.
    // float water[256]: the highest MH2O maxHeight of the chunk (0 if none).
};
static_assert(sizeof(SurfaceGridHeader) == 8, "WSRF header is 8 bytes");
constexpr uint32_t kSurfaceGridBytes = sizeof(SurfaceGridHeader) + 256 * 32 + 256 * 4;

// Server position (x north, y west) -> ADT file tile, chunk and cell of the
// grid: upstream TerrainManager::findChunkAt (guessCy/guessCx from the tile's
// max corner) + TerrainMeshGenerator::chunkFractionsAt (fracY = (chunk X -
// x) / unit, fracX = (chunk Y - y) / unit); floats only (PSP per frame).
struct SurfaceCell {
    int fileX, fileY;  // "<map>_<fileX>_<fileY>.adt"
    int chunk;         // cy * 16 + cx (pipeline::ADTTerrain order)
    int cell;          // row * 8 + col
};
inline SurfaceCell surfaceCellAt(float x, float y) {
    constexpr float kTile = 533.33333f, kChunk = kTile / 16.0f, kUnit = kChunk / 8.0f;
    auto floorI = [](float v) {
        const int i = static_cast<int>(v);
        return (static_cast<float>(i) > v) ? i - 1 : i;
    };
    auto clampI = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    SurfaceCell c{};
    c.fileX = floorI(32.0f - y / kTile);
    c.fileY = floorI(32.0f - x / kTile);
    const float maxX = (32.0f - static_cast<float>(c.fileY)) * kTile;
    const float maxY = (32.0f - static_cast<float>(c.fileX)) * kTile;
    const int cy = clampI(floorI((maxX - x) / kChunk), 15), cx = clampI(floorI((maxY - y) / kChunk), 15);
    const int row = clampI(floorI((maxX - static_cast<float>(cy) * kChunk - x) / kUnit), 7);
    const int col = clampI(floorI((maxY - static_cast<float>(cx) * kChunk - y) / kUnit), 7);
    c.chunk = cy * 16 + cx;
    c.cell = row * 8 + col;
    return c;
}

} // namespace wowee::rendering::psp
