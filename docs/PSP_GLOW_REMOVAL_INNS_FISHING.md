# Glow removal, inn rest notifications and fishing visuals

## Full-screen glow removed

Removed the Full-Screen Glow setting, preset writes, adaptive glow helper and tests, world glow controls, quarter-resolution GE copy/upsample pass and its 18 KiB VRAM reservation. Existing `ffxglow`/`glow` config entries no longer activate any full-screen effect. Normal zone lighting, optional screen tint, fog, spell effects, portrait rest/combat highlights and weapon enchantments remain. Portrait/model preview buffers still reserve their own VRAM.

## Inn rest

The supplied hardware log shows Brill trigger 719 suppressed on the first post-load check at 598.340 s. That trigger was inserted into the active set, so it could not notify the server again until the character left/re-entered it. The later entry fired at 798.196 s and the server allowed instant logout at 802.909 s. This supports an arrival/login suppression bug; it does not establish every cause of missing resting in a newer session.

An earlier revision used a rest-trigger ID list derived from the AzerothCore tavern SQL. That list and its test fixtures were removed during review. First-sample and post-transfer portal suppression are back to the existing policy for every trigger. An inn occupied on arrival may still require leaving and re-entering before the client sends the trigger. The server owns resting, rested XP and instant logout.

An independent follow-up can derive candidate regions from the user's local client geometry and verify classifications against ordinary movement and server rest-state changes. Client trigger geometry alone cannot establish that a trigger is a tavern. A server-specific local override would need independently observed trigger IDs and should remain outside distributed data.

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
