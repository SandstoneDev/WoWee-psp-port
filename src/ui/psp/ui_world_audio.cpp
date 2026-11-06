// M12: zone music in the world, upstream's logic over data/audio.pak.
//
// Zone under the player - upstream Renderer::getCurrentZoneId:
//   1. MCNK areaId of the chunk under the feet -> ZoneManager::resolveAreaZoneId
//      (AreaTable parents, up to 16 steps);
//   2. the last zone step 1 resolved (same map);
//   3. SMSG_INIT_WORLD_STATES zone, resolved the same way;
//   4. (coordinator's current zone - kept by the state below);
//   5. ZoneManager's hand-written tile table.
// Authored music: optional audio.pak music_rules/music_tracks preserve area
// inheritance, day/night pools, and ZoneMusic silence intervals. Older packs
// use their legacy zone lists with spaced playback. musiccontinuous=1 skips
// silence; 0 (default) follows the client's intervals. Zone changes fade.
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_audio.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_music_path.hpp"
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
    musicRules.clear(); musicTracks.clear();
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
    const auto* ruleEntry=pak.find("music_rules");
    const auto* trackEntry=pak.find("music_tracks");
    if (ruleEntry && ruleEntry->size<=1024u*1024u && ruleEntry->size%sizeof(fmt::AudioMusicRule)==0 &&
        trackEntry && trackEntry->size<=1024u*1024u && trackEntry->size%sizeof(uint16_t)==0) {
        void* rules=pak.load(ruleEntry); void* tracks=pak.load(trackEntry);
        if (rules && tracks) {
            const auto* rr=static_cast<const fmt::AudioMusicRule*>(rules);
            const auto* tt=static_cast<const uint16_t*>(tracks);
            musicTracks.assign(tt,tt+trackEntry->size/sizeof(*tt));
            for (uint32_t i=0;i<ruleEntry->size/sizeof(*rr);++i) {
                bool valid=true;
                for (int phase=0;phase<2;++phase)
                    valid=valid && uint64_t(rr[i].firstTrack[phase])+rr[i].trackCount[phase]<=musicTracks.size();
                if (valid) musicRules.push_back(rr[i]);
            }
            for (uint16_t track : musicTracks) if (track>=h->trackCount) { musicRules.clear(); break; }
            std::sort(musicRules.begin(),musicRules.end(),[](const auto& a,const auto& b){return a.areaId<b.areaId;});
        }
        pp::pakFree(rules); pp::pakFree(tracks);
    }
    pak.close();
    return true;
}

const fmt::AudioMusicRule* WorldScreen::ZoneMusic::musicRule(uint32_t areaId) const {
    for (int depth=0;areaId && depth<16;++depth) {
        const auto it=std::lower_bound(musicRules.begin(),musicRules.end(),areaId,
            [](const auto& row,uint32_t id){return row.areaId<id;});
        if (it!=musicRules.end() && it->areaId==areaId) return &*it;
        const auto parent=areaParents.find(areaId);
        if (parent==areaParents.end() || parent->second==areaId) break;
        areaId=parent->second;
    }
    return nullptr;
}

