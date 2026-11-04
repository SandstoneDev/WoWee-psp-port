// Helpers shared by the PSP asset converters (PC side).
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace wowee::pspconv {

// Game path -> loose file path written by asset_extract (lowercase, '/').
inline std::string lower(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

template <typename T>
void put(std::vector<uint8_t>& out, T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

inline void alignTo(std::vector<uint8_t>& out, size_t a) {
    out.resize((out.size() + a - 1) / a * a, 0);
}

struct Rgb {
    float r = 0.5f, g = 0.5f, b = 0.5f, a = 1.0f;
};

inline int quant(float v, int max) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * max + 0.5f); }

// GE 5650: R in bits 0-4, G 5-10, B 11-15.
inline uint16_t to565(Rgb c) {
    return static_cast<uint16_t>(quant(c.r, 31) | (quant(c.g, 63) << 5) | (quant(c.b, 31) << 11));
}

// GE 8888 as a little-endian u32: 0xAABBGGRR.
inline uint32_t to8888(Rgb c) {
    return static_cast<uint32_t>(quant(c.r, 255)) | (static_cast<uint32_t>(quant(c.g, 255)) << 8) |
           (static_cast<uint32_t>(quant(c.b, 255)) << 16) | (static_cast<uint32_t>(quant(c.a, 255)) << 24);
}

// Median cut over r, g, b, a: split the box with the widest channel range at
// its median until 256 boxes; each pixel takes its box's index, the palette
// is the box mean. Constant alpha never wins a split, so opaque input gives
// the same result as an RGB cut.
inline void quantize(const std::vector<Rgb>& px, std::vector<uint8_t>& idx, std::array<Rgb, 256>& pal) {
    struct Box {
        std::vector<int> items;
    };
    std::vector<Box> boxes(1);
    boxes[0].items.resize(px.size());
    for (size_t i = 0; i < px.size(); ++i) boxes[0].items[i] = static_cast<int>(i);
    auto chan = [&](int i, int c) {
        return c == 0 ? px[i].r : c == 1 ? px[i].g : c == 2 ? px[i].b : px[i].a;
    };
    while (boxes.size() < 256) {
        int best = -1, bestChan = 0;
        float bestRange = 0;
        for (size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].items.size() < 2) continue;
            for (int c = 0; c < 4; ++c) {
                float lo = 1e9f, hi = -1e9f;
                for (int i : boxes[b].items) {
                    lo = std::min(lo, chan(i, c));
                    hi = std::max(hi, chan(i, c));
                }
                if (hi - lo > bestRange) {
                    bestRange = hi - lo;
                    best = static_cast<int>(b);
                    bestChan = c;
                }
            }
        }
        if (best < 0 || bestRange < 1.0f / 512.0f) break;
        auto& items = boxes[best].items;
        std::nth_element(items.begin(), items.begin() + items.size() / 2, items.end(),
                         [&](int a, int b) { return chan(a, bestChan) < chan(b, bestChan); });
        Box nb;
        nb.items.assign(items.begin() + items.size() / 2, items.end());
        items.resize(items.size() / 2);
        boxes.push_back(std::move(nb));
    }
    idx.assign(px.size(), 0);
    pal.fill(Rgb{0, 0, 0, 1});
    for (size_t b = 0; b < boxes.size(); ++b) {
        Rgb m{0, 0, 0, 0};
        for (int i : boxes[b].items) {
            m.r += px[i].r;
            m.g += px[i].g;
            m.b += px[i].b;
            m.a += px[i].a;
            idx[i] = static_cast<uint8_t>(b);
        }
        const float n = static_cast<float>(std::max<size_t>(1, boxes[b].items.size()));
        pal[b] = Rgb{m.r / n, m.g / n, m.b / n, m.a / n};
    }
}

// GE fast-mode swizzle: blocks of 16 bytes x 8 rows (width in bytes).
inline std::vector<uint8_t> swizzle(const std::vector<uint8_t>& linear, int widthBytes, int height) {
    std::vector<uint8_t> out(linear.size());
    size_t o = 0;
    for (int by = 0; by < height / 8; ++by)
        for (int bx = 0; bx < widthBytes / 16; ++bx)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 16; ++x) out[o++] = linear[(by * 8 + y) * widthBytes + bx * 16 + x];
    return out;
}

struct PakItem {
    std::string name;       // <= 47 chars
    uint32_t kind;          // include/platform/psp/psp_pak.hpp
    std::vector<uint8_t> blob;
};

