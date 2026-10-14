// psp_transport_format.hpp - records of data/transports.pak and data/gameobjects_wmo.pak.
//
// Clean-room implementation of specs/world/transport_format.md (v2). Header only, <cstdint>, plain little-endian
// PODs with asserted sizes. The movement rules are not part of this header.
//
// Entries:
//   gw_<displayId>  TransportWmoHeader, TransportPart[partCount] (the WMO's groups first, then the doodads of its
//                   default doodad set; model space = the WMO's own)
//   ta_<entry>      TransportAnimHeader, TransportAnimKey[posCount], TransportRotKey[rotCount]
//   tpaths          TaxiPathSetHeader, TaxiPathRef[pathCount], TaxiPathNodeRec[nodeCount]
// Content rules (transport_format.md 3): keys keep their DBC row order; TaxiPathRef sorted by strictly increasing
// pathId, each covering [first, first + count) of the node records; node flags 0x1 = leaves the map after this
// node, 0x2 = stop (wait delaySec); exact record sizes (52 + parts * 68, 16 + (pos + rot) * 20,
// 16 + paths * 12 + nodes * 32); every record carries version 1, readers reject other versions and magics.
#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {
namespace psp {

constexpr uint16_t kTransportFormatVersion = 1;

struct TransportWmoHeader {
    char magic[4];  // "WTRW"
    uint16_t version;
    uint16_t partCount;
    uint16_t groupCount;  // the first groupCount parts are WMO groups
    uint16_t flags;  // 0
    float bmin[3];  // model-space box of the groups
    float bmax[3];
    float center[3];  // bounding sphere of every part
    float radius;
};

struct TransportPart {
    uint16_t model;  // index n of m_<n>
    uint16_t flags;  // 0
    float position[3];
    float axes[9];  // column-major 3 x 3 = rotation x scale
    float sphere[4];  // model-space bounding sphere: centre xyz, radius
};

struct TransportAnimHeader {
    char magic[4];  // "WTRA"
    uint16_t version;
    uint16_t posCount;
    uint16_t rotCount;
    uint16_t reserved;
    uint32_t entry;  // TransportID = game object entry
};

struct TransportAnimKey {
    uint32_t timeMs;  // TimeIndex
    float pos[3];  // offset from the object's position, before its parent rotation
    uint32_t sequence;  // SequenceID
};

struct TransportRotKey {
    uint32_t timeMs;  // TimeIndex
    float quat[4];  // x, y, z, w
};

struct TaxiPathSetHeader {
    char magic[4];  // "WTPN"
    uint16_t version;
    uint16_t pathCount;
    uint32_t nodeCount;
    uint32_t reserved;
};

struct TaxiPathRef {
    uint32_t pathId;
    uint32_t first;  // index of the path's first node record
    uint32_t count;
};

struct TaxiPathNodeRec {
    float pos[3];  // server coordinates
    uint32_t mapId;  // ContinentID
    uint32_t flags;  // 0x1 leaves the map after this node, 0x2 stop
    uint32_t delaySec;
    uint32_t arrivalEvent;
    uint32_t departureEvent;
};

static_assert(sizeof(TransportWmoHeader) == 52, "TransportWmoHeader size");
static_assert(sizeof(TransportPart) == 68, "TransportPart size");
static_assert(sizeof(TransportAnimHeader) == 16, "TransportAnimHeader size");
static_assert(sizeof(TransportAnimKey) == 20, "TransportAnimKey size");
static_assert(sizeof(TransportRotKey) == 20, "TransportRotKey size");
static_assert(sizeof(TaxiPathSetHeader) == 16, "TaxiPathSetHeader size");
static_assert(sizeof(TaxiPathRef) == 12, "TaxiPathRef size");
static_assert(sizeof(TaxiPathNodeRec) == 32, "TaxiPathNodeRec size");

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
