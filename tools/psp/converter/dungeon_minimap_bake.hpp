// The WMO minimap ("wmmi" + "u_<n>") and the dungeon world map ("dmap" +
// "f_<wma>_<floor>_<n>") of a dungeon's minimap_<dir>.pak - include/rendering/psp/psp_minimap_format.hpp.
// Included by psp_minimap_conv.cpp inside its anonymous namespace (Image, loadBlp, resample, encodeWtex, Names,
// loadDbc, parseTrs, wmPath, scaledForPsp, PakItem are its own). The user's own data only (MPQ / DBC files).
//
// The tile rules (dmTilePixels, bakeWmoMinimap) are the clean-room fragment dungeon_minimap_tiles.inc;
// the placement of the WMO is upstream's - a global WMO as
// WorldLoader::loadMapGeometry (psp_model_conv bakeGlobalWmo), an ADT's MODF as terrain_manager placementEuler.

struct DmChunkIter {  // the IFF chunks of a client file (ids stored reversed)
    const std::vector<uint8_t>& d;
    template <typename F>
    void each(size_t from, size_t to, F&& fn) const {
        for (size_t i = from; i + 8 <= to && i + 8 <= d.size();) {
            uint32_t size = 0;
            std::memcpy(&size, &d[i + 4], 4);
            if (i + 8 + size > d.size()) break;
            const char tag[5] = {static_cast<char>(d[i + 3]), static_cast<char>(d[i + 2]), static_cast<char>(d[i + 1]),
                                 static_cast<char>(d[i]), 0};
            fn(std::string(tag), i + 8, static_cast<size_t>(size));
            i += 8 + size;
        }
    }
};

struct DmWmo {
    bool ok = false;
    std::string key;  // md5translate prefix: "wmo\dungeon\...\name" (lower case, no "world\", no ".wmo")
    struct Group {
        float bmin[3]{}, bmax[3]{};
        uint32_t flags = 0, wmoGroupId = 0;
        std::vector<uint16_t> portalRefs;  // MOPR indices
    };
    std::vector<Group> groups;
    std::vector<glm::vec3> mopv;
    std::vector<std::pair<uint16_t, uint16_t>> mopt;      // start, count
    std::vector<std::pair<uint16_t, uint16_t>> mopr;      // portal, group
};

DmWmo dmLoadWmo(const std::string& root, const std::string& gamePath) {
    DmWmo w;
    std::string p = lower(gamePath);
    for (char& c : p)
        if (c == '\\') c = '/';
    std::vector<uint8_t> d;
    if (!readFile(root + "/" + p, d)) return w;
    std::vector<uint32_t> mogi;
    uint32_t nGroups = 0;
    DmChunkIter it{d};
    it.each(0, d.size(), [&](const std::string& tag, size_t off, size_t size) {
        if (tag == "MOHD" && size >= 8) std::memcpy(&nGroups, &d[off + 4], 4);
        else if (tag == "MOGI")
            for (size_t k = 0; k + 32 <= size; k += 32) {
                uint32_t f = 0;
                std::memcpy(&f, &d[off + k], 4);
                mogi.push_back(f);
            }
        else if (tag == "MOPV")
            for (size_t k = 0; k + 12 <= size; k += 12) {
                glm::vec3 v;
                std::memcpy(&v, &d[off + k], 12);
                w.mopv.push_back(v);
            }
        else if (tag == "MOPT")
            for (size_t k = 0; k + 20 <= size; k += 20) {
                uint16_t s = 0, n = 0;
                std::memcpy(&s, &d[off + k], 2);
                std::memcpy(&n, &d[off + k + 2], 2);
                w.mopt.push_back({s, n});
            }
        else if (tag == "MOPR")
            for (size_t k = 0; k + 8 <= size; k += 8) {
                uint16_t pi = 0, gi = 0;
                std::memcpy(&pi, &d[off + k], 2);
                std::memcpy(&gi, &d[off + k + 2], 2);
                w.mopr.push_back({pi, gi});
            }
    });
    const std::string base = p.substr(0, p.size() - 4);
    for (uint32_t g = 0; g < nGroups; ++g) {
        DmWmo::Group G;
        G.flags = g < mogi.size() ? mogi[g] : 0;
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%03u.wmo", g);
        std::vector<uint8_t> gd;
        if (readFile(root + "/" + base + suffix, gd)) {
            DmChunkIter gi{gd};
            gi.each(0, gd.size(), [&](const std::string& tag, size_t off, size_t size) {
                if (tag != "MOGP" || size < 68) return;
                std::memcpy(G.bmin, &gd[off + 12], 12);
                std::memcpy(G.bmax, &gd[off + 24], 12);
                uint16_t ps = 0, pc = 0;
                std::memcpy(&ps, &gd[off + 36], 2);
                std::memcpy(&pc, &gd[off + 38], 2);
                std::memcpy(&G.wmoGroupId, &gd[off + 56], 4);
                for (uint32_t k = 0; k < pc; ++k) G.portalRefs.push_back(static_cast<uint16_t>(ps + k));
            });
        }
        w.groups.push_back(std::move(G));
    }
    std::string key = lower(gamePath);
    for (char& c : key)
        if (c == '/') c = '\\';
    if (key.rfind("world\\", 0) == 0) key = key.substr(6);
    if (key.size() > 4) key.resize(key.size() - 4);
    w.key = key;
    w.ok = nGroups > 0;
    return w;
}

