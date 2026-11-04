// The DBC tables of data/transports.pak
// (include/rendering/psp/psp_transport_format.hpp) - what the client moves its transports by:
//   "ta_<entry>"  TransportAnimation.dbc (ID, TransportID, TimeIndex, Pos xyz, SequenceID) + TransportRotation.dbc
//                 (ID, GameObjectsID, TimeIndex, Rot xyzw) rows of one GAMEOBJECT_TYPE_TRANSPORT entry, DBC order;
//   "tpaths"      TaxiPathNode.dbc (ID, PathID, NodeIndex, ContinentID, Loc xyz, Flags, Delay, ArrivalEventID,
//                 DepartureEventID) of the paths an MO_TRANSPORT can run, DBC order.
// The paths: which one a ship runs is its template's data0, from the server - the DBC alone does not say. Kept are
// the paths that look like a transport's: a stop (flag 0x2) or a map change (flag 0x1 or two continents), an
// arrival / departure event, or a TaxiPath leaving a node with no flight mount (TaxiNodes MountCreatureID 0 / 0:
// the transport nodes). --transport-all-paths keeps every path (the whole DBC, ~700 KB).
#pragma once

#include "conv_common.hpp"
#include "forest_bake.hpp"  // PakView
#include "model_bake.hpp"

#include "pipeline/dbc_loader.hpp"
#include "rendering/psp/psp_transport_format.hpp"

#include <map>
#include <set>

