// Map - the minimap (HUD) and the world map window (Menu -> Map) of the world
// screen: src/ui/psp/ui_world_minimap.cpp (data/minimap_<map>.pak reader, the HUD disc)
// and src/ui/psp/ui_world_map.cpp (the window). Only those two files include this.
//
// Upstream: rendering/minimap.cpp (tiles from md5translate.trs, 3 x 3 composite around the
// player, north up or rotating with the camera, noData colour), ui/game_screen_minimap.cpp
// (blips, player arrow, zone name, zoom), ui/minimap_projection.hpp (the projection, used
// unchanged), rendering/world_map/* (DataRepository -> baked by psp_minimap_conv;
// coordinate_projection, exploration_state, view_state_machine linked unchanged;
// CompositeRenderer's tiles + explored overlays drawn as sprites).
// ui_world_death.cpp includes it too (the corpse / spirit healer marks below).
// World map: the window is WorldMapFrame of the original (frame art, dropdowns,
// highlight, area label from ui.pak "wm_*" / "wmhl_*"): MapUi::World::Frame and the state under it.
// Minimap: the HUD minimap is MinimapCluster of the original (MapUi::Cluster, the layout
// below follows Minimap.xml at the disc's scale) and its CPU work is cached (MapUi::Disc, the blip list).
// World map levels: the world map's "World" and "Cosmic" levels (data/worldmap.pak) and any
// continent with a minimap pak, not only the player's (MapUi::World::alt).
// Other continents: the continents of maps 530 and 571 (their own minimap paks) and the
// virtual continent of map 530's zones shown on the two old continents (MapUi::World::vMap).
#pragma once

#include "platform/psp/psp_texture.hpp"
#include "rendering/psp/psp_worldmap_view.hpp"
#include "rendering/world_map/exploration_state.hpp"
#include "rendering/world_map/view_state_machine.hpp"
#include "rendering/world_map/world_map_types.hpp"
#include "ui/psp/ui_world_screen.hpp"
#include "ui/psp/ui_world_streaming.hpp"  // StreamLoader

