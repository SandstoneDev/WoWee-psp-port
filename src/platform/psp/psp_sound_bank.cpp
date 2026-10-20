// sounds - see include/platform/psp/psp_sound_bank.hpp.
#include "platform/psp/psp_sound_bank.hpp"

#include "platform/psp/psp_audio.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "rendering/psp/psp_sound_format.hpp"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace wowee::platform::psp {

namespace fmt = wowee::rendering::psp;

namespace {
constexpr uint32_t kIdleCloseMs = 2000;  // loader closes the pak after this long without a request
constexpr uint32_t kBusyMarginMs = 150;  // a mixer block is ~23 ms; stolen voices finish their block
} // namespace

SoundBank::~SoundBank() { close(); }

bool SoundBank::open(const char* absPath, uint32_t budget, uint32_t mapId) {
    close();
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    budgetKb = budget;
    uint64_t dataBytes = 0;
    char gridPrefix[24];
    const int gp = std::snprintf(gridPrefix, sizeof(gridPrefix), "g_%u_", static_cast<unsigned>(mapId));
    // sounds.pak, then sounds_1.pak, sounds_2.pak ... while they exist (a set
    // over the FAT32 file limit is split by the converter, --max-pak-mb).
    for (int k = 0; k < kMaxPaks; ++k) {
        if (k == 0) {
            std::snprintf(paths_[0], sizeof(paths_[0]), "%s", absPath);
        } else {
            const size_t n = std::strlen(absPath);
            const size_t stem = n > 4 && std::strcmp(absPath + n - 4, ".pak") == 0 ? n - 4 : n;
            std::snprintf(paths_[k], sizeof(paths_[k]), "%.*s_%d.pak", static_cast<int>(stem), absPath, k);
        }
        SceIoStat st;
        if (k > 0 && sceIoGetstat(paths_[k], &st) < 0) break;  // no more parts (no pak_open error line)
        Pak pak;
        if (!pak.open(paths_[k])) {
            if (k == 0) return false;
            break;
        }
        paks_ = k + 1;
        for (uint32_t i = 0; i < pak.count(); ++i) {
            const PakEntry* e = pak.entry(i);
            unsigned h = 0;
            if (e->name[0] == 's' && e->name[1] == '_' && std::sscanf(e->name + 2, "%8x", &h) == 1 &&
                e->size > sizeof(fmt::SoundClipHeader)) {
                Clip c;
                c.hash = h;
                c.offset = e->offset;
                c.size = e->size;
                c.pak = static_cast<uint8_t>(k);
                clips_.push_back(c);
                dataBytes += e->size;
            } else if (k == 0 && std::strcmp(e->name, "entries") == 0 && e->size >= sizeof(fmt::SoundEntriesHeader)) {
                entries_ = pak.load(e);
                entriesSize_ = entries_ ? e->size : 0;
            } else if (k == 0 && std::strncmp(e->name, gridPrefix, static_cast<size_t>(gp)) == 0 &&
                       e->size >= fmt::kSurfaceGridBytes) {
                int x = 0, y = 0;
                if (std::sscanf(e->name + gp, "%d_%d", &x, &y) != 2) continue;
                // PSP: only where it is - read when a tile is first asked for (surfaceGrid)
                gridOffsets_[static_cast<uint32_t>(x * 100 + y)] = e->offset;
            }
        }
        pak.close();  // card open-file limit: no descriptor kept (the loader reopens on demand)
    }
    std::sort(clips_.begin(), clips_.end(), [](const Clip& a, const Clip& b) { return a.hash < b.hash; });
    stats_ = SoundBankStats{};
    stats_.clips = static_cast<uint32_t>(clips_.size());
    PSP_LOGI(tag::Sys, "sounds_pak entries=%u kb=%u paks=%d table_kb=%u grids=%u ms=%u",
             static_cast<unsigned>(stats_.clips), static_cast<unsigned>(dataBytes / 1024u), paks_,
             static_cast<unsigned>(entriesSize_ / 1024u), static_cast<unsigned>(gridOffsets_.size()),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    if (clips_.empty()) {
        close();
        return false;
    }
    sema_ = sceKernelCreateSema("wowee_sfxreq", 0, 0, kRing, nullptr);
    running_ = true;
    // Below the main thread (0x20), the tile streamer (streamprio 40) and the
    // MP3 decoder (0x30): a clip read never delays a frame or the music.
    thread_ = sceKernelCreateThread("wowee_sfxload", &SoundBank::loaderMain, 0x31, 8 * 1024, PSP_THREAD_ATTR_USER,
                                    nullptr);
    if (sema_ < 0 || thread_ < 0) {
        PSP_LOGE(tag::Sys, "sfx_loader_failed sema=%08X thread=%08X", static_cast<unsigned>(sema_),
                 static_cast<unsigned>(thread_));
        running_ = false;
        close();
        return false;
    }
    SoundBank* self = this;
    sceKernelStartThread(thread_, sizeof(self), &self);
    open_ = true;
    return true;
}

void SoundBank::close() {
    if (thread_ >= 0) {
        running_ = false;
        if (sema_ >= 0) sceKernelSignalSema(sema_, 1);
        SceUInt timeout = 1000 * 1000;
        sceKernelWaitThreadEnd(thread_, &timeout);
        sceKernelDeleteThread(thread_);
        thread_ = -1;
    }
    if (sema_ >= 0) sceKernelDeleteSema(sema_);
    sema_ = -1;
    running_ = false;
    // Loads that finished after the last update().
    while (doneR_ != doneW_) {
        const Done& d = done_[doneR_ % kRing];
        if (d.block) std::free(d.block);
        doneR_ = doneR_ + 1;
    }
    if (residentBytes_) {
        // The mixer may still be reading a clip: silence it and let its
        // current block (~23 ms) finish before the PCM is freed.
        sfxStopAll();
        sceKernelDelayThread(40 * 1000);
    }
    for (Clip& c : clips_)
        if (c.block) std::free(c.block);
    clips_.clear();
    gridOffsets_.clear();
    for (GridSlot& s : gridCache_) s = GridSlot{};
    if (entries_) pakFree(entries_);
    entries_ = nullptr;
    entriesSize_ = 0;
    residentBytes_ = 0;
    reqW_ = 0;
    reqR_ = 0;
    doneW_ = 0;
    doneR_ = 0;
    for (Late& l : late_) l = Late{};
    paks_ = 0;
    open_ = false;
}

int SoundBank::find(uint32_t pathHash) const {
    auto it = std::lower_bound(clips_.begin(), clips_.end(), pathHash,
                               [](const Clip& c, uint32_t h) { return c.hash < h; });
    return it != clips_.end() && it->hash == pathHash ? static_cast<int>(it - clips_.begin()) : -1;
}

const uint8_t* SoundBank::surfaceGrid(int tileX, int tileY) const {
    const uint32_t key = static_cast<uint32_t>(tileX * 100 + tileY);
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    const uint32_t nowMs = static_cast<uint32_t>(t0 / 1000u);
    ++gridTick_;
    GridSlot* lru = &gridCache_[0];
    GridSlot* use = nullptr;
    for (GridSlot& s : gridCache_) {
        if (s.key == key) {
            s.lastUse = gridTick_;
            if (s.ok) return s.data.data();
            if (nowMs - s.failMs < 2000u) return nullptr;  // a failed read (no free descriptor?) again after 2 s
            use = &s;
            break;
        }
        if (s.lastUse < lru->lastUse) lru = &s;
    }
    auto it = gridOffsets_.find(key);
    if (it == gridOffsets_.end()) return nullptr;  // no grid baked for this tile
    // One open + seek + read of sounds.pak (the loader thread keeps its own descriptor).
    GridSlot& s = use ? *use : *lru;
    s.key = key;
    s.lastUse = gridTick_;
    s.ok = false;
    s.failMs = nowMs;
    s.data.resize(fmt::kSurfaceGridBytes);
    const SceUID fd = sceIoOpen(paths_[0], PSP_O_RDONLY, 0);
    if (fd >= 0) {
        if (sceIoLseek32(fd, static_cast<int>(it->second), PSP_SEEK_SET) == static_cast<int>(it->second))
            s.ok = sceIoRead(fd, s.data.data(), fmt::kSurfaceGridBytes) == static_cast<int>(fmt::kSurfaceGridBytes) &&
                   std::memcmp(s.data.data(), "WSRF", 4) == 0;
        sceIoClose(fd);
    }
    ++gridReads_;
    if (++gridLogs_ <= 40)
        PSP_LOGI(tag::Sys, "sound_grid tile=%d_%d ok=%d fd=%08X ms=%u reads=%u", tileX, tileY, s.ok ? 1 : 0,
                 static_cast<unsigned>(fd), static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u),
                 static_cast<unsigned>(gridReads_));
    return s.ok ? s.data.data() : nullptr;
}

