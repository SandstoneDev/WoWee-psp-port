// M12: zone music in the world, upstream's logic over data/audio.pak.
//
// Zone under the player - upstream Renderer::getCurrentZoneId:
//   1. MCNK areaId of the chunk under the feet -> ZoneManager::resolveAreaZoneId
//      (AreaTable parents, up to 16 steps);
//   2. the last zone step 1 resolved (same map);
//   3. SMSG_INIT_WORLD_STATES zone, resolved the same way;
//   4. (coordinator's current zone - kept by the state below);
//   5. ZoneManager's hand-written tile table.
// Music - upstream AudioCoordinator::updateZoneAudio: entering a zone starts
// a random track of it if the switch cooldown ran out (then 6 s); a track
// that ended is followed by a new random one of the current zone (then 2 s);
// ZoneManager::getRandomMusic avoids repeating the last track (5 tries).
// MusicManager's "crossfade" = stop the old track at once, start the new one
// fadeMs*0.3 = 0.9 s later without a fade-in. Default volume 30 % (upstream
// settings: music 30); wowee.cfg musicvol 0..100.
// Taverns/blacksmith overrides are not ported: upstream compares hashed WMO
// ids against fixed numbers that never match (dead code as written).
// step 1's grid is the map's own - data/minimap_<map>.pak "agrid" (every ADT
// tile of the map, compact, psp_worldmap_view.hpp AreaGridView); audio.pak's "a_<x>_<y>" grids are
// map 0's (psp_audio_conv's default map, no map in their names): on map 1 they named map 0's
// areas for the same tile numbers. They are read only on map 0 when the minimap pak has no "agrid"; step 5's
// tile table (no map in its keys either) is used on map 0 only.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_audio.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/psp/psp_audio_format.hpp"
#include "rendering/psp/psp_worldmap_view.hpp"  // AreaGridView

#include <cstdio>
#include <cstring>
#include <array>
#include <random>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace fmt = wowee::rendering::psp;

namespace {
constexpr float kTileSize = 533.33333f;

// the pak folder of a map (same list as mapDirName in ui_world_screen.cpp).
const char* gridMapDir(uint32_t mapId) {
    switch (mapId) {
        case 0: return "azeroth";
        case 1: return "kalimdor";
        case 530: return "expansion01";
        case 571: return "northrend";
        default: return nullptr;
    }
}

// The validated "agrid" blob as the reader's view (ZoneMusic keeps the blob only: no rendering header there).
fmt::AreaGridView gridView(const void* blob) {
    fmt::AreaGridView v;
    if (!blob) return v;
    const auto* p = static_cast<const uint8_t*>(blob);
    v.h = reinterpret_cast<const fmt::AreaGridHeader*>(p);
    v.tiles = reinterpret_cast<const fmt::AreaGridTile*>(p + sizeof(fmt::AreaGridHeader));
    v.data = reinterpret_cast<const uint8_t*>(v.tiles + v.h->tileCount);
    return v;
}
}  // namespace

void WorldScreen::ZoneMusic::releaseGrids() {
    if (gridBlob) pp::pakFree(gridBlob);
    gridBlob = nullptr;
    gridBytes = 0;
    grids.clear();
}

WorldScreen::ZoneMusic::~ZoneMusic() {
    releaseGrids();
    if (blob) pp::pakFree(blob);
    blob = nullptr;
}

