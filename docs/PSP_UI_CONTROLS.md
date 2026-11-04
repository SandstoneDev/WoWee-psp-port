# D-pad UI navigation

The interface uses a native controller focus cursor inspired by ConsolePort's spatial interface cursor, implemented in C++ for this port. ConsolePort is not required and its addon code is not embedded.

- SELECT enters/leaves UI mode (existing behavior).
- D-pad selects neighboring controls inside the current panel. Hold to repeat after 320 ms, with faster repeat after 1.2 seconds.
- L + Left/Right brings another non-modal panel to the front and restores its selection. Modal decisions block panel switching.
- Cross activates the selected control. Circle cancels a held item/spell first, then closes the front window and returns to gameplay after the last window closes.
- On bag/equipment slots, Circle retains use/wear (and contextual item actions). Use the window's Close control to close bags without using an item.
- START opens the game menu over the current panel; closing it restores the previous panel/selection or settings.
- The stick still moves a free cursor. Small drift cannot immediately override a D-pad selection; D-pad takes priority when both inputs arrive together.
- Hold L and move the stick to reposition the active window; release L to save or Circle to cancel. The stick-icon Move control also works: D-pad/stick moves, Cross saves, Circle cancels. R does not start dragging; holding L with a neutral stick still compares item tooltips. Modal dialogs and the full-screen map stay fixed.
- Inline button icons and authored control highlights mark selection; no extra yellow outline or black backing under the X hint is drawn.

Opening a native panel gives it focus; reopening restores its previous content selection when available. New bags/character/spellbook/quest-log panels start at an item, equipment slot, spell or quest row. Inactive panels are slightly dimmed. Covered/inactive controls cannot activate. Close and Move controls reserve navigation slots even on crowded panels.

Action glyphs appear at the actual focused control. Settings page labels use readable L/R text, with navigation beside the selected row. Bag management retains a contextual two-line strip, drawn after windows and tooltips so it cannot disappear behind another panel. Its hints appear only while bags own input. Spell-assignment management retains its dedicated layout.

Dialogs use centered placement; panel placements remain screen-clamped. Informational tutorials and achievement/completion alerts wait behind active menus/settings. Required decisions remain modal. Closing a tutorial leaves remaining tips behind the alert instead of immediately opening another tip.

The shared FocusRing improves screens using it; custom FrameXML cursor implementations and existing per-window paging/scroll shortcuts retain their own behavior. Internal tab shortcuts are not standardized by this patch. This is not a complete ConsolePort feature clone.

Host tests cover navigation, window stack/Back/menu restoration, explicit movement, item cancellation, contextual hint placement and tutorial queues. See `PSP_WINDOW_MANAGEMENT.md` for validation and deployment. Real-device visual and input checks remain required after rebuilding EBOOT.PBP.

Reference: https://github.com/leoaviana/ConsolePortLK/blob/master/ConsolePort/Cursors/Interface.lua

Frame pacing work was deferred at the user's request; this patch does not change frame caps or adaptive quality.
