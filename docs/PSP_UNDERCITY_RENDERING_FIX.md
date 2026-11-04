Undercity rendering and NPC-priority correction

The global indoor/outdoor suppression added in PSP_POLISH has been removed. Sky, terrain, buildings, props, water, game objects, NPCs and players use the normal world passes regardless of room classification. The building renderer retains its existing internal chunk/frustum/portal path; there is no new room prepass or per-unit room/floor query. Old indoorcull config values are no longer used.

Tile movement readiness now distinguishes installed geometry from an in-flight background top-up. Top-ups no longer repeatedly pause walking on a resident tile; initial missing geometry still holds movement.

The entrance collision fix remains: collision is selected in full 3D, independent of render visibility, and antiportal geometry is not required for movement. Movement now waits for a 1.5 yd neighborhood rather than 12 yd. A 3.5 yd mandatory loading neighborhood covers movement/planner margins; optional prefetch is capped at 10 yd and no longer takes priority over all nearby visible geometry. A rejected oversized streaming item does not spend the remaining budget, allowing smaller meshes/textures to load. Overlapping WMO placements each have independent readiness state. Collision checks and walls remain enabled.

NPC candidates are ordered before display lookups/model loading: controlled rider body first, then camera viewport, then distance from the player, with GUID tie-breaking. Other players' mounts and off-screen selected targets have no general world-render priority over nearer visible units. Target portrait-only records do not spend world draw slots. NPC model loading and world drawing use the same selected order.

Adaptive detail alone producing 3-5 FPS improvement does not establish the remaining bottleneck. This update removes specific CPU/streaming overhead and adds a frame_cost diagnostic when Performance HUD is enabled: work, terrain, building/props, crowd sync and NPC CPU times, plus NPC model/pose/load counts. Work time includes more than these draw CPU counters (including other updates and GPU completion). Hardware measurements are still required; this patch makes no 30 FPS guarantee.

Rebuild EBOOT.PBP only. No map, collision, character or sound asset conversion is needed for this correction. Host tests cover collision budget admission and nearby/viewport NPC ordering; changed runtime C++ is checked against PSP headers. Actual Undercity traversal/rendering requires PSP/PPSSPP verification.
