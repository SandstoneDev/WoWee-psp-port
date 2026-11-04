# Bags, crowd lookup stalls and underground terrain

Bag controls: Circle uses/equips an item (or sells it at a merchant), Cross picks up/places, L+Square splits half,
Triangle switches to the next available bag, and Square opens the destruction confirmation for the highlighted
carried item. Switching bags preserves a held item for placement with Cross. Circle cancels the destruction popup.
START cancels a held move. Blue and better items retain the
typed DELETE confirmation. The confirmation defaults to No, rejects trade/mail/auction locks, and is cancelled if
the source GUID or stack count changes. Cross placement uses server-validated stack merging and slot swaps.

The earlier session log recorded long draw.sync/crowd_sync stalls. ItemBank cached only four Item.dbc disk blocks,
while NPC/remote equipment resolves item IDs repeatedly. A bounded 3136-byte result cache now retains both found
and missing IDs, including sheath data, across block eviction. The per-frame NPC fallback that probed unrelated
update fields as weapon IDs has been removed. Unchanged remote appearances reuse their resolved equipment instead
of resolving all 19 slots again after health/combat/entity updates; pending custom-item queries and guild-emblem
changes still get checked. Fully removed terrain chunks skip texture/detail allocations and window requests.
With Performance HUD enabled, item_lookup logs hits, misses and
block reads every five seconds. This targets redundant disk reads; improved PSP frame rates remain unmeasured.
The later session log confirms 98318 item-lookup hits and 271 misses. The remaining
expensive movement path rechecked identical collision-readiness positions after walls made no change; that
redundant scan is now skipped while changed positions still get checked.

That log also shows itemicons.pak open failing with 80020320 (descriptor exhaustion), then being labelled
missing. All icon-open failures are now retried on a timer, even if Getstat fails too. Failed mapping reads are
not cached as unknown icons; failed textures retry, and failed reads close/reopen their descriptor. The shared
item icon cache now holds 96 icons instead of 48. Production-reader host tests cover retries and 70 simultaneous
icons. Existing itemicons.pak data does not need rebuilding.

Adaptive mode keeps all NPCs intersecting the viewport eligible for loading/drawing within the configured NPC
view range, bypassing both adaptive and configured numeric draw-count cutoffs. NPC count zero still disables
the feature. Closest visible units get first access to model/display loading; pose budgets continue staggering
animation work. Memory, bone-upload and display-list safety limits remain and can delay distant models. This
change applies to NPCs, without changing adaptive player/prop limits.

Undercity's entrance problem has an asset cause: psp_terrain_conv explicitly wrote zero for every WTER hole mask.
The ADT loader already reads the original MCNK 4x4 masks correctly. The converter now preserves them, and absent
mesh chunks are entirely holes instead of flat ground. The renderer builds exact strips for chunks with holes,
including cuts in their outer skirts; all LODs and the detail pass use those strips. Fully removed chunks submit
no terrain geometry. Terrain floor queries return no floor inside a hole. Movement/teleport/swimming readiness
checks use tile residency instead of interpreting a legitimate hole as missing streamed geometry. WMO floors,
walls and their existing collision streaming remain active.

The previous global below-surface terrain suppression has been removed. Individual hole masks control topology.
The WMO camera's no-floor fallback is bounded to the group's actual vertical box.

Existing terrain packs can be repaired with small optional data/terrain_holes_<map>.pak files, without rebaking
textures or replacing multi-GB terrain packs. Each t_X_Y record is 256 little-endian uint16 masks (512 bytes),
in the same MCNK order as ADTLoader/WTER. A present override replaces that tile's embedded masks. Failed override
reads cause a tile retry rather than silently filling the entrance back in. The optional pak's descriptor is
released after each tile read. Newly baked terrain packs also work without overrides.

Generate overrides from the same extracted client used for the installed packs:

    python tools/psp/build_terrain_hole_overrides.py --extracted "C:/Kiln-output/work/extracted" --out "<game>/data"

The generated bundle covers 46 maps with hole data (425152 bytes total). Empty/invalid source ADTs were skipped
and recorded in terrain_holes_warnings.txt. No holes are invented for missing source files. Azeroth's 31_28
Undercity tile was checked against the original ADT. The installed runtime must be rebuilt to read these packs.
Both the local Kiln source and exported Kiln source have the corrected terrain converter and override utility.

Validation: native C++ tests exhaust all 65536 hole masks, checking triangles and skirt gaps; the item cache
test exercises repeated crowd lookups, missing IDs, eviction and invalidation. Python tests check ADT parsing,
WPAK alignment/contents, invalid input and solid-tile exclusion. PSP cross-compilation and in-game traversal
still need checking; WSL could not be started in this environment (Wsl/E_ACCESSDENIED).
