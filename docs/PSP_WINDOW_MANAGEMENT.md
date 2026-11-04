# PSP window management

This update implements the eight requested native-UI improvements: consistent
Back, one active panel, predictable placement, D-pad focus, explicit movement,
contextual hints, queued informational alerts, and START menu restoration.
Controls are documented in PSP_UI_CONTROLS.md.

## Implementation

- FocusRing retains content selection per group, chooses a deterministic first
  control, rejects inactive activation, waits for a covered panel to redeclare
  its remembered control and reserves two stops for Close/Move. Management
  buttons do not overwrite the saved item/row selection.
- The stack's front panel owns input, with inline button hints and the controls'
  existing highlights. No extra yellow outline is drawn. Other panels
  are dimmed by a light black overlay. Debug no longer draws above every newer
  window. Rendering keeps the newest 32 windows rather than hiding the latest.
- Circle is Back except when an inventory slot owns a contextual item action.
  Held items/spells cancel first; mandatory death dialogs remain protected.
  The loot shortcut cannot consume input from a panel behind another window.
- Holding L and moving the stick moves the active panel; releasing L saves.
  The stick-icon window action also starts movement; D-pad/stick moves,
  Cross commits to winxy config, Circle restores the original position. R does
  not start movement. START cancels movement before opening the menu.
- Native popups center on screen and ignore saved dragged positions. Existing
  panels retain their bounded layouts and deliberate saved positions. Bags keep
  their bottom-right authored anchors instead of automatic beside-window placement.
- Button glyphs attach to their controls. The bag strip is the management
  exception and draws after windows/tooltips; it changes when an item is held.
  Settings L/R hints sit beside page navigation, and D-pad/Cross hints beside
  the selected row/control. Existing L/R/START labels remain text.
- Tutorials, achievement toasts and dungeon-completion toasts defer while
  panels/settings/logout are visible. Their toast timers pause so messages do
  not expire unseen. Time-sensitive request/decision dialogs are not deferred.
- START preserves the previous window/content focus and can return to settings.
  Held items/spells and pending presses cannot leak into the menu.

## Validation and limits

Production-handler host tests: test_psp_window_menu.py,
test_psp_window_hints.py, test_psp_tutorial_dismiss.py,
test_psp_control_hints.py and test_psp_ui_navigation.cpp.
Syntax checks use the PSP SDK headers plus the existing host compatibility
stubs and the port's OpenSSL compatibility headers. There is no full EBOOT
cross-build or device visual verification here.

These are the native PSP UI paths. The optional FrameXML prototype still has
its own custom cursor/layout behavior; this does not introduce a desktop-style
Interface/AddOns loader or promise compatibility with PC WoW addons.

Rebuild and replace EBOOT.PBP only. The new UI strings have compiled fallbacks;
no UI, character, audio or other asset pack needs rebuilding for this update.