namespace wowee::pspconv {

namespace fmtt = wowee::rendering::psp;

inline bool loadDbc(const std::string& root, const char* name, pipeline::DBCFile& out) {
    std::vector<uint8_t> bytes;
    if (!readFile(root + "/dbfilesclient/" + name, bytes) || !out.load(bytes)) {
        std::fprintf(stderr, "missing dbfilesclient/%s\n", name);
        return false;
    }
    return true;
}

// "ta_<entry>" for every TransportID of TransportAnimation.dbc (rotation keys of the same entry with them).
inline size_t bakeTransportAnimations(const std::string& root, std::vector<PakItem>& out) {
    pipeline::DBCFile anim, rot;
    if (!loadDbc(root, "transportanimation.dbc", anim)) return 0;
    const bool haveRot = loadDbc(root, "transportrotation.dbc", rot);
    std::map<uint32_t, std::vector<fmtt::TransportAnimKey>> keys;
    std::map<uint32_t, std::vector<fmtt::TransportRotKey>> rots;
    for (uint32_t r = 0; r < anim.getRecordCount(); ++r) {
        fmtt::TransportAnimKey k{};
        k.timeMs = anim.getUInt32(r, 2);
        for (int a = 0; a < 3; ++a) k.pos[a] = anim.getFloat(r, 3 + a);
        k.sequence = anim.getUInt32(r, 6);
        keys[anim.getUInt32(r, 1)].push_back(k);
    }
    if (haveRot) {
        for (uint32_t r = 0; r < rot.getRecordCount(); ++r) {
            fmtt::TransportRotKey k{};
            k.timeMs = rot.getUInt32(r, 2);
            for (int a = 0; a < 4; ++a) k.quat[a] = rot.getFloat(r, 3 + a);
            rots[rot.getUInt32(r, 1)].push_back(k);
        }
    }
    size_t n = 0, bytes = 0;
    for (const auto& [entry, list] : keys) {
        const auto& rl = rots[entry];
        fmtt::TransportAnimHeader h{};
        std::memcpy(h.magic, "WTRA", 4);
        h.version = fmtt::kTransportFormatVersion;
        h.posCount = static_cast<uint16_t>(std::min<size_t>(list.size(), 65535));
        h.rotCount = static_cast<uint16_t>(std::min<size_t>(rl.size(), 65535));
        h.entry = entry;
        std::vector<uint8_t> blob(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(list.data()),
                    reinterpret_cast<const uint8_t*>(list.data() + h.posCount));
        blob.insert(blob.end(), reinterpret_cast<const uint8_t*>(rl.data()),
                    reinterpret_cast<const uint8_t*>(rl.data() + h.rotCount));
        bytes += blob.size();
        out.push_back({"ta_" + std::to_string(entry), 3, std::move(blob)});  // kind 3 = raw
        ++n;
    }
    std::printf("transport animations: %zu entries (%u keys, %u rotation keys), %zu KB\n", n,
                anim.getRecordCount(), haveRot ? rot.getRecordCount() : 0u, bytes / 1024);
    return n;
}

// "tpaths": the transport paths of TaxiPathNode.dbc.
inline bool bakeTransportPaths(const std::string& root, bool allPaths, PakItem& out) {
    pipeline::DBCFile tpn, tp, tn;
    if (!loadDbc(root, "taxipathnode.dbc", tpn)) return false;
    std::set<uint32_t> keep;
    std::map<uint32_t, std::set<uint32_t>> maps;
    for (uint32_t r = 0; r < tpn.getRecordCount(); ++r) {
        const uint32_t path = tpn.getUInt32(r, 1);
        maps[path].insert(tpn.getUInt32(r, 3));
        if (allPaths || (tpn.getUInt32(r, 7) & 3u) || tpn.getUInt32(r, 9) || tpn.getUInt32(r, 10)) keep.insert(path);
    }
    for (const auto& [path, m] : maps)
        if (m.size() > 1) keep.insert(path);
    size_t byNode = 0;
    if (!allPaths && loadDbc(root, "taxipath.dbc", tp) && loadDbc(root, "taxinodes.dbc", tn)) {
        std::set<uint32_t> noMount;  // TaxiNodes: ID, map, xyz, Name[17], MountCreatureID[2] (fields 22, 23)
        for (uint32_t r = 0; r < tn.getRecordCount(); ++r)
            if (tn.getFieldCount() >= 24 && tn.getUInt32(r, 22) == 0 && tn.getUInt32(r, 23) == 0)
                noMount.insert(tn.getUInt32(r, 0));
        for (uint32_t r = 0; r < tp.getRecordCount(); ++r)
            if (noMount.count(tp.getUInt32(r, 1)) && keep.insert(tp.getUInt32(r, 0)).second) ++byNode;
    }
    std::vector<fmtt::TaxiPathRef> refs;
    std::vector<fmtt::TaxiPathNodeRec> nodes;
    // DBC order (the client binary-searches the PathID column and walks on from the first row: it is sorted).
    for (uint32_t r = 0; r < tpn.getRecordCount(); ++r) {
        const uint32_t path = tpn.getUInt32(r, 1);
        if (!keep.count(path)) continue;
        if (refs.empty() || refs.back().pathId != path)
            refs.push_back({path, static_cast<uint32_t>(nodes.size()), 0});
        fmtt::TaxiPathNodeRec n{};
        for (int a = 0; a < 3; ++a) n.pos[a] = tpn.getFloat(r, 4 + a);
        n.mapId = tpn.getUInt32(r, 3);
        n.flags = tpn.getUInt32(r, 7);
        n.delaySec = tpn.getUInt32(r, 8);
        n.arrivalEvent = tpn.getUInt32(r, 9);
        n.departureEvent = tpn.getUInt32(r, 10);
        nodes.push_back(n);
        ++refs.back().count;
    }
    std::sort(refs.begin(), refs.end(), [](const auto& a, const auto& b) { return a.pathId < b.pathId; });
    fmtt::TaxiPathSetHeader h{};
    std::memcpy(h.magic, "WTPN", 4);
    h.version = fmtt::kTransportFormatVersion;
    h.pathCount = static_cast<uint16_t>(refs.size());
    h.nodeCount = static_cast<uint32_t>(nodes.size());
    out.name = "tpaths";
    out.kind = 3;
    out.blob.assign(reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h + 1));
    out.blob.insert(out.blob.end(), reinterpret_cast<const uint8_t*>(refs.data()),
                    reinterpret_cast<const uint8_t*>(refs.data() + refs.size()));
    out.blob.insert(out.blob.end(), reinterpret_cast<const uint8_t*>(nodes.data()),
                    reinterpret_cast<const uint8_t*>(nodes.data() + nodes.size()));
    std::printf("transport paths: %zu of %zu paths (%zu by a node without a flight mount), %zu nodes, %zu KB%s\n",
                refs.size(), maps.size(), byNode, nodes.size(), out.blob.size() / 1024, allPaths ? " (all)" : "");
    return !refs.empty();
}

