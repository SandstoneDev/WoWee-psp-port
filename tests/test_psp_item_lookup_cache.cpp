#include "rendering/psp/psp_item_lookup_cache.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

using wowee::rendering::psp::ItemLookupCache;

int main() {
    ItemLookupCache cache;
    unsigned tableReads = 0;
    auto resolve = [&](uint32_t id) {
        if (const auto* entry = cache.find(id)) return *entry;
        ++tableReads;  // simulate an Item.dbc read across many distinct blocks
        const uint32_t display = id == 123456 ? 0 : id + 700;
        cache.store(id, display, display ? 13 : 0, display ? 2 : 255);
        return *cache.find(id);
    };
    // More distinct table blocks than the bank's four-block disk cache holds.
    // Repeated crowd frames must preserve values and require no further reads.
    for (uint32_t id = 1; id <= 80; ++id) resolve(id * 513);
    const auto coldReads = tableReads;
    for (int frame = 0; frame < 120; ++frame) {
        for (uint32_t id = 1; id <= 80; ++id) {
            const auto item = resolve(id * 513);
            assert(item.displayId == id * 513 + 700);
            assert(item.inventoryType == 13 && item.sheath == 2);
        }
    }
    assert(tableReads == coldReads);
    // Missing entries are valid cached answers, including sheath defaults.
    assert(resolve(123456).displayId == 0);
    const auto absentReads = tableReads;
    for (int i = 0; i < 120; ++i) {
        const auto missing = resolve(123456);
        assert(missing.displayId == 0 && missing.inventoryType == 0 && missing.sheath == 255);
    }
    assert(tableReads == absentReads);
    // Eviction must never return another item's value.
    for (uint32_t id = 1; id <= 10000; ++id) cache.store(id, id + 9, 13, 2);
    for (uint32_t id = 1; id <= 10000; ++id)
        if (const auto* item = cache.find(id)) assert(item->itemId == id && item->displayId == id + 9);
    // Reopening a different asset pack invalidates positive and negative answers.
    cache.clear();
    assert(!cache.find(10000) && !cache.find(123456) && !cache.find(0));
    cache.store(0, 1, 2, 3);
    assert(!cache.find(0));
    std::printf("Crowd item lookup cache passed; size=%zu bytes; warm lookups required no table reads.\n", sizeof(cache));
}
