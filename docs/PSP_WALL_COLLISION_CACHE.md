# Reusing wall-collision geometry

The latest hardware log records 5.7-8.4 ms in col.walls during movement-related
19-21 FPS dips. ModelRenderer already caches nearby object candidates, but the
movement substeps and two contact passes still repeatedly searched the same
collision-cell lists and transformed/classified their triangles.

The wall resolver now keeps two prepared cell-list sets, allocated lazily on the
first relevant wall query. Each holds at most 128 wall triangle references with
world-space vertices, normals and bounds. Total allocation is below 20 KiB per
renderer that uses wall collision; unused renderers allocate nothing. clear()
releases the cache. Ordinary hits do not allocate or search triangle cell lists.

Keys include collision header, world generation, instance, exact cell range and
placement transform. Streaming, replacement geometry and moved placements cannot
reuse stale prepared geometry. Cell/list order and duplicate triangle references
are retained, preserving sequential pushes at corners. Contact checks always use
the live capsule position and feet height; no collision answer is approximated.
Dense lists exceeding capacity and allocation failure use the complete resolver.
Overflow is remembered for that key to avoid retrying failed preparation each step.
Replacement uses least-recently-used entries within the same allocation. The
wall_cache_miss log splits generation, transform, cell-range, cold and capacity
misses, allowing the next hardware log to explain remaining churn.

The nearby-instance loop also advances sequentially through a cached candidate
set, rather than looking it up and binary-searching after every visited object.
If a contact pushes the capsule outside the cached column it immediately queries
again, preserving the original instance order. Movement subdivision, step height,
collision readiness and floor/ramp queries remain unchanged. BigWmo's separate
wall resolver is unchanged; this targets the measured col.walls section.

Validation: production-routine host tests cover 12,000 moving/corner queries
against the full scan, 4,608 transformed floor queries, placement/generation
invalidation, cell boundaries, duplicate references, and dense overflow fallback.
500 repeated wall queries reuse two prepared triangles without further geometry
preparation. PSP SDK header syntax checks pass for model/world source. Hardware
FPS gains remain unmeasured; rebuild and copy EBOOT only, with no asset rebake.

With perfhud=1, session.log records cumulative wall_cache hits/builds/prepared/
overflow/kb alongside frame_cost every five seconds. Compare the same walking
route and camera direction with the previous build, especially col.walls timing.
The cpufast=0 reference path bypasses this optimization along with the existing
CPU acceleration; it is useful for correctness checks, not isolated benchmarking.
