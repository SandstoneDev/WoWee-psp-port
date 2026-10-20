#include "ui/psp/ui_world_screen.hpp"
#include "platform/psp/psp_prof.hpp"

#include "core/character_paths.hpp"
#include "core/coordinates.hpp"
#include "game/update_field_table.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "platform/psp/psp_bigheap.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_audio.hpp"  // music volume from the settings
#include "platform/psp/psp_system.hpp"  // logMemory
#include "ui/psp/ui_login_screen.hpp"  // FrameStats
#include "ui/psp/ui_loading_screen.hpp"  // loadingStep (the loading bar)
#include "rendering/psp/psp_attach_crowd.hpp"  // attachcap's crowd queue with the NPCs

#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <malloc.h>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr float kTileSize = 533.33333f;
constexpr uint32_t kSkyABGR = 0xFFE0B080u;  // light sky blue (0xAABBGGRR)

struct MarkerVert {
    uint32_t color;
    float x, y, z;
};

} // namespace

// Was a table of the four continents (maps 0, 1, 530 and 571) - now every map of Map.dbc (its Directory,
// lower case: the paks' names), e.g. map 389 (an instance under a capital city). The continents stay built in
// for an older glue.pak.
std::string worldMapDir(uint32_t mapId) {
    pp::WorldSession::MapInfo m;
    return pp::WorldSession::mapInfo(mapId, m) ? m.dir : std::string();
}

WorldScreen::WorldScreen(const UiKit& kit, pp::WorldSession& session, std::string characterName)
    : kit_(kit), session_(session), name_(std::move(characterName)) {
    const auto& e = session_.worldEntry();
    playerX_ = e.x;
    playerY_ = e.y;
    playerZ_ = e.z;
    facing_ = e.orientation;
    camYaw_ = e.orientation;
    sentFacing_ = e.orientation;
    // terrainsharp, remotepredict, afksit, npcview, npcmax and the rest of the
    // graphics settings menu's keys: SettingsMenu::load + applySettings.
    settings_.load();
    loadingStep(0.02f);  // the loading screen's bar follows these stages (ui_loading_screen.hpp)
    initLighting();  // lighting (light.pak, glow buffer before the models take eDRAM)
    charAnimated_ = pp::configGetInt("charanim", 1) != 0;
    models_.loadForestConfig();  // treelod, treelod2dist, impostors, impostordist
    // KB of mip level 0 from which a doodad texture loads at half size (0 = full).
    models_.texLodBytes = static_cast<uint32_t>(std::max(0, pp::configGetInt("m2texlod", 16))) * 1024u;
    loadingStep(0.05f);
    loadTerrain();
    loadingStep(0.85f);
    loadCharacter();
    loadingStep(0.92f);
    applySettings();
    initAudio();
    loadingStep(0.96f);
    startStreaming();  // streaming
    primeBigWmos();    // a big WMO the player stands in - its near chunks under the loading screen
    {
        // Card open-file headroom once everything is open (0x80020320).
        char probe[256];
        pp::fsPath(probe, sizeof(probe), "data/audio.pak");
        const int freeFds = pp::pakProbeFreeFiles(probe, 40);  // kept for the budget below
        PSP_LOGI(pp::tag::Fs, "fd_probe free=%d (max 40) pak_fds=%d", freeFds,
                 pp::pakOpenFds());  // and how many of them our paks hold
        // the descriptor budget - what was free + what every holder has open now
        // (all of them note theirs: the log, the music, the sound bank, the minimap, ...).
        if (freeFds < 40) pp::pakFdSetCapacity(freeFds + pp::pakOpenFds());
        char list[320];
        const int open = pp::pakFdList(list, sizeof(list));
        PSP_LOGI(pp::tag::Fs, "fd_budget cap=%d open=%d free=%d tight=%d list=%s", pp::pakFdCapacity(), open,
                 pp::pakFdFree(), pp::pakFdTight() ? 1 : 0, list);
    }
}

// the world is destroyed on every transfer between maps (and on a disconnect) and
// built again. The members free themselves (renderers, paks, art structs, the streamer's and the minimap's threads,
// the sounds), except these plain ones: three textures (pp::Texture has no destructor) and the zone music table.
// The GE has finished with all of it: the LoginScreen destroys the world in update(), after frameEnd's sceGuSync.
WorldScreen::~WorldScreen() {
    // the session outlives this screen and goes on reading packets through the transfer's loading - the
    // hooks into this screen go with it (was: SMSG_WHO during a transfer -> sortWhoList -> lk.zoneName on a freed
    // WorldScreen; the spell data likewise). Every user checks them (empty / nullptr = without).
    session_.setWhoLookup(pp::social::WhoLookup{});
    session_.setSpellDb(nullptr);
    uint32_t bytes = 0;
    // + DELETE_ITEM's alert icon (the world goes while the popup shows: a map transfer).
    for (pp::Texture* t : {&watchBtn_, &tabArt_[0], &tabArt_[1], &selCircle_, &del_.alert}) {
        bytes += pp::textureBytes(*t);
        if (t->pixels) pp::textureRelease(*t);
    }
    if (zoneMusic_.blob) {
        pp::pakFree(zoneMusic_.blob);
        zoneMusic_.blob = nullptr;
    }
    PSP_LOGI(pp::tag::Mem, "world_screen_free map=%u textures_kb=%u", static_cast<unsigned>(session_.worldEntry().mapId),
             static_cast<unsigned>(bytes / 1024u));
}

// the server put us on a map whose data is not on the card (no Map.dbc row in
// glue.pak, no terrain_<dir>.pak / doodads_<dir>.pak): a failed transfer, said on the red error line like a
// TRANSFER_ABORT line (the port's game line rule, psp_game_line.hpp: Refused; own text), instead of a silent void.
// The player can still act (the Hearthstone, the chat, a GM's teleport).
void WorldScreen::mapDataMissing(uint32_t mapId, const char* what) {
    pp::WorldSession::MapInfo m;
    pp::WorldSession::mapInfo(mapId, m);
    const std::string text = OWN_FMT(MAP_DATA_MISSING, m.name.empty() ? std::to_string(mapId) : m.name, what);
    session_.gameLine(pp::GameLine::Refused, text);
    PSP_LOGW(pp::tag::Gfx, "map_data_missing map=%u dir=%s what=%s", static_cast<unsigned>(mapId), m.dir.c_str(), what);
}

// The menu's values to the things they set (upstream
// SettingsPanel::applySettingSideEffects), all at once: every change is a
// few assignments.
void WorldScreen::applySettings() {
    const GraphicsSettings& s = settings_.values();
    pp::musicSetVolume(static_cast<float>(s.musicvol) / 100.0f);  // MUSIC_VOLUME
    session_.setAutoLoot(s.autoloot != 0);  // AUTO_LOOT_DEFAULT_TEXT
    models_.viewDistance = static_cast<float>(s.m2view);
    remoteView_ = static_cast<float>(s.playerview);
    remoteMax_ = static_cast<uint32_t>(s.playermax);
    remoteCapMem_ = remoteMax_;
    remoteCapNextMs_ = 0;  // ranked again now
    creatures_.viewDistance = static_cast<float>(s.npcview);
    creatures_.maxDrawn = static_cast<uint32_t>(s.npcmax);
    terrain_.sharpFloor = s.terrainsharp != 0;
    terrain_.sharpDistance = static_cast<float>(s.sharpdist);
    terrain_.detail = s.terraindetail != 0;
    const int interval = s.framecap <= 30 ? 2 : 1;
    if (interval != pp::displayFrameInterval()) {
        pp::displaySetFrameInterval(interval);
        PSP_LOGI(pp::tag::Gfx, "frame_cap fps=%d interval=%d", s.framecap, interval);
    }
    remotePredict_ = s.remotepredict != 0;
    perfHud_ = s.perfhud != 0;
    afkSec_ = static_cast<float>(s.afksit);
    rendering::psp::CharacterModel::blendPercent = pp::configGetInt("animblend", 100);
    // the client's weapon placement (0 = upstream's), ranged on the back (option).
    rendering::psp::CharacterModel::sheathClient = pp::configGetInt("sheathclient", 1) != 0;
    rendering::psp::CharacterModel::rangedOnBack = pp::configGetInt("rangedback", 0) != 0;
}

// What is on screen now: L + up/down (view distance) and D-pad right (sharp
// floor) change these without the menu.
GraphicsSettings WorldScreen::liveSettings() const {
    GraphicsSettings s = settings_.values();
    s.m2view = static_cast<int>(models_.viewDistance + 0.5f);
    s.terrainsharp = terrain_.sharpFloor ? 1 : 0;
    return s;
}

void WorldScreen::loadCharacter() {
    dressUpBaseChanged();  // the DressUpFrame's model shares player_'s - it lets go first
    const game::Character* me = nullptr;
    for (const auto& c : session_.characters())
        if (c.name == name_) me = &c;
    if (!me) return;
    rendering::psp::CharacterLook look;
    look.race = static_cast<uint8_t>(me->race);
    look.sex = (me->gender == game::Gender::FEMALE || (me->gender == game::Gender::NONBINARY && me->useFemaleModel))
                   ? 1 : 0;
    const auto a = core::unpackAppearanceBytes(me->appearanceBytes);
    look.skin = a.skinId;
    look.face = a.faceId;
    look.hairStyle = a.hairStyleId;
    look.hairColor = a.hairColorId;
    look.facial = me->facialFeatures;
    ownLookOverride(look);  // our fields (a haircut), the barber shop's preview
    // M10c: what SMSG_CHAR_ENUM says the character wears (slots 0..18 of the
    // 23; upstream GameScreen::updateCharacterGeosets/Textures dress the own
    // player from the inventory, which the PSP does not track yet).
    rendering::psp::Equipment equip;
    for (size_t s = 0; s < 19 && s < me->equipment.size(); ++s) {
        equip.displayId[s] = me->equipment[s].displayModel;
        equip.invType[s] = me->equipment[s].inventoryType;
    }
    ownEquipmentOverride(equip);  // inventory - what we wear now, once known
    char path[256];
    pp::fsPath(path, sizeof(path), "data/items.pak");
    // items.pak read on demand (PakIndex); wowee.cfg itemlayerkb = its layer cache.
    items_.setBudget(rendering::psp::ItemBank::kRecordSlots,
                     static_cast<uint32_t>(std::max(0, pp::configGetInt("itemlayerkb", 48))) * 1024u);
    items_.setRareFds(true);  // items.pak / emblems.pak give their descriptor back (pakFdRelieve)
    if (!items_.isOpen() && !items_.open(path)) PSP_LOGW(pp::tag::Gfx, "items_missing (no equipment drawn)");
    // the weapons' sheath types and glows (SMSG_CHAR_ENUM's until our fields come).
    for (int k = 0; k < 3 && 15u + k < me->equipment.size(); ++k)
        equip.enchantVisual[k] = k < 2 ? static_cast<uint16_t>(me->equipment[15 + k].enchantment) : 0;
    fillOwnWeaponVisuals(equip);
    fillGuildEmblem(session_.guildId(), player_.equipment(), equip);  // the guild tabard's emblem
    pp::fsPath(path, sizeof(path), "data/characters.pak");
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    pp::Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(pp::tag::Gfx, "characters_missing");
        return;
    }
    player_.setLocalPlayer(true);  // weapons by AppearanceComposer's rules
    const bool ok = player_.load(pak, look, charAnimated_, &equip, &items_);
    animCaps_ = player_.capabilities();
    animator_ = rendering::CharacterAnimator();
    animator_.setCapabilities(animCaps_);
    applyCombatLoadout();  // combat - weapon loadout for the combat FSM
    if (sitting_) animator_.setStandState(1);
    ownLookLoaded();  // a chair's stand state again, the look the model has now
    lastAnimRequest_ = UINT32_MAX;
    pak.close();
    charPak_.setRare(true);  // the main thread's only - a rare pak (pakFdRelieve)
    charPak_.open(path);  // for other players (M10a)
    player_.setEmotePak(&charPak_);
    loadEmoteTable();
    chat_.loadTable(charPak_);  // chat
    PSP_LOGI(pp::tag::Gfx, "char_load ok=%d animated=%d ms=%u", ok ? 1 : 0, player_.animated() ? 1 : 0,
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    pp::logFlush();
}

