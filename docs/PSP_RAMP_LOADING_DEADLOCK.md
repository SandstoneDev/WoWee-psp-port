# Ramp/bridge floating: blocked collision destination

The supplied session repeatedly logs `collision_wait col=9/10`, from 122.473 s
through 147.540 s, around 1626.62, 240.73, -42.42 in Undercity. The pending vertical
probe is about -42.39 while the held feet remain -42.42. Terrain and doodads are
already ready; reason 3 identifies missing big-WMO collision. The airborne
animation remains active while physics is held.

The vertical safety check added earlier exposed a mismatch: readiness checks the
next movement position, while the residency plan only considers the held feet.
A few centimeters of ascent/descent can bring another chunk into the readiness
volume at its vertical boundary. That chunk is not wanted by the old plan, so
waiting at the previous position cannot complete.

The WMO loader now receives the exact blocked collision probe. Changing or
clearing that probe immediately dirties the plan, bypassing its usual movement
threshold/periodic refresh. It keeps both the actual player's neighborhood and
the blocked destination wanted. Collision required at the blocked position sorts
before other resources and remains eligible under visual-budget/memory-pressure
cuts. It retains the normal allocation and I/O retry rules.

Planning and readiness now use the same collisionNeighborhood helper and the
same local-coordinate arithmetic for the blocked position. Successful movement
clears the extra requirement. Floors, walls and gravity are not disabled to escape
the hold. No collider assets or Kiln changes are required.

The host test reproduces a 9/10 deadlock on both vertical boundaries, at three
placement scales, then verifies that the union plan requests the tenth chunk and
readiness can complete. It also executes the production setCollisionProbe method
to verify immediate replanning, clearing, exact repeats and placement transforms.
12,000 randomized neighborhood selections match a brute-force reference. Tests
pass; no PSP build or Vita retest was possible in this environment.

Rebuild EBOOT.PBP from tools/psp/build_psp.sh --no-bump and replace only EBOOT.PBP.
