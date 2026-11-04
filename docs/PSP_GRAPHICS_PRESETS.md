# Graphics presets

Settings > Graphics starts with the wide client-style Graphics preset slider and Target FPS.
Left/Right changes the selected preset; Cross cycles through the six values.
The current name is displayed in normal-sized (13 px) gold client-font text above the native
slider track, matching the reference quality-number placement. No tick marks
or six-name legend are drawn under the bar.
The preset bar stays a single focus target and does not open a dropdown.
The taller row is included in scrolling bounds so its labels remain visible.
Preset values in wowee.cfg are 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Custom,
5 Adaptive. Adaptive is the default. It locks individual graphics rows (including
brightness, effects and weather) while leaving preset and target FPS
available. Sound, Controls, Camera, Interface, Network and Languages remain independent.

| Preset | Object distance | Player distance / cap | NPC distance / cap | Ground detail | Spell visuals |
|---|---:|---:|---:|---|---|
| Low | 80 yd | 60 yd / 5 | 40 yd / 12 | Off; automatic filtering | Own/pet, Basic |
| Medium | 120 yd | 90 yd / 10 | 60 yd / 20 | On; automatic filtering | All, Basic |
| High | 160 yd | 120 yd / 15 | 80 yd / 32 | On; sharp to 120 yd | All, Fancy |
| Ultra | 240 yd | 200 yd / 30 | 150 yd / 64 | On; sharp to 240 yd | All, Fancy |
| Adaptive | 160 yd baseline | 120 yd / 10 baseline | 70 yd / 24 baseline | Managed by existing frame budget | All, Fancy; protected from adaptive reductions |

Low disables decorative water/particles. Other presets enable them. All presets
retain classic fog, rain and level-up effects, neutral brightness. The FPS/ping counter is an Interface preference preserved
by every preset. Adaptive uses the existing CPU/detail governor, including recovery
when load falls. It does not guarantee 30 or 60 FPS, and Ultra is a visual maximum
rather than a hardware performance recommendation.

Selecting Custom retains current values and disables adaptation. An actual
manual graphics change from a fixed preset selects Custom. Target FPS changes,
audio changes, gameplay changes and control changes do not select Custom.
Both dropdown commits and Left/Right edits use the same commit path. Disabled
rows cannot be activated. Preset changes leave target FPS and non-graphics
preferences untouched.

Named presets reapply their values on load. For hand-edited graphics config
values, set graphicspreset=4. Older configs without graphicspreset migrate to
Adaptive when adaptive30=1 (or absent), or to Custom preserving manual values
when adaptive30=0. The legacy adaptive30 key is synchronized on save for older
builds. Default reset returns to Adaptive. Live adaptive detail reductions are
not copied back over the configured baseline when opening Settings.

Validation: host preset/edit/locking/config migration tests, category and dropdown
routing tests, and SDK-header host syntax checks. PSP/Vita performance remains
to be tested. Rebuild EBOOT.PBP; no data pack rebake is required.

The START menu places Spellbook immediately after Character, before Talents.


Full-Screen Glow has been removed from the renderer and Graphics settings; presets do not enable it.