// --verify on a transports pak: every gw_ / ta_ / tpaths record well formed, every part's model in the pak.
inline int verifyTransports(const std::string& path) {
    PakView pak;
    if (!pak.open(path)) return 0;  // verifyPak said it already
    int errors = 0, wmos = 0, anims = 0, parts = 0;
    auto bad = [&](const std::string& name, const char* why) {
        if (++errors <= 40) std::fprintf(stderr, "verify: %s: %s\n", name.c_str(), why);
    };
    for (const auto& [name, loc] : pak.entries) {
        uint32_t size = 0;
        const uint8_t* b = pak.find(name, size);
        if (name.rfind("gw_", 0) == 0) {
            fmtt::TransportWmoHeader h;
            if (size < sizeof(h)) {
                bad(name, "short");
                continue;
            }
            std::memcpy(&h, b, sizeof(h));
            if (std::memcmp(h.magic, "WTRW", 4) != 0 || h.version != fmtt::kTransportFormatVersion ||
                sizeof(h) + h.partCount * sizeof(fmtt::TransportPart) != size || h.groupCount > h.partCount) {
                bad(name, "header");
                continue;
            }
            for (uint32_t i = 0; i < h.partCount; ++i) {
                fmtt::TransportPart p;
                std::memcpy(&p, b + sizeof(h) + i * sizeof(p), sizeof(p));
                if (!pak.entries.count("m_" + std::to_string(p.model))) bad(name, "part model missing");
                if (!(p.sphere[3] >= 0.0f)) bad(name, "part sphere");
            }
            parts += h.partCount;
            ++wmos;
        } else if (name.rfind("ta_", 0) == 0) {
            fmtt::TransportAnimHeader h;
            if (size < sizeof(h)) {
                bad(name, "short");
                continue;
            }
            std::memcpy(&h, b, sizeof(h));
            if (std::memcmp(h.magic, "WTRA", 4) != 0 ||
                sizeof(h) + h.posCount * sizeof(fmtt::TransportAnimKey) + h.rotCount * sizeof(fmtt::TransportRotKey) !=
                    size)
                bad(name, "header");
            ++anims;
        } else if (name == "tpaths") {
            fmtt::TaxiPathSetHeader h;
            if (size < sizeof(h)) {
                bad(name, "short");
                continue;
            }
            std::memcpy(&h, b, sizeof(h));
            if (std::memcmp(h.magic, "WTPN", 4) != 0 ||
                sizeof(h) + h.pathCount * sizeof(fmtt::TaxiPathRef) + h.nodeCount * sizeof(fmtt::TaxiPathNodeRec) != size) {
                bad(name, "header");
                continue;
            }
            uint32_t prev = 0;
            for (uint32_t i = 0; i < h.pathCount; ++i) {
                fmtt::TaxiPathRef r;
                std::memcpy(&r, b + sizeof(h) + i * sizeof(r), sizeof(r));
                if (r.first + r.count > h.nodeCount || (i && r.pathId <= prev)) bad(name, "path ref");
                prev = r.pathId;
            }
        }
    }
    if (wmos || anims)
        std::printf("verify transports: %d WMO displays (%d parts), %d animated entries, %d errors\n", wmos, parts,
                    anims, errors);
    return errors;
}

}  // namespace wowee::pspconv
