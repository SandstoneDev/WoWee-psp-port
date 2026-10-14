// Map-level records of the PSP paks.
//
//  - "wmo_only" (kind 3) in terrain_<dir>.pak and doodads_<dir>.pak of a map whose WDT names a global WMO
//    (MPHD flag 0x1, upstream pipeline::WDTInfo::isWMOOnly - dungeons such as map 389). The
//    terrain pak then holds only this record (no "t_" tiles: the map has no ADTs), the doodads pak the WMO's
//    groups and its doodad set placed by upstream WorldLoader::loadMapGeometry's rules and spread over virtual
//    tiles "d_<x>_<y>" by where each part stands - the PSP's tile loader and streamer take them unchanged.
//    Written by tools/psp/converter/psp_terrain_conv + psp_model_conv, read by WorldScreen::loadTerrain.
//  - glue.pak "mapdirs" (tools/psp/build_glue_pak.py): Map.dbc's Directory (the paks' <dir>), the ruRU
//    MapName_lang, InstanceType, the expansion and the WDT's global-WMO flag of every map
//    (WorldSession::mapInfo).
//  - glue.pak "at_<mapId>": the AreaTrigger.dbc rows of one map (WorldSession::updateAreaTriggers).
#pragma once

#include <cstdint>

namespace wowee::rendering::psp {

constexpr uint16_t kWmoOnlyVersion = 1;

struct WmoOnlyHeader {
    char magic[4];           // "WWMO"
    uint16_t version;        // kWmoOnlyVersion
    uint16_t tileCount;      // virtual tiles "d_<x>_<y>" in the doodads pak (0 in the terrain pak's copy)
    uint32_t mphdFlags;      // WDT MPHD flags
    float origin[3];         // the WMO origin's place, server coordinates (0 for a zero MODF position)
    float rotation[3];       // MODF rotation, degrees, as the WDT has it
    float bmin[3], bmax[3];  // world box of the baked groups, server coordinates (all 0 in the terrain pak)
    uint16_t doodadSet;      // MODF doodad set (upstream loads that one set only)
    uint16_t groups;         // WMO groups baked (0 in the terrain pak)
    char wmo[80];            // root WMO path, lowercase
};
static_assert(sizeof(WmoOnlyHeader) == 144, "wmo_only record is 144 bytes");

// glue.pak "mapdirs": u32 count; count x MapDirRecord (ascending mapId); then the string pool (NUL-terminated:
// the directory in lower case, the ruRU map name in UTF-8). Offsets are from the start of the pool.
struct MapDirRecord {
    uint32_t mapId;
    uint8_t instanceType;  // Map.dbc InstanceType: 0 world, 1 party, 2 raid, 3 battleground, 4 arena
    uint8_t flags;         // kMapDir* below
    uint8_t expansion;     // Map.dbc ExpansionID
    uint8_t maxPlayers;    // Map.dbc MaxPlayers (0 = none)
    uint16_t dirOffset;
    uint16_t nameOffset;
};
static_assert(sizeof(MapDirRecord) == 12, "mapdirs record is 12 bytes");

enum MapDirFlags : uint8_t {
    kMapDirGlobalWmo = 1u << 0,  // the WDT names a global WMO (MPHD 0x1): no terrain tiles
    kMapDirHasTiles = 1u << 1,   // the WDT's MAIN chunk lists ADT tiles
};

// glue.pak "at_<mapId>": n x AreaTriggerRecord - AreaTrigger.dbc as upstream MovementHandler::loadAreaTriggerDbc
// reads it: server coordinates (x = DBC field 2 = the wire's X, y = field 3), radius > 0 = a sphere, else a box
// (length along x, width along y, height along z) turned by boxYaw radians.
struct AreaTriggerRecord {
    uint32_t id;
    uint32_t mapId;
    float x, y, z;
    float radius;
    float boxLength, boxWidth, boxHeight, boxYaw;
};
static_assert(sizeof(AreaTriggerRecord) == 40, "area trigger record is 40 bytes");

}  // namespace wowee::rendering::psp
