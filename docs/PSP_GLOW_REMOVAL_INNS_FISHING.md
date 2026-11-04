# Glow removal, inn rest notifications and fishing visuals

## Full-screen glow removed

Removed the Full-Screen Glow setting, preset writes, adaptive glow helper and tests, world glow controls, quarter-resolution GE copy/upsample pass and its 18 KiB VRAM reservation. Existing `ffxglow`/`glow` config entries no longer activate any full-screen effect. Normal zone lighting, optional screen tint, fog, spell effects, portrait rest/combat highlights and weapon enchantments remain. Portrait/model preview buffers still reserve their own VRAM.

## Inn rest

The supplied hardware log shows Brill trigger 719 suppressed on the first post-load check at 598.340 s. That trigger was inserted into the active set, so it could not notify the server again until the character left/re-entered it. The later entry fired at 798.196 s and the server allowed instant logout at 802.909 s. This supports an arrival/login suppression bug; it does not establish every cause of missing resting in a newer session.

Known WotLK rest notifications now bypass portal cooldown/first-sample suppression. Ordinary portals and paired-return protection retain their previous behavior. Shapes, coordinates, client notification cadence and server validation are unchanged. The normal initial trigger scan delay still applies after world entry. The client does not invent resting, rested XP or instant logout: those still come from server flags/logout responses.

Rest trigger identities were checked against the AzerothCore WotLK tavern table, which identifies 719 as Gallows' End Tavern in Brill:
https://github.com/azerothcore/azerothcore-wotlk/blob/master/data/sql/base/db_world/areatrigger_tavern.sql

A selected PSP controller combat mode no longer hides the resting portrait state by itself. Actual combat/attacking still takes precedence. Rested XP bonus and its accumulation remain server-controlled.

## Fishing

Fishing ranks now select original character clips 133 (FishingCast), then 134 (FishingLoop), with no fabricated spell-release pose. The current local `characters.pak` already has both clips for all 20 race/sex combinations. Kiln explicitly includes these on-demand extras in future exports.

The fishing line is a small depth-tested 12-segment 3D strip. It runs from the current frame's main-hand item attachment 0 to the owned bobber, with slight sag. Older item packs without tip metadata use the far end of the pole's longest model axis. It is not drawn from a stale attachment when the pole wasn't rendered. No line texture or heap allocation is needed per frame.

The owned bobber now uses an explicit server-bite dip in both static and animated paths, plus a bounded landing/bite splash. Clip presence alone no longer disables the visual offset. See PSP_FISHING_CHARGE_DEATH_AUDIO.md for the later correction and its hardware verification limits.

Channel-object and create/type updates can arrive separately. Bobber lookup now accepts the channel object while actively fishing and can temporarily use the newest owned fishing node until the channel-object field arrives; it never selects another player's bobber or uses that fallback for an unrelated channel.

## Install / validation

Rebuild EBOOT only with the normal WSL build command:

```bash
bash tools/psp/build_psp.sh
```

Copy the rebuilt `psp/bin/EBOOT.PBP` to the game directory. No new asset pack is needed with the current local character packs. Existing asset fixes remain applicable. No PSP EBOOT was built in this host session.

Host checks cover rest-trigger suppression versus portal protection, portrait status priority, the production bobber ownership resolver, fishing cast-to-loop/cancellation, rank classification, server-bite timing, and line endpoints. PSP SDK header syntax checks cover settings, lighting, transfer, fishing integration, item attachments and model rendering. Hardware retesting in Brill and during fishing is still required; exact original-client visual parity is not claimed.
