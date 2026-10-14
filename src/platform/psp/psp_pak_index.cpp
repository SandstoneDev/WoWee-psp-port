// Big paks without their TOC in RAM (include/platform/psp/psp_pak_index.hpp).
#include "platform/psp/psp_pak_index.hpp"

#include "platform/psp/psp_bigheap.hpp"  // worldArenaAlloc
#include "platform/psp/psp_log.hpp"

#include <pspiofilemgr.h>
#include <psputils.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <malloc.h>

namespace wowee::platform::psp {

namespace {

struct PakHeaderDisk {
    char magic[4];
    uint32_t version;
    uint32_t count;
    uint32_t tocOffset;
};

constexpr uint32_t kMaxStep = 1024;  // 64 KB a block: a corrupt fence never asks for more

}  // namespace

PakIndex::~PakIndex() { close(); }

bool PakIndex::open(const char* absPath) {
    close();
    fd_ = pakOpenRetry(absPath);  // + one retry after an idle rare pak gave its descriptor back
    if (fd_ < 0) {
        pakLogOpenFail(absPath, fd_);  // with the list of descriptors held
        fd_ = -1;
        return false;
    }
    pakNoteOpen(fd_, absPath);
    lastUseMs_ = logUptimeMs();  // not idle the moment it opens
    if (rare_) pakFdSetOwner(fd_, this, &PakIndex::fdLastUse, &PakIndex::fdRelease);
    std::snprintf(path_, sizeof(path_), "%s", absPath);
    PakHeaderDisk h{};
    PakDiskEntry last{};
    bool ok = pakRead(fd_, 0, &h, sizeof(h)) && std::memcmp(h.magic, "WPAK", 4) == 0 && h.version == 1 &&
              h.count >= 1 &&
              pakRead(fd_, h.tocOffset + (h.count - 1u) * static_cast<uint32_t>(sizeof(PakDiskEntry)), &last,
                      sizeof(last));
    last.name[sizeof(last.name) - 1] = '\0';
    ok = ok && std::strcmp(last.name, kPakFenceName) == 0 && last.size >= sizeof(PakFenceHeader) &&
         last.size <= 1024u * 1024u;
    if (ok) {
        fence_ = static_cast<uint8_t*>(std::malloc(last.size));
        ok = fence_ && pakRead(fd_, last.offset, fence_, last.size);
    }
    PakFenceHeader fh{};
    if (ok) {
        std::memcpy(&fh, fence_, sizeof(fh));
        ok = std::memcmp(fh.magic, "WFNC", 4) == 0 && fh.version == 1 && fh.step >= 1 && fh.step <= kMaxStep &&
             fh.records == h.count && fh.fences == (h.count + fh.step - 1u) / fh.step &&
             sizeof(fh) + fh.fences * 4u <= last.size;
    }
    if (ok) {  // every fence name inside the blob and NUL-terminated
        fenceSize_ = last.size;
        const auto* offs = reinterpret_cast<const uint32_t*>(fence_ + sizeof(fh));
        for (uint32_t i = 0; ok && i < fh.fences; ++i) {
            uint32_t o;
            std::memcpy(&o, offs + i, 4);
            ok = o < last.size && std::memchr(fence_ + o, '\0', last.size - o) != nullptr;
        }
    }
    if (ok) {
        blocks_ = static_cast<PakDiskEntry*>(std::malloc(3u * fh.step * sizeof(PakDiskEntry)));
        ok = blocks_ != nullptr;
    }
    if (!ok) {
        PSP_LOGW(tag::Fs, "pakidx_no_fence path=%s count=%u (not a sorted pak: open it with Pak)", absPath,
                 static_cast<unsigned>(h.count));
        close();
        return false;
    }
    records_ = h.count;
    tocOffset_ = h.tocOffset;
    step_ = fh.step;
    fences_ = fh.fences;
    fileId_ = pakFileId(absPath);
    ramBytes_ = fenceSize_ + 3u * step_ * static_cast<uint32_t>(sizeof(PakDiskEntry));
    PSP_LOGI(tag::Fs, "pakidx_open path=%s count=%u step=%u fence_kb=%u ram_kb=%u", absPath,
             static_cast<unsigned>(records_), static_cast<unsigned>(step_), static_cast<unsigned>(fenceSize_ / 1024u),
             static_cast<unsigned>(ramBytes_ / 1024u));
    return true;
}

void PakIndex::close() {
    if (fd_ >= 0) {
        pakNoteClose(fd_);
        sceIoClose(fd_);
    }
    fd_ = -1;
    path_[0] = '\0';
    std::free(fence_);
    fence_ = nullptr;
    std::free(blocks_);
    blocks_ = nullptr;
    fenceSize_ = records_ = tocOffset_ = step_ = fences_ = fileId_ = ramBytes_ = 0;
    cachedFirst_[0] = cachedFirst_[1] = ~0u;
    cachedN_[0] = cachedN_[1] = 0;
    lru_ = 0;
}

const char* PakIndex::fenceName(uint32_t i) const {
    uint32_t o;
    std::memcpy(&o, fence_ + sizeof(PakFenceHeader) + i * 4u, 4);
    return reinterpret_cast<const char*>(fence_ + o);
}

// TOC records first .. first + n - 1 into dst: from one of the two cached blocks, else one read (cached).
bool PakIndex::readBlock(uint32_t first, uint32_t n, PakDiskEntry* dst) {
    for (int s = 0; s < 2; ++s) {
        if (cachedFirst_[s] == first && cachedN_[s] == n) {
            std::memcpy(dst, blocks_ + s * step_, n * sizeof(PakDiskEntry));
            lru_ = static_cast<uint32_t>(s ^ 1);
            return true;
        }
    }
    if (fd_ < 0 && !ensureFd()) return false;  // given back by releaseFd
    const uint32_t s = lru_;
    PakDiskEntry* slot = blocks_ + s * step_;
    if (!pakRead(fd_, tocOffset_ + first * static_cast<uint32_t>(sizeof(PakDiskEntry)), slot,
                 n * static_cast<uint32_t>(sizeof(PakDiskEntry)))) {
        cachedFirst_[s] = ~0u;
        return false;
    }
    ++blockReads_;
    cachedFirst_[s] = first;
    cachedN_[s] = n;
    lru_ = s ^ 1u;
    std::memcpy(dst, slot, n * sizeof(PakDiskEntry));
    return true;
}

void PakIndex::releaseFd() {
    if (fd_ < 0 || !path_[0]) return;
    pakNoteClose(fd_);
    sceIoClose(fd_);
    fd_ = -1;
}

void PakIndex::releaseIfIdle(uint32_t nowMs, uint32_t idleMs) {
    // the rare paks' rule (psp_fd_budget.hpp) - shorter when descriptors are short.
    if (fd_ >= 0 && nowMs - lastUseMs_ >= pakFdIdleMs(idleMs)) releaseFd();
}

bool PakIndex::ensureFd() {
    if (fd_ >= 0) return true;
    if (!records_ || !path_[0]) return false;
    const int fd = pakOpenRetry(path_);  // + one retry after a rare pak gave one back
    if (fd < 0) {
        pakLogOpenFail(path_, fd);
        return false;
    }
    pakNoteOpen(fd, path_);
    fd_ = fd;
    lastUseMs_ = logUptimeMs();
    if (rare_) pakFdSetOwner(fd_, this, &PakIndex::fdLastUse, &PakIndex::fdRelease);
    return true;
}

// see psp_pak_index.hpp.
void PakIndex::setRare(bool on) {
    rare_ = on;
    if (fd_ >= 0) pakFdSetOwner(fd_, on ? this : nullptr, &PakIndex::fdLastUse, &PakIndex::fdRelease);
}

uint32_t PakIndex::fdLastUse(void* self) { return static_cast<PakIndex*>(self)->lastUseMs_; }
void PakIndex::fdRelease(void* self) { static_cast<PakIndex*>(self)->releaseFd(); }

bool PakIndex::find(const char* name, PakEntry& out) {
    if (!records_ || !name) return false;
    lastUseMs_ = logUptimeMs();
    ++lookups_;
    PakDiskEntry d{};
    PakDiskEntry* scratch = blocks_ + 2u * step_;
    const bool hit = pakIndexLookup(
        name, records_, step_, fences_, [this](uint32_t i) { return fenceName(i); },
        [this](uint32_t first, uint32_t n, PakDiskEntry* dst) { return readBlock(first, n, dst); }, scratch, d);
    if (!hit) return false;
    std::memcpy(name_, d.name, sizeof(name_));
    name_[sizeof(name_) - 1] = '\0';
    out = PakEntry{name_, d.offset, d.size, d.kind};
    return true;
}

void* PakIndex::load(const PakEntry& e) {
    if (!records_) return nullptr;
    if (void* parked = pakPrefetchTake(fileId_, e.offset)) return parked;  // read ahead by the streamer
    lastUseMs_ = logUptimeMs();
    if (fd_ < 0 && !ensureFd()) return nullptr;
    // the world arena first for a world index (setWorldArena: NPC models), newlib otherwise.
    void* block = worldArena_ ? worldArenaAlloc((e.size + 63u) & ~63u, 64) : nullptr;
    if (!block) block = memalign(64, (e.size + 63u) & ~63u);
    if (!block) {
        PSP_LOGE(tag::Mem, "pakidx_alloc_fail size=%u", static_cast<unsigned>(e.size));
        return nullptr;
    }
    if (!pakRead(fd_, e.offset, block, e.size)) {
        PSP_LOGE(tag::Fs, "pakidx_read_fail offset=%u size=%u", static_cast<unsigned>(e.offset),
                 static_cast<unsigned>(e.size));
        std::free(block);
        return nullptr;
    }
    sceKernelDcacheWritebackRange(block, (e.size + 63u) & ~63u);
    return block;
}

} // namespace wowee::platform::psp
