// Frame CPU paths that
// give bit-for-bit the same results as before, only with less work - the
// terrain height lookup (TerrainRenderer::heightAt / chunkAt), the collision
// queries of ModelRenderer (floor, walls, camera rays), the M2 cull/sort and
// the impostor pass. PSP: not upstream (upstream's desktop code has the same
// linear scans; the PSP's 333 MHz CPU pays for them every frame).
//
// wowee.cfg "cpufast" (read with the forest keys at world entry):
//   1 (default) the new paths;
//   0           the build-104 paths (A/B on the same build);
//   2           A/B on one spot: switches every 10 s, at each "cpu_fast" log
//               line (the line says which mode the next 10 s run in).
// Main thread only (the world screen's update/draw).
#pragma once

#include <cstdint>

namespace wowee::rendering::psp {

inline int g_cpuFastMode = 1;   // the cfg value
inline bool g_cpuFast = true;   // the paths in use now (mode 2 flips it)

// Work counters for the 10-s "cpu_fast" log line (ModelRenderer::logForestStats).
struct CpuFastStats {
    uint32_t heightCalls = 0;   // TerrainRenderer::heightAt + chunkAt
    uint32_t heightTests = 0;   // chunk tests (old: one per chunk scanned; new: per candidate row/column)
    uint32_t colQueries = 0;    // floorHeight + raycastBuildings + raycastProps + collideWalls
    uint32_t colCells = 0;      // grid cells tested (new path)
    uint32_t colInsts = 0;      // doodads tested against the query
    uint32_t colMemoHits = 0;   // queries answered by the memo (same input, same world)
    uint32_t drawsNoStall = 0;  // M2 opaque draws sent without a GE stall-address syscall
    uint32_t frames = 0;
};
inline CpuFastStats g_cpuStats;

} // namespace wowee::rendering::psp
