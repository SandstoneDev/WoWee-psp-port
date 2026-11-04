# Audio, menu and Ruins corrections

The supplied logs show file-descriptor exhaustion (80020320) after background
NPC loading was introduced. Two workers retained extra pack descriptors while
idle, and the main creature lookup also kept its synchronous descriptor. This
can prevent music, effects or other packs from opening. The logs also contain
successful music opens and SFX playback, so they do not demonstrate permanent
silence for the entire run.

NPC lookup indexes now release their startup descriptor. The worker opens its
own descriptor for a queued batch, uses a borrowed-descriptor index lookup,
and closes the file when the batch drains. It still joins before buffers/lookup
tables are destroyed. Main creature descriptors are releasable and returned
once asynchronous drawing starts. NPC workers run at priority 50, below the
effects loader (49), music decoder (24), and main thread (32). Music and effects
workers retry temporary descriptor exhaustion up to four opens with 10-ms waits
between attempts. Missing-file errors are not retried by this helper.

Spell kit metadata previously loaded only once, with the result ignored. A
temporary descriptor/allocation failure could therefore silence authored spell
sounds and lose authored cast animations for the whole login. Metadata loading
now retries transient failures after two seconds and caches successful results.
An absent/invalid kits record is logged explicitly and is not repeatedly read.
Existing old packs missing that record still need the appropriate asset data;
code cannot invent missing authored sounds. The new spell_kits loaded/unavailable
log distinguishes metadata recovery from missing data.

L, R and START are text labels even when button art exists; other controls retain
icons. START is handled globally before world UI/controls: it toggles the game
menu over panels, combat, settings or spell assignment. Opening it during a
logout countdown cancels that countdown. Other panels remain open, and the press
is consumed so it cannot also jump, cast or click behind the menu. System OSK
input remains owned by the system dialog.

Window dragging begins only on the title band, not when a modifier is pressed
over an inventory slot. The initial drag update ignores focus-induced cursor
warps. Bag control hints draw at a fixed screen position outside the movable
window bounds; their full-screen width no longer changes the bag's position
clamp. R plus stick still moves a window after grabbing its title.

The logout-position log identifies Undercity camera group 7, throne, under the
Ruins of Lordaeron label. Original WMO metadata marks it 0x2040: interior/portal
classification (0x2000) plus exterior lighting (0x40). The renderer previously
forced baked-only lighting for every interior group except courtyard01. It now
honors the exterior-lighting flag, so these roofed sections use zone lighting
while ordinary underground interiors retain baked illumination. Material unlit
flags and the existing courtyard correction remain in place. This is a runtime
change; it does not require a Kiln or WMO pack rebake.

Host regression tests cover worker descriptor return, open-failure recovery,
cancel/stop buffer ownership, audio-open retries, spell metadata recovery,
button labels, menu modality, title-only dragging, and exterior-lit interiors.
The modified core renderer/index/worker/UI-kit files passed host syntax checks
against PSP SDK headers. The window/bag/control/spell-UI files also passed after
isolating Windows-only macro collisions. The full world-screen host check is
blocked by the unavailable OpenSSL header; a full PSP cross-build and hardware
playback/visual confirmation remain required.

Rebuild EBOOT.PBP only to test these runtime corrections. Existing music and
sound packs can be reused; missing authored metadata/assets are reported rather
than repaired by this build.