bool SoundBank::request(int index) {
    Clip& c = clips_[static_cast<size_t>(index)];
    if (c.state == 1) return true;
    if (c.state != 0) return false;
    if (reqW_ - reqR_ >= kRing || reqW_ - doneR_ >= kRing) return false;  // loader busy: try again later
    req_[reqW_ % kRing] = index;
    asm volatile("" ::: "memory");
    reqW_ = reqW_ + 1;
    c.state = 1;
    sceKernelSignalSema(sema_, 1);
    return true;
}

void SoundBank::prefetch(int index) {
    if (open_ && index >= 0 && static_cast<size_t>(index) < clips_.size()) request(index);
}

bool SoundBank::start(Clip& c, float gain, float pan, float pitch, uint32_t nowMs) {
    const auto* h = static_cast<const fmt::SoundClipHeader*>(c.block);
    const auto* pcm = reinterpret_cast<const int16_t*>(h + 1);
    const float p = pitch > 0.25f && pitch < 4.0f ? pitch : 1.0f;
    const uint32_t rate = static_cast<uint32_t>(static_cast<float>(h->rate) * p);
    c.lastUsedMs = nowMs;
    const int voice = sfxPlay(pcm, h->frames, h->channels, rate, gain, pan, false);
    if (voice < 0) {
        ++stats_.drops;
        return false;
    }
    const uint32_t ms = static_cast<uint32_t>(static_cast<uint64_t>(h->frames) * 1000u / (rate ? rate : 1u));
    c.busyUntilMs = std::max(c.busyUntilMs, nowMs + ms + kBusyMarginMs);
    ++stats_.plays;
    return true;
}