void WorldScreen::loadTerrain() {
    const auto& e = session_.worldEntry();
    // every map's directory from Map.dbc (was 0 / 1 / 530 / 571 only).
    const std::string dirName = worldMapDir(e.mapId);
    const char* dir = dirName.empty() ? nullptr : dirName.c_str();
    if (!dir) {
        loadStatus_ = OWN_STR(MAP_NO_DATA);
        mapDataMissing(e.mapId, "Map.dbc");
        return;
    }
    char rel[64];
    std::snprintf(rel, sizeof(rel), "data/terrain_%s.pak", dir);
    char path[256];
    pp::fsPath(path, sizeof(path), rel);
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    if (!pak_.open(path)) {
        loadStatus_ = OWN_FMT(MAP_NO_FILE, rel);
        mapDataMissing(e.mapId, rel);
        return;
    }
    // Tile of the login position (upstream core/coordinates.hpp:
    // tileX = 32 - serverY / TILE_SIZE, tileY = 32 - serverX / TILE_SIZE).
    const int tx = static_cast<int>(std::floor(32.0f - e.y / kTileSize));
    const int ty = static_cast<int>(std::floor(32.0f - e.x / kTileSize));
    // a map with a global WMO (upstream WorldLoader::loadMapGeometry's WMO-only map,
    // a dungeon) has no terrain - its pak holds the "wmo_only" record alone; the WMO's groups and doodads are the
    // doodads pak's virtual tiles. PSP: the player stands on the WMO's floors (floorAt), moves where there is one.
    if (pak_.find("wmo_only")) {
        wmoOnly_ = true;
        pak_.close();
        PSP_LOGI(pp::tag::Gfx, "terrain map=%s wmo_only=1 tile=%d_%d server_z=%.1f", dir, tx, ty, playerZ_);
        loadingStep(0.35f);
        loadDoodads(dir, tx, ty);
        loadingStep(0.78f);
        loadCreatures(tx, ty);  // M10b hook (ui_world_creatures.cpp)
        return;
    }
    int loaded = 0;
    int step = 0;  // the loading bar, 0.05 .. 0.35 over the 9 tiles
    // The chunk-texture window - the tiles' heads now,
    // the textures within terraintexyd of the player below (the streamer keeps the window as the player moves; with
    // stream=0 nobody would, so the whole blobs as before).
    terrain_.texWindow = pp::configGetInt("terraintex", 1) != 0 && pp::configGetInt("stream", 1) != 0;
    terrain_.texRule = rendering::psp::texWindowRule(rendering::psp::TerrainCamera{}.farZ, 64.0f,
                                                     static_cast<float>(std::clamp(pp::configGetInt("terraintexband", 48), 8, 512)));
    if (const int keepYd = pp::configGetInt("terraintexyd", 0); keepYd > 0) {
        const float band = terrain_.texRule.dropYd - terrain_.texRule.keepYd;
        terrain_.texRule.keepYd = static_cast<float>(std::clamp(keepYd, 100, 4000));
        terrain_.texRule.dropYd = terrain_.texRule.keepYd + band;
    }
    int nomem = 0;  // tiles without memory (or with a failed read) - the streamer loads them later
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            if (terrain_.loadTile(pak_, tx + dx, ty + dy)) ++loaded;
            else if (terrain_.lastLoadNoMemory()) ++nomem;
            loadingStep(0.05f + 0.30f * static_cast<float>(++step) / 9.0f);
        }
    if (nomem)
        PSP_LOGW(pp::tag::Mem, "terrain_entry_retry tiles=%d loaded=%d free_kb=%u largest_kb=%u", nomem, loaded,
                 static_cast<unsigned>(pp::heapFreeKb()), static_cast<unsigned>(pp::heapLargestKb()));
    if (terrain_.texWindow) {
        const uint64_t tt = sceKernelGetSystemTimeWide();
        const uint32_t n = terrain_.texSyncLoad(pak_, playerX_, playerY_);
        const auto ts = terrain_.texStats();
        PSP_LOGI(pp::tag::Mem, "terrain_tex_entry tiles=%u chunks=%u/%u kb=%u nomem=%u keep_yd=%.0f drop_yd=%.0f ms=%u",
                 static_cast<unsigned>(ts.tiles), static_cast<unsigned>(n), static_cast<unsigned>(ts.chunks),
                 static_cast<unsigned>(ts.residentBytes / 1024u), static_cast<unsigned>(ts.nomem),
                 terrain_.texRule.keepYd, terrain_.texRule.dropYd,
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - tt) / 1000u));
    }
    pak_.close();
    loadMs_ = static_cast<uint32_t>((sceKernelGetSystemTimeWide() - t0) / 1000u);
    float z = 0;
    const bool ground = terrain_.heightAt(playerX_, playerY_, z);
    PSP_LOGI(pp::tag::Gfx, "terrain map=%s tile=%d_%d loaded=%d ms=%u ground=%.1f server_z=%.1f", dir, tx, ty,
             loaded, static_cast<unsigned>(loadMs_), ground ? z : 0.0f, playerZ_);
    pp::logFlush();
    if (!loaded && !nomem) {  // short of memory is not "not baked" (the streamer retries)
        loadStatus_ = OWN_STR(MAP_TILES_NOT_BAKED);
        return;
    }
    loadDoodads(dir, tx, ty);
    loadingStep(0.78f);  // the loading bar
    loadCreatures(tx, ty);  // M10b hook (ui_world_creatures.cpp)
}

void WorldScreen::loadDoodads(const char* dir, int tileX, int tileY) {
    char rel[64];
    std::snprintf(rel, sizeof(rel), "data/doodads_%s.pak", dir);
    char path[256];
    pp::fsPath(path, sizeof(path), rel);
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    if (pak_.open(path)) pak_.setWorldArena(true);  // its blobs in the world arena first
    if (!pak_.isOpen()) {
        PSP_LOGW(pp::tag::Gfx, "doodads_missing %s", rel);
        if (wmoOnly_) mapDataMissing(session_.worldEntry().mapId, rel);  // a dungeon is all doodads
        return;
    }
    int loaded = 0, skipped = 0;
    int step = 0;  // the loading bar, 0.35 .. 0.75 over the 9 tiles
    // The whole eastern continent in one map (a starting area's 3x3 ran out of memory here): only the tiles within
    // wowee.cfg doodadradius yd (Chebyshev to the tile's square, as TileStreamer::wantDoodads; 0 = all 9), the
    // player's first; a further tile only while streamreserve KB can still be allocated - the streamer loads
    // the rest when memory and distance allow.
    const float radius = static_cast<float>(std::clamp(pp::configGetInt("doodadradius", 200), 0, 2000));
    const uint32_t reserveKb = static_cast<uint32_t>(std::clamp(pp::configGetInt("streamreserve", 4096), 0, 32768));
    constexpr float kTileYd = 533.33333f;
    auto yardsTo = [&](int x, int y) {
        const float xLo = (31 - y) * kTileYd, xHi = (32 - y) * kTileYd;
        const float yLo = (31 - x) * kTileYd, yHi = (32 - x) * kTileYd;
        return std::max(std::max({0.0f, xLo - playerX_, playerX_ - xHi}), std::max({0.0f, yLo - playerY_, playerY_ - yHi}));
    };
    struct Near {
        int x, y;
        float yd;
    };
    Near order[9];
    int n = 0;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            order[n] = Near{tileX + dx, tileY + dy, yardsTo(tileX + dx, tileY + dy)};
            ++n;
        }
    std::sort(std::begin(order), std::end(order), [](const Near& a, const Near& b) { return a.yd < b.yd; });
    for (int i = 0; i < 9; ++i) {
        const int x = order[i].x, y = order[i].y;
        const bool near = radius <= 0.0f || order[i].yd <= radius;
        bool room = true;
        if (near && i > 0 && reserveKb) {
            // The headroom + one block for the tile's terrain (as TileStreamer::memoryOk; bigheap=0: the
            // old probe).
            room = pp::heapGivesKb(reserveKb + pp::heapGuardKb()) && pp::heapGuardOk() &&  // + the guard
                   (!pp::bigheapOn() || pp::heapGivesBlockKb(std::min<uint32_t>(reserveKb, 2048u)));
        }
        // every tile by budget - nearest placements first, each model only while
        // the heap still gives streamreserve KB besides it; the rest deferred (the streamer's top-up jobs place it
        // when there is room). Was: the player's own tile whole, without a probe (map 571: one coastal
        // tile, 43_28, alone is 19.0 MB of doodads - OOM at world entry).
        rendering::psp::ModelRenderer::LoadBudget budget;
        budget.x = playerX_;
        budget.y = playerY_;
        budget.reserveKb = reserveKb;
        const uint32_t deferredBefore = models_.budgetStats().deferred;
        if (near && room && models_.loadTile(pak_, x, y, budget)) {
            ++loaded;
            const uint32_t deferred = models_.budgetStats().deferred - deferredBefore;
            if (deferred || i == 0)
                PSP_LOGI(pp::tag::Mem, "doodads_entry_budget tile=%d_%d own=%d deferred=%u probes=%u reserve_kb=%u "
                         "heap_kb=%u",
                         x, y, i == 0 ? 1 : 0, static_cast<unsigned>(deferred),
                         static_cast<unsigned>(models_.budgetStats().probes), static_cast<unsigned>(reserveKb),
                         static_cast<unsigned>(pp::heapUsedKb()));
        } else if (near && !room) {
            ++skipped;
        }
        loadingStep(0.35f + 0.40f * static_cast<float>(++step) / 9.0f);
    }
    if (skipped) PSP_LOGW(pp::tag::Mem, "doodads_entry_lowmem skipped=%d reserve_kb=%u", skipped, static_cast<unsigned>(reserveKb));
    loadBigWmoIndex(pak_);  // "wbig" - where the big WMOs stand
    pak_.close();
    const auto& ms = models_.stats();
    PSP_LOGI(pp::tag::Gfx, "doodads tiles=%d instances=%u models=%u textures=%u kb=%u ms=%u heap_used_kb=%u", loaded,
             static_cast<unsigned>(ms.instances), static_cast<unsigned>(ms.models),
             static_cast<unsigned>(ms.textures), static_cast<unsigned>(ms.memoryKb),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u),
             static_cast<unsigned>(pp::heapUsedKb()));
    PSP_LOGI(pp::tag::Gfx, "doodads_texlod dropped=%u of %u (m2texlod %u KB)", static_cast<unsigned>(ms.texLodDropped),
             static_cast<unsigned>(ms.textures), static_cast<unsigned>(models_.texLodBytes / 1024u));
    PSP_LOGI(pp::tag::Gfx, "collision kb=%u", static_cast<unsigned>(models_.collisionKb()));
    pp::logFlush();
}

void WorldScreen::sendMove(game::Opcode opcode, bool moving) {
    game::MovementInfo mi;
    mi.flags = moving ? (moveFlags_ ? moveFlags_ : static_cast<uint32_t>(game::MovementFlags::FORWARD)) : 0u;
    mi.x = playerX_;
    mi.y = playerY_;
    mi.z = playerZ_;
    mi.orientation = facing_;
    if (airborne_) {
        // Upstream MovementHandler (MSG_MOVE_JUMP / heartbeats while falling):
        // FALLING + fall time + the jump's velocity, direction and xy speed.
        mi.flags |= static_cast<uint32_t>(game::MovementFlags::FALLING);
        mi.fallTime = pp::logUptimeMs() - fallStartMs_;
        mi.jumpVelocity = jumpVelocity_;
        mi.jumpSinAngle = std::sin(facing_);
        mi.jumpCosAngle = std::cos(facing_);
        mi.jumpXYSpeed = moving ? session_.serverRunSpeed() : 0.0f;  // mounts (was 7)
    }
    transportMoveInfo(mi);  // riding - ONTRANSPORT, the transport and the place on it
    flightMoveInfo(mi);  // the granted flags (CAN_FLY ...), FLYING / SWIMMING / the climb, the pitch
    session_.sendMovement(opcode, mi);
    sentFacing_ = facing_;
}

// Highest standable floor under (x, y) no higher than maxZ: terrain (if not
// steeper than 50 degrees above the current feet) or a model/building floor.
float WorldScreen::floorAt(float x, float y, float maxZ, bool* onTerrain) const {
    constexpr float kMaxSlope = 1.1918f;  // tan 50 deg
    float ground = -1e30f;
    bool terrainFloor = false;
    float tz = 0;
    if (terrain_.heightAt(x, y, tz) && tz <= maxZ) {
        float zx0 = tz, zx1 = tz, zy0 = tz, zy1 = tz;
        terrain_.heightAt(x - 0.5f, y, zx0);
        terrain_.heightAt(x + 0.5f, y, zx1);
        terrain_.heightAt(x, y - 0.5f, zy0);
        terrain_.heightAt(x, y + 0.5f, zy1);
        const float slope = std::sqrt((zx1 - zx0) * (zx1 - zx0) + (zy1 - zy0) * (zy1 - zy0));
        if (!(slope > kMaxSlope && tz > playerZ_ + 0.01f)) {
            ground = tz;
            terrainFloor = true;
        }
    }
    float mz = 0;
    if (models_.floorHeight(x, y, maxZ, mz) && mz > ground) {
        ground = mz;
        terrainFloor = false;
    }
    if (bigWmoFloor(x, y, maxZ, mz) && mz > ground) {  // big WMO floors (resident chunks)
        ground = mz;
        terrainFloor = false;
    }
    float trz = 0;  // a deck, a lift's floor, a WMO object's floor
    if (transportFloor(x, y, maxZ, trz) && trz > ground) {
        ground = trz;
        terrainFloor = false;
    }
    float wwz = 0;  // water walking - the water's surface is a floor
    if (flightWaterFloor(x, y, maxZ, wwz) && wwz > ground) {
        ground = wwz;
        terrainFloor = false;
    }
    if (onTerrain) *onTerrain = terrainFloor;
    return ground;
}

