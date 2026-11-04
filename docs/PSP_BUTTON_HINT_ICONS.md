# PSP button hints

Inventory, spell assignment (including modifier rows and bar paging), settings,
vendor hints and autotest prompts use the existing ui.pak pad_* textures inline
with their localized action descriptions. The shared UiKit also prefers the
existing SELECT art in glyph displays. L, R and START use readable text as
requested; other buttons use icons with a text fallback for missing art.
Tutorials measure the same icon/text widths when wrapping.

The strings reuse the existing tutorial {cross}, {circle}, {l}, etc. notation.
UiKit::controlHint measures the mixed text/icon line for centered/right alignment
and uses short stack buffers rather than allocating text runs each frame.
Wide button art retains its aspect ratio. English and Russian hints share the
same button tokens; action descriptions remain localized. Input mappings are
unchanged (Circle use/wear, Cross move, Square delete, Triangle switch bags).

The latest supplied log reports ui_pad_glyphs n=14 of 14, so all existing button
textures are already installed. Rebuild EBOOT only; no asset rebake is needed.

tests/test_psp_control_hints.py runs the production glyph and hint methods with
recorded UI calls, checking button art use, modifier combinations, wide icons,
alignment, missing-art fallback and long UTF-8 text runs. Tests passed, and
ui_kit.cpp passed a host syntax check against the PSP SDK headers. A full PSP
cross-build and on-device visual confirmation remain required.
