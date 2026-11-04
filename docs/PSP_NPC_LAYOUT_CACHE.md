# Retained quest/gossip text layout

Gossip, quest details, required-items progress and reward dialogs now retain
their formatted content separately. Character token substitution, Russian
declension markup, word wrapping, item-name wrapping, coin formatting, label
measurements and row positions run when the layout changes, rather than every
draw. Long NPC names keep their animated marquee using a retained width.

Invalidation keys cover conversation packets, relevant late item-name/quality
replies, late NPC text/name information, character GUID/name/race/class/gender,
locale, font reload revision and drawing scale. Item replies unrelated to items
in an open quest dialog do not invalidate its layout. Two open dialogs keep
separate layouts. Closing a window releases its retained content on the next
NPC update; a new WorldScreen owns new caches on character changes.

Scroll position, focus stops, pressed states, reward selection, portraits,
tooltips, icons, and Continue availability remain live. Selection and scrolling
do not rebuild static text. Geometry, colors, wrapping rules and action IDs
retain the existing behavior; this does not redesign the windows.

The shared button/marquee helpers accept optional measured widths. Other callers
can omit them and retain their normal behavior. Font revisions increment on
load/release and successful reload, so allocator address reuse cannot hide a
font change from the layout key.

Host validation runs the production dialog routines with the real text-token
resolver and captured UI output. All four dialog types run for 1,000 unchanged
frames each without font measurements or rebuilding. Tests cover live focus,
scrolling, reward highlighting, overlapping windows, late greetings/items/names,
and character/locale/font changes. An additional run compares text positions,
colors and highlights against the pre-cache implementation. PSP SDK header syntax
checks cover affected source. Full EBOOT compilation and hardware FPS gains
remain unverified.

Rebuild and copy EBOOT only; no asset conversion is required. Session logs emit
npc_layout window/builds/lines on rebuilds. The profiler's npc.layout section
isolates rebuilding from npc.frame drawing, which still submits text and art
each frame. This removes repeated preparation, not all dialog rendering cost.
