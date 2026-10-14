// psp_bigwmo_vis.hpp - big-WMO visibility: record binding, screen rectangles, portal traversal, group lookup,
// collision floor query and small helpers.
//
// Clean-room implementation of specs/world/bigwmo_visibility.md (v2). Header only; glm, <algorithm>, <cmath>,
// <cstdint>, <cstring>, <vector> and our headers psp_bigwmo_format.hpp (records), psp_model_format.hpp (WCOL) and
// psp_ge_limits.hpp (guard band). No platform headers. The same code runs in the EBOOT and in the PC converter.
//
// Portal traversal after D. Luebke, C. Georges, "Portals and Mirrors" (1995): depth-first walk through portals,
// narrowing a screen-space rectangle at each portal.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/psp/psp_bigwmo_format.hpp"
#include "rendering/psp/psp_ge_limits.hpp"
#include "rendering/psp/psp_model_format.hpp"

namespace wowee {
namespace rendering {
namespace psp {

constexpr int kMaxPortalDepth = 24;
constexpr uint32_t kMaxPortalWalk = 4096;

namespace bigwmo_detail {

// MOGP flag bits by value (bigwmo_visibility.md 2.1 / 3.4)
constexpr uint32_t kExterior = 0x8;
constexpr uint32_t kUnreachable = 0x80;
constexpr uint32_t kInterior = 0x2000;
constexpr uint32_t kAlwaysDraw = 0x10000;
constexpr uint32_t kBit400000 = 0x400000;
constexpr uint32_t kAntiportal = 0x4000000;
constexpr uint32_t kOutdoorViewBits = 0x50148;  // 0x8, 0x40, 0x100, 0x10000, 0x40000
constexpr uint32_t kOutdoorStopBits = 0x10008;  // 0x8, 0x10000
constexpr uint32_t kCamOutdoorBits = 0x40140;  // 0x100, 0x40, 0x40000
constexpr float kSurfaceDepth = 1760.0f;

// x > 0 decided on the bit pattern (a NaN is not > 0; +inf is), so a corrupt record never reaches a float compare.
inline bool positive(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof u);
    return (u & 0x80000000u) == 0 && (u & 0x7FFFFFFFu) != 0 && (u & 0x7FFFFFFFu) <= 0x7F800000u;
}

inline bool aligned4(uint32_t v) { return (v & 3u) == 0; }

inline bool interiorOnly(uint32_t flags) { return (flags & kInterior) && !(flags & kExterior); }

inline float boxVolume(const float lo[3], const float hi[3]) {
    return (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
}

}  // namespace bigwmo_detail

// ---------------------------------------------------------------------------------------------------------------
// 2.2 / 3.1 Record views
// ---------------------------------------------------------------------------------------------------------------
struct BigWmoTables {
    const BigWmoHeader* h = nullptr;
    const BigWmoGroup* groups = nullptr;
    const BigWmoPortal* portals = nullptr;
    const BigWmoPortalRef* refs = nullptr;
    const float* portalVerts = nullptr;
    const BigWmoCell* cells = nullptr;
    const BigWmoChunk* chunks = nullptr;
    const BigWmoBlobRef* chunkLod = nullptr;
    const BigWmoBlobRef* textures = nullptr;
    const BigWmoBlobRef* chunkNear = nullptr;

