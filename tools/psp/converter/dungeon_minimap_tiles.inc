// dungeon_minimap_tiles.inc - dmTilePixels and bakeWmoMinimap (PC converter only).
//
// Clean-room implementation of specs/world/dungeon_minimap_tiles.md (v2), sections 2.1 and 3; the wmmi record
// format is specs/world/minimap_format.md 2.3. This is a fragment: it is placed in the converter's
// dungeon_minimap_bake.hpp, which psp_minimap_conv.cpp includes inside its own anonymous namespace, after the
// helpers it uses (spec 2.2): DmWmo, dmLoadWmo, DmPlaced, dmPlacements, dmPutMatrix, DmStats, lower, Rgb, Image,
// loadBlp, resample, encodeWtex, PakItem, kKindTexture, kKindRaw, putStruct, namespace fmt (= wowee::rendering::psp,
// psp_minimap_format.hpp) and glm. It needs <algorithm>, <cmath>, <cstdio>, <map>, <string>, <vector> and
// <glm/glm.hpp> to be included before it.

// 3.1: side of an indoor tile texture in pixels: 0.5 yards per pixel, a power of two between 32 and 256.
int dmTilePixels(float extent) {
    const float e = extent / fmt::kWmoMinimapYardsPerPixel;
    if (!(e > 0.0f)) return 32;  // zero, negative or NaN
    int p = 1;
    while (p < 256 && static_cast<float>(p) < e) p *= 2;
    return p < 32 ? 32 : p;
}

