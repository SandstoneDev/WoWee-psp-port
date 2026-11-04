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
- **NPCs and players:** creature and character models with animation and equipment; mounts; flying and swimming;
  a cached player portrait with your character's appearance and equipment, and a basic player shadow.
- **Gameplay:** combat and spells, targeting, loot, inventory and bags, vendors, trainers, talents, quests, mail,
  auction house, trade, groups and raids, battleground queue, dungeon finder, calendar, achievements, guild bank.
- **Interface:** drawn from your client's own art and strings (converted by Kiln), laid out for 480x272 and the
  PSP's buttons; D-pad navigation, switchable windows, item moving and destruction, minimap, world map, tutorials
  and cinematics. Settings are grouped by category, with button hints beside the relevant controls.
- **Audio:** zone music and ambience, weather, spell and combat sounds, and NPC greetings and farewells selected
  from your client's sound tables. Music files are separate from the audio packs.
- **Rendering:** a 30 or 60 fps target with adaptive detail on or off; nearest-first NPC preparation, lower-detail
  distant animation, terrain water and fog. Adaptive detail can restore world detail as frame time allows and keeps spell and rain settings intact.
- **Languages:** your client's locale (enUS and ruRU tested). With several locales converted, the language is
  chosen on the login screen or in the settings menu.
- **Login:** typed on the login screen with the PSP keyboard. The first successful login is saved outside the game
  folder (`ms0:/WOWEE/account.dat`, as a hash, never the password, sealed to your console), so a copied game
  folder never carries your account.
- **Safety on public servers:** every debug and automation aid works only when the connected server is a private
  address listed in `devhosts=`; no configuration key unlocks them on a public address.

Terrain water renders with masked surfaces, animated ripples and fog. Basic and Fancy spell effects use the converted
client kits and missiles; Basic omits precast/caster decorations and uses fewer sprites. Generated grass and ground
decoration are omitted on PSP. See [water and collision notes](docs/PSP_WATER_COLLISION.md),
[spell visual notes](docs/PSP_SPELL_EFFECTS.md) and [settings notes](docs/PSP_SETTINGS_AND_GRASS.md).

Known gaps: WMO-only liquids and distinct magma/slime materials; a few world-map views of dungeons show a placeholder.
Settings combines graphical options under Graphics and uses the client control art; see [settings and movement notes](docs/PSP_SETTINGS_MOVEMENT_FOLLOWUP.md).
Northshire wolf and duskbat size reports are still under investigation. Build 123 ran on hardware. The latest
streaming changes passed host regressions and PSP source syntax checks. Stormwind hearthstone landings, floor
collision and frame times still need a hardware retest. See [follow-up notes](docs/PSP_UI_WORLD_FOLLOWUP.md).

The frame target defaults to 30 fps; 60 fps can be selected separately from adaptive rendering. These are targets,
not guaranteed minimums: crowded towns and complex interiors can fall below them. The original PSP-3000 figures
(about 28 fps median and 41 MB of game heap) are earlier measurements, not a benchmark of this revision.

## Controls

The scheme has two modes that follow the character's weapon: **Travel** (weapons sheathed) and **Combat** (weapons
drawn). Entering combat switches to Combat; a few seconds after combat ends (`sheathdelay=`, default 5 s) the port
returns to Travel. D-pad Down switches by hand. The switch shows as the weapon animation and sound, crossed swords
in the player frame, button glyphs on the action bar and a short yellow line.

### Always

| Button | Action |
|---|---|
| Analog stick | move relative to the camera (slight tilt walks, full tilt runs) |
| R (hold) + stick | turn the camera; camera follow resumes while moving when enabled |
| R + Up / Down | zoom |
| Down | draw / sheathe weapons (switch mode) |
| Left / Right | previous / next target (Combat: enemies only) |
| Up | interact: talk, loot, open |
| L + R | auto-run on / off (outside interface mode) |
| L + Left / Right | previous / next action bar (outside interface mode) |
| START | open / close the game menu, including during combat and with windows open |
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
| START | game menu |

Settings categories are **Graphics, Sound, Controls, Camera, Interface, Network, and Languages**. Graphics keeps display, preset, texture, environment, unit-detail and effect options together.

Graphics presets are **Low, Medium, High, Ultra, Custom, and Adaptive**. Adaptive is the default and locks individual graphics controls except target FPS. Editing a graphics option in another preset selects Custom. FPS, audio and controls are preserved when selecting a preset.

### Windows (after SELECT)

Open the spellbook from **START -> Spellbook**. SELECT toggles interface focus.

