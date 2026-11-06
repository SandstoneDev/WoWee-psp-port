// Sounds in the world (include/ui/psp/ui_world_sounds.hpp).
//
// Ported logic (upstream file -> here):
//   audio/ui_sound_manager.cpp       playUi / playByName (0.7 x volume, library[0])
//   audio/combat_sound_manager.cpp   onMeleeHit (swing / miss / impact, 0.8, crit 1.2)
//   game/combat_handler.cpp          handleAttackerStateUpdate's sound block (MEDIUM, FLESH)
//   audio/footstep_manager.cpp       playRandomStep (0.09 / 0.14 s, 0.88 / 1.0, pitch 0.97-1.05)
//   rendering/animation/footstep_driver.cpp  Tracker::trigger, ownFootsteps, surface rules
//   audio/activity_sound_manager.cpp playJump / playLanding / playMeleeSwing / grunts / wounds / strokes
//   rendering/animation/sfx_state_driver.cpp updateActivity (edges, water-exit suppression)
//   audio/movement_sound_manager.cpp playWaterFootstep (0.7 x 0.6 / 0.8 / 1.0)
//   audio/audio_engine.cpp           spatial (inverse distance, min 1, rolloff 1, max 100)
// PSP differences (each marked "PSP:" below): clips come from sounds.pak on
// demand (not preloaded), pitch = playback rate of the mixer, others'
// footsteps (upstream: the own character only), no mounts / swimming yet.
#include "ui/psp/ui_world_sounds.hpp"
#include "rendering/psp/psp_water_format.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_audio.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_prof.hpp"
#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/psp/psp_sound_format.hpp"
#include "ui/psp/ui_world_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace fmt = wowee::rendering::psp;
namespace paths = sound_paths;

namespace {
WorldSounds* g_sounds = nullptr;
constexpr uint32_t kOwnStepTag = 0x53544550u;
} // namespace

// --- API for the other world code ------------------------------------------
void playUiSound(UiSound s) {
    if (g_sounds) g_sounds->playUi(s);
}
bool playSoundByName(const char* name) { return g_sounds && g_sounds->playByName(name); }
bool playSound3D(uint32_t soundEntryId, const glm::vec3& serverPos) {
    return g_sounds && g_sounds->playEntry(soundEntryId, &serverPos);
}
bool playSound2D(uint32_t soundEntryId) { return g_sounds && g_sounds->playEntry(soundEntryId, nullptr); }

// --- WorldSounds ------------------------------------------------------------
WorldSounds::~WorldSounds() { shutdown(); }

void WorldSounds::shutdown() {
    if (g_sounds == this) g_sounds = nullptr;
    bank_.close();
    entries_ = nullptr;
    entriesSize_ = 0;
    entryById_.clear();
    entryByName_.clear();
    others_.clear();
    npcVoices_.clear();
    ambienceAreas_.clear(); ambienceEntry_=~uint32_t{0}; ambienceClip_=-1;
}

std::vector<int> WorldSounds::resolve(const std::vector<std::string>& list) const {
    // upstream preloadCandidates / pickLoadedSample: the ones that exist.
    std::vector<int> out;
    for (const std::string& p : list) {
        const int c = bank_.find(fmt::soundPathHash(p.c_str()));
        if (c >= 0) out.push_back(c);
    }
    return out;
}

int WorldSounds::first(const std::vector<std::string>& list) const {
    // upstream playSound(library): library[0] or nothing.
    return list.empty() ? -1 : bank_.find(fmt::soundPathHash(list[0].c_str()));
}

