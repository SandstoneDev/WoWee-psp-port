# October 30 follow-ups

## UI

The Classic interface is restored. The replacement PSP layout cannot be
enabled and the layout setting is removed. Old uilayout values are ignored.
The experimental full-screen inventory code and its fixed slot-plan arrays
were removed. Existing controller navigation, bag item actions, window
management, keyboard and achievement fixes remain.

## Fog in adaptive mode

The emergency adaptive tier reduces the actual camera far clip to 100 yards.
Previously zone fog was fitted to the preset's 177/450-yard projection and
could begin beyond that emergency clip. lightFogRange now fits the authored
range to the camera used for each draw pass. Object fading uses the same
effective range. This retains the wider fog gradient and the object-distance
and streamed-model reveals from PSP_FOG_TRANSITIONS.md.

## Ground clutter

The PSP distance setting was connected to existing detail models, but the
world converter only exported ADT's placed MDDF/MODF objects. Terrain-generated
grass was absent from its packs.

ground_clutter_bake.hpp now reads GroundEffectTexture/ GroundEffectDoodad and
the MCNK effect-layer mapping. It places actual biome-specific detail models
deterministically, up to eight per chunk, into the existing doodad pack format.
Terrain holes, no-effect masks, non-growing layers, water and steep slopes are
excluded. Zero density/weights and missing models do not trigger generic grass
substitution. Height follows the PSP terrain strip surface. Ground clutter Off
and its distance setting still control both draw paths.

A real Azeroth 31_27 conversion produced 1,839 clutter placements with no missing
detail models. That one-tile test used --bigwmo 0 and is a diagnostic only; do
not deploy its pack. Production grass requires a World/Doodads rebake and the
matching bigwmo companion files. The baker stamp now changes with this model-ID
allocation revision to reject mismatched old companions. Terrain packs do not
need replacing for grass. Use outputs/rebuild_grass_kiln.sh in MSYS2 UCRT64;
its defaults select C:/Games/ChromieCraft_3.3.5a, C:/Kiln-output and doodads_azeroth.

## Movement and casting audio

Ordinary take-off and landing no longer select jump/landing voice banks.
Surface contact sounds and swimming remain. The server's environmental-damage
packet now reaches combat text; positive fall damage triggers the existing
race/sex wound voice. Absorbed/resisted damage and other players' damage do not
produce an invented landing grunt. Class ability audio remains spell-driven.

Own precast/cast/channel audio plays without camera-distance attenuation.
Other spell sound entries use their authored full-volume radius and cutoff
instead of a one-yard radius. Nearby casting audio is no longer discarded by
the own-spells visual filter. Missing playback requests are logged.

## Brill Hallow's End

The server's flame helper (23686) is an effect anchor. Its nameplate and creature
mesh are hidden without deleting the entity or its auras. A separate bounded
effect renderer follows the nearest eight active fire helpers within 80 yards,
using their actual state kits and current scale. It works independently of the
own-spells/basic/off visual setting and obeys the world particles toggle.
Aura removal/extinguishing removes flames; leaving the area frees the renderer.
Refreshes retain running flames instead of restarting their animation.

Mapping was checked against the local client DBCs and the
[AzerothCore event script](https://github.com/azerothcore/azerothcore-wotlk/blob/master/src/server/scripts/Events/hallows_end.cpp).
The existing spellfx.pak contains these fire aura kits; no spellfx rebake is
required if that current pack is installed.

SMSG_PLAY_SOUND and SMSG_PLAY_OBJECT_SOUND now queue their authored SoundEntries
IDs to world playback. Previously the PSP session did not handle those packets,
so dialogue could appear in chat without its accompanying event audio. Queues
are bounded and cleared on session close. The sound baker includes the Horseman
voice directory; dialogue is mono 11025Hz, preserving lines up to 30 seconds
within a modest sound-cache footprint rather than trimming the 17-second line.

## Deployment and checks

Rebuild EBOOT.PBP for all runtime fixes. The historical build_psp_polish.sh now
forwards directly to the current source build, avoiding stale patch reapplication.
WSL returned Wsl/E_ACCESSDENIED in this session, so no new PSP EBOOT is claimed.

The rebuilt audio-event-update/data/sounds.pak is ready and was also copied into
the repository's psp/bin/data/sounds.pak. It contains all 1,987 prior local clips,
2,158 total clips, 1,666 entry rows and 5,744 surface grids. Converter verification
reported zero errors. Required Horseman entries and the full 17-second line were
checked. Music/audio.pak are unchanged.

Kiln's local source, exported source archive, manifests and model/sound converter
executables were updated. Full production world packs still need rebaking for grass.

Host tests cover 100 jumps without voice clips, surface contacts, cast routing,
authored attenuation, environmental damage and server sound packets, bounded
queues, fire ownership/lifecycle/nearest-eight budget, fog at the 100-yard
emergency clip, ground-effect masks/density/models/heights, audio file-budget
recovery and existing window/navigation behavior. PSP SDK-header host syntax
checks pass for the changed runtime files. PSP/Vita visual, audio and performance
verification remains required; no hardware FPS improvement is claimed.
