// Map - the minimap in the world HUD (top right) and the minimap pak reader.
//
// Upstream: rendering/minimap.cpp + ui/game_screen_minimap.cpp.
//  - Tiles "<Map>\map<x>_<y>" of md5translate.trs around the player; upstream composites a
//    3 x 3 block into a 768 px render target (refreshed every 0.25 s / 6 yd) and draws a
//    circle of it with a shader. PSP: no render target and no shader - the disc is a
//    40-gon clipped against every tile it touches (psp_minimap_format.hpp), each piece one
//    textured triangle fan with its tile's texture coordinates (the GE interpolates them
//    exactly: the mapping is affine), so the whole minimap is one GE pass of <= 16 fans
//    and costs nothing when the player stands still. Tiles not baked / not read yet: the
//    noData colour (upstream noDataTexture). Mip level picked on the CPU (2D mode).
//  - Projection: upstream include/ui/minimap_projection.hpp unchanged, both ways; the
//    disc's texture coordinates come from minimapOffsetToRenderDelta, the blips from
//    renderDeltaToMinimapOffset, so the map and its blips cannot disagree.
//  - Rotation (minimaprotate, upstream minimap_rotate, off by default like upstream):
//    PSP: bearing = the camera yaw for the map AND the blips (upstream's shader and its
//    marker pass use two different angles and the source says the rotating mode was never
//    checked); the camera's forward is straight up (selftest checks it).
//  - Blips (upstream renderMinimapMarkers order): NPC dots (hostile red, else white; quest
//    kill targets gold - refreshQuestObjectiveCache, PSP: every quest in the log, there is
//    no tracker), flight masters, other players (blue), lootable corpses, quest givers
//    (! / ? on a gold or grey dot, questGiverMarker: status 9 stays off the minimap), the
//    player arrow (Interface\Minimap\MinimapArrow, as upstream's world map; upstream's
//    minimap draws a chevron - the fallback here).
//  - Zone name (renderMinimapReadouts): PSP default = the area under the feet (the original
//    client's MinimapZoneText, minimapsubzone=1); 0 = upstream (the zone: AreaTable parent
//    chain, then the server's SMSG_INIT_WORLD_STATES zone). Area ids of the ground: the
//    ADT area grids of data/audio.pak (ZoneMusic::areaAt, same source as the music).
//  - Zoom (upstream zoomIn/zoomOut: 50 yd steps, 100..800): MinimapZoomIn / MinimapZoomOut
// in cursor mode; ui_world_map.cpp mapWindowAction applies it.
//  - a dungeon's pak (any map now: worldMapDir, was the four continents)
//    may hold the WMO minimap ("wmmi" + "u_<n>"). Indoors - a group with the interior flag 0x2000 holds the player
//    (upstream WMORenderer::isInsideInteriorWMO; wowdev.wiki WMO MOGP flags) - the disc shows the WMO tiles of the
//    groups reached by the portal walk (psp_minimap_format.hpp wmoMinimapWalk, after upstream
//    getVisibleGroupsViaPortals) at the indoor zoom (kIndoorZoomYards: 192 / 160 / 128 / 96 / 64 / 32 yd across;
//    wowee.cfg minimapinzoom 0..5, kept apart from the outdoor zoom like the public CVar minimapInsideZoom). The
//    player's group = the smallest interior group box holding the player (upstream findContainingGroup's rule).
//    Port's own rules: the view box is the disc's square and 8 yd up and down from the feet; the groups nearer the
//    feet in height are drawn later (on top); the disc is first filled with the noData colour, as outdoors. Tile
//    texels: u along +x from the tile's corner, texture row 0 at the tile's +y edge (checked against the groups' own
//    floor geometry, tools/psp/check_wmo_minimap_orientation.py).
//
// MinimapCluster of the original ruRU 3.3.5a around the disc - Minimap.xml /
// .lua, GameTime.xml / .lua, the time manager add-on's .xml - at the disc's scale (minimap_layout::kScale: the
// 140-unit Minimap = the 76 px disc, MinimapCluster's TOPRIGHT at the screen's corner), the art from one
// T8 atlas, ui.pak "mm_cluster" (tools/psp/build_ui_pak.py MM_ATLAS), baked at the drawn size, drawn 1:1:
//  - MinimapBorderTop + MinimapZoneText (GetMinimapZoneText), coloured as Minimap_Update by GetZonePVPInfo:
//    sanctuary / arena / friendly / hostile / contested, else NORMAL_FONT_COLOR; on focus its tooltip
//    Minimap_SetTooltip (the zone white, the subzone and the territory line in that colour). GetZonePVPInfo
//    is the client's own (C); PSP reading: AreaTable Flags of the area (0x800 sanctuary, 0x80 arena), else
//    its FactionGroupMask or, without one, its zone's, against the player's faction; none = contested on
//    the four continents (ui.pak "mm_pvp"). Not read: "combat" (a battlefield zone's battle, a world state).
//    PSP: the text spans the bar's visible width (90 px; the XML's 150 units are 81 px, the PSP font is
//    ~1.5x the scaled GameFontNormal), longer names scroll (textMarquee, as before).
//  - MinimapBorder (the ring) and MinimapNorthTag (rotateMinimap = 0, wowee.cfg minimaprotate) or
//    MinimapCompassTexture (1: ui.pak "mm_compass" turned to north; without it the old letter MM_NORTH).
//  - MinimapZoomIn / MinimapZoomOut: Normal / Pushed / Disabled art, the ADD highlight on focus;
//    Minimap_ZoomInClick / OutClick (igMiniMapZoomIn / Out, disabled at the ends - upstream's 100 / 800 yd);
//    tooltip ZOOM_IN / ZOOM_OUT (UberTooltips) at GameTooltip_SetDefaultAnchor's place.
//  - MiniMapTracking: background (0.6), GetTrackingTexture (the running tracking spell's icon, else
//    Tracking\None), border, highlight, the shine when it changes (MiniMapTrackingShineFadeIn / Out); Cross =
//    MiniMapTrackingDropDown (window kWinMinimapTracking: the tracking spells + NONE, the running one
//    checked; Cross on an item = SetTracking - the spell is cast, as upstream lua_SetTracking; NONE cancels
//    the running one: PSP - upstream's NONE does nothing (its binding has no aura cancel), the client
//    cancels the tracking aura); tooltip GameTooltip:SetTracking (the running one's name, else
//    MINIMAP_TRACKING_TOOLTIP_NONE). The list = upstream lua_GetTrackingInfo: the known spells applying aura
//    44 / 45 (spells.pak SpellRec::aura), by name. Not ported: the townsfolk tracking of 3.1+ (Banker,
//    Flight Master...: upstream has none either - the PSP shows every NPC dot, wowee.cfg minimapdots).
//  - MiniMapWorldMapButton: UI-World-Icon + border, Cross = ToggleFrame(WorldMapFrame); tooltip
//    WORLDMAP_BUTTON (PSP: without NEWBIE_TOOLTIP_WORLDMAP - it explains the mouse buttons).
//  - MiniMapMailFrame: shown while HasNewMail (WorldSession::hasNewMail, psp_world_session_minimap.cpp);
//    tooltip MinimapMailFrameUpdate: HAVE_MAIL, or HAVE_MAIL_FROM + GetLatestThreeSenders.
//  - GameTimeFrame: UI-Calendar-Button with CalendarGetDate's day (the realm's date, run on with the
//    realm time); pending invites (CalendarGetNumPendingInvites rising) show the invite texture + glow,
//    flashing (0.5 + 0.5 cos, 1 s), tooltip GAMETIME_TOOLTIP_CALENDAR_INVITES, Cross takes the flash off
//    (GameTimeFrame_OnClick). PSP: no CalendarFrame (the client's calendar add-on is not ported here), so without invites
//    there is no tooltip (the original's GAMETIME_TOOLTIP_TOGGLE_CALENDAR) and Cross does nothing.
//  - TimeManagerClockButton (CVar showClock = 1): ClockBackground + the realm time (GameTime_GetGameTime;
//    PSP: run on between the server's packets, as the light does); tooltip GameTime_UpdateTooltip
//    (TIMEMANAGER_TOOLTIP_TITLE, realm / local time). PSP: 24-hour time (CVar timeMgrUseMilitaryTime;
//    wowee.cfg clock24=0 = the 12-hour ticker); no TimeManagerFrame (alarm), so no
//    GAMETIME_TOOLTIP_TOGGLE_CLOCK line and Cross does nothing.
//  - Hidden in the original outside their cases, not ported: MiniMapBattlefieldFrame, MiniMapLFGFrame,
//    MiniMapInstanceDifficulty, MinimapPing (no clicks on the map on the PSP), MiniMapRecordingButton.
// CPU (minimap 1.07 ms a frame on the PSP-3000, builds 99-101), the look unchanged: the unit circle and the
// backdrop fan are made once; the disc is clipped against the tiles only when the player, the zoom or the
// bearing changed (MapUi::Disc - standing still costs no clipping); which unit gets which blip (fields,
// hostility, quest objectives, quest giver status) is decided every 8 frames or when the units change
// (MapUi::blipList), the positions every frame; the cluster's texts are made when they change.
// minimap_frame (every 10 s) gives the parts: disc_us / blip_us / ui_us and the rebuilds.
#include "ui/psp/ui_world_map.hpp"

#include "game/protocol_constants.hpp"
#include "game/quest_giver_status.hpp"
#include "game/update_field_table.hpp"
#include "core/coordinates.hpp"
#include "core/local_time.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_vmath.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <malloc.h>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace fmt = wowee::rendering::psp;
using namespace minimap_layout;

// ---------------------------------------------------------------------------
// Raw GE pieces shared with the world map window (ui_world_map.hpp)
// ---------------------------------------------------------------------------

constexpr int kMapGeVtype = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D;

void mapGeFan(const MapGeVert* src, int n) {
    if (n < 3) return;
    const size_t bytes = static_cast<size_t>(n) * sizeof(MapGeVert);
    auto* v = static_cast<MapGeVert*>(sceGuGetMemory(bytes));
    std::memcpy(v, src, bytes);
    sceKernelDcacheWritebackRange(v, bytes);
    sceGuDrawArray(GU_TRIANGLE_FAN, kMapGeVtype, n, nullptr, v);
}

// The player arrow: `tex` (MinimapArrow, pointing up) turned to screen direction (dx, dy),
// or upstream's chevron (renderMinimapPlayerArrow) without the art. Between
// UiRenderer::beginFrame / endFrame, nothing staged.
void mapDrawArrow(const pp::Texture* tex, float cx, float cy, float dx, float dy, float size) {
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!(len > 1e-4f)) {
        dx = 0.0f;
        dy = -1.0f;
    } else {
        dx /= len;
        dy /= len;
    }
    const float px = -dy, py = dx;  // screen right of the tip direction
    MapGeVert v[4];
    if (tex && tex->pixels) {
        const float h = size * 0.5f, W = tex->uw, H = tex->uh;
        v[0] = {0, 0, 0xFFFFFFFFu, cx - px * h + dx * h, cy - py * h + dy * h, 0};
        v[1] = {W, 0, 0xFFFFFFFFu, cx + px * h + dx * h, cy + py * h + dy * h, 0};
        v[2] = {W, H, 0xFFFFFFFFu, cx + px * h - dx * h, cy + py * h - dy * h, 0};
        v[3] = {0, H, 0xFFFFFFFFu, cx - px * h - dx * h, cy - py * h - dy * h, 0};
        pp::textureBind(tex);
        mapGeFan(v, 4);
        pp::textureResetBinding();
        return;
    }
    pp::textureBind(nullptr);
    const float tip = size * 0.45f, base = size * 0.3f, notch = size * 0.15f;
    const uint32_t fill = 0xF5C8F8FFu;  // upstream IM_COL32(255, 248, 200, 245)
    const float tX = cx + dx * tip, tY = cy + dy * tip;
    const float nX = cx - dx * (base - notch), nY = cy - dy * (base - notch);
    MapGeVert l[3] = {{0, 0, fill, tX, tY, 0}, {0, 0, fill, cx - dx * base + px * base, cy - dy * base + py * base, 0},
                      {0, 0, fill, nX, nY, 0}};
    MapGeVert r[3] = {{0, 0, fill, tX, tY, 0}, {0, 0, fill, nX, nY, 0},
                      {0, 0, fill, cx - dx * base - px * base, cy - dy * base - py * base, 0}};
    mapGeFan(l, 3);
    mapGeFan(r, 3);
}

