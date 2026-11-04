# Repeated room queries

BigWmo determines both camera and player room membership during every draw. This
can scan multiple group collision chunks even while both positions stay fixed.
An exact four-entry local-coordinate cache now reuses those answers. Visibility
mode is part of the key. Moving the camera/player always queries the new point;
collision installation, collision eviction, and building close invalidate the
cache. Portal traversal, draw eligibility, NPC counts, spells and rain are unchanged.

The host test covers 1,800 stationary frames with two positions (two membership
computations instead of 3,600), sub-step movement, visibility mode changes,
collision arriving/disappearing, bounded eviction and non-finite coordinates.
It passes. The cache is about 100 bytes per open building. A PSP build/hardware
FPS comparison remains required; this reduces repeated CPU work but does not
remove GPU or cold NPC-loading limits. Rebuild EBOOT.PBP; no asset update needed.
