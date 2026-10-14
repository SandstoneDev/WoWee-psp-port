#include "platform/psp/psp_bigheap.hpp"  // worldArenaAlloc
#include "platform/psp/psp_pak.hpp"

#include "platform/psp/psp_log.hpp"

#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <psputils.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unordered_map>

namespace wowee::platform::psp {

namespace {

uint32_t g_pakReads = 0;  // pakReadCount()
PakReadTick g_readTick = nullptr;  // pakSetReadTick
void* g_readTickUser = nullptr;

struct PakHeader {
    char magic[4];
    uint32_t version;
    uint32_t count;
    uint32_t tocOffset;
};

} // namespace

// the Memory Stick driver runs out of open files (0x80020320
// SCE_KERNEL_ERROR_MFILE on hardware: ui.pak and every music track
// failed to open once the tile streamer held its own second descriptor per
// pak). Every pak read now goes through one lock, seek + read per chunk of at
// most 64 KB, so the streamer thread and the main thread can share a pak's
// descriptor and neither waits for a whole multi-MB read of the other.
namespace {
SceUID g_ioSema = -1;
void ioLock() {
    if (g_ioSema < 0) g_ioSema = sceKernelCreateSema("wowee_pakio", 0, 1, 1, nullptr);
    if (g_ioSema >= 0) sceKernelWaitSema(g_ioSema, 1, nullptr);
}
void ioUnlock() {
    if (g_ioSema >= 0) sceKernelSignalSema(g_ioSema, 1);
}
} // namespace

bool pakRead(int fd, uint32_t offset, void* dst, uint32_t size) {
    constexpr uint32_t kChunk = 64 * 1024;
    auto* p = static_cast<uint8_t*>(dst);
    uint32_t got = 0;
    while (got < size) {
        const uint32_t want = std::min(kChunk, size - got);
        ioLock();
        bool ok = sceIoLseek32(fd, static_cast<int>(offset + got), PSP_SEEK_SET) == static_cast<int>(offset + got);
        uint32_t chunkGot = 0;
        while (ok && chunkGot < want) {
            const int n = sceIoRead(fd, p + got + chunkGot, want - chunkGot);
            if (n <= 0) ok = false;
            else chunkGot += static_cast<uint32_t>(n);
        }
        ioUnlock();
        if (!ok) return false;
        got += want;
    }
    if (PakReadTick tick = g_readTick) tick(g_readTickUser);
    return true;
}

void pakSetReadTick(PakReadTick fn, void* user) {
    // Another thread may be inside pakRead with the old function: the user pointer stays when clearing.
    g_readTick = nullptr;
    if (!fn) return;
    g_readTickUser = user;
    g_readTick = fn;
}

namespace {
bool readAt(int fd, uint32_t offset, void* dst, uint32_t size) { return pakRead(fd, offset, dst, size); }
} // namespace

int pakProbeFreeFiles(const char* absPath, int max) {
    SceUID fds[64];
    int n = 0;
    if (max > 64) max = 64;
    while (n < max) {
        const SceUID fd = sceIoOpen(absPath, PSP_O_RDONLY, 0);
        if (fd < 0) break;
        fds[n++] = fd;
    }
    for (int i = 0; i < n; ++i) sceIoClose(fds[i]);
    return n;
}

Pak::~Pak() { close(); }

// On hardware fd_probe said free=4 at world entry, then gameobjects.pak, the minimap and the music took
// the rest and ui.pak / tutorial.pak failed with 0x80020320 SCE_KERNEL_ERROR_MFILE - the mail window lost its frame
// art, the tooltips theirs, the tutorials went off): the pak descriptors open now, listed when an open fails.
// the table is psp_fd_budget.hpp's FdRegistry - every holder of a card descriptor
// (not only the paks: the log, the music, the sound bank, the minimap, ... note theirs too), the capacity fd_probe
// measured at world entry, the rare paks' release hooks. Same lock, same pak_open_list line.
namespace {
FdRegistry g_fdReg;
SceUID g_openSema = -1;
SceUID g_mainThread = -1;  // pakFdRelieve releases only from here (the rare paks' thread)
void openLock() {
    if (g_openSema < 0) g_openSema = sceKernelCreateSema("wowee_pakfds", 0, 1, 1, nullptr);
    if (g_openSema >= 0) sceKernelWaitSema(g_openSema, 1, nullptr);
}
void openUnlock() {
    if (g_openSema >= 0) sceKernelSignalSema(g_openSema, 1);
}
void noteOpen(int fd, const char* path) {
    openLock();
    g_fdReg.noteOpen(fd, path);
    openUnlock();
}
void noteClose(int fd) {
    openLock();
    g_fdReg.noteClose(fd);
    openUnlock();
}
// An open failed: which paks hold the descriptors (the music, the log and the system hold the others).
void logOpenFail(const char* path, int err) {
    PSP_LOGE(tag::Fs, "pak_open path=%s err=%08X", path, static_cast<unsigned>(err));
    if (static_cast<unsigned>(err) != 0x80020320u) return;
    char list[320];
    openLock();
    g_fdReg.notePressure(logUptimeMs());  // the rare paks give theirs back (pakFdService)
    const int n = g_fdReg.list(list, sizeof(list));
    const int cap = g_fdReg.capacity();
    openUnlock();
    PSP_LOGE(tag::Fs, "pak_open_list n=%d cap=%d %s", n, cap, list);
}
constexpr unsigned kErrMFile = 0x80020320u;  // SCE_KERNEL_ERROR_MFILE
bool onMainThread() { return g_mainThread >= 0 && sceKernelGetThreadId() == g_mainThread; }
} // namespace

void pakNoteOpen(int fd, const char* path) { noteOpen(fd, path); }
void pakNoteClose(int fd) { noteClose(fd); }
void pakLogOpenFail(const char* path, int err) { logOpenFail(path, err); }

int pakOpenFds() {
    openLock();
    const int n = g_fdReg.openCount();
    openUnlock();
    return n;
}

// ---- the descriptor budget (psp_pak.hpp) ----------------------------------------------------------------------
void pakSetMainThread() { g_mainThread = sceKernelGetThreadId(); }

void pakFdSetCapacity(int cap) {
    if (g_mainThread < 0) pakSetMainThread();  // world entry runs on the main thread
    openLock();
    g_fdReg.setCapacity(cap);
    openUnlock();
}

int pakFdCapacity() {
    openLock();
    const int c = g_fdReg.capacity();
    openUnlock();
    return c;
}

int pakFdFree() {
    openLock();
    const int f = g_fdReg.freeFds();
    openUnlock();
    return f;
}

bool pakFdTight() {
    openLock();
    const bool t = g_fdReg.tight();
    openUnlock();
    return t;
}

uint32_t pakFdIdleMs(uint32_t idleMs) {
    openLock();
    const uint32_t ms = g_fdReg.idleFor(logUptimeMs(), idleMs);
    openUnlock();
    return ms;
}

void pakFdSetOwner(int fd, void* owner, FdLastUseFn lastUse, FdReleaseFn release) {
    openLock();
    g_fdReg.setOwner(fd, owner, lastUse, release);
    openUnlock();
}

int pakFdRelieve(int want) {
    if (!onMainThread()) {  // another thread: the main thread does it (pakFdService)
        openLock();
        g_fdReg.notePressure(logUptimeMs());
        openUnlock();
        return 0;
    }
    FdRegistry::Pick picks[4];
    if (want > 4) want = 4;
    const uint32_t now = logUptimeMs();
    openLock();
    const int n = g_fdReg.pickRelieve(now, FdRegistry::kPressureIdleMs, picks, want);
    openUnlock();
    for (int i = 0; i < n; ++i) picks[i].release(picks[i].owner);  // closes through noteClose (outside the lock)
    if (n > 0) {
        openLock();
        g_fdReg.noteRelieved(static_cast<uint32_t>(n));
        const int open = g_fdReg.openCount(), freeFds = g_fdReg.freeFds();
        const uint32_t total = g_fdReg.relieved();
        openUnlock();
        static uint32_t logs = 0;
        if (++logs <= 40)
            PSP_LOGI(tag::Fs, "fd_relieve n=%d first=%s open=%d free=%d total=%u", n, picks[0].name, open, freeFds,
                     static_cast<unsigned>(total));
    }
    return n;
}

void pakFdService() {
    if (g_mainThread < 0 || !onMainThread()) return;
    const uint32_t now = logUptimeMs();
    openLock();
    const bool pressure = g_fdReg.pressure(now);
    openUnlock();
    if (pressure) pakFdRelieve(1);
}

int pakFdList(char* buf, size_t cap) {
    openLock();
    const int n = g_fdReg.list(buf, cap);
    openUnlock();
    return n;
}

void pakFdStats(uint32_t& mfiles, uint32_t& relieved, int& rare) {
    openLock();
    mfiles = g_fdReg.mfiles();
    relieved = g_fdReg.relieved();
    rare = g_fdReg.rareOpen();
    openUnlock();
}

void Pak::setRare(bool on) {
    rare_ = on;
    if (fd_ >= 0) pakFdSetOwner(fd_, on ? this : nullptr, &Pak::fdLastUse, &Pak::fdRelease);
}

uint32_t Pak::fdLastUse(void* self) { return static_cast<Pak*>(self)->lastUseMs_; }
void Pak::fdRelease(void* self) { static_cast<Pak*>(self)->releaseFd(); }

bool Pak::ensureFd() { return fd_ >= 0 || reopenFd(); }

// Opens with one retry: the main thread's open that failed for want of a descriptor gets one from an idle rare pak.
int pakOpenRetry(const char* absPath) {
    int fd = sceIoOpen(absPath, PSP_O_RDONLY, 0);
    if (static_cast<unsigned>(fd) == kErrMFile && pakFdRelieve(1) > 0) fd = sceIoOpen(absPath, PSP_O_RDONLY, 0);
    return fd;
}
// ---- the descriptor budget (end) -------------------------------------------------------------------------------

bool Pak::open(const char* absPath) {
    close();
    fd_ = pakOpenRetry(absPath);  // + one retry after an idle rare pak gave its descriptor back
    if (fd_ < 0) {
        logOpenFail(absPath, fd_);
        fd_ = -1;
        return false;
    }
    noteOpen(fd_, absPath);
    lastUseMs_ = logUptimeMs();  // not idle the moment it opens
    if (rare_) pakFdSetOwner(fd_, this, &Pak::fdLastUse, &Pak::fdRelease);
    std::snprintf(path_, sizeof(path_), "%s", absPath);
    PakHeader h;
    // up to 65535 records (the u16 name index); was 16384 - a whole continent's doodads pak.
    if (!readAt(fd_, 0, &h, sizeof(h)) || std::memcmp(h.magic, "WPAK", 4) != 0 || h.version != 1 ||
        h.count == 0 || h.count > 65535) {
        PSP_LOGE(tag::Fs, "pak_header_bad path=%s count=%u", absPath, static_cast<unsigned>(h.count));
        close();
        return false;
    }
    // the compact TOC (psp_pak.hpp PakEntry) - the disk records are read twice in chunks of 256
    // (16 KB): the first pass sums the names, then one block holds the records and the name pool.
    constexpr uint32_t kChunk = 256;
    PakDiskEntry* chunk = static_cast<PakDiskEntry*>(std::malloc(kChunk * sizeof(PakDiskEntry)));
    if (!chunk) {
        close();
        return false;
    }
    auto forEachRecord = [&](auto&& fn) {
        for (uint32_t first = 0; first < h.count; first += kChunk) {
            const uint32_t n = std::min(kChunk, h.count - first);
            if (!readAt(fd_, h.tocOffset + first * static_cast<uint32_t>(sizeof(PakDiskEntry)), chunk,
                        n * static_cast<uint32_t>(sizeof(PakDiskEntry))))
                return false;
            for (uint32_t k = 0; k < n; ++k) {
                chunk[k].name[sizeof(chunk[k].name) - 1] = '\0';
                fn(first + k, chunk[k]);
            }
        }
        return true;
    };
    uint32_t namesBytes = 0;
    bool ok = forEachRecord([&](uint32_t, const PakDiskEntry& d) {
        namesBytes += static_cast<uint32_t>(std::strlen(d.name)) + 1u;
    });
    const uint32_t tocBytes = h.count * static_cast<uint32_t>(sizeof(PakEntry));
    if (ok) block_ = std::malloc(tocBytes + namesBytes);
    if (ok && block_) {
        toc_ = static_cast<PakEntry*>(block_);
        char* pool = static_cast<char*>(block_) + tocBytes;
        ok = forEachRecord([&](uint32_t i, const PakDiskEntry& d) {
            const size_t len = std::strlen(d.name) + 1u;
            std::memcpy(pool, d.name, len);
            toc_[i] = PakEntry{pool, d.offset, d.size, d.kind};
            pool += len;
        });
    }
    std::free(chunk);
    if (!ok || !block_) {
        PSP_LOGE(tag::Fs, "pak_toc_bad path=%s count=%u", absPath, static_cast<unsigned>(h.count));
        close();
        return false;
    }
    count_ = h.count;
    if (h.count >= 1000)  // what the big TOCs cost now (was 64 B a record)
        PSP_LOGI(tag::Fs, "pak_toc path=%s count=%u kb=%u", absPath, static_cast<unsigned>(h.count),
                 static_cast<unsigned>((tocBytes + namesBytes + h.count * 2u) / 1024u));
    // name index for find() (count <= 65535 fits u16).
    index_ = static_cast<uint16_t*>(std::malloc(h.count * sizeof(uint16_t)));
    if (index_) {
        for (uint32_t i = 0; i < h.count; ++i) index_[i] = static_cast<uint16_t>(i);
        const PakEntry* toc = toc_;
        std::stable_sort(index_, index_ + h.count,
                         [toc](uint16_t a, uint16_t b) { return std::strcmp(toc[a].name, toc[b].name) < 0; });
    }
    fileId_ = pakFileId(absPath);
    return true;
}

void Pak::close() {
    if (fd_ >= 0) {
        noteClose(fd_);
        sceIoClose(fd_);
    }
    fd_ = -1;
    path_[0] = '\0';
    worldArena_ = false;
    std::free(block_);  // the records and the name pool
    block_ = nullptr;
    toc_ = nullptr;
    std::free(index_);
    index_ = nullptr;
    count_ = 0;
    fileId_ = 0;
}

const PakEntry* Pak::find(const char* name) const {
    if (index_) {
        // lower bound over the stable name order = the first entry
        // with this name in TOC order, as the linear scan returned.
        const PakEntry* toc = toc_;
        const uint16_t* it = std::lower_bound(index_, index_ + count_, name, [toc](uint16_t i, const char* n) {
            return std::strcmp(toc[i].name, n) < 0;
        });
        if (it != index_ + count_ && std::strcmp(toc_[*it].name, name) == 0) return &toc_[*it];
        return nullptr;
    }
    for (uint32_t i = 0; i < count_; ++i) {
        if (std::strcmp(toc_[i].name, name) == 0) return &toc_[i];
    }
    return nullptr;
}

void Pak::releaseFd() {
    if (fd_ < 0 || !path_[0]) return;
    noteClose(fd_);
    sceIoClose(fd_);
    fd_ = -1;
}

void Pak::releaseIfIdle(uint32_t nowMs, uint32_t idleMs) {
    // the rare paks' rule - 1 s when one descriptor or none is left, 250 ms after an open failed
    // for want of one (psp_fd_budget.hpp); else the caller's.
    if (fd_ >= 0 && nowMs - lastUseMs_ >= pakFdIdleMs(idleMs)) releaseFd();
}

bool Pak::reopenFd() {
    if (fd_ >= 0) return true;
    if (!count_ || !path_[0]) return false;
    const int fd = pakOpenRetry(path_);  // + one retry after a rare pak gave one back
    if (fd < 0) {
        logOpenFail(path_, fd);
        return false;
    }
    noteOpen(fd, path_);
    fd_ = fd;
    lastUseMs_ = logUptimeMs();
    if (rare_) pakFdSetOwner(fd_, this, &Pak::fdLastUse, &Pak::fdRelease);
    return true;
}

void* Pak::load(const PakEntry* e) {
    if (!e || !count_) return nullptr;
    // streaming - a blob the loader thread already read.
    if (void* parked = pakPrefetchTake(fileId_, e->offset)) return parked;
    lastUseMs_ = logUptimeMs();  // the clock releaseIfIdle is given
    if (fd_ < 0 && !reopenFd()) return nullptr;  // given back by releaseFd
    ++g_pakReads;
    // the world arena first for a world pak (setWorldArena), newlib otherwise / when it is full.
    void* block = worldArena_ ? worldArenaAlloc((e->size + 63u) & ~63u, 64) : nullptr;
    if (!block) block = memalign(64, (e->size + 63u) & ~63u);
    if (!block) {
        PSP_LOGE(tag::Mem, "pak_alloc_fail name=%s size=%u", e->name, static_cast<unsigned>(e->size));
        return nullptr;
    }
    if (!readAt(fd_, e->offset, block, e->size)) {
        PSP_LOGE(tag::Fs, "pak_read_fail name=%s", e->name);
        std::free(block);
        return nullptr;
    }
    sceKernelDcacheWritebackRange(block, (e->size + 63u) & ~63u);
    return block;
}

// see psp_pak.hpp.
bool Pak::readPart(const PakEntry* e, uint32_t offset, void* dst, uint32_t size) {
    if (!e || !count_ || offset > e->size || size > e->size - offset) return false;
    if (!size) return true;
    if (const void* parked = pakPrefetchPeek(fileId_, e->offset)) {  // read ahead already
        std::memcpy(dst, static_cast<const uint8_t*>(parked) + offset, size);
        return true;
    }
    lastUseMs_ = logUptimeMs();
    if (fd_ < 0 && !reopenFd()) return false;
    ++g_pakReads;
    if (!readAt(fd_, e->offset + offset, dst, size)) {
        PSP_LOGE(tag::Fs, "pak_read_fail name=%s part=%u+%u", e->name, static_cast<unsigned>(offset),
                 static_cast<unsigned>(size));
        return false;
    }
    return true;
}

void pakFree(void* block) { std::free(block); }

// streaming prefetch parking (main thread only).
namespace {
std::unordered_map<uint64_t, void*>& parked() {
    static std::unordered_map<uint64_t, void*> m;
    return m;
}
uint64_t parkKey(uint32_t fileId, uint32_t offset) { return (static_cast<uint64_t>(fileId) << 32) | offset; }
} // namespace

uint32_t pakFileId(const char* absPath) {
    uint32_t h = 2166136261u;
    for (const char* p = absPath; p && *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
    return h ? h : 1u;
}

void pakPrefetchPut(uint32_t fileId, uint32_t offset, void* blob) {
    if (!blob) return;
    void*& slot = parked()[parkKey(fileId, offset)];
    if (slot) std::free(slot);  // read twice: keep the newer copy
    slot = blob;
}

void* pakPrefetchTake(uint32_t fileId, uint32_t offset) {
    auto& m = parked();
    if (m.empty()) return nullptr;
    auto it = m.find(parkKey(fileId, offset));
    if (it == m.end()) return nullptr;
    void* b = it->second;
    m.erase(it);
    return b;
}

const void* pakPrefetchPeek(uint32_t fileId, uint32_t offset) {
    auto& m = parked();
    if (m.empty()) return nullptr;
    auto it = m.find(parkKey(fileId, offset));
    return it == m.end() ? nullptr : it->second;
}

uint32_t pakPrefetchDrop(uint32_t fileId) {
    auto& m = parked();
    uint32_t n = 0;
    for (auto it = m.begin(); it != m.end();) {
        if (static_cast<uint32_t>(it->first >> 32) == fileId) {
            std::free(it->second);
            it = m.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

uint32_t pakPrefetchCount() { return static_cast<uint32_t>(parked().size()); }

uint32_t pakReadCount() { return g_pakReads; }

} // namespace wowee::platform::psp