struct DmPlaced {
    std::string wmo;
    uint32_t uniqueId;
    glm::mat4 toWorld;
};

std::vector<DmPlaced> dmPlacements(const std::string& root, const std::string& mapName) {
    std::vector<DmPlaced> out;
    const std::string dir = lower(mapName);
    std::vector<uint8_t> wdt;
    if (!readFile(root + "/world/maps/" + dir + "/" + dir + ".wdt", wdt)) return out;
    uint32_t mphd = 0;
    std::string gwmo;
    std::vector<uint8_t> gmodf;
    DmChunkIter it{wdt};
    it.each(0, wdt.size(), [&](const std::string& tag, size_t off, size_t size) {
        if (tag == "MPHD" && size >= 4) std::memcpy(&mphd, &wdt[off], 4);
        else if (tag == "MWMO") gwmo = std::string(reinterpret_cast<const char*>(&wdt[off]), strnlen(reinterpret_cast<const char*>(&wdt[off]), size));
        else if (tag == "MODF" && size >= 64) gmodf.assign(wdt.begin() + static_cast<long>(off), wdt.begin() + static_cast<long>(off + 64));
    });
    constexpr float kDeg = 3.14159265358979f / 180.0f;
    if ((mphd & 1u) && !gwmo.empty() && gmodf.size() == 64) {  // psp_model_conv bakeGlobalWmo / upstream WorldLoader
        float pos[3], rot[3];
        std::memcpy(pos, &gmodf[8], 12);
        std::memcpy(rot, &gmodf[20], 12);
        glm::vec3 p(0.0f);
        if (pos[0] != 0.0f || pos[1] != 0.0f || pos[2] != 0.0f) p = core::coords::adtToWorld(pos[0], pos[1], pos[2]);
        glm::mat4 m = glm::translate(glm::mat4(1.0f), p);
        m = glm::rotate(m, (rot[1] + 180.0f) * kDeg, glm::vec3(0, 0, 1));
        m = glm::rotate(m, -rot[0] * kDeg, glm::vec3(0, 1, 0));
        m = glm::rotate(m, -rot[2] * kDeg, glm::vec3(1, 0, 0));
        out.push_back({gwmo, 0xFFFFFFFFu, m});
        return out;
    }
    std::set<uint32_t> seen;
    for (const auto& [x, y] : wdtTiles(root, mapName)) {
        std::vector<uint8_t> a;
        if (!readFile(root + "/world/maps/" + dir + "/" + dir + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt", a))
            continue;
        std::vector<uint8_t> mwmo;
        std::vector<uint32_t> mwid;
        std::vector<size_t> modf;
        size_t modfSize = 0;
        DmChunkIter ai{a};
        ai.each(0, a.size(), [&](const std::string& tag, size_t off, size_t size) {
            if (tag == "MWMO") mwmo.assign(a.begin() + static_cast<long>(off), a.begin() + static_cast<long>(off + size));
            else if (tag == "MWID")
                for (size_t k = 0; k + 4 <= size; k += 4) {
                    uint32_t v = 0;
                    std::memcpy(&v, &a[off + k], 4);
                    mwid.push_back(v);
                }
            else if (tag == "MODF") {
                modf.push_back(off);
                modfSize = size;
            }
        });
        for (size_t mo : modf)
            for (size_t k = 0; k + 64 <= modfSize; k += 64) {
                uint32_t nameId = 0, uid = 0;
                float pos[3], rot[3];
                uint16_t scale = 0;
                std::memcpy(&nameId, &a[mo + k], 4);
                std::memcpy(&uid, &a[mo + k + 4], 4);
                std::memcpy(pos, &a[mo + k + 8], 12);
                std::memcpy(rot, &a[mo + k + 20], 12);
                std::memcpy(&scale, &a[mo + k + 62], 2);
                if (nameId >= mwid.size() || mwid[nameId] >= mwmo.size() || !seen.insert(uid).second) continue;
                const char* nm = reinterpret_cast<const char*>(&mwmo[mwid[nameId]]);
                // upstream terrain_manager placementEuler (psp_model_conv placementAxes) + adtToWorld
                glm::mat4 m = glm::translate(glm::mat4(1.0f), core::coords::adtToWorld(pos[0], pos[1], pos[2]));
                m = glm::rotate(m, (rot[1] + 180.0f) * kDeg, glm::vec3(0, 0, 1));
                m = glm::rotate(m, rot[0] * kDeg, glm::vec3(0, 1, 0));
                m = glm::rotate(m, rot[2] * kDeg, glm::vec3(1, 0, 0));
                m = glm::scale(m, glm::vec3((scale ? scale : 1024) / 1024.0f));
                out.push_back({std::string(nm, strnlen(nm, mwmo.size() - mwid[nameId])), uid, m});
            }
    }
    return out;
}

void dmPutMatrix(float dst[12], const glm::mat4& m) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r) dst[c * 3 + r] = m[c][r];
}