| Button | Action |
|---|---|
| D-pad | select controls and items (Enhanced navigation) |
| ✕ | left click: select, pick up / put down an item |
| ○ | close the selected bag; elsewhere, right click or cancel as shown |
| L + ○ in bags | equip, use or sell the selected item |
| □ on a carried item | ask to destroy it; confirm in the dialog |
| □ on a spell / action button | take it to the cursor |
| △ in bags | switch bags |
| △ in loot | take all; a full inventory stops the batch and leaves the remaining items |
| L + □ on a stack | split the stack |
| L + △ on an item | dress-up preview |
| L + R | switch to the next open window |
| L + Left / Right | previous / next open window |
| L (hold) + stick | move a movable window; bags stay pinned bottom right |
| START | open / close the game menu; closing it returns to the previous window |
| SELECT | leave interface mode |

Opening Bags places its containers and bag buttons at the bottom right. Closing the last container hides them. Other window title buttons close, move,
reset and switch windows. A confirmation dialog keeps the controls until it
is answered. Button hints show the action for the selected item or control; L, R and START are written as text.

**Logout** returns to character selection; **Exit Game** leaves the port. The server decides whether logout is
immediate or delayed. A delayed logout shows the countdown and **Cancel**; Exit Game also offers **Exit Now**.

## Setup, step by step

You need about an hour of your own time and a few hours of the PC's (the conversion of a full client took about
4 hours on a desktop PC). Nothing here touches your client: Kiln only reads it.

### 1. Download

- **The port:** `WoWeePSP-<version>.zip` from this repository's Releases page. It holds the game folder
  `WOWEEPSP/` with `EBOOT.PBP`, `sdbg.prx` (writes a crash report if the game crashes), `Data/` (protocol tables)
  and `wowee.cfg.example`.
