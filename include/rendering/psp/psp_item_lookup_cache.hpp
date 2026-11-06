#pragma once

#include <array>
#include <cstdint>

namespace wowee::rendering::psp {

// Item.dbc values are immutable for the lifetime of an open items.pak.
// Keep resolved entries (including absent entries) across table-block eviction.
// Four ways avoid forcing two frequently used items to evict each other.
class ItemLookupCache {
public:
    struct Entry {
        uint32_t itemId = 0;
        uint32_t displayId = 0;
        uint8_t inventoryType = 0;
        uint8_t sheath = 0xFF;
    };

    const Entry* find(uint32_t itemId) const {
        if (!itemId) return nullptr;
        for (const auto& entry : sets_[setOf(itemId)])
            if (entry.itemId == itemId) return &entry;
        return nullptr;
    }

    void store(uint32_t itemId, uint32_t displayId, uint8_t inventoryType, uint8_t sheath) {
        if (!itemId) return;
        const uint32_t set = setOf(itemId);
        for (auto& entry : sets_[set]) {
            if (!entry.itemId || entry.itemId == itemId) {
                entry = Entry{itemId, displayId, inventoryType, sheath};
                return;
            }
        }
        sets_[set][next_[set]] = Entry{itemId, displayId, inventoryType, sheath};
        next_[set] = (next_[set] + 1u) % kWays;
    }

    void clear() {
        for (auto& set : sets_)
            for (auto& entry : set) entry = Entry{};
        next_.fill(0);
    }

private:
    static constexpr uint32_t kSets = 64, kWays = 4;
    static uint32_t setOf(uint32_t id) { return (id * uint32_t{2654435761u}) >> 26; }
    std::array<std::array<Entry, kWays>, kSets> sets_{};
    std::array<uint8_t, kSets> next_{};
};

static_assert(sizeof(ItemLookupCache) <= 4096, "The PSP item lookup cache must stay within 4 KB");

}  // namespace wowee::rendering::psp
