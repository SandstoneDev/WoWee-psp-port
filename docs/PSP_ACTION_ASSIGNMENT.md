# Controller action-bar assignment

Open START -> Spellbook, then focus a spellbook spell and press Cross or Square. A dedicated twelve-slot picker opens above the other windows, avoiding the spellbook panel's focus boundary. Existing copies of the spell are selected first, otherwise the first empty slot is selected. Use the D-pad or free cursor to choose a slot, then Cross to assign. The twelve slots appear in one horizontal line, matching action-bar order. Labels divide them into default combat button groups (face buttons, L + face, R + face), with the actual face-button icons beneath each slot. The incoming spell has its own icon/name preview; the selected destination has a marker and its current action name. L/R switch between the six action bars.

The target's existing action is shown by icon and name. Replacing a populated slot requires a second Cross confirmation; selecting another slot clears confirmation. Circle or START cancels without changing any action. Closing UI mode, a replaced bar/vehicle state, or a new popup also cancels. The origin spell remains in the spellbook; no action is removed before confirmation. After completion/cancellation focus returns to that spell.

Circle on a spellbook entry still casts it. The older Square pickup/place flow for existing action-bar spells and items remains available. The new picker currently assigns spellbook spells; it does not replace the inventory's item drag flow.

The picker uses existing client UI and spell assets. Rebuild EBOOT.PBP; no spell, sound or world asset rebake is needed. Host navigation/assignment tests and SDK-header syntax checks passed. PSP/Vita playback and presentation still need hardware testing.

The picker now reuses Settings' client metal border and overlapping title tab, with an Action Bar caption. Preview, slot selection and destination have inset group panels; slots use the original UI-Quickslot / UI-Quickslot2 art instead of red menu-button faces. The existing popup textures stay resident while assignment is active. This changes drawing only; slot IDs, hit rectangles, controls, confirmation and cancellation remain the same.

Controller prompts retain the original coloured discs with transparent symbol cutouts. UiKit draws a one-pixel silhouette shadow instead of a rectangular backing; L, R and START stay text. The ready complete ui.pak contains the restored face-button artwork.