bool SoundBank::play(int index, float gain, float pan, float pitch) {
    if (!open_ || index < 0 || static_cast<size_t>(index) >= clips_.size()) return false;
    Clip& c = clips_[static_cast<size_t>(index)];
    const uint32_t now = logUptimeMs();
    if (c.state == 2) return start(c, gain, pan, pitch, now);
    if (c.state == 3 || !request(index)) {
        ++stats_.drops;
        return false;
    }
    // Played when the loader delivers it, if that is soon enough.
    Late* slot = nullptr;
    for (Late& l : late_) {
        if (l.index < 0 || static_cast<int32_t>(now - l.deadlineMs) > 0) {
            slot = &l;
            break;
        }
    }
    if (!slot) {
        ++stats_.drops;
        return false;
    }
    *slot = Late{index, gain, pan, pitch, now + lateMs};
    return true;
}

void SoundBank::update() {
    if (!open_) return;
    const uint32_t now = logUptimeMs();
    while (doneR_ != doneW_) {
        const Done d = done_[doneR_ % kRing];
        asm volatile("" ::: "memory");
        doneR_ = doneR_ + 1;
        if (d.index < 0 || static_cast<size_t>(d.index) >= clips_.size()) {
            if (d.block) std::free(d.block);
            continue;
        }
        Clip& c = clips_[static_cast<size_t>(d.index)];
        const auto* h = static_cast<const fmt::SoundClipHeader*>(d.block);
        const bool ok = h && std::memcmp(h->magic, "WSND", 4) == 0 && (h->channels == 1 || h->channels == 2) &&
                        h->rate > 0 && sizeof(fmt::SoundClipHeader) + static_cast<uint64_t>(h->frames) * 2u * h->channels <= c.size;
        if (!ok) {
            if (d.block) std::free(d.block);
            // No block = the card said no (open-file limit, busy): tried again
            // at the next play. A bad header stays failed.
            c.state = d.block ? 3 : 0;
            ++stats_.loadFails;
            if (++failLogs_ <= 20)
                PSP_LOGW(tag::Sys, "sfx_load_failed id=%08x open_fails=%d", static_cast<unsigned>(c.hash), openFails_);
            continue;
        }
        c.block = d.block;
        c.state = 2;
        c.lastUsedMs = now;
        residentBytes_ += c.size;
        ++stats_.loads;
        if (++loadLogs_ <= 60)
            PSP_LOGI(tag::Sys, "sfx_load id=%08x kb=%u ms=%u", static_cast<unsigned>(c.hash),
                     static_cast<unsigned>((c.size + 1023u) / 1024u), static_cast<unsigned>(d.ms));
    }
    for (Late& l : late_) {
        if (l.index < 0) continue;
        Clip& c = clips_[static_cast<size_t>(l.index)];
        if (c.state == 2) {
            if (start(c, l.gain, l.pan, l.pitch, now)) ++stats_.latePlays;
            l.index = -1;
        } else if (c.state != 1 || static_cast<int32_t>(now - l.deadlineMs) > 0) {
            ++stats_.drops;
            l.index = -1;
        }
    }
    evict(now);
    stats_.residentKb = residentBytes_ / 1024u;
}

