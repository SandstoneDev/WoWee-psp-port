// The file descriptor budget - who holds the Memory
// Stick driver's descriptors, how many are left, and which "rare" holders give theirs back.
//
// The PSP-3000's card driver gives the game ~10-11 descriptors; at world entry 3-4 are free (fd_probe)
// and later work added more files that want one (transports.pak, gameobjects_wmo.pak, emblems.pak, bigwmo/*.pak). An
// open past the limit fails with 0x80020320 (SCE_KERNEL_ERROR_MFILE): a window without its art, a WMO not shown, a
// track not played (all seen on hardware). The table here is the one psp_pak.cpp already kept (pakNoteOpen /
// pakNoteClose, pak_open_list), now with every holder in it (the log, the music, the sound bank, the minimap, ...)
// and, for the rare paks (Pak::setRare / PakIndex::setRare - read now and then by the main thread), how to ask for
// the descriptor back:
//   - capacity: the free descriptors fd_probe counted at world entry + those open then (pakFdSetCapacity);
//   - tight: one descriptor or none left - a rare pak gives its descriptor back after 1 s without reads (not 5 s);
//   - pressure: an open failed with MFILE in the last 3 s - a rare pak idle for 250 ms gives it back at once
//     (the main thread's opens retry once after that; the other threads' opens retry on their own schedule);
// Pure logic, no PSP headers (psp_pak.cpp wraps it in its lock): tools/psp/host_tests/memory_budget_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

// A rare holder's hooks: the time of its last read (ms, logUptimeMs) and "give the descriptor back now".
using FdLastUseFn = uint32_t (*)(void* owner);
using FdReleaseFn = void (*)(void* owner);

class FdRegistry {
public:
    static constexpr int kSlots = 48;
    static constexpr uint32_t kPressureMs = 3000;  // an MFILE counts as pressure this long
    static constexpr uint32_t kPressureIdleMs = 250;
    static constexpr uint32_t kTightIdleMs = 1000;

    struct Slot {
        int fd = 0;  // > 0 used
        char name[28] = {};
        void* owner = nullptr;  // a rare holder (nullptr: not asked)
        FdLastUseFn lastUse = nullptr;
        FdReleaseFn release = nullptr;
    };
    struct Pick {
        void* owner;
        FdReleaseFn release;
        int fd;
        char name[28];
    };

    void noteOpen(int fd, const char* path) {
        if (fd <= 0) return;
        const char* base = std::strrchr(path ? path : "", '/');
        base = base ? base + 1 : (path ? path : "?");
        for (Slot& s : slots_) {
            if (s.fd > 0) continue;
            s = Slot{};
            s.fd = fd;
            std::snprintf(s.name, sizeof(s.name), "%s", base);
            ++opens_;
            return;
        }
        ++overflow_;
    }
    void noteClose(int fd) {
        if (fd <= 0) return;
        for (Slot& s : slots_)
            if (s.fd == fd) s = Slot{};
    }
    // A rare holder for the open descriptor fd (nullptr owner: not rare any more).
    void setOwner(int fd, void* owner, FdLastUseFn lastUse, FdReleaseFn release) {
        for (Slot& s : slots_)
            if (s.fd == fd && fd > 0) {
                s.owner = owner;
                s.lastUse = owner ? lastUse : nullptr;
                s.release = owner ? release : nullptr;
            }
    }
    int openCount() const {
        int n = 0;
        for (const Slot& s : slots_) n += s.fd > 0 ? 1 : 0;
        return n;
    }
    int rareOpen() const {
        int n = 0;
        for (const Slot& s : slots_) n += (s.fd > 0 && s.owner) ? 1 : 0;
        return n;
    }

    void setCapacity(int cap) { capacity_ = cap; }
    int capacity() const { return capacity_; }
    // -1 while the capacity is not known (before world entry's fd_probe).
    int freeFds() const { return capacity_ > 0 ? capacity_ - openCount() : -1; }
    bool tight() const { return capacity_ > 0 && freeFds() <= 1; }

    void notePressure(uint32_t nowMs) {
        pressureMs_ = nowMs;
        pressureSeen_ = true;
        ++mfiles_;
    }
    bool pressure(uint32_t nowMs) const { return pressureSeen_ && nowMs - pressureMs_ < kPressureMs; }
    // The idle time after which a rare holder gives its descriptor back (its own rule `idleMs`, shortened).
    uint32_t idleFor(uint32_t nowMs, uint32_t idleMs) const {
        if (pressure(nowMs) && idleMs > kPressureIdleMs) return kPressureIdleMs;
        if (tight() && idleMs > kTightIdleMs) return kTightIdleMs;
        return idleMs;
    }

    // Up to `max` rare holders idle for at least minIdleMs, the longest idle first (the caller releases them outside
    // its lock - release() closes through noteClose).
    int pickRelieve(uint32_t nowMs, uint32_t minIdleMs, Pick* out, int max) const {
        int n = 0;
        bool taken[kSlots] = {};
        while (n < max) {
            int best = -1;
            uint32_t bestIdle = 0;
            for (int i = 0; i < kSlots; ++i) {
                const Slot& s = slots_[i];
                if (taken[i] || s.fd <= 0 || !s.owner || !s.release) continue;
                const uint32_t last = s.lastUse ? s.lastUse(s.owner) : 0u;
                const uint32_t idle = nowMs - last;
                if (idle < minIdleMs) continue;
                if (best < 0 || idle > bestIdle) {
                    best = i;
                    bestIdle = idle;
                }
            }
            if (best < 0) break;
            taken[best] = true;
            Pick& p = out[n++];
            p.owner = slots_[best].owner;
            p.release = slots_[best].release;
            p.fd = slots_[best].fd;
            std::memcpy(p.name, slots_[best].name, sizeof(p.name));
        }
        return n;
    }

    // "name,name,..." of the open descriptors (rare ones marked '*'); returns how many.
    int list(char* buf, size_t cap) const {
        int n = 0;
        size_t len = 0;
        if (cap) buf[0] = '\0';
        for (const Slot& s : slots_) {
            if (s.fd <= 0) continue;
            ++n;
            if (cap && len + 24 < cap) {
                const int w = std::snprintf(buf + len, cap - len, "%s%s%s", len ? "," : "", s.name, s.owner ? "*" : "");
                if (w > 0) len += static_cast<size_t>(w);
            }
        }
        return n;
    }

    uint32_t opens() const { return opens_; }
    uint32_t mfiles() const { return mfiles_; }
    uint32_t overflow() const { return overflow_; }
    uint32_t relieved() const { return relieved_; }
    void noteRelieved(uint32_t n) { relieved_ += n; }

private:
    Slot slots_[kSlots];
    int capacity_ = 0;
    uint32_t pressureMs_ = 0;
    bool pressureSeen_ = false;
    uint32_t opens_ = 0, mfiles_ = 0, overflow_ = 0, relieved_ = 0;
};

}  // namespace wowee::platform::psp
