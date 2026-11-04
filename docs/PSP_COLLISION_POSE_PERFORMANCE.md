# Collision and NPC pose CPU work

The 47c00e3e session has wall checks at 7.51–8.20 ms in samples at 19.2–20.7 FPS.
This change targets collision work and distant NPC bone evaluation without
reducing NPC visibility, spell effects, rain or draw distance.

## Collision

Static model floors and walls share two fixed-size nearby candidate caches.
Each cache covers a padded XY column, including every height, so it cannot
discard a basement or an upper floor. Candidate IDs retain the original
instance order. Only the broad phase is cached: floor heights and wall contacts
are computed against the current query and collision triangles.

The cache refreshes when a query leaves its column or the model grid generation
changes. Streaming and changed placements already rebuild that grid. Wall
pushes recheck cache containment before visiting the next instance; a push
cannot silently move the capsule outside a fixed candidate list. More than
128 overlapping objects uses the original full scan instead of truncating.
Nearby cache storage is about 1 KiB per renderer, with no query-time allocation.
Triangles outside the current capsule XY box skip normal/closest-point work.
Existing camera-ray code and BigWmo contact resolution are retained.

The ten-second `col_candidates hits= builds=` log exposes broad-phase reuse.
Existing `cpu_fast` and profiler `col.walls` samples measure remaining work.
`cpufast=0` retains the stable full instance scan for comparison.

## NPCs

With the default npcanimdist=45, bone poses stay at the existing full rate
within 45 yards. Moving distant actors keep their previous two-/four-frame
cadence. Idle actors use two frames beyond 45 yards, four beyond about 60,
and six beyond 90. Cached per-unit matrices remain valid between evaluations.
Animation clocks, movement and state checks still advance each frame.

Targets, portraits, the controlled mount, combat-ready actors and one-shots
request full-rate poses. Nearby combat actors and one-shots also bypass the
pose-count cap, alongside targets/mounts/portraits and first-time poses.
Locomotion/death sequence transitions request an immediate pose. GUID phases
spread distant pose work across frames. npcanimdist=0 disables throttling.
No NPC is removed or replaced with a static model by this policy.

## Validation

- 12,000 randomized moving/corner wall queries matched full-scan resolution;
  synthetic instance visits decreased from 3,072,000 to 57,550.
- 4,608 transformed floor queries matched brute force. Streaming collision
  recovery and changed-placement floor invalidation passed.
- Candidate tests cover reuse, stacked floors, changed generations, bounds,
  two cached columns, overflow fallback and nonfinite input.
- Pose tests retain protected actors and reduce a synthetic distant-idle
  sample from 1,800 to 1,200 evaluations (33% fewer).
- Existing ramp, bridge-loading and elevator-rider regressions passed,
  including elevator movement at 3/9/15/30/60 FPS.
- Host syntax checks of models, creatures and impostor logging passed with
  PSP SDK headers. These are not a full PSP cross-build or hardware benchmark.

Rebuild and replace EBOOT.PBP only; no asset packs need rebaking. Repeat the
same Brill/Undercity route on hardware and compare frame-time spikes before
attributing an FPS gain. The work-count reductions are not FPS predictions.