// "WPAK" v1 (include/platform/psp/psp_pak.hpp), entries 64-byte aligned.
inline bool writePak(const std::string& path, const std::vector<PakItem>& entries) {
    auto align = [](size_t v) { return (v + 63) & ~size_t(63); };
    size_t off = align(16 + 64 * entries.size());
    std::vector<uint8_t> toc;
    std::vector<size_t> offs;
    for (const auto& e : entries) {
        char nm[48] = {};
        std::snprintf(nm, sizeof(nm), "%s", e.name.c_str());
        toc.insert(toc.end(), nm, nm + 48);
        put<uint32_t>(toc, static_cast<uint32_t>(off));
        put<uint32_t>(toc, static_cast<uint32_t>(e.blob.size()));
        put<uint32_t>(toc, e.kind);
        put<uint32_t>(toc, 0);
        offs.push_back(off);
        off = align(off + e.blob.size());
    }
    std::vector<uint8_t> file;
    file.insert(file.end(), {'W', 'P', 'A', 'K'});
    put<uint32_t>(file, 1);
    put<uint32_t>(file, static_cast<uint32_t>(entries.size()));
    put<uint32_t>(file, 16);
    file.insert(file.end(), toc.begin(), toc.end());
    for (size_t i = 0; i < entries.size(); ++i) {
        file.resize(offs[i], 0);
        file.insert(file.end(), entries[i].blob.begin(), entries[i].blob.end());
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    std::printf("wrote %s: %zu entries, %zu bytes\n", path.c_str(), entries.size(), file.size());
    return static_cast<bool>(f);
}

// A pak PakIndex (include/platform/psp/psp_pak_index.hpp) opens without its TOC in RAM -
// the same WPAK v1, the TOC sorted by name (stable) and a last raw record "~fence": every `step`-th record's
// name. Streams: add() appends the blob to `<path>.tmp` and keeps only the name, kind, size and temp offset,
// finish() writes the header, the sorted TOC, the blobs in TOC order (64-byte aligned) and the fence, then
// removes the temp file - a multi-GB pak never sits in memory. Every offset must stay below 4 GB (FAT32
// on the card: a file is < 4 GB anyway); add() refuses names >= "~fence" and longer than 47.
class SortedPakWriter {
public:
    explicit SortedPakWriter(std::string path, uint32_t step = 128) : path_(std::move(path)), step_(step ? step : 128) {
        tmp_ = std::fopen((path_ + ".tmp").c_str(), "wb+");
    }
    ~SortedPakWriter() {
        if (tmp_) std::fclose(tmp_);
        if (!finished_) std::remove((path_ + ".tmp").c_str());
    }
    SortedPakWriter(const SortedPakWriter&) = delete;
    SortedPakWriter& operator=(const SortedPakWriter&) = delete;

    bool ok() const { return tmp_ != nullptr && !failed_; }
    size_t count() const { return items_.size(); }
    uint64_t bytes() const { return tmpSize_; }

    bool add(const std::string& name, uint32_t kind, const void* data, size_t size) {
        if (!ok()) return false;
        if (name.empty() || name.size() > 47 || name >= std::string("~fence")) {
            std::fprintf(stderr, "SortedPakWriter: bad name '%s'\n", name.c_str());
            failed_ = true;
            return false;
        }
        if (size && std::fwrite(data, 1, size, tmp_) != size) {
            failed_ = true;
            return false;
        }
        items_.push_back(Item{name, kind, static_cast<uint32_t>(size), tmpSize_});
        tmpSize_ += size;
        return true;
    }
    bool add(const PakItem& e) { return add(e.name, e.kind, e.blob.data(), e.blob.size()); }

    bool finish() {
        if (!ok() || finished_) return false;
        std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
        // The fence: names of records 0, step, 2 step, ... of the final TOC ("~fence" is the last record).
        const uint32_t records = static_cast<uint32_t>(items_.size()) + 1u;
        const uint32_t fences = (records + step_ - 1u) / step_;
        std::vector<uint8_t> fence;
        const char magic[4] = {'W', 'F', 'N', 'C'};
        fence.insert(fence.end(), magic, magic + 4);
        put<uint32_t>(fence, 1);
        put<uint32_t>(fence, step_);
        put<uint32_t>(fence, fences);
        put<uint32_t>(fence, records);
        put<uint32_t>(fence, 0);
        const size_t offs = fence.size();
        fence.resize(offs + fences * 4u, 0);
        for (uint32_t i = 0; i < fences; ++i) {
            const uint32_t r = i * step_;
            const std::string& nm = r < items_.size() ? items_[r].name : std::string("~fence");
            const uint32_t o = static_cast<uint32_t>(fence.size());
            std::memcpy(fence.data() + offs + i * 4u, &o, 4);
            fence.insert(fence.end(), nm.begin(), nm.end());
            fence.push_back(0);
        }
        auto align = [](uint64_t v) { return (v + 63) & ~uint64_t(63); };
        uint64_t off = align(16 + 64ull * records);
        std::vector<uint32_t> at(items_.size());
        for (size_t i = 0; i < items_.size(); ++i) {
            at[i] = static_cast<uint32_t>(off);
            off = align(off + items_[i].size);
        }
        const uint64_t fenceAt = off;
        if (fenceAt + fence.size() >= 0xFFFFFFFFull) {
            std::fprintf(stderr, "SortedPakWriter: %s would pass 4 GB - split it\n", path_.c_str());
            failed_ = true;
            return false;
        }
        std::FILE* f = std::fopen(path_.c_str(), "wb");
        if (!f) {
            failed_ = true;
            return false;
        }
        std::vector<uint8_t> head;
        head.insert(head.end(), {'W', 'P', 'A', 'K'});
        put<uint32_t>(head, 1);
        put<uint32_t>(head, records);
        put<uint32_t>(head, 16);
        auto rec = [&](const std::string& nm, uint32_t o, uint32_t sz, uint32_t kind) {
            char n[48] = {};
            std::memcpy(n, nm.data(), std::min<size_t>(nm.size(), 47));
            head.insert(head.end(), n, n + 48);
            put<uint32_t>(head, o);
            put<uint32_t>(head, sz);
            put<uint32_t>(head, kind);
            put<uint32_t>(head, 0);
        };
        for (size_t i = 0; i < items_.size(); ++i) rec(items_[i].name, at[i], items_[i].size, items_[i].kind);
        rec("~fence", static_cast<uint32_t>(fenceAt), static_cast<uint32_t>(fence.size()), 3);
        bool good = std::fwrite(head.data(), 1, head.size(), f) == head.size();
        uint64_t pos = head.size();
        std::vector<uint8_t> buf;
        auto padTo = [&](uint64_t target) {
            static const uint8_t zeros[64] = {};
            while (good && pos < target) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(64, target - pos));
                good = std::fwrite(zeros, 1, n, f) == n;
                pos += n;
            }
        };
        std::fflush(tmp_);
        for (size_t i = 0; good && i < items_.size(); ++i) {
            padTo(at[i]);
            buf.resize(items_[i].size);
#if defined(_WIN32)
            good = _fseeki64(tmp_, static_cast<long long>(items_[i].tmpOffset), SEEK_SET) == 0;
#else
            good = fseeko(tmp_, static_cast<off_t>(items_[i].tmpOffset), SEEK_SET) == 0;
#endif
            good = good && (buf.empty() || std::fread(buf.data(), 1, buf.size(), tmp_) == buf.size());
            good = good && (buf.empty() || std::fwrite(buf.data(), 1, buf.size(), f) == buf.size());
            pos += buf.size();
        }
        padTo(fenceAt);
        good = good && std::fwrite(fence.data(), 1, fence.size(), f) == fence.size();
        good = std::fclose(f) == 0 && good;
        std::fclose(tmp_);
        tmp_ = nullptr;
        std::remove((path_ + ".tmp").c_str());
        finished_ = good;
        std::printf("wrote %s: %u records (sorted, fence step %u, %u fences), %llu bytes\n", path_.c_str(),
                    records, step_, fences, static_cast<unsigned long long>(fenceAt + fence.size()));
        return good;
    }

private:
    struct Item {
        std::string name;
        uint32_t kind;
        uint32_t size;
        uint64_t tmpOffset;
    };
    std::string path_;
    uint32_t step_;
    std::FILE* tmp_ = nullptr;
    uint64_t tmpSize_ = 0;
    std::vector<Item> items_;
    bool failed_ = false;
    bool finished_ = false;
};

// writePak with the sorted TOC and the fence (small paks; big ones: SortedPakWriter).
inline bool writePakSorted(const std::string& path, const std::vector<PakItem>& entries, uint32_t step = 128) {
    SortedPakWriter w(path, step);
    for (const auto& e : entries)
        if (!w.add(e)) return false;
    return w.finish();
}

} // namespace wowee::pspconv
