# WoWee PSP port

**Need help? Join the Sandstone_PSP Discord: [discord.gg/wV8x46ZTDu](https://discord.gg/wV8x46ZTDu)**

A port of the [WoWee](https://github.com/Kelsidavis/WoWee) game client to the Sony PSP-2000 and PSP-3000.
It works with **World of Warcraft 3.3.5a (build 12340)**, which you must own: this repository contains no game
data. You convert your own client on a PC with **[Kiln](https://github.com/SandstoneDev/Kiln)**.

> Not affiliated with or endorsed by Blizzard Entertainment or Sony Interactive Entertainment. World of Warcraft,
> PSP and PlayStation are trademarks of their owners and are named here only to say what the port works with.

## Requirements

- **PSP-2000 or PSP-3000** in 64 MB mode (`MEMSIZE=1` in `PARAM.SFO`). Only the PSP-3000 has been tested. The
  PSP-1000 (32 MB) is not supported.
- A PSP that runs homebrew, with the extra RAM of these models enabled for homebrew. This project does not
  provide, link to or explain such firmware.
- Your own **World of Warcraft 3.3.5a (12340)** client on a PC, converted with Kiln (enUS and ruRU are tested).
- A memory stick with room for the converted data (about 17 GB for the full set). The stick is FAT32: no single
  file may exceed 4 GB.
- Wi-Fi and a 3.3.5a server you are allowed to connect to (see [Servers](#servers)).

## Status

Work in progress; playable on a private AzerothCore server.

- **World:** streamed terrain, doodads and WMOs for all four continents, the dungeon and raid maps, map transfers
  with loading screens, transports (ships, zeppelins, lifts), sky and lighting, weather and particles.
- **NPCs and players:** creature and character models with animation and equipment; mounts; flying and swimming.
- **Gameplay:** combat and spells, targeting, loot, inventory and bags, vendors, trainers, talents, quests, mail,
  auction house, trade, groups and raids, battleground queue, dungeon finder, calendar, achievements, guild bank.
- **Interface:** drawn from your client's own art and strings (converted by Kiln), laid out for 480x272 and the
  PSP's buttons; minimap, world map, tutorials, cinematics.
- **Languages:** your client's locale (enUS and ruRU tested). With several locales converted, the language is
  chosen on the login screen or in the settings menu.
- **Login:** typed on the login screen with the PSP keyboard. The first successful login is saved outside the game
  folder (`ms0:/WOWEE/account.dat`, as a hash, never the password, sealed to your console), so a copied game
  folder never carries your account.
- **Safety on public servers:** every debug and automation aid works only when the connected server is a private
  address listed in `devhosts=`; no configuration key unlocks them on a public address.

Known gaps: no water rendering yet; a few world-map views of dungeons show a placeholder.

On a PSP-3000 the port runs at about 28 fps (median, loading included) with about 41 MB of heap for the game.

## Controls

The scheme has two modes that follow the character's weapon: **Travel** (weapons sheathed) and **Combat** (weapons
drawn). Entering combat switches to Combat; a few seconds after combat ends (`sheathdelay=`, default 5 s) the port
returns to Travel. D-pad Down switches by hand. The switch shows as the weapon animation and sound, crossed swords
in the player frame, button glyphs on the action bar and a short yellow line.

### Always

| Button | Action |
|---|---|
| Analog stick | move relative to the camera (slight tilt walks, full tilt runs) |
| R (hold) + stick | turn the camera; release and it returns behind the character |
| R + Up / Down | zoom |
| Down | draw / sheathe weapons (switch mode) |
| Left / Right | previous / next target (Combat: enemies only) |
| Up | interact: talk, loot, open |
| L + R | auto-run on / off |
| SELECT | cursor and windows (interface mode) |
| HOME | quit (or Menu -> Exit) |

### Travel (sheathed)

| Button | Action |
|---|---|
| ✕ | jump |
| ○ | sit / stand |
| □ | clear target |
| △ | mount |
| L + □ △ ○ ✕ | action buttons 1-4 |
| START | game menu |

### Combat (drawn)

| Button | Action |
|---|---|
| □ △ ○ ✕ | action buttons 1-4 |
| L + □ △ ○ ✕ | action buttons 5-8 |
| R + □ △ ○ ✕ | action buttons 9-12 |
| START | jump |

### Windows (after SELECT)

| Button | Action |
|---|---|
| ✕ | left click: press, pick up / put down an item |
| ○ | right click: equip, use, sell / buy |
| □ | pick up / put down an item; on a spell, take it to the cursor |
| START | close the top window (no window: the game menu) |
| R + stick | move a window |
| L (hold) + △ on an item | dress-up preview |
| SELECT | leave interface mode |

## Game data (Kiln)

[Kiln](https://github.com/SandstoneDev/Kiln) checks your client's version, extracts what the port needs from it,
converts it into PSP packs and copies them to the memory stick. Everything happens on your PC; the output is for
your own use only - do not share it.

## Installing on the memory stick

```
ms0:/PSP/GAME/WOWEEPSP/
  EBOOT.PBP                 from a release (or built from this repository)
  sdbg.prx                  from the same release (crash reports)
  Data/                     protocol tables from the release (Data/expansions, Data/opcodes)
  wowee.cfg                 your settings (plain text; start from wowee.cfg.example)
  data/                     Kiln output: *.pak, bigwmo/, movies/, locale/<xxYY>/, locales.txt
  data/client/Wow.exe       optional: your own client's Wow.exe (integrity hash, Warden memory checks)
```

`wowee.cfg` is `key=value`, `#` starts a comment. The basics:

| Key | Meaning |
|---|---|
| `server=` | realm list host (IP address or name) |
| `port=` | realm list port, default `3724` |
| `netprofile=` | the PSP's Wi-Fi profile number; `0` (default) picks one automatically |
| `locale=` | language, e.g. `enUS`; normally set from the in-game language menu |
| `framecap=` | `60` (default) or `30` |
| `devhosts=` | private test servers that unlock the debug aids (public addresses are ignored) |

The account and password are typed on the login screen (they are not kept in `wowee.cfg`). Never share
`session.log` or the `data/` folder.

## Servers

The port is a compatibility client: it aims to behave like the original 3.3.5a client and gives no gameplay
advantage. It has been tested only on a private AzerothCore server.

- **Ask before you connect.** Many community servers forbid third-party or modified clients and ban them
  automatically. Read the server's rules and ask its staff first; you alone are responsible for your account.
- Debug and automation features work only on private development hosts (see `devhosts=`).
- Do not log in with an account you are not willing to lose.

## Building

The EBOOT is built with [pspdev](https://github.com/pspdev/pspdev) (GCC 15.2) under Linux or WSL.

```bash
bash tools/psp/cloud_setup.sh          # once, on a Linux box without pspdev: installs it into ~/pspdev
bash tools/psp/build_psp.sh --no-bump  # from Git Bash (re-runs itself in WSL) or from Linux
```

The script builds a clean copy under `$HOME`, fails on any `stubs out of order`, and writes `EBOOT.PBP` and
`wowee_psp.debug.elf` to `psp/bin/`. `MEMSIZE 1` is set in `psp/CMakeLists.txt`. The start-up community screen
can be left out with `-DWOWEE_PSP_SPLASH=OFF`.

## Credits and licence

- **WoWee** by [Kelsi Davis](https://github.com/Kelsidavis) and the WoWee contributors: the C++ client this port
  is based on.
- PSP port by **SandstoneDev**.
- Source code: MIT with an additional restriction - no use as the basis or a component of a commercial game
  product without written permission ([LICENSE](LICENSE), [NOTICE](NOTICE)). The port is free and non-commercial.
- WoWee's original music and audio are copyright Kelsi Davis, all rights reserved, and not covered by the MIT
  terms.
- Libraries under `extern/` keep their own licences.
- No game data, game code or firmware is included or linked. Do not open issues asking where to get the game,
  firmware or converted data, and do not share converted data or builds made from it.
