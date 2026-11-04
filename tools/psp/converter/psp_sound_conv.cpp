// psp_sound_conv - sound effects for the PSP (docs/PSP_PORT_ARCHITECTURE.md).
//
//   psp_sound_conv --extracted <dir> --out <psp/bin/data/sounds.pak>
//                  [--map <map>]... [--all-maps] [--rate 22050] [--stereo] [--max-sec 12] [--trim-long]
//                  [--entry-prefix "Sound\Interface\"]... [--no-entries]
//                  [--max-pak-mb 1024] [--verify] [--dump <dir>] [--list-missing]
//   psp_sound_conv --verify-pak <sounds.pak>      (check a baked pak, and its parts)
//
// What goes in (format: include/rendering/psp/psp_sound_format.hpp):
//  - every file the ported sound managers ask for
//    (include/ui/psp/ui_world_sound_paths.hpp: upstream UiSoundManager,
//    CombatSoundManager, ActivitySoundManager for all races and sexes,
//    MovementSoundManager, audio/footstep_paths.hpp), when extracted;
//  - SoundEntries.dbc rows with a .wav under an --entry-prefix (default
//    Sound\Interface\ = FrameXML's PlaySound names) or naming one of the files
//    above, and those files. Upstream plays WAV only (AudioEngine::playSound2D
//    answers false for the mp3 rows), so .mp3 files are not baked;
//  - "g_<map>_<x>_<y>": the footstep surface of every terrain cell of every
//    ADT tile of each --map (upstream TerrainManager::getDominantTextureAt +
//    FootstepDriver's texture-name tokens) and the chunk's MH2O height.
// Samples are decoded and resampled by miniaudio (what upstream's
// AudioEngine plays them with) to s16 mono at --rate (22050 by default: the
// client effects are 22.05 kHz or 44.1 kHz recordings of short transients; the
// PSP mixer resamples to 44.1 kHz; half the RAM of 44.1 kHz; --rate 0 keeps
// each file's own rate). A set over --max-pak-mb is split into
// sounds.pak, sounds_1.pak, ... (FAT32: a file < 4 GB; pakRead: offsets < 2 GB).
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#include "miniaudio.h"

#include "pipeline/adt_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/psp/psp_sound_format.hpp"
#include "ui/psp/ui_world_sound_paths.hpp"

#include "conv_common.hpp"
#include "sound_bake.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace wowee;
using namespace wowee::pspconv;
namespace fs = std::filesystem;
namespace fmt = wowee::rendering::psp;

