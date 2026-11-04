# Undercity entrance collision wait

Two code paths could indefinitely block normal walking: low-memory mode gave BigWmo::update zero request slots, even while collisionReadyAt waited for missing nearby collision; and the visual budget could cut a collision blob still required by that readiness check. This update permits only essential nearby collision requests during pressure and keeps that collision at the front of the working set, displacing visual detail. Optional/distant reads remain deferred and allocation/read failures retain their existing retry behavior.

The memory governor also retains a WMO containing the player even if its previous portal result says outside and no visual chunks are drawn. Opening priority considers both the camera and player, so a camera outside the arch cannot exclude the building the player needs.

A hold at the current position or a proposed step now sends MSG_MOVE_STOP and clears local movement intent instead of leaving the preceding movement packet and animation active. This can explain a stationary local world/camera while other clients extrapolate continued movement. It does not prove that every report of a frozen view has this cause.

collision_wait logs show the position and resident/required WMO collision count every five seconds during a hold. world_view logs give player/camera coordinates every ten seconds. The separate display-state reset keeps depth clearing independent of fast sky. These changes require a new EBOOT, not asset rebaking. Actual PSP reproduction remains required.
