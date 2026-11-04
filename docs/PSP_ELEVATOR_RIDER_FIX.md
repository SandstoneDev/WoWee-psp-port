# Elevator rider synchronization

The platform animation uses the transport/server clock; character physics caps its timestep. Boarding was
checked only after advancing the lift, with a 0.05-yard tolerance. On a slow frame a departing elevator can
move beyond that contact window before the player is registered as a rider.

Grounded contact is now checked against the previous frame's collision and platform transforms before
updateTransports advances them. Boarding tolerates up to 0.25 yards of position/collision quantization and
requires an actual walkable collision floor. ModelRenderer::floorHeight reports the winning placement ID,
including on memoized queries, so boarding attaches to the specific supporting transport part rather than
choosing the smallest overlapping object box.

Transport carrying runs before teleport/collision holds, and reconstructs the rider position from the same
current platform transform used to render the lift. It is not scaled by the clamped character timestep.
Grounded riding clears obsolete vertical velocity/fall timers; airborne riders keep gravity/jump behavior,
with their fall-origin height translated by the platform's movement. Actual jumps/falls still follow the
normal landing/damage path. No fall-damage immunity or elevator speed change was added.

Existing part IDs and movement packet structure are unchanged. Rebuild EBOOT.PBP; no asset rebake is needed.
Board/leave logs remain; transport_carry now logs vertical delta, local feet height and airborne state while
riding, at five-second intervals.

Host regression tests exercise fast descending contact, rejection of airborne players, exact rider offsets
at 3/9/15/30/60 FPS with rotation, and transport ownership across part/set ID changes. These test the shared
production helpers; full runtime integration, elevator timing against a server and PSP traversal still need
hardware verification.
