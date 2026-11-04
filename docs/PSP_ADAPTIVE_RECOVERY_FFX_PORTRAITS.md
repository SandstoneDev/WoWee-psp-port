# Adaptive recovery, FFX Glow and close player portraits

The adaptive controller previously held reduced detail at a steady 29.5 ms of
work with a 30 FPS cap, or 14.75 ms at 60 FPS. Both are below the presentation
deadline, but the previous 29/28 ms cut/recovery thresholds prevented recovery.
The new cut thresholds are 32 ms raw / 31 ms smoothed at 30 FPS, and recovery
requires raw work below 31.5 ms and smoothed work below 30 ms for three seconds
per tier. Values halve at 60 FPS. Sustained overload still quickly reduces detail;
isolated stalls cannot trigger instant quality toggles. This gives approximately
10% work headroom for a recovery attempt. RAM-governor cuts still require memory
headroom before they can be restored; this change does not bypass that protection.

Full-Screen Glow was subsequently removed at the user's request, including its setting, preset integration, GE pass and VRAM reservation. Adaptive recovery and portrait framing remain.

Player portraits now frame the head attachment at 0.15 character-height half-span,
instead of the old minimum 0.22 span plus full-body/crown constraints. Their
centre is 0.02 height below the head anchor. This is roughly 1.5-1.8x closer for
ordinary upright heads, and avoids zooming back out for hunched models. The shared
camera applies to character-based player/target/party portraits; equipment and
customization remain visible within the closer face crop. Cached static rendering
and customization/equipment invalidation are unchanged. Helmets/hair can be cropped
at the edge like a close portrait; race-specific visual checks remain necessary.

Validation: regression tests for near-target recovery at both caps, quick reduction
after a costly restore, isolated spikes, preset defaults,
portrait framing over sampled race heights/head anchors and cached rendering.
SDK-header syntax checks cover Settings and the lighting integration. Actual
PSP/Vita appearance and sustained FPS remain unverified.

Rebuild EBOOT.PBP only. No UI, audio, character or world pack rebake is required.
