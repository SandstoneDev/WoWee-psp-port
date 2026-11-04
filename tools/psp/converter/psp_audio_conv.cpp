// psp_audio_conv - zone music for the PSP (M12).
//
//   psp_audio_conv --extracted <dir> --map <map> --out <audio.pak> --music-out <data/music dir>
//                  [--no-original]
//
// Runs upstream's own game::ZoneManager (src/game/zone_manager.cpp, compiled
// here unchanged): initialize() = the hand-written zone table, tile table and
// the client's "Original Music" tracks (file:, found relative to the working
// directory, so run it from the repo root), enrichFromDBC() = AreaTable ->
// ZoneMusic -> SoundEntries, and its "track not in the data" pruning. The
// AssetManager it wants is served by two stand-ins below (loadDBC,
// fileExists over the extracted tree). Its result is written out instead of
// being rebuilt on the PSP:
//   "zones"            AudioZones: tracks (card paths), zone -> track list,
//                      AreaTable parents (resolveAreaZoneId), tile -> zone
//                      (getZoneId), see psp_audio_format.hpp;
//   "a_<x>_<y>"        256 x u16: the MCNK areaId of every chunk of an ADT
//                      tile (upstream Renderer::getCurrentZoneId step 1);
// and every referenced MP3 is copied to --music-out (lowercase path, as the
// PSP opens it: data/music/<path>), the client's own tracks to
// music/original/.
#include "game/zone_manager.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/psp/psp_audio_format.hpp"

#include "conv_common.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace wowee;
using namespace wowee::pspconv;
namespace fs = std::filesystem;
namespace fmt = wowee::rendering::psp;

namespace {
std::string g_root;

std::string normalise(std::string p) {
    for (char& c : p) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return p;
}
} // namespace

// Stand-ins for the two AssetManager calls ZoneManager::enrichFromDBC makes.
namespace wowee::pipeline {
std::shared_ptr<DBCFile> AssetManager::loadDBC(const std::string& name) {
    auto dbc = std::make_shared<DBCFile>();
    std::vector<uint8_t> bytes;
    if (!wowee::pspconv::readFile(g_root + "/dbfilesclient/" + normalise(name), bytes) || !dbc->load(bytes)) return nullptr;
    return dbc;
}
bool AssetManager::fileExists(const std::string& path) const {
    std::error_code ec;
    return fs::exists(g_root + "/" + normalise(path), ec);
}
} // namespace wowee::pipeline

