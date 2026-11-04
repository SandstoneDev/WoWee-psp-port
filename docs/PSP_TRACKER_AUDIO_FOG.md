# Tracker, audio, reverse-stick and low-distance fog update

The quest watch now follows the supplied vanilla screenshot: bright gold titles,
white indented objectives in name: count order, completed counts retained, no
expanded Objectives heading or backdrop, and compact inter-quest spacing. It
stays under the minimap and above the action bar on the 480x272 display. SELECT
exposes collapse and tracked-quest selection with D-pad/Cross; selecting a quest
opens its details. A +N Quest Log link exposes overflow. Oversized first quests
show a bounded excerpt instead of blanking the entire watch. Cached rows rebuild
twice per second, preserving the existing lightweight draw path.

Own footsteps require actual movement plus movement intent, ground contact and
a walking/running state. They do not come from stale run poses while idle or
from swimming/flying/mount states. Tagged pending footsteps are cancelled on
idle and their playing voices receive a short fade. Other nearby units still
have their own positional footsteps.

The SFX mixer now interpolates resampled PCM, fades clip starts/ends and stopped
footsteps, and soft-limits loud sums instead of abruptly hard-clipping. The MP3
decoder runs ahead of gameplay priority so long rendering work is less likely to
starve it. These address specific click/crackle sources; actual hardware audio
underruns still need measurement and cannot be ruled out here.

Sound-bank voice profiles are built from ChrRaces -> CreatureDisplayInfo ->
CreatureModelData/CreatureSoundData -> SoundEntries. This includes the previously
missing Blood Elf/Draenei attack and wound libraries. Own race/sex is taken from
live unit fields when available. Spell damage now triggers the race's wound
vocal too. Jump playback no longer includes a landing clip. A missing authored
jump clip is left silent rather than replaced with another race's voice.

Zone music now uses separate authored day/night pools, subzone inheritance and
ZoneMusic silence intervals. Typical supplied classic-zone intervals are three
to five minutes after a song finishes; some zones have longer intervals. This
silence is intentional. Day/night follows the advancing realm clock (day 06-18).
Zone changes fade the stream instead of making an abrupt cut. musiccontinuous=1
skips the gaps; default 0 follows the supplied data. These are this installation's
WotLK client data, not a claim of byte-exact 1.12 music sequencing.

Holding nearly straight down used to change the movement heading as follow-cam
rotated the camera, creating a feedback loop near 180 degrees. Smart mode now
keeps that reverse gesture's movement basis stable while the camera swings
behind. Release, sideways steering, autorun, manual orbit and Old mode release
this special case; other steering remains camera-relative.

Graphics > Classic low fog toggles a 177-yard low-distance horizon (Off restores
450 yards). Fog uses the same projection limit, authored zone colors and density,
retaining naturally denser zones. This is a classic low-distance preset fitted
to the PSP, not pixel-exact parity with another version's lighting assets.
Config: classicfog=1 (default), 0 Off.

Reference layouts used for the existing client data:
https://github.com/wowdev/WoWDBDefs/blob/master/definitions/ZoneMusic.dbd
https://github.com/wowdev/WoWDBDefs/blob/master/definitions/CreatureSoundData.dbd
Low-view-distance range reference:
https://github.com/pkuzic/benilla-everwood_graphics/blob/main/LIGHTING.md

Required rebuild: psp_audio_conv, psp_sound_conv and the PSP executable. The latest
launcher also rebuilds spellfx.pak to keep the earlier spell update in sync.
Copy EBOOT.PBP, sdbg.prx, data/audio.pak, data/music/ (complete folder),
data/spellfx.pak and the complete data/sounds*.pak set. Remove old numbered sound
parts absent from the new set. Preserve wowee.cfg and other assets.

Validation performed here: six host suites compiled and passed (frame budget,
camera with reverse-input regression, assignment, navigation, audio PCM math,
fog policy). Client data audit checked all 20 playable race/sex attack/wound/critical profiles
(212 available WAVs) and music intervals. Of 630 unique music MP3s referenced by
the client tables, 625 are present; the converter skips the five absent files.
No PSP cross-build, emulator playback, or real hardware measurement was completed.
