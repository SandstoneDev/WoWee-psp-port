# Spell effects, casting and gathering

Both Basic (`spellfxreal=0`) and Fancy (`spellfxreal=1`) use the converted client effects in `data/spellfx.pak`.
There are no synthetic school-coloured balls, aura orbits, generic flashes or replacement missiles when assets are missing.
Missing effects stay absent; rebuild the appropriate packs rather than relying on an invented substitute.

- Basic retains client missiles, impacts and state effects, with a smaller sprite budget; precast/caster decorations are omitted.
- Fancy also draws the available precast/caster kits, within the existing cache, instance and sprite limits.
- `spellfx=0/1/2` selects Off / You + pet / Everyone. Sound and character poses remain active when visuals are off.
- A missile comes only from the client HasMissile/MissileModel data. Missing caster/impact kits no longer borrow the missile model and display it as a stationary orb.
- Client state effects remain tied to authoritative aura updates; no StateKit means no fabricated persistent glow.
- Existing level-up visuals use the client model, without a generic burst fallback.

## Animation selection

SpellVisualKit StartAnimID and AnimID are converted separately for precast, cast and channel phases. The optional
`kitstarts` record preserves compatibility with the existing 32-byte `kits` records. An authored no-animation value
is respected. A successful SPELL_GO plays the release; cancellation or failure clears the cast without pretending
the release occurred. The existing weapon-family special-attack selection remains in use for weapon abilities.

The character converter includes SpellVisualKit animations as on-demand `o_` extras instead of adding them all to
every resident base model. Player and remote character checks include these extras. Creature conversion requests
the client spell animations while retaining its model animation budget. A model can only play sequences present
in its source assets and retained by conversion; this is not a claim of visually verified parity for every spell.

For gathering/opening, the tested client rows select use/open poses (for example animation 123), rather than a
generic spell stance. Kiln's normal spell step now uses `--all` for both extraction probing and baking, so these
non-class spells are not omitted. Loot windows also drive Loot (50), LootHold (188), and LootUp (189).

## Cast audio

Precast/channel clips have bounded per-caster sound tags. SPELL_GO, cancellation, channel completion and loss of
cast state stop those tags, including deferred clips waiting for loading. Release and impact one-shots remain
independent, and stopping one caster does not stop another caster's sound. Replacing the renderer also cancels
its active tags.

## Rebuild

Rebuild the EBOOT and the matching Kiln converters. Rebuild and merge `characters.pak`, `creatures.pak` and
`spellfx.pak` into the existing `data/` folder. Older packs remain readable, but cannot supply omitted animations
or the new start-animation metadata. Existing spell sounds need no rebake just for the tag/lifetime fix.

Host regressions cover bag closing/focus, cast audio ownership, cast cancellation/release, and the loot sequence.
A real converter audit compares all metadata fields with the supplied client DBCs for fire, frost, weapon and
gathering examples. Human male/female conversion passes the existing vertex and attachment verification at the
default 30 Hz sampling rate. A deliberately reduced 15 Hz probe failed that accuracy tolerance; it is not the
recommended bake setting. These are source/converter checks, not complete visual verification on PSP/Vita.

Known renderer limits remain: arbitrary procedural kit effects, exact channel beams and every attachment/model
combination are not fully reproduced. Full all-race/all-creature conversion and hardware validation remain required.
