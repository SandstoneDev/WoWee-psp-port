# Object transitions through fog

The world fog previously followed the zone's far-distance band while static
objects often used much shorter, size-scaled draw distances (and shorter
adaptive limits). Objects could therefore cross their cull boundary before
the world fog hid them. Narrow authored fog bands could also become a sharp
wall after fitting to the PSP's projection.

Zone fog now has a minimum fade width of 28% of its existing end distance.
Its end, colour and far clip are preserved. Existing broader bands retain
their authored proportions. This does not pull the entire world's fog end
in to the nearest object/NPC budget.

Static world models now use an additional object-local fog transition in
the last quarter of their own draw range (12–40 yards, at most 40% of a very
short range). At the cull boundary the model is effectively fog-coloured;
it reveals smoothly on approach. Ground clutter uses its own shorter limit.
Normal nearby objects keep their original fog. Newly loaded model geometry
also reveals over 400 milliseconds rather than appearing at full contrast
immediately. The timer belongs to the loaded model resource, so rebuilding
the cull grid does not restart the reveal on every instance.

Both CPU rendering paths apply this to opaque, cutout and blended batches.
Tree impostors also participate. Normal impostors retain their grouped atlas
draws; only the transition shell/new resources split out individual fog draws.
Fog start changes are cached to avoid repeated identical GE commands. Each
subsequent terrain/NPC/character pass restores its original zone fog state.
Material alpha, collision and depth rules are unchanged. There is no additional
geometry pass, no expanded distance budget, no new asset format and no pack
rebake. Rebuild EBOOT.PBP to deploy.

This targets static object distance and resource-loading transitions. It does
not guarantee that geometry discarded by display-list/triangle limits, missing
assets, or portal/room visibility will never appear abruptly. Mesh-to-impostor
LOD transitions and NPC/player resource/cap transitions are separate policies.

Validation: the existing host fog-policy test checks broad bands, monotonic
reveals, normal/adaptive/clutter limits, clear nearby visibility, resource
timing and timestamp wrap. test_psp_object_fog.py exercises the production
object-selection and GE-state functions, including identical-command caching,
zone colour/end preservation and next-pass restoration. PSP SDK-header syntax
checks pass for models, impostors, lighting GE and world lighting. A PSP/Vita
build and visual/performance checks on hardware remain required. No measured
hardware FPS benefit is claimed; transition-shell impostors can add draws.
