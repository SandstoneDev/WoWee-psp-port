# Personal login background and official menu music

The supplied Icecrown/Sindragosa PNG is converted as a static 512x256 indexed PSP texture in optional data/login.pak. The login/realm backdrop uses it; race-specific character scenes remain. No image is compiled into source or included in source ZIPs.

The exact local 3.3.5a client's Interface/GlueXML/glueparent.lua sets CurrentGlueMusic="GS_LichKing" and calls PlayGlueMusic on login and non-movie/non-credit glue screens. accountlogin.lua starts the same kit on show. movieframe.lua stops glue music for movies, and the game's movie PCM path already stops streamed music. CharacterSelect_EnterWorld stops glue ambience and enters the world. The title track is shared across login/realm/character screens; it is not a new track restarted on every menu change. The in-game Escape menu retains zone music.

The builder resolves GS_LichKing through the actual SoundEntries.dbc row (12765 in this client): Sound/Music/GlueScreenMusic/WotLK_main_title.mp3. The MP3 remains streamed rather than decoded into a large RAM clip. The runtime starts it once, retries failed starts after two seconds, stops at world entry, and resumes when back in login/character selection or after a cinematic. Existing musicvol applies.

Build the personal pack with Kiln/bakers/tools/psp/bake_login_media.py --image <supplied PNG> --extracted <client extraction> --out <output/data/login.pak> --music-out <output/data/music>. Copy login.pak and the matching title MP3 tree alongside a newly built EBOOT. This implements title-music usage; race-specific GlueAmbienceTracks and the animated login scene are not added by this change. The supplied background is static as requested.
