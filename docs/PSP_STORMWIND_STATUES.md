# Stormwind entrance bridge heroes

The hardware log disables big-WMO optional doodads before visiting Stormwind. The five hero statue models exist in the exported packs, but shared the optional-prop path.

The baker now appends a bounded landmark table to the WMO header, identified by original client model names (Alleria, Danath, Khadgar, Kurdran and Turalyon), not bake-specific IDs. The runtime can assemble nearby landmarks even while the memory governor cuts optional props; normal memory probes and loading limits remain active. Landmark instances use normal model drawing, depth testing, fog and distance/frustum culling. Their exterior visibility does not depend on an indoor portal group. Duplicate placements in resident doodad cells are filtered. Old packs without this extension remain readable.

The matching local Stormwind pack has eight placements using the five models. The combined packed mesh/collision/texture payload is 416,836 bytes, plus instance/allocator overhead at runtime. The metadata adds 612 bytes to its header. Existing world stamp, geometry, terrain and collision references are unchanged. This is not a general bypass of memory pressure.

## Install

1. Rebuild EBOOT using `bash tools/psp/build_psp.sh` and copy the resulting `psp/bin/EBOOT.PBP` to the PSP game directory.
2. Merge `outputs/Stormwind-statue-fix/data` into the game's existing `data` directory, replacing only `data/bigwmo/azeroth_stormwind.pak`.

This prepared asset matches the current local world bake, stamp b70ab5f7, and its `doodads_azeroth.pak`. For a different world bake, rebuild world assets with the updated Kiln converter instead; model IDs and stamps must match. A complete world rebake is unnecessary for the matching existing packs.

`tools/psp/repair_wmo_landmarks.py` can construct the same extension from an existing pack and its original extracted WMO without changing any geometry offsets. It requires a separate output path.

## Validation

- Full PSP UI/asset host suite passed, including malformed/truncated/oversized landmark tables and original model-name matching.
- Production doodad iterator tested with governor-only selection, absent optional cells, duplicate filtering and doodad sets.
- Real pack verified: all original geometry/collision/texture bytes, TOC names and world stamp unchanged.
- PSP SDK header syntax checks passed for big-WMO renderer and world integration.
- Updated native Kiln model converter built successfully.

A new PSP EBOOT has not been built here. Final visibility and memory behavior need testing on PSP/Vita hardware.
