// Memory diet (docs/PSP_PORT_ARCHITECTURE.md): the small-object arena - pure C++ (no PSP headers;
// tools/psp/host_tests/heap_frag_host_test.sh builds exactly this code).
//
// PSP: why. The emulator autopilot of the memory diet (its session log): after the teleports newlib had 14-39 MB
// free and no 2 MB block - transfer_unloaded: 1.5 MB of live objects in a 41 MB arena, largest block 1.7 MB. The
// world's big blocks (terrain, doodad models, WMO chunks, NPC models, pak reads) come and go; the game's small
// long-lived objects (strings, map / list nodes, small vectors: sessions, caches, UI) land in whatever hole is free
// at that moment and stay - every freed big block leaves a few of them behind as pins. Kept in their own arena they
// pin nothing: newlib keeps the big blocks, its holes coalesce when the world goes.
//
// SmallArena: one region of 16 KB pages; a page serves one size class (16 .. 4096 bytes in 16 classes) and keeps its
// own free list; a class keeps the pages with free blocks in a list. alloc / free are O(1) (no search - newlib's bins
// would be no faster), a page whose blocks are all free goes back to the free pages (another class may take it).
// A bitmap per page marks the live blocks: a double free or a pointer that is no block is counted and ignored (the
// arena's lists stay intact). Not locked: the PSP glue (psp_bigheap.cpp) locks around it. No allocation inside.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wowee::platform::psp {

class SmallArena {
public:
    static constexpr uint32_t kPage = 16384;
    static constexpr uint32_t kClasses = 16;
    static constexpr uint32_t kMaxSize = 4096;
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    static constexpr uint16_t kFreePage = 0xFFFF;

    struct Stats {
        uint32_t usedBytes = 0;   // class bytes of the live blocks
        uint32_t peakUsedBytes = 0;
        uint32_t pages = 0;       // pages of the region
        uint32_t pagesUsed = 0;   // pages serving a class now
        uint32_t peakPagesUsed = 0;
        uint32_t allocs = 0, frees = 0;
        uint32_t fails = 0;       // no free page for a class with no free block
        uint32_t badFrees = 0;    // a double free / not a block (ignored)
    };

    static uint32_t classSize(uint32_t c) {
        static constexpr uint16_t k[kClasses] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
                                                 1536, 2048, 3072, 4096};
        return k[c];
    }
    // The class of a request of n bytes (n <= kMaxSize), kClasses when it does not fit any.
    static uint32_t classOf(size_t n) {
        for (uint32_t c = 0; c < kClasses; ++c)
            if (n <= classSize(c)) return c;
        return kClasses;
    }

    // mem: the region (16-byte aligned internally); the page table and bitmaps live at its start.
    bool init(void* mem, size_t bytes) {
        base_ = nullptr;
        stats_ = Stats{};
        if (!mem || bytes < 64u * 1024u) return false;
        uintptr_t a = (reinterpret_cast<uintptr_t>(mem) + 15u) & ~static_cast<uintptr_t>(15u);
        size_t avail = bytes - static_cast<size_t>(a - reinterpret_cast<uintptr_t>(mem));
        // pages * (kPage + sizeof(Page)) <= avail, the page area 16-aligned after the table
        uint32_t pages = static_cast<uint32_t>(avail / (kPage + sizeof(Page)));
        while (pages && (((pages * sizeof(Page) + 15u) & ~15u) + static_cast<size_t>(pages) * kPage > avail)) --pages;
        if (!pages) return false;
        meta_ = reinterpret_cast<Page*>(a);
        base_ = reinterpret_cast<uint8_t*>(a + ((pages * sizeof(Page) + 15u) & ~15u));
        pages_ = pages;
        regionBegin_ = reinterpret_cast<const uint8_t*>(mem);
        regionEnd_ = regionBegin_ + bytes;
        for (uint32_t i = 0; i < pages; ++i) {
            meta_[i] = Page{};
            meta_[i].next = i + 1 < pages ? i + 1 : kNone;  // the free-page list
        }
        freePages_ = 0;
        for (uint32_t& h : partial_) h = kNone;
        stats_.pages = pages;
        return true;
    }

    bool ready() const { return base_ != nullptr; }
    const Stats& stats() const { return stats_; }
    uint32_t pageBytes() const { return pages_ * kPage; }
    uint32_t freeBytes() const { return pageBytes() - stats_.usedBytes; }  // incl. the free part of class pages
    // The whole region (table included) - a free() of any pointer in it belongs here.
    bool contains(const void* p) const {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        return base_ && q >= regionBegin_ && q < regionEnd_;
    }

    void* alloc(size_t n) {
        if (!base_) return nullptr;
        const uint32_t c = classOf(n ? n : 1);
        if (c >= kClasses) return nullptr;
        uint32_t pi = partial_[c];
        if (pi == kNone) {
            pi = freePages_;
            if (pi == kNone) {
                ++stats_.fails;
                return nullptr;
            }
            freePages_ = meta_[pi].next;
            Page& g = meta_[pi];
            g = Page{};
            g.cls = static_cast<uint16_t>(c);
            std::memset(g.live, 0, sizeof(g.live));
            pushPartial(c, pi);
            ++stats_.pagesUsed;
            if (stats_.pagesUsed > stats_.peakPagesUsed) stats_.peakPagesUsed = stats_.pagesUsed;
        }
        Page& g = meta_[pi];
        const uint32_t sz = classSize(c);
        uint32_t slot;
        if (g.freeHead != kNone16) {
            slot = g.freeHead;
            uint16_t next;
            std::memcpy(&next, block(pi, slot, sz), sizeof(next));
            g.freeHead = next;
        } else {
            slot = g.bump++;
        }
        ++g.used;
        g.live[slot >> 3] = static_cast<uint8_t>(g.live[slot >> 3] | (1u << (slot & 7u)));
        if (g.used == perPage(c)) removePartial(c, pi);  // full
        stats_.usedBytes += sz;
        if (stats_.usedBytes > stats_.peakUsedBytes) stats_.peakUsedBytes = stats_.usedBytes;
        ++stats_.allocs;
        return block(pi, slot, sz);
    }

    // p must be inside (contains); false: not a live block (counted, ignored).
    bool free(void* p) {
        uint32_t pi, slot;
        if (!locate(p, pi, slot)) {
            ++stats_.badFrees;
            return false;
        }
        Page& g = meta_[pi];
        const uint32_t c = g.cls, sz = classSize(c);
        const bool wasFull = g.used == perPage(c);
        g.live[slot >> 3] = static_cast<uint8_t>(g.live[slot >> 3] & ~(1u << (slot & 7u)));
        const uint16_t next = g.freeHead;
        std::memcpy(block(pi, slot, sz), &next, sizeof(next));
        g.freeHead = static_cast<uint16_t>(slot);
        --g.used;
        stats_.usedBytes -= sz;
        ++stats_.frees;
        if (wasFull) pushPartial(c, pi);
        if (g.used == 0) {  // the whole page back
            removePartial(c, pi);
            g.cls = kFreePage;
            g.next = freePages_;
            freePages_ = pi;
            --stats_.pagesUsed;
        }
        return true;
    }

    // The block's usable bytes (its class), 0 when p is no live block.
    size_t usable(const void* p) const {
        uint32_t pi, slot;
        if (!locate(p, pi, slot)) return 0;
        return classSize(meta_[pi].cls);
    }

    // Consistency (host test): the counters agree with the pages, every list is well formed.
    bool check() const {
        uint32_t used = 0, pagesUsed = 0, freeCount = 0;
        for (uint32_t pi = freePages_; pi != kNone; pi = meta_[pi].next) {
            if (pi >= pages_ || meta_[pi].cls != kFreePage || ++freeCount > pages_) return false;
        }
        for (uint32_t i = 0; i < pages_; ++i) {
            const Page& g = meta_[i];
            if (g.cls == kFreePage) continue;
            if (g.cls >= kClasses) return false;
            ++pagesUsed;
            uint32_t live = 0;
            for (uint32_t s = 0; s < perPage(g.cls); ++s) live += (g.live[s >> 3] >> (s & 7u)) & 1u;
            if (live != g.used || g.used == 0) return false;
            used += live * classSize(g.cls);
        }
        if (freeCount + pagesUsed != pages_) return false;
        for (uint32_t c = 0; c < kClasses; ++c)
            for (uint32_t pi = partial_[c], n = 0; pi != kNone; pi = meta_[pi].next)
                if (pi >= pages_ || meta_[pi].cls != c || meta_[pi].used >= perPage(c) || ++n > pages_) return false;
        return used == stats_.usedBytes && pagesUsed == stats_.pagesUsed;
    }

