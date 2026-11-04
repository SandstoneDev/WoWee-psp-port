# Terrain walls visible inside buildings

The user reports a terrain-looking wall on Brill inn's bottom floor which
can be walked through. One matching rendering mechanism is the eight-yard
vertical skirt added to every terrain chunk edge. Skirts have no collision
and can extend into basements even when the ground above is correct.
This is a likely cause, not a confirmed diagnosis from a screenshot.

The terrain renderer now stores the surface-only prefix of each index strip.
Near the camera, when a conservative neighbor margin lies inside the full-detail
LOD range, it draws only the surface. Farther away and around LOD transitions,
it retains the skirts that hide seams. Terrain-hole strips use the same policy
without filling any authored holes. The detail pass overlays surface indices
only. No collision heights, building meshes or terrain assets are changed.

The exhaustive 65,536-mask terrain regression passed, including surface-only
prefix checks and near/transition policy checks. Host syntax checking passed.
The Brill inn appearance still needs an in-game check with the rebuilt EBOOT.
If the wall remains, obtain a screenshot and a fresh session at that location;
do not remove terrain surface chunks blindly or create new collision holes.

Replace EBOOT.PBP after rebuilding; no asset rebake is required.