bool WorldSounds::init(uint32_t mapId, uint32_t race, bool male) {
    shutdown();
    footsteps_ = std::clamp(pp::configGetInt("footsteps", 2), 0, 2);
    const bool audio = pp::configGetInt("sfx", 1) != 0; // water still needs the surface grids
    const int budget = std::clamp(pp::configGetInt("sfxbudget", 512)  /* the hardware heap */, 128, 8192);
    char path[256];
    pp::fsPath(path, sizeof(path), "data/sounds.pak");
    if (!bank_.open(path, static_cast<uint32_t>(budget), mapId, audio)) {
        PSP_LOGW(pp::tag::Sys, "sounds_missing path=%s", path);
        return false;
    }
    entries_ = bank_.entriesBlob(entriesSize_);
    if (entries_ && entriesSize_ >= sizeof(fmt::SoundEntriesHeader)) {
        const auto* h = static_cast<const fmt::SoundEntriesHeader*>(entries_);
        const uint64_t rowsEnd = h->rowOffset + static_cast<uint64_t>(h->rowCount) * sizeof(fmt::SoundEntryRow);
        const uint64_t filesEnd = h->fileOffset + static_cast<uint64_t>(h->fileCount) * 4u;
        if (std::memcmp(h->magic, "WSEN", 4) == 0 && rowsEnd <= entriesSize_ && filesEnd <= entriesSize_) {
            const auto* rows = reinterpret_cast<const fmt::SoundEntryRow*>(static_cast<const uint8_t*>(entries_) +
                                                                            h->rowOffset);
            for (uint32_t i = 0; i < h->rowCount; ++i) {
                if (rows[i].firstFile + static_cast<uint32_t>(rows[i].fileCount) > h->fileCount) continue;
                entryById_.emplace(rows[i].id, i);
                entryByName_.emplace(rows[i].nameHash, i);  // upstream: the first row with a name wins
            }
        } else {
            entries_ = nullptr;
        }
    }

    pp::Pak ambiencePak;
    if (ambiencePak.open(path)) {
        const auto* e=ambiencePak.find("ambience");
        void* data=e && e->size<=1024*1024 && e->size%sizeof(fmt::AreaAmbience)==0 ? ambiencePak.load(e) : nullptr;
        if (data) {
            const auto* rows=static_cast<const fmt::AreaAmbience*>(data);
            for (uint32_t i=0;i<e->size/sizeof(*rows);++i) ambienceAreas_[rows[i].area]={rows[i].parent,rows[i].day,rows[i].night};
            pp::pakFree(data);
        }
        const auto* voiceEntry=ambiencePak.find("npcvoices");
        void* voiceData=voiceEntry && voiceEntry->size<=1024*1024 && voiceEntry->size%sizeof(fmt::NpcVoiceRecord)==0 ? ambiencePak.load(voiceEntry):nullptr;
        if(voiceData) {
            const auto* rows=static_cast<const fmt::NpcVoiceRecord*>(voiceData);
            npcVoices_.assign(rows,rows+voiceEntry->size/sizeof(*rows));
            std::sort(npcVoices_.begin(),npcVoices_.end(),[](const auto& a,const auto& b){return a.display<b.display;});
            pp::pakFree(voiceData);
        }
        ambiencePak.close();
    }
    // UiSoundManager::initialize (+ its fallbacks, whisper before mail).
    for (int s = 0; s < static_cast<int>(UiSound::Count); ++s) {
        uiClip_[s] = -1;
        for (const std::string& p : paths::ui(static_cast<UiSound>(s))) {
            const int c = bank_.find(fmt::soundPathHash(p.c_str()));
            if (c >= 0) {
                uiClip_[s] = c;
                break;
            }
        }
    }
    for (UiSound s : {UiSound::AchievementAlert, UiSound::WhisperReceived, UiSound::MailReceived,
                      UiSound::MinimapPing}) {
        auto& c = uiClip_[static_cast<int>(s)];
        if (c < 0) c = uiClip_[static_cast<int>(paths::uiFallback(s))];
    }
    // CombatSoundManager::initialize.
    const char* sizes[3] = {"Small", "Medium", "Large"};
    for (int i = 0; i < 3; ++i) {
        swing_[i] = resolve(paths::swing(sizes[i]));
        swingCrit_[i] = first(paths::swingCrit(sizes[i]));
    }
    miss1H_ = first(paths::missWhoosh(false));
    miss2H_ = first(paths::missWhoosh(true));
    hitFlesh_ = resolve(paths::impact("Flesh"));
    hitChain_ = resolve(paths::impact("Chain"));
    hitPlate_ = resolve(paths::impact("Plate"));
    hitShield_ = resolve(paths::impact("MetalShield"));
    hitMetalWeapon_ = resolve(paths::impact("MetalWeapon"));
    hitWood_ = resolve(paths::impact("Wood"));
    hitStone_ = resolve(paths::impact("Stone"));
    critFlesh_ = first(paths::impactCrit("Flesh"));
    critChain_ = first(paths::impactCrit("Chain"));
    critPlate_ = first(paths::impactCrit("Plate"));
    critShield_ = first(paths::impactCrit("MetalShield"));
    // FootstepManager::initialize (character bank).
    const char* mats[7] = {"Stone", "Dirt", "Grass", "Wood", "Metal", "Water", "Snow"};
    for (int s = 0; s < 7; ++s) steps_[s].clear();
    for (int s : {0, 1, 2, 3, 6}) steps_[s] = resolve(audio::classicFootstepPaths(mats[s]));
    steps_[5] = resolve(audio::waterFootstepPaths());
    steps_[4] = resolve(audio::altFootstepPaths("MediumLargeMetalFootsteps", "MediumLargeFootstepMetal"));
    if (steps_[4].empty()) steps_[4] = resolve(audio::classicFootstepPaths("Metal"));
    // ActivitySoundManager::initialize + setCharacterVoiceProfile(race, sex).
    splash_ = resolve(paths::splashEnter());
    for (int s = 0; s < 7; ++s) landing_[s] = resolve(audio::classicFootstepPaths(mats[s]));
    meleeSwing_ = resolve(paths::meleeSwing());
    const paths::RaceVoice rv = paths::raceVoice(race);
    swimStroke_ = resolve(paths::swimStroke());
    attackGrunt_ = resolve(paths::vocals(0, rv.folder, rv.base, male));
    wound_ = resolve(paths::vocals(1, rv.folder, rv.base, male));
    woundCrit_ = resolve(paths::vocals(2, rv.folder, rv.base, male));
    deathVoice_ = resolve(paths::vocals(3, rv.folder, rv.base, male));
    pp::Pak voicePak;
    if (voicePak.open(path)) {
        const auto* entry=voicePak.find("voices");
        void* data=entry && entry->size<=4096 && entry->size%sizeof(fmt::CharacterVoice)==0 ? voicePak.load(entry) : nullptr;
        if (data) {
            const auto* rows=static_cast<const fmt::CharacterVoice*>(data);
            for (uint32_t i=0;i<entry->size/sizeof(*rows);++i) {
                if (rows[i].race!=race || rows[i].sex!=(male ? 0u : 1u)) continue;
                auto use=[&](std::vector<int>& bank,uint32_t id) {
                    auto authored=resolveEntry(id);
                    if (!authored.empty()) bank=std::move(authored);
                };
                use(attackGrunt_,rows[i].attack); use(wound_,rows[i].wound);
                use(woundCrit_,rows[i].critical); use(deathVoice_,rows[i].death);
                break;
            }
            pp::pakFree(data);
        }
    }
    PSP_LOGI(pp::tag::Sys,"voice_profile race=%u sex=%s jump=%u wound=%u",
             static_cast<unsigned>(race),male ? "male" : "female",
             0u,static_cast<unsigned>(wound_.size()));
    // MovementSoundManager::initialize (its "small" set loads the medium files).
    const char* water[3] = {"Small", "Medium", "Giant"};
    for (int i = 0; i < 3; ++i) enterWater_[i] = first(paths::enterWater(water[i]));
    waterStep_[0] = resolve(paths::waterFootstep(false));
    waterStep_[1] = waterStep_[0];
    waterStep_[2] = resolve(paths::waterFootstep(true));

    unsigned ui = 0, steps = 0;
    for (int c : uiClip_) ui += c >= 0 ? 1u : 0u;
    for (const auto& v : steps_) steps += static_cast<unsigned>(v.size());
    PSP_LOGI(pp::tag::Sys,
             "sfx_init ui=%u steps=%u swing=%u hits=%u jump=%u grunt=%u wound=%u rows=%u budget_kb=%d feet=%d",
             ui, steps, static_cast<unsigned>(swing_[1].size()), static_cast<unsigned>(hitFlesh_.size()),
             0u, static_cast<unsigned>(attackGrunt_.size()),
             static_cast<unsigned>(wound_.size()), static_cast<unsigned>(entryById_.size()), budget, footsteps_);
    g_sounds = this;
    nextLogMs_ = pp::logUptimeMs() + 10000u;
    return true;
}

