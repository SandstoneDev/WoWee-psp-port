# Effects and portrait correction

Adaptive frame budgeting no longer scales spell density (including basic spell sprite counts) or rain/weather density. Their explicit settings still apply; adaptive reductions to world distance, background models and optional world particles remain.

The previous portrait head anchor incorrectly used only the translation part of the attachment matrix. WATP stores a position separately; the correct anchor is standMatrix * position. Auditing the actual rebuilt pack changes the human male head from near z=-0.015 to z=2.000, and undead male from z=-0.289 to z=1.764. The anchor now uses that full transform with a plausible-height fallback.

Portrait rendering reports whether draw batches were submitted. Insufficient command-list space defers before clearing the target, and an empty draw cannot be marked as a completed cache entry. The customized static pose/framing remains. These are EBOOT changes only; existing corrected character/sound/music assets remain valid. Actual PSP visuals still need validation.
