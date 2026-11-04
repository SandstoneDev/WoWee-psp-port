PSP controls, appearance, audio and performance update

Controls
- L + D-pad left/right switches six server action-bar pages. Stance page 1 and vehicle overrides retain their server-provided slots. The small label reads server page:four-button group. Existing face-button bindings work on the selected page.
- Spell assignment: L/R switches destination bar; Cross selects/confirms; Circle cancels.
- Bags: Square picks/places; L+Square splits half a stack; Triangle transfers to the next bag with a free slot. Empty/full destinations do not clear the held-item cursor. No local-only inventory sorting.
- START closes the top closable window before opening the menu. Circle on empty window space cancels a held item or closes the panel. SELECT returns to gameplay and clears temporary cursors.
- Settings pages: World, Display, Controls, Gameplay, Audio, Effects, Interface.

Appearance
Rebuild characters.pak. The converter adds facial/scalp overlays indexed by style and hair color plus special weapon attacks 57/58/118. New runtime reads the optional overlays; old packs remain readable. Melee strikes restart the same sequence, use its authored duration up to 2.5 seconds, and Heroic Strike uses a weapon special when available, falling back to a main-hand swing.
The own-player frame and player-owned panel portraits share a cached 64x64 render of the actual character. An additional 8 KiB of VRAM is reserved; refreshes slow down under overload.

Audio
Rebuild sounds.pak and every numbered part for authored AreaTable/SoundAmbience day/night zone loops and weather sounds. Runtime keeps at most two ambient loops; original ambience WAVs are converted to mono 11025 Hz, at most six seconds each (about 129 KiB per active loop). They share the existing SFX cache; ambientvol/weathervol (0..100) independently control them. Weather is muted indoors, zone ambience attenuated indoors, and parent-area rules supply subzone fallbacks. Missing client assets are skipped.
The real PSP audio feeders now alternate two 64-byte-aligned output buffers and write back cache before submission. A submitted buffer is never rewritten while the hardware can still use it. The effects cache checks active/snapshotted PCM before eviction. Mixer logs report voice count, late blocks, maximum mixing time and hardware output errors. The extra output buffers cost 8 KiB RAM. PSP/PPSSPP audio timing differs; host checks do not prove hardware playback is clean.

Rendering
The normal world sky/terrain/props/units passes run in all areas. The global room-culling optimization was removed after an Undercity regression; see PSP_UNDERCITY_RENDERING_FIX.md. The existing per-building chunk/frustum/portal renderer remains. Collision planning is independent of render visibility and speculative collision prefetch cannot consume the whole visual budget.
Sustained work above 65 ms for 0.25 s enables an emergency performance tier: two background NPC/player models per category, fewer poses, shorter distances, 100 yd far clip and no optional cosmetic particles. Player/target/mount handling stays prioritized. Capped-out remote models no longer receive floor and animation updates. This is mitigation for reported 9 FPS towns, not a guarantee of 30 FPS on every PSP.

Logout and tracker
Logout has a countdown and Cancel modal; only the server ends the session. A zero countdown waits for the server. Server rejection/cancellation dismisses the modal. Tracker overflow no longer adds a Quest Journal button.

XMB
The checked local EBOOT had zero-byte ICON0.PNG and PIC1.PNG sections. Builds now accept personal psp/xmb art or WOWEE_PSP_XMB=/path/to/Kiln-output/xmb and preserve art from the previous EBOOT. Kiln invalidates its XMB step when the executable changes, validates PNG dimensions, preserves executable/SFO sections and copies the repacked EBOOT to the game root for CLI as well as GUI installs.

Verification
Host C++ regression suites cover frame budget, reverse-stick camera, action assignment/page resolution, navigation (including modal free cursor), audio math, fog and collision selection. A test harness executes production feeder functions with 1/2/3/4/8/12 overlapping voices, checks sample limits and submitted-buffer lifetime, then checks music/movie and empty-ring silence. Host syntax checks use the PSP headers. Kiln Python tests include synthetic sound converter integration, compact settings, size report and XMB executable preservation/copying. Real PSP timing and visuals still need device verification; no new EBOOT is claimed to have been cross-compiled here.
