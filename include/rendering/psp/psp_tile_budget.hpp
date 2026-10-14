// Memory budget: ModelRenderer's tile loads by budget.
//
// A tile's doodads ("d_<x>_<y>") went in all at once: every model, texture and collision mesh its placements use.
// The world entry loaded the player's own tile that way without asking the heap - one tile of the northern continent
// (43_28) alone is 19.0 MB of doodads, another (41_30) 15.5 MB, against ~14 MB the entry has for doodads (measured in
// a village) - and the tile streamer's jobs read a whole neighbour tile once streamreserve could be allocated. Now the
// placements go nearest first (the distance to their bounding sphere's surface: a big building near the player comes
// early), and before each model not resident yet the meter below asks the heap for the reserve plus that model (one
// probe per stepBytes loaded, not one per model); when the heap says no, the rest of the tile is deferred - placed
// later (TileStreamer's top-up jobs, nearest first) when there is room. Pure logic: the host test runs it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee::rendering::psp {

struct TileBudgetMeter {
    uint32_t reserveBytes = 0;     // 0 = no budget (everything admitted)
    uint32_t stepBytes = 512u * 1024u;
    uint32_t sinceProbe = 0;       // bytes admitted since the last passed probe
    uint32_t probes = 0;
    bool stopped = false;          // a probe failed: nothing more this call

    // Before reading `need` bytes: true = read it. probe(bytes) must try to allocate that much (and free it).
    template <class Probe>
    bool admit(uint32_t need, Probe&& probe) {
        if (stopped) return false;
        if (!reserveBytes) return true;
        if (probes == 0 || sinceProbe + need > stepBytes) {
            ++probes;
            if (!probe(reserveBytes + std::max(need, stepBytes))) {
                stopped = true;
                return false;
            }
            sinceProbe = 0;
        }
        sinceProbe += need;
        return true;
    }
};

// Indices of p[0..n) nearest (x, y) first - the horizontal distance to the bounding sphere's surface (sphere[0..3]:
// centre and radius, server coordinates); ties in placement order (stable).
template <class Placement>
void orderNearest(const Placement* p, uint32_t n, float x, float y, std::vector<uint32_t>& out) {
    out.resize(n);
    std::vector<float> key(n);
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = i;
        const float dx = p[i].sphere[0] - x, dy = p[i].sphere[1] - y;
        key[i] = std::sqrt(dx * dx + dy * dy) - p[i].sphere[3];
    }
    std::stable_sort(out.begin(), out.end(), [&](uint32_t a, uint32_t b) { return key[a] < key[b]; });
}

}  // namespace wowee::rendering::psp