namespace {

constexpr uint32_t kNoDataABGR = 0xFF1E140Cu;  // upstream noDataTexture (12, 20, 30)
constexpr int kDiscSegments = WorldScreen::MapUi::Disc::kSegments;
constexpr float kIndoorBandYards = 8.0f;  // indoor view box: this far up and down from the feet (own rule, ~ a storey)
constexpr uint32_t kCloseAfterMs = 3000;
constexpr uint32_t abgr(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (a << 24) | (b << 16) | (g << 8) | r; }

// Same table as ui_world_screen.cpp (terrain_<dir>.pak). Any other map by Map.dbc (worldMapDir,
// glue.pak "mapdirs") - a dungeon's minimap_<dir>.pak (was: none, the dungeons had no minimap).
const char* mapDir(uint32_t mapId) {
    switch (mapId) {
        case 0: return "azeroth";
        case 1: return "kalimdor";
        case 530: return "expansion01";
        case 571: return "northrend";
        default: break;
    }
    static std::string other;  // the world screen's thread only
    other = worldMapDir(mapId);
    return other.empty() ? nullptr : other.c_str();
}

bool loadUiArt(pp::Pak& pak, const char* name, pp::Texture& out, uint32_t& bytes) {
    const pp::PakEntry* e = pak.find(name);
    if (!e) return false;
    void* blob = pak.load(e);
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, e->size, out, true)) {
        pp::pakFree(blob);
        return false;
    }
    bytes += e->size;
    return true;
}

// A 16 x 16 white disc (4444, soft edge): every round blip, tinted by the vertex colour.
void* makeDot(pp::Texture& t) {
    constexpr int kN = 16;
    auto* px = static_cast<uint16_t*>(memalign(16, kN * kN * 2));
    if (!px) return nullptr;
    for (int y = 0; y < kN; ++y)
        for (int x = 0; x < kN; ++x) {
            const float dx = x + 0.5f - kN * 0.5f, dy = y + 0.5f - kN * 0.5f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float a = std::clamp(kN * 0.5f - d, 0.0f, 1.0f);
            px[y * kN + x] = static_cast<uint16_t>(0x0FFFu | (static_cast<uint16_t>(a * 15.0f + 0.5f) << 12));
        }
    sceKernelDcacheWritebackRange(px, kN * kN * 2);
    t = pp::Texture{};
    t.pixels = px;
    t.mip[0] = px;
    t.w = t.h = t.uw = t.uh = kN;
    t.psm = GU_PSM_4444;
    t.flags = pp::kTexHasAlpha | pp::kTexLinear;
    t.levels = 1;
    t.name = "mm_dot";
    return px;
}

// Tile texture bound at mip level `level` (2D mode: the GE's automatic LOD is not used).
void bindTileLevel(const pp::Texture& t, int level) {
    sceGuEnable(GU_TEXTURE_2D);
    if (t.clut) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(t.clutCount / 8, t.clut);
    }
    sceGuTexMode(t.psm, 0, 0, t.swizzled);
    sceGuTexImage(0, t.w >> level, t.h >> level, t.w >> level, t.mip[level]);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    pp::prof::count(pp::prof::kTexBinds, 1);
}

// ---------------------------------------------------------------------------
// MinimapCluster
// ---------------------------------------------------------------------------

// The atlas "mm_cluster" (content texels) = MM_ATLAS of tools/psp/build_ui_pak.py.
constexpr SrcRect kSrcRing{0, 0, 104, 104}, kSrcBar{0, 106, 104, 17};
constexpr SrcRect kSrcZoomIn[3] = {{106, 0, 17, 17}, {124, 0, 17, 17}, {142, 0, 17, 17}};  // up, down, disabled
constexpr SrcRect kSrcZoomOut[3] = {{160, 0, 17, 17}, {178, 0, 17, 17}, {196, 0, 17, 17}};
constexpr SrcRect kSrcZoomHl{214, 0, 17, 17};
constexpr SrcRect kSrcTrkBorder{106, 18, 18, 18}, kSrcTrkBg{125, 18, 14, 14}, kSrcTrkNone{140, 18, 11, 11};
constexpr SrcRect kSrcShine{152, 18, 15, 15}, kSrcWorldIcon{168, 18, 11, 11}, kSrcMailIcon{180, 18, 10, 10};
constexpr SrcRect kSrcNorth{191, 18, 9, 9};
constexpr SrcRect kSrcCalUp{106, 37, 22, 22}, kSrcCalDown{129, 37, 22, 22}, kSrcCalGlow{152, 37, 26, 26};
constexpr SrcRect kSrcCalInvite{179, 37, 15, 17}, kSrcClockBg{195, 37, 33, 15};

// Where the pieces sit (px, top-left): the anchors of Minimap.xml / GameTime.xml / the time manager add-on's .xml
// times kScale from the cluster's TOPRIGHT (480, 0), rounded to whole pixels for the 1:1 art.
constexpr Rect kBarRect{376, 0, 104, 17};       // MinimapBorderTop: TOPRIGHT of the cluster, 192 x 32
constexpr Rect kZoneTextBox{387, 1, 90, 11};    // MinimapZoneText: CENTER (7, 83) of the cluster (the bar's inside)
constexpr Rect kRingRect{376, 11, 104, 104};    // MinimapBorder = MinimapBackdrop, 192 x 192
constexpr Rect kNorthRect{428, 9, 9, 9};        // MinimapNorthTag: Minimap CENTER + (0, 67), 16
constexpr Rect kZoomInRect{458, 65, 17, 17};    // MinimapZoomIn: backdrop CENTER + (71, -20), 32
constexpr Rect kZoomInHit{460, 66, 13, 13};     //   HitRectInsets 4, 4, 2, 6
constexpr Rect kZoomOutRect{447, 75, 17, 17};   // MinimapZoomOut: backdrop CENTER + (51, -39)
constexpr Rect kZoomOutHit{449, 76, 13, 13};
constexpr Rect kTrackRect{383, 33, 17, 17};     // MiniMapTracking: backdrop TOPLEFT + (13, -40), 32
constexpr Rect kTrackBg{384, 35, 14, 14};       //   Background TOPLEFT (2, -4), 25, alpha 0.6
constexpr Rect kTrackIcon{386, 36, 11, 11};     //   Icon TOPLEFT (6, -6), 20; pushed (8, -8)
constexpr Rect kTrackIconDown{387, 37, 11, 11};
constexpr Rect kTrackBorder{383, 33, 18, 18};   //   Border 54 (its ring: 40 of 64 texels)
constexpr Rect kTrackShine{384, 34, 15, 15};    //   Shine TOPLEFT (2, -2), 27, ADD
constexpr Rect kWorldRect{451, 12, 18, 18};     // MiniMapWorldMapButton: backdrop TOPRIGHT + (-21, -1), 33
constexpr Rect kWorldIcon{453, 14, 11, 11};     //   Icon CENTER (-2, 2), 20
constexpr Rect kMailRect{464, 41, 18, 18};      // MiniMapMailFrame: Minimap TOPRIGHT + (21, -53), 33
constexpr Rect kMailIcon{468, 44, 10, 10};      //   MiniMapMailIcon TOPLEFT (7, -6), 18
constexpr Rect kCalRect{460, 21, 22, 22};       // GameTimeFrame: Minimap TOPRIGHT + (20, -17), 40
constexpr Rect kCalHit{463, 24, 18, 13};        //   HitRectInsets 6, 0, 5, 10
constexpr Rect kCalInvite{463, 23, 15, 17};     //   GameTimeCalendarInvitesTexture CENTER (0, 1), 28 x 32
constexpr Rect kCalGlow{458, 18, 26, 26};       //   GameTimeCalendarInvitesGlow CENTER (0, 2), 48, ADD
constexpr float kCalTextX = 470.0f, kCalTextY = 32.5f;  // ButtonText CENTER (-1, -1)
constexpr Rect kClockRect{416, 79, 33, 15};     // TimeManagerClockButton: Minimap CENTER + (0, -68), 60 x 28
constexpr float kCompassDx = -2.0f * kScale;    // MinimapCompassTexture: CENTER of Minimap + (-2, 0)
constexpr float kCompassSize = 118.0f;          // its ring cut of 216.7 units (build_ui_pak.py MM_COMPASS)
constexpr float kDropGap = 5.0f * kScale;       // ToggleDropDownMenu(..., "MiniMapTracking", 0, -5)

// GetZonePVPInfo (bits of "mm_pvp" = build_ui_pak.py MM_PVP_*).
constexpr uint8_t kPvpAlliance = 1, kPvpHorde = 2, kPvpSanctuary = 4, kPvpArena = 8;
enum : uint8_t { kPvpNone, kPvpSanctuaryType, kPvpArenaType, kPvpFriendly, kPvpHostile, kPvpContested };
// Minimap_Update's MinimapZoneText:SetTextColor (ABGR).
uint32_t pvpColor(uint8_t type) {
    switch (type) {
        case kPvpSanctuaryType: return abgr(105, 204, 240, 255);  // 0.41, 0.8, 0.94
        case kPvpArenaType:
        case kPvpHostile: return abgr(255, 26, 26, 255);          // 1.0, 0.1, 0.1
        case kPvpFriendly: return abgr(26, 255, 26, 255);         // 0.1, 1.0, 0.1
        case kPvpContested: return abgr(255, 179, 0, 255);        // 1.0, 0.7, 0.0
        default: return kColGold;                                 // NORMAL_FONT_COLOR
    }
}

// GameTooltip pieces (Interface\Tooltips backdrop as the kit's other tooltips).
struct TipLine {
    std::string left, right;
    uint32_t lc = kColWhite, rc = kColWhite;
};
enum class TipAnchor : uint8_t { Left, Right, BottomLeft, Default };

void drawClusterTip(UiRenderer& ui, const UiKit& kit, const std::vector<TipLine>& lines, const Rect& owner, TipAnchor a) {
    if (lines.empty()) return;
    const Font& f = kit.font(kFontNormal);
    const float lh = static_cast<float>(f.lineHeight() > 0 ? f.lineHeight() : 14);
    float w = 0.0f;
    for (const TipLine& l : lines) {
        float lw = static_cast<float>(f.measure(l.left.c_str()));
        if (!l.right.empty()) lw += 12.0f + static_cast<float>(f.measure(l.right.c_str()));
        w = std::max(w, lw);
    }
    Rect r{0, 0, w + 16.0f, lh * static_cast<float>(lines.size()) + 10.0f};
    switch (a) {
        case TipAnchor::Left:  // the tooltip's BOTTOMRIGHT at the owner's TOPLEFT
            r.x = owner.x - r.w;
            r.y = owner.y - r.h;
            break;
        case TipAnchor::Right:  // BOTTOMLEFT at TOPRIGHT
            r.x = owner.x + owner.w;
            r.y = owner.y - r.h;
            break;
        case TipAnchor::BottomLeft:  // TOPRIGHT at BOTTOMLEFT
            r.x = owner.x - r.w;
            r.y = owner.y + owner.h;
            break;
        case TipAnchor::Default:  // GameTooltip_SetDefaultAnchor: the bottom right, over the action bars
            r.x = pp::kScreenW - 4.0f - r.w;
            r.y = 230.0f - r.h;
            break;
    }
    r.x = std::max(2.0f, std::min(r.x, pp::kScreenW - 2.0f - r.w));
    r.y = std::max(2.0f, std::min(r.y, pp::kScreenH - 2.0f - r.h));
    if (kit.has(kTexTipBg)) ui.tile(kit.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit.has(kTexTipBorder)) ui.nineSlice(kit.tex(kTexTipBorder), r);
    float y = r.y + 5.0f;
    for (const TipLine& l : lines) {
        if (!l.left.empty()) ui.text(f, r.x + 8, y, l.left.c_str(), l.lc);
        if (!l.right.empty()) ui.text(f, r.x + r.w - 8, y, l.right.c_str(), l.rc, Align::Right);
        y += lh;
    }
}

int daysInMonth(int month, int year) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return kDays[(month - 1) % 12];
}

// GameTime_GetFormattedTime (TIMEMANAGER_TICKER_24HOUR / _12HOUR; with AM / PM: TIME_TWELVEHOURAM / PM).
void formatTime(char* out, size_t n, unsigned minutes, bool military, bool wantAmPm) {
    unsigned hour = (minutes / 60u) % 24u;
    const unsigned minute = minutes % 60u;
    if (military) {
        std::snprintf(out, n, "%02u:%02u", hour, minute);
        return;
    }
    const bool pm = hour >= 12u;
    hour %= 12u;
    if (hour == 0u) hour = 12u;
    if (wantAmPm && pm) std::snprintf(out, n, "%u:%02u PM", hour, minute);
    else if (wantAmPm) std::snprintf(out, n, "%u:%02u AM", hour, minute);
    else std::snprintf(out, n, "%u:%02u", hour, minute);
}

}  // namespace