const char* WorldScreen::ZoneMusic::authoredMusic(const fmt::AudioMusicRule& rule,bool night) {
    const int phase=night ? 1 : 0;
    const uint32_t count=rule.trackCount[phase];
    if (!count) return nullptr;
    static std::mt19937 rng(pp::logUptimeMs() ^ 0xC0FFEEu);
    const char* chosen=nullptr;
    for (int retry=0;retry<5;++retry) {
        const uint32_t index=std::uniform_int_distribution<uint32_t>(0,count-1)(rng);
        chosen=names+nameOffsets[musicTracks[rule.firstTrack[phase]+index]];
        if (!lastPlayed || count==1 || std::strcmp(chosen,lastPlayed)!=0) break;
    }
    lastPlayed=chosen;
    return chosen;
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

bool WorldScreen::playZoneMusic(const char* track) {
    if (!track || !*track) return false;
    musicStartCount_=pp::audioStats().musicStarts;
    char path[256];
    if (!pp::musicFilePath(path, sizeof(path), track)) {
        PSP_LOGW(pp::tag::Sys, "zone_music_missing track=%s layout=data/music/<category>/", track);
        return false;
    }
    if (pp::musicActive()) {
        // Fade out before changing the streamed MP3, then fade the new track in.
        pp::musicStop(1200);
        std::snprintf(pendingTrack_, sizeof(pendingTrack_), "%s", path);
        pendingTrackSec_ = 1.3f;
    } else {
        pp::musicPlay(path, false, 0, 800);
        pendingTrack_[0] = 0;
    }
    PSP_LOGI(pp::tag::Sys, "zone_music zone=%u track=%s", static_cast<unsigned>(zoneId_), track);
    return true;
}

void WorldScreen::updateAudio(float dtSec) {
    updateSounds(dtSec);  // sounds (effects run without the music pak too)
    updateEnvironmentAudio();  // area/day/night ambience must run even without music
    const uint8_t mode=session_.life()==pp::WorldSession::Life::Alive?0u:1u;
    if(mode!=deathAudioMode_) {
        const bool wasDead=deathAudioMode_==1u;
        deathAudioMode_=mode;deathAudioRetry_=0;
        pendingTrack_[0]=0;musicCooldown_=0;musicKey_=0xFFFFFFFFu;musicWasPlaying_=false;
        if(mode || wasDead)pp::musicStop(0);
        PSP_LOGI(pp::tag::Sys,"death_audio dead=%u",static_cast<unsigned>(mode));
    }
    pp::musicSetVolume(static_cast<float>(mode?settings_.values().ambientvol:settings_.values().musicvol)/100.f);
    if(mode) {
        deathAudioRetry_=std::max(0.f,deathAudioRetry_-dtSec);
        if(!pp::musicActive() && deathAudioRetry_<=0) {
            char path[256];
            if(pp::musicFilePath(path,sizeof(path),"ambience/ghoststate.mp3"))pp::musicPlay(path,true,0,800);
            else PSP_LOGW(pp::tag::Sys,"death_audio_missing path=data/music/ambience/ghoststate.mp3");
            deathAudioRetry_=5.f;
        }
        return; // ordinary zone playlists resume from their authored rules on resurrection
    }
    if (!musicOk_) return;
    PROF_SCOPE("audio");
    if (musicCooldown_ > 0.0f) musicCooldown_ = std::max(0.0f, musicCooldown_ - dtSec);
    if (pendingTrack_[0]) {
        pendingTrackSec_ -= dtSec;
        if (pendingTrackSec_ <= 0.0f) {
            pp::musicPlay(pendingTrack_, false, 0, 800);
            pendingTrack_[0] = 0;
        }
        return;
    }
    // Zone under the player (Renderer::getCurrentZoneId).
    uint32_t zone = 0, musicArea = 0;
    int tx = 0, ty = 0, chunk = 0;
    if (terrain_.chunkAt(playerX_, playerY_, tx, ty, chunk)) {
        const uint32_t area = zoneMusic_.areaAt(tx, ty, chunk);
        if (area) { musicArea=area; zone = zoneMusic_.resolveAreaZoneId(area); }
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
    if (zone && zone!=zoneId_) {
        zoneId_=zone;
        PSP_LOGI(pp::tag::Sys,"zone_enter zone=%u",static_cast<unsigned>(zone));
    }
    if (!musicArea) musicArea=session_.worldStateZoneId() ? session_.worldStateZoneId() : zone;
    const auto* rule=zoneMusic_.musicRule(musicArea);
    float hour=session_.getGameTime();
    if (hour>=0) hour=std::fmod(hour+static_cast<float>(pp::logUptimeMs()-session_.gameTimeReceivedMs())/3600000.f,24.f);
    const bool night=hour>=0 && (hour<6 || hour>=18);
    const int phase=night ? 1 : 0;
    // Preserve explicit authored silence (soundId=0), but a rule whose files
    // were unavailable must not disable a usable legacy zone playlist.
    const bool useRule=rule && (!rule->soundId[phase] || rule->trackCount[phase]);
    const uint32_t key=useRule ? rule->soundId[phase] : (zone|0x80000000u);
    auto choose=[&]() -> const char* {
        return useRule ? zoneMusic_.authoredMusic(*rule,night) : zoneMusic_.randomMusic(zone);
    };
    if (key!=musicKey_) {
        musicKey_=key; musicCooldown_=0; musicWasPlaying_=false;
        if (const char* track=choose()) {
            musicWasPlaying_=playZoneMusic(track);
            if (!musicWasPlaying_) musicCooldown_=2.0f;
        }
        else if (pp::musicActive()) pp::musicStop(1200);
        return;
    }
    if (pp::musicActive()) { musicWasPlaying_=true; return; }
    if (musicWasPlaying_) {
        musicWasPlaying_=false;
        // A queued request is not proof of playback. Failed opens/decodes
        // retry promptly instead of imposing the authored post-track silence.
        if (pp::audioStats().musicStarts==musicStartCount_) {
            musicCooldown_=2.0f;
            PSP_LOGW(pp::tag::Sys,"music_start_retry seconds=2 check=data/music and music_open_failed");
            return;
        }
        uint32_t low=rule ? rule->silenceMinMs[phase] : 180000;
        uint32_t high=rule ? rule->silenceMaxMs[phase] : 300000;
        if (high<low) std::swap(low,high);
        static std::mt19937 rng(pp::logUptimeMs() ^ 0x51EACEu);
        musicCooldown_=pp::configGetInt("musiccontinuous",0) ? 0.f :
            static_cast<float>(std::uniform_int_distribution<uint32_t>(low,high)(rng))/1000.f;
        PSP_LOGI(pp::tag::Sys,"music_silence ms=%u night=%d",static_cast<unsigned>(musicCooldown_*1000),night ? 1 : 0);
    }
    if (musicCooldown_<=0) {
        if (const char* track=choose()) {
            musicWasPlaying_=playZoneMusic(track);
            if (!musicWasPlaying_) musicCooldown_=2.0f;
        }
        else musicCooldown_=5.f;
    }
}

} // namespace wowee::ui::psp
