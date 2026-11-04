# Ground-decoration removal and streaming regression

Generated grass, flowers and ground decoration are removed from the PSP port. The distance/density settings and the dynamic clutter-pruning/reload cycle are removed. Trees, ordinary bushes, fences, buildings, terrain textures, NPCs and collision keep their existing paths.

## Evidence and resulting behaviour

The latest supplied session log (the 208-second run) shows roughly 15-16 ms/frame of streaming self time, repeated clutter_prune messages, and repeated top-ups installing almost no new data. The previous change moved plants in and out of deferred lists and rebuilt the grid repeatedly. The readiness diagnostic also scanned deferred records every frame.

The runtime now permanently excludes models carrying the legacy kModelGroundDetail flag. An old world pack remains usable: decoration headers are consumed once, without texture loads, instances, seen-ID nodes or deferred retries. Prefetched decoration is released before its atlases are requested. The exporter no longer generates ground-effect placements or exports authored ground-detail models, so optional rebuilt packs also remove the unused disk/tile data.

Streaming readiness is polled at most four times per second instead of scanning the window every frame. This affects diagnostics and starvation detection, not collision readiness, movement or terrain installation. A stream.ready profiler zone makes its remaining cost visible. The placement-memory accounting and OOM guards remain, as do quest-marker wall occlusion and the removal of the white object reveal.

## Validation

Production-loader host fixture: 20,000 legacy grass placements produce zero textures, render instances or deferred retries; their header loads once and stays excluded across retry resets. Ordinary building placement still succeeds. Readiness polling tests pass at four scans per second, including timer wraparound. Host UI/asset regressions, settings-category checks, marker visibility, object-fog checks and collision references passed. 12,000 moving wall queries and 4,608 transformed floor queries match the reference. SDK-header host syntax checks and the native model converter build passed.

No hardware FPS improvement is claimed yet. The fixes target the streaming regression recorded in the supplied log; remaining particle, collision, UI and rendering costs still apply. Re-test the rebuilt EBOOT and send a new log if the slowdown persists.

## Build and copy

Only the EBOOT needs rebuilding to remove decoration and test the FPS fix with the current packs:

```bash
bash tools/psp/build_psp.sh
```

Copy the resulting EBOOT.PBP from the repository's psp/bin folder to the game folder. Audio and UI packs do not need rebuilding for this change.

Optional: reclaim the unused decoration data by rebuilding world packs in MSYS2 UCRT64:

```bash
bash /c/Users/busch/Documents/Codex/2026-10-26/usin/outputs/rebuild_world_kiln.sh
```

Merge C:/Kiln-output/data/doodads_azeroth.pak and its matching data/bigwmo/ companions into the existing game data folder. Never substitute the partial probe packs under outputs/checks for the full map pack. The legacy rebuild_grass_kiln.sh command forwards to this world rebuild, which no longer produces grass.
