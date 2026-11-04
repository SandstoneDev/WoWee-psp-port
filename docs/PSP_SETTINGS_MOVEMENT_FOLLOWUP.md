# PSP settings, movement and HUD follow-up

The settings categories are Graphics, Sound, Controls, Camera, Interface, Network and Languages. Graphics contains all former Graphics, Advanced and Effects rows. Its list scrolls to keep the D-pad selection visible; no existing setting is duplicated or dropped. The category and settings panels are both 197 pixels high at the native 480x272 layout. Help text occupies the unused lower part of the category panel.

Settings uses the client UI-DialogBox-Header texture with the title Settings. Category selection uses OptionsListButtonTemplate's UI-QuestLogTitleHighlight and authored tint. Dropdowns use UIDropDownMenuTemplate's CharacterCreate-LabelFrame, arrow button, list background, highlight and checkmark. Cross opens a list, Up/Down selects, Cross commits, and Circle/Triangle cancels; Left/Right can still adjust the parent setting directly. Booleans use the client's checkbox art. Sliders use the background, border and horizontal thumb from OptionsSliderTemplate, scaled to the PSP layout.

The updated local Kiln UI exporter includes three settings-slider texture entries. A ready complete UI pack is under outputs/settings-ui/data/ui.pak. It preserves all 674 entries of the existing local UI pack and adds the three slider entries (677 total). The code can fall back with older packs, but the new slider artwork requires the updated pack. The preview images are software layout previews using client assets, not captured PSP frames.

Bags and their bag buttons appear only while the Bags window is open. Opening Bags from Start always anchors its contents to the bottom right; closing one container restacks the others there. Closing the last one hides the bar and returns focus to another open window or gameplay. The bag buttons no longer occupy the normal gameplay HUD or the Start menu. Duplicate bag-icon loads are avoided.

NPC names and quest !/? markers share camera-to-anchor wall visibility checks. The fixed 32-entry cache covers the existing 12-name and 16-marker limits, shares matching checks and invalidates after 100 ms or movement. Building, big-WMO and transport walls are checked. These are whole-label/marker visibility checks, not per-pixel depth clipping.

Local-player non-taxi SMSG_MONSTER_MOVE packets now feed the same entity interpolation used for remote units, instead of being discarded by the taxi-only handler. Server start, waypoints, duration, facing and endpoint drive the player; stick input and local gravity cannot hold a Charge-style rush in place. The exact endpoint is published once before returning controls. Explicit stops and teleports cancel motion; taxi splines retain their existing path. This fixes the missing local server-spline path, not every separately encoded knockback/teleport ability.

Adaptive quality now recovers a level after three seconds of sustained work headroom, with thresholds scaled to the 30/60 FPS target. Ordinary 26 ms work at 30 FPS can recover; isolated spikes reduce accumulated recovery rather than resetting it completely. Overload still reduces quality promptly. The controller reapplies distance, terrain, cosmetic, animation and player-cap settings as levels recover. Spell and rain quality remain protected. Memory-governor cuts still require memory headroom before restoration to protect against OOM.

Validation: host tests passed for all category membership, traversal of all 19 Graphics settings, bounded scrolling, dropdown commit/cancel/focus, bag closing, window management, marker cache, Charge interpolation/waypoints/endpoints/stops/taxis/teleports, and 30/60 FPS adaptive recovery. SDK-header host syntax checks passed for the changed UI and movement translation units. PSP cross-build and hardware/server validation remain necessary; exact hardware FPS and all-ability behaviour are not claimed.

## Install

Rebuild the EBOOT in WSL:

```bash
bash tools/psp/build_psp.sh
```

Copy that EBOOT.PBP and outputs/settings-ui/data/ui.pak to the game folder (ui.pak goes inside its existing data/ folder). No music, audio, character or world-pack rebake is required for this follow-up. Ground decoration stays removed.


## Settings navigation and sustained cast sounds

Settings uses one repeat owner for both the category list and controls: a tap
moves once, holding repeats after 500 ms at 140 ms intervals without acceleration.
The focus ring receives these pulses rather than the raw held direction.

Precast and channel sounds use a tagged mixer loop at the original sample rate.
The existing PCM loop crossfade joins the short clip's end to its beginning.
SPELL_GO, cancellation, interruption, or cast completion stops that caster's
voice and any pending load. Release/impact sounds remain one-shots. Active PCM
remains protected from cache eviction. This reuses one decoded clip and voice;
it does not allocate a longer PCM copy or require a sounds.pak rebake.

START now contains Spellbook. SELECT still toggles interface focus, and the old
floating Spellbook shortcut is removed. The assignment picker shows all twelve
slots in action-bar order, grouped by face / L / R bindings, with incoming spell
and destination previews. Replacing an action still needs confirmation.


## Settings reference styling

The Settings-only visual refinement uses the reference's yellow-green selected-category glow, compact sidebar spacing, more translucent panel fills, a separate first section and a continuous lower options group with thin heading dividers. Both panels keep their matching height. Existing settings, native sliders/dropdowns, title tab, help wrapping, scrolling and hold-repeat controls are retained. The action picker is unchanged by this refinement. Rebuild EBOOT.PBP; no new asset pack is required.


## Graphics presets

Low, Medium, High, Ultra, Custom and Adaptive are now in the wide Graphics preset bar. Adaptive is the default and disables manual graphics controls except Target FPS. An actual manual graphics edit from a fixed preset switches to Custom. FPS and non-graphics changes stay independent. The former Adaptive toggle is now owned by the preset, with its legacy config value saved for compatibility. See PSP_GRAPHICS_PRESETS.md for exact levels, config migration and testing.


## Client-style settings organization

- Graphics: Display (FPS and brightness), Base settings (preset), Textures,
  Environment (object distance, fog, weather), Unit detail (player/NPC distance
  and counts), Effects (spell quality, level-up, particles, water).
- Sound: music, effects, ambient and weather volume.
- Controls: targeting, auto-loot and automatic AFK sitting, following the original
  client Controls panel's targeting/loot/AFK organization.
- Camera: automatic follow and old/smart camera behavior.
- Interface: UI navigation, action-bar assignment and FPS/ping display.
- Network: remote-player prediction.
- Languages: installed client language selection.

Graphics remains consolidated. All schema settings appear exactly once. The
seven-row sidebar leaves a separate bounded help area below it; long help text
still pages. Adaptive locks quality options on Graphics except FPS; the FPS
counter is an Interface setting and no longer changes/resets the quality preset.
The source reference was the locally extracted 3.3.5a FrameXML controls, camera
and action-bar panels, adapted to the PSP's available options.