// ---------------------------------------------------------------------------
// MinimapStore
// ---------------------------------------------------------------------------

MinimapStore::~MinimapStore() {
    loader_.stop();  // the thread may be reading fd_
    for (auto& [k, t] : tiles_) pp::textureRelease(t.tex);
    if (fd_ >= 0) {
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
    }
}

bool MinimapStore::open(const char* absPath, bool useThread, uint32_t budgetBytes) {
    path_ = absPath;
    budget_ = budgetBytes;
    pp::Pak pak;
    if (!pak.open(absPath)) return false;
    spans_.clear();
    for (uint32_t i = 0; i < pak.count(); ++i) {
        const pp::PakEntry* e = pak.entry(i);
        int x = 0, y = 0, n = 0;
        unsigned wma = 0;
        uint32_t key = 0;
        if (std::sscanf(e->name, "t_%d_%d", &x, &y) == 2) key = tileKey(x, y);
        else if (std::sscanf(e->name, "w_%u_%d", &wma, &n) == 2) key = baseKey(wma, n);
        else if (std::sscanf(e->name, "o_%u_%d", &wma, &n) == 2) key = overlayKey(wma, n);
        else if (std::strcmp(e->name, "wmhdr") == 0) key = kWorldHeaderKey;
        else if (std::sscanf(e->name, "u_%u", &wma) == 1) key = wmoTileKey(wma);
        else if (std::sscanf(e->name, "f_%u_%d_%d", &wma, &x, &n) == 3) key = floorKey(wma, x, n);
        else if (std::strcmp(e->name, "wmmi") == 0) key = kWmoIndexKey;
        else if (std::strcmp(e->name, "dmap") == 0) key = kDungeonKey;
        else continue;
        spans_.push_back({key, e->offset, e->size});
    }
    std::sort(spans_.begin(), spans_.end(), [](const Span& a, const Span& b) { return a.key < b.key; });
    const pp::PakEntry* h = pak.find("mmhdr");
    void* blob = h ? pak.load(h) : nullptr;
    if (blob) {
        hdrBlob_.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + h->size);
        pp::pakFree(blob);
    }
    pak.close();  // one descriptor fewer for the session (card open-file limit)
    if (!view_.attach(hdrBlob_.data(), static_cast<uint32_t>(hdrBlob_.size()))) {
        PSP_LOGE(pp::tag::Fs, "minimap_pak bad header size=%u", static_cast<unsigned>(hdrBlob_.size()));
        hdrBlob_.clear();
        spans_.clear();
        return false;
    }
    if (!loader_.start(useThread, 42)) loader_.start(false, 42);  // below the tile streamer (40)
    return true;
}

const MinimapStore::Span* MinimapStore::find(uint32_t key) const {
    auto it = std::lower_bound(spans_.begin(), spans_.end(), key, [](const Span& s, uint32_t k) { return s.key < k; });
    return it != spans_.end() && it->key == key ? &*it : nullptr;
}

bool MinimapStore::ensureOpen() {
    if (fd_ >= 0) return true;
    if (path_.empty()) return false;
    const uint32_t now = pp::logUptimeMs();
    if (openFailMs_ && now - openFailMs_ < 2000) return false;  // back off after a failed open (MFILE)
    fd_ = pp::pakOpenRetry(path_.c_str());  // + one retry after a rare pak gave one back
    if (fd_ < 0) {
        if (++logs_ <= 60) PSP_LOGE(pp::tag::Fs, "minimap_pak open err=%08X", static_cast<unsigned>(fd_));
        fd_ = -1;
        openFailMs_ = now ? now : 1;
        return false;
    }
    pp::pakNoteOpen(fd_, path_.c_str());  // in the descriptor budget
    openFailMs_ = 0;
    lastUseMs_ = now;
    return true;
}

// by key (was tile(x, y): the ADT tiles only; tile() is now this with tileKey).
const pp::Texture* MinimapStore::tileByKey(uint32_t key, uint32_t frame) {
    auto it = tiles_.find(key);
    if (it != tiles_.end()) {
        it->second.lastFrame = frame;
        return it->second.pending || it->second.failed ? nullptr : &it->second.tex;
    }
    const Span* s = find(key);
    if (!s || !ensureOpen()) return nullptr;
    StreamLoader::Request r;
    r.fd = fd_;
    r.offset = s->offset;
    r.size = s->size;
    r.kind = 1;
    r.id = key;
    if (!loader_.push(r)) return nullptr;
    Tile& t = tiles_[key];
    t.pending = true;
    t.lastFrame = frame;
    t.requestMs = pp::logUptimeMs();
    ++pending_;
    return nullptr;
}

void MinimapStore::update(uint32_t frame) {
    const uint32_t now = pp::logUptimeMs();
    if (!loader_.threaded()) loader_.pumpSync();  // minimapthread=0: one read per frame here
    StreamLoader::Result res;
    while (loader_.pop(res)) {
        lastUseMs_ = now;
        auto it = tiles_.find(res.req.id);
        if (it == tiles_.end() || !it->second.pending) {
            std::free(res.blob);
            continue;
        }
        Tile& t = it->second;
        t.pending = false;
        if (pending_) --pending_;
        if (!res.blob || !pp::textureFromBlob(res.blob, res.req.size, t.tex, true)) {
            std::free(res.blob);
            t.failed = true;
            if (++logs_ <= 60)
                PSP_LOGW(pp::tag::Gfx, "minimap_tile_fail x=%u y=%u", static_cast<unsigned>((res.req.id >> 8) & 0xFF),
                         static_cast<unsigned>(res.req.id & 0xFF));
            continue;
        }
        t.bytes = res.req.size;
        bytes_ += t.bytes;
        ++loads_;
        if (++logs_ <= 60)
            PSP_LOGI(pp::tag::Gfx, "minimap_tile x=%u y=%u kb=%u ms=%u", static_cast<unsigned>((res.req.id >> 8) & 0xFF),
                     static_cast<unsigned>(res.req.id & 0xFF), static_cast<unsigned>(t.bytes / 1024u),
                     static_cast<unsigned>(now - t.requestMs));
    }
    // Budget: least recently drawn first, never one of the last 2 frames (the GE may
    // still read it). Failed reads are forgotten after ~5 s so they are tried again.
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (it->second.failed && frame - it->second.lastFrame > 300) it = tiles_.erase(it);
        else ++it;
    }
    while (bytes_ > budget_) {
        auto victim = tiles_.end();
        for (auto it = tiles_.begin(); it != tiles_.end(); ++it) {
            if (it->second.pending || it->second.failed || frame - it->second.lastFrame <= 2) continue;
            if (victim == tiles_.end() || it->second.lastFrame < victim->second.lastFrame) victim = it;
        }
        if (victim == tiles_.end()) break;
        bytes_ -= victim->second.bytes;
        pp::textureRelease(victim->second.tex);
        tiles_.erase(victim);
    }
    if (fd_ >= 0 && loader_.inFlight() == 0 && now - lastUseMs_ > kCloseAfterMs) {
        pp::pakNoteClose(fd_);
        sceIoClose(fd_);
        fd_ = -1;
    }
}

bool MinimapStore::readBlocking(uint32_t offset, uint32_t size, void* dst) {
    if (!ensureOpen()) return false;
    const bool ok = pp::pakRead(fd_, offset, dst, size);
    lastUseMs_ = pp::logUptimeMs();
    return ok;
}

// ---------------------------------------------------------------------------
// MapUi
// ---------------------------------------------------------------------------

WorldScreen::MapUi::~MapUi() {
    releaseWorld();
    pp::textureRelease(border);
    pp::textureRelease(arrow);
    pp::textureRelease(cluster.atlas);
    pp::textureRelease(cluster.compass);
    pp::textureRelease(cluster.menuCheck);
    std::free(dotBlock);
}

void WorldScreen::destroyMapUi(MapUi* p) { delete p; }