void WorldSounds::environment(uint32_t area,bool night,bool indoors,int weather,float intensity,int zoneVolume,int weatherVolume) {
    uint32_t entry=0;
    for (int depth=0;area && depth<16;++depth) {
        const auto it=ambienceAreas_.find(area);
        if (it==ambienceAreas_.end()) break;
        entry=night ? it->second.night : it->second.day;
        if (entry || it->second.parent==area) break;
        area=it->second.parent;
    }
    if (entry!=ambienceEntry_) {
        ambienceEntry_=entry;
        const auto clips=resolveEntry(entry);
        ambienceClip_=clips.empty() ? -1 : clips[static_cast<size_t>(frame_)%clips.size()];
        ambienceGain_=1.0f;
        const auto row=entryById_.find(entry);
        if (entries_ && row!=entryById_.end()) {
            const auto* header=static_cast<const fmt::SoundEntriesHeader*>(entries_);
            const auto* rows=reinterpret_cast<const fmt::SoundEntryRow*>(static_cast<const uint8_t*>(entries_)+header->rowOffset);
            ambienceGain_=std::clamp(rows[row->second].volume,0.0f,1.0f);
        }
        PSP_LOGI(pp::tag::Sys,"zone_ambience entry=%u clip=%d gain=%.2f night=%d",
                 static_cast<unsigned>(entry),ambienceClip_,ambienceGain_,night ? 1 : 0);
    }
    bank_.ambience(0,ambienceClip_,ambienceGain_*zoneVolume*.01f*(indoors ? .25f : 1.f));
    int clip=-1;
    if (!indoors && weather && intensity>.05f && weatherVolume>0) {
        const char* type=weather==1 ? "rain" : weather==2 ? "snow" : "sandstorm";
        const char* grade=intensity>.65f ? "heavy" : intensity>.3f ? "medium" : "light";
        char path[128]; std::snprintf(path,sizeof(path),"sound/ambience/weather/%s%s%s.wav",type,grade,weather==1 ? "loop" : "");
        clip=bank_.find(fmt::soundPathHash(path));
    }
    bank_.ambience(1,clip,weatherVolume*.01f*std::clamp(intensity,0.f,1.f));
}

void WorldSounds::beginFrame(float dtSec, int volumePercent, const glm::vec3& listener, float listenerYaw) {
    (void)dtSec;
    ++frame_;
    bank_.update();
    if (volumePercent != volume_) {
        volume_ = volumePercent;
        pp::sfxSetVolume(static_cast<float>(std::clamp(volumePercent, 0, 100)) / 100.0f);
    }
    listener_ = listener;
    listenerYaw_ = listenerYaw;
    const uint32_t now = pp::logUptimeMs();
    if (static_cast<int32_t>(now - nextLogMs_) >= 0) {
        nextLogMs_ = now + 10000u;
        const auto& audio=pp::audioStats();
        PSP_LOGI(pp::tag::Sys,"sfx_mixer voices=%u late=%u max_us=%u output_errors=%u",
                 static_cast<unsigned>(audio.sfxVoices),static_cast<unsigned>(audio.sfxLateBlocks),
                 static_cast<unsigned>(audio.sfxMixMaxUs),static_cast<unsigned>(audio.outputErrors));
        const pp::SoundBankStats& s = bank_.stats();
        PSP_LOGI(pp::tag::Sys, "sfx_cache kb=%u n=%u loads=%u fails=%u evict=%u plays=%u late=%u drops=%u",
                 static_cast<unsigned>(s.residentKb), static_cast<unsigned>(s.resident), static_cast<unsigned>(s.loads),
                 static_cast<unsigned>(s.loadFails), static_cast<unsigned>(s.evictions), static_cast<unsigned>(s.plays),
                 static_cast<unsigned>(s.latePlays), static_cast<unsigned>(s.drops));
        if (stepsOwn_ + stepsOther_ + stepsNpc_) {
            PSP_LOGI(pp::tag::Sys, "footstep n=%u own=%u players=%u npc=%u",
                     static_cast<unsigned>(stepsOwn_ + stepsOther_ + stepsNpc_), static_cast<unsigned>(stepsOwn_),
                     static_cast<unsigned>(stepsOther_), static_cast<unsigned>(stepsNpc_));
            stepsOwn_ = 0;
            stepsOther_ = 0;
            stepsNpc_ = 0;
        }
    }
}

static uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

float WorldSounds::rnd(float lo, float hi) {
    return lo + (hi - lo) * static_cast<float>(xorshift(rng_) & 0xFFFFu) / 65535.0f;
}

int WorldSounds::pick(const std::vector<int>& clips) {
    if (clips.empty()) return -1;
    return clips[xorshift(rng_) % clips.size()];
}

bool WorldSounds::elapsed(uint32_t& lastMs, float minSec, uint32_t now) const {
    return lastMs == 0 || static_cast<float>(now - lastMs) >= minSec * 1000.0f;
}

bool WorldSounds::play2D(int clip, float volume, float pitch, uint32_t tag) {
    if (clip < 0) return false;
    const float v = std::clamp(volume, 0.0f, 1.0f);
    if (++playLogs_ <= 60)
        PSP_LOGI(pp::tag::Sys, "sfx_play id=%08x vol=%.2f pan=0.00", static_cast<unsigned>(bank_.clipHash(clip)), v);
    return bank_.play(clip, v, 0.0f, pitch, tag);
}

