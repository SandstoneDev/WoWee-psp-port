# Facial layers and ambience correction

The actual C:/Kiln-output/logs/characters.log showed human facialupperhair/faciallowerhair texture layers skipped as "no region". model_bake.hpp regionFor now recognizes those names as the existing upper/lower face atlas regions. Rebuild characters.pak with the updated converter; merely rebuilding the preceding version repeats the missing layers.

updateEnvironmentAudio was defined but never called. updateAudio now invokes it every frame after effect updates, before the music-availability early return. This enables authored area/day/night ambience independently of music availability. Zone ambience now respects the SoundEntries volume field rather than always playing at full requested volume.

Looping PCM now overlaps its final 100 ms (at most a quarter of a short clip) with the head and resumes after that overlap. This smooths the wrap even for previously rebuilt six-second packs. Newly converted ambience no longer gets a one-shot tail fade. These remain PSP-sized mono excerpts, not a complete reproduction of the PC soundscape or every positional emitter. Existing music track selection, authored silence ranges and track playback are unchanged.

Logout popup textures remain resident during the countdown, avoiding release/reload of six textures every frame. Undercity collision diagnostics now include terrain and doodad readiness. Hardware validation is still needed; no new device success is claimed.

The rebuilt stock client models carry racial eye glow in 1702; the old helper requested 1701 and only enabled night elves. Racial selection now enables 1702 for night elf, undead, blood elf and draenei, without enabling 1703 (death-knight glow). Human scalpupperhair/scalplowerhair aliases are mapped alongside facial overlays.

Player portrait framing now uses the baked Stand head attachment, a fixed front/quarter camera and a frozen Stand time of zero. The own-player portrait refreshes only when appearance/equipment dressing changes rather than four times a second. This avoids world-motion framing and recurring portrait bone evaluation.

Creature asset I/O failures are no longer permanently cached as absent displays or failed models/textures. PakIndex distinguishes failed TOC reads from genuine missing names; transient resource loads retry after one second. This addresses a code path consistent with missing NPCs after character-switch resource pressure, but the Tender-specific device reproduction has not been verified.

For an unresolved Undercity device block, collision_probe reason is 1 terrain, 2 tile doodads, 3 WMO collision. The probe is the actual rejected step rather than the current player position. movement_block mask reports M2 walls (1), WMO walls (2), transport walls (4), or no reachable floor (8). Keep session.log after reproducing at the arch.
