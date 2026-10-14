// Memory diet: the terrain's memory pool - pure C++ (no PSP headers;
// tools/psp/host_tests/heap_frag_host_test.sh runs it).
//
// PSP: why. The chunk-texture window (psp_terrain_window.hpp) took ~900 blocks of 5.9 KB from newlib and gave them back
// as the player moved, the 3x3 window took and freed a 292 KB vertex block per tile, every job a 87 KB head - all
// interleaved with the doodads, WMO chunks, NPC models and the game's small objects. The emulator autopilot of
// one test build had 14 MB free and a 640 KB largest block after a few teleports. The pool is ONE block taken at boot (the heap
// empty, it lies low) and kept for the whole run: fixed slots for the chunk textures (5888 bytes: CLUT + 64x64 + mips),
// for the tiles (the Tile record + its vertex block), the job's head, and the texture rows in flight. Terrain never
// takes or frees newlib memory again, so it can neither fragment the heap nor be starved by it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::rendering::psp {

struct TerrainPoolLayout {
    uint32_t texSlots = 0;        // chunk textures resident at most
    uint32_t texSlotBytes = 5888;
    uint32_t tileSlots = 0;       // terrain tiles resident at most (the 3x3 + the ones leaving)
    uint32_t tileSlotBytes = 0;   // the Tile record (16-aligned) + its vertex block
    uint32_t headBytes = 0;       // a tile's head + the v4 detail table (one job at a time)
    uint32_t runSlots = 0;        // texture rows in flight (terraintexreads)
    uint32_t runSlotBytes = 0;    // 16 chunks x texSlotBytes
};

// Chunks whose sphere can lie within keepYd of the player: the squares of 33.3 yd within keepYd + their radius (~24),
// plus 5% - the window's own count (host test: 864-872 at 529 yd; emulator 938-970 at entry).
inline uint32_t terrainTexSlotsFor(float keepYd) {
    const double s = 533.33333 / 16.0, r = static_cast<double>(keepYd) + 24.0;
    const double n = (3.14159265 * r * r + 4.0 * r * s + s * s) / (s * s);
    uint32_t v = static_cast<uint32_t>(n * 1.05) + 1u;
    if (v < 64u) v = 64u;
    if (v > 2304u) v = 2304u;
    return v;
}

class TerrainPool {
public:
    static uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }
    static size_t bytesFor(const TerrainPoolLayout& l) {
        return static_cast<size_t>(l.texSlots) * align16(l.texSlotBytes) +
               static_cast<size_t>(l.tileSlots) * align16(l.tileSlotBytes) + align16(l.headBytes) +
               static_cast<size_t>(l.runSlots) * align16(l.runSlotBytes) + 16u;
    }

    // mem: bytesFor(l) bytes (any alignment; slots are 16-aligned inside).
    bool init(void* mem, size_t bytes, const TerrainPoolLayout& l) {
        if (!mem || bytes < bytesFor(l)) return false;
        l_ = l;
        uintptr_t a = (reinterpret_cast<uintptr_t>(mem) + 15u) & ~static_cast<uintptr_t>(15u);
        tex_ = reinterpret_cast<uint8_t*>(a);
        a += static_cast<uintptr_t>(l.texSlots) * align16(l.texSlotBytes);
        tile_ = reinterpret_cast<uint8_t*>(a);
        a += static_cast<uintptr_t>(l.tileSlots) * align16(l.tileSlotBytes);
        head_ = reinterpret_cast<uint8_t*>(a);
        a += align16(l.headBytes);
        run_ = reinterpret_cast<uint8_t*>(a);
        end_ = static_cast<const uint8_t*>(mem) + bytes;
        texFree_.clear();
        tileFree_.clear();
        runFree_.clear();
        texFree_.reserve(l.texSlots);
        tileFree_.reserve(l.tileSlots);
        runFree_.reserve(l.runSlots);
        for (uint32_t i = l.texSlots; i-- > 0;) texFree_.push_back(i);  // lowest first
        for (uint32_t i = l.tileSlots; i-- > 0;) tileFree_.push_back(i);
        for (uint32_t i = l.runSlots; i-- > 0;) runFree_.push_back(i);
        begin_ = static_cast<const uint8_t*>(mem);
        return true;
    }
    bool ready() const { return tex_ != nullptr; }
    const TerrainPoolLayout& layout() const { return l_; }
    bool contains(const void* p) const {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        return tex_ && q >= begin_ && q < end_;
    }

    uint8_t* allocTex() { return take(texFree_, tex_, l_.texSlotBytes); }
    bool freeTex(void* p) { return give(texFree_, tex_, l_.texSlotBytes, l_.texSlots, p); }
    uint32_t texFree() const { return static_cast<uint32_t>(texFree_.size()); }

    uint8_t* allocTile() { return take(tileFree_, tile_, l_.tileSlotBytes); }
    bool freeTile(void* p) { return give(tileFree_, tile_, l_.tileSlotBytes, l_.tileSlots, p); }
    uint32_t tileFree() const { return static_cast<uint32_t>(tileFree_.size()); }

    uint8_t* allocRun() { return take(runFree_, run_, l_.runSlotBytes); }
    bool freeRun(void* p) { return give(runFree_, run_, l_.runSlotBytes, l_.runSlots, p); }
    uint32_t runFree() const { return static_cast<uint32_t>(runFree_.size()); }

    uint8_t* head() const { return head_; }
    uint32_t badFrees() const { return badFrees_; }
    // world_flush: slots handed out now, and every slot free again (the caller has dropped every
    // tile and stopped every read - the old place's leftovers are lost slots, not live data). No allocation (capacity
    // was reserved by init).
    uint32_t texUsed() const { return l_.texSlots - texFree(); }
    uint32_t tileUsed() const { return l_.tileSlots - tileFree(); }
    uint32_t runUsed() const { return l_.runSlots - runFree(); }
    void resetFree() {
        if (!tex_) return;
        texFree_.clear();
        tileFree_.clear();
        runFree_.clear();
        for (uint32_t i = l_.texSlots; i-- > 0;) texFree_.push_back(i);
        for (uint32_t i = l_.tileSlots; i-- > 0;) tileFree_.push_back(i);
        for (uint32_t i = l_.runSlots; i-- > 0;) runFree_.push_back(i);
    }

private:
    static uint8_t* take(std::vector<uint32_t>& free, uint8_t* base, uint32_t slotBytes) {
        if (free.empty()) return nullptr;
        const uint32_t i = free.back();
        free.pop_back();
        return base + static_cast<size_t>(i) * align16(slotBytes);
    }
    bool give(std::vector<uint32_t>& free, uint8_t* base, uint32_t slotBytes, uint32_t slots, void* p) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        const size_t stride = align16(slotBytes);
        if (!base || q < base || q >= base + stride * slots || (static_cast<size_t>(q - base) % stride) != 0 ||
            free.size() >= slots) {
            ++badFrees_;
            return false;
        }
        free.push_back(static_cast<uint32_t>(static_cast<size_t>(q - base) / stride));  // (capacity reserved)
        return true;
    }

    TerrainPoolLayout l_{};
    uint8_t* tex_ = nullptr;
    uint8_t* tile_ = nullptr;
    uint8_t* head_ = nullptr;
    uint8_t* run_ = nullptr;
    const uint8_t* begin_ = nullptr;
    const uint8_t* end_ = nullptr;
    std::vector<uint32_t> texFree_, tileFree_, runFree_;
    uint32_t badFrees_ = 0;
};

}  // namespace wowee::rendering::psp