struct DmStats {
    size_t instances = 0, groups = 0, tiles = 0, baked = 0, missing = 0, empty = 0, sizeOff = 0, bytes = 0;
};

// dmTilePixels and bakeWmoMinimap ("wmmi" + "u_<n>"): the clean-room fragment (dungeon_minimap_tiles.md v2).
#include "dungeon_minimap_tiles.inc"

// DUNGEON_FLOOR_<NAME><n> = "..." of the data's GlobalStrings.lua (ruRU).
std::map<std::string, std::string> dmFloorNames(const std::string& root) {
    std::map<std::string, std::string> out;
    std::vector<uint8_t> d;
    if (!readFile(root + "/interface/framexml/globalstrings.lua", d)) return out;
    const std::string s(d.begin(), d.end());
    size_t pos = 0;
    while ((pos = s.find("DUNGEON_FLOOR_", pos)) != std::string::npos) {
        const size_t eq = s.find('=', pos), q0 = s.find('"', eq), q1 = q0 == std::string::npos ? q0 : s.find('"', q0 + 1);
        const size_t nl = s.find('\n', pos);
        if (eq == std::string::npos || q0 == std::string::npos || q1 == std::string::npos || q1 > nl) {
            pos += 14;
            continue;
        }
        std::string key = s.substr(pos, eq - pos);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        out[key] = s.substr(q0 + 1, q1 - q0 - 1);
        pos = q1;
    }
    return out;
}

struct DmFloorStats {
    int floors = 0, chunks = 0, artTiles = 0, missing = 0;
    uint32_t wma = 0;
    bool terrain = false;
};