void SoundBank::evict(uint32_t now) {
    const uint64_t budget = static_cast<uint64_t>(budgetKb) * 1024u;
    uint32_t resident = 0;
    for (const Clip& c : clips_)
        if (c.state == 2) ++resident;
    stats_.resident = resident;
    while (residentBytes_ > budget) {
        Clip* oldest = nullptr;
        for (Clip& c : clips_) {
            if (c.state != 2 || static_cast<int32_t>(now - c.busyUntilMs) < 0) continue;
            if (!oldest || static_cast<int32_t>(c.lastUsedMs - oldest->lastUsedMs) < 0) oldest = &c;
        }
        if (!oldest) break;  // everything resident is playing: over budget until they end
        std::free(oldest->block);
        oldest->block = nullptr;
        oldest->state = 0;
        residentBytes_ -= oldest->size;
        ++stats_.evictions;
        --stats_.resident;
    }
}

int SoundBank::loaderMain(unsigned int args, void* argp) {
    if (args < sizeof(SoundBank*) || !argp) return 0;
    SoundBank* self = *static_cast<SoundBank**>(argp);
    return self->loaderLoop();
}

int SoundBank::loaderLoop() {
    SceUID fd = -1;  // at most one descriptor: the part of the last read
    int fdPak = -1;
    uint64_t lastUseUs = 0;
    while (running_) {
        SceUInt timeout = 500 * 1000;
        sceKernelWaitSema(sema_, 1, &timeout);
        while (running_ && reqR_ != reqW_) {
            const int32_t index = req_[reqR_ % kRing];
            asm volatile("" ::: "memory");
            reqR_ = reqR_ + 1;
            const uint64_t t0 = sceKernelGetSystemTimeWide();
            void* block = nullptr;
            const bool valid = index >= 0 && static_cast<size_t>(index) < clips_.size();
            const int part = valid ? clips_[static_cast<size_t>(index)].pak : 0;
            if (fd >= 0 && fdPak != part) {
                pakNoteClose(fd);
                sceIoClose(fd);
                fd = -1;
            }
            if (fd < 0 && valid) {
                fd = sceIoOpen(paths_[part], PSP_O_RDONLY, 0);
                fdPak = part;
                if (fd < 0) {
                    if (static_cast<unsigned>(fd) == 0x80020320u) pakFdRelieve(1);  // MFILE
                    openFails_ = openFails_ + 1;
                    fd = -1;
                } else {
                    pakNoteOpen(fd, paths_[part]);  // in the descriptor budget
                }
            }
            if (fd >= 0 && valid) {
                const Clip& c = clips_[static_cast<size_t>(index)];
                block = memalign(64, (c.size + 63u) & ~63u);
                if (block && !pakRead(fd, c.offset, block, c.size)) {
                    std::free(block);
                    block = nullptr;
                }
            }
            lastUseUs = sceKernelGetSystemTimeWide();
            while (running_ && doneW_ - doneR_ >= kRing) sceKernelDelayThread(5 * 1000);  // main thread behind
            done_[doneW_ % kRing] = Done{index, block, static_cast<uint32_t>((lastUseUs - t0) / 1000u)};
            asm volatile("" ::: "memory");
            doneW_ = doneW_ + 1;
        }
        if (fd >= 0 && sceKernelGetSystemTimeWide() - lastUseUs > kIdleCloseMs * 1000ull) {
            pakNoteClose(fd);
            sceIoClose(fd);
            fd = -1;
        }
    }
    if (fd >= 0) {
        pakNoteClose(fd);
        sceIoClose(fd);
    }
    return 0;
}

} // namespace wowee::platform::psp