- **The converter:** `Kiln-<version>-win64.zip` from [Kiln's Releases page](https://github.com/SandstoneDev/Kiln).
  Unpack it anywhere; it needs no installation and brings its own Python. These source changes also need the
  matching updated Kiln bakers; an older Kiln executable does not gain the changes just by rebuilding the EBOOT.

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
- `data/music/` holds the separate MP3 files, with their category folders, for example
  `data/music/gluescreenmusic/wotlk_main_title.mp3`. Copy the whole folder; `audio.pak` alone is not the music.
- `sounds.pak` and any `sounds_N.pak` parts hold sound effects and voices. Keep all parts from the same bake.
- When updating an existing installation, merge the data folders. Do not replace the whole folder with only the
  changed files. Rebuilt `doodads_<map>.pak` files and their `bigwmo/` companions must come from the same bake.
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

In the world: **START** opens the game menu; **Settings** opens the settings. The categories on the left are
**Graphics**, **Advanced**, **Controls**, **Gameplay**, **Audio**, **Effects** and **Languages**. Up / Down picks a
category or row; Right or ✕ enters a category, Left / Right changes a value, □ returns to the category list, and
L / R changes category. **Close** or ○ saves to `wowee.cfg`. A language change needs a restart.

| Row | `wowee.cfg` key | Values (default) | What it does |
|---|---|---|---|
| Brightness | `brightness` | 50-150 % (100) | adjusts the world image, not the LCD backlight |
| Classic fog | `classicfog` | on / off (on) | uses the shorter, zone-coloured fog horizon |
| Model distance | `m2view` | 40-240 yd (160) | trees, buildings and props farther away are not drawn |
| Player distance | `playerview` | 30-200 yd (120) | other players farther away are not drawn |
| NPC distance | `npcview` | 20-150 yd (70) | creatures farther away are not drawn |
| Frame limit | `framecap` | 30 / 60 (30) | target frame rate; not a guaranteed minimum |
| Graphics preset | `graphicspreset` | Low / Medium / High / Ultra / Custom / Adaptive (Adaptive) | Adaptive adjusts detail and locks manual graphics options except target FPS |
| FPS and ping | `perfhud` | on / off (off) | frame rate and server ping next to the minimap; enables frame-cost logging |
| Players drawn | `playermax` | 5-50 (10) | nearest players drawn; adaptive rendering can reduce the crowd budget |
| NPC count | `npcmax` | 0-64 (24) | fixed-detail NPC budget; adaptive rendering prioritizes nearby visible NPCs |
| Ground | `terrainsharp` | AUTO / Sharp (AUTO) | ground texture filtering; AUTO is cheaper |
| Sharp ground distance | `sharpdist` | 30-240 yd (120) | how far the ground stays sharp (Sharp only) |
| Ground detail | `terraindetail` | on / off (on) | detail texture on the ground up close |
| Camera follow | `camfollow` | on / off (on) | follows behind the character while moving |
| Camera system | `camsmartfollow` | Old / Smart (Smart) | chooses the camera-follow system |
| Targets (Right) | `targetall` | Enemies only / All (Enemies only) | the targeting preference |
| UI navigation | `uinavigation` | Basic / Enhanced (Enhanced) | cursor or D-pad navigation |
| Spell assignment | `barassignment` | Cursor / Slot picker (Slot picker) | how spells are placed on the action bar |
| AFK: sit after | `afksit` | off-600 s (300) | the character sits down after this long without input |
| Auto Loot | `autoloot` | on / off (off) | takes items one at a time; stops on an inventory error |
| Player prediction | `remotepredict` | on / off (on) | smoother movement of other players over Wi-Fi |
| Music / Sound | `musicvol` / `sfxvol` | 0-100 % (30 / 100) | music and sound-effect volumes |
| Ambience / Weather sound | `ambientvol` / `weathervol` | 0-100 % (35 / 35) | zone ambience and weather volumes |
| Spells | `spellfx` | Off / You + pet / Everyone (You + pet) | whose spell effects are drawn |
| Spell quality | `spellfxreal` | Basic / Fancy (Fancy) | both use `spellfx.pak`; Basic keeps essential client effects, Fancy adds caster decorations |
| Level-up effects | `levelupfx` | on / off (on) | level-up visuals |
| Water | `water` | on / off (on) | terrain water surfaces |
| Particles | `particles` | on / off (on) | world particles |
| Weather | `weather` | on / off (on) | rain and other weather visuals |
| Language | `locale` | the converted languages | needs a restart |

In crowded places try the **Adaptive** preset with a **30 fps** target first. To adjust individual distances or crowd limits, choose **Custom** or a fixed preset. Adaptive does not turn down spell or rain density. Visible NPCs are prepared nearest first, with distant animation detail reduced before nearby
animation. See [follow-up notes](docs/PSP_UI_WORLD_FOLLOWUP.md) for the current testing limits.

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
| `framecap=` | `30` | `30` or `60`; independent of adaptive rendering |
| `graphicspreset=` | `5` | 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Custom, 5 Adaptive |
| `adaptive30=` | `1` | legacy compatibility value; now derived from the selected preset |
| `musiccontinuous=` | `0` | `0`: use the client zone-music pauses; `1`: continuous music |
| `playershadow=` | `1` | basic player contact shadow; `0` disables it |
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
- **Music is missing but effects play** - check Music volume and that the entire `data/music/` folder was copied,
  including `gluescreenmusic/`. Keep its relative paths; the music is not inside `audio.pak`.
- **NPC greetings are missing** - rebuild `sounds.pak` with the matching updated Kiln and copy every generated
  sound-pack part. Rebuilding the EBOOT alone does not add voice clips.
- **Grass is sparse or tree trunks are invisible** - check Ground clutter and Grass density, then rebuild the
  affected world's doodads and matching big-WMO companion packs with the updated Kiln.
- **Critical-hit text still uses the old font** - rebuild `ui.pak`; older packs use the normal combat font as a
  fallback. The player portrait and basic player shadow do not need an asset rebake.
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
bash tools/psp/build_psp.sh            # from WSL or Linux
```

The script builds a clean copy under `$HOME`, fails on any `stubs out of order`, and writes `EBOOT.PBP` and
`wowee_psp.debug.elf` to `psp/bin/`. `MEMSIZE 1` is set in `psp/CMakeLists.txt`. The start-up community screen
can be left out with `-DWOWEE_PSP_SPLASH=OFF`.

The matching Kiln source must also be rebuilt for converter changes. Rebuild `ui.pak` for the critical-hit font,
`sounds.pak` and its parts for NPC speech, and each affected world's doodads plus big-WMO companions for terrain and
opaque tree materials. Rebuild `characters.pak`, `creatures.pak` and `spellfx.pak` for the current authored spell
and loot animation changes. Music remains under `data/music/`. Ghost ambience uses a separately prepared
`data/music/ambience/ghoststate.mp3`. See [death audio setup](docs/PSP_FISHING_CHARGE_DEATH_AUDIO.md). See [follow-up notes](docs/PSP_UI_WORLD_FOLLOWUP.md) for
build and copy requirements. Asset changes and EBOOT changes are separate; keep their versions matched.

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