    bool bind(const void* blob, uint32_t size) {
        using namespace bigwmo_detail;
        *this = BigWmoTables();
        if (!blob || size < sizeof(BigWmoHeader)) return false;
        const uint8_t* b = static_cast<const uint8_t*>(blob);
        const BigWmoHeader* hh = reinterpret_cast<const BigWmoHeader*>(b);
        if (std::memcmp(hh->magic, "WBWO", 4) != 0) return false;
        if (hh->version != kBigWmoVersion) return false;

        struct Table {
            uint32_t offset;
            uint64_t bytes;
        };
        const Table tables[9] = {
            {hh->groupOffset, uint64_t(hh->groupCount) * sizeof(BigWmoGroup)},
            {hh->portalOffset, uint64_t(hh->portalCount) * sizeof(BigWmoPortal)},
            {hh->portalRefOffset, uint64_t(hh->portalRefCount) * sizeof(BigWmoPortalRef)},
            {hh->portalVertexOffset, uint64_t(hh->portalVertexCount) * 12u},
            {hh->cellOffset, uint64_t(hh->cellCount) * sizeof(BigWmoCell)},
            {hh->chunkOffset, uint64_t(hh->chunkCount) * sizeof(BigWmoChunk)},
            {hh->chunkLodOffset, uint64_t(hh->chunkCount) * sizeof(BigWmoBlobRef)},
            {hh->textureOffset, uint64_t(hh->textureCount) * sizeof(BigWmoBlobRef)},
            {hh->chunkNearOffset, uint64_t(hh->chunkCount) * sizeof(BigWmoBlobRef)},
        };
        for (const Table& t : tables) {
            if (!aligned4(t.offset)) return false;
            if (t.offset > size) return false;
            if (t.bytes > uint64_t(size) - t.offset) return false;
        }
        if (!positive(hh->step) || !positive(hh->cellSize)) return false;

        const BigWmoGroup* gr = reinterpret_cast<const BigWmoGroup*>(b + hh->groupOffset);
        const BigWmoPortal* po = reinterpret_cast<const BigWmoPortal*>(b + hh->portalOffset);
        const BigWmoPortalRef* rf = reinterpret_cast<const BigWmoPortalRef*>(b + hh->portalRefOffset);
        const BigWmoCell* ce = reinterpret_cast<const BigWmoCell*>(b + hh->cellOffset);
        const BigWmoChunk* ch = reinterpret_cast<const BigWmoChunk*>(b + hh->chunkOffset);
        for (uint32_t g = 0; g < hh->groupCount; ++g)
            if (uint32_t(gr[g].portalStart) + gr[g].portalCount > hh->portalRefCount) return false;
        for (uint32_t r = 0; r < hh->portalRefCount; ++r)
            if (rf[r].portal >= hh->portalCount || rf[r].group >= hh->groupCount) return false;
        for (uint32_t p = 0; p < hh->portalCount; ++p)
            if (uint32_t(po[p].vertexStart) + po[p].vertexCount > hh->portalVertexCount) return false;
        for (uint32_t c = 0; c < hh->cellCount; ++c)
            if (uint64_t(ce[c].chunkStart) + ce[c].chunkCount > hh->chunkCount) return false;
        for (uint32_t k = 0; k < hh->chunkCount; ++k)
            if (ch[k].group >= hh->groupCount || ch[k].cell >= hh->cellCount) return false;

        h = hh;
        groups = gr;
        portals = po;
        refs = rf;
        portalVerts = reinterpret_cast<const float*>(b + hh->portalVertexOffset);
        cells = ce;
        chunks = ch;
        chunkLod = reinterpret_cast<const BigWmoBlobRef*>(b + hh->chunkLodOffset);
        textures = reinterpret_cast<const BigWmoBlobRef*>(b + hh->textureOffset);
        chunkNear = reinterpret_cast<const BigWmoBlobRef*>(b + hh->chunkNearOffset);
        return true;
    }

    glm::vec3 portalVertex(uint32_t i) const {
        return glm::vec3(portalVerts[3 * i], portalVerts[3 * i + 1], portalVerts[3 * i + 2]);
    }

    void chunkBox(uint32_t c, glm::vec3& lo, glm::vec3& hi) const {
        const BigWmoChunk& k = chunks[c];
        const BigWmoCell& cell = cells[k.cell];
        const float step = h->step;
        for (int a = 0; a < 3; ++a) {
            lo[a] = cell.center[a] + static_cast<float>(k.bmin[a]) * step;
            hi[a] = cell.center[a] + static_cast<float>(k.bmax[a]) * step;
        }
    }

    uint32_t groupCount() const { return h ? h->groupCount : 0u; }
};

// ---------------------------------------------------------------------------------------------------------------
// 3.2 Screen rectangles in normalised device coordinates
// ---------------------------------------------------------------------------------------------------------------
struct NdcRect {
    float x0 = 1, y0 = 1, x1 = -1, y1 = -1;