bool WorldSounds::spatial(const glm::vec3& p, float maxDistance, float& gain, float& pan, float minDistance) const {
    const glm::vec3 d = p - listener_;
    const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    // PSP: past maxDistance nothing is started (miniaudio keeps the 1/max
    // floor there: 1 % - inaudible, but a voice of the 12).
    if (!(dist <= maxDistance)) return false;
    gain = minDistance / std::max(dist, minDistance); // authored full-volume radius, inverse rolloff
    const float h = std::sqrt(d.x * d.x + d.y * d.y);
    pan = 0.0f;
    if (h > 0.01f) {
        // Listener right = the camera's forward (cos, sin) turned -90 deg.
        // PSP: 0.7 of a hard pan (headphones; miniaudio's spatializer never
        // mutes a side completely either).
        pan = 0.7f * (d.x * std::sin(listenerYaw_) - d.y * std::cos(listenerYaw_)) / h;
    }
    return true;
}

bool WorldSounds::surfaceAt(float x, float y, float z, StepSurface& out) const {
    if (!(std::fabs(x) < 20000.0f && std::fabs(y) < 20000.0f)) return false;  // NaN / off the map
    // upstream TerrainManager::findChunkAt / chunkFractionsAt (psp_sound_format.hpp).
    const fmt::SurfaceCell sc = fmt::surfaceCellAt(x, y);
    const uint8_t* g = bank_.surfaceGrid(sc.fileX, sc.fileY);
    if (!g) return false;
    const int chunk = sc.chunk;
    const uint32_t n = static_cast<uint32_t>(chunk * 64 + sc.cell);
    const uint8_t* cells = g + sizeof(fmt::SurfaceGridHeader);
    const uint8_t nib = static_cast<uint8_t>((cells[n / 2] >> ((n & 1u) * 4u)) & 0xFu);
    if (nib & fmt::kSurfCellWater) {
        float wh = 0.0f;
        // Use the rendered surface, including v2 slopes, for footsteps too.
        if (waterAt(x, y, wh) && z < wh + 0.25f) {
            out = StepSurface::Water;
            return true;
        }
    }
    const uint8_t s = nib & 0x7u;
    out = s < fmt::kSurfCount ? static_cast<StepSurface>(s) : StepSurface::Stone;
    return true;
}

// the water of a cell for the swim check (ui_world_flying.cpp) - the cell's MH2O bit and
// its chunk's highest liquid level, as surfaceAt reads them.
bool WorldSounds::waterAt(float x, float y, float& level) const {
    if (!(std::fabs(x) < 20000.0f && std::fabs(y) < 20000.0f)) return false;  // NaN / off the map
    const fmt::SurfaceCell sc = fmt::surfaceCellAt(x, y);
    const uint8_t* g = bank_.surfaceGrid(sc.fileX, sc.fileY);
    if (!g) return false;
    const uint32_t n = static_cast<uint32_t>(sc.chunk * 64 + sc.cell);
    const uint8_t* cells = g + sizeof(fmt::SurfaceGridHeader);
    const uint8_t nib = static_cast<uint8_t>((cells[n / 2] >> ((n & 1u) * 4u)) & 0xFu);
    unsigned patchCount = 0;
    const auto* patches = fmt::waterPatches(g, patchCount);
    if (reinterpret_cast<const fmt::SurfaceGridHeader*>(g)->version == 2) {
        constexpr float tile = 533.33333f, chunk = tile / 16.0f, unit = chunk / 8.0f;
        const float fr = std::clamp(((32 - sc.fileY) * tile - (sc.chunk / 16) * chunk - x) / unit, 0.0f, 8.0f);
        const float fc = std::clamp(((32 - sc.fileX) * tile - (sc.chunk % 16) * chunk - y) / unit, 0.0f, 8.0f);
        const int r = std::min(static_cast<int>(fr), 7), c = std::min(static_cast<int>(fc), 7);
        bool found = false;
        for (unsigned i = 0; i < patchCount; ++i) {
            const auto& p = patches[i];
            if (p.chunk != sc.chunk || !fmt::waterCell(p, r, c)) continue;
            const float h = fmt::waterHeight(p, r, c, fr - r, fc - c);
            if (!found || h > level) level = h;
            found = true;
        }
        return found;
    }
    if (!(nib & fmt::kSurfCellWater)) return false;
    uint32_t bits = 0;
    std::memcpy(&bits, cells + 256 * 32 + sc.chunk * 4, 4);
    if ((bits & 0x7F800000u) == 0x7F800000u) return false;  // not finite (no float compare: NaN traps on the Allegrex)
    std::memcpy(&level, &bits, 4);
    return true;
}

// --- UiSoundManager ---------------------------------------------------------
void WorldSounds::playUi(UiSound s) {
    if (s >= UiSound::Count) return;
    play2D(uiClip_[static_cast<int>(s)], 0.7f);  // upstream 0.7 * volumeScale
}

bool WorldSounds::playNpcVoice(uint32_t display,uint8_t kind,const glm::vec3& pos) {
    if(kind>=4)return false;
    const auto it=std::lower_bound(npcVoices_.begin(),npcVoices_.end(),display,[](const auto& row,uint32_t id){return row.display<id;});
    return it!=npcVoices_.end() && it->display==display && it->sound[kind] && playEntry(it->sound[kind],&pos);
}

bool WorldSounds::playByName(const char* name) {
    if (!name || !*name || !entries_) return false;
    auto it = entryByName_.find(fmt::soundNameHash(name));
    if (it == entryByName_.end()) return false;
    const auto* h = static_cast<const fmt::SoundEntriesHeader*>(entries_);
    const auto* base = static_cast<const uint8_t*>(entries_);
    const auto& row = reinterpret_cast<const fmt::SoundEntryRow*>(base + h->rowOffset)[it->second];
    const auto* files = reinterpret_cast<const uint32_t*>(base + h->fileOffset);
    // upstream: the first of the row's files that loads.
    for (uint32_t f = 0; f < row.fileCount; ++f) {
        const int c = bank_.find(files[row.firstFile + f]);
        if (c >= 0) return play2D(c, 0.7f);
    }
    return false;
}