void WorldScreen::initMap() {
    map_ = std::unique_ptr<MapUi, void (*)(MapUi*)>(new MapUi(), &WorldScreen::destroyMapUi);
    MapUi& m = *map_;
    m.on = pp::configGetInt("minimap", 1) != 0;
    m.rotate = pp::configGetInt("minimaprotate", 0) != 0;
    m.dots = pp::configGetInt("minimapdots", 1) != 0;
    m.subzone = pp::configGetInt("minimapsubzone", 1) != 0;
    m.borderArt = pp::configGetInt("minimapborder", 1) != 0;
    m.zoom = static_cast<float>(std::clamp(pp::configGetInt("minimapzoom", 200), 100, 800));
    m.borderDx = static_cast<float>(std::clamp(pp::configGetInt("minimapborderdx", 0), -20, 20));
    m.borderDy = static_cast<float>(std::clamp(pp::configGetInt("minimapborderdy", 0), -20, 20));
    const int kb = std::clamp(pp::configGetInt("minimapkb", 256)  /* was 384: the hardware heap */, 128, 2048);
    const bool thread = pp::configGetInt("minimapthread", 1) != 0;
    PSP_LOGI(pp::tag::Gfx, "minimap_cfg on=%d zoom=%d rotate=%d dots=%d subzone=%d border=%d kb=%d thread=%d",
             m.on ? 1 : 0, static_cast<int>(m.zoom), m.rotate ? 1 : 0, m.dots ? 1 : 0, m.subzone ? 1 : 0,
             m.borderArt ? 1 : 0, kb, thread ? 1 : 0);
    const uint32_t mapId = session_.worldEntry().mapId;
    const char* dir = mapDir(mapId);
    char rel[64], path[256];
    std::snprintf(rel, sizeof(rel), "data/minimap_%s.pak", dir ? dir : "none");
    pp::fsPath(path, sizeof(path), rel);
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    m.storeOk = dir && m.store.open(path, thread, static_cast<uint32_t>(kb) * 1024u);
    if (m.storeOk && m.store.header().h->mapId != mapId) {
        PSP_LOGW(pp::tag::Fs, "minimap_missing reason=map pak=%u world=%u", static_cast<unsigned>(m.store.header().h->mapId),
                 static_cast<unsigned>(mapId));
        m.storeOk = false;
    }
    if (m.storeOk) {
        const auto& h = *m.store.header().h;
        PSP_LOGI(pp::tag::Fs, "minimap_load ok=1 map=%s tiles=%u tile=%u areas=%u kb=%u ms=%u", h.mapName,
                 static_cast<unsigned>(h.tileCount), static_cast<unsigned>(h.tileSize), static_cast<unsigned>(h.areaCount),
                 static_cast<unsigned>((sizeof(h) + h.areaCount * sizeof(fmt::MinimapArea) + h.namesSize) / 1024u),
                 static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    } else {
        PSP_LOGW(pp::tag::Fs, "minimap_missing path=%s (noData disc, blips only)", rel);
    }
    // the WMO minimap and the dungeon world map of a dungeon's pak (a few KB, kept).
    if (m.storeOk) {
        auto readRec = [&](uint32_t key, std::vector<uint8_t>& out) {
            const MinimapStore::Span* sp = m.store.find(key);
            if (!sp) return false;
            out.resize(sp->size);
            return m.store.readBlocking(sp->offset, sp->size, out.data());
        };
        MapUi::Dungeon& d = m.dg;
        if (readRec(MinimapStore::kWmoIndexKey, d.wmmiBlob) &&
            !d.wmmi.attach(d.wmmiBlob.data(), static_cast<uint32_t>(d.wmmiBlob.size())))
            d.wmmiBlob.clear();
        if (readRec(MinimapStore::kDungeonKey, d.dmapBlob) &&
            !d.dmap.attach(d.dmapBlob.data(), static_cast<uint32_t>(d.dmapBlob.size())))
            d.dmapBlob.clear();
        d.inZoom = std::clamp(pp::configGetInt("minimapinzoom", 0), 0, 5);
        if (d.wmmi.h) {  // many small tiles: room for a view's worth (512 KB - a dungeon has no terrain tiles)
            m.store.setBudget(std::max<uint32_t>(m.store.budget(), 512u * 1024u));
            d.seen.assign(d.wmmi.h->groupCount, 0);
        }
        PSP_LOGI(pp::tag::Fs, "minimap_dungeon wmmi=%d inst=%u groups=%u tiles=%u dmap=%d floors=%u wma=%u inzoom=%d kb=%u",
                 d.wmmi.h ? 1 : 0, d.wmmi.h ? static_cast<unsigned>(d.wmmi.h->instanceCount) : 0u,
                 d.wmmi.h ? static_cast<unsigned>(d.wmmi.h->groupCount) : 0u,
                 d.wmmi.h ? static_cast<unsigned>(d.wmmi.h->tileCount) : 0u, d.dmap.h ? 1 : 0,
                 d.dmap.h ? static_cast<unsigned>(d.dmap.h->floorCount) : 0u, d.dmap.h ? static_cast<unsigned>(d.dmap.h->wma) : 0u,
                 d.inZoom, static_cast<unsigned>((d.wmmiBlob.size() + d.dmapBlob.size()) / 1024u));
    }
    // Art: ui.pak "mm_border" (UI-Minimap-Border ring), "mm_arrow" (MinimapArrow). Optional.
    // "mm_cluster" (the cluster's atlas, the ring in it: "mm_border" is then not read),
    // "mm_compass" (minimaprotate=1 only), "mm_pvp" (GetZonePVPInfo's AreaTable bits).
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak ui;
    uint32_t bytes = 0;
    int borderOk = 0, arrowOk = 0;
    auto& c = m.cluster;
    if (ui.open(path)) {
        if (loadUiArt(ui, "mm_cluster", c.atlas, c.artBytes)) borderOk = 1;
        else borderOk = loadUiArt(ui, "mm_border", m.border, bytes) ? 1 : 0;
        arrowOk = loadUiArt(ui, "mm_arrow", m.arrow, bytes) ? 1 : 0;
        if (m.rotate) (void)loadUiArt(ui, "mm_compass", c.compass, c.artBytes);
        if (const pp::PakEntry* e = ui.find("mm_pvp")) {
            if (void* blob = ui.load(e)) {
                const auto* w = static_cast<const uint32_t*>(blob);
                if (e->size >= 4 && w[0] <= (e->size - 4) / 4) c.pvpAreas.assign(w + 1, w + 1 + w[0]);
                pp::pakFree(blob);
            }
        }
        ui.close();
    }
    m.dotBlock = makeDot(m.dot);
    PSP_LOGI(pp::tag::Gfx, "minimap_art border=%d arrow=%d kb=%u", borderOk, arrowOk, static_cast<unsigned>(bytes / 1024u));
    // the player's faction for GetZonePVPInfo (ChrRaces: Alliance 1 3 4 7 11, Horde 2 5 6 8 10).
    if (const game::Character* me = session_.activeCharacter()) {
        switch (static_cast<int>(me->race)) {
            case 1: case 3: case 4: case 7: case 11: c.team = kPvpAlliance; break;
            case 2: case 5: case 6: case 8: case 10: c.team = kPvpHorde; break;
            default: break;
        }
    }
    PSP_LOGI(pp::tag::Gfx, "minimap_cluster atlas=%d compass=%d pvp=%u team=%u kb=%u", c.atlas.pixels ? 1 : 0,
             c.compass.pixels ? 1 : 0, static_cast<unsigned>(c.pvpAreas.size()), static_cast<unsigned>(c.team),
             static_cast<unsigned>((c.artBytes + c.pvpAreas.size() * 4u) / 1024u));
}

float WorldScreen::minimapReserveW() const { return map_ && map_->on ? kReserveW : 0.0f; }

// ---------------------------------------------------------------------------
// The HUD minimap
// ---------------------------------------------------------------------------

void WorldScreen::drawMinimap(UiRenderer& ui) {
    if (!map_) initMap();
    MapUi& m = *map_;
    // The window stack closes windows from several places (Circle, the bar): the world
    // map's data goes when its window is gone.
    if (m.world.open && !windowOpen(kWinWorldMap)) {
        const uint32_t artKb = m.worldBytes() / 1024u;  // + the World / Cosmic level's art
        m.releaseWorld();
        playUiSound(UiSound::QuestLogClose);  // WorldMapFrame_OnHide - igQuestLogClose
        PSP_LOGI(pp::tag::Gfx, "worldmap_close freed_kb=%u", static_cast<unsigned>(artKb));
    }
    if (!m.on) return;
    PROF_SCOPE("minimap");
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    const uint32_t now = pp::logUptimeMs();
    ++m.frame;
    if (m.storeOk) m.store.update(m.frame);
    session_.requestMinimapNotices();  // once per session - the mail and calendar notices
    PROF_GE_PASS("minimap");

    // indoors in a dungeon's WMO the view is the indoor zoom (half of its yards across).
    updateDungeonGroup(m);
    m.viewR = m.dg.indoor ? fmt::kIndoorZoomYards[m.dg.inZoom] * 0.5f : m.zoom;
    fmt::MinimapProj v;
    v.viewRadius = m.viewR;
    v.mapRadius = kRadius;
    if (m.rotate) {  // PSP: camera yaw for the map and the blips (see the file comment)
        pp::vmath::sincos(camYaw_, v.sinBearing, v.cosBearing);  // vrot
    }
    const float cx = kCenterX, cy = kCenterY;
    MapUi::Disc& D = m.disc;
    if (!D.tables) {  // once - the same numbers the per-frame code computed
        const float step = 6.28318530718f / static_cast<float>(kDiscSegments);
        for (int k = 0; k < kDiscSegments; ++k) {  // minimapDisc's circle
            D.unitCos[k] = std::cos(k * step);
            D.unitSin[k] = std::sin(k * step);
        }
        D.backdrop[0] = {0, 0, 0xE0000000u, cx, cy, 0};
        for (int k = 0; k <= kDiscSegments; ++k) {
            float sa = 0.0f, ca = 0.0f;
            pp::vmath::sincos(k * 6.28318530718f / kDiscSegments, sa, ca);  // vrot
            D.backdrop[k + 1] = {0, 0, 0xE0000000u, cx + ca * (kRadius + 2.0f), cy + sa * (kRadius + 2.0f), 0};
        }
        D.tables = true;
    }

    ui.beginFrame();
    // 1. Backdrop disc 2 px larger: the dark edge upstream's shader draws (border ring).
    pp::textureBind(nullptr);
    mapGeFan(D.backdrop, kDiscSegments + 2);
    // 2. The disc, one fan per tile piece. Clipped again only when the view moved.
    // indoors the WMO tiles instead (drawIndoorMinimap; the outdoor cache is rebuilt after).
    if (m.dg.indoor) {
        drawIndoorMinimap(m, v.sinBearing, v.cosBearing, cx, cy);
        D.valid = false;
    } else if (!D.valid || D.px != playerX_ || D.py != playerY_ || D.zoom != m.viewR || D.sinB != v.sinBearing ||
        D.cosB != v.cosBearing) {
        fmt::MinimapVert disc[kDiscSegments], out[kDiscSegments + 8], tmp[kDiscSegments + 8];
        for (int k = 0; k < kDiscSegments; ++k) {  // fmt::minimapDisc with the circle above
            const float ox = D.unitCos[k] * v.mapRadius, oy = D.unitSin[k] * v.mapRadius;
            float dx = 0.0f, dy = 0.0f;
            fmt::minimapOffsetToDelta(ox, oy, v, dx, dy);
            disc[k].sx = cx + ox;
            disc[k].sy = cy + oy;
            fmt::minimapTileCoords(playerX_ + dx, playerY_ + dy, disc[k].gx, disc[k].gy);
        }
        int x0, y0, x1, y1;
        fmt::minimapTileRange(disc, kDiscSegments, x0, y0, x1, y1);
        D.pieceCount = 0;
        int used = 0;
        for (int ty = y0; ty <= y1; ++ty) {
            for (int tx = x0; tx <= x1; ++tx) {
                const int pn = fmt::minimapClipToTile(disc, kDiscSegments, tx, ty, out, tmp);
                if (!pn) continue;
                if (D.pieceCount >= MapUi::Disc::kMaxPieces || used + pn > MapUi::Disc::kMaxVerts) continue;
                D.pieces[D.pieceCount++] = {static_cast<int16_t>(tx), static_cast<int16_t>(ty), static_cast<uint16_t>(used),
                                            static_cast<uint16_t>(pn)};
                std::memcpy(&D.verts[used], out, static_cast<size_t>(pn) * sizeof(fmt::MinimapVert));
                used += pn;
            }
        }
        D.px = playerX_;
        D.py = playerY_;
        D.zoom = m.viewR;
        D.sinB = v.sinBearing;
        D.cosB = v.cosBearing;
        D.valid = true;
        ++m.discBuilds;
    }
    m.tilesDrawn = 0;
    const int tileSize = m.storeOk ? m.store.header().h->tileSize : 128;
    if (m.dg.indoor) D.pieceCount = 0;  // drawn above
    // Texels per screen pixel -> mip level (2^level <= that).
    const float texPerPx = tileSize / fmt::kMinimapTileYards * (m.viewR / kRadius);
    int wantLevel = 0;
    while (wantLevel < 7 && static_cast<float>(2 << wantLevel) <= texPerPx) ++wantLevel;
    for (int i = 0; i < D.pieceCount; ++i) {
        const MapUi::Disc::Piece& pc = D.pieces[i];
        const int tx = pc.tx, ty = pc.ty, pn = pc.count;
        const fmt::MinimapVert* out = &D.verts[pc.first];
        const pp::Texture* t = m.storeOk && tx >= 0 && ty >= 0 && tx < 64 && ty < 64 ? m.store.tile(tx, ty, m.frame) : nullptr;
        MapGeVert gv[kDiscSegments + 8];
        if (t) {
            const int level = std::min(wantLevel, t->levels - 1);
            const float tw = static_cast<float>(t->w >> level), th = static_cast<float>(t->h >> level);
            for (int k = 0; k < pn; ++k)
                gv[k] = {(out[k].gx - tx) * tw, (out[k].gy - ty) * th, 0xFFFFFFFFu, out[k].sx, out[k].sy, 0};
            bindTileLevel(*t, level);
            ++m.tilesDrawn;
        } else {
            for (int k = 0; k < pn; ++k) gv[k] = {0, 0, kNoDataABGR, out[k].sx, out[k].sy, 0};
            pp::textureResetBinding();
        }
        mapGeFan(gv, pn);
    }
    pp::textureResetBinding();  // forget the manual binds
    // Read ahead: the 3 x 3 around the player's tile, while the budget has room.
    if (m.storeOk && !m.dg.indoor && (m.frame % 30) == 0 && m.store.pending() == 0 &&
        m.store.cachedBytes() + 3u * 24u * 1024u < m.store.budget()) {
        float gx, gy;
        fmt::minimapTileCoords(playerX_, playerY_, gx, gy);
        const int px = static_cast<int>(std::floor(gx)), py = static_cast<int>(std::floor(gy));
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (px + dx >= 0 && py + dy >= 0 && px + dx < 64 && py + dy < 64) (void)m.store.tile(px + dx, py + dy, m.frame);
    }
    const uint32_t tDisc = sceKernelGetSystemTimeLow();

    // 3. Blips (upstream renderMinimapMarkers order). The list every 8 frames (or when the
    // units / the zoom changed), the positions every frame.
    if (m.blipFrame == 0 || m.frame - m.blipFrame >= 8 || session_.entityVersion() != m.blipEntityVersion ||
        m.blipZoom != m.viewR)
        refreshMinimapBlips();
    auto project = [&](const glm::vec3& pos, float& sx, float& sy) {
        float ox = 0, oy = 0;
        fmt::minimapDeltaToOffset(pos.x - playerX_, pos.y - playerY_, v, ox, oy);
        if (ox * ox + oy * oy > (kRadius - 3.0f) * (kRadius - 3.0f)) return false;  // upstream: mapRadius - 3
        sx = cx + ox;
        sy = cy + oy;
        return true;
    };
    auto dot = [&](float sx, float sy, float r, uint32_t col) {
        if (m.dot.pixels) ui.image(m.dot, Rect{sx - r, sy - r, 2 * r, 2 * r}, col);
        else ui.fill(Rect{sx - r, sy - r, 2 * r, 2 * r}, col);
    };
    const Font& small = kit_.font(kFontSmall);
    m.blips = 0;
    for (const MapUi::Blip& b : m.blipList) {
        glm::vec3 pos;
        float h = 0.0f, sx = 0.0f, sy = 0.0f;
        if (!unitPosition(b.guid, pos, h) || !project(pos, sx, sy)) continue;
        if (b.outer) dot(sx, sy, b.ro, b.outer);
        dot(sx, sy, b.ri, b.inner);
        if (b.symbol) ui.text(small, sx, sy - 6.0f, b.symbol, 0xFF000000u, Align::Center);
        if (b.counted) ++m.blips;
    }
    ui.endFrame();
    const uint32_t tBlip = sceKernelGetSystemTimeLow();

    // 4. The player arrow over the blips (raw quad, own draw).
    ui.beginFrame();
    // The ghost's corpse (upstream renderMinimapCorpseMarker), under the arrow: an X in
    // a ring on the disc, else a triangle at the rim pointing at it (ui_world_death.cpp).
    if (session_.life() == pp::WorldSession::Life::Ghost && session_.corpseKnown()) {
        const glm::vec3& c = session_.corpsePosition();
        float ox = 0, oy = 0;
        fmt::minimapDeltaToOffset(c.x - playerX_, c.y - playerY_, v, ox, oy);
        const float len = std::sqrt(ox * ox + oy * oy);
        if (len <= kRadius - 3.0f) mapDrawCorpseMark(cx + ox, cy + oy);
        else if (len > 1e-3f)
            mapDrawCorpsePointer(cx + ox / len * (kRadius - 7.0f), cy + oy / len * (kRadius - 7.0f), ox / len, oy / len);
    }
    {
        float ax = 0, ay = 0;
        fmt::minimapDeltaToOffset(std::cos(facing_), std::sin(facing_), v, ax, ay);
        mapDrawArrow(m.arrow.pixels ? &m.arrow : nullptr, cx, cy, ax, ay, 16.0f);
    }
    // MinimapCompassTexture (rotateMinimap = 1), its "N" to the north (raw, before the
    // cluster's art; it lies outside the ring).
    if (m.rotate && m.cluster.compass.pixels) {
        float nx = 0, ny = 0;
        fmt::minimapDeltaToOffset(1.0f, 0.0f, v, nx, ny);
        mapDrawArrow(&m.cluster.compass, cx + kCompassDx, cy, nx, ny, kCompassSize);
    }
    // 5. the cluster - zone bar and text, ring, north tag, buttons (was: the ring, the zone
    // name above it and "+" / "-" kit buttons in cursor mode).
    refreshMinimapCluster();
    drawMinimapCluster(ui);
    ui.endFrame();

    // 10 s summary (no per-frame lines).
    const uint32_t tEnd = sceKernelGetSystemTimeLow();
    const uint32_t us = tEnd - t0;
    m.cpuSumUs += us;
    m.cpuMaxUs = std::max(m.cpuMaxUs, us);
    m.discUsSum += tDisc - t0;
    m.blipUsSum += tBlip - tDisc;
    m.clusterUsSum += tEnd - tBlip;
    ++m.cpuFrames;
    if (now >= m.nextLogMs) {
        if (m.nextLogMs != 0 && m.cpuFrames) {
            PSP_LOGI(pp::tag::Gfx, "minimap_frame cpu_us=%u max=%u tiles=%u cached=%u kb=%u loads=%u pending=%u blips=%u zoom=%d rot=%d",
                     static_cast<unsigned>(m.cpuSumUs / m.cpuFrames), static_cast<unsigned>(m.cpuMaxUs),
                     static_cast<unsigned>(m.tilesDrawn), static_cast<unsigned>(m.store.cachedTiles()),
                     static_cast<unsigned>(m.store.cachedBytes() / 1024u), static_cast<unsigned>(m.store.tilesLoaded()),
                     static_cast<unsigned>(m.store.pending()), static_cast<unsigned>(m.blips), static_cast<int>(m.zoom),
                     m.rotate ? 1 : 0);
            // where the time goes (avg us a frame) and how often the caches were rebuilt
            PSP_LOGI(pp::tag::Gfx, "minimap_parts disc_us=%u blip_us=%u ui_us=%u frames=%u disc_builds=%u blip_builds=%u list=%u",
                     static_cast<unsigned>(m.discUsSum / m.cpuFrames), static_cast<unsigned>(m.blipUsSum / m.cpuFrames),
                     static_cast<unsigned>(m.clusterUsSum / m.cpuFrames), static_cast<unsigned>(m.cpuFrames),
                     static_cast<unsigned>(m.discBuilds), static_cast<unsigned>(m.blipBuilds),
                     static_cast<unsigned>(m.blipList.size()));
        }
        m.cpuSumUs = m.cpuMaxUs = m.cpuFrames = 0;
        m.discUsSum = m.blipUsSum = m.clusterUsSum = m.discBuilds = m.blipBuilds = 0;
        m.nextLogMs = now + 10000;
    }
}

// ---------------------------------------------------------------------------
// the WMO minimap of a dungeon
// ---------------------------------------------------------------------------

namespace {

// Minimap_ZoomIn/OutClick's limits: outdoors upstream's 100..800 yd, indoors the six levels of kIndoorZoomYards.
bool minimapCanZoom(const WorldScreen::MapUi& m, bool in) {
    if (m.dg.indoor) return in ? m.dg.inZoom < 5 : m.dg.inZoom > 0;
    return in ? m.zoom > kZoomMin + 0.5f : m.zoom < kZoomMax - 0.5f;
}

// The indoor zoom kept apart from the outdoor one (as the public CVar minimapInsideZoom): wowee.cfg minimapinzoom.
bool minimapIndoorZoom(WorldScreen::MapUi& m, bool in) {
    m.dg.inZoom = std::clamp(m.dg.inZoom + (in ? 1 : -1), 0, 5);
    PSP_LOGI(pp::tag::Ctrl, "minimap_inzoom level=%d yd=%d", m.dg.inZoom, static_cast<int>(fmt::kIndoorZoomYards[m.dg.inZoom]));
    pp::configSet("minimapinzoom", std::to_string(m.dg.inZoom));
    pp::configSave();
    return true;
}

}  // namespace

// The player's WMO instance and group: the smallest box among the interior groups (flag 0x2000) holding the player
// (upstream WMORenderer::findContainingGroup picks the smallest box; isInsideInteriorWMO tests interior groups only),
// the previous group kept while it still holds them (PSP). Indoors = there is such a group.
void WorldScreen::updateDungeonGroup(MapUi& m) {
    MapUi::Dungeon& d = m.dg;
    d.indoor = false;
    if (!d.wmmi.h) {
        d.inst = d.group = -1;
        return;
    }
    const fmt::WmoMinimapView& v = d.wmmi;
    auto holds = [&](const fmt::WmoMinimapGroup& g, float x, float y, float z) {
        return x >= g.bmin[0] - 0.5f && x <= g.bmax[0] + 0.5f && y >= g.bmin[1] - 0.5f && y <= g.bmax[1] + 0.5f &&
               z >= g.bmin[2] - 1.0f && z <= g.bmax[2] + 1.0f;
    };
    int bestInst = -1, bestGroup = -1;
    float bestVol = 1e30f;
    for (uint32_t i = 0; i < v.h->instanceCount; ++i) {
        const fmt::WmoMinimapInstance& in = v.inst[i];
        if (playerX_ < in.bmin[0] - 2.0f || playerX_ > in.bmax[0] + 2.0f || playerY_ < in.bmin[1] - 2.0f ||
            playerY_ > in.bmax[1] + 2.0f || playerZ_ < in.bmin[2] - 2.0f || playerZ_ > in.bmax[2] + 2.0f)
            continue;
        float lx, ly, lz;
        fmt::wmoMinimapApply(in.toLocal, playerX_, playerY_, playerZ_, lx, ly, lz);
        for (uint32_t g = in.firstGroup; g < uint32_t{in.firstGroup} + in.groupCount; ++g) {
            const fmt::WmoMinimapGroup& G = v.groups[g];
            if (!(G.flags & 0x2000u) || !holds(G, lx, ly, lz)) continue;
            if (static_cast<int>(g) == d.group) {  // sticky: the previous group while it holds the player
                bestInst = static_cast<int>(i);
                bestGroup = static_cast<int>(g);
                bestVol = -1.0f;
                break;
            }
            const float vol = (G.bmax[0] - G.bmin[0]) * (G.bmax[1] - G.bmin[1]) * (G.bmax[2] - G.bmin[2]);
            if (vol < bestVol) {
                bestVol = vol;
                bestInst = static_cast<int>(i);
                bestGroup = static_cast<int>(g);
            }
        }
        if (bestVol < 0.0f) break;
    }
    if (bestGroup != d.group) {
        ++d.groupChanges;
        if (d.groupChanges <= 200)
            PSP_LOGI(pp::tag::Gfx, "minimap_wmo_group inst=%d group=%d id=%u flags=%08x",
                     bestInst, bestGroup, bestGroup >= 0 ? static_cast<unsigned>(v.groups[bestGroup].wmoGroupId) : 0u,
                     bestGroup >= 0 ? static_cast<unsigned>(v.groups[bestGroup].flags) : 0u);
    }
    d.inst = bestInst;
    d.group = bestGroup;
    d.indoor = bestGroup >= 0;
}

// The indoor minimap (see the file comment): the disc filled with the noData colour, then the view box around the
// player (XY: the disc's square; Z: kIndoorBandYards up and down from the feet), the portal walk from the player's
// group, the tiles of the reached groups that meet the box, each a fan of the disc clipped to the tile - the groups
// farther from the feet in height first, so the nearest ones (the player's own group: 0) end on top.
void WorldScreen::drawIndoorMinimap(MapUi& m, float sinB, float cosB, float cx, float cy) {
    MapUi::Dungeon& d = m.dg;
    fmt::MinimapProj v;
    v.viewRadius = m.viewR;
    v.mapRadius = kRadius;
    v.sinBearing = sinB;
    v.cosBearing = cosB;
    const fmt::WmoMinimapView& mv = d.wmmi;
    if (d.inst < 0 || d.group < 0) return;
    const fmt::WmoMinimapInstance& in = mv.inst[d.inst];
    // The view box, WMO local (the world box's 8 corners through toLocal).
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 8; ++k) {
        const float wx = playerX_ + ((k & 1) ? m.viewR : -m.viewR), wy = playerY_ + ((k & 2) ? m.viewR : -m.viewR);
        const float wz = (k & 4) ? playerZ_ + kIndoorBandYards : playerZ_ - kIndoorBandYards;
        float l[3];
        fmt::wmoMinimapApply(in.toLocal, wx, wy, wz, l[0], l[1], l[2]);
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], l[a]);
            hi[a] = std::max(hi[a], l[a]);
        }
    }
    uint8_t* seen = d.seen.data() + in.firstGroup;
    std::fill(seen, seen + in.groupCount, 0);
    d.groupsWalked = static_cast<uint32_t>(fmt::wmoMinimapWalk(mv, in, static_cast<uint32_t>(d.group), lo, hi, seen));
    float plx, ply, plz;
    fmt::wmoMinimapApply(in.toLocal, playerX_, playerY_, playerZ_, plx, ply, plz);
    struct Item {
        float key;
        uint32_t tile;
    };
    Item items[96];
    int n = 0;
    for (uint32_t g = in.firstGroup; g < uint32_t{in.firstGroup} + in.groupCount && n < 96; ++g) {
        if (!seen[g - in.firstGroup]) continue;
        const fmt::WmoMinimapGroup& G = mv.groups[g];
        // draw key: the height between the feet and the group's box (0 when the box spans the feet's height)
        const float key = static_cast<int>(g) == d.group ? 0.0f : std::max({G.bmin[2] - plz, plz - G.bmax[2], 0.0f});
        for (uint32_t t = G.firstTile; t < G.firstTile + G.tileCount && n < 96; ++t) {
            const fmt::WmoMinimapTile& T = mv.tiles[t];
            if (!T.baked) continue;
            const float x1 = T.x0 + T.pxW * fmt::kWmoMinimapYardsPerPixel, y1 = T.y0 + T.pxH * fmt::kWmoMinimapYardsPerPixel;
            if (x1 < lo[0] || T.x0 > hi[0] || y1 < lo[1] || T.y0 > hi[1]) continue;
            items[n++] = {key, t};
        }
    }
    std::sort(items, items + n,
              [](const Item& a, const Item& b) { return a.key > b.key || (a.key == b.key && a.tile < b.tile); });
    // The disc in WMO local coordinates (gx / gy = local x / y).
    fmt::MinimapVert disc[kDiscSegments], out[kDiscSegments + 8], tmp[kDiscSegments + 8];
    const MapUi::Disc& D = m.disc;
    {  // the noData fill (upstream noDataTexture's colour, as the outdoor pieces without a tile)
        MapGeVert fill[kDiscSegments + 2];
        fill[0] = {0, 0, kNoDataABGR, cx, cy, 0};
        for (int k = 0; k <= kDiscSegments; ++k) {
            const int j = k % kDiscSegments;
            fill[k + 1] = {0, 0, kNoDataABGR, cx + D.unitCos[j] * kRadius, cy + D.unitSin[j] * kRadius, 0};
        }
        pp::textureResetBinding();
        mapGeFan(fill, kDiscSegments + 2);
    }
    for (int k = 0; k < kDiscSegments; ++k) {
        const float ox = D.unitCos[k] * v.mapRadius, oy = D.unitSin[k] * v.mapRadius;
        float dx = 0.0f, dy = 0.0f, lz = 0.0f;
        fmt::minimapOffsetToDelta(ox, oy, v, dx, dy);
        disc[k].sx = cx + ox;
        disc[k].sy = cy + oy;
        fmt::wmoMinimapApply(in.toLocal, playerX_ + dx, playerY_ + dy, playerZ_, disc[k].gx, disc[k].gy, lz);
    }
    d.tilesDrawn = 0;
    for (int i = 0; i < n; ++i) {
        const fmt::WmoMinimapTile& T = mv.tiles[items[i].tile];
        const float pw = T.pxW * fmt::kWmoMinimapYardsPerPixel, ph = T.pxH * fmt::kWmoMinimapYardsPerPixel;
        int pn = fmt::minimapClipAxis(disc, kDiscSegments, tmp, 0, T.x0, true);
        if (pn >= 3) pn = fmt::minimapClipAxis(tmp, pn, out, 0, T.x0 + pw, false);
        if (pn >= 3) pn = fmt::minimapClipAxis(out, pn, tmp, 1, T.y0, true);
        if (pn >= 3) pn = fmt::minimapClipAxis(tmp, pn, out, 1, T.y0 + ph, false);
        if (pn < 3) continue;
        const pp::Texture* t = m.store.tileByKey(MinimapStore::wmoTileKey(items[i].tile), m.frame);
        if (!t) continue;  // being read: the noData fill this frame
        // u along +x from the tile's corner, the texture's row 0 at the tile's +y edge (see the file comment).
        const float texPerPx = static_cast<float>(t->w) / pw * (m.viewR / kRadius);
        int level = 0;
        while (level < 7 && static_cast<float>(2 << level) <= texPerPx) ++level;
        level = std::min(level, t->levels - 1);
        const float tw = static_cast<float>(t->w >> level), th = static_cast<float>(t->h >> level);
        MapGeVert gv[kDiscSegments + 8];
        for (int k = 0; k < pn; ++k)
            gv[k] = {(out[k].gx - T.x0) / pw * tw, (T.y0 + ph - out[k].gy) / ph * th, 0xFFFFFFFFu, out[k].sx, out[k].sy, 0};
        bindTileLevel(*t, level);
        mapGeFan(gv, pn);
        ++d.tilesDrawn;
    }
    pp::textureResetBinding();
    m.tilesDrawn = d.tilesDrawn;
}

