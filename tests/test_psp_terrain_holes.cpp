#include "rendering/psp/psp_terrain_holes.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

using namespace wowee::rendering::psp;

struct Point { int r, c, z; };
Point point(int i) {
    if (i < 81) return {i / 9, i % 9, 0};
    const int edge = (i - 81) / 9, offset = (i - 81) % 9;
    if (edge == 0) return {0, offset, -1};
    if (edge == 1) return {8, offset, -1};
    if (edge == 2) return {offset, 0, -1};
    return {offset, 8, -1};
}

int main() {
    // A single asymmetric bit verifies the row/column convention.
    assert(terrainQuadIsHole(0x10, 2, 0));
    assert(!terrainQuadIsHole(0x10, 0, 2));
    assert(!terrainQuadIsHole(0xFFFF, -1, 0));
    for (unsigned mask = 0; mask <= 0xFFFF; ++mask) {
        const auto mesh = buildTerrainHoleIndices(static_cast<uint16_t>(mask));
        assert(mesh.surfaceCount <= mesh.count);
        unsigned surfaceExpected = 0;
        for (int r=0;r<8;++r) for (int c=0;c<8;++c)
            if (!terrainQuadIsHole(static_cast<uint16_t>(mask),r,c)) surfaceExpected+=2;
        assert(mesh.surfaceTriangles==surfaceExpected);
        for (unsigned i=0;i<mesh.surfaceCount;++i) assert(mesh.indices[i]<81);
        assert(mesh.count <= mesh.indices.size());
        unsigned expected = 0;
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c)
                if (!terrainQuadIsHole(static_cast<uint16_t>(mask), r, c))
                    expected += 2 + 2 * ((r == 0) + (r == 7) + (c == 0) + (c == 7));
        unsigned actual = 0;
        for (unsigned i = 0; i < mesh.count; ++i) assert(mesh.indices[i] < 117);
        for (unsigned i = 2; i < mesh.count; ++i) {
            const Point a = point(mesh.indices[i-2]), b = point(mesh.indices[i-1]), c = point(mesh.indices[i]);
            const int ur=b.r-a.r, uc=b.c-a.c, uz=b.z-a.z;
            const int vr=c.r-a.r, vc=c.c-a.c, vz=c.z-a.z;
            if (uc*vz-uz*vc == 0 && uz*vr-ur*vz == 0 && ur*vc-uc*vr == 0) continue;
            ++actual;
            int row = (a.r+b.r+c.r)/3, column = (a.c+b.c+c.c)/3;
            if (row == 8) row = 7;
            if (column == 8) column = 7;
            // No surface or outer skirt triangle may seal a removed quad.
            assert(!terrainQuadIsHole(static_cast<uint16_t>(mask), row, column));
        }
        assert(actual == expected && actual == mesh.triangles);
    }
    assert(buildTerrainHoleIndices(0xFFFF).count == 0);
    assert(!terrainNeedsSkirts(25.f,24.f,1.f)); // inn basement, nearby full-resolution edges
    assert(!terrainNeedsSkirts(25.f,24.f,.45f));
    assert(terrainNeedsSkirts(60.f,24.f,.45f)); // adaptive LOD transition retains seam cover
    assert(terrainNeedsSkirts(150.f,24.f,1.f));
    std::puts("All 65536 terrain hole masks preserve openings, triangle counts, and skirt gaps.");
}