#include <cstdint>
#include <memory>  // world map levels: World::alt
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::ui::psp {

// A vertex of the raw GE fans (mapGeFan below; minimap: moved up for MapUi::Disc).
struct MapGeVert {
    float u, v;
    uint32_t color;
    float x, y, z;
};

// data/minimap_<map>.pak. PSP: opened only while reading and closed 3 s after the last
// read (the card's open-file limit, like SpellDb); the TOC is kept as a
// sorted key table (12 B an entry), the "mmhdr" area table (~40 KB) in RAM. Minimap tiles
// are read by a low-priority loader thread (the streamer's StreamLoader, its own
// instance: a tile read never stalls a frame) into a cache with a byte budget
// (least recently drawn first out, never one drawn in the last 2 frames).
class MinimapStore {
public:
    struct Span {
        uint32_t key, offset, size;
    };
    static uint32_t tileKey(int x, int y) { return (1u << 28) | (static_cast<uint32_t>(x & 0xFF) << 8) | (y & 0xFF); }
    static uint32_t baseKey(uint32_t wma, int n) { return (2u << 28) | ((wma & 0xFFFFFu) << 8) | (n & 0xFF); }
    static uint32_t overlayKey(uint32_t wma, int i) { return (3u << 28) | ((wma & 0xFFFFFu) << 8) | (i & 0xFF); }
    static constexpr uint32_t kWorldHeaderKey = 4u << 28;
    // dungeons: the WMO minimap's tiles "u_<n>", a dungeon floor's art "f_<wma>_<floor>_<n>", "wmmi", "dmap".
    static uint32_t wmoTileKey(uint32_t n) { return (5u << 28) | (n & 0xFFFFFFu); }
    static uint32_t floorKey(uint32_t wma, int floor, int n) {
        return (6u << 28) | ((wma & 0xFFFFu) << 12) | ((static_cast<uint32_t>(floor) & 0xFFu) << 4) | (n & 0xF);
    }
    static constexpr uint32_t kWmoIndexKey = 7u << 28, kDungeonKey = 8u << 28;

    MinimapStore() = default;
    ~MinimapStore();
    MinimapStore(const MinimapStore&) = delete;
    MinimapStore& operator=(const MinimapStore&) = delete;

    // Reads the TOC and "mmhdr", closes the file. useThread: tile reads on a loader
    // thread (wowee.cfg minimapthread), else one read per frame on the main thread.
    bool open(const char* absPath, bool useThread, uint32_t budgetBytes);
    bool loaded() const { return view_.h != nullptr; }
    const rendering::psp::MinimapHeaderView& header() const { return view_; }
    const Span* find(uint32_t key) const;
    bool hasTile(int x, int y) const { return find(tileKey(x, y)) != nullptr; }
    // The tile's texture when cached (marked used this frame); otherwise queues its read
    // and returns nullptr (the caller draws upstream's noData colour meanwhile).
    const platform::psp::Texture* tile(int x, int y, uint32_t frame) { return tileByKey(tileKey(x, y), frame); }
    // dungeons: any texture entry by its key (the ADT tiles and the WMO minimap's "u_" share the cache).
    const platform::psp::Texture* tileByKey(uint32_t key, uint32_t frame);
    void setBudget(uint32_t bytes) { budget_ = bytes; }  // dungeons: a dungeon's WMO tiles are many and small
    // Once per frame: takes finished reads, keeps the cache in budget, closes the file.
    void update(uint32_t frame);
    // Blocking read (world map window: a zone's art in one piece). Main thread.
    bool readBlocking(uint32_t offset, uint32_t size, void* dst);

    uint32_t cachedTiles() const { return static_cast<uint32_t>(tiles_.size()); }
    uint32_t cachedBytes() const { return bytes_; }
    uint32_t tilesLoaded() const { return loads_; }
    uint32_t pending() const { return pending_; }
    uint32_t budget() const { return budget_; }
    bool threaded() const { return loader_.threaded(); }

private:
    bool ensureOpen();
    struct Tile {
        platform::psp::Texture tex;
        uint32_t lastFrame = 0, bytes = 0, requestMs = 0;
        bool pending = false, failed = false;
    };
    std::string path_;
    int fd_ = -1;
    uint32_t lastUseMs_ = 0, openFailMs_ = 0;
    std::vector<uint8_t> hdrBlob_;
    rendering::psp::MinimapHeaderView view_;
    std::vector<Span> spans_;  // sorted by key
    std::unordered_map<uint32_t, Tile> tiles_;
    StreamLoader loader_;
    uint32_t budget_ = 384u * 1024u, bytes_ = 0, loads_ = 0, pending_ = 0, logs_ = 0;
};

struct WorldScreen::MapUi {
    MinimapStore store;
    bool storeOk = false;
    // wowee.cfg (read at world entry)
    bool on = true;           // minimap
    bool rotate = false;      // minimaprotate (upstream minimap_rotate, default off)
    bool dots = true;         // minimapdots (upstream minimapNpcDots_, default off there)
    bool subzone = true;      // minimapsubzone: 1 = the area under the feet (MinimapZoneText), 0 = zone (upstream)
    bool borderArt = true;    // minimapborder: 0 = drawn ring
    float zoom = 200.0f;      // minimapzoom: view radius, yards (upstream viewRadius 400 on a 200 px map)
    float borderDx = 0.0f, borderDy = 0.0f;  // minimapborderdx/dy: ring art offset, px
    // art: ui.pak "mm_border", "mm_arrow"; a generated round dot for the blips
    platform::psp::Texture border, arrow, dot;
    void* dotBlock = nullptr;
    // stats (10 s summary)
    uint32_t frame = 0, cpuSumUs = 0, cpuMaxUs = 0, cpuFrames = 0, nextLogMs = 0, tilesDrawn = 0, blips = 0;
    // minimap: the 10 s summary's parts (us summed over the frames) and cache rebuilds
    uint32_t discUsSum = 0, blipUsSum = 0, clusterUsSum = 0, discBuilds = 0, blipBuilds = 0;
    // zone text
    uint32_t lastArea = 0, lastZone = 0, shownId = ~0u;
    uint64_t lastMismatch = 0;
    std::string zoneText;
    // upstream refreshQuestObjectiveCache
    uint64_t questSig = 0;
    std::unordered_set<uint32_t> questCreatures;

    // minimap: CPU caches (nothing is drawn differently). The disc: the 40-gon's unit
    // circle and the backdrop fan are made once; the disc clipped against the tiles (minimapDisc +
    // minimapClipToTile) is kept while the player, the zoom and the bearing stay the same - the texture of
    // a tile is looked up every frame (a tile read in the meantime shows at once).
    struct Disc {
        static constexpr int kSegments = 40, kMaxPieces = 25, kMaxVerts = 320;  // zoom 800: <= 4 x 4 tiles
        bool tables = false, valid = false;
        float unitCos[kSegments] = {}, unitSin[kSegments] = {};
        MapGeVert backdrop[kSegments + 2] = {};
        float px = 0, py = 0, zoom = 0, sinB = 0, cosB = 1;
        struct Piece {
            int16_t tx, ty;
            uint16_t first, count;
        };
        Piece pieces[kMaxPieces] = {};
        int pieceCount = 0;
        rendering::psp::MinimapVert verts[kMaxVerts] = {};
    } disc;
    // The blips: which unit gets which dot (type, flags, health, hostility, quest objectives, quest giver
    // status) is decided every few frames or when the units change (entityVersion, the zoom); their
    // positions are taken every frame, so the dots move as before.
    struct Blip {
        uint64_t guid;
        uint32_t outer, inner;  // ABGR; outer = 0: one dot (inner)
        float ro, ri;
        const char* symbol;     // quest giver ! / ? (null: none)
        bool counted;           // counts in `blips` (the summary)
    };
    std::vector<Blip> blipList;
    uint32_t blipFrame = 0, blipEntityVersion = ~0u;
    float blipZoom = 0.0f;

    // minimap: MinimapCluster (Minimap.xml, GameTime.xml, the TimeManager addon).
    struct Cluster {
        platform::psp::Texture atlas;    // ui.pak "mm_cluster" (T8 256 x 128, build_ui_pak.py MM_ATLAS)
        platform::psp::Texture compass;  // ui.pak "mm_compass": MinimapCompassTexture (minimaprotate=1 only)
        std::vector<uint32_t> pvpAreas;  // ui.pak "mm_pvp": AreaTable id << 8 | bits (MM_PVP_*), by id
        uint32_t artBytes = 0;
        uint8_t team = 0;                // the player's MM_PVP_ALLIANCE / MM_PVP_HORDE bit
        // MinimapZoneText (Minimap_Update) and its tooltip (Minimap_SetTooltip): GetZonePVPInfo
        uint8_t pvpType = 0;             // kPvp* of ui_world_minimap.cpp
        uint32_t pvpArea = ~0u, pvpZone = ~0u;
        std::string zoneName, subzoneName;
        // MiniMapTracking: upstream trackingSpells (known spells applying aura 44 / 45, by name) and the
        // running one (GetTrackingTexture); the shine (MiniMapTrackingShineFadeIn / Out) when it changes.
        std::vector<uint32_t> trackSpells;
        uint32_t trackKnownVersion = ~0u, trackActive = 0, shineMs = 0;
        bool trackInit = false;
        // MiniMapTrackingDropDown (window kWinMinimapTracking): its items (0 = NONE), since when the focus
        // is away from it (UIDROPDOWNMENU_SHOW_TIME)
        std::vector<uint32_t> menuItems;
        uint32_t menuAwayMs = 0;
        platform::psp::Texture menuCheck;  // ui.pak "wm_dd_check" (UI-CheckBox-Check) while it is open
        uint32_t menuFrame = 0;            // the minimap frame it was last drawn in (freed 3 frames later)
        // GameTimeFrame (CalendarGetDate's day, the invites flash) and TimeManagerClockButton (realm time)
        int clockMinutes = -1, day = 0;
        char clockText[8] = {};
        char dayText[4] = {};
        uint32_t pendingInvites = 0, flashMs = 0, sessionInvites = ~0u;
        bool flashInvite = false;
        // the focused piece (its GameTooltip is drawn after the windows), anchored to its rect
        uint32_t tipId = 0;
        Rect tipRect{};
    } cluster;

    // World map window (loaded on open, freed on close).
    struct World {
        bool open = false, ok = false;
        std::vector<uint8_t> hdrBlob;
        rendering::psp::WorldMapView view;
        std::vector<rendering::world_map::Zone> zones;
        std::unordered_map<uint32_t, uint32_t> exploreFlags;
        rendering::world_map::ViewStateMachine state;
        rendering::world_map::ExplorationState exploration;
        int artZone = -1;
        void* artBlock = nullptr;
        uint32_t artBytes = 0;
        platform::psp::Texture base[12];
        bool haveBase[12] = {};
        std::vector<platform::psp::Texture> overlays;
        std::vector<uint8_t> haveOverlay;
        int hoverZone = -1;
        uint32_t nextExploreMs = 0, lastExplored = ~0u, logs = 0;
        std::string hoverName;

        // world map: WorldMapFrame's art from ui.pak (tools/psp/build_ui_pak.py
        // worldmap_frame), read when the window opens, freed when it closes (worldmap_art / worldmap_close).
        struct Frame {
            platform::psp::Texture top, bottom, sides;              // WorldMapFrameTexture1..18, cut
            platform::psp::Texture dd, ddBtn, ddBtnDown, ddHl;       // UIDropDownMenuTemplate
            platform::psp::Texture ddBg, ddCheck;                    // UIDropDownListTemplate
            platform::psp::Texture corpse, party;                    // WorldMapCorpseTemplate, WorldMapPartyIcon
            // The mini mode's UI-WorldMapSmall frame, the size buttons and their highlight.
            platform::psp::Texture mini, smaller, smallerDown, bigger, biggerDown, sizeHl;
            Font font;                                               // WorldMapTextFont (wm_font, 20 px)
            bool tried = false;
            uint32_t bytes = 0;
        } frame;
        // GetMapContinents: map id and the client's name (wm_conts), WorldMapContinent order.
        std::vector<uint32_t> contMaps;
        std::vector<std::string> contNames;
        // WorldMapHighlight of the shown continent's zones ("wmhl_<folder>", by zone index): one read on the
        // continent view, freed off it.
        std::vector<platform::psp::Texture> hl;
        void* hlBlock = nullptr;
        uint32_t hlBytes = 0;
        int hlContinent = -1;
        // UIDropDownMenu: 0 = closed, 1 = WorldMapContinentDropDown's list, 2 = WorldMapZoneDropDown's.
        int ddOpen = 0;
        uint32_t ddLeftMs = 0;          // the cursor left the list (UIDROPDOWNMENU_SHOW_TIME = 2 s)
        std::vector<int> ddItems;       // the open list: continent order / zone indices (alphabetical)
        Rect ddRect{};                  // the open list, last frame (the map takes no hover under it)
        std::string areaName;           // WorldMapFrameAreaLabel
        bool areaExplored = true;
        // PSP (owner: "pick the zones with the D-pad too, hover is hit and miss"): a point inside each
        // zone of the shown continent (its ZMP cell nearest the zone's centroid, map u/v) - a focus stop each
        // (kIdMapZone0 + k), so the D-pad walks the zones and Cross zooms in; the cursor still hovers the ZMP.
        struct ZoneSpot {
            int zone;
            float u, v;
        };
        std::vector<ZoneSpot> spots;
        int spotsContinent = -1;
        Rect mapOnScreen{};  // the map picture last frame, screen coordinates (the mini window moves)

        // world map levels: the levels above the continents and the other continents.
        // upstream DataRepository::loadZones appends a synthetic "Cosmic" and "World" zone after the map's own:
        // the view state machine's WORLD / COSMIC levels show them (setCosmicEnabled(true): 3.3.5a).
        int cosmicIdx = -1, worldIdx = -1;
        // The map whose zones are shown (zones / view / hdrBlob): the player's (MapUi::store) or another
        // continent's data/minimap_<map>.pak (alt: opened when that continent is chosen - no loader thread,
        // blocking reads - and closed when the player's continent is shown again or the window closes).
        std::unique_ptr<MinimapStore> alt;
        uint32_t shownMap = ~0u;
        // GetMapContinents (contMaps order): 1 = its minimap pak (with the world map) is on the card.
        std::vector<uint8_t> contAvail;
        // The World / Cosmic levels: data/worldmap.pak (rendering::psp::WorldLevelView, psp_minimap_conv --world).
        // The header when a level is first shown, a level's art (12 tiles + its highlights, one read) while it is.
        std::vector<uint8_t> wlHdr;
        rendering::psp::WorldLevelView wl;
        bool wlTried = false;
        bool levelsAvail = false;  // data/worldmap.pak is on the card (checked when the window opens)
        int lvlArt = 0;  // what lvlTiles / lvlHl hold: 0 nothing, 1 the World level, 2 the Cosmic level
        void* lvlBlock = nullptr;
        uint32_t lvlBytes = 0;
        platform::psp::Texture lvlTiles[12];
        bool haveLvlTile[12] = {};
        std::vector<platform::psp::Texture> lvlHl;  // World: by WorldLevelContinent; Cosmic: by cosmic button
        int hoverCont = -1;  // World: the WorldLevelContinent under the cursor; Cosmic: the button under it
        // (end world map levels)
        // other continents: the virtual continent (upstream WorldMapFacade::render's
        // virtualMapOverride) - the player's zone is on one map and shown under another (WorldMapArea
        // DisplayMapID: map 530's zones on the eastern and on the western continent).
        // Found when the window opens: that continent's data is opened (alt) and the player's position (and the
        // units around them) is shifted by the zone's WorldMapTransforms offset there (server X / Y).
        uint32_t vMap = ~0u;  // the display map, ~0u = none
        float vOffX = 0.0f, vOffY = 0.0f;
        // dungeons: GetCurrentMapDungeonLevel (0 = none: the zone has no levels) of the dungeon's zone shown;
        // the orphan instance's continent (own rule: the whole continent of its corpse map, no arrow).
        int dLevel = 0;
    } world;
    // dungeons: a dungeon's WMO minimap ("wmmi") and dungeon world map ("dmap"),
    // read with the pak (a few KB). The player's WMO group (the smallest MOGP box holding them - PSP: the client
    // asks the WMO's BSP), indoors = that group has no exterior flag 0x8: the minimap then draws the WMO tiles of the
    // groups the client's portal walk reaches, at the indoor zoom (minimapinzoom 0..5 = 150 .. 25 yd across).
    struct Dungeon {
        std::vector<uint8_t> wmmiBlob, dmapBlob;
        rendering::psp::WmoMinimapView wmmi;
        rendering::psp::DungeonMapView dmap;
        bool indoor = false;
        int inst = -1, group = -1;  // the player's instance / group (global index), -1 none
        int inZoom = 0;             // CVar minimapInsideZoom
        std::vector<uint8_t> seen;  // the walk's scratch
        uint32_t tilesDrawn = 0, groupsWalked = 0, groupChanges = 0;
    } dg;
    float viewR = 200.0f;  // dungeons: the minimap's view radius this frame (zoom, or half the indoor zoom)
    // WorldMapFrame's mini mode (CVar miniWorldMap -> wowee.cfg worldmapmini); -1 = not read yet.
    int mini = -1;
    // The full view's BlackoutWorld alpha (wowee.cfg worldmapdim, 0..255); -1 = not read yet.
    int dim = -1;

    MapUi() = default;
    MapUi(const MapUi&) = delete;
    MapUi& operator=(const MapUi&) = delete;
    ~MapUi();
    void releaseWorldArt();
    void releaseWorld();
    void releaseHighlights();  // world map
    // world map levels: the World / Cosmic level art; the store of the continent shown; the window's
    // bytes (frame, highlights, zone or level art, headers) for worldmap_close.
    void releaseLevelArt();
    MinimapStore& shownStore() { return world.alt ? *world.alt : store; }
    uint32_t worldBytes() const;
};

// Screen layout of the minimap cluster (480 x 272): top right, where the client puts
// MinimapCluster; the buffs move left of it (ui_world_spells.cpp drawAuras).
// minimap: Minimap.xml at the disc's scale kScale (the 140-unit Minimap = the 76 px disc):
// MinimapCluster (192 x 192 units) TOPRIGHT at the screen's corner, Minimap = CENTER of the cluster's TOP +
// (9, -92), MinimapBackdrop (the ring and the buttons) = the cluster's CENTER + (0, -20). Was: the disc at
// (428, 70) with the zone name above it - the original puts MinimapBorderTop at the very top.
namespace minimap_layout {
constexpr float kRadius = 38.0f;             // disc, px (the client's 140 px Minimap scaled to the PSP)
constexpr float kScale = 2.0f * kRadius / 140.0f;              // Minimap.xml units -> px
constexpr float kCenterX = 480.0f - (96.0f - 9.0f) * kScale;  // 432.8
constexpr float kCenterY = 92.0f * kScale;                     // 49.9
constexpr float kBackdropX = 480.0f - 96.0f * kScale;          // MinimapBackdrop centre: 427.9
constexpr float kBackdropY = (96.0f + 20.0f) * kScale;         // 63.0
constexpr float kReserveW = 108.0f;          // what the cluster takes from the right edge
constexpr float kZoomMin = 100.0f, kZoomMax = 800.0f, kZoomStep = 50.0f;  // upstream Minimap zoomIn / zoomOut
// Widget ids (range 0x6800-0x68FF; 0x6800 = WorldScreen::kIdMenuMap).
// world map: kIdMapUp = WorldMapZoomOutButton (ZOOM_OUT), kIdMapClose = WorldMapFrameCloseButton,
// the two dropdowns and their list items (up to 64).
enum : uint32_t {
    kIdZoomIn = 0x6801, kIdZoomOut = 0x6802,
    kIdMapArea = 0x6810, kIdMapUp = 0x6811, kIdMapClose = 0x6813,
    kIdMapSize = 0x6814,  // WorldMapFrameSizeDownButton / SizeUpButton (WorldMapFrame_ToggleWindowSize)
    kIdMapContDrop = 0x6820, kIdMapZoneDrop = 0x6821,
    kIdMapDdItem0 = 0x6840, kIdMapDdItemEnd = 0x6880,
    kIdMapZone0 = 0x68D0, kIdMapZoneEnd = 0x6900,  // The continent view's zone spots (World::spots)
    // minimap: the cluster's pieces (outside mapWindowAction's 0x6801..0x687F) and the
    // tracking dropdown's items
    kIdMmZone = 0x6890, kIdMmTracking = 0x6891, kIdMmWorldMap = 0x6892, kIdMmMail = 0x6893,
    kIdMmCalendar = 0x6894, kIdMmClock = 0x6895,
    kIdMmTrackItem0 = 0x68A0, kIdMmTrackItemEnd = 0x68C0,
    // dungeons (ids 0x9100-0x91FF): WorldMapLevelDropDown (its items: kIdMapDdItem0.., list 3).
    kIdMapLevelDrop = 0x9100,
};
}  // namespace minimap_layout

// Raw GE drawing shared by the minimap and the world map window (2D, float vertices):
// a triangle fan, and the player arrow (MinimapArrow turned to (dx, dy), else upstream's
// chevron). Call between UiRenderer::beginFrame / endFrame with nothing staged.
// (MapGeVert: above MapUi - minimap keeps the backdrop fan in it.)
void mapGeFan(const MapGeVert* src, int n);
void mapDrawArrow(const platform::psp::Texture* tex, float cx, float cy, float dx, float dy, float size);
// ui_world_death.cpp: the ghost's corpse on the minimap (upstream
// renderMinimapCorpseMarker: an X in a ring; off the disc a triangle at the rim pointing
// (nx, ny) at it) and on the world map (CorpseMarkerLayer: the corpse X, the spirit healer's
// cross). Same rules as above: raw GE, nothing staged.
void mapDrawCorpseMark(float cx, float cy);
void mapDrawCorpsePointer(float ax, float ay, float nx, float ny);
void mapDrawWorldCorpse(float cx, float cy);
void mapDrawHealerCross(float gx, float gy);

}  // namespace wowee::ui::psp