namespace {

std::string g_root;
uint32_t g_rate = 22050;
bool g_stereo = false;
float g_maxSec = 12.0f;
bool g_trimLong = false;

struct Clip {
    std::string path;  // normalised (lower, '/')
    uint32_t hash = 0;
    uint32_t rate = 0, frames = 0;
    uint8_t channels = 1;
    std::vector<uint8_t> blob;  // SoundClipHeader + PCM
};

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

// miniaudio: any WAV (PCM 8/16/24/32, float, ADPCM) -> s16 at g_rate.
bool decode(const std::string& path, Clip& c, std::string& why) {
    std::vector<uint8_t> bytes;
    if (!readFile(g_root + "/" + path, bytes) || bytes.empty()) {
        why = "unreadable";
        return false;
    }
    const bool ambience=startsWith(path,"sound/ambience/");
    const bool horseman=startsWith(path,"sound/creature/headlesshorseman/");
    const ma_uint32 ch = ambience || horseman ? 1 : g_stereo ? 2 : 1;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, ch, ambience || horseman ? 11025 : g_rate);
    cfg.resampling.linear.lpfOrder = 8;  // anti-aliasing when going down to 22.05 kHz
    ma_decoder dec;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) != MA_SUCCESS) {
        why = "not decodable";
        return false;
    }
    ma_format f;
    ma_uint32 outCh = 0, outRate = 0;
    ma_decoder_get_data_format(&dec, &f, &outCh, &outRate, nullptr, 0);
    std::vector<int16_t> pcm;
    int16_t buf[4096 * 2];
    for (;;) {
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
        pcm.insert(pcm.end(), buf, buf + got * outCh);
        if (r != MA_SUCCESS || got < 4096) break;
    }
    ma_decoder_uninit(&dec);
    uint32_t frames = static_cast<uint32_t>(pcm.size() / outCh);
    if (frames == 0 || outRate == 0) {
        why = "empty";
        return false;
    }
    const float clipLimit=ambience ? std::min(g_maxSec,6.0f) : horseman ? std::max(g_maxSec,30.f) : g_maxSec;
    if (static_cast<float>(frames) / static_cast<float>(outRate) > clipLimit) {
        if (!g_trimLong && !ambience) {
            why = "longer than --max-sec";
            return false;
        }
        frames = std::max(1u,static_cast<uint32_t>(clipLimit * outRate));
        pcm.resize(static_cast<size_t>(frames) * outCh);
        // Fade the cut rather than ending the waveform abruptly.
        const uint32_t fade = std::min(frames,std::max(1u,outRate / 20u));
        for (uint32_t i=0;!ambience && i<fade;++i)
            for (uint32_t channel=0;channel<outCh;++channel) {
                auto& sample=pcm[(static_cast<size_t>(frames-fade+i) * outCh)+channel];
                sample=static_cast<int16_t>(static_cast<int32_t>(sample) * (fade-1-i) / fade);
            }
        std::printf("  trimmed %s to %.1f sec (PSP clip limit)\n",path.c_str(),g_maxSec);
    }
    c.rate = outRate;
    c.frames = frames;
    c.channels = static_cast<uint8_t>(outCh);
    fmt::SoundClipHeader h{};
    std::memcpy(h.magic, "WSND", 4);
    h.version = fmt::kSoundFormatVersion;
    h.channels = c.channels;
    h.rate = outRate;
    h.frames = frames;
    c.blob.resize(sizeof(h) + pcm.size() * 2);
    std::memcpy(c.blob.data(), &h, sizeof(h));
    std::memcpy(c.blob.data() + sizeof(h), pcm.data(), pcm.size() * 2);
    return true;
}

struct PakRead {
    std::vector<uint8_t> file;
    struct Entry {
        std::string name;
        uint32_t offset, size, kind;
    };
    std::vector<Entry> toc;
    bool load(const std::string& path) {
        if (!readFile(path, file) || file.size() < 16 || std::memcmp(file.data(), "WPAK", 4) != 0) return false;
        uint32_t count = 0, tocOff = 0;
        std::memcpy(&count, file.data() + 8, 4);
        std::memcpy(&tocOff, file.data() + 12, 4);
        if (tocOff + static_cast<uint64_t>(count) * 64 > file.size()) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* p = file.data() + tocOff + i * 64;
            Entry e;
            e.name.assign(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), 48));
            std::memcpy(&e.offset, p + 48, 4);
            std::memcpy(&e.size, p + 52, 4);
            std::memcpy(&e.kind, p + 56, 4);
            if (e.offset + static_cast<uint64_t>(e.size) > file.size()) return false;
            toc.push_back(e);
        }
        return true;
    }
};

std::string partPath(const std::string& out, int k) {
    if (k == 0) return out;
    const std::string stem = out.size() > 4 && out.compare(out.size() - 4, 4, ".pak") == 0 ? out.substr(0, out.size() - 4) : out;
    return stem + "_" + std::to_string(k) + ".pak";
}