std::vector<int> WorldSounds::resolveEntry(uint32_t id) const {
    std::vector<int> clips;
    const auto it=entryById_.find(id);
    if (!entries_ || it==entryById_.end()) return clips;
    const auto* h=static_cast<const fmt::SoundEntriesHeader*>(entries_);
    const auto* base=static_cast<const uint8_t*>(entries_);
    const auto& row=reinterpret_cast<const fmt::SoundEntryRow*>(base+h->rowOffset)[it->second];
    const auto* files=reinterpret_cast<const uint32_t*>(base+h->fileOffset);
    for (uint32_t i=0;i<row.fileCount;++i) {
        const int clip=bank_.find(files[row.firstFile+i]);
        if (clip>=0 && std::find(clips.begin(),clips.end(),clip)==clips.end()) clips.push_back(clip);
    }
    return clips;
}

bool WorldSounds::playEntry(uint32_t id, const glm::vec3* pos, uint32_t tag, bool sustain) {
    if (!entries_) return false;
    auto it = entryById_.find(id);
    if (it == entryById_.end()) return false;
    const auto* h = static_cast<const fmt::SoundEntriesHeader*>(entries_);
    const auto* base = static_cast<const uint8_t*>(entries_);
    const auto& row = reinterpret_cast<const fmt::SoundEntryRow*>(base + h->rowOffset)[it->second];
    if (row.fileCount == 0) return false;
    const auto* files = reinterpret_cast<const uint32_t*>(base + h->fileOffset);
    // PSP: a random file of the row (the client weighs them by Freq).
    const int c = bank_.find(files[row.firstFile + xorshift(rng_) % row.fileCount]);
    if (c < 0) return false;
    const float vol = row.volume > 0.0f ? std::min(row.volume, 1.0f) : 1.0f;
    if (!pos) return bank_.play(c,vol,0,1.0f,tag,sustain);
    float gain = 0.0f, pan = 0.0f;
    const float near=std::isfinite(row.minDistance) && row.minDistance>0.f ? row.minDistance : 1.f;
    const float far=std::isfinite(row.distanceCutoff) && row.distanceCutoff>near ? row.distanceCutoff : std::max(100.f,near);
    if (!spatial(*pos, far, gain, pan, near)) return false;
    if (++playLogs_ <= 60)
        PSP_LOGI(pp::tag::Sys, "sfx_play id=%08x vol=%.2f pan=%.2f", static_cast<unsigned>(bank_.clipHash(c)),
                 vol * gain, pan);
    return bank_.play(c, vol * gain, pan, 1.0f,tag,sustain);
}

// --- CombatSoundManager + the attacker-state sound block ---------------------
void WorldSounds::playImpact(Impact type, bool crit) {
    const std::vector<int>* normal = nullptr;
    int critClip = -1;
    switch (type) {
        case kFlesh: normal = &hitFlesh_; critClip = critFlesh_; break;
        case kChain: normal = &hitChain_; critClip = critChain_; break;
        case kPlate: normal = &hitPlate_; critClip = critPlate_; break;
        case kShield: normal = &hitShield_; critClip = critShield_; break;
        case kMetalWeapon: normal = &hitMetalWeapon_; break;
        case kWood: normal = &hitWood_; break;
        case kStone: normal = &hitStone_; break;
    }
    if (!normal) return;
    // upstream: "crit version if available" = a non-empty crit library, then
    // playSound(library) plays it only when its [0] loaded.
    if (crit && (type == kFlesh || type == kChain || type == kPlate || type == kShield)) {
        play2D(critClip, 0.8f * 1.2f);
        return;
    }
    play2D(pick(*normal), 0.8f);
}

void WorldSounds::onMeleeHit(bool playerAttacker, bool playerTarget, uint32_t hitInfo, uint32_t victimState) {
    if (!active() || (!playerAttacker && !playerTarget)) return;  // upstream: not our combat
    const bool miss = (hitInfo & 0x0010) != 0, crit = (hitInfo & 0x0200) != 0;
    const bool avoided = victimState == 1 || victimState == 2;  // dodge / parry
    // PSP: upstream AnimationController::triggerMeleeSwing plays
    // ActivitySoundManager::playMeleeSwing on our own swing; the swing is
    // started by this same event here (WorldScreen::processMeleeHits).
    if (playerAttacker) playMeleeSwing();
    constexpr int kMedium = 1;  // upstream: weaponSize = MEDIUM always
    if (miss) {
        play2D(miss1H_, 0.8f);  // playWeaponMiss(false)
    } else if (avoided) {
        play2D(pick(swing_[kMedium]), 0.8f);  // swing whoosh, no impact
    } else {
        if (crit) play2D(swingCrit_[kMedium], 0.8f);
        else play2D(pick(swing_[kMedium]), 0.8f);
        playImpact(kFlesh, crit);
    }
    if (playerAttacker && !miss && !avoided) playAttackGrunt();
    if (playerTarget && !miss && !avoided) playWound(crit);
}

// --- ActivitySoundManager ---------------------------------------------------
void WorldSounds::playLanding(StepSurface surface, bool hardLanding) {
    const uint32_t now = pp::logUptimeMs();
    if (!elapsed(lastLandMs_, 0.10f, now)) return;
    const auto& clips = landing_[static_cast<int>(surface)];
    if (!clips.empty()) {
        const float base = hardLanding ? 1.00f : 0.82f;
        play2D(pick(clips), rnd(base * 0.95f, base * 1.05f), rnd(0.95f, 1.03f));
        lastLandMs_ = now;
    }
    // Voice pain is driven by a server-confirmed damage event, never by landing alone.
}

void WorldSounds::playMeleeSwing() {
    const uint32_t now = pp::logUptimeMs();
    if (meleeSwing_.empty() || !elapsed(lastMeleeSwingMs_, 0.12f, now)) return;
    if (play2D(pick(meleeSwing_), rnd(0.76f, 0.84f), rnd(0.96f, 1.04f))) lastMeleeSwingMs_ = now;
}