// Movement with collision, gravity and jump:
//  - face buttons walk relative to the camera;
//    walls push the target out, the floor is the highest reachable one
//    (step <= 0.6 yd up, terrain <= 50 degrees);
//  - START jumps (the client's 7.96 yd/s), gravity -19.29 yd/s^2 (upstream
//    CameraController WOW_GRAVITY / WOW_JUMP_VELOCITY); walking off a ledge
//    falls instead of snapping down; MSG_MOVE_JUMP / MSG_MOVE_FALL_LAND.
void WorldScreen::updateMovement(const pp::PadState& pad, float dtSec) {
    PROF_SCOPE("move");
    // The landing hold - world entry once, then after every teleport (applyTeleport).
    if (!tpHold_.entryDone) {
        tpHold_.entryDone = true;
        startTeleportHold("entry");
    }
    if (teleportHold()) return;  // the floor under the player is not loaded yet: no gravity, no input
    if (taxiMovement(dtSec)) return;  // the flight / the landing clamp moves the player
    // carried by the transport we ride (held still while it changes map), and after the
    // step below (every return): boarding / leaving, the place in its frame.
    if (transportCarry(dtSec)) return;
    if (classBarMovement(dtSec)) return;  // a vehicle passenger (the seat carries us) / no control
    struct TransportAfter {
        WorldScreen& w;
        ~TransportAfter() { w.transportAfterMove(); }
    } transportAfter{*this};
    // the water and the air - swimming / flying move in 3D there (ui_world_flying.cpp); take-off,
    // landing, the swim's start and end decide here first.
    if (flightMovement(pad, dtSec)) return;
    // Mounts - the server's speeds (upstream CameraController's
    // runSpeedOverride_ etc. from MovementHandler: 7 / 4.5 / 2.5 until the
    // server changes them - a mount raises the run speed).
    const float kRunSpeed = session_.serverRunSpeed();
    const float kBackSpeed = session_.serverRunBackSpeed();
    const float kWalkSpeed = session_.serverWalkSpeed();  // upstream WOW_WALK_SPEED
    constexpr float kMaxStepUp = 0.60f;
    constexpr float kGravity = 19.29f;
    constexpr float kJumpVelocity = 7.96f;
    const uint32_t nowMs = pp::logUptimeMs();
    if (dtSec > 0.1f) dtSec = 0.1f;

    // L / R = left / right mouse button (upstream CameraController): R turns
    // the character with the camera (cameraDrivesFacing) and the buttons
    // walk / backpedal / strafe relative to it; L + R runs forward
    // (mouseAutorun). Without R the character turns to where it walks.
    const bool lHeld = (pad.held & PSP_CTRL_LTRIGGER) != 0;
    const bool rHeld = (pad.held & PSP_CTRL_RTRIGGER) != 0;
    const bool mouseAutorun = lHeld && rHeld;
    const bool drivesFacing = controlsScheme_ < 2 && rHeld;
    const bool stick = controlsScheme_ >= 2;  // controls=2: the stick moves (ui_world_controls.cpp)
    const bool stickAutorun = stick && autorun_ && !stickMove_;
    const bool nowForward = stick ? (moveY_ < 0.0f || stickAutorun) : ((pad.held & PSP_CTRL_TRIANGLE) || mouseAutorun);
    const bool nowBackward = stick ? moveY_ > 0.0f : (pad.held & PSP_CTRL_CROSS) != 0;
    const float fwd = stick ? (stickAutorun ? 1.0f : -moveY_)
                            : (nowForward ? 1.0f : 0.0f) - (nowBackward ? 1.0f : 0.0f);
    const float side = stick ? moveX_
                             : ((pad.held & PSP_CTRL_CIRCLE) ? 1.0f : 0.0f) - ((pad.held & PSP_CTRL_SQUARE) ? 1.0f : 0.0f);
    // controls=2 autorun runs along the facing (the camera stays free).
    const float yawBase = stickAutorun ? facing_ : camYaw_;
    const glm::vec2 f(std::cos(yawBase), std::sin(yawBase));
    const glm::vec2 r(f.y, -f.x);  // x north, y west: right of north is east = -y
    glm::vec2 dir = f * fwd + r * side;
    const bool wantMove = glm::dot(dir, dir) > 1e-4f;
    wantMove_ = wantMove;
    if (drivesFacing) facing_ = camYaw_;
    // Movement flags the server and the other clients see.
    uint32_t flags = 0;
    if (wantMove) {
        if (!drivesFacing) {
            flags = static_cast<uint32_t>(game::MovementFlags::FORWARD);
            if (stick && stickWalk_) flags |= static_cast<uint32_t>(game::MovementFlags::WALKING);
        } else {
            if (fwd > 0.0f) flags |= static_cast<uint32_t>(game::MovementFlags::FORWARD);
            if (fwd < 0.0f) flags |= static_cast<uint32_t>(game::MovementFlags::BACKWARD);
            if (side > 0.0f) flags |= static_cast<uint32_t>(game::MovementFlags::STRAFE_RIGHT);
            if (side < 0.0f) flags |= static_cast<uint32_t>(game::MovementFlags::STRAFE_LEFT);
        }
    }
    const uint32_t oldFlags = moveFlags_;
    moveFlags_ = flags;
    animBackward_ = drivesFacing && fwd < 0.0f;
    animStrafe_ = (drivesFacing && fwd == 0.0f) ? side : 0.0f;
    const bool wasMoving = moving_;
    const bool wasAirborne = airborne_;

    // Jump (START; controls=2: the mode's jump). PSP: no START+SELECT exit - HOME / the menu.
    const bool jumpKey = !sitStandNoJump_ &&
                         (stick ? jumpReq_ : (pad.pressed & PSP_CTRL_START));
    sitStandNoJump_ = false;  // PSP: the press that stood the character up (updateSitting) is not a jump
    jumpReq_ = false;
    // Mounts - upstream CameraController: no jump while mounted
    // (canJump ... && !mounted_); the key goes to MountFSM, whose
    // triggerMountJump hops at getMountJumpVelocity (sqrt(2 x 18 x 1) = 6).
    const bool mountedNow = mountActive_;
    mountJumpKey_ = mountedNow && jumpKey;
    const bool takeOff = mountedNow ? mountJumpGo_ : jumpKey;
    mountJumpGo_ = false;
    if (!airborne_ && takeOff) {
        airborne_ = true;
        vz_ = mountedNow ? 6.0f : kJumpVelocity;
        jumpVelocity_ = vz_;
        fallStartMs_ = nowMs;
        // LocomotionFSM ignores a new take-off while JUMP_END plays (up to
        // 1.1 s); its own JUMP event restarts JUMP_START on every jump.
        if (!mountedNow) animator_.onEvent(rendering::AnimEvent::JUMP);
        moving_ = wantMove;
        sendMove(game::Opcode::MSG_MOVE_JUMP, wantMove);
    }

    // Horizontal step with walls.
    float nx = playerX_, ny = playerY_;
    if (wantMove) {
        dir = glm::normalize(dir);
        if (!drivesFacing) facing_ = std::atan2(dir.y, dir.x);
        // Upstream: backpedal at WOW_BACK_SPEED when only going backwards.
        float speed = (nowBackward && !nowForward && drivesFacing) ? kBackSpeed : kRunSpeed;
        if (stick && stickWalk_) speed = kWalkSpeed;  // controls=2: a light tilt walks (WALKING)
        glm::vec3 to(playerX_ + dir.x * speed * dtSec, playerY_ + dir.y * speed * dtSec, playerZ_);
        models_.collideWalls(to, playerZ_);
        bigWmoWalls(to, playerZ_);
        transportWalls(to, playerZ_);  // hulls, lift cages, WMO objects
        float tz = 0;
        // a dungeon has no terrain - the floor search below decides (no floor: stay).
        // so does a deck at sea (no terrain tile under a ship between continents).
        if (terrain_.heightAt(to.x, to.y, tz) || wmoOnly_ || transportRiding()) {  // else: off the loaded tiles, stay
            nx = to.x;
            ny = to.y;
        }
    }

    // Vertical.
    const float oldZ = playerZ_;
    if (!airborne_) {
        const float ground = floorAt(nx, ny, playerZ_ + kMaxStepUp);
        if (ground < -1e29f) {
            nx = playerX_;  // a wall of rock or a step too high: stay
            ny = playerY_;
        } else if (ground < playerZ_ - kMaxStepUp) {
            airborne_ = true;  // walked off a ledge: fall
            vz_ = 0.0f;
            jumpVelocity_ = 0.0f;
            fallStartMs_ = nowMs;
        } else {
            playerZ_ = ground;
        }
    }
    if (airborne_) {
        vz_ -= kGravity * dtSec;
        float z = playerZ_ + vz_ * dtSec;
        // Land on the highest floor between the old and the new height.
        const float ground = floorAt(nx, ny, std::max(playerZ_, z) + 0.05f);
        if (vz_ <= 0.0f && ground > -1e29f && z <= ground) {
            z = ground;
            airborne_ = false;
            vz_ = 0.0f;
        } else if (ground < -1e29f && z < oldZ - 200.0f) {
            // Nothing below at all (hole in the data): put the feet back on
            // the terrain rather than falling forever.
            float tz = 0;
            if (terrain_.heightAt(nx, ny, tz)) z = tz;
            // PSP: no terrain in a dungeon - back to the last floor stood on (upstream's void-fall
            // unstuck is off by default; the server hears the place with MSG_MOVE_FALL_LAND below).
            else if (wmoOnly_ && wmoSafeValid_) {
                nx = wmoSafePos_.x;
                ny = wmoSafePos_.y;
                z = wmoSafePos_.z;
                if (++wmoVoidFalls_ <= 10)
                    PSP_LOGW(pp::tag::Gfx, "wmo_void_fall back x=%.1f y=%.1f z=%.1f", nx, ny, z);
            }
            airborne_ = false;
            vz_ = 0.0f;
        }
        playerZ_ = z;
    }
    playerX_ = nx;
    playerY_ = ny;
    if (wmoOnly_ && !airborne_) {  // the last floor stood on
        wmoSafePos_ = glm::vec3(playerX_, playerY_, playerZ_);
        wmoSafeValid_ = true;
    }

    // A sudden drop while grounded is the collision missing a floor: log
    // where, so the baked mesh can be checked offline (on hardware it fell
    // into a house and could not get out).
    if (!wasAirborne && !airborne_ && oldZ - playerZ_ > 1.5f && ++dropLogs_ <= 10) {
        float tz = 0;
        terrain_.heightAt(playerX_, playerY_, tz);
        PSP_LOGW(pp::tag::Gfx, "floor_drop x=%.2f y=%.2f z=%.2f->%.2f terrain=%.2f", playerX_, playerY_, oldZ,
                 playerZ_, tz);
    }

    // Server.
    if (wasAirborne && !airborne_) {
        sendMove(game::Opcode::MSG_MOVE_FALL_LAND, wantMove);
        moving_ = wantMove;
        nextHeartbeatMs_ = nowMs + 500;
        return;
    }
    float df = std::fabs(facing_ - sentFacing_);
    if (df > 3.14159265f) df = 6.2831853f - df;
    if (!wantMove) {
        if (moving_ && !airborne_) {
            moving_ = false;
            sendMove(game::Opcode::MSG_MOVE_STOP, false);
        } else if (airborne_ && nowMs >= nextHeartbeatMs_) {
            nextHeartbeatMs_ = nowMs + 500;
            sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, false);
        } else if (!airborne_ && df > 0.1f && nowMs >= nextHeartbeatMs_) {
            // Turning in place with R: the facing, at most every 200 ms.
            nextHeartbeatMs_ = nowMs + 200;
            sendMove(game::Opcode::MSG_MOVE_SET_FACING, false);
        }
        return;
    }
    if ((!wasMoving && !moving_) || (flags != oldFlags && !airborne_)) {
        moving_ = true;
        nextHeartbeatMs_ = nowMs + 500;
        using MF = game::MovementFlags;
        const auto start = (flags & static_cast<uint32_t>(MF::FORWARD))       ? game::Opcode::MSG_MOVE_START_FORWARD
                           : (flags & static_cast<uint32_t>(MF::BACKWARD))    ? game::Opcode::MSG_MOVE_START_BACKWARD
                           : (flags & static_cast<uint32_t>(MF::STRAFE_LEFT)) ? game::Opcode::MSG_MOVE_START_STRAFE_LEFT
                                                                              : game::Opcode::MSG_MOVE_START_STRAFE_RIGHT;
        sendMove(start, true);
        if (++moveStarts_ <= 3)
            PSP_LOGI(pp::tag::Net, "move_start x=%.1f y=%.1f z=%.1f o=%.2f", playerX_, playerY_, playerZ_, facing_);
        return;
    }
    moving_ = true;
    if (df > 0.1f) {
        sendMove(game::Opcode::MSG_MOVE_SET_FACING, true);
    } else if (nowMs >= nextHeartbeatMs_) {
        nextHeartbeatMs_ = nowMs + 500;
        sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, true);
    }
}

// Player animation, after upstream AnimationController::updateCharacterAnimation
// + CharacterAnimator: the renderer's time advances first (a finished
// one-shot is back to Stand), then LocomotionFSM resolves from the movement
// state and the current animation; a new request plays, a looping one is
// re-asserted only if the model drifted from it. Input mapping (upstream
// CameraController): moving = input held, sprinting = run pace (the PSP
// always runs at 7 yd/s), grounded = !airborne, jumping = rising
// (isJumping: !grounded && verticalVelocity > 0). The character always
// turns to the step direction, so it only ever moves forward.
// Sitting like upstream CameraController: D-pad left toggles it (the X
// key), any movement or jump stands up; the server hears
// CMSG_STANDSTATECHANGE, the animator its stand state (ActivityFSM: sit down
// -> sitting -> sit up). AFK (like the PC client): no input
// for afkSec_ seconds sits the character down; any input stands it up.
void WorldScreen::setSitting(bool sit, bool afk) {
    if (sit == sitting_) return;
    if (sit && mountActive_) return;  // mounts - upstream: no sitting while mounted (AFK too)
    if (sit && ownSwimOrFly()) return;  // not in the water / in the air (the sit key descends there)
    if (sit && session_.inVehicle()) return;  // nor on a vehicle seat
    sitting_ = sit;
    afk_ = sit && afk;
    session_.sendStandState(sit ? 1 : 0);
    animator_.setStandState(sit ? 1 : 0);
    PSP_LOGI(pp::tag::Gfx, "stand_state sit=%d afk=%d", sit ? 1 : 0, afk_ ? 1 : 0);
}

void WorldScreen::updateSitting(const pp::PadState& pad, float dtSec) {
    const uint32_t moveKeys = PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS | PSP_CTRL_SQUARE | PSP_CTRL_CIRCLE | PSP_CTRL_START;
    const bool lr = (pad.held & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) == (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER);
    const bool stick = std::fabs(pad.ax) > 0.2f || std::fabs(pad.ay) > 0.2f;
    const bool anyInput = pad.held != 0 || stick;
    if (sitting_ && ((pad.held & moveKeys) || lr)) {
        // PSP: upstream CameraController (nowJump needs !sitting; "Stand up on any movement input or jump while sitting")
        // - the jump key of this frame only stands up (controls 0/1: START, read again by updateMovement below).
        if (pad.pressed & (PSP_CTRL_START | PSP_CTRL_CROSS)) sitStandNoJump_ = true;
        setSitting(false, false);
    }
    const bool sitKey = (pad.pressed & PSP_CTRL_LEFT) && !(pad.held & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER));
    if (sitKey && !airborne_) setSitting(!sitting_, false);
    if (afk_ && anyInput && !sitKey) setSitting(false, false);
    idleSec_ = anyInput ? 0.0f : idleSec_ + dtSec;
    if (afkSec_ > 0.0f && idleSec_ >= afkSec_ && !sitting_ && !airborne_) setSitting(true, true);
}