    static NdcRect full() {
        NdcRect r;
        r.x0 = -1;
        r.y0 = -1;
        r.x1 = 1;
        r.y1 = 1;
        return r;
    }
    bool empty() const { return !(x0 < x1 && y0 < y1); }
    bool contains(const NdcRect& r) const {
        return !empty() && r.x0 >= x0 && r.y0 >= y0 && r.x1 <= x1 && r.y1 <= y1;
    }
    NdcRect intersect(const NdcRect& r) const {
        NdcRect o;
        o.x0 = std::max(x0, r.x0);
        o.y0 = std::max(y0, r.y0);
        o.x1 = std::min(x1, r.x1);
        o.y1 = std::min(y1, r.y1);
        return o;
    }
    void unite(const NdcRect& r) {
        if (r.empty()) return;
        if (empty()) {
            *this = r;
            return;
        }
        x0 = std::min(x0, r.x0);
        y0 = std::min(y0, r.y0);
        x1 = std::max(x1, r.x1);
        y1 = std::max(y1, r.y1);
    }
};
static_assert(sizeof(NdcRect) == 16, "NdcRect size");

// ---------------------------------------------------------------------------------------------------------------
// 3.3 Projections (homogeneous clipping at w = wMin)
// ---------------------------------------------------------------------------------------------------------------
namespace bigwmo_detail {

struct RectAcc {
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    void add(const glm::vec4& c) {
        const float x = c.x / c.w, y = c.y / c.w;
        if (!any) {
            x0 = x1 = x;
            y0 = y1 = y;
            any = true;
            return;
        }
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
};

// the point with w = wMin on the segment from a towards b (clip coordinates interpolated linearly)
inline glm::vec4 cutAt(const glm::vec4& a, const glm::vec4& b, float wMin) {
    const float t = (wMin - a.w) / (b.w - a.w);
    return a + (b - a) * t;
}

}  // namespace bigwmo_detail

inline NdcRect projectPolygon(const glm::mat4& toClip, const glm::vec3* pts, int n, float wMin = 0.05f) {
    NdcRect r;
    if (n < 3 || n > 32) return r;
    glm::vec4 c[32];
    for (int i = 0; i < n; ++i) c[i] = toClip * glm::vec4(pts[i], 1.0f);
    bigwmo_detail::RectAcc acc;
    for (int i = 0; i < n; ++i) {
        const glm::vec4& a = c[i];
        const glm::vec4& b = c[(i + 1) % n];
        const bool ia = a.w >= wMin, ib = b.w >= wMin;
        if (ia) acc.add(a);
        if (ia != ib) acc.add(bigwmo_detail::cutAt(a, b, wMin));
    }
    if (!acc.any) return r;
    const float g = 1e-4f;  // a portal seen edge-on keeps an area
    r.x0 = acc.x0 - g;
    r.y0 = acc.y0 - g;
    r.x1 = acc.x1 + g;
    r.y1 = acc.y1 + g;
    return r;
}

inline NdcRect projectBox(const glm::mat4& toClip, const glm::vec3& lo, const glm::vec3& hi, float wMin = 0.05f) {
    glm::vec4 c[8];
    bool in[8];
    bool anyBehind = false;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 p((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        c[i] = toClip * glm::vec4(p, 1.0f);
        in[i] = c[i].w >= wMin;
        if (!in[i]) anyBehind = true;
    }
    bigwmo_detail::RectAcc acc;
    for (int i = 0; i < 8; ++i)
        if (in[i]) acc.add(c[i]);
    if (anyBehind) {
        // the 12 box edges: corner pairs that differ in exactly one coordinate bit
        for (int i = 0; i < 8; ++i)
            for (int bit = 1; bit < 8; bit <<= 1) {
                const int j = i | bit;
                if (j == i) continue;
                if (in[i] != in[j]) acc.add(bigwmo_detail::cutAt(c[i], c[j], wMin));
            }
    }
    NdcRect r;
    if (!acc.any) return r;
    r.x0 = acc.x0;
    r.y0 = acc.y0;
    r.x1 = acc.x1;
    r.y1 = acc.y1;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// 3.4 - 3.7 Portal visibility
// ---------------------------------------------------------------------------------------------------------------
class PortalVisibility {
public:
    enum Mode : int { kVisAll = 0, kVisWalk = 1, kVisWalkExt = 2, kVisUpstream = 3, kVisWalkOutdoor = 4 };

    std::vector<uint32_t> visible;  // bit (g mod 32) of word (g / 32)
    std::vector<NdcRect> rects;  // per group: union of every rectangle it was reached with
    bool outside = false;
    bool noGroup = false;
    bool seenOutsideOnly = false;
    bool outsideVisible = false;
    uint32_t walked = 0;
    uint32_t visibleGroups = 0;
    NdcRect outdoorRect;

    void compute(const BigWmoTables& t, const glm::vec3& localCam, const glm::mat4& toClip, int camGroup,
                 int viewerGroup, int portalSide = 1, int mode = kVisWalkExt) {
        using namespace bigwmo_detail;
        const uint32_t gc = t.groups ? t.groupCount() : 0u;
        if (mode < kVisAll || mode > kVisWalkOutdoor) mode = kVisWalk;  // any other value behaves as 1
        if (camGroup >= static_cast<int>(gc)) camGroup = -1;  // not specified: an index past the table = no group
        if (viewerGroup >= static_cast<int>(gc)) viewerGroup = -1;

        if (mode == kVisAll || (camGroup < 0 && mode != kVisWalkOutdoor)) {
            all(t);
            noGroup = camGroup < 0;
            seenOutsideOnly = noGroup && mode != kVisAll && mode != kVisUpstream;
            return;
        }

        Ctx c;
        c.t = &t;
        c.cam = localCam;
        c.toClip = &toClip;
        c.portalSide = portalSide;
        c.mode = mode;
        c.indoor = true;

        if (mode == kVisWalkOutdoor) {
            computeOutdoor(c, camGroup);
            return;
        }

        // modes 1, 2, 3
        reset(gc);
        walked = 0;
        visibleGroups = 0;
        outsideVisible = false;
        noGroup = false;
        seenOutsideOnly = false;
        const BigWmoGroup& cg = t.groups[camGroup];
        outside = !interiorOnly(cg.flags);

        if (mode == kVisUpstream) {
            bool allCase = outside || cg.portalCount == 0;
            for (uint32_t g = 0; g < gc && !allCase; ++g) {
                if (static_cast<int>(g) == camGroup) continue;
                const BigWmoGroup& G = t.groups[g];
                if ((G.flags & kExterior) && boxContains(G, localCam)) allCase = true;
            }
            if (allCase) {
                all(t);
                outside = true;
                return;
            }
        }

        const NdcRect full = NdcRect::full();
        if (mode == kVisUpstream) {
            visit(c, static_cast<uint32_t>(camGroup), full, 0, -1);
            exteriorSeeds(c);
        } else if (!outside) {
            visit(c, static_cast<uint32_t>(camGroup), full, 0, -1);
            if (mode == kVisWalkExt) exteriorSeeds(c);
        } else {
            exteriorSeeds(c);
        }

        if (viewerGroup >= 0 && viewerGroup != camGroup && interiorOnly(t.groups[viewerGroup].flags))
            visit(c, static_cast<uint32_t>(viewerGroup), full, 0, -1);

        markAlwaysDraw(t);
        visibleGroups = countVisible();
    }

    void all(const BigWmoTables& t) {
        using namespace bigwmo_detail;
        const uint32_t gc = t.groups ? t.groupCount() : 0u;
        reset(gc);
        const NdcRect full = NdcRect::full();
        for (uint32_t g = 0; g < gc; ++g)
            if (!(t.groups[g].flags & kAntiportal)) mark(g, full);
        visibleGroups = countVisible();
        outside = true;
        outsideVisible = true;
        noGroup = false;
        seenOutsideOnly = false;
    }

    bool isVisible(uint32_t g) const {
        return g < rects.size() && ((visible[g >> 5] >> (g & 31u)) & 1u) != 0;
    }

    bool chunkVisible(const BigWmoChunk& k) const {
        return isVisible(k.group) && (!seenOutsideOnly || (k.flags & kChunkSeenOutside));
    }

private:
    struct Ctx {
        const BigWmoTables* t = nullptr;
        glm::vec3 cam{0.0f};
        const glm::mat4* toClip = nullptr;
        int portalSide = 1;
        int mode = kVisWalkExt;
        bool indoor = true;  // mode 4: indoor or outdoor traversal
    };

    void reset(uint32_t gc) {
        visible.assign((gc + 31u) / 32u, 0u);
        rects.assign(gc, NdcRect());
    }

    void mark(uint32_t g, const NdcRect& r) {
        visible[g >> 5] |= 1u << (g & 31u);
        rects[g].unite(r);
    }

    uint32_t countVisible() const {
        uint32_t n = 0;
        for (uint32_t g = 0; g < rects.size(); ++g)
            if (isVisible(g)) ++n;
        return n;
    }

    static bool boxContains(const BigWmoGroup& G, const glm::vec3& p) {
        for (int a = 0; a < 3; ++a)
            if (p[a] < G.bmin[a] || p[a] > G.bmax[a]) return false;
        return true;
    }

    static NdcRect groupRect(const Ctx& c, const BigWmoGroup& G) {
        return projectBox(*c.toClip, glm::vec3(G.bmin[0], G.bmin[1], G.bmin[2]),
                          glm::vec3(G.bmax[0], G.bmax[1], G.bmax[2]));
    }

    void markAlwaysDraw(const BigWmoTables& t) {
        using namespace bigwmo_detail;
        const NdcRect full = NdcRect::full();
        for (uint32_t g = 0; g < rects.size(); ++g) {
            const uint32_t f = t.groups[g].flags;
            if ((f & kAlwaysDraw) && !(f & kAntiportal)) mark(g, full);
        }
    }

    // exterior seeds: groups in index order with 0x8, not excluded, in view (box rectangle within the screen)
    void exteriorSeeds(const Ctx& c) {
        using namespace bigwmo_detail;
        const BigWmoTables& t = *c.t;
        const NdcRect full = NdcRect::full();
        for (uint32_t g = 0; g < rects.size(); ++g) {
            const BigWmoGroup& G = t.groups[g];
            if (!(G.flags & kExterior) || (G.flags & kAntiportal)) continue;
            if (groupRect(c, G).intersect(full).empty()) continue;
            visit(c, g, full, 0, -1);
        }
    }

    void computeOutdoor(Ctx& c, int camGroup) {
        using namespace bigwmo_detail;
        const BigWmoTables& t = *c.t;
        const uint32_t gc = t.groups ? t.groupCount() : 0u;
        reset(gc);
        walked = 0;
        visibleGroups = 0;
        outsideVisible = false;
        noGroup = camGroup < 0;
        seenOutsideOnly = false;
        outdoorRect = NdcRect();
        outside = camGroup < 0;
        const NdcRect full = NdcRect::full();

        // indoor pass
        if (camGroup >= 0) {
            if (t.groups[camGroup].flags & kCamOutdoorBits) outdoorRect = full;
            c.indoor = true;
            visit(c, static_cast<uint32_t>(camGroup), full, 0, -1);
        } else {
            outdoorRect = full;
        }

        // outdoor pass
        if (!outdoorRect.empty()) {
            outsideVisible = true;
            const NdcRect od = outdoorRect;
            c.indoor = false;
            for (uint32_t g = 0; g < gc; ++g) {
                const BigWmoGroup& G = t.groups[g];
                if (!(G.flags & kExterior) || (G.flags & (kAntiportal | kAlwaysDraw))) continue;
                if (groupRect(c, G).intersect(od).empty()) continue;
                visit(c, g, od, 0, -1);
            }
        }

        markAlwaysDraw(t);
        visibleGroups = countVisible();
    }

    // V(g, R, D, F): enter group g with rectangle R at depth D, arriving from group F (-1 = none)
    void visit(const Ctx& c, uint32_t g, const NdcRect& R, int D, int from) {
        using namespace bigwmo_detail;
        const BigWmoTables& t = *c.t;
        const BigWmoGroup& G = t.groups[g];
        if (G.flags & kAntiportal) return;
        if (c.mode == kVisWalkOutdoor && (G.flags & kAlwaysDraw)) return;
        if (isVisible(g) && rects[g].contains(R)) return;
        mark(g, R);
        if (c.mode != kVisWalkOutdoor && D > 0 && (G.flags & kExterior)) outsideVisible = true;
        if (D >= kMaxPortalDepth || walked >= kMaxPortalWalk) return;

        for (uint32_t i = 0; i < G.portalCount; ++i) {
            const BigWmoPortalRef& e = t.refs[G.portalStart + i];
            ++walked;
            if (c.mode == kVisWalkOutdoor && static_cast<int>(e.group) == from) continue;
            const BigWmoPortal& P = t.portals[e.portal];
            if (P.vertexCount < 3) continue;

            const glm::vec3 n(P.plane[0], P.plane[1], P.plane[2]);
            const glm::vec3 v0 = t.portalVertex(P.vertexStart);
            const float dist = glm::dot(n, c.cam) - glm::dot(n, v0);
            const int s = static_cast<int>(e.side) * c.portalSide;
            if (c.mode == kVisWalk || c.mode == kVisWalkExt) {
                if (static_cast<float>(s) * dist < -0.5f) continue;
            } else if (c.mode == kVisWalkOutdoor) {
                const float d = s < 0 ? -dist : dist;
                if (d < -0.5f) continue;
            }

            // aperture(e, R)
            NdcRect next;
            bool fromPolygon = false;
            if (std::fabs(dist) < 1.5f) {
                next = R;
            } else {
                glm::vec3 pts[32];
                const int m = std::min<int>(P.vertexCount, 32);
                for (int k = 0; k < m; ++k) pts[k] = t.portalVertex(uint32_t(P.vertexStart) + uint32_t(k));
                next = projectPolygon(*c.toClip, pts, m).intersect(R);
                fromPolygon = true;
            }
            if (c.mode == kVisUpstream && fromPolygon && !next.empty()) next = NdcRect::full();
            if (next.empty()) continue;

            if (c.mode == kVisWalkOutdoor) {
                const uint32_t C = t.groups[e.group].flags;
                if (c.indoor && (C & kOutdoorViewBits)) outdoorRect.unite(next);
                if (C & kOutdoorStopBits) continue;
            }
            visit(c, e.group, next, D + 1, static_cast<int>(g));
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------
// 3.8 Camera group for mode 4 (highest surface below the point)
// ---------------------------------------------------------------------------------------------------------------
template <typename SurfaceBelow, typename HaveGeometry>
int findBigWmoGroupBySurface(const BigWmoTables& t, const glm::vec3& p, SurfaceBelow&& surfaceBelow,
                             HaveGeometry&& haveGeometry) {
    using namespace bigwmo_detail;
    if (!t.groups) return -1;
    const uint32_t gc = t.groupCount();
    int bestH = -1, bestB = -1;
    float bestZ = 0.0f, bestVol = 0.0f;
    for (uint32_t g = 0; g < gc; ++g) {
        const BigWmoGroup& G = t.groups[g];
        if (G.flags & (kAntiportal | kUnreachable | kAlwaysDraw | kBit400000)) continue;
        if (p.x < G.bmin[0] || p.x > G.bmax[0] || p.y < G.bmin[1] || p.y > G.bmax[1]) continue;
        if (!(G.bmin[2] <= p.z)) continue;
        if (!(G.bmax[2] >= p.z - kSurfaceDepth)) continue;
        if (haveGeometry(g)) {
            float z = 0.0f;
            if (surfaceBelow(g, p, z) && (bestH < 0 || z > bestZ)) {
                bestH = static_cast<int>(g);
                bestZ = z;
            }
        } else if (p.z <= G.bmax[2]) {
            const float vol = boxVolume(G.bmin, G.bmax);
            if (bestB < 0 || vol < bestVol) {
                bestB = static_cast<int>(g);
                bestVol = vol;
            }
        }
    }
    int r = bestH >= 0 ? bestH : bestB;
    if (r >= 0 && (t.groups[r].flags & kExterior)) r = -1;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// 3.9 Camera / player group for modes 0 to 3
// ---------------------------------------------------------------------------------------------------------------
template <typename FloorBelow, typename HaveGeometry>
int findBigWmoGroup(const BigWmoTables& t, const glm::vec3& p, FloorBelow&& floorBelow, HaveGeometry&& haveGeometry,
                    bool boxFallback = true, bool boxOnlyRule = false, float margin = 0.25f) {
    using namespace bigwmo_detail;
    if (!t.groups) return -1;
    const uint32_t gc = t.groupCount();
    int bestA = -1, bestG = -1, bestB = -1;
    float volA = 0.0f, gapG = 0.0f, volB = 0.0f;
    for (uint32_t g = 0; g < gc; ++g) {
        const BigWmoGroup& G = t.groups[g];
        if (G.flags & kAntiportal) continue;
        bool inside = true;
        for (int a = 0; a < 3; ++a)
            if (p[a] < G.bmin[a] - margin || p[a] > G.bmax[a] + margin) inside = false;
        if (!inside) continue;
        const float vol = boxVolume(G.bmin, G.bmax);
        if (bestA < 0 || vol < volA) {
            bestA = static_cast<int>(g);
            volA = vol;
        }
        if (boxOnlyRule) continue;
        if (haveGeometry(g)) {
            float z = 0.0f;
            if (floorBelow(g, p, z)) {
                const float gap = p.z - z;
                if (bestG < 0 || gap < gapG) {
                    bestG = static_cast<int>(g);
                    gapG = gap;
                }
            }
        } else if (bestB < 0 || vol < volB) {
            bestB = static_cast<int>(g);
            volB = vol;
        }
    }
    if (boxOnlyRule) return bestA;
    if (bestG >= 0) return bestG;
    if (bestB >= 0) return bestB;
    return boxFallback ? bestA : -1;
}

// ---------------------------------------------------------------------------------------------------------------
// 3.10 Collision meshes (WCOL)
// ---------------------------------------------------------------------------------------------------------------
inline bool collisionValid(const void* blob, uint32_t size) {
    using namespace bigwmo_detail;
    if (!blob || size < sizeof(CollisionHeader)) return false;
    const uint8_t* b = static_cast<const uint8_t*>(blob);
    const CollisionHeader* h = reinterpret_cast<const CollisionHeader*>(b);
    if (std::memcmp(h->magic, "WCOL", 4) != 0) return false;
    if (h->nx == 0 || h->ny == 0 || !positive(h->cell)) return false;
    if (!aligned4(h->vertexOffset) || !aligned4(h->cellOffset) || !aligned4(h->listOffset)) return false;
    if (h->triangleOffset & 1u) return false;
    const uint64_t cellsN = uint64_t(h->nx) * h->ny;
    if (uint64_t(h->vertexOffset) + uint64_t(h->vertexCount) * 12u > size) return false;
    if (uint64_t(h->triangleOffset) + uint64_t(h->triangleCount) * sizeof(CollisionTri) > size) return false;
    if (uint64_t(h->cellOffset) + cellsN * 8u > size) return false;
    if (uint64_t(h->listOffset) + uint64_t(h->listCount) * 4u > size) return false;
    const uint32_t* cells = reinterpret_cast<const uint32_t*>(b + h->cellOffset);
    for (uint64_t i = 0; i < cellsN; ++i)
        if (uint64_t(cells[2 * i]) + cells[2 * i + 1] > h->listCount) return false;
    const uint32_t* list = reinterpret_cast<const uint32_t*>(b + h->listOffset);
    for (uint32_t i = 0; i < h->listCount; ++i)
        if (list[i] >= h->triangleCount) return false;
    const CollisionTri* tris = reinterpret_cast<const CollisionTri*>(b + h->triangleOffset);
    for (uint32_t i = 0; i < h->triangleCount; ++i)
        if (tris[i].a >= h->vertexCount || tris[i].b >= h->vertexCount || tris[i].c >= h->vertexCount) return false;
    return true;
}

inline bool collisionFloorBelow(const CollisionHeader* h, uint32_t size, const glm::vec3& p, float& z,
                                float above = 0.05f) {
    using namespace bigwmo_detail;
    if (!h || size < sizeof(CollisionHeader)) return false;
    if (h->nx == 0 || h->ny == 0 || !positive(h->cell)) return false;
    if (p.x < h->bmin[0] || p.x > h->bmax[0] || p.y < h->bmin[1] || p.y > h->bmax[1]) return false;
    const float top = p.z + above;
    if (top < h->bmin[2]) return false;

    const uint8_t* b = reinterpret_cast<const uint8_t*>(h);
    int cx = static_cast<int>((p.x - h->bmin[0]) / h->cell);
    int cy = static_cast<int>((p.y - h->bmin[1]) / h->cell);
    cx = std::max(0, std::min(cx, int(h->nx) - 1));
    cy = std::max(0, std::min(cy, int(h->ny) - 1));
    const uint64_t cellIndex = uint64_t(cy) * h->nx + uint64_t(cx);
    // defensive bounds (a mesh that passed collisionValid always satisfies them)
    if (uint64_t(h->cellOffset) + (cellIndex + 1u) * 8u > size) return false;
    const uint32_t* range = reinterpret_cast<const uint32_t*>(b + h->cellOffset) + 2u * cellIndex;
    const uint32_t start = range[0], count = range[1];
    if (uint64_t(start) + count > h->listCount || uint64_t(h->listOffset) + uint64_t(h->listCount) * 4u > size)
        return false;
    const uint32_t* list = reinterpret_cast<const uint32_t*>(b + h->listOffset);
    const float* verts = reinterpret_cast<const float*>(b + h->vertexOffset);
    const CollisionTri* tris = reinterpret_cast<const CollisionTri*>(b + h->triangleOffset);

    bool found = false;
    float best = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t ti = list[start + i];
        if (ti >= h->triangleCount) continue;
        const CollisionTri& T = tris[ti];
        if (T.a >= h->vertexCount || T.b >= h->vertexCount || T.c >= h->vertexCount) continue;
        const float* A = verts + 3u * T.a;
        const float* B = verts + 3u * T.b;
        const float* C = verts + 3u * T.c;
        const float area2 = (B[0] - A[0]) * (C[1] - A[1]) - (C[0] - A[0]) * (B[1] - A[1]);
        if (!(std::fabs(area2) >= 1e-9f)) continue;
        // barycentric weights of p in the XY plane
        const float wa = ((B[0] - p.x) * (C[1] - p.y) - (C[0] - p.x) * (B[1] - p.y)) / area2;
        const float wb = ((C[0] - p.x) * (A[1] - p.y) - (A[0] - p.x) * (C[1] - p.y)) / area2;
        const float wc = ((A[0] - p.x) * (B[1] - p.y) - (B[0] - p.x) * (A[1] - p.y)) / area2;
        if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f) continue;
        const float hz = wa * A[2] + wb * B[2] + wc * C[2];
        if (!(hz <= top)) continue;
        if (!found || hz > best) {
            best = hz;
            found = true;
        }
    }
    if (!found) return false;
    z = best;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// 3.11 Small helpers
// ---------------------------------------------------------------------------------------------------------------
inline float boxDistance(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi) {
    float s = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const float d = std::max(std::max(lo[a] - p[a], 0.0f), p[a] - hi[a]);
        s += d * d;
    }
    return std::sqrt(s);
}

struct BigWmoLod {
    float lodDistance = 50.0f;  // full chunk mesh nearer than this
    float shellDistance = 160.0f;  // far shell beyond this
    float viewDistance = 450.0f;  // nothing beyond this
    float hysteresis = 12.0f;  // switching back this much inside
};

inline float wmoFocalPx(float fovyDeg) { return ge::focalPx(fovyDeg); }

inline bool wmoSphereSafe(float cx, float cy, float cz, float r, float nearZ, float focalPx) {
    return ge::sphereSafe(cx, cy, cz, r, nearZ, focalPx);
}

inline float wmoSafeEdge(float nearZ, float fovyDeg) { return nearZ * ge::safeEdgePerNear(fovyDeg); }

}  // namespace psp
}  // namespace rendering
}  // namespace wowee
