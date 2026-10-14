// The reader of strings.pak's "strings" entry (WSTR, tools/psp/build_strings_pak.py) - pure, shared
// with the host test. It reads only what the EBOOT uses: the index is walked in 16 KB slices against the sorted
// hashes of the wanted keys (no 147 KB index in RAM), then the wanted texts are read in runs (texts closer than
// 4 KB in the pool share one read; the pool is in key order, so one area's keys are close) and appended to the
// caller's pool.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::platform::psp::text {

// Reads `size` bytes at `offset` of the entry into dst; false on a failed read.
using WstrRead = bool (*)(void* ctx, uint32_t offset, void* dst, uint32_t size);

struct WstrWant {
    uint64_t hash;  // fnv1a64 of the key
    uint32_t slot;  // the caller's index for it
};

struct WstrResult {
    bool ok = false;      // the header was valid
    char locale[5] = {};  // the pak's locale ("ruRU")
    uint32_t count = 0;   // keys in the pak
    uint32_t found = 0;   // wanted keys found
    uint32_t reads = 0;   // card reads made
};

// For every wanted hash present in the entry: its text (NUL-terminated) appended to `pool`, at[slot] = its offset in
// the pool. Slots not found keep their value (the caller's "missing" mark).
inline WstrResult wstrLoad(WstrRead read, void* ctx, uint32_t entrySize, std::vector<WstrWant> wants, std::string& pool,
                           std::vector<int32_t>& at) {
    WstrResult r;
    uint8_t head[24];
    if (entrySize < sizeof(head) || !read(ctx, 0, head, sizeof(head))) return r;
    ++r.reads;
    uint16_t version = 0;
    uint32_t count = 0, poolOff = 0, poolSize = 0;
    std::memcpy(&version, head + 4, 2);
    std::memcpy(&count, head + 12, 4);
    std::memcpy(&poolOff, head + 16, 4);
    std::memcpy(&poolSize, head + 20, 4);
    if (std::memcmp(head, "WSTR", 4) != 0 || version != 1 || static_cast<uint64_t>(count) * 16u + 24u > poolOff ||
        static_cast<uint64_t>(poolOff) + poolSize > entrySize)
        return r;
    std::memcpy(r.locale, head + 8, 4);
    r.count = count;
    r.ok = true;

    // 1. The index against the wanted hashes, both sorted: one pass over the index in slices.
    std::sort(wants.begin(), wants.end(), [](const WstrWant& a, const WstrWant& b) { return a.hash < b.hash; });
    struct Hit {
        uint32_t off, len, slot;
    };
    std::vector<Hit> hits;
    hits.reserve(wants.size());
    constexpr uint32_t kSlice = 1024;  // records a read (16 KB)
    std::vector<uint8_t> buf(kSlice * 16u);
    size_t w = 0;
    for (uint32_t base = 0; base < count && w < wants.size(); base += kSlice) {
        const uint32_t n = std::min(kSlice, count - base);
        if (!read(ctx, 24u + base * 16u, buf.data(), n * 16u)) return r;
        ++r.reads;
        uint64_t last = 0;
        std::memcpy(&last, buf.data() + (n - 1) * 16u, 8);
        for (uint32_t i = 0; i < n && w < wants.size(); ++i) {
            uint64_t h = 0;
            std::memcpy(&h, buf.data() + i * 16u, 8);
            while (w < wants.size() && wants[w].hash < h) ++w;  // not in the pak
            while (w < wants.size() && wants[w].hash == h) {
                Hit hit;
                std::memcpy(&hit.off, buf.data() + i * 16u + 8, 4);
                std::memcpy(&hit.len, buf.data() + i * 16u + 12, 4);
                hit.slot = wants[w].slot;
                if (static_cast<uint64_t>(hit.off) + hit.len < poolSize) hits.push_back(hit);
                ++w;
            }
        }
        while (w < wants.size() && wants[w].hash <= last) ++w;
    }

    // 2. The texts, in runs.
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.off < b.off; });
    size_t total = pool.size();
    for (const Hit& h : hits) total += h.len + 1u;
    pool.reserve(total);
    constexpr uint32_t kGap = 4096, kRunMax = 32u * 1024u;
    std::vector<char> run;
    for (size_t i = 0; i < hits.size();) {
        const uint32_t start = hits[i].off;
        uint32_t end = hits[i].off + hits[i].len;
        size_t j = i + 1;
        while (j < hits.size() && hits[j].off <= end + kGap && hits[j].off + hits[j].len - start <= kRunMax) {
            end = std::max(end, hits[j].off + hits[j].len);
            ++j;
        }
        run.resize(end - start);
        if (!run.empty() && !read(ctx, poolOff + start, run.data(), static_cast<uint32_t>(run.size()))) return r;
        ++r.reads;
        for (size_t k = i; k < j; ++k) {
            if (hits[k].slot >= at.size()) continue;
            at[hits[k].slot] = static_cast<int32_t>(pool.size());
            pool.append(run.data() + (hits[k].off - start), hits[k].len);
            pool += '\0';
            ++r.found;
        }
        i = j;
    }
    return r;
}

}  // namespace wowee::platform::psp::text