// M9c emotes, after upstream: SMSG_EMOTE -> Emotes.dbc (EmoteRegistry) ->
// emoteAnimCallback. Other players (AnimationCallbackHandler, player
// branch): anim 0 -> Stand, a state emote loops, else one-shot (the model
// returns to Stand). Own player: CharacterAnimator::playEmote / cancelEmote
// (ActivityFSM; moving cancels it). UNIT_NPC_EMOTESTATE (e.g. /dance) is a
// looping state emote, 0 clears it.
void WorldScreen::loadEmoteTable() {
    emoteTable_.clear();
    const pp::PakEntry* e = charPak_.find("emotes");
    void* blob = e ? charPak_.load(e) : nullptr;
    if (blob && e->size >= sizeof(rendering::psp::EmoteTable) &&
        std::memcmp(blob, "WEMO", 4) == 0) {
        const auto* t = static_cast<const rendering::psp::EmoteTable*>(blob);
        const auto* rows = reinterpret_cast<const rendering::psp::EmoteRow*>(t + 1);
        if (sizeof(*t) + uint64_t{t->count} * sizeof(rendering::psp::EmoteRow) <= e->size)
            for (uint32_t i = 0; i < t->count; ++i)
                emoteTable_[rows[i].emoteId] = rows[i].animId | (rows[i].state ? 0x10000u : 0u);
    }
    if (blob) pp::pakFree(blob);
    PSP_LOGI(pp::tag::Gfx, "emote_table rows=%u", static_cast<unsigned>(emoteTable_.size()));
}

void WorldScreen::playRemoteEmote(RemotePlayer& r, uint32_t emoteId, bool fromField) {
    if (!r.model) return;
    auto it = emoteTable_.find(emoteId);
    const uint32_t anim = emoteId && it != emoteTable_.end() ? (it->second & 0xFFFFu) : 0u;
    const bool state = fromField || (emoteId && it != emoteTable_.end() && (it->second & 0x10000u));
    if (emoteId != 0 && anim == 0) return;  // text-only emote
    if (anim == 0) {
        r.model->playAnimation(rendering::anim::STAND, true);
        r.anim = rendering::anim::STAND;
    } else if (r.model->canPlay(anim)) {
        r.model->playAnimation(anim, state);
        r.anim = anim;
    }
    PSP_LOGI(pp::tag::Gfx, "emote remote emote=%u anim=%u state=%d", static_cast<unsigned>(emoteId),
             static_cast<unsigned>(anim), state ? 1 : 0);
}

// Upstream AnimationController::playEmote: the ActivityFSM takes the emote
// AND the renderer plays it at once - otherwise the FSM's next frame sees
// "current anim != emote" and calls the one-shot finished (seen on hardware:
// no emote ever showed).
void WorldScreen::playOwnEmote(uint32_t anim, bool loop) {
    animator_.playEmote(anim, loop);
    player_.playAnimation(anim, loop);
    lastAnimRequest_ = anim;
    lastAnimLoop_ = loop;
}

void WorldScreen::processEmotes() {
    uint64_t guid = 0;
    uint32_t emoteId = 0;
    while (session_.takeEmote(guid, emoteId)) {
        if (guid == session_.playerGuid()) {
            auto it = emoteTable_.find(emoteId);
            const uint32_t anim = emoteId && it != emoteTable_.end() ? (it->second & 0xFFFFu) : 0u;
            if (emoteId == 0) {
                animator_.cancelEmote();
            } else if (anim && player_.canPlay(anim)) {
                playOwnEmote(anim, (it->second & 0x10000u) != 0);
            }
            PSP_LOGI(pp::tag::Gfx, "emote self emote=%u anim=%u", static_cast<unsigned>(emoteId),
                     static_cast<unsigned>(anim));
            continue;
        }
        auto r = remotes_.find(guid);
        if (r != remotes_.end()) playRemoteEmote(r->second, emoteId, false);
        else playCreatureEmote(guid, emoteId);  // creature anim
    }
    const uint16_t fEmoteState = game::fieldIndex(game::UF::UNIT_NPC_EMOTESTATE);
    // Our own state emote (e.g. /dance): the server sets it in our own update
    // fields, which the session keeps apart from the entities.
    const uint32_t own = session_.ownField(fEmoteState);
    if (own != ownEmoteState_) {
        ownEmoteState_ = own;
        auto it = emoteTable_.find(own);
        const uint32_t anim = own && it != emoteTable_.end() ? (it->second & 0xFFFFu) : 0u;
        if (anim && player_.canPlay(anim)) playOwnEmote(anim, true);
        else animator_.cancelEmote();
        PSP_LOGI(pp::tag::Gfx, "emote self_state emote=%u anim=%u", static_cast<unsigned>(own), static_cast<unsigned>(anim));
    }
    for (auto& [g, r] : remotes_) {
        auto e = session_.entities().getEntity(g);
        if (!e || !r.model) continue;
        const uint32_t st = e->getField(fEmoteState);
        if (st == r.emoteState) continue;
        r.emoteState = st;
        playRemoteEmote(r, st, true);
    }
}

void WorldScreen::updateAnimation(float dtSec) {
    if (!player_.animated()) return;
    PROF_SCOPE("char.fsm");
    player_.update(dtSec);
    if (combatAnimOverride()) return;  // combat - dead: Death, held
    if (updateMountedAnimation(dtSec)) return;  // mounts - upstream: mounted -> updateMountedAnimation only
    if (classBarSeatAnimation()) return;  // the vehicle seat's ride animation / the possessing body
    if (flightOwnAnim()) return;  // flying on foot: Fly / Hover
    // Upstream AnimationController::updateCharacterAnimation: FrameInput ->
    // CharacterAnimator (locomotion, activity = sit, combat) -> output.
    rendering::CharacterAnimator::FrameInput in;
    in.moving = wantMove_;
    in.movingForward = wantMove_ && !animBackward_ && animStrafe_ == 0.0f;
    in.movingBackward = animBackward_;
    in.strafeLeft = animStrafe_ < 0.0f;
    in.strafeRight = animStrafe_ > 0.0f;
    in.sprinting = true;
    in.grounded = !airborne_;
    in.jumping = airborne_ && vz_ > 0.0f;
    in.sitting = sitting_;
    in.swimming = ownSwimming();  // the swim set
    in.characterYaw = facing_;
    in.haveAnimState = player_.getAnimationState(in.currentAnimId, in.currentAnimTime, in.currentAnimDuration);
    combatFrameInput(in, dtSec);  // combat - melee swing timer
    animator_.setFrameInput(in);
    animator_.update(dtSec);
    const rendering::AnimOutput out = animator_.getLastOutput();
    if (!out.valid) return;
    uint32_t curId = 0;
    float curT = 0.0f, curDur = 0.0f;
    const bool haveState = player_.getAnimationState(curId, curT, curDur);
    const bool requestChanged = lastAnimRequest_ != out.animId || lastAnimLoop_ != out.loop;
    const bool drifted = haveState && curId != out.animId && out.loop;
    if (requestChanged || drifted) {
        if (requestChanged && ++animLogs_ <= 300)
            PSP_LOGI(pp::tag::Gfx, "anim_req id=%u loop=%d air=%d vz=%.1f has=%d", static_cast<unsigned>(out.animId),
                     out.loop ? 1 : 0, airborne_ ? 1 : 0, vz_, player_.hasAnimation(out.animId) ? 1 : 0);
        player_.playAnimation(out.animId, out.loop);
        lastAnimRequest_ = out.animId;
        lastAnimLoop_ = out.loop;
    }
}

// Camera collision, after upstream CameraController (camera_controller.cpp
// "Camera collision"): a ray from the pivot to the camera against buildings
// (upstream: props do not stop the camera; PSP: with camm2=1 tree
// trunks and other props >= camm2min yd tall do, as in the original client), a
// march over the terrain height field, a
// +-14 degree yaw to step around pillars and door frames, fast pull-in
// (60 ms, instant while the stick turns) and slow recovery (400 ms).
float WorldScreen::cameraDistance(const glm::vec3& pivot, glm::vec3& dir) {
    // Wall clearance: upstream keeps 0.32 + 0.22, but the near plane's corners
    // reach ~1.5x nearZ from the eye (fov 60, 16:9) - keep ~1 yd so a wall
    // is never cut by the near plane.
    constexpr float kMinDist = 0.5f, kSphere = 1.0f, kEps = 0.6f;  // 1.6 yd: near plane 1.0 x ~1.53 corner
    const float want = currentDist_;
    auto wallLimit = [&](const glm::vec3& d) {
        float hit = models_.raycastBuildings(pivot, d, want);
        hit = bigWmoRay(pivot, d, hit);  // big WMOs hold the camera like the other buildings
        hit = transportCameraRay(pivot, d, hit);  // a ship's hull, a WMO object
        if (camM2_) hit = models_.raycastProps(pivot, d, hit, camM2Min_);  // camera vs trees (PSP, camm2)
        return hit >= want ? want : std::max(kMinDist, hit - kSphere - kEps);
    };
    float raw = wallLimit(dir);
    float deflect = 0.0f;
    if (raw < want - 0.5f) {
        float best = raw;
        for (float deg : {14.0f, -14.0f}) {
            const float a = glm::radians(deg), ca = std::cos(a), sa = std::sin(a);
            const glm::vec3 td = glm::normalize(glm::vec3(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca, dir.z));
            const float lim = wallLimit(td);
            if (lim > best + 1.0f) {
                best = lim;
                deflect = deg;
            }
        }
        raw = best;
    }
    const float dt = lastDt_;
    camDeflectDeg_ += (deflect - camDeflectDeg_) * (1.0f - std::exp(-dt / (deflect != 0.0f ? 0.12f : 0.30f)));
    if (std::fabs(camDeflectDeg_) > 0.05f) {
        const float a = glm::radians(camDeflectDeg_), ca = std::cos(a), sa = std::sin(a);
        dir = glm::normalize(glm::vec3(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca, dir.z));
    }
    // Terrain between the pivot and the camera (upstream raymarchTerrainCameraLimit).
    float terrainLimit = want;
    float pivotGround = 0;
    if (!(terrain_.heightAt(pivot.x, pivot.y, pivotGround) && pivot.z < pivotGround - 0.2f)) {
        const int steps = std::clamp(static_cast<int>(want / 1.25f) + 1, 4, 24);
        const float stepLen = want / steps;
        for (int i = 1; i <= steps; ++i) {
            const glm::vec3 p = pivot + dir * (stepLen * i);
            float h = 0;
            if (terrain_.heightAt(p.x, p.y, h) && p.z < h + 1.1f) {
                terrainLimit = std::max(kMinDist, stepLen * (i - 1));
                break;
            }
        }
    }
    if (camTerrainLimit_ < 0.0f) camTerrainLimit_ = terrainLimit;
    camTerrainLimit_ += (terrainLimit - camTerrainLimit_) *
                        (1.0f - std::exp(-dt / (terrainLimit < camTerrainLimit_ ? 0.18f : 0.45f)));
    // The zoom debt below follows the wall sweep only, like upstream (its
    // bite is userTargetDistance - smoothedCollisionDist_; the terrain march
    // clamps the camera without taking the zoom). With the terrain in it, a
    // hill behind the player took the zoom in and the camera crept back out
    // afterwards.
    if (camWallLimit_ < 0.0f) camWallLimit_ = raw;
    if (raw < camWallLimit_ && rotating_) {
        camWallLimit_ = raw;
    } else {
        camWallLimit_ += (raw - camWallLimit_) * (1.0f - std::exp(-dt / (raw < camWallLimit_ ? 0.06f : 0.40f)));
    }
    raw = std::min(raw, camTerrainLimit_);
    if (camLimit_ < 0.0f) camLimit_ = raw;
    if (raw < camLimit_ && rotating_) {
        camLimit_ = raw;
    } else {
        camLimit_ += (raw - camLimit_) * (1.0f - std::exp(-dt / (raw < camLimit_ ? 0.06f : 0.40f)));
    }
    // "A collision takes the zoom in with it" (upstream): pull the player's
    // zoom toward what the sweep allows over ~1 s, give it back 1 s after the
    // way is clear, over ~2.5 s - the smooth zoom-out seen on the PC client.
    const float bite = distance_ - camWallLimit_;
    if (bite > 0.1f) {
        clearSeconds_ = 0.0f;
        const float step = bite * (1.0f - std::exp(-dt / 1.0f));
        const float pulled = std::max(distance_ - step, kMinDist);
        zoomDebt_ += distance_ - pulled;
        distance_ = pulled;
    } else {
        clearSeconds_ += dt;
        if (clearSeconds_ > 1.0f && zoomDebt_ > 0.001f) {
            const float give = zoomDebt_ * (1.0f - std::exp(-dt / 2.5f));
            distance_ += give;
            zoomDebt_ -= give;
        }
    }
    return std::min(want, camLimit_);
}

