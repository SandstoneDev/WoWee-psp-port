# Building batching and camera-ray broad phase

## Behavior

Camera collision now uses the existing immutable collision-bounds tree for buildings, props and large WMOs. Slab tests visit intersected nodes near-first. A closer triangle hit shortens the ray and prunes remaining nodes. The triangle intersection routines, exact-input memoization and collision data remain unchanged. The traversal allocates no per-query memory and adds no second spatial index. Model rendering retains its previous full-scan fallback while its grid is dirty or the CPU fast path is disabled.

After the existing portal, frustum, distance, LOD and guard-band selection, large-WMO rendering can combine adjacent compatible opaque/alpha-key batches. Runs must share cell, source group, lighting category, material flags, texture, blend mode and depth band. Transparent surfaces retain their existing back-to-front drawing. Each source's texture coordinates are decoded into floating UVs, while positions and baked colors are copied unchanged. Indexed vertex reuse and triangle strip parity are preserved.

The lazy four-slot cache is bounded below 64 KiB of vector payload per open large WMO, plus small object/allocator overhead. A cache hit performs no conversion. At most one run of 832 strip indices is built per frame. Buffers referenced by the current GE list cannot be evicted or overwritten. CPU cache ranges are flushed before the PSP GE reads new buffers. Geometry eviction invalidates the cache so recycled blob addresses cannot select stale geometry. Memory pressure discards the cache before optional world geometry, and cache construction requires budget and heap headroom. Uncached/oversized/incompatible runs use the original drawing path. Far shells also explicitly retain zone lighting without indexing a nonexistent source group.

New `bigwmo_batch name=... saved=... cache_kb=...` lines report the draw calls saved on the sampled frame and current cache allocation. Existing frame timing and camera-collision timings remain available for before/after comparisons. Savings depend on visible compatible runs; this is a bounded initial batching implementation, not a claim that all building draws merge.

## Verification

- Full PSP UI/asset host regression runner passed.
- 10,000 seeded ray queries against 4,096 boxes matched the reference scan, including parallel, negative and non-unit rays. The synthetic fixture visited 5,219 leaf candidates instead of 40,960,000 full-scan entries; this is a broad-phase fixture result, not a measured PSP speedup.
- Nearest-hit pruning, empty trees, indexed/nonindexed mesh inputs, UV/color/position preservation, strip connectors/parity, cache reuse, pinned buffers, memory cap and blended/oversized fallbacks passed.
- 128 pairs of actual local Stormwind mesh batches were merged from two draws to one with every source strip vertex, color and decoded UV checked. This asset fixture does not reproduce a live camera's visibility or establish an FPS gain.
- PSP SDK header syntax checks passed for `psp_bigwmo.cpp`, `psp_models.cpp` and `ui_world_bigwmo.cpp`.

## Install and measure

These runtime changes require EBOOT only; no new asset bake is needed. Existing asset updates from earlier fixes remain applicable.

Rebuild in WSL:

```bash
bash tools/psp/build_psp.sh
```

Copy the rebuilt `psp/bin/EBOOT.PBP` into the game directory. A PSP EBOOT was not built in this host session. Retest the same Stormwind/Brill route on hardware, keeping camera direction, FPS target and graphics settings the same. Compare `col.camera`, `col.bigwmo`, `bigwmo_batch`, WMO draws, CPU build time, GE busy time and frame time. No hardware FPS improvement or constant 30/60 FPS is claimed before that measurement.
