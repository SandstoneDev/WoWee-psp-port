# Static equipped player portrait

The player unit frame uses a cached render of the loaded, customized character in a fixed Stand pose, with the same body composite, hair/facial geosets and equipped attachments as the world model. Helm/cloak visibility settings remain effective. This is a close-up head-and-shoulders portrait: lower-body armor is outside the crop.

The camera anchors to the Stand head attachment (with a checked fallback), leaves crown/helmet margin, and uses a slight three-quarter angle. The player portrait fills the existing circular opening under the original frame artwork. It uses independent camera parameters and neutral studio lighting rather than world fog and lighting.

The render runs before world/crowd draws, only on first use or appearance revision. Character reloads and attachment refreshes invalidate it. A failed draw retries; switching character never reuses the previous face. Existing portraits-disabled configuration and fallback artwork remain available.

Validation: tests/test_psp_player_portrait.py checks head/shoulder framing over short/tall/hunched models, invalid bounds, cached frames, equipment revisions, failed draws and character switching. Host syntax checks with PSP SDK headers pass for ui_world_target.cpp, ui_world_screen.cpp and psp_character.cpp. No PSP/Vita visual verification or EBOOT build was possible in this environment.

Rebuild EBOOT with the normal build script. No Kiln asset rebake is required for this change; installed character/item packs must already contain the relevant customization and armor models. This change does not modify the separate optional lossless data project.