void WorldSounds::playAttackGrunt() {
    const uint32_t now = pp::logUptimeMs();
    if (attackGrunt_.empty() || !elapsed(lastGruntMs_, 1.5f, now)) return;
    if (xorshift(rng_) % 10u > 2u) return;  // ~30 % of swings
    if (play2D(pick(attackGrunt_), rnd(0.55f, 0.70f), rnd(0.96f, 1.04f))) lastGruntMs_ = now;
}

void WorldSounds::playDeath() {
    if(active() && !deathVoice_.empty())play2D(pick(deathVoice_),1.f,1.f);
}

void WorldSounds::playWound(bool crit) {
    const auto& clips = (crit && !woundCrit_.empty()) ? woundCrit_ : wound_;
    const uint32_t now = pp::logUptimeMs();
    if (clips.empty() || !elapsed(lastWoundMs_, 0.8f, now)) return;
    if (play2D(pick(clips), crit ? 0.80f : 0.65f, rnd(0.96f, 1.04f))) lastWoundMs_ = now;
}

// SfxStateDriver::update + ActivitySoundManager::update (swim strokes).
void WorldSounds::updateActivity(float dtSec, bool grounded, float verticalVelocity, bool swimming, bool moving,
                                 StepSurface surface) {
    if (!active()) return;
    const bool falling = !grounded && !swimming && verticalVelocity <= 0.0f;  // isFalling
    if (!actInit_) {
        prevGrounded_ = grounded;
        prevFalling_ = falling;
        prevSwimming_ = swimming;
        actInit_ = true;
    }
    constexpr float kWaterExitSfxSuppress = 0.45f;
    if (!swimming && prevSwimming_) sinceWaterExit_ = 0.0f;
    else if (sinceWaterExit_ < kWaterExitSfxSuppress) sinceWaterExit_ += dtSec;
    const bool justLeftWater = sinceWaterExit_ < kWaterExitSfxSuppress;
    if (grounded && !prevGrounded_ && !justLeftWater) playLanding(surface, prevFalling_);
    const uint32_t now = pp::logUptimeMs();
    if ((swimming && !prevSwimming_) || (!swimming && prevSwimming_)) {  // playWaterEnter / playWaterExit
        if (elapsed(lastSplashMs_, 0.20f, now) && play2D(pick(splash_), 0.95f, rnd(0.95f, 1.05f))) lastSplashMs_ = now;
    }
    if (swimming && moving && !swimStroke_.empty()) {
        if (lastStrokeMs_ == 0 || now - lastStrokeMs_ >= 800u) {
            play2D(pick(swimStroke_), 0.6f);
            lastStrokeMs_ = now;
        }
    } else if (!swimming) {
        lastStrokeMs_ = 0;
    }
    prevGrounded_ = grounded;
    prevFalling_ = falling;
    prevSwimming_ = swimming;
}

// --- FootstepManager / FootstepDriver -----------------------------------------
bool WorldSounds::Tracker::trigger(uint32_t animationId, float animationTimeMs, float animationDurationMs,
                                   const uint32_t* ev, uint32_t n, bool genericBeats) {
    if (!(animationDurationMs > 1.0f)) {
        initialized = false;
        return false;
    }
    float wrappedTime = std::fmod(animationTimeMs, animationDurationMs);  // upstream: a subtraction loop
    if (wrappedTime < 0.0f) wrappedTime += animationDurationMs;
    const float norm = wrappedTime / animationDurationMs;
    if (animationId != lastAnimationId) {
        lastAnimationId = animationId;
        lastNormTime = norm;
        initialized = true;
        return false;
    }
    if (!initialized) {
        initialized = true;
        lastNormTime = norm;
        return false;
    }
    auto crossed = [&](float eventNorm) {
        if (lastNormTime <= norm) return lastNormTime < eventNorm && eventNorm <= norm;
        return lastNormTime < eventNorm || eventNorm <= norm;
    };
    bool fire = false;
    if (ev && n) {
        for (uint32_t i = 0; i < n && !fire; ++i)
            fire = crossed(std::min(static_cast<float>(ev[i]) / animationDurationMs, 0.999f));
    } else if (genericBeats) {
        fire = crossed(0.22f) || crossed(0.72f);
    }
    lastNormTime = norm;
    return fire;
}

void WorldSounds::playRandomStep(StepSurface surface, float minInterval, float volumeMul) {
    const uint32_t now = pp::logUptimeMs();
    if (!elapsed(lastStepMs_, minInterval, now)) return;
    const std::vector<int>* list = &steps_[static_cast<int>(surface)];
    if (list->empty()) list = &steps_[0];
    if (list->empty()) return;
    const float pitch = rnd(0.97f, 1.05f);
    const float volume = std::clamp(rnd(0.92f, 1.00f) * volumeMul, 0.1f, 1.0f);
    if (play2D(pick(*list), volume, pitch, kOwnStepTag)) {
        lastStepMs_ = now;
        ++stepsOwn_;
    }
}

void WorldSounds::playWaterFootstep(int size) {
    static const float kMul[3] = {0.6f, 0.8f, 1.0f};
    const int i = std::clamp(size, 0, 2);
    play2D(pick(waterStep_[i]), 0.7f * kMul[i], 1.f, kOwnStepTag);
}

void WorldSounds::ownFootsteps(bool canPlay, bool footstepState, uint32_t animId, float timeMs, float durationMs,
                               const uint32_t* events, uint32_t eventCount, bool sprinting, StepSurface surface) {
    if (!active()) return;
    if (footsteps_==0 || !(canPlay && footstepState)) {
        bank_.cancelTag(kOwnStepTag);
        own_.initialized = false;
        return;
    }
    if (!own_.trigger(animId, timeMs, durationMs, events, eventCount, true)) return;
    playRandomStep(surface, sprinting ? 0.09f : 0.14f, sprinting ? 1.0f : 0.88f);
    if (surface == StepSurface::Water) playWaterFootstep(1);  // playWaterStepExtras: MEDIUM
}