// 3.2: builds the wmmi record and its u_<n> tile textures for every placed WMO of the map that has minimap tiles.
bool bakeWmoMinimap(const std::string& root, const std::string& mapName,
                    const std::map<std::string, std::string>& trs, std::vector<PakItem>& items, DmStats& st) {
    std::vector<fmt::WmoMinimapInstance> instances;
    std::vector<fmt::WmoMinimapGroup> groups;
    std::vector<fmt::WmoMinimapTile> tiles;
    std::vector<fmt::WmoMinimapPortal> portals;
    std::vector<PakItem> textures;
    std::map<std::string, DmWmo> loaded;  // each distinct lower-cased WMO path is loaded at most once per call

    auto tileKey = [](const std::string& key, unsigned g, unsigned tx, unsigned ty) {
        char suffix[48];
        std::snprintf(suffix, sizeof suffix, "_%03u_%02u_%02u", g, tx, ty);
        return key + suffix;
    };
    auto tileCount = [](float extent) {
        if (!(extent > 0.0f)) return 0;  // none when the extent is 0 (or not positive)
        const float n = std::ceil(extent / fmt::kWmoMinimapTileYards);
        return n >= 256.0f ? 256 : static_cast<int>(n);
    };

    const std::vector<DmPlaced> placements = dmPlacements(root, mapName);
    for (const DmPlaced& pl : placements) {
        const std::string path = lower(pl.wmo);
        auto it = loaded.find(path);
        if (it == loaded.end()) it = loaded.emplace(path, dmLoadWmo(root, pl.wmo)).first;
        const DmWmo& w = it->second;
        if (!w.ok) continue;

        bool hasTiles = false;
        for (size_t g = 0; g < w.groups.size() && !hasTiles; ++g)
            if (trs.count(tileKey(w.key, static_cast<unsigned>(g), 0, 0))) hasTiles = true;
        if (!hasTiles) continue;
        if (groups.size() + w.groups.size() > 65535u) break;  // this placement and every later one are dropped

        fmt::WmoMinimapInstance in{};
        dmPutMatrix(in.toWorld, pl.toWorld);
        dmPutMatrix(in.toLocal, glm::inverse(pl.toWorld));
        in.uniqueId = pl.uniqueId;
        in.firstGroup = static_cast<uint16_t>(groups.size());
        in.groupCount = static_cast<uint16_t>(w.groups.size());
        bool first = true;
        for (const DmWmo::Group& gr : w.groups) {
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec4 c((corner & 1) ? gr.bmax[0] : gr.bmin[0], (corner & 2) ? gr.bmax[1] : gr.bmin[1],
                                  (corner & 4) ? gr.bmax[2] : gr.bmin[2], 1.0f);
                const glm::vec4 wp = pl.toWorld * c;
                for (int a = 0; a < 3; ++a) {
                    if (first || wp[a] < in.bmin[a]) in.bmin[a] = wp[a];
                    if (first || wp[a] > in.bmax[a]) in.bmax[a] = wp[a];
                }
                first = false;
            }
        }
        instances.push_back(in);

        for (size_t g = 0; g < w.groups.size(); ++g) {
            const DmWmo::Group& gr = w.groups[g];
            fmt::WmoMinimapGroup out{};
            for (int a = 0; a < 3; ++a) {
                out.bmin[a] = gr.bmin[a];
                out.bmax[a] = gr.bmax[a];
            }
            out.flags = gr.flags;
            out.wmoGroupId = gr.wmoGroupId;

            // portal records (created before the tiles)
            out.firstPortal = static_cast<uint32_t>(portals.size());
            for (uint16_t ref : gr.portalRefs) {
                if (ref >= w.mopr.size()) continue;
                const uint16_t portal = w.mopr[ref].first;
                const uint16_t other = w.mopr[ref].second;
                if (portal >= w.mopt.size()) continue;
                if (other >= w.groups.size()) continue;
                fmt::WmoMinimapPortal pr{};
                bool any = false;
                const uint32_t v0 = w.mopt[portal].first, vn = w.mopt[portal].second;
                for (uint32_t v = v0; v < v0 + vn; ++v) {
                    if (v >= w.mopv.size()) continue;
                    const glm::vec3& p = w.mopv[v];
                    for (int a = 0; a < 3; ++a) {
                        if (!any || p[a] < pr.bmin[a]) pr.bmin[a] = p[a];
                        if (!any || p[a] > pr.bmax[a]) pr.bmax[a] = p[a];
                    }
                    any = true;
                }
                if (!any) continue;
                pr.group = static_cast<uint16_t>(in.firstGroup + other);
                portals.push_back(pr);
            }
            out.portalCount = static_cast<uint16_t>(portals.size() - out.firstPortal);

            // tile records
            out.firstTile = static_cast<uint32_t>(tiles.size());
            const float ex = gr.bmax[0] - gr.bmin[0];
            const float ey = gr.bmax[1] - gr.bmin[1];
            const int cols = tileCount(ex), rows = tileCount(ey);
            const int pxW = dmTilePixels(ex), pxH = dmTilePixels(ey);
            for (int ty = 0; ty < rows; ++ty) {
                for (int tx = 0; tx < cols; ++tx) {
                    const auto entry = trs.find(tileKey(w.key, static_cast<unsigned>(g), static_cast<unsigned>(tx),
                                                        static_cast<unsigned>(ty)));
                    if (entry == trs.end()) continue;
                    fmt::WmoMinimapTile tr{};
                    tr.group = static_cast<uint16_t>(in.firstGroup + g);
                    tr.x = static_cast<uint8_t>(tx);
                    tr.y = static_cast<uint8_t>(ty);
                    tr.pxW = static_cast<uint16_t>(pxW);
                    tr.pxH = static_cast<uint16_t>(pxH);
                    tr.x0 = gr.bmin[0] + static_cast<float>(tx) * fmt::kWmoMinimapTileYards;
                    tr.y0 = gr.bmin[1] + static_cast<float>(ty) * fmt::kWmoMinimapTileYards;
                    tr.baked = 0;

                    const uint32_t tileIndex = static_cast<uint32_t>(tiles.size());
                    Image img;
                    if (!loadBlp(root + "/textures/minimap/" + lower(entry->second) + ".blp", img)) {
                        ++st.missing;
                    } else {
                        if (img.w != pxW || img.h != pxH) ++st.sizeOff;  // informational
                        bool visible = false;
                        for (const Rgb& px : img.px)
                            if (px.a >= 0.02f) {
                                visible = true;
                                break;
                            }
                        if (!visible) {
                            ++st.empty;
                        } else {
                            char name[32];
                            fmt::wmoMinimapTileName(name, sizeof name, tileIndex);
                            PakItem item;
                            item.name = name;
                            item.kind = kKindTexture;
                            item.blob = encodeWtex(resample(img, std::max(8, pxW / 2), std::max(8, pxH / 2)), true);
                            st.bytes += item.blob.size();
                            ++st.baked;
                            tr.baked = 1;
                            textures.push_back(std::move(item));
                        }
                    }
                    tiles.push_back(tr);
                }
            }
            out.tileCount = static_cast<uint16_t>(tiles.size() - out.firstTile);
            groups.push_back(out);
        }
    }

    st.instances = instances.size();
    st.groups = groups.size();
    st.tiles = tiles.size();
    if (instances.empty()) return false;

    fmt::WmoMinimapHeader hdr{};
    hdr.magic[0] = 'W';
    hdr.magic[1] = 'M';
    hdr.magic[2] = 'M';
    hdr.magic[3] = 'I';
    hdr.version = fmt::kWmoMinimapVersion;
    hdr.instanceCount = static_cast<uint16_t>(instances.size());
    hdr.groupCount = static_cast<uint16_t>(groups.size());
    hdr.tileCount = static_cast<uint32_t>(tiles.size());
    hdr.portalCount = static_cast<uint32_t>(portals.size());

    PakItem rec;
    rec.name = "wmmi";
    rec.kind = kKindRaw;
    putStruct(rec.blob, hdr);
    for (const auto& v : instances) putStruct(rec.blob, v);
    for (const auto& v : groups) putStruct(rec.blob, v);
    for (const auto& v : tiles) putStruct(rec.blob, v);
    for (const auto& v : portals) putStruct(rec.blob, v);
    items.push_back(std::move(rec));
    for (PakItem& t : textures) items.push_back(std::move(t));
    return true;
}