// which unit gets which blip - what drawMinimap decided every frame before
// (same tests, same order, same colours), kept in MapUi::blipList.
void WorldScreen::refreshMinimapBlips() {
    MapUi& m = *map_;
    m.blipFrame = m.frame;
    m.blipEntityVersion = session_.entityVersion();
    m.blipZoom = m.viewR;
    ++m.blipBuilds;
    m.blipList.clear();
    // upstream refreshQuestObjectiveCache (PSP: every quest in the log is "tracked").
    {
        uint64_t sig = 1469598103934665603ull;
        auto mix = [&](uint64_t value) {
            sig ^= value;
            sig *= 1099511628211ull;
        };
        const auto& log = session_.getQuestLog();
        mix(log.size());
        for (const auto& q : log) {
            if (q.complete || q.questId == 0) continue;
            mix(q.questId);
            for (const auto& o : q.killObjectives) {
                if (o.required == 0) continue;
                const uint32_t entry = static_cast<uint32_t>(o.npcOrGoId > 0 ? o.npcOrGoId : -o.npcOrGoId);
                auto c = q.killCounts.find(entry);
                mix(static_cast<uint32_t>(o.npcOrGoId));
                mix(o.required);
                mix(c == q.killCounts.end() ? 0 : c->second.first);
            }
        }
        if (sig != m.questSig) {
            m.questSig = sig;
            m.questCreatures.clear();
            for (const auto& q : log) {
                if (q.complete || q.questId == 0) continue;
                for (const auto& o : q.killObjectives) {
                    if (o.required == 0 || o.npcOrGoId <= 0) continue;
                    const uint32_t entry = static_cast<uint32_t>(o.npcOrGoId);
                    auto c = q.killCounts.find(entry);
                    if (c != q.killCounts.end() && c->second.first >= c->second.second) continue;
                    m.questCreatures.insert(entry);
                }
            }
        }
    }
    const uint16_t fHealth = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
    const uint16_t fNpcFlags = game::fieldIndex(game::UF::UNIT_NPC_FLAGS);
    const uint16_t fEntry = game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY);
    const uint16_t fDyn = game::fieldIndex(game::UF::UNIT_DYNAMIC_FLAGS);
    // Units beyond 1.5 x the view radius are left out (as before); 8 frames cannot bring one onto the disc.
    const float reach = m.viewR * 1.5f;
    for (const auto& [guid, e] : session_.entities().getEntities()) {
        if (!e || guid == session_.playerGuid()) continue;
        const game::ObjectType type = e->getType();
        if (type != game::ObjectType::UNIT && type != game::ObjectType::PLAYER) continue;
        // Cheap reject (canonical x = server y).
        if (std::abs(e->getY() - playerX_) > reach || std::abs(e->getX() - playerY_) > reach) continue;
        if (type == game::ObjectType::PLAYER) {
            if (m.dots) m.blipList.push_back({guid, 0, abgr(80, 160, 255, 220), 0.0f, 1.5f, nullptr, true});  // renderMinimapPlayerDots
            continue;
        }
        if (e->getField(fHealth) == 0) {  // renderMinimapLootCorpses: dead and lootable
            if (e->getField(fDyn) & game::UNIT_DYNFLAG_LOOTABLE)
                m.blipList.push_back({guid, 0, abgr(180, 230, 80, 230), 0.0f, 2.5f, nullptr, false});
            continue;
        }
        const uint32_t flags = e->getField(fNpcFlags);
        if (flags & game::NPC_FLAG_FLIGHT_MASTER) {  // renderMinimapFlightMasters
            m.blipList.push_back({guid, abgr(70, 45, 0, 230), abgr(255, 215, 0, 245), 3.5f, 2.5f, nullptr, true});
            continue;
        }
        if (!m.dots) continue;
        if (m.questCreatures.count(e->getField(fEntry))) {  // renderMinimapNpcDots: quest target
            m.blipList.push_back({guid, abgr(80, 50, 0, 180), abgr(255, 210, 30, 240), 3.0f, 2.3f, nullptr, true});
        } else {
            m.blipList.push_back({guid, 0, session_.isHostileUnit(guid) ? abgr(220, 70, 70, 220) : abgr(245, 245, 245, 210),
                                  0.0f, 1.2f, nullptr, true});
        }
    }
    // renderMinimapQuestGivers: ! / ? on a dot (status 9 stays off the minimap).
    for (const auto& [guid, status] : session_.getNpcQuestStatuses()) {
        const game::QuestGiverMarker mk = game::questGiverMarker(status);
        if (!mk.symbol || !mk.onMinimap) continue;
        m.blipList.push_back({guid, 0, mk.dim ? abgr(160, 160, 160, 255) : abgr(255, 210, 0, 255), 0.0f, 4.5f, mk.symbol,
                              true});
    }
}

