#pragma once

#include <array>
#include <cstdint>

namespace wowee::rendering::psp {

// MCNK: each bit removes a 2x2 area of the 8x8 terrain quad grid.
// Rows decrease server X; columns decrease server Y, like the WTER vertices.
inline bool terrainQuadIsHole(uint16_t holes, int row, int column) {
    return row >= 0 && row < 8 && column >= 0 && column < 8 &&
           (holes & (uint16_t{1} << ((row / 2) * 4 + column / 2))) != 0;
}

struct TerrainHoleIndices {
    std::array<uint16_t, 768> indices{};
    uint16_t count = 0, triangles = 0;
    uint16_t surfaceCount = 0, surfaceTriangles = 0;
};

// Hole chunks always retain their exact topology, including at distant LOD.
// Their outer skirts are also cut at holes so a skirt cannot seal an entrance.
inline TerrainHoleIndices buildTerrainHoleIndices(uint16_t holes) {
    TerrainHoleIndices mesh;
    auto run = [&](int first, int end, auto vertexA, auto vertexB) {
        if (mesh.count) {
            mesh.indices[mesh.count] = mesh.indices[mesh.count - 1];
            ++mesh.count;
            mesh.indices[mesh.count++] = static_cast<uint16_t>(vertexA(first));
        }
        for (int i = first; i <= end; ++i) {
            mesh.indices[mesh.count++] = static_cast<uint16_t>(vertexA(i));
            mesh.indices[mesh.count++] = static_cast<uint16_t>(vertexB(i));
        }
        mesh.triangles += static_cast<uint16_t>((end - first) * 2);
    };
    for (int row = 0; row < 8; ++row) {
        for (int c = 0; c < 8;) {
            if (terrainQuadIsHole(holes, row, c)) { ++c; continue; }
            const int first = c;
            while (c < 8 && !terrainQuadIsHole(holes, row, c)) ++c;
            run(first, c, [&](int i) { return row * 9 + i; },
                          [&](int i) { return (row + 1) * 9 + i; });
        }
    }
    mesh.surfaceCount = mesh.count;
    mesh.surfaceTriangles = mesh.triangles;
    auto edgeVertex = [](int edge, int i) {
        return edge == 0 ? i : edge == 1 ? 72 + i : edge == 2 ? i * 9 : i * 9 + 8;
    };
    for (int edge = 0; edge < 4; ++edge) {
        auto isHole = [&](int i) {
            return terrainQuadIsHole(holes, edge < 2 ? (edge == 0 ? 0 : 7) : i,
                                             edge < 2 ? i : (edge == 2 ? 0 : 7));
        };
        for (int i = 0; i < 8;) {
            if (isHole(i)) { ++i; continue; }
            const int first = i;
            while (i < 8 && !isHole(i)) ++i;
            run(first, i, [&](int p) { return edgeVertex(edge, p); },
                          [&](int p) { return 81 + edge * 9 + p; });
        }
    }
    return mesh;
}

// Nearby full-detail neighbors share edge vertices and need no seam walls.
// Use a conservative radius margin so the LOD transition keeps its skirts.
inline bool terrainNeedsSkirts(float distance, float radius, float lodScale) {
    return !(distance < 80.f && distance + 2.f * radius < 200.f * lodScale);
}

}  // namespace wowee::rendering::psp