int main(int argc, char** argv) {
    std::string map = "Azeroth", out, musicOut;
    bool original = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) g_root = argv[++i];
        else if (a == "--map" && i + 1 < argc) map = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--music-out" && i + 1 < argc) musicOut = argv[++i];
        else if (a == "--no-original") original = false;
    }
    if (g_root.empty() || out.empty() || musicOut.empty()) {
        std::fprintf(stderr, "usage: psp_audio_conv --extracted <dir> --map Azeroth --out <audio.pak> "
                             "--music-out <psp/bin/data/music> [--no-original]\n");
        return 2;
    }

    game::ZoneManager zm;
    zm.initialize();
    alignas(pipeline::AssetManager) unsigned char amStorage[sizeof(pipeline::AssetManager)] = {};
    // Only the two member functions defined above are called; they use no state.
    zm.enrichFromDBC(reinterpret_cast<pipeline::AssetManager*>(amStorage));

    pipeline::DBCFile area;
    std::vector<uint8_t> bytes;
    if (!readFile(g_root + "/dbfilesclient/areatable.dbc", bytes) || !area.load(bytes)) {
        std::fprintf(stderr, "areatable.dbc missing\n");
        return 1;
    }

    // Tracks and zones (every AreaTable id that has a ZoneInfo).
    std::vector<std::string> tracks;
    std::map<std::string, uint16_t> trackIndex;
    std::vector<fmt::AudioZone> zones;
    std::vector<uint16_t> zoneTracks;
    size_t copied = 0, missing = 0, copiedKb = 0;
    auto cardPath = [&](const std::string& p) -> std::string {
        if (p.rfind("file:", 0) == 0) return "original/" + normalise(fs::path(p.substr(5)).filename().string());
        return normalise(p);
    };
    auto addTrack = [&](const std::string& p) -> int {
        if (!original && p.rfind("file:", 0) == 0) return -1;
        const std::string card = cardPath(p);
        auto it = trackIndex.find(card);
        if (it != trackIndex.end()) return it->second;
        const std::string src = p.rfind("file:", 0) == 0 ? p.substr(5) : g_root + "/" + normalise(p);
        const fs::path dst = fs::path(musicOut) / card;
        std::error_code ec;
        if (!fs::exists(src, ec)) {
            ++missing;
            return -1;
        }
        fs::create_directories(dst.parent_path(), ec);
        if (!fs::exists(dst, ec) || fs::file_size(dst, ec) != fs::file_size(src, ec)) {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::fprintf(stderr, "copy failed %s: %s\n", src.c_str(), ec.message().c_str());
                return -1;
            }
        }
        ++copied;
        copiedKb += static_cast<size_t>(fs::file_size(dst, ec) / 1024);
        const uint16_t idx = static_cast<uint16_t>(tracks.size());
        tracks.push_back(card);
        trackIndex[card] = idx;
        return idx;
    };
    std::vector<fmt::AudioArea> areas;
    for (uint32_t r = 0; r < area.getRecordCount(); ++r) {
        const uint32_t id = area.getUInt32(r, 0);
        if (!id) continue;
        areas.push_back({id, area.getUInt32(r, 2)});
        const game::ZoneInfo* zi = zm.getZoneInfo(id);
        if (!zi || zi->musicPaths.empty()) continue;
        fmt::AudioZone z{};
        z.zoneId = id;
        z.firstTrack = static_cast<uint16_t>(zoneTracks.size());
        for (const std::string& p : zi->musicPaths) {
            const int t = addTrack(p);
            if (t >= 0) zoneTracks.push_back(static_cast<uint16_t>(t));
        }
        z.trackCount = static_cast<uint16_t>(zoneTracks.size() - z.firstTrack);
        if (z.trackCount) zones.push_back(z);
    }
    std::vector<fmt::AudioMusicRule> musicRules;
    std::vector<uint16_t> musicTracks;
    pipeline::DBCFile zoneMusic, soundEntries;
    std::vector<uint8_t> zoneBytes, soundBytes;
    if (readFile(g_root+"/dbfilesclient/zonemusic.dbc",zoneBytes) && zoneMusic.load(zoneBytes) &&
        readFile(g_root+"/dbfilesclient/soundentries.dbc",soundBytes) && soundEntries.load(soundBytes) &&
        zoneMusic.getFieldCount()>=8 && soundEntries.getFieldCount()>23 && area.getFieldCount()>8) {
        for (uint32_t i=0;i<area.getRecordCount();++i) {
            const uint32_t musicId=area.getUInt32(i,8);
            const int32_t mi=zoneMusic.findRecordById(musicId);
            if (!musicId || mi<0) continue;
            fmt::AudioMusicRule rule{};
            rule.areaId=area.getUInt32(i,0);
            for (uint32_t phase=0;phase<2;++phase) {
                rule.soundId[phase]=zoneMusic.getUInt32(mi,6+phase);
                rule.silenceMinMs[phase]=zoneMusic.getUInt32(mi,2+phase);
                rule.silenceMaxMs[phase]=zoneMusic.getUInt32(mi,4+phase);
                rule.firstTrack[phase]=static_cast<uint32_t>(musicTracks.size());
                const int32_t si=soundEntries.findRecordById(rule.soundId[phase]);
                if (si>=0) {
                    const std::string dir=soundEntries.getString(si,23);
                    for (uint32_t field=3;field<=12;++field) {
                        const std::string file=soundEntries.getString(si,field);
                        if (file.empty()) continue;
                        const int track=addTrack(dir.empty() ? file : dir+"/"+file);
                        if (track>=0) musicTracks.push_back(static_cast<uint16_t>(track));
                    }
                }
                rule.trackCount[phase]=static_cast<uint32_t>(musicTracks.size())-rule.firstTrack[phase];
            }
            musicRules.push_back(rule);
        }
        std::sort(musicRules.begin(),musicRules.end(),[](const auto& a,const auto& b){ return a.areaId<b.areaId; });
    }
    std::printf("authored music: %zu area rules, %zu day/night track references\n",musicRules.size(),musicTracks.size());
    std::vector<fmt::AudioTile> tiles;
    for (int x = 0; x < 64; ++x)
        for (int y = 0; y < 64; ++y)
            if (uint32_t z = zm.getZoneId(x, y)) tiles.push_back({static_cast<uint32_t>(x * 100 + y), z});

    // "zones" blob: header, track path table, zones, zone tracks, areas, tiles.
    std::vector<uint8_t> blob(sizeof(fmt::AudioZonesHeader));
    auto put = [&](const void* p, size_t n) {
        while (blob.size() & 3u) blob.push_back(0);
        const uint32_t off = static_cast<uint32_t>(blob.size());
        blob.insert(blob.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
        return off;
    };
    fmt::AudioZonesHeader h{};
    std::memcpy(h.magic, "WZMU", 4);
    h.version = 1;
    h.trackCount = static_cast<uint32_t>(tracks.size());
    h.zoneCount = static_cast<uint32_t>(zones.size());
    h.zoneTrackCount = static_cast<uint32_t>(zoneTracks.size());
    h.areaCount = static_cast<uint32_t>(areas.size());
    h.tileCount = static_cast<uint32_t>(tiles.size());
    std::vector<uint32_t> nameOffsets;
    std::string names;
    for (const std::string& t : tracks) {
        nameOffsets.push_back(static_cast<uint32_t>(names.size()));
        names += t;
        names.push_back('\0');
    }
    h.trackNameOffset = put(nameOffsets.data(), nameOffsets.size() * 4);
    h.namesOffset = put(names.data(), names.size());
    h.zoneOffset = put(zones.data(), zones.size() * sizeof(fmt::AudioZone));
    h.zoneTrackOffset = put(zoneTracks.data(), zoneTracks.size() * 2);
    h.areaOffset = put(areas.data(), areas.size() * sizeof(fmt::AudioArea));
    h.tileOffset = put(tiles.data(), tiles.size() * sizeof(fmt::AudioTile));
    std::memcpy(blob.data(), &h, sizeof(h));
    std::vector<PakItem> items;
    items.push_back({"zones", 3, std::move(blob)});
    if (!musicRules.empty()) {
        const auto* rules=reinterpret_cast<const uint8_t*>(musicRules.data());
        items.push_back({"music_rules",3,std::vector<uint8_t>(rules,rules+musicRules.size()*sizeof(fmt::AudioMusicRule))});
        const auto* indices=reinterpret_cast<const uint8_t*>(musicTracks.data());
        if (!musicTracks.empty()) items.push_back({"music_tracks",3,std::vector<uint8_t>(indices,indices+musicTracks.size()*sizeof(uint16_t))});
    }

    // MCNK area ids per ADT tile of the map.
    size_t adts = 0;
    const std::string dir = g_root + "/world/maps/" + normalise(map) + "/";
    for (int x = 0; x < 64; ++x) {
        for (int y = 0; y < 64; ++y) {
            std::vector<uint8_t> adt;
            if (!readFile(dir + normalise(map) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".adt", adt)) continue;
            const pipeline::ADTTerrain t = pipeline::ADTLoader::load(adt);
            std::vector<uint8_t> grid(256 * 2);
            for (int c = 0; c < 256; ++c) {
                const uint16_t a = static_cast<uint16_t>(t.chunks[c].areaId);
                std::memcpy(&grid[c * 2], &a, 2);
            }
            items.push_back({"a_" + std::to_string(x) + "_" + std::to_string(y), 3, std::move(grid)});
            ++adts;
        }
    }
    if (!writePak(out, items)) return 1;
    std::printf("zones %zu, tracks %zu (%zu MB, %zu missing), areas %zu, tile zones %zu, area grids %zu -> %s\n",
                zones.size(), tracks.size(), copiedKb / 1024, missing, areas.size(), tiles.size(), adts, out.c_str());
    return 0;
}