// the cluster's state - the zone text twice a second (as before), the rest
// every 8 frames.
void WorldScreen::refreshMinimapCluster() {
    MapUi& m = *map_;
    auto& c = m.cluster;
    const uint32_t now = pp::logUptimeMs();
    // The dropdown's check, 3 frames after the dropdown was last drawn (the GE may still read it before).
    if (c.menuCheck.pixels && !windowOpen(kWinMinimapTracking) && m.frame - c.menuFrame > 3) pp::textureRelease(c.menuCheck);
    // Zone name (renderMinimapReadouts): refreshed twice a second.
    if (m.frame % 30 == 1 || m.shownId == ~0u) {
        uint32_t area = 0;
        int tx = 0, ty = 0, chunk = 0;
        if (terrain_.chunkAt(playerX_, playerY_, tx, ty, chunk)) area = zoneMusic_.areaAt(tx, ty, chunk);
        if (area) m.lastArea = area;
        else area = m.lastArea;
        // upstream ZoneManager::resolveAreaZoneId over the pak's AreaTable parents
        uint32_t zone = area;
        for (int depth = 0; zone && m.storeOk && depth < 16; ++depth) {
            const fmt::MinimapArea* a = m.store.header().area(zone);
            if (!a || a->parent == 0 || a->parent == zone) break;
            zone = a->parent;
        }
        const uint32_t server = session_.worldStateZoneId();
        if (!zone) zone = server;
        if (server && zone && server != zone) {
            // upstream "Zone disagreement": said once per pair
            const uint64_t pair = (uint64_t{server} << 32) | zone;
            if (pair != m.lastMismatch) {
                m.lastMismatch = pair;
                PSP_LOGW(pp::tag::Gfx, "minimap_zone_mismatch server=%u terrain=%u area=%u", static_cast<unsigned>(server),
                         static_cast<unsigned>(zone), static_cast<unsigned>(area));
            }
        }
        const uint32_t id = m.subzone && area ? area : zone;
        if (id != m.shownId) {
            m.shownId = id;
            const char* name = m.storeOk ? m.store.header().name(id) : nullptr;
            if (!name && m.storeOk) name = m.store.header().name(zone);
            m.zoneText = name ? name : "";
            PSP_LOGI(pp::tag::Gfx, "minimap_zone area=%u zone=%u named=%d", static_cast<unsigned>(area),
                     static_cast<unsigned>(zone), name ? 1 : 0);
        }
        m.lastZone = zone;
        // GetZonePVPInfo (Minimap_Update's colour) and GetZoneText / GetSubZoneText (the tooltip)
        if (area != c.pvpArea || zone != c.pvpZone) {
            c.pvpArea = area;
            c.pvpZone = zone;
            auto bitsOf = [&](uint32_t areaId) -> uint8_t {
                const uint32_t key = areaId << 8;
                auto it = std::lower_bound(c.pvpAreas.begin(), c.pvpAreas.end(), key);
                return it != c.pvpAreas.end() && (*it >> 8) == areaId ? static_cast<uint8_t>(*it & 0xFF) : 0;
            };
            const uint8_t ab = bitsOf(area ? area : zone), zb = bitsOf(zone);
            const uint8_t teamBits = (ab & (kPvpAlliance | kPvpHorde)) ? (ab & (kPvpAlliance | kPvpHorde))
                                                                       : (zb & (kPvpAlliance | kPvpHorde));
            const uint32_t mapId = session_.worldEntry().mapId;
            const bool continent = mapId == 0 || mapId == 1 || mapId == 530 || mapId == 571;
            if (ab & kPvpSanctuary) c.pvpType = kPvpSanctuaryType;
            else if (ab & kPvpArena) c.pvpType = kPvpArenaType;
            else if (teamBits && c.team) c.pvpType = (teamBits & c.team) ? kPvpFriendly : kPvpHostile;
            else if (!teamBits && continent && zone) c.pvpType = kPvpContested;
            else c.pvpType = kPvpNone;
            const char* zn = m.storeOk && zone ? m.store.header().name(zone) : nullptr;
            const char* sn = m.storeOk && area ? m.store.header().name(area) : nullptr;
            c.zoneName = zn ? zn : "";
            c.subzoneName = sn && c.zoneName != sn ? sn : "";  // Minimap_SetTooltip: subzone == zone -> ""
            PSP_LOGI(pp::tag::Gfx, "minimap_pvp area=%u zone=%u bits=%u/%u type=%u", static_cast<unsigned>(area),
                     static_cast<unsigned>(zone), static_cast<unsigned>(ab), static_cast<unsigned>(zb),
                     static_cast<unsigned>(c.pvpType));
        }
    }
    if (c.trackInit && (m.frame & 7) != 1) return;  // the rest every 8 frames
    // MiniMapTracking: upstream trackingSpells (when the known spells change) and trackingActive.
    if (session_.knownSpellsVersion() != c.trackKnownVersion) {
        c.trackKnownVersion = session_.knownSpellsVersion();
        c.trackSpells.clear();
        for (uint32_t id : session_.knownSpells()) {
            const pp::SpellInfo* si = spellDb_.get(id);
            if (!si) continue;
            for (uint16_t aura : si->rec.aura) {
                if (aura == 44 || aura == 45) {  // SPELL_AURA_TRACK_CREATURES / _RESOURCES
                    c.trackSpells.push_back(id);
                    break;
                }
            }
        }
        std::sort(c.trackSpells.begin(), c.trackSpells.end(),
                  [&](uint32_t a, uint32_t b) { return spellDb_.name(a) < spellDb_.name(b); });
    }
    uint32_t active = 0;
    for (const auto& au : session_.playerAuras()) {
        if (au.isEmpty()) continue;
        if (std::find(c.trackSpells.begin(), c.trackSpells.end(), au.spellId) != c.trackSpells.end()) {
            active = au.spellId;
            break;
        }
    }
    if (active != c.trackActive || !c.trackInit) {  // MiniMapTracking_Update: a new texture -> the shine
        c.shineMs = now ? now : 1;
        PSP_LOGI(pp::tag::Gfx, "minimap_track spells=%u active=%u", static_cast<unsigned>(c.trackSpells.size()),
                 static_cast<unsigned>(active));
    }
    c.trackActive = active;
    c.trackInit = true;
    // GameTimeFrame_OnEvent (CALENDAR_UPDATE_PENDING_INVITES): a rising count lights the invites, 0 takes them off.
    const uint32_t invites = session_.getCalendarPendingInvites();
    if (invites != c.sessionInvites) {
        c.sessionInvites = invites;
        if (invites > c.pendingInvites) {
            c.flashInvite = true;
            c.flashMs = now;
            c.pendingInvites = invites;
        } else if (invites == 0) {
            c.flashInvite = false;
            c.pendingInvites = 0;
        }
    }
    // TimeManagerClockButton / GameTimeFrame: GetGameTime (the realm's hour, run on between packets as the
    // light does; before the server says, the PSP's clock - upstream lua_GetGameTime) and CalendarGetDate.
    const float serverHours = session_.getGameTime();
    float hours;
    if (serverHours >= 0.0f) {
        hours = std::fmod(serverHours + static_cast<float>(now - session_.gameTimeReceivedMs()) / 3600000.0f, 24.0f);
    } else {
        const std::tm lt = core::localTime(std::time(nullptr));
        hours = static_cast<float>(lt.tm_hour) + static_cast<float>(lt.tm_min) / 60.0f;
    }
    const int minutes = static_cast<int>(hours * 60.0f + 0.01f) % 1440;
    if (minutes != c.clockMinutes) {
        c.clockMinutes = minutes;
        static const bool military = pp::configGetInt("clock24", 1) != 0;
        formatTime(c.clockText, sizeof(c.clockText), static_cast<unsigned>(minutes), military, false);  // TimeManagerClockTicker
        int day = 0;
        game::WowDate d;
        uint32_t recvMs = 0;
        if (session_.realmDate(d, recvMs)) {
            day = d.day;
            int month = d.month, year = d.fullYear();
            int add = (d.hour * 60 + d.minute + static_cast<int>((now - recvMs) / 60000u)) / 1440;
            while (add-- > 0) {
                if (++day > daysInMonth(month, year)) {
                    day = 1;
                    if (++month > 12) {
                        month = 1;
                        ++year;
                    }
                }
            }
        } else {
            day = core::localTime(std::time(nullptr)).tm_mday;
        }
        if (day != c.day) {
            c.day = day;
            std::snprintf(c.dayText, sizeof(c.dayText), "%u", static_cast<unsigned>(day) % 100u);  // GameTimeFrame_SetDate
        }
    }
}

