// data/audio.pak (tools/psp/converter psp_audio_conv): what upstream's
// game::ZoneManager knows after initialize() + enrichFromDBC(), written out
// on the PC so the PSP needs neither the DBCs nor the hand-written tables.
//   "zones"      AudioZonesHeader + sections below;
//   "a_<x>_<y>"  uint16[256]: MCNK areaId of every chunk of ADT tile x_y,
//                chunk index = MapChunk order of pipeline::ADTTerrain.
// Tracks are card paths relative to data/music/ (lowercase, '/').
#pragma once

#include <cstdint>

namespace wowee::rendering::psp {

struct AudioZonesHeader {
    char magic[4];            // "WZMU"
    uint32_t version;         // 1
    uint32_t trackCount, zoneCount, zoneTrackCount, areaCount, tileCount;
    uint32_t trackNameOffset; // uint32[trackCount]: offsets into the names block
    uint32_t namesOffset;     // '\0'-terminated paths
    uint32_t zoneOffset;      // AudioZone[zoneCount], by zoneId ascending order of AreaTable
    uint32_t zoneTrackOffset; // uint16[zoneTrackCount]: track indices
    uint32_t areaOffset;      // AudioArea[areaCount]
    uint32_t tileOffset;      // AudioTile[tileCount]
};

struct AudioZone {
    uint32_t zoneId;
    uint16_t firstTrack;      // into the zone-track list
    uint16_t trackCount;      // ZoneInfo::musicPaths, in upstream's order
};

struct AudioArea {
    uint32_t areaId;
    uint32_t parent;          // AreaTable field 2 (ZoneManager::areaParents_)
};

struct AudioTile {
    uint32_t key;             // tileX * 100 + tileY (ZoneManager::tileToZone)
    uint32_t zoneId;
};

} // namespace wowee::rendering::psp
