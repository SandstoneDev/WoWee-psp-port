# New in-game settings

Open the game menu, then Graphics/Video (the existing settings entry). L/R changes pages; D-pad up/down selects, left/right adjusts, Cross cycles/toggles. Circle or Triangle closes and saves; the on-screen Close button also saves. Values apply immediately, and changed keys are saved on close. The pages retain 17-pixel rows instead of shrinking the entire list.

- Spell effects: Off, You + pet, Everyone (`spellfx`).
- Spell quality: Basic or Fancy (`spellfxreal`). Basic uses only built-in effects and does not open spellfx.pak. Fancy uses the baked models/kits with fallbacks. Quality is disabled when effects are off.
- Level-up effects (`levelupfx`): enables the level-up model/burst and sound while effects are on.
- Camera follow (`camfollow`) and Old/Smart camera (`camsmartfollow`). Smart uses the old camera smoothing after movement, following every moving frame without steering locks or direction-change pauses.
- UI navigation: Basic/Enhanced (`uinavigation`). Enhanced adds repeat, scoped panel navigation and gold focus outline. Basic retains safe click/modal handling but removes those enhancements in the world UI.
- Assign action buttons: Cursor/Slot picker (`barassignment`). Picker mode uses the new confirmation grid; Cursor mode preserves the old Square pickup/place flow and Cross-to-cast in the spellbook.
- Scene brightness (`brightness`): 50..150, 100 neutral. Software darkening/lightening affects the world, leaving menus/HUD untouched. This is not hardware backlight or a full gamma correction.
- Water rendering (`water`): hides/shows surfaces; swimming/gameplay collision remain unchanged.
- World particles (`particles`) and Weather effects (`weather`): independently reset/recreate their renderers, so off/on works without restarting.

Fancy assets still require the previously generated data/spellfx.pak. Switching renderer modes frees old renderer caches and unregisters the old spell event sink before replacement. The shared particle engine reserves configured spell/world slot shares while any consumer remains alive, allowing runtime off/on without reallocating another consumer's pool. An off renderer emits nothing, but its reserved share is not necessarily returned while another shared consumer is active.

This supersedes older config-only notes for these switches. Existing configs retain their stored values, and new keys use defaults. Display now offers an FPS target (30/60) and an independent Adaptive rendering toggle; see PSP_ADAPTIVE30.md for behavior and measurement limits.

Validation here: settings/key/label and page-layout audits, patch applicability, and whitespace checks. Compilation and in-game tests remain required; the launcher runs camera, navigation, and assignment host tests before compiling the PSP executable. No rebuilt EBOOT is supplied from this session.