// the cluster's art and buttons (pass "minimap", after the disc and the arrow).
void WorldScreen::drawMinimapCluster(UiRenderer& ui) {
    MapUi& m = *map_;
    auto& c = m.cluster;
    const bool focusable = uiMode_ && !settings_.isOpen();
    const Font& small = kit_.font(kFontSmall);
    const pp::Texture* A = c.atlas.pixels ? &c.atlas : nullptr;
    const uint32_t now = pp::logUptimeMs();
    c.tipId = 0;
    auto stop = [&](uint32_t id, const Rect& r) {
        if (!focusable || !winFocus_.addStop(id, r)) return false;
        c.tipId = id;
        c.tipRect = r;
        return true;
    };
    auto spr = [&](const Rect& r, const SrcRect& s, uint32_t col = 0xFFFFFFFFu, Blend b = Blend::Alpha) {
        if (A) ui.image(*A, r, s, col, b);
    };
    // MinimapBorderTop + MinimapZoneText (Minimap_Update: the PvP colour).
    spr(kBarRect, kSrcBar);
    (void)stop(kIdMmZone, kZoneTextBox);
    if (!m.zoneText.empty()) ui.textMarquee(small, kZoneTextBox, m.zoneText.c_str(), pvpColor(c.pvpType));
    // MinimapBorder (minimapborder=0: none - the backdrop's dark edge stays).
    if (m.borderArt) {
        if (A) {
            spr(Rect{kRingRect.x + m.borderDx, kRingRect.y + m.borderDy, kRingRect.w, kRingRect.h}, kSrcRing);
        } else if (m.border.pixels) {
            // Old ui.pak: "mm_border" = UI-Minimap-Border texcoords (0.25, 0.125)-(1, 0.875): the 192 px ring
            // around the client's 140 px Minimap, scaled with the disc, centred on MinimapBackdrop.
            const float s = 2.0f * kRadius / 140.0f * 192.0f;
            ui.image(m.border, Rect{kBackdropX - s * 0.5f + m.borderDx, kBackdropY - s * 0.5f + m.borderDy, s, s});
        }
    }
    // MinimapNorthTag (rotateMinimap = 0); rotating: MinimapCompassTexture (drawn raw before) or, without
    // the art, the old letter MM_NORTH where north is.
    if (!m.rotate) {
        spr(kNorthRect, kSrcNorth);
    } else if (!c.compass.pixels) {
        fmt::MinimapProj v;
        v.viewRadius = m.viewR;
        v.mapRadius = kRadius;
        pp::vmath::sincos(camYaw_, v.sinBearing, v.cosBearing);
        float nx = 0, ny = 0;
        fmt::minimapDeltaToOffset(1.0f, 0.0f, v, nx, ny);
        const float len = std::sqrt(nx * nx + ny * ny);
        if (len > 1e-6f)
            ui.textShadow(small, kCenterX + nx / len * (kRadius - 5.0f), kCenterY + ny / len * (kRadius - 5.0f) - 6.0f, OWN_STR(MM_NORTH),
                          kColWhite, Align::Center);
    }
    // MiniMapLFGFrame (the eye - shown in a Dungeon Finder mode; its own art from wotlk.pak,
    // so also with an old ui.pak); the GameTimeFrame button now opens the CalendarFrame (ui_world_calendar.cpp).
    drawMinimapLfg(ui, focusable);
    if (!A) {  // old ui.pak: the cluster's buttons are the zoom pair as kit buttons (cursor mode only)
        if (focusable) {
            kit_.button(ui, winFocus_, kIdZoomIn, Rect{pp::kScreenW - 20.0f, kCenterY + kRadius - 26.0f, 18, 14}, "+",
                        minimapCanZoom(m, true));
            kit_.button(ui, winFocus_, kIdZoomOut, Rect{pp::kScreenW - 20.0f, kCenterY + kRadius - 10.0f, 18, 14}, "-",
                        minimapCanZoom(m, false));
        }
        return;
    }
    // MiniMapTracking: Background, Icon (GetTrackingTexture), IconOverlay while pushed, Border, Shine, Highlight.
    {
        const bool f = stop(kIdMmTracking, kTrackRect);
        const bool down = (f && winFocus_.isPressed(kIdMmTracking)) || windowOpen(kWinMinimapTracking);
        spr(kTrackBg, kSrcTrkBg, 0x99FFFFFFu);  // Color a=0.6
        const Rect& ir = down ? kTrackIconDown : kTrackIcon;
        const pp::Texture* icon = c.trackActive ? spellDb_.icon(c.trackActive, spellFrame_) : nullptr;
        if (icon) ui.image(*icon, ir);
        else spr(ir, kSrcTrkNone);
        if (down) ui.fill(ir, 0x80000000u);
        spr(kTrackBorder, kSrcTrkBorder);
        if (c.shineMs) {  // UIFrameFade in 0.5 s, then UIFrameFadeOut 0.5 s
            const uint32_t t = now - c.shineMs;
            if (t >= 1000) {
                c.shineMs = 0;
            } else {
                const float a = t < 500 ? static_cast<float>(t) / 500.0f : static_cast<float>(1000 - t) / 500.0f;
                spr(kTrackShine, kSrcShine, (static_cast<uint32_t>(a * 255.0f) << 24) | 0x00FFFFFFu, Blend::Add);
            }
        }
        if (f) spr(kTrackRect, kSrcZoomHl, 0xFFFFFFFFu, Blend::Add);
    }
    // MiniMapWorldMapButton (MiniMapButtonTemplate: the icon 1 px down-right while pushed).
    {
        const bool f = stop(kIdMmWorldMap, kWorldRect);
        const bool down = f && winFocus_.isPressed(kIdMmWorldMap);
        spr(down ? Rect{kWorldIcon.x + 1, kWorldIcon.y + 1, kWorldIcon.w, kWorldIcon.h} : kWorldIcon, kSrcWorldIcon);
        spr(kWorldRect, kSrcTrkBorder);
        if (f) spr(kWorldRect, kSrcZoomHl, 0xFFFFFFFFu, Blend::Add);
    }
    // MinimapZoomIn / MinimapZoomOut (Minimap_ZoomInClick disables the button at the last level).
    for (int k = 0; k < 2; ++k) {
        const bool in = k == 0;
        const uint32_t id = in ? kIdZoomIn : kIdZoomOut;
        const bool enabled = minimapCanZoom(m, in);  // + the indoor zoom
        const bool f = stop(id, in ? kZoomInHit : kZoomOutHit);
        const SrcRect* art = in ? kSrcZoomIn : kSrcZoomOut;
        const Rect& r = in ? kZoomInRect : kZoomOutRect;
        spr(r, art[!enabled ? 2 : (f && winFocus_.isPressed(id)) ? 1 : 0]);
        if (f && enabled) spr(r, kSrcZoomHl, 0xFFFFFFFFu, Blend::Add);
    }
    // MiniMapMailFrame: HasNewMail.
    if (session_.hasNewMail()) {
        (void)stop(kIdMmMail, kMailRect);
        spr(kMailIcon, kSrcMailIcon);
        spr(kMailRect, kSrcTrkBorder);
    }
    drawBattlefieldMinimapButton(ui, A, focusable);  // MiniMapBattlefieldFrame (ui_world_battleground.cpp)
    // GameTimeFrame: Normal / Pushed calendar, the day (GameFontBlack), the invites and their glow (flashing).
    {
        const bool f = stop(kIdMmCalendar, kCalHit);
        spr(kCalRect, (f && winFocus_.isPressed(kIdMmCalendar)) ? kSrcCalDown : kSrcCalUp);
        if (c.dayText[0]) {
            const Font& tiny = kit_.font(kFontTiny);
            ui.text(tiny, kCalTextX, kCalTextY - static_cast<float>(tiny.lineHeight()) * 0.5f, c.dayText, 0xFF000000u,
                    Align::Center);
        }
        if (c.flashInvite) {
            // GameTimeFrame_OnUpdate: max(0, 0.5 + 0.5 cos(2 pi t INVITE_PULSE_SEC)), INVITE_PULSE_SEC = 0.5, the
            // timer back to 0 at 2 pi: bright - dark - bright every 2 s
            const float t = static_cast<float>((now - c.flashMs) % 2000u) / 1000.0f;
            const float a = std::max(0.0f, 0.5f + 0.5f * std::cos(3.14159265359f * t));
            const uint32_t col = (static_cast<uint32_t>(a * 255.0f) << 24) | 0x00FFFFFFu;
            spr(kCalInvite, kSrcCalInvite, col);
            spr(kCalGlow, kSrcCalGlow, col, Blend::Add);
        }
        if (f) spr(kCalRect, kSrcZoomHl, 0xFFFFFFFFu, Blend::Add);
    }
    // TimeManagerClockButton: ClockBackground + TimeManagerClockTicker (GameFontHighlightSmall).
    {
        (void)stop(kIdMmClock, kClockRect);
        spr(kClockRect, kSrcClockBg);
        if (c.clockText[0])
            ui.text(small, kClockRect.x + kClockRect.w * 0.5f,
                    kClockRect.y + (kClockRect.h - static_cast<float>(small.lineHeight())) * 0.5f, c.clockText, kColWhite,
                    Align::Center);
    }
}