// "dmap" + "f_<wma>_<floor>_<n>": DungeonMap.dbc floors of the map, DungeonMapChunk, the WorldMapArea of the map and
// its DefaultDungeonFloor (-1: DungeonUsesTerrainMap), Map.dbc's corpse point (the orphan instance's map).
bool bakeDungeonMap(const std::string& root, uint32_t mapId, int wmTile, std::vector<PakItem>& items, DmFloorStats& st) {
    pipeline::DBCFile map, dm, dmc, wma;
    if (!loadDbc(root, "map.dbc", map)) return false;
    int32_t corpseMap = -1;
    float corpseX = 0.0f, corpseY = 0.0f;
    bool dungeon = false;
    for (uint32_t i = 0; i < map.getRecordCount(); ++i)
        if (map.getUInt32(i, 0) == mapId) {
            const uint32_t t = map.getUInt32(i, 2);
            dungeon = t == 1 || t == 2;
            corpseMap = static_cast<int32_t>(map.getUInt32(i, 59));
            corpseX = map.getFloat(i, 60);
            corpseY = map.getFloat(i, 61);
        }
    if (!dungeon) return false;
    std::string folder;
    uint32_t wmaId = 0;
    if (loadDbc(root, "worldmaparea.dbc", wma))
        for (uint32_t i = 0; i < wma.getRecordCount(); ++i)
            if (wma.getUInt32(i, 1) == mapId) {
                wmaId = wma.getUInt32(i, 0);
                folder = wma.getString(i, 3);
                st.terrain = static_cast<int32_t>(wma.getUInt32(i, 9)) == -1;
                break;
            }
    st.wma = wmaId;
    struct F {
        fmt::DungeonFloor rec;
        std::string name;
    };
    std::vector<F> floors;
    if (loadDbc(root, "dungeonmap.dbc", dm))
        for (uint32_t i = 0; i < dm.getRecordCount(); ++i) {
            if (dm.getUInt32(i, 1) != mapId) continue;
            F f{};
            f.rec.id = dm.getUInt32(i, 0);
            f.rec.floor = static_cast<uint16_t>(dm.getUInt32(i, 2));
            // fields 3 / 4: server Y min / max, 5 / 6: server X min / max (some raids: the rows
            // hold the server's own coordinates of the floors).
            f.rec.locRight = dm.getFloat(i, 3);
            f.rec.locLeft = dm.getFloat(i, 4);
            f.rec.locBottom = dm.getFloat(i, 5);
            f.rec.locTop = dm.getFloat(i, 6);
            floors.push_back(f);
        }
    std::sort(floors.begin(), floors.end(), [](const F& a, const F& b) { return a.rec.floor < b.rec.floor; });
    // Two rows of one floor (Trial of the Crusader 94 / 100): the first.
    floors.erase(std::unique(floors.begin(), floors.end(), [](const F& a, const F& b) { return a.rec.floor == b.rec.floor; }),
                 floors.end());
    std::vector<fmt::DungeonChunk> chunks;
    if (loadDbc(root, "dungeonmapchunk.dbc", dmc))
        for (uint32_t i = 0; i < dmc.getRecordCount(); ++i)
            if (dmc.getUInt32(i, 1) == mapId)
                chunks.push_back({dmc.getUInt32(i, 2), dmc.getUInt32(i, 3), dmc.getFloat(i, 4)});
    const auto names = dmFloorNames(root);
    std::string up = folder;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    Names pool;
    std::vector<PakItem> art;
    for (F& f : floors) {
        // WorldMapLevelDropDown_Initialize: floorNum = the level (- 1 with a terrain map) = the floor index itself.
        auto it = names.find("DUNGEON_FLOOR_" + up + std::to_string(f.rec.floor));
        if (it != names.end()) f.rec.nameOffset = pool.add(it->second);
        if (folder.empty()) continue;
        for (int n = 0; n < 12; ++n) {
            Image img;
            const std::string p = wmPath(root, folder, folder + std::to_string(f.rec.floor) + "_" + std::to_string(n + 1));
            if (!loadBlp(p, img)) {
                ++st.missing;
                continue;
            }
            char nm[48];
            fmt::dungeonFloorTileName(nm, sizeof(nm), wmaId, f.rec.floor, n);
            art.push_back({nm, kKindTexture, encodeWtex(scaledForPsp(img, wmTile), false)});
            f.rec.tileMask |= static_cast<uint16_t>(1u << n);
            ++st.artTiles;
        }
    }
    fmt::DungeonMapHeader h{};
    std::memcpy(h.magic, "WDMP", 4);
    h.version = fmt::kDungeonMapVersion;
    h.mapId = mapId;
    h.wma = wmaId;
    h.corpseMap = corpseMap;
    h.corpseX = corpseX;
    h.corpseY = corpseY;
    h.floorCount = static_cast<uint16_t>(floors.size());
    h.chunkCount = static_cast<uint16_t>(chunks.size());
    h.terrainLevel = st.terrain && !floors.empty() ? 1 : 0;
    if (h.terrainLevel)
        if (auto it = names.find("DUNGEON_FLOOR_" + up + "0"); it != names.end()) h.terrainName = pool.add(it->second);
    h.namesSize = static_cast<uint32_t>(pool.data.size());
    std::vector<uint8_t> rec;
    putStruct(rec, h);
    for (const F& f : floors) putStruct(rec, f.rec);
    for (const auto& c : chunks) putStruct(rec, c);
    rec.insert(rec.end(), pool.data.begin(), pool.data.end());
    items.push_back({"dmap", kKindRaw, std::move(rec)});
    for (auto& a : art) items.push_back(std::move(a));
    st.floors = static_cast<int>(floors.size());
    st.chunks = static_cast<int>(chunks.size());
    return true;
}

