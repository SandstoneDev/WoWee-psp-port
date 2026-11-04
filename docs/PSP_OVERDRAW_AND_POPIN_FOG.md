# Building overdraw and distance pop-in

The supplied session records settled crowded Undercity samples around 13–15 FPS,
roughly 66–73 ms per frame. Its WMO pass is about 10–15 ms in those samples;
NPC reads, collision walls and building planning also cost time. The worst
2.3/4.1 FPS windows overlap world entry, so they should not be treated as steady
scene FPS. Later outdoor samples frequently reach the 30 FPS cap. These views
are not a controlled before/after comparison.

Opaque and alpha-key WMO batches now sort into 32-yard depth bands before
texture/cell grouping. Nearby opaque surfaces establish depth first, aiming to
reduce hidden pixel work in distant rooms. Transparency retains its original
back-to-front ordering. Band keys are computed once while building the draw list.
This can increase material switches; a device A/B test is required to determine
the net frame-time effect. `wmofrontsort=0` restores the previous material-first
ordering without reverting the source. Default: enabled. Geometry, portal
eligibility and collision are not removed by this change.

The added pop-in fog has been removed. Fog uses the existing zone-authored
start/end ratio, color and classic fog setting again. No NPC or adaptive distance
budget changes the fog. The old `fogpopin` key is ignored. Background NPC loading
remains enabled as before this fog revert. Rebuild EBOOT only.

Host draw-order tests verify opaque occlusion on a synthetic overlapping scene,
opaque/alpha-key precedence, unchanged transparent order, material grouping
within depth bands and strict comparator ordering. Fog tests verify the classic
projection fit, authored range ratios and invalid input handling.
Tests pass. No PSP build or hardware speedup has been verified here.

Rebuild tools/psp/build_psp.sh --no-bump, replace EBOOT.PBP only. Existing asset
packs and music remain usable. The log reports wmo_draw_order at startup;
light_frame reports the applied fog range.