// the cluster's buttons. Zoom: the original's sound, then mapWindowAction
// (ui_world_map.cpp) zooms as upstream Minimap::zoomIn / zoomOut and saves minimapzoom.
bool WorldScreen::minimapAction(uint32_t act) {
    if (act != kIdZoomIn && act != kIdZoomOut && (act < kIdMmZone || act >= kIdMmTrackItemEnd)) return false;
    if (!map_) initMap();
    MapUi& m = *map_;
    auto& c = m.cluster;
    switch (act) {
        case kIdZoomIn:  // Minimap_ZoomInClick (disabled at the last level)
            if (!minimapCanZoom(m, true)) return true;
            playSoundByName("igMiniMapZoomIn");
            if (m.dg.indoor) return minimapIndoorZoom(m, true);  // minimapInsideZoom
            return false;
        case kIdZoomOut:  // Minimap_ZoomOutClick
            if (!minimapCanZoom(m, false)) return true;
            playSoundByName("igMiniMapZoomOut");
            if (m.dg.indoor) return minimapIndoorZoom(m, false);
            return false;
        case kIdMmTracking: {  // ToggleDropDownMenu(1, nil, MiniMapTrackingDropDown, "MiniMapTracking", 0, -5)
            playSoundByName("igMainMenuOptionCheckBoxOn");
            if (windowOpen(kWinMinimapTracking)) {
                closeWindow(kWinMinimapTracking);
                return true;
            }
            // MiniMapTrackingDropDown_Initialize: GetTrackingInfo 1..n, then NONE (checked when none runs)
            c.menuItems = c.trackSpells;
            if (c.menuItems.size() > kIdMmTrackItemEnd - kIdMmTrackItem0 - 1)
                c.menuItems.resize(kIdMmTrackItemEnd - kIdMmTrackItem0 - 1);
            c.menuItems.push_back(0);
            size_t checked = c.menuItems.size() - 1;
            for (size_t i = 0; i + 1 < c.menuItems.size(); ++i)
                if (c.menuItems[i] == c.trackActive) checked = i;
            c.menuAwayMs = 0;
            if (!c.menuCheck.pixels) {  // UI-CheckBox-Check (the world map's "wm_dd_check"), while it is open
                char path[256];
                pp::fsPath(path, sizeof(path), "data/ui.pak");
                pp::Pak pak;
                uint32_t bytes = 0;
                if (pak.open(path)) {
                    (void)loadUiArt(pak, "wm_dd_check", c.menuCheck, bytes);
                    pak.close();
                }
            }
            openWindow(kWinMinimapTracking);
            winFocus_.setFocus(kIdMmTrackItem0 + static_cast<uint32_t>(checked));
            PSP_LOGI(pp::tag::Ctrl, "minimap_track_menu items=%u active=%u", static_cast<unsigned>(c.menuItems.size()),
                     static_cast<unsigned>(c.trackActive));
            return true;
        }
        case kIdMmWorldMap:  // ToggleFrame(WorldMapFrame)
            if (windowOpen(kWinWorldMap)) closeWindow(kWinWorldMap);
            else openWorldMap();
            return true;
        case kIdMmCalendar:  // GameTimeFrame_OnClick: the invites off (PSP: no CalendarFrame to show)
            if (c.flashInvite) {
                c.flashInvite = false;
                c.pendingInvites = 0;
            }
            return true;
        default: break;
    }
    if (act >= kIdMmTrackItem0 && act < kIdMmTrackItemEnd) {  // UIDropDownMenuButton_OnClick -> SetTracking
        const size_t i = act - kIdMmTrackItem0;
        closeWindow(kWinMinimapTracking);
        if (i >= c.menuItems.size()) return true;
        playSoundByName("UChatScrollButton");
        const uint32_t spell = c.menuItems[i];
        if (spell) session_.castSpell(spell, 0, wantMove_ || moving_);  // upstream lua_SetTracking: castSpell
        else if (c.trackActive) session_.cancelAura(c.trackActive);   // PSP: NONE cancels (see the file comment)
        PSP_LOGI(pp::tag::Ctrl, "minimap_track_set spell=%u cancel=%u", static_cast<unsigned>(spell),
                 static_cast<unsigned>(spell ? 0u : c.trackActive));
    }
    return true;  // the zone text, the mail, the clock: tooltip only
}

// MiniMapTrackingDropDown - UIDropDownMenu "MENU" (the tooltip backdrop), one
// row per item: the check (UI-CheckBox-Check), the name (GameFontHighlightSmall), the icon at the right
// (tCoords 0.0625-0.9: "spell"), UI-QuestTitleHighlight under the focus. Closed by a pick, START, the
// tracking button, or 2 s after the focus left it (UIDROPDOWNMENU_SHOW_TIME).
bool WorldScreen::drawMinimapWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinMinimapTracking) return false;
    if (!map_ || !uiMode_ || settings_.isOpen() || map_->cluster.menuItems.empty()) {
        closeWindow(kWinMinimapTracking);
        return true;
    }
    auto& c = map_->cluster;
    c.menuFrame = map_->frame;  // the check is freed by refreshMinimapCluster once the GE is done with it
    loadNpcArt(false);  // UI-QuestTitleHighlight
    const Font& f = kit_.font(kFontSmall);
    const float lh = static_cast<float>(f.lineHeight());
    constexpr float kRowH = 12.0f, kCheckW = 12.0f, kIconW = 10.0f, kPad = 8.0f;
    const size_t n = c.menuItems.size();
    float textW = 0.0f;
    for (uint32_t item : c.menuItems)
        textW = std::max(textW, static_cast<float>(f.measure(item ? spellDb_.name(item).c_str() : CLIENT_STR(NONE))));
    Rect r{kTrackRect.x, kTrackRect.y + kTrackRect.h + kDropGap, kPad * 2 + kCheckW + textW + 6.0f + kIconW,
           kPad * 2 + kRowH * static_cast<float>(n)};
    r.x = std::min(r.x, pp::kScreenW - 2.0f - r.w);  // clampedToScreen
    r.y = std::min(r.y, pp::kScreenH - 2.0f - r.h);
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    bool onMenu = winFocus_.focused() == kIdMmTracking;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t item = c.menuItems[i];
        const Rect ir{r.x + kPad, r.y + kPad + kRowH * static_cast<float>(i), r.w - kPad * 2, kRowH};
        const bool focus = winFocus_.addStop(kIdMmTrackItem0 + static_cast<uint32_t>(i), ir);
        onMenu = onMenu || focus;
        if (focus && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, ir, 0xFFFFFFFFu, Blend::Add);
        const bool checked = item ? item == c.trackActive : c.trackActive == 0;
        if (checked && c.menuCheck.pixels) ui.image(c.menuCheck, Rect{ir.x, ir.y, kCheckW, kCheckW});
        ui.text(f, ir.x + kCheckW, ir.y + (kRowH - lh) * 0.5f, item ? spellDb_.name(item).c_str() : CLIENT_STR(NONE), kColWhite);
        if (item) {
            if (const pp::Texture* ic = spellDb_.icon(item, spellFrame_)) {
                const float s = static_cast<float>(ic->uw);
                ui.image(*ic, Rect{ir.x + ir.w - kIconW, ir.y + 1.0f, kIconW, kIconW},
                         SrcRect{s * 0.0625f, s * 0.0625f, s * 0.8375f, s * 0.8375f});
            }
        }
    }
    const uint32_t now = pp::logUptimeMs();
    if (onMenu) {
        c.menuAwayMs = 0;
    } else if (c.menuAwayMs == 0) {
        c.menuAwayMs = now ? now : 1;
    } else if (now - c.menuAwayMs > 2000) {  // UIDROPDOWNMENU_SHOW_TIME
        closeWindow(kWinMinimapTracking);
    }
    return true;
}

// the GameTooltip of the focused piece, after all the windows.
void WorldScreen::drawMinimapTooltip(UiRenderer& ui) {
    if (!map_) return;
    MapUi& m = *map_;
    auto& c = m.cluster;
    const uint32_t id = c.tipId;
    c.tipId = 0;
    if (!id || !m.on || !uiMode_ || settings_.isOpen() || winFocus_.focused() != id) return;
    std::vector<TipLine> lines;
    TipAnchor anchor = TipAnchor::Left;
    switch (id) {
        case kIdMmZone: {  // Minimap_SetTooltip (ANCHOR_LEFT)
            lines.push_back({c.zoneName, {}, kColWhite});
            const uint32_t col = pvpColor(c.pvpType);
            lines.push_back({c.subzoneName, {}, col});
            std::string terr;
            switch (c.pvpType) {
                case kPvpSanctuaryType: terr = CLIENT_STR(SANCTUARY_TERRITORY); break;
                case kPvpArenaType: terr = CLIENT_STR(FREE_FOR_ALL_TERRITORY); break;
                case kPvpFriendly:
                case kPvpHostile: {
                    // FACTION_CONTROLLED_TERRITORY of the controlling faction (FACTION_ALLIANCE / FACTION_HORDE;
                    // ruRU "|3-7(%s)": DeclinedWord case 7 through questText's declension)
                    const bool alliance = (c.pvpType == kPvpFriendly) == (c.team == kPvpAlliance);
                    terr = questText(CLIENT_FMT(FACTION_CONTROLLED_TERRITORY,
                                                alliance ? CLIENT_STR(FACTION_ALLIANCE) : CLIENT_STR(FACTION_HORDE)));
                    break;
                }
                case kPvpContested: terr = CLIENT_STR(CONTESTED_TERRITORY); break;
                default: break;
            }
            if (!terr.empty()) lines.push_back({terr, {}, col});
            break;
        }
        case kIdMmTracking: {  // GameTooltip:SetTracking (ANCHOR_RIGHT)
            anchor = TipAnchor::Right;
            lines.push_back({c.trackActive ? spellDb_.name(c.trackActive) : std::string(CLIENT_STR(MINIMAP_TRACKING_TOOLTIP_NONE)),
                             {}, kColWhite});
            break;
        }
        case kIdMmWorldMap:  // GameTooltip_AddNewbieTip(WORLDMAP_BUTTON) - default anchor
            anchor = TipAnchor::Default;
            lines.push_back({CLIENT_STR(WORLD_MAP), {}, kColWhite});
            break;
        case kIdZoomIn:
        case kIdZoomOut:  // UberTooltips: GameTooltip_SetDefaultAnchor + ZOOM_IN / ZOOM_OUT
            anchor = TipAnchor::Default;
            lines.push_back({id == kIdZoomIn ? CLIENT_STR(ZOOM_IN) : CLIENT_STR(ZOOM_OUT), {}, kColWhite});
            break;
        case kIdMmMail: {  // MinimapMailFrameUpdate (ANCHOR_BOTTOMLEFT)
            anchor = TipAnchor::BottomLeft;
            const std::vector<std::string> senders = session_.latestMailSenders();
            lines.push_back({senders.empty() ? CLIENT_STR(HAVE_MAIL) : CLIENT_STR(HAVE_MAIL_FROM), {}, kColWhite});
            for (const std::string& s : senders) lines.push_back({s, {}, kColWhite});
            break;
        }
        case kIdMmCalendar:  // GameTimeFrame_OnUpdate's tooltip (ANCHOR_BOTTOMLEFT): the invites only on the PSP
            anchor = TipAnchor::BottomLeft;
            if (c.flashInvite) {
                const Font& f = kit_.font(kFontNormal);
                for (const std::string& l : wrapQuestText(
                         f, CLIENT_STR(GAMETIME_TOOLTIP_CALENDAR_INVITES), 220.0f))
                    lines.push_back({l, {}, kColWhite});
            }
            // GAMETIME_TOOLTIP_TOGGLE_CALENDAR (with the invites: after a blank line, while the
            // CalendarFrame is hidden).
            if (!c.flashInvite || !windowOpen(kWinCalendar)) {
                if (c.flashInvite) lines.push_back({" ", {}, kColWhite});
                for (const std::string& l : wrapQuestText(kit_.font(kFontNormal), CLIENT_STR(GAMETIME_TOOLTIP_TOGGLE_CALENDAR), 220.0f))
                    lines.push_back({l, {}, kColWhite});
            }
            break;
        case kIdMmClock: {  // TimeManagerClockButton_UpdateTooltip -> GameTime_UpdateTooltip (ANCHOR_LEFT)
            static const bool military = pp::configGetInt("clock24", 1) != 0;
            char realm[16], local[16];
            formatTime(realm, sizeof(realm), c.clockMinutes < 0 ? 0u : static_cast<unsigned>(c.clockMinutes), military, true);
            const std::tm lt = core::localTime(std::time(nullptr));
            formatTime(local, sizeof(local), static_cast<unsigned>(lt.tm_hour * 60 + lt.tm_min), military, true);
            lines.push_back({CLIENT_STR(TIMEMANAGER_TOOLTIP_TITLE), {}, kColWhite});
            lines.push_back({CLIENT_STR(TIMEMANAGER_TOOLTIP_REALMTIME), realm, kColGold, kColWhite});
            lines.push_back({CLIENT_STR(TIMEMANAGER_TOOLTIP_LOCALTIME), local, kColGold, kColWhite});
            break;
        }
        default: break;
    }
    drawClusterTip(ui, kit_, lines, c.tipRect, anchor);
}

}  // namespace wowee::ui::psp
