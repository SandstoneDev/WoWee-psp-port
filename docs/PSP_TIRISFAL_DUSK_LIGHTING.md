# Tirisfal dusk visibility

The a13f4bb1 session records zone 85, light volume 27, clear weather and realm
time 18:04. Outdoor tint stays at 0.22 while the nearby Undercity renderer has
no interior draws. This rules out an interior batch being drawn over Brill;
it does not establish exactly which prior build introduced the difference.

The flat-ground sun-angle approximation removes almost all diffuse light at
the horizon. Since the PSP meshes have baked lighting and no runtime normals,
the same loss affects walls, trees and characters, not just horizontal ground.
The authored evening colors already dim the scene.

`outdoorDiffuseWeight` now retains diffuse fill in zone 85 at dusk. The logged
RGB values produce an average tint near 0.50 instead of 0.22 at default gain.
The fill transitions continuously into moonlight; normal daytime and deep
night samples keep their previous direct contribution. This is a PSP visibility
compensation, not a claim of pixel-identical original-client lighting.

Other zones, including Undercity (1497), retain their previous weighting.
WMO group lighting flags, baked underground interiors, zone color bands, fog,
weather and time progression are unchanged by this fix.

Host regression `tests/test_psp_outdoor_light.cpp` passed for the logged RGB,
day/night samples, horizon continuity, moonlight disabled and other zones.
Actual hardware appearance still requires checking. Rebuild and replace only
EBOOT.PBP; no light.pak or other asset rebuild is needed.
