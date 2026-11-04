# Frame-work caches and staged appearance updates

These source changes address the measured minimap, wall-cache, equipment and NPC
setup costs. They preserve configured NPC visibility and do not change spell,
rain, fog, collision accuracy or movement subdivision. No asset conversion is
needed; rebuild and copy EBOOT. Hardware frame-rate gains remain unmeasured.

## Minimap

Marker classification and unit-position snapshots refresh every 100 ms, rather
than querying every marker's world position on every frame. Zoom, target and
font changes refresh immediately. Cached positions are reprojected whenever the
player or camera moves, and projection is reused while the view stands still.
The player arrow and corpse pointer still draw from current state every frame.
Other units' dots/status can lag by at most one snapshot interval, plus frame
delay. Quest-symbol widths are measured once per snapshot. Tile loading and
existing exact disc-clipping invalidation remain unchanged.

## Wall-cache investigation

The existing two-entry geometry cache keeps the same allocation limit below
20 KiB. Replacement now uses actual recency, so a frequently reused entry is not
discarded just because its round-robin turn arrived. Miss counters distinguish
generation, placement-transform, cell-range, cold and capacity replacement
causes. This supplies evidence before increasing memory. Full ordered fallback,
overflow handling, contact resolution and floor queries remain unchanged.

## Equipment preparation

The remote-player dress queue stages one new record/layer dependency per step,
skipping dependencies already cached. Existing worker results remain staged
until the body composer consumes them; routing all through the smaller layer
cache would evict earlier layers and repeat reads. Newer outfits supersede
in-flight preparation. Existing body appearance stays until preparation ends.
Body composition/model loading and attachment installation use separate queue
turns, with at most one completed appearance job per frame. NPC equipment retains
its existing asynchronous path and per-frame install limit.

This is cooperative staging, not a hard time limit on file I/O: model creation,
body composition and individual attachment installation still contain atomic
work. Missing/uncacheable/expired staged assets retain existing fallback behavior.
Existing staging-slot limits, expiry and heap reserve checks remain in force.
Own-character equipment is not delayed by this remote-player queue.

## NPC setup

Each loaded display retains the selected batch indices for LOD, hair/scalp and
opaque/blended variants. These selections are rebuilt on display mask or geometry
changes. Every instance still submits its current model/bone matrices, textures
and batches in the original selected order; outline filtering remains live.
Skinned units no longer participate in the static-model grouping sort, whose
results were ignored for them. All existing visible-NPC priority and draw/list
safety rules remain in effect. This reduces setup; it does not reduce NPC count,
triangles or bone-upload requirements.

## Verification and logs

Host tests exercise production minimap/arrow and skinned draw routines, all
selection variants, 3,000 repeated NPC submissions, pending equipment preparation,
superseded outfits, staging buffer ownership, LRU hot-entry retention and miss
classification. Collision tests compare 12,000 wall and 4,608 transformed floor
queries with the full resolver; ramp/landing and asynchronous installation tests
also pass. Affected source syntax is checked against PSP SDK headers. A full PSP
cross-build and actual PSP/Vita playback were not available here.

Use the same route, camera and settings to compare logs. New summaries:

- minimap_cache: snapshot and projection counts; arrow=live.
- wall_cache_miss: reasons and evictions alongside existing hit/build totals.
- equipment_staging: preparation yields, completed jobs and queue size.
- npc_batch_plan: cached selection hits/builds; visible_preserved=1.

These indicate reuse, not proof of a fixed 30 or 60 FPS. Compare frame_cost and
profiler frame/build/GE timing as well, keeping login and character selection out
of gameplay averages.