bool WorldSounds::otherStepDue(const Walker& w) {
    Tracker& t = others_[w.guid];
    t.seenFrame = frame_;
    if (!w.moving) {
        t.initialized = false;
        return false;
    }
    // PSP: only authored $FSD times for others (the biped beats would be
    // wrong for four legs, and a guess per unit is not worth a voice).
    return t.trigger(w.animId, w.timeMs, w.durationMs, w.events, w.eventCount, false);
}

void WorldSounds::playOtherStep(const Walker& w, StepSurface surface) {
    // PSP: upstream plays footsteps of the own character only. Others' go
    // through the same banks, 3D (1/d) within otherDistance, at most one
    // every 50 ms in all; the mixer steals the quietest voice first.
    const uint32_t now = pp::logUptimeMs();
    if (now - lastOtherStepMs_ < 50u) return;
    float gain = 0.0f, pan = 0.0f;
    if (!spatial(w.pos, otherDistance, gain, pan)) return;
    const std::vector<int>* list = &steps_[static_cast<int>(surface)];
    if (list->empty()) list = &steps_[0];
    const int c = pick(*list);
    if (c < 0) return;
    const float volume = std::clamp(rnd(0.92f, 1.00f) * 0.88f, 0.1f, 1.0f) * gain;
    if (bank_.play(c, volume, pan, rnd(0.97f, 1.05f))) {
        lastOtherStepMs_ = now;
        if (w.player) ++stepsOther_;
        else ++stepsNpc_;
    }
}

void WorldSounds::sweepTrackers() {
    if ((frame_ & 255u) != 0) return;
    for (auto it = others_.begin(); it != others_.end();) {
        if (frame_ - it->second.seenFrame > 600u) it = others_.erase(it);
        else ++it;
    }
}

// --- WorldScreen glue (hooks) ------------------------------------------------
void WorldScreen::initSounds() {
    uint32_t race = 1;
    bool male = true;
    for (const auto& c : session_.characters()) {
        if (c.name != name_) continue;
        race = static_cast<uint32_t>(c.race);
        male = !(c.gender == game::Gender::FEMALE || (c.gender == game::Gender::NONBINARY && c.useFemaleModel));
    }
    const uint32_t bytes=session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
    if ((bytes & 0xFFu)!=0) { race=bytes & 0xFFu; male=((bytes>>16)&0xFFu)==0; }
    sounds_.init(session_.worldEntry().mapId, race, male);
}

// Upstream FootstepDriver::resolveFootstepSurface for the own character:
// cached while within 1.5 yd for 0.5 s; water; standing on a building (WMO
// floor chosen by floorAt) -> STONE; else the ground layer's surface.
StepSurface WorldScreen::soundSurface(float dtSec) {
    sfxSurfaceTimer_ += dtSec;
    const glm::vec3 p(playerX_, playerY_, playerZ_);
    const glm::vec3 d = p - sfxSurfacePos_;
    if (glm::dot(d, d) < 2.25f && sfxSurfaceTimer_ < 0.5f) return sfxSurface_;
    sfxSurfacePos_ = p;
    sfxSurfaceTimer_ = 0.0f;
    StepSurface s = StepSurface::Stone;
    StepSurface terrainSurf = StepSurface::Stone;
    const bool haveGrid = sounds_.surfaceAt(p.x, p.y, p.z, terrainSurf);
    if (haveGrid && terrainSurf == StepSurface::Water) {
        s = StepSurface::Water;
    } else {
        bool onTerrain = true;
        const float floor = floorAt(p.x, p.y, p.z + 1.5f, &onTerrain);
        const bool standingOnWmo = !onTerrain && floor > -1e29f && std::fabs(floor - p.z) <= 2.5f;
        s = standingOnWmo ? StepSurface::Stone : (haveGrid ? terrainSurf : StepSurface::Stone);
    }
    sfxSurface_ = s;
    return s;
}