bool WorldScreen::ZoneMusic::load(uint32_t mapId) {
    // reloadable (another map) - the previous map's tables go first.
    releaseGrids();
    if (blob) pp::pakFree(blob);
    blob = nullptr;
    h = nullptr;
    lastPlayed = nullptr;
    areaParents.clear();
    tileToZone.clear();
    zoneIndex.clear();
    // The map's area grid ("agrid" of its minimap pak): read whole, closed at once.
    char path[256];
    if (const char* dir = gridMapDir(mapId)) {
        char rel[64];
        std::snprintf(rel, sizeof(rel), "data/minimap_%s.pak", dir);
        pp::fsPath(path, sizeof(path), rel);
        pp::Pak mm;
        if (mm.open(path)) {
            if (const pp::PakEntry* ge = mm.find("agrid")) {
                void* g = mm.load(ge);
                fmt::AreaGridView v;
                if (g && v.attach(g, ge->size) && v.h->mapId == mapId) {
                    gridBlob = g;  // kept (pp::pakFree in releaseGrids)
                    gridBytes = ge->size;
                } else if (g) {
                    pp::pakFree(g);
                }
            }
            mm.close();
        }
    }
    PSP_LOGI(pp::tag::Sys, "area_grid map=%u tiles=%u kb=%u", static_cast<unsigned>(mapId),
             gridBlob ? static_cast<unsigned>(gridView(gridBlob).h->tileCount) : 0u, static_cast<unsigned>(gridBytes / 1024u));
    pp::fsPath(path, sizeof(path), "data/audio.pak");
    if (!pak.open(path)) return false;
    const pp::PakEntry* e = pak.find("zones");
    blob = e ? pak.load(e) : nullptr;
    if (!blob || e->size < sizeof(fmt::AudioZonesHeader) || std::memcmp(blob, "WZMU", 4) != 0) {
        if (blob) pp::pakFree(blob);
        blob = nullptr;
        pak.close();  // was left open on this path
        return false;
    }
    h = static_cast<const fmt::AudioZonesHeader*>(blob);
    const auto* base = static_cast<const uint8_t*>(blob);
    names = reinterpret_cast<const char*>(base + h->namesOffset);
    nameOffsets = reinterpret_cast<const uint32_t*>(base + h->trackNameOffset);
    zones = reinterpret_cast<const fmt::AudioZone*>(base + h->zoneOffset);
    zoneTracks = reinterpret_cast<const uint16_t*>(base + h->zoneTrackOffset);
    const auto* areas = reinterpret_cast<const fmt::AudioArea*>(base + h->areaOffset);
    for (uint32_t i = 0; i < h->areaCount; ++i) areaParents[areas[i].areaId] = areas[i].parent;
    const auto* tiles = reinterpret_cast<const fmt::AudioTile*>(base + h->tileOffset);
    for (uint32_t i = 0; i < h->tileCount; ++i) tileToZone[tiles[i].key] = tiles[i].zoneId;
    for (uint32_t i = 0; i < h->zoneCount; ++i) zoneIndex[zones[i].zoneId] = i;
    // Every area grid now (18 x 512 B), then the pak is closed: one descriptor
    // fewer for the whole session (card open-file limit).
    // they are map 0's - only on map 0, only without the minimap pak's "agrid".
    for (uint32_t i = 0; mapId == 0 && !gridBlob && i < pak.count(); ++i) {
        const pp::PakEntry* ge = pak.entry(i);
        int x = 0, y = 0;
        if (std::sscanf(ge->name, "a_%d_%d", &x, &y) != 2 || ge->size < 512) continue;
        void* g = pak.load(ge);
        if (!g) continue;
        std::array<uint16_t, 256>& dst = grids[static_cast<uint32_t>(x * 100 + y)];
        std::memcpy(dst.data(), g, 512);
        pp::pakFree(g);
    }
    pak.close();
    return true;
}

// upstream ZoneManager::resolveAreaZoneId
uint32_t WorldScreen::ZoneMusic::resolveAreaZoneId(uint32_t areaId) const {
    uint32_t current = areaId;
    for (int depth = 0; current != 0 && depth < 16; ++depth) {
        auto it = areaParents.find(current);
        if (it == areaParents.end() || it->second == 0 || it->second == current) return current;
        current = it->second;
    }
    return current != 0 ? current : areaId;
}

uint32_t WorldScreen::ZoneMusic::zoneOfTile(int tileX, int tileY) const {
    auto it = tileToZone.find(static_cast<uint32_t>(tileX * 100 + tileY));
    return it == tileToZone.end() ? 0 : it->second;
}

uint32_t WorldScreen::ZoneMusic::areaAt(int tileX, int tileY, int chunk) {
    if (gridBlob) return gridView(gridBlob).areaAt(tileX, tileY, chunk);  // the map's own grid
    auto it = grids.find(static_cast<uint32_t>(tileX * 100 + tileY));
    return it != grids.end() && chunk >= 0 && chunk < 256 ? it->second[chunk] : 0;
}

// upstream ZoneManager::getRandomMusic (the client's own "file:" tracks are
// in the pool like upstream with the original soundtrack on).
const char* WorldScreen::ZoneMusic::randomMusic(uint32_t zoneId) {
    auto it = zoneIndex.find(zoneId);
    if (it == zoneIndex.end()) return nullptr;
    const fmt::AudioZone& z = zones[it->second];
    if (z.trackCount == 0) return nullptr;
    auto name = [&](uint16_t k) { return names + nameOffsets[zoneTracks[z.firstTrack + k]]; };
    if (z.trackCount == 1) {
        lastPlayed = name(0);
        return lastPlayed;
    }
    static std::mt19937 rng(12345u);
    const char* pick = name(0);
    for (int attempts = 0; attempts < 5; ++attempts) {
        pick = name(static_cast<uint16_t>(std::uniform_int_distribution<int>(0, z.trackCount - 1)(rng)));
        if (!lastPlayed || std::strcmp(pick, lastPlayed) != 0) break;
    }
    lastPlayed = pick;
    return lastPlayed;
}