/// M10a: other players. Their look comes from the update fields exactly as
// upstream EntityController::extractPlayerAppearance reads them: race =
// UNIT_FIELD_BYTES_0 byte 0, gender = byte 2, PLAYER_BYTES = skin/face/hair
// style/hair colour, PLAYER_BYTES_2 byte 0 = facial hair. Built only once the
// display id is set (upstream dispatchEntitySpawn), rebuilt when it changes.
//
// M10c: equipment as upstream InventoryHandler::emitOtherPlayerEquipment ->
// EntitySpawner::setOnlinePlayerEquipment: the visible item entries resolved
// by item queries (WorldSession::equipmentOf).
// Item.dbc first (as the original client), so the
// queries are only waited for on items the client's DBC lacks.
//
// the scan (when the session's entity version moves:
// a player came / went, a look field, a visible item's entry or enchantment, the helm / cloak flag changed) only
// notes what each player should wear; the dresses are done by dressRemoteQueue, nearest first, within a time
// budget per frame - a change is dressed at once (as upstream's emit on every change), a crowd that appears
// does not stop the frame. Any raw change of those fields re-dresses (what the model shows of them is the
// equipment code's: the helm / cloak flags, the enchantments' glow).
// PSP: an entry the client's Item.dbc lacks (a server's custom item) is dressed without it at once and again
// when its query is answered - answers that trickle in within 1 s make one re-dress (upstream: one per answer).
void WorldScreen::syncRemotePlayers() {
    const uint32_t nowMs = static_cast<uint32_t>(sceKernelGetSystemTimeWide() / 1000u);
    if (session_.entityVersion() != seenEntityVersion_ || (equipWaiting_ && nowMs >= equipCheckMs_)) {
        seenEntityVersion_ = session_.entityVersion();
        equipWaiting_ = false;
        const uint16_t fBytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
        const uint16_t fDisplay = game::fieldIndex(game::UF::UNIT_FIELD_DISPLAYID);
        const uint16_t fBytes = game::fieldIndex(game::UF::PLAYER_BYTES);
        const uint16_t fBytes2 = game::fieldIndex(game::UF::PLAYER_BYTES_2);
        const uint16_t fFlags = game::fieldIndex(game::UF::PLAYER_FLAGS);
        constexpr uint16_t kVisibleItem = 283;  // PLAYER_VISIBLE_ITEM_1_ENTRYID (3.3.5a), + 1 its ENCHANTMENT, stride 2
        std::unordered_map<uint64_t, bool> alive;
        for (const auto& [guid, e] : session_.entities().getEntities()) {
            if (e->getType() != game::ObjectType::PLAYER || guid == session_.playerGuid()) continue;
            if (e->getField(fDisplay) == 0) continue;
            alive[guid] = true;
            const uint32_t b0 = e->getField(fBytes0);
            rendering::psp::CharacterLook look;
            look.race = static_cast<uint8_t>(b0 & 0xFF);
            look.sex = ((b0 >> 16) & 0xFF) == 1 ? 1 : 0;
            const auto a = core::unpackAppearanceBytes(e->getField(fBytes));
            look.skin = a.skinId;
            look.face = a.faceId;
            look.hairStyle = a.hairStyleId;
            look.hairColor = a.hairColorId;
            look.facial = static_cast<uint8_t>(e->getField(fBytes2) & 0xFF);
            rendering::psp::Equipment equip;
            int unresolved = 0;
            // the client's Item.dbc first (items.pak "u_", ItemBank::itemDisplay) -
            // dressed at once, as the original client does; the item query only for what the DBC lacks.
            const bool anyResolved = session_.equipmentOf(
                guid, equip.displayId, equip.invType, &unresolved,
                [](void* bank, uint32_t entry, uint32_t& displayId, uint8_t& invType) {
                    return static_cast<rendering::psp::ItemBank*>(bank)->itemDisplay(entry, displayId, invType);
                },
                &items_);
            fillRemoteWeaponVisuals(*e, equip);  // the weapons' sheath types, enchantment glows
            // that player's SHOW_HELM / SHOW_CLOAK (PLAYER_FLAGS_HIDE_HELM
            // 0x400 / _HIDE_CLOAK 0x800): the client adds no head / back item then (shownEquipment).
            const uint32_t playerFlags = e->getField(fFlags);
            equip.hideHelm = (playerFlags & 0x400u) != 0;
            equip.hideCloak = (playerFlags & 0x800u) != 0;
            {  // a guild tabard's emblem (PLAYER_GUILDID = UNIT_END + 3; the guild query when unknown)
                const uint16_t fEnd = game::fieldIndex(game::UF::UNIT_END);
                const auto worn = remotes_.find(guid);
                fillGuildEmblem(fEnd != 0xFFFF ? e->getField(static_cast<uint16_t>(fEnd + 3)) : 0u,
                                worn != remotes_.end() ? worn->second.equip : equip, equip);
            }
            RemotePlayer& r = remotes_[guid];
            // the raw fields of every look (a change of any of them re-dresses).
            bool entriesChanged = false, rawChanged = false;
            for (int s = 0; s < 19; ++s) {
                const uint32_t entry = e->getField(static_cast<uint16_t>(kVisibleItem + s * 2));
                const uint32_t enchant = e->getField(static_cast<uint16_t>(kVisibleItem + s * 2 + 1));
                entriesChanged |= entry != r.seenEntry[s];
                rawChanged |= entry != r.seenEntry[s] || enchant != r.seenEnchant[s];
                r.seenEntry[s] = entry;
                r.seenEnchant[s] = enchant;
            }
            const uint32_t flags = e->getField(fFlags) & 0xC00u;  // PLAYER_FLAGS_HIDE_HELM | PLAYER_FLAGS_HIDE_CLOAK
            rawChanged |= flags != r.seenFlags;
            r.seenFlags = flags;
            const glm::vec3 at = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
            r.dist = glm::length(at - glm::vec3(playerX_, playerY_, playerZ_));
            if (unresolved && !r.unresolvedSinceMs) r.unresolvedSinceMs = nowMs ? nowMs : 1u;
            if (!unresolved) r.unresolvedSinceMs = 0;
            const bool lookSame = std::memcmp(&r.look, &look, sizeof(look)) == 0;
            if (!r.model) {
                if (r.modelFailed && lookSame) continue;  // no model for this race / sex in characters.pak
                r.modelFailed = false;
                r.wantLook = look;
                r.wantEquip = equip;
                r.wantUnresolved = unresolved;
                r.dressWanted = true;  // a new player: its model (the items' models a frame later)
                if (unresolved) equipWaiting_ = true;  // dressed again when the answers are in
                continue;
            }
            if (!lookSame) {
                r.wantLook = look;
                r.wantEquip = equip;
                r.wantUnresolved = unresolved;
                r.dressWanted = true;  // the look changed: the model again
                continue;
            }
            if (!anyResolved && !rawChanged) continue;  // the queries are out, nothing to strip it to
            if (equip == r.equip && !rawChanged) {
                if (r.dressWanted && r.wantEquip == equip) r.dressWanted = false;  // back to what it wears
                continue;
            }
            // Answers to item queries only (the same entries, more of them resolved): one re-dress when all are
            // in, or 1 s after the first was waited for.
            if (!entriesChanged && !rawChanged && unresolved && nowMs - r.unresolvedSinceMs < 1000u) {
                equipWaiting_ = true;
                continue;
            }
            r.wantLook = look;
            r.wantEquip = equip;
            r.wantUnresolved = unresolved;
            r.dressWanted = true;
            if (unresolved) equipWaiting_ = true;
        }
        if (equipWaiting_) equipCheckMs_ = nowMs + 250u;
        for (auto it = remotes_.begin(); it != remotes_.end();)
            it = alive.count(it->first) ? std::next(it) : remotes_.erase(it);
    }
    remoteCapTurn(nowMs);
    dressRemoteQueue(nowMs);
    remoteCrowdTurn(nowMs);
}

// PSP: playermax - every 500 ms the other players are ranked by distance (one that has its model counts 5 yd nearer:
// no swapping at the edge) and the nearest remoteMax_ are in the cap. Out of it: not drawn, no new model; a model is
// released 3 s after its player left the cap and is read again when it comes back. On a crowded server (~25 players
// in a starting zone, 10-15) every model cost memory and GE time; the server cannot be asked to send fewer
// (layers / shards are the server's).
void WorldScreen::remoteCapTurn(uint32_t nowMs) {
    if (static_cast<int32_t>(nowMs - remoteCapNextMs_) < 0) return;
    remoteCapNextMs_ = nowMs + 500u;
    // Memory (hardware, 35 players of 9 races / sexes in a starting zone: the race blobs alone ~8 MB, the heap full and
    // the game ended): under 2 MB free the cap goes one lower a turn and the farthest model goes at once; from 6 MB
    // free it comes back one a turn, up to playermax.
    uint32_t models0 = 0;
    for (const auto& [g, r] : remotes_) models0 += r.model ? 1u : 0u;
    const uint32_t freeKb = pp::heapFreeKb();
    const bool squeeze = freeKb < 2048u;
    if (squeeze && remoteCapMem_ > 3u)
        remoteCapMem_ = std::min<uint32_t>(remoteCapMem_ - 1u, std::max<uint32_t>(3u, models0 > 0u ? models0 - 1u : 3u));
    else if (freeKb >= 6144u && remoteCapMem_ < remoteMax_) ++remoteCapMem_;
    if (remoteCapMem_ > remoteMax_) remoteCapMem_ = remoteMax_;
    remoteCapRank_.clear();
    for (auto& [guid, r] : remotes_) {
        r.dist = remoteDistance(guid, r);
        // a model held counts 5 yd nearer (no swapping at the edge), a race whose model is resident 8 yd (fewer
        // race models in memory)
        const bool resident = rendering::psp::CharacterModel::raceBlobResident(charPak_, r.wantLook, true);
        remoteCapRank_.emplace_back(r.dist - (r.model ? 5.0f : 0.0f) - (resident ? 8.0f : 0.0f), guid);
    }
    std::sort(remoteCapRank_.begin(), remoteCapRank_.end());
    uint32_t in = 0, out = 0, models = 0;
    for (size_t i = 0; i < remoteCapRank_.size(); ++i) {
        RemotePlayer& r = remotes_[remoteCapRank_[i].second];
        const bool inCap = i < remoteCapMem_;
        if (r.inCap && !inCap) r.outSinceMs = nowMs;
        r.inCap = inCap;
        in += inCap ? 1u : 0u;
        out += inCap ? 0u : 1u;
        if (!inCap && r.model && (squeeze || nowMs - r.outSinceMs >= 3000u)) {
            r.model.reset();
            r.mountReady = false;  // the mount comes back with the rider
            r.anim = ~0u;
            r.attachWanted = false;
            r.dressWanted = true;  // dressed again (r.wantLook / r.wantEquip) when it is back in the cap
            r.equip = rendering::psp::Equipment{};
            ++remoteCapUnloads_;
        }
        models += r.model ? 1u : 0u;
    }
    if (nowMs - remoteCapLogMs_ >= 10000u && !remotes_.empty()) {
        remoteCapLogMs_ = nowMs;
        // two lines: the log cuts a line past ~160 characters
        PSP_LOGI(pp::tag::Gfx, "remote_cap max=%u mem_cap=%u players=%u in=%u models=%u free_kb=%u",
                 static_cast<unsigned>(remoteMax_), static_cast<unsigned>(remoteCapMem_),
                 static_cast<unsigned>(remotes_.size()), static_cast<unsigned>(in), static_cast<unsigned>(models),
                 static_cast<unsigned>(freeKb));
        PSP_LOGI(pp::tag::Gfx, "remote_cap2 out=%u unloads=%u mem_waits=%u races=%u races_kb=%u",
                 static_cast<unsigned>(out), static_cast<unsigned>(remoteCapUnloads_),
                 static_cast<unsigned>(remoteMemWaits_),
                 static_cast<unsigned>(rendering::psp::CharacterModel::sharedBlobCount()),
                 static_cast<unsigned>(rendering::psp::CharacterModel::sharedBlobKb()));
    }
}

// yd from the player - the drawn position once it has one, else the entity's.
float WorldScreen::remoteDistance(uint64_t guid, const RemotePlayer& r) const {
    const glm::vec3 me(playerX_, playerY_, playerZ_);
    if (r.hasCache) return glm::length(r.pos - me);
    if (auto e = session_.entities().getEntity(guid))
        return glm::length(core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ())) - me);
    return r.dist;
}

// the dresses the scan asked for, nearest first. PSP: at most one new model a frame
// (its body: the race / sex model from characters.pak), re-dresses while the frame's budget lasts (at least one);
// a new player's item models come in a later frame (AttachDeferScope), nearest first - the crowd that walks in is
// spread over frames, not one long stop. Logs remote_player / remote_equip / remote_attach with ms= and dist=.
void WorldScreen::dressRemoteQueue(uint32_t nowMs) {
    dressJobs_.clear();
    for (auto& [guid, r] : remotes_)
        if (r.dressWanted || r.attachWanted) {
            r.dist = remoteDistance(guid, r);
            dressJobs_.emplace_back(r.dist, guid);
        }
    if (dressJobs_.empty()) return;
    PROF_SCOPE("remote.dress");
    std::sort(dressJobs_.begin(), dressJobs_.end());
    dressQueueMax_ = std::max(dressQueueMax_, static_cast<uint32_t>(dressJobs_.size()));
    constexpr uint64_t kBudgetUs = 6000;  // re-dresses after the first one this frame while under 6 ms
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    int loads = 0, done = 0;
    for (const auto& [dist, guid] : dressJobs_) {
        if (done > 0 && sceKernelGetSystemTimeWide() - t0 >= kBudgetUs) break;
        auto it = remotes_.find(guid);
        if (it == remotes_.end()) continue;
        RemotePlayer& r = it->second;
        const uint64_t j0 = sceKernelGetSystemTimeWide();
        const char* kind = nullptr;
        bool again = false;  // the model could not be read now (memory): the dress stays wanted
        if (r.dressWanted) {
            const bool newModel = !r.model || std::memcmp(&r.look, &r.wantLook, sizeof(r.look)) != 0;
            if (newModel) {
                if (!r.inCap) continue;   // playermax: not among the nearest - waits (dressWanted stays)
                if (loads > 0) continue;  // one model a frame
                // Memory: a race model not resident yet needs its blob + 4 MB left after it, any model 1.5 MB free
                // (its textures and bones); else it waits (not drawn) - no static white stand-in.
                {
                    namespace rp = rendering::psp;
                    const bool resident = rp::CharacterModel::raceBlobResident(charPak_, r.wantLook, true);
                    const uint32_t blob = resident ? 0u : rp::CharacterModel::raceBlobBytes(charPak_, r.wantLook, true);
                    if (resident ? pp::heapFreeKb() < 1536u : !pp::heapRoomFor(blob, 4096u)) {
                        ++remoteMemWaits_;
                        continue;
                    }
                }
                if (!charPak_.isOpen()) {
                    r.dressWanted = false;
                    continue;
                }
                ++loads;
                r.look = r.wantLook;
                r.equip = r.wantEquip;
                r.equipChangeMs = 0;
                r.model = std::make_unique<rendering::psp::CharacterModel>();
                // dressed in its sheath state from the start
                if (const auto ent = session_.entities().getEntity(guid)) initRemoteSheath(*r.model, *ent);
                {
                    rendering::psp::AttachDeferScope defer;  // the item models in a later frame
                    if (!r.model->load(charPak_, r.look, true, &r.equip, &items_)) r.model.reset();
                }
                if (r.model) r.model->setEmotePak(&charPak_);
                // not tried again only when characters.pak has no model for this race / sex (no memory: next time)
                r.modelFailed = !r.model && rendering::psp::CharacterModel::raceBlobBytes(charPak_, r.look, true) == 0;
                again = !r.model && !r.modelFailed;  // no memory: dressed again when there is room
                r.attachWanted = r.model != nullptr;
                kind = "player";
                if (++dressLogs_ <= 200)
                    PSP_LOGI(pp::tag::Gfx, "remote_player guid=%08X race=%u sex=%u ok=%d unresolved=%d ms=%u dist=%.0f",
                             static_cast<unsigned>(guid & 0xFFFFFFFF), r.look.race, r.look.sex, r.model ? 1 : 0,
                             r.wantUnresolved, static_cast<unsigned>((sceKernelGetSystemTimeWide() - j0) / 1000u),
                             static_cast<double>(dist));
            } else {
                // What changed shows at once: the body, and the item models (a new weapon's model is read now).
                r.model->setEquipment(charPak_, r.wantEquip, &items_);
                r.equip = r.wantEquip;
                r.equipChangeMs = 0;
                r.attachWanted = false;
                kind = "equip";
                if (++dressLogs_ <= 200)
                    PSP_LOGI(pp::tag::Gfx, "remote_equip guid=%08X unresolved=%d ms=%u dist=%.0f capped=%u",
                             static_cast<unsigned>(guid & 0xFFFFFFFF), r.wantUnresolved,
                             static_cast<unsigned>((sceKernelGetSystemTimeWide() - j0) / 1000u),
                             static_cast<double>(dist), static_cast<unsigned>(r.model->attachmentsCapped()));
            }
            r.dressWanted = again;
        } else if (r.attachWanted) {
            r.attachWanted = false;
            if (!r.model) continue;
            // The sheath code may have attached them already (setWeaponsSheathed re-attaches).
            if (r.model->attachmentCount() > 0 && r.model->attachmentsCapped() == 0) continue;
            r.model->refreshAttachments();
            kind = "attach";
            if (++dressLogs_ <= 200)
                PSP_LOGI(pp::tag::Gfx, "remote_attach guid=%08X n=%u capped=%u ms=%u dist=%.0f",
                         static_cast<unsigned>(guid & 0xFFFFFFFF), static_cast<unsigned>(r.model->attachmentCount()),
                         static_cast<unsigned>(r.model->attachmentsCapped()),
                         static_cast<unsigned>((sceKernelGetSystemTimeWide() - j0) / 1000u), static_cast<double>(dist));
        }
        if (kind) {
            ++done;
            ++dressCount_;
            dressMaxMs_ = std::max(dressMaxMs_, static_cast<uint32_t>((sceKernelGetSystemTimeWide() - j0) / 1000u));
        }
    }
    (void)nowMs;
}

