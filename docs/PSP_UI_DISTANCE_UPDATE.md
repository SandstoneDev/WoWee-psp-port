# UI, nearby animation and placement work

## Changes

- Bag frames keep their authored bottom-right anchors (right 478, bottom 240, above the bag bar). Automatic side-by-side placement no longer relocates them. Saved manual positions remain valid. The no-art fallback also ends at y=240.
- L plus stick moves the front non-modal panel. Releasing L commits one config write. Circle restores the starting position. Neutral-stick L comparison, L plus D-pad panel cycling, START handling, and the explicit Move control remain available.
- Settings help wraps using the actual button-glyph/text widths, clips to its own region and cycles additional pages every five seconds if needed. Wrapped help is cached until content, font revision or scale changes. Page membership is built once rather than allocating strings and scanning the schema every draw/update.
- Nearby NPCs bypass both configured and adaptive pose-count caps. The default full-rate radius is 20 yards (`npcanimdist`; existing explicit values are respected). Farther NPCs retain phased bone updates; their animation clocks and movement remain live. Important actors and initial poses remain protected. Visible NPC count and rendering distance are unchanged. Crowds entirely within the full-rate radius may cost more CPU than before.
- Transport placement geometry is reused while its set and object data are unchanged. Motion invalidates only its own renderer slot. Model swaps, retries and server object changes still rebuild. Moving platform evaluation, rider transforms, and collision motion remain per-frame. `transport_work builds=... reused=...` identifies reuse in future logs.
- Settings / Effects / Ground clutter controls drawing distance for existing `kModelGroundDetail` placements: 0 (off) through 50 yards, default 25. Both model draw paths use it; collision is unaffected. This does not add terrain-generated grass or unload existing assets. No pack format or Kiln conversion change is involved.

- General text/name keyboards follow the firmware system language instead of forcing Russian. Explicit numeric, Latin-only, and Cyrillic-only fields retain their restrictions. The dialog language has an English fallback if the system query fails; `osk_language` logs the console/keyboard selection. On Vita this uses the PSP/Adrenaline firmware language exposed to the game.

## Hardware budget from supplied log

The attachment 847ca73d-bb46-45a8-8230-e876e26c685d is byte-identical to the previous supplied attachment 05cbb359-3483-410f-a457-1da08b0ec2c7.

Before the user changes framecap to 30 at 97.959 seconds, the settled 45-97.959 second profiler samples average 28.3 FPS, 26.8 ms frame building and 21.5 ms GE busy. These measurements overlap and must not be added. Each already exceeds the 16.7 ms allowance for 60 FPS. Waiting for the next display refresh also quantizes frame presentation. This is workload exceeding the deadline, not evidence that the 60 FPS setting still requests the 30 FPS interval.

The later 305-505 second samples average 29.6 FPS with the 30 FPS target, 29.3 ms building and 26.6 ms GE busy. They do not measure uncapped performance. Some frames exceed 150,000 triangles, and windows add measurable work (talents roughly 4.5 ms, bags roughly 2.6 ms, settings roughly 2.5 ms in respective samples).

There is insufficient measured spare time to promise full-density WoW ground grass in these scenes. The PSP converter currently does not bake the terrain-layer GroundEffectTexture/GroundEffectDoodad placements used by the desktop renderer. A future grass implementation needs those authored assets/placement rules, bounded model and texture residency, short-range density limits, and hardware measurements. The new setting controls existing packed clutter only.

Next larger gains to investigate: cached talent ordering/labels and bag layouts keyed by content changes; cached minimap blip draw geometry; fewer distant world-model batches and texture binds while preserving nearby silhouettes. Stable 30 requires frame work below 33.3 ms with streaming margin; 60 requires both CPU and GE below 16.7 ms. The near-animation change prioritizes appearance and can consume some savings.

## Validation

Host checks cover L-stick release/save/cancel and modal protection, bag auto-placement exclusion, actual glyph width wrapping and UTF-8, near pose protection, transport transform equivalence over 3000 slot-frames, collision regression against brute force, and the production OSK charset switch against SDK language/type constants. In the transport fixture, moving transforms rebuild 1000 times; each unchanged slot rebuilds four times rather than 1000, including explicit data changes and reload. This is operation-count evidence, not a PSP timing prediction.

All changed source files pass MSVC syntax checks against PSP SDK headers using host compatibility shims. A PSP binary has not been built or tested on hardware here. Rebuild EBOOT.PBP; no asset pack replacements are required for these changes.