void WorldScreen::initAudio() {
    musicOk_ = zoneMusic_.load(session_.worldEntry().mapId);  // the map's own area grid
    pp::musicSetVolume(static_cast<float>(pp::configGetInt("musicvol", 30)) / 100.0f);
    PSP_LOGI(pp::tag::Sys, "zone_music_load ok=%d zones=%u tracks=%u", musicOk_ ? 1 : 0,
             musicOk_ ? static_cast<unsigned>(zoneMusic_.h->zoneCount) : 0u,
             musicOk_ ? static_cast<unsigned>(zoneMusic_.h->trackCount) : 0u);
    initSounds();  // sounds (sounds.pak read and closed here, before fd_probe)
}

void WorldScreen::playZoneMusic(const char* track) {
    char path[256], rel[200];
    std::snprintf(rel, sizeof(rel), "data/music/%s", track);
    pp::fsPath(path, sizeof(path), rel);
    if (pp::musicActive()) {
        // MusicManager::crossfadeTo: stop now, start 0.9 s later, no fade-in.
        pp::musicStop(0);
        std::snprintf(pendingTrack_, sizeof(pendingTrack_), "%s", path);
        pendingTrackSec_ = 0.9f;
    } else {
        pp::musicPlay(path, false, 0, 0);
        pendingTrack_[0] = 0;
    }
    PSP_LOGI(pp::tag::Sys, "zone_music zone=%u track=%s", static_cast<unsigned>(zoneId_), track);
}

void WorldScreen::updateAudio(float dtSec) {
    updateSounds(dtSec);  // sounds (effects run without the music pak too)
    if (!musicOk_) return;
    PROF_SCOPE("audio");
    if (musicCooldown_ > 0.0f) musicCooldown_ = std::max(0.0f, musicCooldown_ - dtSec);
    if (pendingTrack_[0]) {
        pendingTrackSec_ -= dtSec;
        if (pendingTrackSec_ <= 0.0f) {
            pp::musicPlay(pendingTrack_, false, 0, 0);
            pendingTrack_[0] = 0;
        }
        return;
    }
    // Zone under the player (Renderer::getCurrentZoneId).
    uint32_t zone = 0;
    int tx = 0, ty = 0, chunk = 0;
    if (terrain_.chunkAt(playerX_, playerY_, tx, ty, chunk)) {
        const uint32_t area = zoneMusic_.areaAt(tx, ty, chunk);
        if (area) zone = zoneMusic_.resolveAreaZoneId(area);
    }
    if (zone) lastResolvedZone_ = zone;
    else zone = lastResolvedZone_;
    if (!zone && session_.worldStateZoneId()) zone = zoneMusic_.resolveAreaZoneId(session_.worldStateZoneId());
    if (!zone) zone = zoneId_;
    // PSP: upstream ZoneManager's hand-written tile table has no map in its keys and its numbers
    // are map 0's (its rows for map 1 do not match map 1's ADT tiles) - on another map (map 1,
    // a dungeon) it would name a map 0 zone: there the grid of the map and the server's zone only.
    if (!zone && session_.worldEntry().mapId == 0) {
        const int tileX = static_cast<int>(std::floor(32.0f - playerY_ / kTileSize));
        const int tileY = static_cast<int>(std::floor(32.0f - playerX_ / kTileSize));
        zone = zoneMusic_.zoneOfTile(tileX, tileY);
    }
    // AudioCoordinator::updateZoneAudio (music part).
    if (zone != 0 && zone != zoneId_) {
        zoneId_ = zone;
        PSP_LOGI(pp::tag::Sys, "zone_enter zone=%u", static_cast<unsigned>(zone));
        if (musicCooldown_ <= 0.0f) {
            if (const char* m = zoneMusic_.randomMusic(zone)) {
                playZoneMusic(m);
                musicCooldown_ = 6.0f;
            }
        }
    }
    if (!pp::musicActive() && zoneId_ != 0 && musicCooldown_ <= 0.0f) {
        if (const char* m = zoneMusic_.randomMusic(zoneId_)) {
            playZoneMusic(m);
            musicCooldown_ = 2.0f;
        }
    }
}

} // namespace wowee::ui::psp