// attachcap's crowd turn, every 250 ms while nothing waits in the dress queue. The
// nearest player whose item models the cap left off gets them when there is room; when there is none, the farthest
// player holding models (more than 5 yd farther) gives way first (AttachDeferScope: its models released - it counts
// as capped and gets them back when room returns). One give-way and one attach a turn. Every 10 s:
// remote_crowd players= with_models= capped= in_use_kb= cap_kb= turns= yields= dresses= dress_max_ms= queue_max=.
void WorldScreen::remoteCrowdTurn(uint32_t nowMs) {
    namespace rp = rendering::psp;
    if (nowMs - crowdLogMs_ >= 10000u) {
        crowdLogMs_ = nowMs;
        uint32_t players = 0, capped = 0, withModels = 0;
        for (auto& [guid, r] : remotes_) {
            if (!r.model) continue;
            ++players;
            if (r.model->attachmentsCapped()) ++capped;
            if (r.model->attachmentCount()) ++withModels;
        }
        // the NPCs in the same queue (npcs= with item models or left without, npc_capped=),
        // the give-ways to get under a lowered cap (sheds=) and the memory governor's cap (gov_cap_kb=, 0 none).
        uint32_t npcCapped = 0;
        for (const auto& n : crowdNpcs_) npcCapped += n.capped ? 1u : 0u;
        if (players || !crowdNpcs_.empty())
            PSP_LOGI(pp::tag::Gfx, "remote_crowd players=%u with_models=%u capped=%u in_use_kb=%u cap_kb=%u turns=%u "
                                   "yields=%u dresses=%u dress_max_ms=%u queue_max=%u npcs=%u npc_capped=%u "
                                   "npc_turns=%u npc_yields=%u sheds=%u gov_cap_kb=%u",
                     static_cast<unsigned>(players), static_cast<unsigned>(withModels), static_cast<unsigned>(capped),
                     static_cast<unsigned>(rp::attachmentInUseBytes() / 1024u),
                     static_cast<unsigned>(rp::attachmentCapBytes() / 1024u),
                     static_cast<unsigned>(crowdTurns_), static_cast<unsigned>(crowdYields_),
                     static_cast<unsigned>(dressCount_), static_cast<unsigned>(dressMaxMs_),
                     static_cast<unsigned>(dressQueueMax_), static_cast<unsigned>(crowdNpcs_.size()),
                     static_cast<unsigned>(npcCapped), static_cast<unsigned>(crowdNpcTurns_),
                     static_cast<unsigned>(crowdNpcYields_), static_cast<unsigned>(crowdSheds_),
                     static_cast<unsigned>(rp::attachmentGovernorCap() / 1024u));
        dressMaxMs_ = 0;
        dressQueueMax_ = 0;
    }
    if (static_cast<int32_t>(nowMs - crowdNextMs_) < 0 || !dressJobs_.empty()) return;
    crowdNextMs_ = nowMs + 250u;
    if (!rp::attachmentsEnabled()) return;
    // one queue - the players and the NPCs (psp_attach_crowd.hpp).
    // looked at the players only: a player 5 yd away stayed bare while NPCs 20 yd away held the cap. The nearest one
    // waiting gets its models (an NPC counts crowdnpcbias yd farther: near players before far NPCs); full, the
    // farthest holder - player or NPC - more than 5 yd farther gives way; over the cap (the memory governor lowered it)
    // the farthest gives way though nobody waits.
    constexpr float kHysteresisYd = 5.0f;
    if (crowdNpcBiasYd_ < 0.0f)
        crowdNpcBiasYd_ = static_cast<float>(std::clamp(pp::configGetInt("crowdnpcbias", 10), 0, 200));
    crowdHolders_.clear();
    for (auto& [guid, r] : remotes_) {
        if (!r.model) continue;
        r.dist = remoteDistance(guid, r);
        rp::CrowdHolder h;
        h.guid = guid;
        h.npc = false;
        h.dist = r.dist;
        h.count = static_cast<uint32_t>(r.model->attachmentCount());
        h.capped = r.model->attachmentsCapped();
        h.busy = r.dressWanted || r.attachWanted;
        crowdHolders_.push_back(h);
    }
    creatures_.attachHolders(playerX_, playerY_, playerZ_, crowdNpcs_);
    for (const auto& n : crowdNpcs_) {
        rp::CrowdHolder h;
        h.guid = n.guid;
        h.npc = true;
        h.dist = n.dist;
        h.count = n.count;
        h.capped = n.capped;
        crowdHolders_.push_back(h);
    }
    const uint32_t cap = rp::attachmentCapBytes();
    const rp::CrowdPick pick = rp::crowdTurnPick(crowdHolders_.data(), crowdHolders_.size(), rp::attachmentInUseBytes(),
                                                 cap, crowdNpcBiasYd_, kHysteresisYd);
    if (pick.yield >= 0) {
        const rp::CrowdHolder far = crowdHolders_[static_cast<size_t>(pick.yield)];
        const uint32_t before = rp::attachmentInUseBytes();
        bool gave = false;
        if (far.npc) {
            gave = creatures_.attachYield(far.guid);
        } else if (auto it = remotes_.find(far.guid); it != remotes_.end() && it->second.model) {
            rp::AttachDeferScope giveWay;
            it->second.model->refreshAttachments();
            gave = true;
        }
        if (gave) {
            ++crowdYields_;
            if (far.npc) ++crowdNpcYields_;
            if (pick.shed) ++crowdSheds_;
            const rp::CrowdHolder* need = pick.need >= 0 ? &crowdHolders_[static_cast<size_t>(pick.need)] : nullptr;
            if (++dressLogs_ <= 200)
                PSP_LOGI(pp::tag::Gfx, "remote_crowd_yield guid=%08X dist=%.0f npc=%d for=%08X dist=%.0f npc=%d shed=%d "
                         "in_use_kb=%u->%u cap_kb=%u",
                         static_cast<unsigned>(far.guid & 0xFFFFFFFF), static_cast<double>(far.dist), far.npc ? 1 : 0,
                         need ? static_cast<unsigned>(need->guid & 0xFFFFFFFF) : 0u,
                         need ? static_cast<double>(need->dist) : 0.0, need && need->npc ? 1 : 0, pick.shed ? 1 : 0,
                         static_cast<unsigned>(before / 1024u),
                         static_cast<unsigned>(rp::attachmentInUseBytes() / 1024u), static_cast<unsigned>(cap / 1024u));
        }
        if (rp::attachmentInUseBytes() >= cap) return;  // not enough yet: the next turn
    }
    if (pick.need < 0 || rp::attachmentInUseBytes() >= cap) return;
    const rp::CrowdHolder need = crowdHolders_[static_cast<size_t>(pick.need)];
    if (need.npc) {
        // The NPC is dressed again at the next update (CreatureRenderer::updateEquipment, one a frame).
        if (creatures_.attachRetry(need.guid)) {
            ++crowdTurns_;
            ++crowdNpcTurns_;
            if (++dressLogs_ <= 200)
                PSP_LOGI(pp::tag::Gfx, "remote_crowd_turn guid=%08X dist=%.0f npc=1 capped=%u in_use_kb=%u",
                         static_cast<unsigned>(need.guid & 0xFFFFFFFF), static_cast<double>(need.dist),
                         static_cast<unsigned>(need.capped), static_cast<unsigned>(rp::attachmentInUseBytes() / 1024u));
        }
        return;
    }
    auto it = remotes_.find(need.guid);
    if (it == remotes_.end() || !it->second.model) return;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    it->second.model->refreshAttachments();
    ++crowdTurns_;
    if (++dressLogs_ <= 200)
        PSP_LOGI(pp::tag::Gfx, "remote_crowd_turn guid=%08X dist=%.0f npc=0 n=%u capped=%u ms=%u in_use_kb=%u",
                 static_cast<unsigned>(need.guid & 0xFFFFFFFF), static_cast<double>(need.dist),
                 static_cast<unsigned>(it->second.model->attachmentCount()),
                 static_cast<unsigned>(it->second.model->attachmentsCapped()),
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u),
                 static_cast<unsigned>(rp::attachmentInUseBytes() / 1024u));
}