private:
    static constexpr uint16_t kNone16 = 0xFFFF;
    struct Page {
        uint16_t cls = kFreePage;
        uint16_t used = 0;
        uint16_t bump = 0;               // blocks never handed out start here
        uint16_t freeHead = kNone16;     // a freed block's first 2 bytes: the next
        uint32_t prev = kNone, next = kNone;  // the class's partial list / the free-page list
        uint8_t live[128] = {};          // up to 1024 blocks (16-byte class)
    };
    static uint32_t perPage(uint32_t c) { return kPage / classSize(c); }
    uint8_t* block(uint32_t pi, uint32_t slot, uint32_t sz) const { return base_ + pi * kPage + slot * sz; }
    bool locate(const void* p, uint32_t& pi, uint32_t& slot) const {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        if (!base_ || q < base_ || q >= base_ + static_cast<size_t>(pages_) * kPage) return false;
        const uint32_t off = static_cast<uint32_t>(q - base_);
        pi = off / kPage;
        const Page& g = meta_[pi];
        if (g.cls >= kClasses) return false;
        const uint32_t sz = classSize(g.cls), in = off % kPage;
        if (in % sz) return false;
        slot = in / sz;
        if (slot >= perPage(g.cls) || !((g.live[slot >> 3] >> (slot & 7u)) & 1u)) return false;
        return true;
    }
    void pushPartial(uint32_t c, uint32_t pi) {
        Page& g = meta_[pi];
        g.prev = kNone;
        g.next = partial_[c];
        if (partial_[c] != kNone) meta_[partial_[c]].prev = pi;
        partial_[c] = pi;
    }
    void removePartial(uint32_t c, uint32_t pi) {
        Page& g = meta_[pi];
        if (g.prev != kNone) meta_[g.prev].next = g.next;
        else if (partial_[c] == pi) partial_[c] = g.next;
        else return;  // not in the list
        if (g.next != kNone) meta_[g.next].prev = g.prev;
        g.prev = g.next = kNone;
    }

    Page* meta_ = nullptr;
    uint8_t* base_ = nullptr;
    const uint8_t* regionBegin_ = nullptr;
    const uint8_t* regionEnd_ = nullptr;
    uint32_t pages_ = 0;
    uint32_t freePages_ = kNone;
    uint32_t partial_[kClasses] = {};
    Stats stats_;
};

}  // namespace wowee::platform::psp
