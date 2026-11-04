// FxPool as described in particle-bake-level3.md section 2 (existing interface; restated here so the level-3 builder
// is self-contained). The integrator may replace it by the converter's existing definition.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wowee {
namespace pspconv {

struct FxPool {
    std::vector<uint8_t> b;

    void pad4() {
        while (b.size() % 4) b.push_back(0);
    }
    // every add* first pads the pool to a multiple of 4 and returns the pool-relative offset
    uint32_t add(const float* f, size_t n) {
        pad4();
        uint32_t off = static_cast<uint32_t>(b.size());
        b.resize(b.size() + 4 * n);
        if (n) std::memcpy(&b[off], f, 4 * n);
        return off;
    }
    // u16 stamps, padding to 4, then the float values
    uint32_t addLife(const std::vector<uint16_t>& ts, const std::vector<float>& v) {
        pad4();
        uint32_t off = static_cast<uint32_t>(b.size());
        for (uint16_t s : ts) {
            b.push_back(static_cast<uint8_t>(s & 0xFF));
            b.push_back(static_cast<uint8_t>(s >> 8));
        }
        pad4();
        size_t vo = b.size();
        b.resize(vo + 4 * v.size());
        if (!v.empty()) std::memcpy(&b[vo], v.data(), 4 * v.size());
        return off;
    }
    template <class K>
    uint32_t addKeys(const K* k, size_t n) {
        pad4();
        uint32_t off = static_cast<uint32_t>(b.size());
        b.resize(b.size() + sizeof(K) * n);
        if (n) std::memcpy(&b[off], k, sizeof(K) * n);
        return off;
    }
};

}  // namespace pspconv
}  // namespace wowee