// Reads the pak (and its parts) back the way the PSP does and checks it.
int verifyPaks(const std::string& out, const std::string& dumpDir) {
    int errors = 0;
    std::set<uint32_t> hashes;
    uint64_t pcmBytes = 0;
    double seconds = 0.0;
    size_t clips = 0, grids = 0;
    std::vector<uint8_t> entries;
    FILE* dump = dumpDir.empty() ? nullptr : std::fopen((dumpDir + "/sounds_dump.txt").c_str(), "w");
    for (int k = 0; k < 8; ++k) {
        const std::string p = partPath(out, k);
        if (k > 0 && !fs::exists(p)) break;
        PakRead pak;
        if (!pak.load(p)) {
            std::fprintf(stderr, "verify: %s unreadable or not a WPAK\n", p.c_str());
            return 3;
        }
        if (fs::file_size(p) >= (2ull << 30)) {
            std::fprintf(stderr, "verify: %s is >= 2 GB (pakRead offsets are 31-bit)\n", p.c_str());
            ++errors;
        }
        for (const auto& e : pak.toc) {
            const uint8_t* b = pak.file.data() + e.offset;
            unsigned h = 0;
            if (e.name.size() == 10 && startsWith(e.name, "s_") && std::sscanf(e.name.c_str() + 2, "%8x", &h) == 1) {
                fmt::SoundClipHeader ch;
                if (e.size < sizeof(ch)) {
                    ++errors;
                    continue;
                }
                std::memcpy(&ch, b, sizeof(ch));
                const uint64_t want = sizeof(ch) + static_cast<uint64_t>(ch.frames) * 2u * ch.channels;
                if (std::memcmp(ch.magic, "WSND", 4) != 0 || ch.version != fmt::kSoundFormatVersion ||
                    (ch.channels != 1 && ch.channels != 2) || ch.rate < 4000 || ch.rate > 96000 || want != e.size) {
                    std::fprintf(stderr, "verify: %s bad clip header\n", e.name.c_str());
                    ++errors;
                    continue;
                }
                if (!hashes.insert(h).second) {
                    std::fprintf(stderr, "verify: %s twice\n", e.name.c_str());
                    ++errors;
                }
                ++clips;
                if (dump)
                    std::fprintf(dump, "clip %s part %d %u Hz %u ch %u frames\n", e.name.c_str(), k, ch.rate, ch.channels,
                                 ch.frames);
                pcmBytes += e.size - sizeof(ch);
                seconds += static_cast<double>(ch.frames) / ch.rate;
                if (!dumpDir.empty()) {
                    // A WAV of what the PSP plays, to listen to.
                    std::vector<uint8_t> wav;
                    const uint32_t data = e.size - static_cast<uint32_t>(sizeof(ch));
                    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
                    put<uint32_t>(wav, 36 + data);
                    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
                    put<uint32_t>(wav, 16);
                    put<uint16_t>(wav, 1);
                    put<uint16_t>(wav, ch.channels);
                    put<uint32_t>(wav, ch.rate);
                    put<uint32_t>(wav, ch.rate * 2u * ch.channels);
                    put<uint16_t>(wav, static_cast<uint16_t>(2 * ch.channels));
                    put<uint16_t>(wav, 16);
                    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
                    put<uint32_t>(wav, data);
                    wav.insert(wav.end(), b + sizeof(ch), b + e.size);
                    std::ofstream(dumpDir + "/" + e.name + ".wav", std::ios::binary)
                        .write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
                }
            } else if (e.name == "entries") {
                if (k != 0) ++errors;
                entries.assign(b, b + e.size);
            } else if (startsWith(e.name, "g_")) {
                if (k != 0 || !fmt::surfaceGridValid(b, e.size)) {
                    std::fprintf(stderr, "verify: %s bad grid\n", e.name.c_str());
                    ++errors;
                }
                ++grids;
            }
        }
    }
    size_t rows = 0, rowFiles = 0, missingFiles = 0;
    if (!entries.empty()) {
        fmt::SoundEntriesHeader h;
        std::memcpy(&h, entries.data(), sizeof(h));
        if (std::memcmp(h.magic, "WSEN", 4) != 0 || h.rowOffset + h.rowCount * sizeof(fmt::SoundEntryRow) > entries.size() ||
            h.fileOffset + h.fileCount * 4ull > entries.size()) {
            std::fprintf(stderr, "verify: entries table bad\n");
            ++errors;
        } else {
            rows = h.rowCount;
            for (uint32_t i = 0; i < h.rowCount; ++i) {
                fmt::SoundEntryRow r;
                std::memcpy(&r, entries.data() + h.rowOffset + i * sizeof(r), sizeof(r));
                if (r.firstFile + static_cast<uint32_t>(r.fileCount) > h.fileCount || r.fileCount == 0) {
                    ++errors;
                    continue;
                }
                for (uint32_t f = 0; f < r.fileCount; ++f) {
                    uint32_t fh = 0;
                    std::memcpy(&fh, entries.data() + h.fileOffset + (r.firstFile + f) * 4u, 4);
                    ++rowFiles;
                    if (!hashes.count(fh)) ++missingFiles;
                }
                if (dump)
                    std::fprintf(dump, "entry %u name %08x files %u vol %.2f dist %.1f-%.1f type %u\n", r.id, r.nameHash,
                                 r.fileCount, r.volume, r.minDistance, r.distanceCutoff, r.soundType);
            }
            if (missingFiles) {
                std::fprintf(stderr, "verify: %zu entry files not in the pak\n", missingFiles);
                ++errors;
            }
        }
    }
    // The fixed paths the PSP resolves: how many it will find.
    size_t fixedFound = 0, fixedTotal = 0;
    std::set<std::string> seen;
    for (const std::string& p : ui::psp::sound_paths::all()) {
        if (!seen.insert(lower(p)).second) continue;
        ++fixedTotal;
        if (hashes.count(fmt::soundPathHash(p.c_str()))) ++fixedFound;
    }
    if (dump) std::fclose(dump);
    std::printf("verify: %zu clips, %.1f MB PCM, %.0f s; entries %zu rows (%zu files); grids %zu; "
                "manager paths %zu of %zu present; errors %d\n",
                clips, pcmBytes / 1048576.0, seconds, rows, rowFiles, grids, fixedFound, fixedTotal, errors);
    return errors ? 3 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string out, dumpDir, verifyPak;
    std::vector<std::string> maps, prefixes;
    bool entriesOn = true, verify = false, listMissing = false, allMaps = false;
    uint64_t maxPakMb = 1024;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--extracted" && i + 1 < argc) g_root = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--map" && i + 1 < argc) maps.push_back(argv[++i]);
        else if (a == "--all-maps") allMaps = true;
        else if (a == "--rate" && i + 1 < argc) g_rate = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--stereo") g_stereo = true;
        else if (a == "--trim-long") g_trimLong = true;
        else if (a == "--max-sec" && i + 1 < argc) g_maxSec = std::stof(argv[++i]);
        else if (a == "--entry-prefix" && i + 1 < argc) prefixes.push_back(lower(argv[++i]));
        else if (a == "--no-entries") entriesOn = false;
        else if (a == "--max-pak-mb" && i + 1 < argc) maxPakMb = std::stoull(argv[++i]);
        else if (a == "--verify") verify = true;
        else if (a == "--dump" && i + 1 < argc) dumpDir = argv[++i];
        else if (a == "--list-missing") listMissing = true;
        else if (a == "--verify-pak" && i + 1 < argc) verifyPak = argv[++i];
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (!dumpDir.empty()) fs::create_directories(dumpDir);
    if (!verifyPak.empty()) return verifyPaks(verifyPak, dumpDir);
    if (g_root.empty() || out.empty()) {
        std::fprintf(stderr, "usage: psp_sound_conv --extracted <dir> --out <sounds.pak> [--map Azeroth]... [--all-maps] "
                             "[--rate 22050] [--stereo] [--max-sec 12] [--trim-long] [--entry-prefix \"Sound\\Interface\\\"]... "
                             "[--no-entries] [--max-pak-mb 1024] [--verify] [--dump <dir>] [--list-missing]\n"
                             "       psp_sound_conv --verify-pak <sounds.pak> [--dump <dir>]\n");
        return 2;
    }
    if (prefixes.empty()) {
        prefixes.push_back(lower("Sound\\Interface\\"));
        prefixes.push_back(lower("Sound\\Spells\\"));
        prefixes.push_back(lower("Sound\\Creature\\HeadlessHorseman\\"));
    }
    maxPakMb = std::clamp<uint64_t>(maxPakMb, 16, 1900);

    // 1. The managers' files (upstream lists), in their order, once each.
    std::vector<std::string> want;
    std::set<std::string> wanted;
    size_t fixedTotal = 0, fixedMissing = 0;
    for (const std::string& p : ui::psp::sound_paths::all()) {
        const std::string n = lower(p);
        if (!wanted.insert(n).second) continue;
        ++fixedTotal;
        std::error_code ec;
        if (!fs::exists(g_root + "/" + n, ec)) {
            ++fixedMissing;
            if (listMissing) std::printf("missing %s\n", p.c_str());
            continue;
        }
        want.push_back(n);
    }
    const std::set<std::string> fixedSet(want.begin(), want.end());

    // 2. SoundEntries rows (upstream UiSoundManager::ensureSoundEntriesLoaded
    // reading: 2 Name, 3..12 File, 23 DirectoryBase; + 24 Volume, 25 Flags,
    // 26 MinDistance, 27 DistanceCutoff).
    struct RowIn {
        fmt::SoundEntryRow row;
        std::vector<std::string> files;
    };
    std::vector<RowIn> rowsIn;
    // Some spell kits reference creature or item directories rather than Sound/Spells.
    // Select those authored entries too, without baking every SoundEntries row.
    std::set<uint32_t> spellSounds;
    pipeline::DBCFile spellKits;
    std::vector<uint8_t> kitBytes;
    if (readFile(g_root + "/dbfilesclient/spellvisualkit.dbc",kitBytes) && spellKits.load(kitBytes) && spellKits.getFieldCount()>15)
        for (uint32_t i=0;i<spellKits.getRecordCount();++i)
            if (const uint32_t id=spellKits.getUInt32(i,15)) spellSounds.insert(id);
    std::vector<fmt::CharacterVoice> voiceRows;
    pipeline::DBCFile races, displays, models, creatureSounds;
    auto loadVoiceDbc=[&](const char* name,pipeline::DBCFile& dbc) {
        std::vector<uint8_t> data;
        return readFile(g_root+"/dbfilesclient/"+name,data) && dbc.load(data);
    };
    if (loadVoiceDbc("chrraces.dbc",races) && loadVoiceDbc("creaturedisplayinfo.dbc",displays) &&
        loadVoiceDbc("creaturemodeldata.dbc",models) && loadVoiceDbc("creaturesounddata.dbc",creatureSounds) &&
        races.getFieldCount()>5 && displays.getFieldCount()>2 && models.getFieldCount()>13 && creatureSounds.getFieldCount()>27) {
        for (uint32_t i=0;i<races.getRecordCount();++i) for (uint32_t sex=0;sex<2;++sex) {
            const int32_t di=displays.findRecordById(races.getUInt32(i,4+sex));
            if (di<0) continue;
            uint32_t soundSet=displays.getUInt32(di,2);
            if (!soundSet) {
                const int32_t mi=models.findRecordById(displays.getUInt32(di,1));
                if (mi>=0) soundSet=models.getUInt32(mi,13);
            }
            const int32_t si=creatureSounds.findRecordById(soundSet);
            if (si<0) continue;
            fmt::CharacterVoice row{races.getUInt32(i,0),sex,
                creatureSounds.getUInt32(si,1),creatureSounds.getUInt32(si,3),creatureSounds.getUInt32(si,4),
                creatureSounds.getUInt32(si,26),creatureSounds.getUInt32(si,27),creatureSounds.getUInt32(si,6)};
            voiceRows.push_back(row);
            for (uint32_t id : {row.attack,row.wound,row.critical,row.jump,row.land,row.death})
                if (id) spellSounds.insert(id);
        }
    }
    std::vector<fmt::AreaAmbience> areaAmbience;
    pipeline::DBCFile areaDb, ambienceDb;
    if (loadVoiceDbc("areatable.dbc",areaDb) && loadVoiceDbc("soundambience.dbc",ambienceDb) &&
        areaDb.getFieldCount()>7 && ambienceDb.getFieldCount()>=3) {
        for (uint32_t i=0;i<areaDb.getRecordCount();++i) {
            fmt::AreaAmbience row{areaDb.getUInt32(i,0),areaDb.getUInt32(i,2),0,0};
            const int32_t a=ambienceDb.findRecordById(areaDb.getUInt32(i,7));
            if (a>=0) { row.day=ambienceDb.getUInt32(a,1); row.night=ambienceDb.getUInt32(a,2); }
            areaAmbience.push_back(row);
            if (row.day) spellSounds.insert(row.day);
            if (row.night) spellSounds.insert(row.night);
        }
    }
    // NPC voices follow the actual display's NPCSounds set, including the
    // authored voice personality, instead of guessing a race from its name.
    std::vector<fmt::NpcVoiceRecord> npcVoices;
    pipeline::DBCFile npcDisplayDb,npcSoundDb;
    if(loadVoiceDbc("creaturedisplayinfo.dbc",npcDisplayDb) && loadVoiceDbc("npcsounds.dbc",npcSoundDb) &&
       npcDisplayDb.getFieldCount()>12 && npcSoundDb.getFieldCount()>=5) {
        for(uint32_t i=0;i<npcDisplayDb.getRecordCount();++i) {
            const int32_t sound=npcSoundDb.findRecordById(npcDisplayDb.getUInt32(i,12));
            if(sound<0)continue;
            fmt::NpcVoiceRecord row{};row.display=npcDisplayDb.getUInt32(i,0);
            for(int k=0;k<4;++k){row.sound[k]=npcSoundDb.getUInt32(sound,k+1);if(row.sound[k])spellSounds.insert(row.sound[k]);}
            npcVoices.push_back(row);
        }
    }
    size_t mp3Skipped = 0;
    if (entriesOn) {
        pipeline::DBCFile se;
        std::vector<uint8_t> bytes;
        if (!readFile(g_root + "/dbfilesclient/soundentries.dbc", bytes) || !se.load(bytes) || se.getFieldCount() < 28) {
            std::fprintf(stderr, "soundentries.dbc missing or too few fields - no entries table\n");
        } else {
            for (uint32_t r = 0; r < se.getRecordCount(); ++r) {
                const std::string name = se.getString(r, 2);
                const std::string dir = se.getString(r, 23);
                RowIn in{};
                bool match = spellSounds.count(se.getUInt32(r,0)) != 0 || startsWith(lower(dir),"sound/ambience/weather") || startsWith(lower(dir),"sound\\ambience\\weather");
                for (uint32_t f = 3; f <= 12; ++f) {
                    const std::string file = se.getString(r, f);
                    if (file.empty()) continue;
                    const std::string n = lower(dir.empty() ? file : dir + "\\" + file);
                    if (n.size() < 4 || n.compare(n.size() - 4, 4, ".wav") != 0) {
                        ++mp3Skipped;  // upstream playSound2D: WAV only
                        continue;
                    }
                    bool pre = fixedSet.count(n) != 0;
                    for (const std::string& p : prefixes) pre = pre || startsWith(n, p);
                    match = match || pre;
                    in.files.push_back(n);
                }
                if (!match || in.files.empty()) continue;
                in.row.id = se.getUInt32(r, 0);
                in.row.soundType = static_cast<uint8_t>(se.getUInt32(r, 1));
                in.row.nameHash = fmt::soundNameHash(name.c_str());
                in.row.volume = se.getFloat(r, 24);
                in.row.flags = se.getUInt32(r, 25);
                in.row.minDistance = se.getFloat(r, 26);
                in.row.distanceCutoff = se.getFloat(r, 27);
                for (const std::string& n : in.files)
                    if (wanted.insert(n).second) want.push_back(n);
                rowsIn.push_back(std::move(in));
            }
        }
    }

    // 3. Decode.
    std::vector<Clip> clips;
    std::map<uint32_t, std::string> byHash;
    size_t absent = 0, failed = 0;
    uint64_t pcmBytes = 0;
    double seconds = 0.0;
    for (const std::string& n : want) {
        std::error_code ec;
        if (!fs::exists(g_root + "/" + n, ec)) {
            ++absent;
            if(listMissing)std::printf("missing %s\n",n.c_str()); // include DBC-selected NPC/spell clips in Kiln's extraction probe
            continue;
        }
        Clip c;
        c.path = n;
        c.hash = fmt::soundPathHash(n.c_str());
        auto it = byHash.find(c.hash);
        if (it != byHash.end()) {
            std::fprintf(stderr, "hash collision %08x: %s / %s - rename one in psp_sound_format.hpp\n", c.hash,
                         it->second.c_str(), n.c_str());
            return 1;
        }
        std::string why;
        if (!decode(n, c, why)) {
            ++failed;
            std::fprintf(stderr, "  skipped %s: %s\n", n.c_str(), why.c_str());
            continue;
        }
        byHash[c.hash] = n;
        pcmBytes += c.blob.size();
        seconds += static_cast<double>(c.frames) / c.rate;
        clips.push_back(std::move(c));
    }

    // 4. Entries table: rows keep only their baked files.
    std::vector<uint8_t> entries;
    size_t rowsOut = 0;
    if (!rowsIn.empty()) {
        std::vector<fmt::SoundEntryRow> rows;
        std::vector<uint32_t> files;
        for (RowIn& in : rowsIn) {
            in.row.firstFile = static_cast<uint16_t>(files.size());
            uint32_t n = 0;
            for (const std::string& f : in.files) {
                const uint32_t h = fmt::soundPathHash(f.c_str());
                if (byHash.count(h) && byHash[h] == f) {
                    files.push_back(h);
                    ++n;
                }
            }
            if (n == 0 || files.size() > 65535) {
                files.resize(in.row.firstFile);
                continue;
            }
            in.row.fileCount = static_cast<uint8_t>(n);
            rows.push_back(in.row);
        }
        rowsOut = rows.size();
        fmt::SoundEntriesHeader h{};
        std::memcpy(h.magic, "WSEN", 4);
        h.version = fmt::kSoundFormatVersion;
        h.rowCount = static_cast<uint32_t>(rows.size());
        h.fileCount = static_cast<uint32_t>(files.size());
        h.rowOffset = sizeof(h);
        h.fileOffset = static_cast<uint32_t>(sizeof(h) + rows.size() * sizeof(fmt::SoundEntryRow));
        entries.resize(sizeof(h));
        std::memcpy(entries.data(), &h, sizeof(h));
        const auto* rp = reinterpret_cast<const uint8_t*>(rows.data());
        entries.insert(entries.end(), rp, rp + rows.size() * sizeof(fmt::SoundEntryRow));
        const auto* fp = reinterpret_cast<const uint8_t*>(files.data());
        entries.insert(entries.end(), fp, fp + files.size() * 4);
    }

    // 5. Surface grids.
    std::vector<PakItem> first;
    if (!voiceRows.empty()) {
        const auto* bytes=reinterpret_cast<const uint8_t*>(voiceRows.data());
        first.push_back({"voices",3,std::vector<uint8_t>(bytes,bytes+voiceRows.size()*sizeof(fmt::CharacterVoice))});
        std::printf("character voices: %zu race/sex profiles\n",voiceRows.size());
    }
    if (!areaAmbience.empty()) {
        const auto* p=reinterpret_cast<const uint8_t*>(areaAmbience.data());
        first.push_back({"ambience",3,std::vector<uint8_t>(p,p+areaAmbience.size()*sizeof(fmt::AreaAmbience))});
    }
    if(!npcVoices.empty()) {
        const auto* p=reinterpret_cast<const uint8_t*>(npcVoices.data());
        first.push_back({"npcvoices",3,std::vector<uint8_t>(p,p+npcVoices.size()*sizeof(fmt::NpcVoiceRecord))});
    }
    if (!entries.empty()) first.push_back({"entries", 3, std::move(entries)});
    uint32_t hist[8] = {};
    uint32_t waterCells = 0;
    size_t gridCount = 0;
    if (allMaps || !maps.empty()) {
        pipeline::DBCFile mapDbc;
        std::vector<uint8_t> bytes;
        const bool mapOk = readFile(g_root + "/dbfilesclient/map.dbc", bytes) && mapDbc.load(bytes);
        if (allMaps && mapOk) {
            maps.clear();
            for (uint32_t row = 0; row < mapDbc.getRecordCount(); ++row) {
                const std::string dir = mapDbc.getString(row, 1);
                if (!dir.empty()) maps.push_back(dir);
            }
            std::sort(maps.begin(), maps.end());
            maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
        }
        if (allMaps && !mapOk) {
            std::fprintf(stderr, "--all-maps requires a valid map.dbc\n");
            return 3;
        }
        for (const std::string& m : maps) {
            uint32_t mapId = 0xFFFFFFFFu;
            if (mapOk)
                for (uint32_t r = 0; r < mapDbc.getRecordCount(); ++r)
                    if (lower(mapDbc.getString(r, 1)) == lower(m)) mapId = mapDbc.getUInt32(r, 0);
            if (mapId == 0xFFFFFFFFu) {
                std::fprintf(stderr, "map %s not in map.dbc - no surface grids for it\n", m.c_str());
                continue;
            }
            const std::string dir = g_root + "/world/maps/" + lower(m) + "/";
            // Enumerate existing ADTs instead of probing 4096 filenames for
            // each of hundreds of map records (most have few or no tiles).
            std::error_code ec;
            std::vector<fs::path> tiles;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (lower(it->path().extension().string()) == ".adt") tiles.push_back(it->path());
            std::sort(tiles.begin(), tiles.end());
            const std::string prefix = lower(m) + "_";
            for (const auto& tile : tiles) {
                const std::string filename = lower(tile.filename().string());
                if (!startsWith(filename, prefix)) continue;
                int x = -1, y = -1;
                if (std::sscanf(filename.c_str() + prefix.size(), "%d_%d.adt", &x, &y) != 2 ||
                    x < 0 || x >= 64 || y < 0 || y >= 64) continue;
                if (filename != prefix + std::to_string(x) + "_" + std::to_string(y) + ".adt") continue;
                std::vector<uint8_t> adt;
                if (!readFile(tile.string(), adt)) continue;
                const pipeline::ADTTerrain t = pipeline::ADTLoader::load(adt);
                char name[48];
                std::snprintf(name, sizeof(name), "g_%u_%d_%d", mapId, x, y);
                first.push_back({name, 3, surfaceGrid(t, hist, waterCells)});
                ++gridCount;
            }
        }
    }

    // --dump: hash -> game path, to read the PSP's "sfx_load id=" lines.
    if (!dumpDir.empty()) {
        if (FILE* f = std::fopen((dumpDir + "/sounds_paths.txt").c_str(), "w")) {
            for (const Clip& c : clips)
                std::fprintf(f, "%08x %s %u Hz %u ch %.2f s\n", c.hash, c.path.c_str(), c.rate, c.channels,
                             static_cast<double>(c.frames) / c.rate);
            std::fclose(f);
        }
    }

    // 6. Paks: the tables and grids in the first, clips in path order, a new
    // part past --max-pak-mb.
    std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.path < b.path; });
    std::vector<std::vector<PakItem>> parts(1);
    parts[0] = std::move(first);
    uint64_t partBytes = 0;
    for (const auto& it : parts[0]) partBytes += (it.blob.size() + 63) & ~uint64_t(63);
    for (Clip& c : clips) {
        const uint64_t sz = (c.blob.size() + 63) & ~uint64_t(63);
        if (partBytes + sz > maxPakMb * 1048576ull || parts.back().size() >= 16000) {
            if (parts.size() >= 8) {
                std::fprintf(stderr, "more than 8 parts - raise --max-pak-mb\n");
                return 1;
            }
            parts.emplace_back();
            partBytes = 0;
        }
        char name[16];
        std::snprintf(name, sizeof(name), "s_%08x", c.hash);
        parts.back().push_back({name, 3, std::move(c.blob)});
        partBytes += sz;
    }
    for (size_t k = 0; k < parts.size(); ++k)
        if (!writePak(partPath(out, static_cast<int>(k)), parts[k])) return 1;
    for (int k = static_cast<int>(parts.size()); k < 8; ++k) {  // stale parts of an earlier, larger bake
        std::error_code ec;
        if (fs::remove(partPath(out, k), ec)) std::printf("removed stale %s\n", partPath(out, k).c_str());
    }

    std::printf("sounds: %zu clips, %.1f MB PCM (%.0f s at %u Hz %s), manager paths %zu/%zu missing, "
                "not decoded %zu, entry rows %zu (files not extracted %zu, mp3 skipped %zu), parts %zu\n",
                clips.size(), pcmBytes / 1048576.0, seconds, g_rate, g_stereo ? "stereo" : "mono", fixedMissing,
                fixedTotal, failed, rowsOut, absent, mp3Skipped, parts.size());
    std::printf("surface grids: %zu tiles, cells stone %u dirt %u grass %u wood %u metal %u snow %u, water %u\n",
                gridCount, hist[0], hist[1], hist[2], hist[3], hist[4], hist[6], waterCells);
    if (verify || !dumpDir.empty()) return verifyPaks(out, dumpDir);
    return 0;
}
