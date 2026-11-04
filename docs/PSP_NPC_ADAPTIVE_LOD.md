# Adaptive NPC detail and visibility

The user's device did not show a measurable FPS gain from the earlier CPU
optimizations. Those changes should not be described as measured FPS improvements.

The all-visible NPC policy set CreatureRenderer::performanceScale to 1 so draw
range would remain intact. That same field also scaled the mesh LOD threshold,
so preserving visibility accidentally disabled adaptive NPC mesh detail.

NPC view range and mesh detail now have separate scales. All-visible mode keeps
its range/count policy; performanceLodScale follows the existing frame-budget
distance scale. For eligible NPCs with baked low-detail meshes, the default
18-yard LOD threshold becomes 9 yards at scale 0.5 and 5.4 yards at maximum
pressure (0.3). Targeted/important units retain the full threshold. Disabling
adaptive mode restores scale 1. Spell and rain density are unaffected. NPCs
without a baked lower LOD continue using their existing mesh.

The existing NPC priority/visibility host test passes and diff checks are clean.
This checks preservation of the draw-policy rules, not a hardware speedup. The
new npc_detail log reports the applied threshold; npc_frame already reports
LOD counts, triangles and draw batches for comparison from the same viewpoint.
An EBOOT rebuild is required; no new assets are needed to use existing LODs.
Actual FPS benefit is unverified and cannot establish a constant 30 FPS.
