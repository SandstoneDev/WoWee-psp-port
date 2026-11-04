# PSP UI, world assets and NPC follow-up

## Implemented

- Window switching: L+R cycles eligible open windows; L+Left/Right and title arrows also switch. Cycling uses a stable order and preserves selected inventory slots/held items. Modal popups retain focus.
- Full-inventory looting: send one pickup at a time, stop the batch on inventory error or timeout, and leave remaining loot available for a manual retry. Up/Down navigate; Triangle takes all. A delayed release from another corpse cannot dismiss the current loot window. Disconnect clears pending requests.
- Basic player contact shadow: a small depth-tested, soft radial fan; no shadow textures or shadow map. It fades with jump height and is disabled while swimming. Set `playershadow=0` in the config to disable it. Flat geometry does not conform to steep slopes.
- Damage text: normal and critical hits use the client FRIZQT font. The new critical font is optional and falls back to the normal combat font with an older UI pack.
- Quest markers: use the existing client quest icons instead of font punctuation. These are camera-facing UI sprites, not rotating 3D quest-marker models.
- NPC greetings/farewells: select the display's NPCSounds personality and SoundEntries clips. Hostile units do not greet. Kiln's missing-file probe now includes these selected voice clips.
- Tree bark: opaque foliage materials ignore atlas/specular alpha unless the texture passes the existing silhouette test. Leaf and grass cutouts are preserved. The world-pack stamp changes to invalidate incompatible companion packs.
- Adaptive fog: start fog later within the same fog band; retain the existing fog endpoint, draw distance and streaming limits. No measured frame-rate claim is made.
- Earlier portrait and settings-sidebar changes remain included. Ground decoration was subsequently removed; see PSP_BRIDGE_MEMORY.md.

## Northshire wolves / duskbats: diagnosis still open

The extracted WotLK data assigns Young Wolf display 31049 a scale of 0.40 and display 31048 a scale of 0.55; both use the same Wolf model with model scale 1.0. Duskbat display 1954 uses FelBat at 0.60. The converter verification passed for those displays, including animated vertex and attachment checks. This does not establish which display or server scale the active realm sends.

No arbitrary wolf/bat enlargement is applied. Bounded `npc_scale` session lines now record display, server scale, baked client scale and effective scale after assets load. Visit Northshire and the affected duskbat area with the rebuilt EBOOT and collect those lines to resolve the remaining size report.

## Rebuild and copy

EBOOT (WSL):

```bash
bash tools/psp/build_psp.sh
```

The EBOOT is written to the port repository's `psp/bin/EBOOT.PBP`.

Asset rebuild (MSYS2 UCRT64):

```bash
bash /c/Users/busch/Documents/Codex/2026-10-26/usin/outputs/rebuild_gameplay_assets_kiln.sh
```

Defaults: client `C:/Games/ChromieCraft_3.3.5a`, output `C:/Kiln-output`, steps `ui,sounds,doodads_azeroth`. Supply a comma-separated third argument to select other world maps, e.g. `ui,sounds,doodads_azeroth,doodads_kalimdor,doodads_expansion01,doodads_northrend`.

Merge these into the existing game directory:

- `EBOOT.PBP` at the game root.
- `C:/Kiln-output/data/ui.pak` into `data/` for the critical-hit font.
- `C:/Kiln-output/data/sounds.pak` and every newly generated `sounds_N.pak` into `data/` for NPC speech. Remove only obsolete old `sounds_N.pak` parts after backing them up; do not remove other data packs.
- Each rebuilt `data/doodads_<map>.pak` and its matching `data/bigwmo/` companion files together, for bark changes. Never mix companion packs from different bakes.

This script builds sound effects in `sounds.pak`; it does not rebuild `audio.pak` or music. No diagnostic probe pack in `outputs/checks/` is a complete deployable pack. Keep existing data files by merging folders. The local Kiln source and exported Kiln source contain the converter changes; the existing standalone Kiln executable is not rebuilt here.

## Validation

- Host UI/asset runner: window switching and hints, portrait framing/cache, settings coverage, fog boundaries, loot request sequencing, legacy ground-decoration exclusion, tree alpha policy, real sound converter NPC voice fixture.
- SDK-header host syntax checks for changed gameplay/UI/rendering translation units.
- Native model and sound converters built successfully.
- Actual client tree-texture audit: 285 checked, 42 non-silhouette alpha masks identified for opaque repair, 19 silhouettes preserved. This is a texture-policy audit, not proof every audited texture is rendered by an opaque material.
- Wolf/bat probe: three display records verified, zero pak verification errors. The partial extracted-audio probe also verified with zero errors; missing clips still require a normal Kiln extraction/rebuild.

A PSP cross-build could not run in this environment because WSL access is denied. Real PSP/Vita testing remains required. Do not label the PR hardware-verified or promise constant 30/60 FPS.
