# NPC asset streaming

The gameplay NPC draw path now requests display records, skinned/static models,
textures and attachment points through NpcAssetLoader. Its worker owns a separate
PakIndex (or a read-only legacy Pak TOC). Both TOC lookups
and bulk reads happen on the worker; renderer state and graphics installation
remain on the main thread. Each worker runs at priority 50, below the main
thread (32), MP3 decoder (24), and effects loader (49), and holds four slots.
The index keeps its metadata but releases its startup descriptor. A worker
borrows its own raw descriptor for a batch, then closes it before publishing
the final result. Idle workers therefore pin no files. The main creature index
also releases its synchronous warm-up descriptor during asynchronous drawing.

Creature and equipment workers share a reservation counter so queued reads
cannot each spend the same heap headroom. Unknown small metadata records reserve
at most 64 KB; unknown bulk assets initially reserve up to 2 MB. Larger known
assets can retry with their actual size when heap headroom allows. Polling an
existing request or a full queue does not repeat a heap scan. Allocations/read
failures retry after 250 ms rather than marking an NPC permanently absent.
Completed unused results expire after two seconds. Renderer cache eviction rules
and the existing visibility/nearest-first policy remain in place.

Main-thread NPC installs retain npcloads' resource-count cap and add a soft
two-millisecond installation budget. A single parse cannot be preempted, but no
further install starts after the measured budget is exhausted. Resident NPCs
continue to draw/animate without consuming that budget. Multiple texture reads
can be queued while an earlier dependency is pending; incomplete textures are
never treated as ready, and pending animated skins never trigger static fallback.

Equipment metadata, models and both replacement/embedded textures are prewarmed
through the same worker class. ItemBank supplies a cache-only scope for the NPC
equipment path. A complete replacement is built separately; unexpected cold
dependencies defer without poisoning missing-model caches. The current equipment
is retained until the replacement succeeds, including under attachment-cap
pressure. The warm cache has 32 LRU entries with two-second expiry.

To minimize visible differences, the loading-screen warm-up keeps the previous
synchronous preload/equipment path. Explicit mount/player-transformation preloads
also retain their existing behavior. This change targets ordinary NPC asset reads
during gameplay; it does not claim that every asset system in the game is now
asynchronous. No new mesh-quality, draw-distance, animation-rate, spell or rain
reductions are introduced by this change. Newly encountered actors can still wait
briefly for their dependencies; nearby visible actors are requested first.

Map-cache flushes cancel old-generation results. Closing a renderer/bank joins
its worker before freeing lookup tables, buffers or descriptors. Workers cannot
invoke loading-screen pak-read hooks; those callbacks remain on the registering
thread to avoid touching UI/network state from a worker.

Configuration (wowee.cfg):

- npcasync=1, default: enable streaming. npcasync=0 restores the synchronous path
  for controlled comparisons. If a worker cannot start, startup logs enabled=0
  and the original path remains available.
- npcinstallus=2000, default, clamped to 500–5000: soft NPC installation budget.
- Existing npcloads and npcheapkb continue to apply to the creature path.

Diagnostics: npc_async / npc_equipment_async at startup and npc_async_frame
inside NPC profiling summaries record worker reads, deferred/read failures,
main-thread installs/time, and shared reservation KB. Compare the same crowded
view and route with the previous EBOOT, especially npc.load, npc.equip, frame
build time and worst frames. Background I/O targets loading stalls; it does not
remove the GPU cost of the same crowd or establish constant 30 FPS.

Validation completed:

- Production worker with simulated kernel/file APIs: background-only lookup and
  data reads, nonblocking polling, missing/empty records, retry/backoff, memory
  allowances, cross-worker reservations, cancellation, stale-result cleanup,
  legacy paks, startup failure, and joined shutdown during an active read. Every
  test buffer is tracked for correct ownership/freeing.
- Production NPC installation/ensure routines: pending skin/texture states,
  parallel texture dependencies, no static fallback while pending, existing NPC
  readiness with zero install budget, and true missing/malformed fallback.
- Production ItemBank staging functions: cache-only reads, absent versus pending
  results, models/textures/race-helm records, LRU expiry and buffer ownership.
- Production pak-read hook: workers never execute the main loading callback.
- Changed loader, pak, creature, attachment and equipment translation units pass
  host syntax checks against the PSPSDK declarations. The equipment unit also
  compiles in its desktop-converter compatibility mode.

These checks do not replace a full PSP cross-build or Vita performance test.
WSL cannot execute in this environment. Rebuild with tools/psp/build_psp.sh
--no-bump and replace EBOOT.PBP only. Asset packs do not need rebuilding.
