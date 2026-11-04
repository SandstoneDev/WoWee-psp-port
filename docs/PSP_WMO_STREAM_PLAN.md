# Cached WMO residency planning

The latest supplied session has several bigwmo.plan samples around 2–8 ms.
Previously, every completed read dirtied the full WMO residency plan. Rebuilding
cleared six resource tables, visited cells/chunks, scanned resource tables again
to collect candidates, and sorted the result. Optional reads deferred by memory
pressure also dirtied the full plan.

The renderer now retains its ordered candidate list separately from the current
memory-budget cut. A completed read refreshes budget/request state over that
working set. A newly installed mesh inserts its own texture dependencies into
the ordered list; unrelated cells are not rescanned. Cancelled requests return
on the next update, and failed reads retain their retry deadlines. Changing the
memory budget or entering/leaving collision-only pressure refreshes the cached
queue rather than the whole building. Eviction candidates come from installed
resource keys rather than scans of all resource slots.

Movement collision is separate: every exact player-position change, or a change
to the blocked movement probe, refreshes indexed nearby collision candidates.
It keeps both held-feet and blocked-destination neighborhoods. Essential
collision retains priority even when the cosmetic budget is exhausted. This
does not reuse stale collision readiness or disable floors/walls.

The larger visual plan retains the existing two-yard camera / one-yard player
movement thresholds and twenty-frame periodic refresh. Teleports, planning
configuration changes and newly relevant portal visibility invalidate it.
Draw distances, mesh/LOD thresholds, portal eligibility, spells, rain and fog
are unchanged by this optimization. It does not make all cell scanning
incremental; it removes whole-building work from asset-completion and local
collision events.

A short bigwmo_plan log reports cumulative full (spatial rebuilds), queue
(working-set budget/request refreshes), local (collision refreshes), and current
candidate count. Compare deltas alongside the existing bigwmo.plan profiler
times on the same route. Counters alone do not measure FPS.

Validation: tests/test_psp_wmo_stream_plan.py executes production planning,
installation and request functions against 2,281-chunk / 27-cell synthetic
tables. It covers dependency discovery, deduplication, cancellation, exact
blocked probes, low-memory collision priority, budget recovery, read retries,
configuration changes, teleports and eviction. An optional saved pre-change
source can supply the reference planner as the script's second argument.
With that reference, ten completions produced identical selected asset sets
while reducing full spatial plans from eleven to one; 400 movement/LOD/budget
snapshots also selected the same sets. Bridge-loading and ramp-landing tests
passed. The changed renderer passed a host syntax check with PSP SDK headers.
Full EBOOT cross-build and real hardware performance remain unverified.

Rebuild EBOOT.PBP only. No Kiln change or asset rebake is required.
