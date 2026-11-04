# Adaptive rendering at 30 or 60 FPS

Settings > Display has two independent controls: FPS target (30/60) and Adaptive
rendering (On/Off). Both apply immediately and save when the menu closes. The
default remains 30 FPS with adaptive rendering enabled. At boot and in the world,
30 uses two-vblank presentation and 60 uses one, regardless of the adaptive toggle.

The config keys are framecap=30 or 60, and adaptive30=1 or 0. The legacy adaptive30
key name is retained so existing configurations work; it now applies to both
targets. On uses the PSP normal maximum CPU clock, 333 MHz. Off restores the prior
CPU clock and fixed detail limits while keeping the selected FPS target. Automatic
quality levels never overwrite the user's detail settings.

The controller measures input/update/render/GE-completion work before waiting
for vblank. It does not treat deliberate frame-cap idle time as overload.
At 30 FPS, sustained work above roughly 29-31 ms reduces quality; work below 22-24 ms
must remain healthy for eight seconds before one quality step is restored.
At 60 FPS those thresholds halve to 14.5-15.5 ms and 11-12 ms. Sustained severe
overload above 65 ms (30 FPS) or 32.5 ms (60 FPS) can use the emergency tier.
There are six quality levels (0-5); startup uses the balanced level 1. Individual
loading stalls are bounded in the filter to avoid immediately collapsing quality.
Changing FPS targets resets timing history and recovery timers while preserving
the current quality tier, avoiding an immediate jump to full detail.

Under pressure it brings terrain LOD transitions closer, reduces the near
terrain detail pass, reduces distant doodad render distance, uses earlier NPC
LOD, and reduces off-screen crowd caps. Visible NPCs keep their configured view
range and bypass numeric draw-count cutoffs; nearby visible NPCs load first.
Setting NPC count to zero still disables them. Animation poses can update less
often; memory, bone-upload and display-list safety limits remain in effect.
Adaptive player and prop limits keep their existing behavior.

World particles use fewer emitter instances and particles. Rain/snow and spell
effect density follow their own settings. Remote weapon glows are omitted at
the lower levels. Changing quality does not reopen packs or cancel active spells.

This is a performance target, not proof of a locked hardware frame rate. It
cannot remove every card-I/O stall, network burst, or expensive building draw.
The PSP build and worst-case towns/combat must still be measured on real hardware.
The existing world_frame log reports FPS/GE timing; the adaptive log records
enabled, target_fps, quality transitions and averaged work_us. Compare identical
routes/scenes at each framecap with adaptive30=0 and 1 on the same PSP and data.

Validation: production FrameBudget regression compiled and ran with MSVC,
including both target budgets, sustained overload, slow recovery, one-off stalls,
target changes, Off/On, zero/low crowd limits, and invalid timing. World/settings
source syntax checks passed with PSP SDK headers. Full PSP compilation and
hardware playback were not performed in this environment. Only EBOOT needs
rebuilding and copying; no asset packs or Kiln conversion are required.
