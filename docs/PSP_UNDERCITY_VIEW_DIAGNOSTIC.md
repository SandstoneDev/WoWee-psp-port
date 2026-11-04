# Undercity world-view freeze investigation

Hardware report: crossing the entrance archway leaves the world picture unchanged, while HUD and menus continue updating. The earlier room-culling diagnosis is not confirmed by this symptom.

This update resets the framebuffer, depth buffer, viewport, scissor, pixel mask and reversed-depth state at the start of every frame. It clears depth even when fast sky or a movie replaces colour. Previously frameBegin inherited these states and skipped both clears with skyfast; correctness depended on later passes overwriting every depth pixel. This is a defensive render-state fix, not a hardware-confirmed root cause.

Every existing ten-second world_frame sample now includes a world_view sample with player/camera position, forward direction, collision distance and dt. If the freeze persists, reproduce it while walking through the arch and keep moving for at least 15 seconds; preserve the session log. Changing camera/player coordinates distinguish stale geometry from a stopped camera or local movement. Keep the last working outdoor samples for comparison.

The existing WMO portal visibility and normal world passes remain in place. No asset conversion is needed. PSP device validation and a cross-build are still required.