// Upstream Application::update, "other players" loop: the position during
// the dead-reckoning overrun is the destination (no drift + backward snap),
// a jump over 6 yd (or 3 yd up/down) is set at once, smaller changes start a
// CharacterRenderer::moveInstanceTo of dist / 5.5 s (0.05..0.22). Animation
// changes only on a moving/walking transition (Run / Walk / Stand); an
// MSG_MOVE_JUMP plays Jump (upstream unitAnimHintCallback, anim 38).
// Mounts - split in two: updateRemotePlayers (positions and
// animations, before the creatures are drawn - a mount is a creature unit
// placed at its rider) and drawRemotePlayers (the models, as before).
void WorldScreen::updateRemotePlayers() {
    PROF_SCOPE("remote");
    const uint16_t fBytes1 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1);
    remoteMountLoadedThisFrame_ = false;  // mounts - one mount model load per frame (upstream)
    for (auto& [guid, r] : remotes_) {
        r.visible = false;
        auto e = session_.entities().getEntity(guid);
        if (!e || !r.model) continue;
        const bool inOverrun = e->isEntityMoving() && !e->isActivelyMoving();
        const glm::vec3 canonical(inOverrun ? e->getLatestX() : e->getX(), inOverrun ? e->getLatestY() : e->getY(),
                                  inOverrun ? e->getLatestZ() : e->getZ());
        glm::vec3 target = core::coords::canonicalToServer(canonical);
        bool isMovingNow = e->isActivelyMoving();
        // PSP: over Wi-Fi the packets bunch up and the interval interpolation
        // stops and starts; predict from the last packet instead (see
        // WorldSession::predictRemote), feet on our floor. wowee.cfg
        // remotepredict=0 = upstream interpolation only.
        glm::vec3 predicted;
        float predYaw = 0.0f;
        bool predMoving = false, predFalling = false;
        const bool usePrediction =
            remotePredict_ && session_.predictRemote(guid, predicted, predYaw, predMoving, predFalling);
        if (usePrediction) {
            const float ground = floorAt(predicted.x, predicted.y, predicted.z + 1.5f);
            if (ground > -1e29f && (predFalling ? predicted.z < ground : std::fabs(ground - predicted.z) < 2.0f) &&
                !flightRemoteKeepsZ(guid))  // a flier / swimmer keeps its height
                predicted.z = ground;
            target = predicted;
        }
        if (!r.hasCache) {
            r.pos = target;
            r.cache = target;
            r.hasCache = true;
        } else {
            const float dx = target.x - r.cache.x, dy = target.y - r.cache.y;
            const float planarSq = dx * dx + dy * dy;
            const float dz = std::fabs(target.z - r.cache.z);
            const bool posChanging = planarSq > 0.03f * 0.03f || dz > 0.08f;
            isMovingNow = usePrediction ? predMoving : (isMovingNow || (posChanging && !e->isEntityMoving()));
            if (planarSq > 36.0f || dz > 3.0f) {
                r.pos = target;
                r.moveActive = false;
            } else if (posChanging) {
                r.moveStart = r.pos;
                r.moveEnd = target;
                r.moveDuration = std::clamp(std::sqrt(planarSq) / 5.5f, 0.05f, 0.22f);
                r.moveElapsed = 0.0f;
                r.moveActive = true;
            }
            r.cache = target;
        }
        if (r.moveActive) {  // CharacterRenderer::update, "Interpolate creature movement"
            r.moveElapsed += lastDt_;
            const float t = r.moveElapsed / r.moveDuration;
            if (t >= 1.0f) {
                r.pos = r.moveEnd;
                r.moveActive = false;
            } else {
                r.pos = glm::mix(r.moveStart, r.moveEnd, t);
            }
        }
        // on a transport - where it carries the player this frame (no easing behind a ship),
        // moving by its own flags (not by the transport's motion).
        if (transportRemotePos(guid, target, isMovingNow)) {
            r.pos = r.cache = target;
            r.moveActive = false;
        }
        r.yaw = usePrediction ? predYaw : core::coords::canonicalToServerYaw(e->getOrientation());  // orientation sync
        const bool walking = session_.isWalking(guid);
        // PSP: npc-walk - backpedalling (BACKWARD without FORWARD) is WalkBackwards (13) in the original client, walking
        // or not; upstream runs forward. wowee.cfg remoteback=0 = upstream.
        static const int remoteBack = pp::configGetInt("remoteback", 1);
        const uint32_t remoteFlags = remoteBack ? session_.remoteMoveFlags(guid) : 0u;
        const bool backward = (remoteFlags & static_cast<uint32_t>(game::MovementFlags::BACKWARD)) != 0 &&
                              (remoteFlags & static_cast<uint32_t>(game::MovementFlags::FORWARD)) == 0;
        auto gaitAnim = [&]() -> uint32_t {
            if (backward && r.model->hasAnimation(rendering::anim::WALK_BACKWARDS)) return rendering::anim::WALK_BACKWARDS;
            return walking ? rendering::anim::WALK : rendering::anim::RUN;
        };
        // Mounts - upstream player sync: a rider keeps Mount, the
        // locomotion goes to the mount (a creature unit, appendMountUnits).
        r.movingNow = isMovingNow;
        r.walkingNow = walking;
        updateRemoteMount(guid, r, *e);
        if (r.mountReady) {
            r.wasMoving = isMovingNow;
            r.wasWalking = walking;
            r.wasBackward = backward;  // PSP: npc-walk
            r.airborne = false;
            r.sittingDown = false;
            session_.takeJumpHint(guid);
            uint32_t id = 0;
            float t = 0.0f, dur = 0.0f;
            // (A model without MOUNT, an old characters.pak, plays its fallback once, not every frame.)
            const bool lost = !r.model->getAnimationState(id, t, dur) ||
                              (id != rendering::anim::MOUNT && r.model->hasAnimation(rendering::anim::MOUNT));
            if (r.anim != rendering::anim::MOUNT || lost) {
                r.model->playAnimation(rendering::anim::MOUNT, true);
                r.anim = rendering::anim::MOUNT;
            }
            r.model->update(lastDt_);
            continue;
        }
        if (flightRemoteAnim(guid, r, isMovingNow)) continue;  // swimming: the swim set, flying: Fly / Hover
        // Landing: upstream only switches on moving/walking transitions, so a
        // jump on the spot kept looping Jump until the player walked. Land
        // like our own LocomotionFSM: JUMP_END when standing (the model goes
        // back to Stand when the one-shot ends), else straight to Run/Walk.
        const bool landed = r.airborne && usePrediction && !predFalling;
        if (landed) {
            r.airborne = false;
            r.wasMoving = isMovingNow;
            r.wasWalking = walking;
            r.wasBackward = backward;  // PSP: npc-walk
            const uint32_t want = isMovingNow ? gaitAnim() : rendering::anim::JUMP_END;
            r.model->playAnimation(want, isMovingNow);
            r.anim = want;
        } else if (!r.airborne && (isMovingNow != r.wasMoving ||
                                   ((walking != r.wasWalking || backward != r.wasBackward) && isMovingNow))) {
            r.wasMoving = isMovingNow;
            r.wasWalking = walking;
            r.wasBackward = backward;  // PSP: npc-walk
            const uint32_t want = isMovingNow ? gaitAnim() : rendering::anim::STAND;
            r.model->playAnimation(want, true);
            r.anim = want;
        }
        // Sitting (UNIT_FIELD_BYTES_1 byte 0): sit down -> sitting loop,
        // sit up on standing. Upstream does not animate other units' stand
        // state; the PSP shows the PC player sitting (and AFK).
        const uint8_t stand = static_cast<uint8_t>(e->getField(fBytes1) & 0xFF);
        if (stand != r.standState) {
            if (stand == 1 && !isMovingNow) {
                r.model->playAnimation(rendering::anim::SIT_GROUND_DOWN, false);
                r.anim = rendering::anim::SIT_GROUND_DOWN;
                r.sittingDown = true;
            } else if (r.standState == 1 && stand == 0) {
                r.sittingDown = false;
                if (!isMovingNow) {
                    r.model->playAnimation(rendering::anim::SIT_GROUND_UP, false);
                    r.anim = rendering::anim::SIT_GROUND_UP;
                }
            }
            r.standState = stand;
        }
        if (r.sittingDown) {
            uint32_t id = 0;
            float t = 0.0f, dur = 0.0f;
            // Into the loop before the one-shot ends (the model would fall back to Stand for a frame).
            if (!r.model->getAnimationState(id, t, dur) || id != rendering::anim::SIT_GROUND_DOWN ||
                t + lastDt_ * 1000.0f >= dur) {
                r.model->playAnimation(rendering::anim::SITTING, true);
                r.anim = rendering::anim::SITTING;
                r.sittingDown = false;
            }
        }
        if (session_.takeJumpHint(guid)) {
            r.model->playAnimation(rendering::anim::JUMP, true);
            r.anim = rendering::anim::JUMP;
            r.airborne = usePrediction;  // landing is seen only through the prediction's FALLING flag
        }
        r.model->update(lastDt_);
    }
}

void WorldScreen::drawRemotePlayers(const rendering::psp::TerrainCamera& cam) {
    PROF_SCOPE("remote.draw");
    for (auto& [guid, r] : remotes_) {
        r.visible = false;
        if (!r.model || !session_.entities().getEntity(guid)) continue;
        const glm::vec3 d = r.pos - cam.pos;
        if (glm::dot(d, d) > remoteView_ * remoteView_) continue;  // was 120 yd, now wowee.cfg playerview
        if (!r.inCap) continue;  // playermax: only the nearest N are drawn
        r.visible = true;
        if (classBarRemoteHidden(guid)) continue;  // drawn as its form's creature (appendClassBarUnits)
        // Mounts - upstream: the rider at the mount's position + riderHeight.
        const glm::vec3 at = r.mountReady ? r.pos + glm::vec3(0.0f, 0.0f, r.riderHeight) : r.pos;
        r.model->draw(cam, at, r.yaw, kSkyABGR);
    }
}

void WorldScreen::update(const pp::PadState& keyPad, float dtSec) {
    PROF_SCOPE("world.update");
    // breadcrumbs for the hardware crash at world entry
    const bool firstUpdate = firstUpdates_ == 0;
    if (firstUpdates_ < 3) PSP_LOGI(pp::tag::Loop, "world_update n=%d", firstUpdates_++);
    if (firstUpdate) pp::logMemory("world_update0");
    // The memory watch every 10 s, named by the terrain tile the player stands on.
    memWatchSec_ += dtSec;
    if (memWatchSec_ >= 10.0f) {
        memWatchSec_ = 0.0f;
        char at[32];
        std::snprintf(at, sizeof(at), "tile_%d_%d", static_cast<int>(std::floor(32.0f - playerY_ / 533.33333f)),
                      static_cast<int>(std::floor(32.0f - playerX_ / 533.33333f)));
        pp::memWatchTick(at);
    }
    const pp::PadState rawPad = chat_.filterPad(keyPad);  // chat - the keyboard owns the pad
    {
        PROF_SCOPE("net");
        session_.update();
        session_.updateEntities(dtSec);
    }
    // SMSG_NEW_WORLD to another map was just acknowledged - the LoginScreen destroys this world
    // right after this update and builds the next map's; nothing of it runs any more (movement would tell the
    // server the old map's place).
    if (session_.transferStage() == pp::WorldSession::TransferStage::Ready) return;
    chat_.update(dtSec);  // chat
    if (chat_.takeSent() && uiMode_ && !settings_.isOpen()) setUiMode(false);  // sent -> back to the game
    // SELECT: game <-> interface (cursor + windows). PSP: no START+SELECT exit.
    // Buttons held across a mode change stay dead until released.
    suppress_ &= rawPad.held;
    pp::PadState armed = rawPad;
    armed.held &= ~suppress_;
    armed.pressed &= ~suppress_;
    armed.released &= ~suppress_;
    const bool wasUi = uiMode_;
    const bool wasSettings = settings_.isOpen();
    const bool selectKey = (rawPad.pressed & PSP_CTRL_SELECT);
    if (settings_.isOpen()) {
        // Graphics settings (Menu -> Graphics, cloud task 1): its own rows and
        // cursor; the world keeps running without input, the AFK timer stops.
        // Interface rule: Circle = back (to the windows), SELECT = to the game.
        PROF_SCOPE("settings");
        if (selectKey) {
            settings_.close();
            setUiMode(false);
        } else if (armed.pressed & PSP_CTRL_CIRCLE) {
            settings_.close();
        } else {
            pp::PadState p = armed;
            p.held &= ~PSP_CTRL_CIRCLE;
            p.pressed &= ~PSP_CTRL_CIRCLE;
            p.released &= ~PSP_CTRL_CIRCLE;
            if (settings_.update(p, dtSec)) applySettings();
        }
        idleSec_ = 0.0f;
    } else if (selectKey) {
        setUiMode(!uiMode_);
    } else if (uiMode_) {
        updateWindows(armed, dtSec);
    }
    if (uiMode_ != wasUi || settings_.isOpen() != wasSettings) {
        suppress_ |= rawPad.held & ~PSP_CTRL_SELECT;
        armed.held &= ~suppress_;  // this very frame too
        armed.pressed &= ~suppress_;
        armed.released &= ~suppress_;
    }
    // While the interface has the controls the game sees an idle pad
    // (physics, network and animation keep running - source-psp lesson).
    static const pp::PadState kIdle{};
    const pp::PadState& gamePad = (uiMode_ || settings_.isOpen()) ? kIdle : armed;
    // Controls with modes (controls=2, ui_world_controls.cpp): acts on the pad
    // and hands the handlers below what is left; controls=1 = the pad as is.
    const pp::PadState& pad = updateControls(gamePad, dtSec);
    // Orbit camera: nub turns and tilts (the mouse drag), D-pad up/down zoom
    // (the wheel). L / R are the left / right mouse buttons (updateMovement).
    lastDt_ = dtSec;
    // The stick is a rate, not a 1:1 drag like upstream's mouse: ease the turn
    // rate in and out (max 180 deg/s, upstream WOW_TURN_SPEED) so starting and
    // stopping a turn is not a jolt. rotating_ (instant pull-in) stays off -
    // upstream snaps only while the mouse drags the view.
    rotating_ = false;
    const float ease = 1.0f - std::exp(-dtSec / 0.10f);
    yawVel_ += (-pad.ax * 3.14159f - yawVel_) * ease;
    pitchVel_ += (pad.ay * 1.5708f - pitchVel_) * ease;  // public CVar cameraPitchMoveSpeed 90 deg/s (was 1.4)
    camYaw_ += yawVel_ * dtSec;
    pitch_ += pitchVel_ * dtSec;
    if (pitch_ < flightCamMinPitch()) pitch_ = flightCamMinPitch();  // under the player in flight (was -0.2)
    // 88 deg over the player at most: upstream CameraController MAX_PITCH 88 (camera_controller.hpp); was 89.
    if (pitch_ > kCamMaxPitch) pitch_ = kCamMaxPitch;  // -0.2 stays (ground)
    // Zoom like upstream CameraController: 0.5 (first person) .. 22 yd,
    // eased (ZOOM_SMOOTH_SPEED 15). Zooming in cancels the collision debt.
    const bool lHeld = (pad.held & PSP_CTRL_LTRIGGER) != 0;
    const bool rHeld = (pad.held & PSP_CTRL_RTRIGGER) != 0;
    if (!lHeld && !rHeld && (pad.held & PSP_CTRL_UP)) {
        distance_ -= kZoomSpeed * dtSec;
        zoomDebt_ = 0.0f;
    }
    if (!lHeld && !rHeld && (pad.held & PSP_CTRL_DOWN)) {
        // Upstream processMouseWheel: the player's own zoom, in or out, clears the debt.
        distance_ += kZoomSpeed * dtSec;
        zoomDebt_ = 0.0f;
    }
    // Emotes (M9c): R + D-pad, EmotesText.dbc ids (the server plays them for
    // everyone, SMSG_EMOTE comes back for us too).
    if (rHeld && !lHeld) {
        uint32_t te = 0;
        if (pad.pressed & PSP_CTRL_UP) te = 101;    // WAVE
        if (pad.pressed & PSP_CTRL_RIGHT) te = 21;  // CHEER
        if (pad.pressed & PSP_CTRL_DOWN) te = 34;   // DANCE (a state emote)
        if (pad.pressed & PSP_CTRL_LEFT) te = 17;   // BOW
        if (te) {
            if (sitting_) setSitting(false, false);
            session_.sendTextEmote(te);
            PSP_LOGI(pp::tag::Net, "text_emote id=%u", static_cast<unsigned>(te));
        }
    }
    distance_ = std::clamp(distance_, 0.5f, camDistMax_);  // cameraDistanceMax (was 22)
    currentDist_ += (distance_ - currentDist_) * std::min(1.0f, 15.0f * dtSec);
    updateCombat(pad, dtSec);  // combat - target, melee, death (dead: no movement)
    updateSpells(pad, dtSec);  // spells: L + face buttons, cast bar, auras (ui_world_spells.cpp)
    updateClassBars(dtSec);  // the bars' state and art, the model swap, the vehicle / the mover
    updateLoot(dtSec);  // loot - loot window, lootable corpses
    updateTaxi(dtSec);  // TaxiFrame <-> the session, the flight (before the NPC windows: the gossip goes)
    updateNpc(pad, dtSec);  // npc - L + D-pad up talks to the target, NPC windows
    updateMount(dtSec);  // mounts (ui_world_mount.cpp): the own mount field, speeds
    updateTransports(dtSec);  // the transports' places this frame (before we move with them)
    updateSitting(spellPad(combatPad(pad)), dtSec);
    updateMovement(spellPad(combatPad(pad)), dtSec);
    // area triggers (instance portals, the tram, rest areas) - upstream GameHandler::update.
    session_.updateAreaTriggers(dtSec, glm::vec3(playerX_, playerY_, playerZ_));
    updateInstancePopups();  // INSTANCE_BOOT / INSTANCE_LOCK
    updateAnimation(dtSec);
    updateAudio(dtSec);
    updateLighting(dtSec);  // lighting
    if (firstUpdate) pp::logMemory("before_streaming");  // the hardware crash is in here
    updateStreaming(dtSec);  // streaming
    updateBigWmos(dtSec);    // big WMOs (reach, reads, doodad set)
    updateGameObjects();  // the game objects' models (ui_world_gameobjects.cpp)
    updateGameObjectUse(dtSec);  // the hover, the session's object events, ItemTextFrame
    updateParticles(dtSec);  // the emitters' game objects, the weather (ui_world_particles.cpp)
    // The card driver's few descriptors (HW 110: 0x80020320, the mail window without its art) - the
    // characters pak (other players, own equipment, emotes) gives its back after 5 s unused.
    charPak_.releaseIfIdle(pp::logUptimeMs(), 5000u);
    items_.releaseIfIdle(pp::logUptimeMs(), 5000u);  // items.pak - dressing reads only
    updateMemoryGovernor(dtSec);  // the heap probe, the cuts and their return, descriptors
    if (firstUpdate) pp::logMemory("before_inventory");
    updateInventory(dtSec);  // inventory (own equipment on the model, events)
    if (firstUpdate) pp::logMemory("before_framexml");
    updateFrameXml(dtSec);   // framexml
    if (firstUpdate) pp::logMemory("update0_done");
    updateTrainer(dtSec);  // trainers + talents (windows follow the session)
    updateTradeSkill(dtSec);  // TradeSkillFrame follows the session's crafting window
    updateTutorial(dtSec);   // tutorials (TUTORIAL_TRIGGER -> TutorialFrame)
}

