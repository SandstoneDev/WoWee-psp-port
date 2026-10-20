# WoWee PSP port

**English** | [Русский](README.ru.md)

**Need help? Join the Sandstone_PSP Discord: [discord.gg/wV8x46ZTDu](https://discord.gg/wV8x46ZTDu)**

**New here? Follow [Setup, step by step](#setup-step-by-step)** - from your PC client to the first login on the PSP.

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

## Setup, step by step

You need about an hour of your own time and a few hours of the PC's (the conversion of a full client took about
4 hours on a desktop PC). Nothing here touches your client: Kiln only reads it.

### 1. Download

- **The port:** `WoWeePSP-<version>.zip` from this repository's Releases page. It holds the game folder
  `WOWEEPSP/` with `EBOOT.PBP`, `sdbg.prx` (writes a crash report if the game crashes), `Data/` (protocol tables)
  and `wowee.cfg.example`.
- **The converter:** `Kiln-<version>-win64.zip` from [Kiln's Releases page](https://github.com/SandstoneDev/Kiln).
  Unpack it anywhere; it needs no installation and brings its own Python.

### 2. Convert your client with Kiln

1. Start `Kiln.exe`.
2. **1 Your game folder** - *Browse...* to the folder of your World of Warcraft 3.3.5a (12340) client: the one with `Wow.exe` and
   the `Data` folder. Kiln checks it at once and shows the build and the languages it found. A client from a
   private server's download is fine as long as it is 3.3.5a 12340.
3. **2 Extra languages** - optional. Leave it empty unless you have a second copy of the client in another
   language.
4. **3 Where to put it** - pick one:
   - **PSP memory stick** - connect the PSP by USB (*Settings > USB Connection* on the PSP) or put the stick into a
     card reader, then *Refresh*. Kiln makes everything on the PC first and copies it to the stick at the end.
   - **A folder on this PC** - you copy it to the stick yourself afterwards (see step 3 below).
5. *Advanced settings* - optional but recommended:
   - **Your EBOOT.PBP of the port** - point it at `EBOOT.PBP` from the port's zip. Kiln makes a copy with a menu
     icon and background taken from your own client (`xmb\EBOOT.PBP` in the output) and puts it on the stick.
   - **Server** - pre-filled with `logon.therawow.com`, a public 3.3.5a server, so the game works at once. To play
     elsewhere, type that server's realm list address (`host:port` if it is not 3724). Kiln writes it into
     `wowee.cfg`.
   - The work folder needs about 10 GB on a fast local disk; the data itself is about 17 GB.
6. Press **Start**. The *Steps* tab shows every step, the status line shows the time left. When it ends, the
   *Summary* tab shows the total time and where everything is. A cancelled or failed run resumes where it stopped
   when you press *Start* again.

### 3. Put it on the memory stick

If Kiln copied to the stick, only check the folder. Otherwise copy by hand. The stick must look like this:

```
ms0:/PSP/GAME/WOWEEPSP/
  EBOOT.PBP        the port's EBOOT - best the one with your client's art from Kiln's xmb\ folder
  sdbg.prx         from the port's zip
  wowee.cfg        your settings (Kiln writes it when you gave a server; else copy wowee.cfg.example)
  data/            Kiln's output folder data\ (about 17 GB, 1300+ files), together with the zip's Data/
```

- `data/client/` holds your client's executable, copied by Kiln. Keep it: servers with anti-cheat (Warden) check
  the client through it, and without it such a server stops answering (players show as `?`, NPC names as dots).
- `data` and the zip's `Data` are the same folder (the memory stick does not tell upper and lower case apart):
  their contents simply go together.
- The memory stick is FAT32: no file over 4 GB. Kiln already splits what would be bigger.
- Copying 17 GB over the PSP's USB takes 30-40 minutes. The PSP drops the USB connection when it goes to sleep:
  plug in the charger and set *Settings > Power Save Settings > Auto Sleep* to *Off* before you start. A card reader
  is faster. After an interrupted copy, copy again with "skip identical files", or let Kiln's *Check integrity* and
  *Repair* find and fix the missing or cut files.

### 4. Set the server

Kiln writes `server=logon.therawow.com` by default; skip this step to play there. For another server open
`wowee.cfg` (in `PSP/GAME/WOWEEPSP/` on the stick) with Notepad and change the one required line:

```
server=logon.therawow.com
```

Use the address the server's website gives for its realm list (`set realmlist ...` without the `set realmlist`).
If the port is not 3724, add `port=` on its own line. Everything else has defaults; see
[wowee.cfg reference](#wowee-cfg-reference).

### 5. First start

1. Start **WoWee PSP** from the PSP's *Game > Memory Stick* menu. Any button skips the start-up screen.
2. On the login screen select the account box and the password box: the PSP keyboard opens for each.
3. Select **Login**. The PSP's own **network dialog** opens: pick a Wi-Fi network that reaches the internet (it
   must be saved in *Settings > Network Settings* on the PSP first; WPA2 home networks work). Circle cancels.
4. The port connects, logs in and shows the realms and then your characters.

The first successful login is saved on the stick outside the game folder (`ms0:/WOWEE/account.dat`): a hash, never
the password, usable only on this PSP and only for that server. The next start logs in without typing. To use
another account, type it over the saved one; to forget it, delete `ms0:/WOWEE/account.dat`.

## Settings menu

In the world: **START** (in Travel mode) opens the game menu; its first button (*Video*) opens the settings. Up / Down picks
a row, Left / Right or ✕ changes it, the hint under the rows says what it does, **Close** saves to `wowee.cfg`.

| Row | `wowee.cfg` key | Values (default) | What it does |
|---|---|---|---|
| Model distance | `m2view` | 40-240 yd (160) | trees, buildings and props farther away are not drawn |
| Player distance | `playerview` | 30-200 yd (120) | other players farther away are not drawn |
| Players drawn | `playermax` | 5-50 (10) | only the nearest players are drawn; lower is smoother in cities |
| NPC distance | `npcview` | 20-150 yd (70) | creatures farther away are not drawn |
| NPC count | `npcmax` | 0-64 (24) | how many of the nearest creatures are drawn |
| Ground | `terrainsharp` | AUTO / Sharp (AUTO) | ground texture filtering; AUTO is cheaper |
| Sharp ground distance | `sharpdist` | 30-240 yd (120) | how far the ground stays sharp (Sharp only) |
| Ground detail | `terraindetail` | on / off (on) | detail texture on the ground up close |
| Frame limit | `framecap` | 30 / 60 (60) | 30 gives an even pace instead of jumping between 60 and 30 |
| Player prediction | `remotepredict` | on / off (on) | smoother movement of other players over Wi-Fi |
| FPS and ping | `perfhud` | on / off (off) | frames per second and the server ping next to the minimap |
| AFK: sit after | `afksit` | off-600 s (300) | the character sits down after this long without input |
| Targets (Right) | `targetall` | Enemies only / All | what Left / Right cycle through |
| Auto Loot | `autoloot` | on / off (off) | looting takes everything at once |
| Music / Sound | `musicvol` / `sfxvol` | 0-100 % (30 / 100) | volumes |
| Language | `locale` | the converted languages | needs a restart |

In crowded places (a capital, a starting zone of a busy server) lower **Players drawn** and **NPC count** first.

## wowee.cfg reference

`wowee.cfg` is plain text next to `EBOOT.PBP`: one `key=value` per line, `#` starts a comment. The settings menu
writes its own keys there; you only need to edit the file for the keys below.

| Key | Default | Meaning |
|---|---|---|
| `server=` | `logon.therawow.com` (Kiln; built-in `127.0.0.1`) | realm list host: an IP address or a host name |
| `port=` | `3724` | realm list port |
| `netdialog=` | `1` | `1`: the PSP's network dialog at login (pick the Wi-Fi network). `0`: connect to a saved network by itself (`netprofile=`) |
| `netprofile=` | `0` | with `netdialog=0`: the number of the PSP's saved network; `0` picks one automatically |
| `locale=` | from Kiln | language, e.g. `enUS`; normally set in the settings menu |
| `framecap=` | `60` | `60` or `30` |
| `perfhud=` | `0` | `1` shows fps and ping next to the minimap |
| `playermax=` | `10` | other players drawn (5-50) |
| `devhosts=` | `127.0.0.1,localhost` | private test servers that unlock the debug aids; public addresses are always ignored |

The account and password are never kept in `wowee.cfg`. Never share `session.log` (it names your server and
character) or the `data/` folder.

## Troubleshooting

- **"Wi-Fi: no connection was chosen"** - the network dialog was cancelled. **"No saved Wi-Fi network"** - save one
  in the PSP's *Settings > Network Settings* first. **"The WLAN switch is OFF"** - slide the switch on the PSP's
  side.
- **The server cannot be reached** - check `server=` and `port=` in `wowee.cfg` and that the chosen Wi-Fi reaches the
  internet.
- **The game closes while the world loads, or freezes in crowds** - the PSP is out of memory. Lower *Players drawn*
  and *NPC count*. Send `PSP/GAME/WOWEEPSP/session.log` and `CRASH.TXT` (if there is one) to the Discord.
- **Missing art, a black window or "pak" errors in `session.log`** - a file did not reach the stick whole. Run
  Kiln's *Check integrity*, then *Repair*.
- **The menu icon looks generic** - you used the EBOOT from the zip; use the one from Kiln's `xmb\` folder.

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
