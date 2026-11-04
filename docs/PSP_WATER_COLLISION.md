# Water, movement and performance changes

Based on WoWee-psp-port `628489e2927fabe447b09f1957b41e63b82133c3` (psp-port), with Kiln `0366c1f` (main).

## Use

Build the port normally with `bash tools/psp/build_psp.sh --no-bump` under Linux/WSL with pspdev. This change does not bump the release/build number.

Existing `sounds.pak` works: masked terrain water uses its original per-chunk height. For sloped surfaces, rebuild `psp_sound_conv` from the accompanying Kiln source and run Kiln's sound step again (or Update assets after replacing that converter). Copy `sounds.pak` and any sound pak parts together. The sound step now exports grids for all extracted terrain maps via `--all-maps`, rather than map 0 alone. No other game assets need conversion. Kiln fingerprints the converter executable, so replacing it invalidates the sound step.

`water=1` enables rendering; `water=0` disables only drawing. Swimming still uses water data. `waterview=200` controls the water draw range, clamped to 40-240 yards. `sfx=0` now keeps surface grids available while disabling audio clips.

## Implementation

* Transparent, depth-tested terrain water uses exact cell masks, a small procedural scrolling ripple texture, camera-relative vertices, frustum culling, fog and back-to-front chunk ordering. Water draws over submerged actors before particles. Near surfaces are subdivided for the GE guard band.
* Water scratch storage is fixed. The pass uses at most 192 KB of the existing display list and reserves 96 KB for subsequent effects/UI. Under list pressure, farther water is omitted. Four intersecting ADTs fit the six-slot surface cache. New grids typically add 336 bytes per liquid layer; the absolute cap is 1024 layers/tile. Allocation checks keep a 256 KB heap margin. WMO-only liquids, reflections, refraction and distinct magma/slime materials are not implemented.
* Kiln WSRF v2 retains the complete v1 prefix, then adds a `WWTR` table of 336-byte masked 9x9 height patches. The client validates versions, lengths, counts, chunk indices and finite heights. Old grids remain supported. Swimming and water walking interpolate the same triangles as rendering, using the highest visible layer.
* Movement checks walls and floors together in slices no larger than 0.25 yard and 1/60 second. The 64-slice cap reduces simulated time rather than enlarging steps. Pending terrain/doodad tiles and big-WMO collision block entry until loaded. Both building wall paths now agree with the 0.6-yard floor step height.
* Ray/triangle shared-edge tolerance closes rounding cracks, and terrain height queries match the rendered triangle diagonal instead of a bilinear patch. Void recovery measures total drop since fall start; teleports invalidate old recovery positions.
* Model inverse transforms are cached with the instance grid (40 extra bytes/instance); triangle slope classification avoids square roots; level/downhill terrain queries skip four unnecessary slope samples. Kiln decodes each terrain alpha layer once per chunk instead of once per cell (up to 64 times fewer decodes).

## Validation and remaining device checks

Host C++ regression executables pass for seams, reversed/degenerate triangles, thin-wall substeps, fast mounts/long frames, water masks, diagonal interpolation, legacy/v2 validation, corrupt/truncated grids and synthetic Kiln baking. Kiln's 48 Python tests pass, including an end-to-end bake/verify of two synthetic maps. All modified/new runtime translation units pass Clang syntax checks against PSPSDK headers, with a host-only shim for Windows macros/newlib declarations. This is not a PSP link/build.

No PSP EBOOT was produced, and no FPS improvement is claimed without profiling. WSL was inaccessible in this environment. Device/emulator verification still needs: run and jump across shared floor edges; traverse stairs/fences and narrow bridges at low FPS/on mounts; cross streaming boundaries; jump/land in a WMO; teleport between maps and verify recovery; check ponds/coasts/tile edges while swimming; compare frame time, display-list use and heap in the same camera view with water enabled/disabled.

The host regression is registered as `psp_water_collision` in the normal CTest build. It can also be compiled directly:

```sh
c++ -std=c++20 -O2 -Iinclude -I/path/to/glm tests/test_psp_water_collision.cpp -o /tmp/psp-water-test
/tmp/psp-water-test
```

The follow-up spell changes are described in [PSP_SPELL_EFFECTS.md](PSP_SPELL_EFFECTS.md).
