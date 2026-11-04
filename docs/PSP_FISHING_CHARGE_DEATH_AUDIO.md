# Fishing bite/splash, Charge, weapon errors and death audio

## Fishing correction

The previous dip fallback returned zero solely because the bobber had a Custom0 animation sequence. Existence of a sequence does not prove it moves visibly in the PSP renderer. The owned bobber now always uses an explicit server-bite-driven render offset; its Custom0 pose is not additionally layered over that offset. Static and animated object paths and the line endpoint use the same dip. The server bite handler also binds the bite timestamp to its bobber identity, preventing the next object-use update from clearing a just-arrived bite.

Landing (server-created bobber) and server bite now draw a short, depth-tested splash/ripple cue after the water surface. The procedural PSP cue has two expanding rings and eight droplets, lasts less than 0.85 seconds and uses at most 240 vertices (3,840 bytes in the GE list), with an 8 KiB free-list guard. It requires no texture, new pack, per-frame heap allocation or rendered fishing pole. It is an approximation rather than a claim of original-client particle/animation parity. The owned bobber's network coordinates, collision and click range remain unchanged. Previously supplied fishing cast/idle and line fixes remain.

## Charge/rush movement

The PSP intentionally excludes its own player from the remote entity manager. The previous rush path asked that manager for the local player and returned before starting movement. A dedicated, lazily allocated Entity now tracks the local server spline, advances in updateEntities and supplies its position/end point to movement. It is not inserted into NPC/player rendering lists. The world animator explicitly uses its Charge/run state during the rush and releases it on completion/cancellation. Taxi and teleport/vehicle separation remain. The host regression now starts with NO local player in the remote manager and drives the production updateEntities function; its previous fixture incorrectly pre-populated that player.

## Combat text and failures

Absorb entries are excluded from floating combat text. Combat-log data, damage and absorb calculations remain intact. Equipment-class failure codes 28-31 now use localized complete messages in both cast-failure paths. Heroic Strike with a fishing pole is still rejected by the server; raw `%s` templates are no longer shown for these failures. This fix does not infer a specific weapon subclass from missing packet metadata.

## Death voice and ghost ambience

The world sound profile now resolves its existing authored race/sex death entry (with original character-path fallback) and plays it once on a live-to-dead transition. Logging in already dead does not trigger a new death cry.

While dead/ghost, normal zone music and zone/weather ambience are replaced by the original extracted client `sound/ambience/ghoststate.wav`, transcoded to PSP MPEG-1 Layer III. The existing music decoder streams the full track rather than allocating its 48 seconds as resident PCM. It uses Ambient volume while dead, restores Music volume and normal authored zone selection on resurrection, clears pending zone-track requests at transitions, and retries a missing file with a five-second backoff. Death-to-ghost does not restart the ambience. This does not implement the original client's complete DSP/filter pipeline or claim sample-identical MP3/gapless playback.

Prepared file: `outputs/Death-audio-fix/data/music/ambience/ghoststate.mp3` (768,545 bytes). All 1,842 MPEG-1 Layer III frames were checked, 44.1 kHz, approximately 48.118 seconds including codec padding. The source WAV is 48.065 seconds. `tools/psp/encode_ghost_mp3_windows.cpp` reproduces it with Windows Media Foundation; `tools/psp/rebuild_ghost_ambience.sh` can regenerate it using ffmpeg from an extracted client. The media file is kept outside source-review archives.

## Install

1. Rebuild EBOOT in WSL:

```bash
bash tools/psp/build_psp.sh
```

2. Copy the rebuilt `psp/bin/EBOOT.PBP` to the game directory.
3. Merge the `data` folder in `Death-audio-fix.zip` into the game's existing `data` folder. The new file must be `data/music/ambience/ghoststate.mp3`.

No sounds.pak or audio.pak rebake is needed with the current packs. Existing asset updates from earlier fixes still apply. The ghost ambience file is new and must be copied along with the EBOOT.

## Checks

The full host regression suite passed, including production bobber draw routing with an inert clip, splash expiry and bounded geometry; source-owned fishing lookup; actual local spline update with the player absent from remote entities; alive/dead/ghost/resurrection audio transitions, pending-track cancellation, ambient volume, missing-file backoff and death-voice playback. PSP SDK header syntax checks passed across the changed world/session/audio files. The prepared MP3 was encoded successfully using Windows Media Foundation and frame-validated. No PSP EBOOT was built here; real hardware gameplay, audio and visual verification remain necessary.