void WorldScreen::updateSounds(float dtSec) {
    pp::WorldSession::ServerSound eventSound;
    while(session_.takeServerSound(eventSound)){
        glm::vec3 at;float height=0;
        const bool positional=eventSound.source && unitPosition(eventSound.source,at,height);
        const bool played=sounds_.playEntry(eventSound.id,positional?&at:nullptr);
        PSP_LOGI(pp::tag::Sys,"server_sound entry=%u source=%u played=%d",static_cast<unsigned>(eventSound.id),
            static_cast<unsigned>(eventSound.source),played?1:0);
    }
    // UI sounds the session queued (loot, inventory, quests).
    using SS = pp::WorldSession::SessionSound;
    SS ss;
    while (session_.takeSound(ss)) {
        switch (ss) {
            case SS::LootCoinSmall: playUiSound(UiSound::LootCoinSmall); break;
            case SS::LootCoinLarge: playUiSound(UiSound::LootCoinLarge); break;
            case SS::LootItem: playUiSound(UiSound::LootItem); break;
            case SS::Error: playUiSound(UiSound::Error); break;
            case SS::QuestActivate: playUiSound(UiSound::QuestActivate); break;
            case SS::QuestComplete: playUiSound(UiSound::QuestComplete); break;
            case SS::QuestFailed: playUiSound(UiSound::QuestFailed); break;
            case SS::QuestUpdate: playUiSound(UiSound::QuestUpdate); break;
        }
    }
    if (!sounds_.active()) return;
    PROF_SCOPE("sfx");
    sounds_.beginFrame(dtSec, settings_.values().sfxvol, camPosForHud_, camYaw_);
    updateEnvironmentAudio();
    const StepSurface surface = soundSurface(dtSec);
    // PSP: no swimming in the port yet (upstream: CameraController water).
    sounds_.updateActivity(dtSec, !airborne_, vz_, ownSwimming(), wantMove_, surface);
    uint32_t animId = 0, n = 0;
    float t = 0.0f, dur = 0.0f;
    const uint32_t* ev = nullptr;
    const bool clock = charAnimated_ && player_.footstepClock(animId, t, dur, ev, n);
    const auto loco = animator_.getLocomotion().getState();
    const bool stepState =
        loco == rendering::LocomotionFSM::State::WALK || loco == rendering::LocomotionFSM::State::RUN;
    const glm::vec3 stepPos(playerX_,playerY_,playerZ_);
    const glm::vec3 stepDelta=stepPos-sfxStepPos_;
    sfxStepPos_=stepPos;
    const bool actuallyMoving=wantMove_ && stepDelta.x*stepDelta.x+stepDelta.y*stepDelta.y>0.000001f;
    sounds_.ownFootsteps(clock && !airborne_ && !ownSwimOrFly() && !mountActive_ && actuallyMoving, stepState, animId, t, dur, ev, n,
                         loco == rendering::LocomotionFSM::State::RUN, surface);
    if (sounds_.footstepMode() < 2) return;
    // PSP: other players and NPCs (not upstream, see playOtherStep).
    WorldSounds::Walker w[24];
    uint32_t count = 0;
    const glm::vec3 me(playerX_, playerY_, playerZ_);
    const float r2 = sounds_.otherDistance * sounds_.otherDistance;
    for (auto& [guid, r] : remotes_) {
        if (count >= 12) break;
        if (!r.model || !r.visible || r.airborne) continue;
        const glm::vec3 d = r.pos - me;
        if (!(glm::dot(d, d) <= r2)) continue;
        WorldSounds::Walker& x = w[count];
        if (!r.model->footstepClock(x.animId, x.timeMs, x.durationMs, x.events, x.eventCount)) continue;
        x.guid = guid;
        x.pos = r.pos;
        x.moving = r.wasMoving;
        x.player = true;
        ++count;
    }
    rendering::psp::CreatureRenderer::FootstepProbe probes[12];
    const uint32_t np = creaturesOn_ ? creatures_.footstepProbes(me.x, me.y, me.z, sounds_.otherDistance, probes, 12) : 0;
    for (uint32_t i = 0; i < np && count < 24; ++i) {
        WorldSounds::Walker& x = w[count++];
        x.guid = probes[i].guid;
        x.pos = glm::vec3(probes[i].x, probes[i].y, probes[i].z);
        x.animId = probes[i].animId;
        x.timeMs = probes[i].timeMs;
        x.durationMs = probes[i].durationMs;
        x.events = probes[i].events;
        x.eventCount = probes[i].eventCount;
        x.moving = probes[i].moving;
        x.player = false;
    }
    sounds_.otherFootsteps(w, count, [this](const WorldSounds::Walker& x) {
        // PSP: the terrain rules only (no building floor query per unit):
        // a unit well above the ground stands on a building -> STONE.
        StepSurface s = StepSurface::Stone;
        if (!sounds_.surfaceAt(x.pos.x, x.pos.y, x.pos.z, s)) return StepSurface::Stone;
        float tz = 0.0f;
        if (s != StepSurface::Water && terrain_.heightAt(x.pos.x, x.pos.y, tz) && x.pos.z > tz + 1.0f)
            return StepSurface::Stone;
        return s;
    });
}

void WorldScreen::onMeleeHitSound(bool mine, bool atMe, uint32_t hitInfo, uint32_t victimState) {
    sounds_.onMeleeHit(mine, atMe, hitInfo, victimState);
}

// Debug window row (ids of task F: 0x6500..): a test sound per press and the
// footsteps mode (wowee.cfg footsteps, not saved from here).
namespace {
enum : uint32_t { kIdDbgSfxTest = 0x6500, kIdDbgSfxSteps };
}

const char* WorldSounds::playTest(uint32_t n) {
    static const UiSound kUi[] = {UiSound::ButtonClick, UiSound::TargetSelect, UiSound::BagOpen,
                                  UiSound::LootCoinSmall, UiSound::QuestActivate, UiSound::LevelUp};
    constexpr uint32_t kUiCount = sizeof(kUi) / sizeof(kUi[0]);
    const uint32_t i = n % (kUiCount + 3);
    if (i < kUiCount) {
        playUi(kUi[i]);
        static const char* const kNames[] = {"click", "target", "bag", "coins", "quest", "levelup"};
        return kNames[i];
    }
    if (i == kUiCount) {
        play2D(pick(steps_[static_cast<int>(StepSurface::Grass)]), 0.9f);
        return "step_grass";
    }
    if (i == kUiCount + 1) {
        play2D(pick(swing_[1]), 0.8f);
        return "swing";
    }
    playImpact(kFlesh, false);
    return "hit";
}

bool WorldScreen::soundDebugAction(uint32_t act) {
    if (act == kIdDbgSfxTest) {
        const char* what = sounds_.playTest(sfxTestIndex_++);
        PSP_LOGI(pp::tag::Ctrl, "debug_set sfxtest=%u sound=%s active=%d", static_cast<unsigned>(sfxTestIndex_), what,
                 sounds_.active() ? 1 : 0);
        return true;
    }
    if (act == kIdDbgSfxSteps) {
        sounds_.setFootstepMode((sounds_.footstepMode() + 1) % 3);
        PSP_LOGI(pp::tag::Ctrl, "debug_set footsteps=%d", sounds_.footstepMode());
        return true;
    }
    return false;
}

void WorldScreen::drawSoundDebugRow(UiRenderer& ui, float c0, float c1, float bw, float bh, float y) {
    // own strings (ps_b7_world_hud.inc SND_*).
    std::string line = sounds_.active()
                           ? OWN_FMT(SND_TEST_FMT, static_cast<unsigned>(sounds_.bankStats().residentKb))
                           : std::string(OWN_STR(SND_NO_PAK));
    kit_.button(ui, winFocus_, kIdDbgSfxTest, Rect{c0, y, bw, bh}, line.c_str(), sounds_.active());
    static constexpr pp::PS kSteps[] = {pp::PS::PORT_OFF_LC, pp::PS::SND_STEPS_OWN, pp::PS::SND_STEPS_ALL};
    line = OWN_FMT(SND_STEPS_FMT, pp::ps(kSteps[sounds_.footstepMode()]));
    kit_.button(ui, winFocus_, kIdDbgSfxSteps, Rect{c1, y, bw, bh}, line.c_str());
}

} // namespace wowee::ui::psp
