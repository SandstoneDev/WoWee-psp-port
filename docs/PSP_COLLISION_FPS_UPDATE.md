# Model collision and CPU frame cost

The latest session reaches `shutdown stage=complete`. In its crowded Undercity
view, floor queries repeatedly cost about 5–9 ms per frame while moving; building
rendering, NPC reads/draws and GPU work remain additional costs. At 317.282 s it
records an incomplete building neighborhood (`col=6/7`) at roughly
1629.9, 245.9, -42.4. This identifies loading pressure, not the exact triangle or
object the user fell through.

Changes:

- ModelRenderer floor and wall broad-phase checks now use transformed WCOL
  bounds rather than the visible model's sphere. The two need not cover the
  same volume. A balanced bounds index selects floor candidates; floor tie
  ownership retains the original instance order. Wall checks retain sequential
  contact order and recheck tight bounds after each push.
- The index is rebuilt with the placement grid, including moving elevators,
  and existing floor memoization is invalidated with that grid. Index memory
  is included in collision KB accounting.
- A failed/invalid `k_<model>` read is remembered as collision pending, retried
  with a one-second backoff, and validated using the shared WCOL validator.
  Recovery is serviced for static scenery, game objects and transport renderers.
  A nearby pending object holds movement rather than being considered ready
  merely because its visible mesh loaded. Assets intentionally lacking a
  collision entry retain that behavior.
- Each vertical physics slice now checks readiness before changing height or
  velocity, including falls/jumps with no horizontal input. Waiting preserves
  fall velocity and allows streaming to continue. Reason 4 in collision_probe
  identifies pending model collision.

Validation: a host harness compiles the production floor query, wall contact,
readiness, retry and collision-loader routines against Pak/kernel substitutes.
4,608 floor queries across 512 rotated, reflected and nonuniformly scaled
placements matched a full scan. The index visited 4,608 candidates versus
2,359,296 full-scan instance visits. Tests also cover undersized visual spheres,
wall contact, shared edges, moving-platform cache invalidation, corrupt collision
data, read-failure recovery, retry backoff and intentionally absent collision.
Existing 12,000-query building-index and elevator-rider tests also pass.

These are host correctness/candidate-count results, not a PSP FPS benchmark.
No full EBOOT build or hardware run could be performed here because WSL is
unavailable. Rebuild the updated source and replace EBOOT.PBP only. No converter
or Kiln changes, or asset rebake, are required by this runtime update. Constant
30 FPS is not established: NPC asset loading and dense geometry still need work.