// --verify of the two records (the PSP's views) and their textures; --dump: each floor's art stitched.
int verifyDungeonRecords(PakView& pak, const std::string& dumpDir, bool quiet) {
    int errors = 0;
    auto bad = [&](const std::string& what) {
        if (++errors <= 40) std::printf("verify ERROR: %s\n", what.c_str());
    };
    uint32_t size = 0;
    if (const uint8_t* b = pak.find("wmmi", size)) {
        fmt::WmoMinimapView v;
        if (!v.attach(b, size)) {
            bad("wmmi refused by WmoMinimapView");
        } else {
            uint32_t baked = 0;
            for (uint32_t i = 0; i < v.h->tileCount; ++i) {
                if (!v.tiles[i].baked) continue;
                char n[32];
                fmt::wmoMinimapTileName(n, sizeof(n), i);
                uint32_t es = 0;
                const uint8_t* t = pak.find(n, es);
                int levels = 0;
                if (!t || !wtexSane(t, es, levels)) bad(std::string("wmo minimap tile missing/bad: ") + n);
                ++baked;
            }
            // Every group's tiles inside its box (+ the last tile's reach), every instance's matrices inverse.
            for (uint32_t i = 0; i < v.h->instanceCount; ++i) {
                const auto& in = v.inst[i];
                float x, y, z, a, b, c;
                fmt::wmoMinimapApply(in.toWorld, 10.0f, 20.0f, 30.0f, x, y, z);
                fmt::wmoMinimapApply(in.toLocal, x, y, z, a, b, c);
                if (std::abs(a - 10.0f) + std::abs(b - 20.0f) + std::abs(c - 30.0f) > 0.05f)
                    bad("wmmi instance " + std::to_string(i) + ": toLocal is not toWorld's inverse");
            }
            if (!quiet)
                std::printf("verify wmmi: instances=%u groups=%u tiles=%u baked=%u portals=%u kb=%u\n", v.h->instanceCount,
                            v.h->groupCount, v.h->tileCount, baked, v.h->portalCount, size / 1024u);
        }
    }
    if (const uint8_t* b = pak.find("dmap", size)) {
        fmt::DungeonMapView v;
        if (!v.attach(b, size)) {
            bad("dmap refused by DungeonMapView");
        } else {
            for (uint32_t f = 0; f < v.h->floorCount; ++f) {
                const fmt::DungeonFloor& F = v.floors[f];
                Image canvas;
                for (int n = 0; n < 12; ++n) {
                    if (!(F.tileMask & (1u << n))) continue;
                    char nm[48];
                    fmt::dungeonFloorTileName(nm, sizeof(nm), v.h->wma, F.floor, n);
                    uint32_t es = 0;
                    const uint8_t* t = pak.find(nm, es);
                    Image img;
                    if (!t || !decodeContent(t, es, img)) {
                        bad(std::string("dungeon floor tile missing/bad: ") + nm);
                        continue;
                    }
                    if (!dumpDir.empty()) {
                        if (!canvas.w) {
                            canvas.w = img.w * 4;
                            canvas.h = img.h * 3;
                            canvas.px.assign(static_cast<size_t>(canvas.w) * canvas.h, Rgb{0, 0, 0, 1});
                        }
                        for (int y = 0; y < img.h && y < canvas.h / 3; ++y)
                            for (int x = 0; x < img.w && x < canvas.w / 4; ++x)
                                canvas.at((n % 4) * (canvas.w / 4) + x, (n / 4) * (canvas.h / 3) + y) = img.at(x, y);
                    }
                }
                // The PC check of both rules at once: the WMO minimap tiles of this floor's groups (DungeonMapChunk),
                // placed by their WMO's matrix and projected through the floor's bounds, tinted red over the art -
                // the art's corridors and the tiles must coincide (floor_<wma>_<n>_check.png).
                uint32_t mmSize = 0;
                const uint8_t* mmb = pak.find("wmmi", mmSize);
                fmt::WmoMinimapView mv;
                if (!dumpDir.empty() && canvas.w && mmb && mv.attach(mmb, mmSize)) {
                    Image chk = canvas;
                    const float W = static_cast<float>(canvas.w), H = static_cast<float>(canvas.h);
                    // the art is 1002 x 668 of the 1024 x 768 tiles (WorldMapDetailFrame)
                    const float sx = W * 1002.0f / 1024.0f, sy = H * 668.0f / 768.0f;
                    int painted = 0;
                    for (uint32_t ii = 0; ii < mv.h->instanceCount; ++ii) {
                        const auto& in = mv.inst[ii];
                        for (uint32_t g = in.firstGroup; g < uint32_t{in.firstGroup} + in.groupCount; ++g) {
                            bool on = false;
                            for (uint32_t c = 0; c < v.h->chunkCount; ++c)
                                on = on || (v.chunks[c].wmoGroupId == mv.groups[g].wmoGroupId && v.chunks[c].dungeonMapId == F.id);
                            if (!on) continue;
                            for (uint32_t t = mv.groups[g].firstTile; t < mv.groups[g].firstTile + mv.groups[g].tileCount; ++t) {
                                const fmt::WmoMinimapTile& T = mv.tiles[t];
                                if (!T.baked) continue;
                                char n[32];
                                fmt::wmoMinimapTileName(n, sizeof(n), t);
                                uint32_t es = 0;
                                const uint8_t* tb = pak.find(n, es);
                                Image img;
                                if (!tb || !decodeContent(tb, es, img)) continue;
                                const float pw = T.pxW * fmt::kWmoMinimapYardsPerPixel, ph = T.pxH * fmt::kWmoMinimapYardsPerPixel;
                                for (int j = 0; j < img.h; ++j)
                                    for (int i = 0; i < img.w; ++i) {
                                        const Rgb& q = img.at(i, j);
                                        if (q.a < 0.5f) continue;
                                        const float lx = T.x0 + (i + 0.5f) * pw / img.w, ly = T.y0 + ph - (j + 0.5f) * ph / img.h;
                                        float wx, wy, wz;
                                        fmt::wmoMinimapApply(in.toWorld, lx, ly, 0.0f, wx, wy, wz);
                                        const float u = (F.locLeft - wy) / (F.locLeft - F.locRight);
                                        const float vv = (F.locTop - wx) / (F.locTop - F.locBottom);
                                        const int px = static_cast<int>(u * sx), py = static_cast<int>(vv * sy);
                                        if (px < 0 || py < 0 || px >= chk.w || py >= chk.h) continue;
                                        Rgb& d = chk.at(px, py);
                                        d = Rgb{0.5f * d.r + 0.5f, 0.5f * d.g, 0.5f * d.b, 1.0f};
                                        ++painted;
                                    }
                            }
                        }
                    }
                    writePng(dumpDir + "/floor_" + std::to_string(v.h->wma) + "_" + std::to_string(F.floor) + "_check.png",
                             chk.w, chk.h, toRgba8(chk));
                    if (!quiet) std::printf("verify dmap floor %u: %d minimap texels on the art\n", F.floor, painted);
                }
                if (!dumpDir.empty() && canvas.w)
                    writePng(dumpDir + "/floor_" + std::to_string(v.h->wma) + "_" + std::to_string(F.floor) + ".png", canvas.w,
                             canvas.h, toRgba8(canvas));
                if (F.locLeft <= F.locRight || F.locTop <= F.locBottom) bad("dmap floor " + std::to_string(F.id) + ": empty bounds");
            }
            if (!quiet)
                std::printf("verify dmap: map=%u wma=%u floors=%u chunks=%u terrain=%u corpse=%d (%.0f, %.0f)\n", v.h->mapId,
                            v.h->wma, v.h->floorCount, v.h->chunkCount, v.h->terrainLevel, v.h->corpseMap, v.h->corpseX,
                            v.h->corpseY);
        }
    }
    return errors;
}
