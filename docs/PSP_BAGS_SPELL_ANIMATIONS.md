# Pinned bags, cast audio and client spell animations

## Bag controls

Bags are pinned to the bottom right, independently of saved window positions and other windows. Their bar appears only while Bags is open, inside the inventory window rather than the normal gameplay HUD.
Containers restack from that corner when one is closed; closing the last container also hides the bar.

- D-pad: select a slot/control.
- Cross: pick up or place/move an item.
- Circle: close the selected container. With an item already held, the first Circle cancels the move.
- L+Circle: use/equip/sell the selected item. Context in the character/bank windows otherwise stays as before.
- Square: destroy the selected carried item through the existing confirmation.
- Triangle: next equipped bag; L+Square: split half a stack; L+Triangle: dress-up preview.
- L+R or L+Left/Right: switch open windows. Bags remain pinned; other movable windows still use L+stick.

Closing the final bag returns focus to another open window, or to gameplay if no window remains. Reopen Bags from Start.

## Spell and loot changes

Precast/channel audio has per-caster cancellation tags, including queued audio awaiting loading. Completion,
interruption and channel end stop that phase without stopping another caster or cutting the release/impact
one-shot. This lifetime fix needs the new EBOOT; existing sound packs do not need rebuilding just for it.

Both Basic and Fancy use original converted client effects. Basic retains missiles, impacts and state effects
with a smaller sprite budget; Fancy adds precast/caster decoration. Missing assets produce no invented orb,
ring or burst. The converter no longer copies missile models into missing cast/impact slots.

Animation routing reads the client start/hold/release/channel IDs and respects explicit no-animation values.
Successful casts release; interrupted casts cancel. Weapon-family special animations are preserved. Player
spell animations outside the base set are converted into on-demand extras; creature conversion requests the
client spell animations within its existing budget. Kiln's spell probe and bake now use --all, including
non-class gathering/opening spells. Opening/gathering uses the client use/open pose. Loot windows drive the
Loot -> LootHold -> LootUp sequence.

## Build and copy

EBOOT, in WSL:

```bash
bash tools/psp/build_psp.sh
```

Assets, in MSYS2 UCRT64:

```bash
bash /c/Users/busch/Documents/Codex/2026-10-26/usin/outputs/rebuild_bags_spells_kiln.sh
```

Defaults: your client is C:/Games/ChromieCraft_3.3.5a; Kiln output is C:/Kiln-output. Build the matching local
Kiln source, not an old packaged executable. Copy the repo's psp/bin/EBOOT.PBP to the game root, and merge
C:/Kiln-output/data/characters.pak, creatures.pak and spellfx.pak into the existing data/ folder. Keep all
unrelated packs and data/music/. No audio.pak rebake is needed for these changes.

Older packs remain readable but cannot supply absent animation sequences or new start metadata. Both spell
quality modes now need spellfx.pak for visuals. Do not deploy partial diagnostic probe packs from outputs/checks/.

## Verification and limits

Host bag navigation and window tests pass, including selecting a container independently of the previously
active bag, preserving others and restoring bar focus. Audio tests check separate caster tags, release one-shots
and channel completion. Production FSM tests cover cast pushback, cancellation/release and the loot sequence.
The real spell-converter metadata audit matches client rows for weapon, fire, frost and gathering examples.
Human male/female vertex and attachment checks pass at the normal 30 Hz character bake rate. A deliberately
reduced 15 Hz diagnostic failed the accuracy tolerance; use the normal rate. No full PSP cross-build or hardware
visual validation was available here. The changes do not establish exact visual parity for every spell/ability;
source-model omissions and renderer limitations (including some channel/procedural effects) still apply.