void WorldScreen::draw(UiRenderer& ui, const FrameStats& stats) {
    if (firstDraws_ < 3) PSP_LOGI(pp::tag::Loop, "world_draw n=%d", firstDraws_++);  // breadcrumbs
    // this GE pass was "sky", but a pass lasts from its signal to the
    // next one, and the camera below is CPU (collision rays, floor, terrain heights) while the GE
    // waits - hardware 94..101 showed that wait as "sky" 6.2 ms. Now "cam"; "sky" starts at the sky
    // itself (drawLightingSky).
    PROF_GE_PASS("cam");
    // Sky.
    if (!lightingSkyOn() && !light_.skyFastFrame) {  // lighting - else the band gradient (drawLightingSky); skyfast clears in this colour there
        ui.beginFrame();
        ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)}, kSkyABGR);
        ui.endFrame();
    }

    // Camera behind the character (server orientation: 0 = north (+x),
    // counter-clockwise towards +y).
    // Pivot at the head: upstream pivots on the head bone; the baked stand
    // pose has no bones, so ~0.9 of the model height (1.6 fallback).
    // Steps and stairs: the physics height snaps to each tread, the drawn
    // height and the camera pivot follow it eased (upstream eases eyeHeight),
    // so climbing reads as a slope. Falls and jumps are followed as they are.
    // profiler sub-zone - the camera (collision rays, terrain
    // march, floor under the camera) was part of "draw"'s own time.
    static const uint16_t kDrawCamZone = pp::prof::zoneId("draw.cam");
    pp::prof::zoneEnter(kDrawCamZone);
    const float stepGap = playerZ_ - shownZ_;
    if (!camInit_ || airborne_ || taxiLocksControls() || flightCamFree() || std::fabs(stepGap) > 1.5f) {  // no easing in flight
        shownZ_ = playerZ_;
    } else {
        shownZ_ += stepGap * (1.0f - std::exp(-lastDt_ / 0.07f));
    }
    const float charHeight = player_.height();
    // Mounts - upstream pivot + mountHeightOffset_ while mounted.
    const glm::vec3 target(playerX_, playerY_,
                           shownZ_ + (charHeight > 0.5f ? charHeight * 0.9f : 1.6f) +
                               (mountActive_ ? mountHeightOffset_ : 0.0f));
    glm::vec3 back(-std::cos(camYaw_) * std::cos(pitch_), -std::sin(camYaw_) * std::cos(pitch_),
                   std::sin(pitch_));
    rendering::psp::TerrainCamera cam;
    cam.fovyDeg = camFovY_;  // cfg fov x 0.6 (the rule awaits observation: camera_fov)
    shownDist_ = cameraDistance(target, back);
    // First person below MIN_DISTANCE + 0.1 (upstream): the eye at the pivot,
    // a little forward so the head does not clip.
    if (shownDist_ < 0.6f) {
        cam.pos = target - back * 0.1f;
    } else {
        cam.pos = target + back * shownDist_;
    }
    // Upstream: smooth the camera position (camSmoothSpeed 30) - the pivot,
    // the collision limit and the orbit all arrive eased instead of jumping -
    // then keep it above the floor under it (terrain or a building floor no
    // higher than the camera) and above the feet. Clearance 1.1: our near
    // plane is 1.0 (upstream 0.35 with near 0.5). First person follows 1:1.
    if (!camInit_ || shownDist_ < 0.6f) {
        smoothedCam_ = cam.pos;
        camInit_ = true;
    } else {
        smoothedCam_ += (cam.pos - smoothedCam_) * (1.0f - std::exp(-30.0f * lastDt_));
    }
    cam.pos = smoothedCam_;
    if (shownDist_ >= 0.6f) {
        const float floor = floorAt(cam.pos.x, cam.pos.y, cam.pos.z);
        if (floor > -1e29f && cam.pos.z < floor + 1.1f) cam.pos.z = floor + 1.1f;
        if (!flightCamFree()) cam.pos.z = std::max(cam.pos.z, playerZ_ + 0.15f);  // may look up from below
    }
    cam.forward = shownDist_ < 0.6f ? -back : glm::normalize(target - cam.pos);
    pp::prof::zoneLeave(kDrawCamZone);
    // Fill test (SPP_EXP idea): same vertices, one pixel of fill. If the
    // frame time collapses the GE is fill/texture bound, not vertex bound.
    if (fillTest_) sceGuScissor(0, 0, 1, 1);
    drawLightingSky(cam, kSkyABGR);  // lighting (sky gradient, no-op when off); GE pass "sky"
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    terrain_.draw(cam, kSkyABGR);
    terrainCpuUs_ = static_cast<uint32_t>(sceKernelGetSystemTimeWide() - t0);
    {
        PROF_SCOPE("draw.target");  // profiler sub-zone (floor under the target)
        drawSelectionCircle(cam);  // on the ground, under the models drawn next
    }
    if (modelsOn_) {
        const uint64_t m0 = sceKernelGetSystemTimeWide();
        drawBigWmos(cam, kSkyABGR);  // before the M2 pass (its doodads take the portals' groups)
        models_.draw(cam, kSkyABGR);
        drawGameObjects(cam, kSkyABGR);  // the game objects' models
        drawTransports(cam, kSkyABGR);   // WMO objects, lifts, ships, zeppelins
        drawGameObjectHighlight(cam);    // the hovered object's highlight
        modelsCpuUs_ = static_cast<uint32_t>(sceKernelGetSystemTimeWide() - m0);
    } else {
        modelsCpuUs_ = 0;
    }
    // Mounts - other players move before the creatures are drawn:
    // a mount is a creature unit and must stand where its rider is this frame.
    {
        PROF_SCOPE("draw.sync");  // profiler sub-zone (units, NPC floors)
        syncRemotePlayers();
        processEmotes();
        updateRemotePlayers();
        syncCreatures();
    }
    if (creaturesOn_) creatures_.draw(cam, kSkyABGR);  // M10b hook: NPCs/creatures (no-op without data/creatures.pak)
    drawTargetOutline(cam);  // the selected creature's rim
    drawGameObjectPost(cam);  // the objects' one-shot animations, the hovered one's rim

    rendering::psp::attachmentNextFrame();  // the glowing weapons drawn from here on are this frame's
    if (player_.loaded()) {
        // The original client fades the character as the camera closes in and hides it in
        // first person (upstream hides at MIN_DISTANCE + 0.15).
        float fade = std::clamp((shownDist_ - 0.65f) / (1.6f - 0.65f), 0.0f, 1.0f);
        // The ghost at half opacity (upstream GameScreen setInstanceOpacity 0.5).
        if (session_.life() == pp::WorldSession::Life::Ghost) fade *= 0.5f;
        // On the mount's seat; a form's creature instead (appendClassBarUnits), a vehicle seat
        if (fade > 0.02f && !classBarHidesPlayer())
            player_.draw(cam, classBarRiderPos(), classBarRiderYaw(), kSkyABGR, fade);
    } else {
        // No baked model: a two-colour pyramid at the login position.
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        ScePspFVector3 t = {playerX_ - cam.pos.x, playerY_ - cam.pos.y, playerZ_ - cam.pos.z};
        sceGumTranslate(&t);
        sceGumRotateZ(facing_);
        auto* v = static_cast<MarkerVert*>(sceGuGetMemory(12 * sizeof(MarkerVert)));
        const MarkerVert top{0xFF20D0FFu, 0, 0, 2.2f}, n{0xFF2040FFu, 0.9f, 0, 0}, w{0xFF208020u, 0, 0.6f, 0},
            s{0xFF208020u, -0.6f, 0, 0}, e{0xFF208020u, 0, -0.6f, 0};
        const MarkerVert tris[12] = {top, n, w, top, w, s, top, s, e, top, e, n};
        for (int i = 0; i < 12; ++i) v[i] = tris[i];
        sceGuDisable(GU_TEXTURE_2D);
        sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 12, nullptr, v);
    }

    drawRemotePlayers(cam);  // mounts - sync / emotes / movement moved before the creatures
    drawWeaponFx(cam);  // the glows on the weapons just drawn (no-op at enchantfx=0)
    drawParticles(cam);  // the world's emitters and ribbons, blended (no-op at particles=0)
    drawSpellFx(cam);  // spell effects, blended over every 3D draw (no-op at spellfx=0)
    drawWeather(cam);  // rain / snow / sand (no-op at weather=0 or a fine sky)
    // the post pass before the render targets below - they clear the depth buffer's corner (up to
    // 128 x 128) and lighttint=2's multiply tests the frame's depth (cleared = sky = untinted): the top-left
    // square of the world was left untinted while a portrait / the character or inspect window was drawn.
    drawLightingPost();  // lighting (screen tint / glow, GE pass "glow") - between the 3D and the HUD
    {
        PROF_SCOPE("draw.portrait");  // profiler sub-zone
        renderPortrait(cam);  // after every 3D draw (it reuses the depth buffer's corner)
        renderPaperDollModel(cam);  // the character window's model (the same corner, larger)
        renderInspectModel(cam);  // InspectFrame's model (its own target below it)
        renderPetPortrait(cam);  // PetPortrait (its own 64 x 64 target, redrawn when the pet changes / every 2 s)
    }
    {
        const float aspect = static_cast<float>(pp::kScreenW) / pp::kScreenH;
        viewProj_ = glm::perspective(glm::radians(cam.fovyDeg), aspect, cam.nearZ, cam.farZ) *
                    glm::lookAt(glm::vec3(0.0f), cam.forward, cam.up);
        camPosForHud_ = cam.pos;
    }
    if (fillTest_) sceGuScissor(0, 0, pp::kScreenW, pp::kScreenH);
    const auto& st = terrain_.stats();
    const auto& ms = models_.stats();
    // One hardware sample every 10 s (not per frame): the numbers PPSSPP cannot give.
    const uint32_t nowMs = pp::logUptimeMs();
    if (nowMs >= nextStatsMs_) {
        nextStatsMs_ = nowMs + 10000;
        PSP_LOGI(pp::tag::Gfx, "world_frame fps=%.1f frame_us=%u ge_us=%u cpu_us=%u chunks=%u culled=%u tris=%u",
                 stats.fps, static_cast<unsigned>(stats.frameUs), static_cast<unsigned>(stats.geWaitUs),
                 static_cast<unsigned>(terrainCpuUs_), static_cast<unsigned>(st.chunksDrawn),
                 static_cast<unsigned>(st.chunksCulled), static_cast<unsigned>(st.triangles));
        PSP_LOGI(pp::tag::Gfx, "m2_frame on=%d fill_test=%d view=%.0f drawn=%u culled=%u lod1=%u batches=%u tris=%u cpu_us=%u",
                 modelsOn_ ? 1 : 0, fillTest_ ? 1 : 0, models_.viewDistance, static_cast<unsigned>(ms.drawn),
                 static_cast<unsigned>(ms.culled), static_cast<unsigned>(ms.lowLod),
                 static_cast<unsigned>(ms.batches), static_cast<unsigned>(ms.triangles),
                 static_cast<unsigned>(modelsCpuUs_));
        models_.logForestStats();  // m2_forest lod2= imp_n= ...
        uint32_t animId = 0;
        float animT = 0.0f, animDur = 0.0f;
        player_.getAnimationState(animId, animT, animDur);
        PSP_LOGI(pp::tag::Gfx, "char_frame animated=%d anim=%u loco=%d act=%d tris=%u batches=%u bone_uploads=%u",
                 player_.animated() ? 1 : 0, static_cast<unsigned>(animId),
                 static_cast<int>(animator_.getLocomotion().getState()),
                 static_cast<int>(animator_.getActivity().getState()), static_cast<unsigned>(player_.triangles()),
                 static_cast<unsigned>(player_.batchesDrawn()), static_cast<unsigned>(player_.boneUploads()));
    }

    if (const float fade = worldFade(); fade > 0.0f) {  // the entry fade (lit world only)
        ui.beginFrame();
        ui.fill(Rect{0, 0, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)},
                static_cast<uint32_t>(fade * 255.0f) << 24);
        ui.endFrame();
    }
    // layers - one fixed order of everything drawn over the world
    // (ui_world_hud.cpp) until task A's layer manager replaces it.
    drawHudLayers(ui, stats);
}

} // namespace wowee::ui::psp
