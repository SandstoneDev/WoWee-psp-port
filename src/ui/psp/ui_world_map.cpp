// Map - the world map window (SELECT -> Menu -> WORLD_MAP), window stack id 0x98.
//
// Upstream rendering/world_map/*: WorldMapFacade (open on the player's continent and
// zone, zoom in on the hovered zone or the player's, zoom out, the player arrow, quest
// givers) over DataRepository's zones - here rebuilt from the pak's "wmhdr"
// (psp_worldmap_view.hpp) - with upstream's coordinate_projection, exploration_state and
// view_state_machine linked unchanged. CompositeRenderer's picture (12 base tiles of the
// zone, the explored WorldMapOverlay pieces over them, a fog tint when nothing is
// explored) is drawn directly as sprites: no 1024 x 768 render target.
//
// the window is
// WorldMapFrame.xml / .lua of the ruRU 3.3.5a client in its full-size view
// (WorldMapFrame_SetFullMapView: WORLDMAP_FULLMAP_SIZE, no quest list):
//   - BlackoutWorld, the frame WorldMapFrameTexture1..18 (UI-WorldMap-Top/Middle/Bottom1..4 and
//     the full-map patches; ui.pak wm_top / wm_bottom / wm_sides), WorldMapFrameTitle WORLD_MAP
//     (WORLD_MAP), WorldMapFrameCloseButton;
//   - WorldMapContinentDropDown (CONTINENT) and WorldMapZoneDropDown (ZONE):
//     UIDropDownMenuTemplate boxes, their lists (UIDropDownListTemplate) with GetMapContinents /
//     GetMapZones (upstream WorldMapFacade continentNames / zonesOnContinent: map 1, 0, 530, 571;
//     the zones alphabetically), the shown one checked; WorldMapZoomOutButton (ZOOM_OUT);
//   - WorldMapDetailFrame 1002 x 668 (the map), WorldMapHighlight (the hovered zone's
//     <Zone>Highlight added at its WorldMapArea rect, continent view), WorldMapFrameAreaLabel
//     (WorldMapTextFont: the zone under the cursor on a continent, the area on a zone map);
//   - WorldMapPlayer (the arrow, its tooltip = the player's name), WorldMapParty1..4
//     (WorldMapPartyIcon + name tooltips; the PSP session has no group yet - collectPartyDots),
//     WorldMapCorpse / WorldMapDeathRelease (POIIcons tombstone, CORPSE_RED / SPIRIT_HEALER_RELEASE_RED).
// PSP:
//  - 4:3 in the middle of the screen (the client's full-screen frame on a wide monitor), map
//    1002 x 668 units -> 345 x 230 px; the top band (69 units) gets 32 px and the dropdown row is
//    re-spaced so the 10 px font fits (kContBox / kZoneBox / kZoomBtn);
//  - the World and Cosmic levels are there (data/worldmap.pak, psp_minimap_conv
//    --world; setCosmicEnabled(true) as upstream's 3.3.5a DataRepository): ZOOM_OUT / Circle goes zone ->
//    continent -> World -> Cosmic (WorldMapZoomOutButton_OnClick; map 530's continent -> Cosmic), the World art
//    (Interface\WorldMap\World), the hovered continent's <Folder>Highlight where WorldMapContinent.dbc puts it
//    and its name, the player's arrow (GetPlayerMapPosition on the world map = the same transform); Cosmic:
//    the AzerothButton / OutlandButton of WorldMapFrame.xml with their highlights. Cross on a continent /
//    button or a continent of the list = SetMapZoom(continent): any continent whose data/minimap_<map>.pak
//    is on the card (another map's pak is opened for the window - upstream switchToMap), the others grey;
//    without data/worldmap.pak the map stops at the continent as before;
//  - no WorldMapZoneMinimapDropDown (no battlefield minimap), no size toggle, no quest objectives
//    (WorldMapQuestShowObjectives / quest POIs), no ping, no dungeon level dropdown;
//  - the area label of a zone map: upstream SubzoneTooltipLayer (smallest overlay under the cursor
//    by its hit rect); an unexplored one is grey (upstream's "(Unexplored)" has no ruRU string);
//  - the quest giver marks (upstream setQuestPois from getNpcQuestStatuses) stay;
//  - the art is read when the view changes (one read of the zone's consecutive entries,
//    `worldmap_open` / `worldmap_view` log its size and time); the frame art when the window opens
//    (`worldmap_art`), the continent's highlights on the continent view (`worldmap_hl`); all of it is
//    freed when the window closes (WorldScreen::drawMinimap sees the window gone: `worldmap_close`);
//  - the virtual continent switch (map 530's islands shown on map 1's continent) is not done: a pak
//    holds one map;
//  - the player's arrow, the quest givers, the corpse and the group are drawn only on the
//    player's own map (upstream's PlayerMarkerLayer tests the zones by position - another continent's zones
//    would place it there too), and exploration of another continent ignores the player's position.
//  - ... and on the continent of their virtual zone (upstream's
//    virtualMapOverride: map 530's northern zones on map 0's continent, its islands on map 1's - opened
//    with the window, everything of the player's map shifted by the zone's WorldMapTransforms offset there).
// Controls (docs/PSP_CONTROLS.md): stick = cursor, Cross = the left button (a zone on the
// continent map = zoom in, the dropdowns, their items, ZOOM_OUT, the close button), Circle on the
// map = the right button (WorldMapButton_OnClick: zoom out), START = Esc (the dropdown list, then the
// window).
#include "ui/psp/ui_world_map.hpp"

#include "core/coordinates.hpp"  // canonicalToRender (the party dots)
#include "game/quest_giver_status.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_locale.hpp"  // localeFontPak
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "rendering/world_map/coordinate_projection.hpp"

#include <pspctrl.h>
#include <pspiofilemgr.h>  // sceIoGetstat (which continents' paks are on the card)
#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace fmt = wowee::rendering::psp;
namespace wmap = wowee::rendering::world_map;
using namespace minimap_layout;

namespace {

constexpr float kMapW = 1002.0f, kMapH = 668.0f;  // upstream: 1002 x 668 of the 1024 x 768 art is map

// the layout (tools/psp/build_ui_pak.py WM_* bakes the frame for these numbers).
constexpr float kWmS = 345.0f / 1002.0f;                  // map units -> px (WM_S)
constexpr float kWmFrameX = 64.0f, kWmFrameW = 352.0f;    // the 1024-unit frame (WM_FRAME_W), centred
constexpr float kWmTopH = 32.0f, kWmBottomH = 10.0f;      // WM_TOP_H (69 units), WM_BOTTOM_H (31 units)
constexpr float kWmSideL = 3.0f, kWmSideR = 4.0f;         // WM_SIDE_L / WM_SIDE_R (10 / 12 units)
constexpr float kWmTy = kWmTopH / 69.0f;                  // top band units -> px (PSP: taller than kWmS)
// WorldMapDetailFrame: TOPLEFT at the guide's TOP -502,-69 (full-map view), 1002 x 668.
// The map's rect this frame - the full view or the mini mode (drawMapWindow sets it).
Rect g_mapRect{kWmFrameX + kWmSideL, kWmTopH, 345.0f, 230.0f};
const Rect kMapRectFull{kWmFrameX + kWmSideL, kWmTopH, 345.0f, 230.0f};
// PSP: the dropdown row re-spaced for the 10 px font (the original boxes are 159 units = 55 px wide):
// WorldMapContinentDropDown ends where the original's does (the guide's centre - 50), the zone one and
// WorldMapZoomOutButton (110 x 22) follow; WorldMapFrameCloseButton in the title bar's right socket.
const Rect kContBox{92.0f, 14.0f, 126.0f, 18.0f};
const Rect kZoneBox{220.0f, 14.0f, 126.0f, 18.0f};
const Rect kZoomBtn{349.0f, 17.0f, 58.0f, 14.0f};
const Rect kCloseBtn{403.0f, 0.0f, 13.0f, 13.0f};
// WorldMapFrameSizeDownButton (TOPRIGHT of the guide -16,4: left of the close button).
const Rect kSizeBtn{389.0f, 0.0f, 13.0f, 13.0f};
// The world map as a window: the mini mode of the original (WorldMap_ToggleSizeDown,
// WorldMapFrame_SetMiniMode): the UI-WorldMapSmall border (640 x 512 units, drawn 593 x 440) at TOPLEFT 10,-14
// of the 623 x 437 frame, the map at WORLDMAP_WINDOWED_SIZE 0.573 at TOPLEFT 37,-66 (its own units), no
// BlackoutWorld, no dropdowns or ZOOM_OUT (the right click zooms out); the close button at MiniBorderRight's
// TOPRIGHT -44,5, WorldMapFrameSizeUpButton at -66,5 (32 units). PSP: 0.42 px a unit (the original's 0.573 of
// our full map, 198 x 132 px, is hard to read on the PSP); a window like the others (R + stick moves it).
constexpr float kMiniS = 0.42f;  // build_ui_pak.py WM_MINI_S
constexpr float kMiniW = 593.0f * kMiniS, kMiniH = 440.0f * kMiniS;
const float kMiniX = std::floor((480.0f - kMiniW) * 0.5f), kMiniY = std::floor((272.0f - kMiniH) * 0.5f);
const Rect kMiniMapRect{kMiniX + (37.0f * 0.573f - 10.0f) * kMiniS, kMiniY + (66.0f * 0.573f - 14.0f) * kMiniS,
                        1002.0f * 0.573f * kMiniS, 668.0f * 0.573f * kMiniS};
const Rect kMiniClose{kMiniX + (640.0f - 44.0f - 32.0f) * kMiniS, kMiniY - 5.0f * kMiniS, 32.0f * kMiniS, 32.0f * kMiniS};
const Rect kMiniSize{kMiniX + (640.0f - 66.0f - 32.0f) * kMiniS, kMiniY - 5.0f * kMiniS, 32.0f * kMiniS, 32.0f * kMiniS};
constexpr uint32_t kColWorldMapText = 0xFFC2EDFFu;  // WorldMapTextFont (1, 0.9294, 0.7607)
constexpr uint32_t kColRed2020 = 0xFF2020FFu;       // |cffff2020 (CORPSE_RED, SPIRIT_HEALER_RELEASE_RED)
constexpr uint32_t kColDisableSmall = 0xFF808080u;  // GameFontDisableSmall (0.5, 0.5, 0.5)
constexpr uint32_t kDropDownShowMs = 2000;          // UIDROPDOWNMENU_SHOW_TIME
constexpr int kDdMaxItems = static_cast<int>(kIdMapDdItemEnd - kIdMapDdItem0);

constexpr uint32_t abgr(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (a << 24) | (b << 16) | (g << 8) | r; }

using World = WorldScreen::MapUi::World;

// upstream WorldMapFacade::zoneAtMapPoint (continent map point -> zone via the ZMP).
int zoneAtMapPoint(const World& w, float u, float v) {
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f || !w.view.zmp) return -1;
    const wmap::ViewLevel level = w.state.currentLevel();
    if (level != wmap::ViewLevel::CONTINENT && level != wmap::ViewLevel::ZONE) return -1;
    const int cont = w.state.continentIdx();
    if (cont < 0) return -1;
    float left = 0, right = 0, top = 0, bottom = 0;
    if (!wmap::getContinentProjectionBounds(w.zones, cont, left, right, top, bottom)) return -1;
    const float du = left - right, dv = top - bottom;
    if (std::abs(du) < 0.001f || std::abs(dv) < 0.001f) return -1;
    const float wowX = left - u * du, wowY = top - v * dv;  // upstream names: wowX = server Y
    const uint32_t area = w.view.zmpArea(wowX, wowY);
    if (area == 0) return -1;
    const int idx = wmap::findZoneByAreaId(w.zones, area);  // upstream zoneIndexForAreaId
    if (idx < 0 || !wmap::zoneBelongsToContinent(w.zones, idx, cont)) return -1;
    return idx;
}

// One point inside each zone of the continent - its ZMP cell nearest the zone's centroid, as map u/v
// (zoneAtMapPoint backwards). The D-pad's stops on the continent view (the original has the mouse only).
void buildZoneSpots(World& w, int cont) {
    w.spots.clear();
    w.spotsContinent = cont;
    float left = 0, right = 0, top = 0, bottom = 0;
    if (!w.view.zmp || cont < 0 || !wmap::getContinentProjectionBounds(w.zones, cont, left, right, top, bottom))
        return;
    const float du = left - right, dv = top - bottom;
    if (std::abs(du) < 0.001f || std::abs(dv) < 0.001f) return;
    constexpr float kWorldSize = 64.0f * fmt::kMinimapTileYards;
    std::vector<int16_t> cellZone(128 * 128, -1);
    std::vector<float> cellU(128 * 128), cellV(128 * 128);
    std::unordered_map<uint32_t, int> zoneOfArea;
    struct Acc {
        double su = 0.0, sv = 0.0;
        int n = 0;
    };
    std::unordered_map<int, Acc> acc;
    for (int row = 0; row < 128; ++row)
        for (int col = 0; col < 128; ++col) {
            const int i = row * 128 + col;
            const uint32_t area = w.view.zmp[i];
            if (area == 0) continue;
            const float wowX = (0.5f - (static_cast<float>(col) + 0.5f) / 128.0f) * kWorldSize;
            const float wowY = (0.5f - (static_cast<float>(row) + 0.5f) / 128.0f) * kWorldSize;
            const float u = (left - wowX) / du, v = (top - wowY) / dv;
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;
            auto it = zoneOfArea.find(area);
            if (it == zoneOfArea.end()) {
                int z = wmap::findZoneByAreaId(w.zones, area);
                if (z >= 0 && !wmap::zoneBelongsToContinent(w.zones, z, cont)) z = -1;
                it = zoneOfArea.emplace(area, z).first;
            }
            if (it->second < 0) continue;
            cellZone[i] = static_cast<int16_t>(it->second);
            cellU[i] = u;
            cellV[i] = v;
            Acc& a = acc[it->second];
            a.su += u;
            a.sv += v;
            ++a.n;
        }
    for (const auto& [zone, a] : acc) {
        const float cu = static_cast<float>(a.su / a.n), cv = static_cast<float>(a.sv / a.n);
        float best = 1e9f;
        World::ZoneSpot spot{zone, cu, cv};
        for (int i = 0; i < 128 * 128; ++i) {
            if (cellZone[i] != zone) continue;
            const float d = (cellU[i] - cu) * (cellU[i] - cu) + (cellV[i] - cv) * (cellV[i] - cv);
            if (d < best) {
                best = d;
                spot.u = cellU[i];
                spot.v = cellV[i];
            }
        }
        w.spots.push_back(spot);
        if (w.spots.size() >= kIdMapZoneEnd - kIdMapZone0) break;
    }
    // A stable order (top to bottom, then left to right) - the D-pad picks by position anyway.
    std::sort(w.spots.begin(), w.spots.end(),
              [](const World::ZoneSpot& a, const World::ZoneSpot& b) { return a.v != b.v ? a.v < b.v : a.u < b.u; });
}

// The AreaTable name (upstream areaNameByAreaId), else the texture folder (as upstream's zone list).
const char* zoneTitle(const World& w, int idx) {
    if (idx < 0 || idx >= static_cast<int>(w.zones.size())) return "";
    // the synthetic World / Cosmic zones (appended after the baked ones) have no row there.
    if (!w.view.h || idx >= static_cast<int>(w.view.h->zoneCount)) return w.zones[idx].areaName.c_str();
    const fmt::WorldMapZone& z = w.view.zones[idx];
    const char* n = w.view.str(z.nameOffset);
    return n[0] ? n : w.view.str(z.folderOffset);
}

// GetMapContinents' name of a continent zone (wm_conts: Map.dbc), else its title.
const char* continentTitle(const World& w, int contIdx) {
    if (contIdx < 0 || contIdx >= static_cast<int>(w.zones.size())) return "";
    const uint32_t mapId = w.zones[contIdx].mapID;
    for (size_t i = 0; i < w.contMaps.size() && i < w.contNames.size(); ++i)
        if (w.contMaps[i] == mapId && !w.contNames[i].empty()) return w.contNames[i].c_str();
    return zoneTitle(w, contIdx);
}

// upstream WorldMapFacade zonesOnContinent: the zones of a continent with a name, alphabetically.
std::vector<int> zonesOnContinent(const World& w, int contIdx) {
    std::vector<int> out;
    if (contIdx < 0) return out;
    for (int i = 0; i < static_cast<int>(w.zones.size()); ++i) {
        if (w.zones[i].areaID == 0 || !wmap::zoneBelongsToContinent(w.zones, i, contIdx)) continue;
        if (zoneTitle(w, i)[0]) out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [&](int a, int b) { return std::strcmp(zoneTitle(w, a), zoneTitle(w, b)) < 0; });
    return out;
}

// upstream SubzoneTooltipLayer: the smallest overlay of the zone whose hit rect (else its own box) holds
// the point - its area's name and whether it is explored. PSP: the point in the 1002 x 668 map pixels the
// overlays are drawn in (upstream multiplied by its 1024 x 768 FBO size).
bool subzoneAt(const World& w, const fmt::MinimapHeaderView& areas, int idx, float u, float v, std::string& name,
               bool& explored) {
    if (idx < 0 || idx >= static_cast<int>(w.zones.size())) return false;
    const auto& zone = w.zones[idx];
    const float px = u * kMapW, py = v * kMapH;
    float bestArea = 1e30f;
    name.clear();
    explored = false;
    for (int oi = 0; oi < static_cast<int>(zone.overlays.size()); ++oi) {
        const auto& ov = zone.overlays[oi];
        const bool hasHitRect = ov.hitRectRight > ov.hitRectLeft && ov.hitRectBottom > ov.hitRectTop;
        if (hasHitRect) {
            if (px < static_cast<float>(ov.hitRectLeft) || px > static_cast<float>(ov.hitRectRight) ||
                py < static_cast<float>(ov.hitRectTop) || py > static_cast<float>(ov.hitRectBottom))
                continue;
        } else if (px < ov.offsetX || px > ov.offsetX + ov.texWidth || py < ov.offsetY ||
                   py > ov.offsetY + ov.texHeight) {
            continue;
        }
        const float area = static_cast<float>(ov.texWidth) * static_cast<float>(ov.texHeight);
        if (area >= bestArea) continue;
        bestArea = area;
        for (uint32_t areaId : ov.areaIDs) {
            if (areaId == 0) continue;
            if (const char* n = areas.name(areaId)) {
                name = n;
                break;
            }
        }
        explored = w.exploration.exploredOverlays().count(oi) > 0;
    }
    return !name.empty();
}

// WorldMapFrameAreaLabel / WorldMapTextFont: OUTLINE THICK - the black outline around the text.
void textOutline(UiRenderer& ui, const Font& f, float x, float y, const char* s, uint32_t col) {
    static const float kOff[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    for (const auto& o : kOff) ui.text(f, x + o[0], y + o[1], s, 0xFF000000u, Align::Center);
    ui.text(f, x, y, s, col, Align::Center);
}

struct TipLine {
    std::string text;
    uint32_t color;
};

// WorldMapTooltip (GameTooltipTemplate): ANCHOR_RIGHT = its BOTTOMLEFT at the owner's TOPRIGHT,
// ANCHOR_LEFT = its BOTTOMRIGHT at the owner's TOPLEFT (the left one when the owner is right of the
// map's centre). Not part of the window's bounds.
void drawMapTooltip(UiRenderer& ui, const UiKit& kit, const Rect& owner, bool leftSide, const std::vector<TipLine>& lines) {
    if (lines.empty()) return;
    const Font& f = kit.font(kFontNormal);
    float w = 0.0f;
    for (const TipLine& l : lines) w = std::max(w, static_cast<float>(f.measure(l.text.c_str())));
    const float lh = static_cast<float>(f.lineHeight());
    Rect r{0.0f, 0.0f, w + 16.0f, lh * static_cast<float>(lines.size()) + 10.0f};
    r.x = leftSide ? owner.x - r.w : owner.x + owner.w;
    r.y = owner.y - r.h;
    // On the screen - the mini map is a moved window (the renderer adds its offset).
    const float ox = ui.offsetX(), oy = ui.offsetY();
    r.x = std::clamp(r.x, 2.0f - ox, static_cast<float>(pp::kScreenW) - 2.0f - r.w - ox);
    r.y = std::clamp(r.y, 2.0f - oy, static_cast<float>(pp::kScreenH) - 2.0f - r.h - oy);
    ui.boundsHold(true);
    if (kit.has(kTexTipBg)) ui.tile(kit.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xE0FFFFFFu);
    else ui.fill(r, 0xE0100000u);
    if (kit.has(kTexTipBorder)) ui.nineSlice(kit.tex(kTexTipBorder), r);
    float y = r.y + 5.0f;
    for (const TipLine& l : lines) {
        ui.text(f, r.x + 8.0f, y, l.text.c_str(), l.color);
        y += lh;
    }
    ui.boundsHold(false);
}

// UIDropDownMenuTemplate's Left / Middle / Right (CharacterCreate-LabelFrame, its visible box): three slices.
void drawDropDownBox(UiRenderer& ui, const pp::Texture& t, const Rect& r) {
    if (!t.pixels) {
        ui.fill(r, 0xC0101010u);
        return;
    }
    const float tw = t.uw, th = t.uh, capL = 8.0f, capR = 7.0f, s = r.h / th;
    ui.image(t, Rect{r.x, r.y, capL * s, r.h}, SrcRect{0, 0, capL, th});
    ui.image(t, Rect{r.x + capL * s, r.y, r.w - (capL + capR) * s, r.h}, SrcRect{capL, 0, tw - capL - capR, th});
    ui.image(t, Rect{r.x + r.w - capR * s, r.y, capR * s, r.h}, SrcRect{tw - capR, 0, capR, th});
}

bool loadTex(pp::Pak& pak, const char* name, pp::Texture& t, uint32_t& bytes) {
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, e->size, t, true)) {
        pp::pakFree(blob);
        return false;
    }
    bytes += e->size;
    return true;
}

// WorldMapFrame's art (ui.pak, tools/psp/build_ui_pak.py worldmap_frame), read when the window opens.
void loadFrameArt(World& w) {
    auto& f = w.frame;
    if (f.tried) return;
    f.tried = true;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(pp::tag::Fs, "worldmap_art missing ui.pak");
        return;
    }
    uint32_t b = 0;
    int frame = 0, dd = 0;
    frame += loadTex(pak, "wm_top", f.top, b) ? 1 : 0;
    frame += loadTex(pak, "wm_bottom", f.bottom, b) ? 1 : 0;
    frame += loadTex(pak, "wm_sides", f.sides, b) ? 1 : 0;
    dd += loadTex(pak, "wm_dd", f.dd, b) ? 1 : 0;
    dd += loadTex(pak, "wm_dd_btn", f.ddBtn, b) ? 1 : 0;
    dd += loadTex(pak, "wm_dd_btn_dn", f.ddBtnDown, b) ? 1 : 0;
    dd += loadTex(pak, "wm_dd_hl", f.ddHl, b) ? 1 : 0;
    dd += loadTex(pak, "wm_dd_bg", f.ddBg, b) ? 1 : 0;
    dd += loadTex(pak, "wm_dd_check", f.ddCheck, b) ? 1 : 0;
    const int marks = (loadTex(pak, "wm_corpse", f.corpse, b) ? 1 : 0) + (loadTex(pak, "wm_party", f.party, b) ? 1 : 0);
    // The mini mode's frame and the size buttons (optional).
    const int mini = (loadTex(pak, "wm_mini", f.mini, b) ? 1 : 0) + (loadTex(pak, "wm_smaller", f.smaller, b) ? 1 : 0) +
                     (loadTex(pak, "wm_smaller_dn", f.smallerDown, b) ? 1 : 0) + (loadTex(pak, "wm_bigger", f.bigger, b) ? 1 : 0) +
                     (loadTex(pak, "wm_bigger_dn", f.biggerDown, b) ? 1 : 0) + (loadTex(pak, "wm_size_hl", f.sizeHl, b) ? 1 : 0);
    int font = 0;
    if (const pp::PakEntry* e = pak.find("wm_font")) {
        if (void* blob = pak.load(e)) {
            if (f.font.load(blob, e->size)) {  // takes the blob (frees it on failure)
                b += e->size;
                font = 1;
            }
        }
    }
    // The zone names are the primary's (worldmap.pak) - a font without Cyrillic takes the one of
    // the ui.pak the UI kit's fonts came from (main_psp.cpp).
    if (font && !f.font.coversCyrillic() && pp::localeFontPak()[0]) {
        pp::Pak other;
        const pp::PakEntry* e = other.open(pp::localeFontPak()) ? other.find("wm_font") : nullptr;
        if (void* blob = e ? other.load(e) : nullptr) {
            Font next;
            if (next.load(blob, e->size) && next.coversCyrillic()) {
                f.font.release();
                f.font = next;
            } else {
                next.release();
            }
        }
        other.close();
    }
    w.contMaps.clear();
    w.contNames.clear();
    if (const pp::PakEntry* e = pak.find("wm_conts")) {  // "<map>\t<name>\n" lines (Map.dbc names)
        if (void* blob = pak.load(e)) {
            const char* s = static_cast<const char*>(blob);
            const char* end = s + e->size;
            while (s < end) {
                const char* nl = static_cast<const char*>(std::memchr(s, '\n', static_cast<size_t>(end - s)));
                if (!nl) nl = end;
                const char* tab = static_cast<const char*>(std::memchr(s, '\t', static_cast<size_t>(nl - s)));
                if (tab) {
                    w.contMaps.push_back(static_cast<uint32_t>(std::strtoul(std::string(s, tab).c_str(), nullptr, 10)));
                    w.contNames.emplace_back(tab + 1, nl);
                }
                s = nl + 1;
            }
            pp::pakFree(blob);
        }
    }
    pak.close();
    f.bytes = b;
    PSP_LOGI(pp::tag::Gfx, "worldmap_art frame=%d/3 dd=%d/6 marks=%d/2 mini=%d/6 font=%d conts=%u kb=%u ms=%u", frame, dd,
             marks, mini, font, static_cast<unsigned>(w.contMaps.size()), static_cast<unsigned>(b / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
}

// WorldMapHighlight art of the continent's zones ("wmhl_<folder>", baked in continent order: one read).
void loadHighlights(WorldScreen::MapUi& m, int cont) {
    auto& w = m.world;
    m.releaseHighlights();
    w.hlContinent = cont;
    if (cont < 0) return;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak pak;
    if (!pak.open(path)) return;
    struct Piece {
        int zone;
        const pp::PakEntry* e;
    };
    std::vector<Piece> pieces;
    uint32_t lo = ~0u, hi = 0, sum = 0;
    for (int i = 0; i < static_cast<int>(w.zones.size()); ++i) {
        if (w.zones[i].areaID == 0 || !wmap::zoneBelongsToContinent(w.zones, i, cont)) continue;
        char name[48] = "wmhl_";
        const std::string& folder = w.zones[i].areaName;
        size_t k = 5;
        for (char c : folder) {
            if (k + 1 >= sizeof(name)) break;
            name[k++] = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        name[k] = '\0';
        if (const pp::PakEntry* e = pak.find(name)) {
            pieces.push_back({i, e});
            lo = std::min(lo, e->offset);
            hi = std::max(hi, e->offset + e->size);
            sum += e->size;
        }
    }
    w.hl.assign(w.zones.size(), pp::Texture{});
    int loaded = 0;
    if (!pieces.empty() && hi - lo <= sum * 2u + 4096u) {  // consecutive (build_ui_pak.py order): one read
        void* block = memalign(64, (hi - lo + 63u) & ~63u);
        if (block && pp::pakRead(pak.fd(), lo, block, hi - lo)) {
            sceKernelDcacheWritebackRange(block, (hi - lo + 63u) & ~63u);
            w.hlBlock = block;
            w.hlBytes = hi - lo;
            for (const Piece& p : pieces)
                if (pp::textureFromBlob(static_cast<uint8_t*>(block) + (p.e->offset - lo), p.e->size, w.hl[p.zone], false))
                    ++loaded;
        } else {
            std::free(block);
        }
    } else {
        for (const Piece& p : pieces)  // not in one run (an older pak): entry by entry, owned
            if (loadTex(pak, p.e->name, w.hl[p.zone], w.hlBytes)) ++loaded;
    }
    pak.close();
    PSP_LOGI(pp::tag::Gfx, "worldmap_hl cont=%u zones=%d kb=%u ms=%u", static_cast<unsigned>(w.zones[cont].wmaID),
             loaded, static_cast<unsigned>(w.hlBytes / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
}

// WorldMapParty1..4: the group members' positions (GetPlayerMapPosition("party"..i); upstream feeds
// WorldMapFacade::setPartyDots from GameHandler::getPartyData). The layer below draws what this returns
// (server = render space, as the player's position here).
// the session's roster (SMSG_GROUP_LIST / SMSG_PARTY_MEMBER_STATS position), as
// upstream game_screen_hud.cpp: online members with stats and a position; posY -> canonical X, posX -> Y.
// PSP: no class colour (the icon is WorldMapPartyIcon), the colour field unused.
void collectPartyDots(const pp::WorldSession& session, std::vector<wmap::PartyDot>& out) {
    out.clear();
    if (!session.isInGroup()) return;
    const auto& pd = session.getPartyData();
    for (const auto& member : pd.members) {
        if (member.guid == session.playerGuid()) continue;
        if (!member.isOnline || !member.hasPartyStats) continue;
        if (member.posX == 0 && member.posY == 0) continue;
        const float wowX = static_cast<float>(member.posY);
        const float wowY = static_cast<float>(member.posX);
        out.push_back({core::coords::canonicalToRender(glm::vec3(wowX, wowY, 0.0f)), 0xFFFFFFFFu, member.name});
    }
}

// ---------------------------------------------------------------------------
// the World / Cosmic levels and the other continents
// ---------------------------------------------------------------------------

// The pak folder of a map (same list as mapDirName in ui_world_screen.cpp).
const char* contMapDir(uint32_t mapId) {
    switch (mapId) {
        case 0: return "azeroth";
        case 1: return "kalimdor";
        case 530: return "expansion01";
        case 571: return "northrend";
        default: return nullptr;
    }
}

bool fileOnCard(const char* rel) {
    char path[256];
    pp::fsPath(path, sizeof(path), rel);
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

// upstream DataRepository::loadZones: a synthetic "Cosmic" and "World" zone after the map's own (no area, no
// bounds, mapID 0 - continentZoneIndex and the projections pass them by).
void appendSyntheticZones(World& w) {
    wmap::Zone cosmic;
    cosmic.areaName = "Cosmic";
    w.cosmicIdx = static_cast<int>(w.zones.size());
    w.zones.push_back(std::move(cosmic));
    wmap::Zone world;
    world.areaName = "World";
    w.worldIdx = static_cast<int>(w.zones.size());
    w.zones.push_back(std::move(world));
}

bool contAvailable(const World& w, uint32_t mapId) {
    for (size_t i = 0; i < w.contMaps.size() && i < w.contAvail.size(); ++i)
        if (w.contMaps[i] == mapId) return w.contAvail[i] != 0;
    return false;
}

// GetMapContinents' name of a map (wm_conts: Map.dbc ruRU).
const char* mapContName(const World& w, uint32_t mapId) {
    for (size_t i = 0; i < w.contMaps.size() && i < w.contNames.size(); ++i)
        if (w.contMaps[i] == mapId) return w.contNames[i].c_str();
    return "";
}

// The World level's continent under a map point (1002 x 668 units): its box of WorldMapContinent tiles.
int worldContinentAt(const World& w, float mx, float my) {
    if (!w.wl.h) return -1;
    for (uint32_t i = 0; i < w.wl.h->continentCount; ++i) {
        const fmt::WorldLevelContinent& c = w.wl.conts[i];
        if (c.worldMapId != 1) continue;  // not on the world map of maps 0 / 1 (map 530's continent)
        float x, y, bw, bh;
        fmt::worldLevelBox(c, x, y, bw, bh);
        if (mx >= x && mx < x + bw && my >= y && my < y + bh) return static_cast<int>(i);
    }
    return -1;
}

// The Cosmic level's button under a map point (OutlandButton / AzerothButton).
int cosmicButtonAt(float mx, float my) {
    int n = 0;
    const fmt::CosmicButton* b = fmt::cosmicButtons(n);
    for (int k = 0; k < n; ++k)
        if (mx >= b[k].x && mx < b[k].x + b[k].size && my >= b[k].y && my < b[k].y + b[k].size) return k;
    return -1;
}

// The D-pad's stops on the World / Cosmic level: a continent box's centre / a button's (WorldMapButton has the
// mouse only; the continent view's zone spots do the same). spotsContinent: -2 World, -3 Cosmic.
void buildLevelSpots(World& w, bool cosmic) {
    w.spots.clear();
    w.spotsContinent = cosmic ? -3 : -2;
    if (cosmic) {
        int n = 0;
        const fmt::CosmicButton* b = fmt::cosmicButtons(n);
        for (int k = 0; k < n; ++k)
            w.spots.push_back({k, (b[k].x + b[k].size * 0.5f) / kMapW, (b[k].y + b[k].size * 0.5f) / kMapH});
        return;
    }
    for (uint32_t i = 0; w.wl.h && i < w.wl.h->continentCount; ++i) {
        const fmt::WorldLevelContinent& c = w.wl.conts[i];
        if (c.worldMapId != 1) continue;
        float x, y, bw, bh;
        fmt::worldLevelBox(c, x, y, bw, bh);
        w.spots.push_back({static_cast<int>(i), std::clamp((x + bw * 0.5f) / kMapW, 0.02f, 0.98f),
                           std::clamp((y + bh * 0.5f) / kMapH, 0.02f, 0.98f)});
    }
}

// The player's place on the World level (GetPlayerMapPosition there): their map's WorldMapContinent row.
bool worldLevelPlayerUV(const World& w, uint32_t playerMap, float serverX, float serverY, float& u, float& v) {
    const fmt::WorldLevelContinent* c = w.wl.byMap(playerMap);
    if (!c || c->worldMapId != 1) return false;
    float gx = 0, gy = 0, mx = 0, my = 0;
    fmt::minimapTileCoords(serverX, serverY, gx, gy);
    fmt::worldLevelPoint(*c, gx, gy, mx, my);
    u = mx / kMapW;
    v = my / kMapH;
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

// a position on the player's map as the map shown places it - their own map
// as is; the display map of their virtual zone (World::vMap) shifted by that zone's WorldMapTransforms offset
// (upstream DataRepository::transformRenderPosition; server X += offset X, server Y += offset Y - the axes
// psp_minimap_conv's readWorldMap shifts the zone's box by). false: another continent (no positions there).
bool shownPosition(const World& w, uint32_t playerMap, const glm::vec3& p, glm::vec3& out) {
    if (w.shownMap == playerMap) {
        out = p;
        return true;
    }
    if (w.vMap != ~0u && w.shownMap == w.vMap) {
        out = glm::vec3(p.x + w.vOffX, p.y + w.vOffY, p.z);
        return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------

void WorldScreen::MapUi::releaseWorldArt() {
    World& w = world;
    for (auto& t : w.base) pp::textureRelease(t);  // not owning: resets the bind cache only
    for (auto& t : w.overlays) pp::textureRelease(t);
    w.overlays.clear();
    w.haveOverlay.clear();
    std::fill(std::begin(w.haveBase), std::end(w.haveBase), false);
    std::free(w.artBlock);
    w.artBlock = nullptr;
    w.artBytes = 0;
    w.artZone = -1;
}

void WorldScreen::MapUi::releaseHighlights() {
    World& w = world;
    for (auto& t : w.hl) pp::textureRelease(t);  // the one-read set does not own (resets the bind cache)
    w.hl.clear();
    w.hl.shrink_to_fit();
    std::free(w.hlBlock);
    w.hlBlock = nullptr;
    w.hlBytes = 0;
    w.hlContinent = -1;
}

// the World / Cosmic level's art (one block, the textures point into it).
void WorldScreen::MapUi::releaseLevelArt() {
    World& w = world;
    for (auto& t : w.lvlTiles) pp::textureRelease(t);  // not owning: resets the bind cache only
    for (auto& t : w.lvlHl) pp::textureRelease(t);
    w.lvlHl.clear();
    w.lvlHl.shrink_to_fit();
    std::fill(std::begin(w.haveLvlTile), std::end(w.haveLvlTile), false);
    std::free(w.lvlBlock);
    w.lvlBlock = nullptr;
    w.lvlBytes = 0;
    w.lvlArt = 0;
    w.hoverCont = -1;
}

uint32_t WorldScreen::MapUi::worldBytes() const {
    const World& w = world;
    return w.frame.bytes + w.hlBytes + w.artBytes + static_cast<uint32_t>(w.hdrBlob.size()) + w.lvlBytes +
           static_cast<uint32_t>(w.wlHdr.size());
}

void WorldScreen::MapUi::releaseWorld() {
    releaseWorldArt();
    releaseHighlights();
    releaseLevelArt();
    World& w = world;
    w.alt.reset();  // another continent's pak (closes its file)
    w.shownMap = ~0u;
    w.vMap = ~0u;
    w.vOffX = w.vOffY = 0.0f;
    w.contAvail.clear();
    w.wlHdr.clear();
    w.wlHdr.shrink_to_fit();
    w.wl = fmt::WorldLevelView{};
    w.wlTried = false;
    w.levelsAvail = false;
    w.cosmicIdx = w.worldIdx = -1;
    w.spots.clear();
    w.spotsContinent = -1;
    auto& f = w.frame;
    for (pp::Texture* t : {&f.top, &f.bottom, &f.sides, &f.dd, &f.ddBtn, &f.ddBtnDown, &f.ddHl, &f.ddBg, &f.ddCheck,
                           &f.corpse, &f.party, &f.mini, &f.smaller, &f.smallerDown, &f.bigger, &f.biggerDown, &f.sizeHl})
        pp::textureRelease(*t);
    f.font.release();
    f.tried = false;
    f.bytes = 0;
    w.contMaps.clear();
    w.contNames.clear();
    w.ddOpen = 0;
    w.ddItems.clear();
    w.ddRect = Rect{};
    w.areaName.clear();
    w.zones.clear();
    w.zones.shrink_to_fit();
    w.hdrBlob.clear();
    w.hdrBlob.shrink_to_fit();
    w.view = fmt::WorldMapView{};
    w.exploreFlags.clear();
    w.exploration = wmap::ExplorationState{};
    w.state = wmap::ViewStateMachine{};
    w.open = w.ok = false;
    w.hoverZone = -1;
    w.lastExplored = ~0u;
}

// the dungeon's zone (WorldMapArea of the player's map, "dmap") is shown and
// it has levels (GetNumDungeonMapLevels > 0) - the floor of the level shown, or null on the terrain level / no level.
static bool dungeonZoneShown(const WorldScreen::MapUi& m) {
    const auto& w = m.world;
    const int idx = w.state.currentZoneIdx();
    return m.dg.dmap.h && m.dg.dmap.h->wma && w.alt == nullptr && idx >= 0 && idx < static_cast<int>(w.zones.size()) &&
           w.state.currentLevel() == wmap::ViewLevel::ZONE && w.zones[idx].wmaID == m.dg.dmap.h->wma;
}
static const fmt::DungeonFloor* shownFloor(const WorldScreen::MapUi& m) {
    if (!dungeonZoneShown(m) || m.world.dLevel <= 0) return nullptr;
    const int f = m.dg.dmap.floorOfLevel(m.world.dLevel);
    return f >= 0 ? &m.dg.dmap.floors[f] : nullptr;
}
// WorldMapLevelDropDown_Initialize's text: DUNGEON_FLOOR_<MAP><n> (baked), else FLOOR_NUMBER.
static std::string levelTitle(const WorldScreen::MapUi& m, int level) {
    const fmt::DungeonMapView& v = m.dg.dmap;
    const int f = v.floorOfLevel(level);
    const char* name = f >= 0 ? v.str(v.floors[f].nameOffset) : (v.h && v.h->terrainLevel ? v.str(v.h->terrainName) : nullptr);
    if (name) return name;
    return CLIENT_FMT(FLOOR_NUMBER, level);
}

// The zone's art in one read: its w_ / o_ entries are consecutive in the pak.
static bool loadWorldArt(WorldScreen::MapUi& m, int idx, uint32_t& kb, uint32_t& ms) {
    auto& w = m.world;
    m.releaseWorldArt();
    w.artZone = idx;
    kb = ms = 0;
    if (idx < 0 || idx >= static_cast<int>(w.zones.size())) return false;
    // the World / Cosmic zones are drawn from data/worldmap.pak (loadLevelArt); the art of
    // the continent shown comes from its own pak (another continent's: World::alt).
    if (!w.view.h || idx >= static_cast<int>(w.view.h->zoneCount)) return false;
    MinimapStore& src = m.shownStore();
    const fmt::WorldMapZone& z = w.view.zones[idx];
    struct Piece {
        const MinimapStore::Span* s;
        int base;     // 0..11, or -1
        int overlay;  // index, or -1
    };
    std::vector<Piece> pieces;
    // a dungeon level's floor - WorldMapFrame_Update's "<map><level>_<n>" tiles, no overlays.
    const fmt::DungeonFloor* floor = shownFloor(m);
    for (int n = 0; n < 12; ++n) {
        if (floor) {
            if (floor->tileMask & (1u << n))
                if (const auto* s = src.find(MinimapStore::floorKey(z.wmaId, floor->floor, n))) pieces.push_back({s, n, -1});
            continue;
        }
        if (z.tileMask & (1u << n))
            if (const auto* s = src.find(MinimapStore::baseKey(z.wmaId, n))) pieces.push_back({s, n, -1});
    }
    for (uint32_t i = 0; i < z.overlayCount && !floor; ++i)
        if (w.view.overlays[z.firstOverlay + i].baked)
            if (const auto* s = src.find(MinimapStore::overlayKey(z.wmaId, static_cast<int>(i))))
                pieces.push_back({s, -1, static_cast<int>(i)});
    if (pieces.empty()) return false;
    uint32_t lo = ~0u, hi = 0;
    for (const Piece& p : pieces) {
        lo = std::min(lo, p.s->offset);
        hi = std::max(hi, p.s->offset + p.s->size);
    }
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    void* block = memalign(64, (hi - lo + 63u) & ~63u);
    if (!block || !src.readBlocking(lo, hi - lo, block)) {
        std::free(block);
        PSP_LOGE(pp::tag::Fs, "worldmap_read_fail wma=%u kb=%u", static_cast<unsigned>(z.wmaId),
                  static_cast<unsigned>((hi - lo) / 1024u));
        return false;
    }
    sceKernelDcacheWritebackRange(block, (hi - lo + 63u) & ~63u);
    w.artBlock = block;
    w.artBytes = hi - lo;
    w.overlays.assign(z.overlayCount, pp::Texture{});
    w.haveOverlay.assign(z.overlayCount, 0);
    for (const Piece& p : pieces) {
        void* at = static_cast<uint8_t*>(block) + (p.s->offset - lo);
        pp::Texture& t = p.base >= 0 ? w.base[p.base] : w.overlays[p.overlay];
        if (!pp::textureFromBlob(at, p.s->size, t, false)) continue;
        if (p.base >= 0) w.haveBase[p.base] = true;
        else w.haveOverlay[p.overlay] = 1;
    }
    kb = w.artBytes / 1024u;
    ms = static_cast<uint32_t>((sceKernelGetSystemTimeWide() - t0) / 1000u);
    return true;
}

// upstream ExplorationState over the session's PLAYER_EXPLORED_ZONES bits.
static void updateExploration(WorldScreen::MapUi& m, const pp::WorldSession& session, const glm::vec3& pos) {
    auto& w = m.world;
    w.exploration.setServerMask(session.getPlayerExploredZoneMasks(), session.hasPlayerExploredZoneMasks());
    // PSP: another continent's zones - the player is on none of them (their position and the
    // server's zone are of the player's map; by position alone a zone there would count as entered).
    // the continent of the player's virtual zone - the player shifted onto it.
    glm::vec3 shown = pos;
    const bool here = shownPosition(w, session.worldEntry().mapId, pos, shown);
    w.exploration.update(w.zones, here ? shown : glm::vec3(1e9f, 1e9f, 0.0f), w.state.currentZoneIdx(), w.exploreFlags,
                         here ? session.worldStateZoneId() : 0u);
    const uint32_t sig = static_cast<uint32_t>(w.exploration.exploredZones().size()) << 16 |
                         static_cast<uint32_t>(w.exploration.exploredOverlays().size());
    if (sig != w.lastExplored && ++w.logs <= 60) {
        w.lastExplored = sig;
        PSP_LOGI(pp::tag::Gfx, "explored n=%u overlays=%u mask=%d",
                 static_cast<unsigned>(w.exploration.exploredZones().size()),
                 static_cast<unsigned>(w.exploration.exploredOverlays().size()), w.exploration.hasServerMask() ? 1 : 0);
    }
}

// upstream WorldMapFacade::render, first-open block: the player's continent, then their
// zone when it belongs to that continent (else the continent).
static void showPlayerView(WorldScreen::MapUi& m, const glm::vec3& pos, uint32_t serverZone) {
    auto& w = m.world;
    const int playerZone = wmap::findZoneForPlayer(w.zones, pos, serverZone);
    const int best = wmap::findBestContinentForPlayer(w.zones, pos);
    if (best >= 0) w.state.setContinentIdx(best);
    if (playerZone >= 0 && w.state.continentIdx() >= 0 &&
        wmap::zoneBelongsToContinent(w.zones, playerZone, w.state.continentIdx())) {
        w.state.setCurrentZoneIdx(playerZone);
        w.state.setLevel(wmap::ViewLevel::ZONE);
    } else if (w.state.continentIdx() >= 0) {
        w.state.setCurrentZoneIdx(w.state.continentIdx());
        w.state.setLevel(wmap::ViewLevel::CONTINENT);
    }
}

// the WorldMapContinent rows of data/worldmap.pak ("wlhdr", ~300 B), once per window.
static void readLevelHeader(pp::Pak& pak, WorldScreen::MapUi::World& w) {
    if (!w.wlHdr.empty()) return;
    if (const pp::PakEntry* e = pak.find("wlhdr")) {
        if (void* b = pak.load(e)) {
            w.wlHdr.assign(static_cast<const uint8_t*>(b), static_cast<const uint8_t*>(b) + e->size);
            pp::pakFree(b);
        }
    }
    if (!w.wl.attach(w.wlHdr.data(), static_cast<uint32_t>(w.wlHdr.size()))) {
        w.wlHdr.clear();
        w.wl = fmt::WorldLevelView{};
    }
}

// a World / Cosmic level's art from data/worldmap.pak - its 12 tiles and its
// highlights, consecutive in the pak: one read, the textures point into the block; freed off the level.
static bool loadLevelArt(WorldScreen::MapUi& m, bool cosmic, uint32_t& kb, uint32_t& ms) {
    auto& w = m.world;
    kb = ms = 0;
    const int want = cosmic ? 2 : 1;
    if (w.lvlArt == want) return true;
    m.releaseLevelArt();
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    char path[256];
    pp::fsPath(path, sizeof(path), "data/worldmap.pak");
    pp::Pak pak;
    if (!pak.open(path)) {
        if (!w.wlTried) PSP_LOGW(pp::tag::Fs, "worldmap_level_missing (no data/worldmap.pak: psp_minimap_conv --world)");
        w.wlTried = true;
        return false;
    }
    readLevelHeader(pak, w);
    w.wlTried = true;
    struct Piece {
        const pp::PakEntry* e;
        int tile;  // 0..11, or -1
        int hl;    // continent row / cosmic button, or -1
    };
    std::vector<Piece> pieces;
    char n[48];
    for (int t = 0; t < 12; ++t) {
        fmt::worldLevelTileName(n, sizeof(n), cosmic, t);
        if (const pp::PakEntry* e = pak.find(n)) pieces.push_back({e, t, -1});
    }
    int hlCount = 0;
    if (!cosmic) {
        hlCount = w.wl.h ? static_cast<int>(w.wl.h->continentCount) : 0;
        for (int i = 0; i < hlCount; ++i) {
            if (!(w.wl.conts[i].flags & 1u)) continue;
            fmt::worldLevelHighlightName(n, sizeof(n), false, w.wl.conts[i].mapId);
            if (const pp::PakEntry* e = pak.find(n)) pieces.push_back({e, -1, i});
        }
    } else {
        const fmt::CosmicButton* b = fmt::cosmicButtons(hlCount);
        for (int k = 0; k < hlCount; ++k) {
            fmt::worldLevelHighlightName(n, sizeof(n), true, b[k].mapId);
            if (const pp::PakEntry* e = pak.find(n)) pieces.push_back({e, -1, k});
        }
    }
    w.lvlHl.assign(static_cast<size_t>(hlCount), pp::Texture{});
    uint32_t lo = ~0u, hi = 0;
    for (const Piece& p : pieces) {
        lo = std::min(lo, p.e->offset);
        hi = std::max(hi, p.e->offset + p.e->size);
    }
    int tiles = 0, hls = 0;
    if (!pieces.empty()) {
        void* block = memalign(64, (hi - lo + 63u) & ~63u);
        if (block && pp::pakRead(pak.fd(), lo, block, hi - lo)) {
            sceKernelDcacheWritebackRange(block, (hi - lo + 63u) & ~63u);
            w.lvlBlock = block;
            w.lvlBytes = hi - lo;
            for (const Piece& p : pieces) {
                void* at = static_cast<uint8_t*>(block) + (p.e->offset - lo);
                if (p.tile >= 0) {
                    if (pp::textureFromBlob(at, p.e->size, w.lvlTiles[p.tile], false)) {
                        w.haveLvlTile[p.tile] = true;
                        ++tiles;
                    }
                } else if (pp::textureFromBlob(at, p.e->size, w.lvlHl[static_cast<size_t>(p.hl)], false)) {
                    ++hls;
                }
            }
        } else {
            std::free(block);
            PSP_LOGE(pp::tag::Fs, "worldmap_read_fail level=%s kb=%u", cosmic ? "cosmic" : "world",
                     static_cast<unsigned>((hi - lo) / 1024u));
        }
    }
    pak.close();
    w.lvlArt = want;
    kb = w.lvlBytes / 1024u;
    ms = static_cast<uint32_t>((sceKernelGetSystemTimeWide() - t0) / 1000u);
    PSP_LOGI(pp::tag::Gfx, "worldmap_level level=%s tiles=%d/12 hl=%d conts=%u kb=%u ms=%u", cosmic ? "cosmic" : "world", tiles,
             hls, w.wl.h ? static_cast<unsigned>(w.wl.h->continentCount) : 0u, static_cast<unsigned>(kb),
             static_cast<unsigned>(ms));
    return tiles > 0;
}

static const char* levelName(wmap::ViewLevel l) {
    switch (l) {
        case wmap::ViewLevel::ZONE: return "zone";
        case wmap::ViewLevel::CONTINENT: return "continent";
        case wmap::ViewLevel::WORLD: return "world";
        case wmap::ViewLevel::COSMIC: return "cosmic";
    }
    return "?";
}

// After the view changed (zoom, a dropdown item): exploration and the new view's art.
static void viewChanged(WorldScreen::MapUi& m, const pp::WorldSession& session, const glm::vec3& pos) {
    auto& w = m.world;
    updateExploration(m, session, pos);
    uint32_t kb = 0, ms = 0;
    // the World / Cosmic level - its own art; a continent or zone frees the level's.
    const wmap::ViewLevel level = w.state.currentLevel();
    if (level == wmap::ViewLevel::WORLD || level == wmap::ViewLevel::COSMIC) {
        m.releaseWorldArt();
        m.releaseHighlights();
        loadLevelArt(m, level == wmap::ViewLevel::COSMIC, kb, ms);
    } else {
        m.releaseLevelArt();
        loadWorldArt(m, w.state.currentZoneIdx(), kb, ms);
    }
    const int idx = w.state.currentZoneIdx();
    PSP_LOGI(pp::tag::Gfx, "worldmap_view zone=%u wma=%u level=%s map=%u kb=%u ms=%u",
             idx >= 0 ? static_cast<unsigned>(w.zones[idx].areaID) : 0u,
             idx >= 0 ? static_cast<unsigned>(w.zones[idx].wmaID) : 0u, levelName(level),
             static_cast<unsigned>(w.shownMap), static_cast<unsigned>(kb), static_cast<unsigned>(ms));
}

// upstream WorldMapFacade::switchToMap - another map's zones. The player's map is
// the HUD's store; another continent's data/minimap_<map>.pak is opened for the window (its TOC and mmhdr,
// then its wmhdr) and closed when the player's continent is shown again or the window closes. Nothing
// changes when the pak cannot be read.
static bool switchMapData(WorldScreen::MapUi& m, uint32_t mapId, uint32_t playerMap) {
    auto& w = m.world;
    if (mapId == w.shownMap && w.ok) return true;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    std::unique_ptr<MinimapStore> next;
    MinimapStore* src = nullptr;
    if (mapId == playerMap && m.storeOk) {
        src = &m.store;
    } else {
        const char* dir = contMapDir(mapId);
        if (!dir) return false;
        char rel[64], path[256];
        std::snprintf(rel, sizeof(rel), "data/minimap_%s.pak", dir);
        pp::fsPath(path, sizeof(path), rel);
        next = std::make_unique<MinimapStore>();
        if (!next->open(path, false, 64u * 1024u) || next->header().h->mapId != mapId) {
            PSP_LOGW(pp::tag::Fs, "worldmap_cont_missing map=%u pak=%s", static_cast<unsigned>(mapId), rel);
            return false;
        }
        src = next.get();
    }
    const MinimapStore::Span* s = src->find(MinimapStore::kWorldHeaderKey);
    if (!s) return false;
    std::vector<uint8_t> hdr(s->size);
    fmt::WorldMapView view;
    if (!src->readBlocking(s->offset, s->size, hdr.data()) || !view.attach(hdr.data(), static_cast<uint32_t>(hdr.size())))
        return false;
    // The old map's art, highlights and zone spots go with it (indices mean other zones now).
    m.releaseWorldArt();
    m.releaseHighlights();
    w.spots.clear();
    w.spotsContinent = -1;
    w.hdrBlob = std::move(hdr);  // the buffer moves with it: view stays valid
    w.view = view;
    w.view.buildZones(w.zones);
    appendSyntheticZones(w);
    src->header().exploreFlags(w.exploreFlags);
    w.exploration = wmap::ExplorationState{};
    w.lastExplored = ~0u;
    w.alt = std::move(next);  // null for the player's map: another continent's store closes
    w.shownMap = mapId;
    w.ok = true;
    PSP_LOGI(pp::tag::Gfx, "worldmap_cont map=%u pak=%s zones=%u hdr_kb=%u ms=%u", static_cast<unsigned>(mapId),
             w.alt ? "alt" : "own", static_cast<unsigned>(w.zones.size()), static_cast<unsigned>(w.hdrBlob.size() / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return true;
}

// SetMapZoom(continent): WorldMapContinentButton_OnClick, ProcessMapClick on the World level - upstream
// resolveWorldRegionClick (the shown map: NAVIGATE_CONTINENT, another: LOAD_MAP = switchMapData).
static bool showContinent(WorldScreen::MapUi& m, uint32_t mapId, uint32_t playerMap) {
    auto& w = m.world;
    if (!contAvailable(w, mapId)) return false;  // PSP: no pak for it on the card (grey)
    if (!switchMapData(m, mapId, playerMap)) return false;
    const int ci = wmap::continentZoneIndex(w.zones, mapId);
    if (ci < 0) return false;
    w.state.setContinentIdx(ci);
    w.state.setCurrentZoneIdx(ci);
    w.state.setLevel(wmap::ViewLevel::CONTINENT);
    return true;
}

// WorldMapZoomOutButton_OnClick (and WorldMapButton_OnClick's right button): zone -> continent; a continent ->
// the World (map 530's continent -> Cosmic: upstream switchToWorldView sends a world other than map 0 / 1's there); World -> Cosmic.
// upstream ViewStateMachine::zoomOut + switchToWorldView / enterCosmicView. PSP: without worldmap.pak the
// continent is the top (as before).
static bool zoomOutOnce(WorldScreen::MapUi& m) {
    auto& w = m.world;
    const wmap::ViewLevel level = w.state.currentLevel();
    if (level == wmap::ViewLevel::ZONE && w.state.continentIdx() < 0) return false;  // a dungeon's zone
    if (level == wmap::ViewLevel::CONTINENT && !w.levelsAvail) return false;
    if (level == wmap::ViewLevel::COSMIC) return false;
    const auto zr = w.state.zoomOut();
    if (!zr.changed) return false;
    if (zr.newLevel == wmap::ViewLevel::WORLD) {  // switchToWorldView
        const fmt::WorldLevelContinent* c = w.wl.byMap(w.shownMap);
        if (c && c->worldMapId != 1 && w.state.cosmicEnabled()) {  // map 530's continent: the Cosmic map
            (void)w.state.enterCosmicView();
            w.state.setCurrentZoneIdx(w.cosmicIdx);
        } else {
            (void)w.state.enterWorldView();
            w.state.setCurrentZoneIdx(w.worldIdx);
        }
    } else if (zr.newLevel == wmap::ViewLevel::COSMIC) {
        w.state.setCurrentZoneIdx(w.cosmicIdx);
    }
    return true;
}

// IsZoomOutAvailable: anything but the Cosmic level (and the continent without the levels' pak).
static bool zoomOutAvailable(const WorldScreen::MapUi::World& w) {
    const wmap::ViewLevel level = w.state.currentLevel();
    if (level == wmap::ViewLevel::ZONE) return w.state.continentIdx() >= 0;  // not a dungeon's zone
    if (level == wmap::ViewLevel::CONTINENT) return w.levelsAvail;
    return level == wmap::ViewLevel::WORLD && w.state.cosmicEnabled();
}

bool WorldScreen::mapMini() const { return map_ && map_->mini > 0; }

// Zones picked with the D-pad: on the continent view, with the focus on the map or a zone
// spot, a D-pad press goes to the nearest zone spot that way (along + 2 x across, as FocusRing::pick) - the focus
// ring alone prefers a wide button under the map (the action bar: Cross then cast the Hearthstone). None that
// way: the press goes on to the focus ring (out of the map: the dropdowns, the close button...).
void WorldScreen::mapZonePad(pp::PadState& p) {
    if (!map_ || !uiMode_ || !windowOpen(kWinWorldMap)) return;
    auto& w = map_->world;
    // the World / Cosmic level's continents / buttons are spots too (spotsContinent -2 / -3).
    const wmap::ViewLevel level = w.state.currentLevel();
    const int spotsKey = level == wmap::ViewLevel::CONTINENT ? w.state.continentIdx()
                         : level == wmap::ViewLevel::WORLD   ? -2
                         : level == wmap::ViewLevel::COSMIC  ? -3
                                                             : -100;
    if (!w.ok || w.ddOpen != 0 || level == wmap::ViewLevel::ZONE || w.spots.empty() || w.spotsContinent != spotsKey ||
        w.mapOnScreen.w <= 0.0f)
        return;
    const uint32_t f = winFocus_.focused();
    const bool onSpot = f >= kIdMapZone0 && f - kIdMapZone0 < w.spots.size();
    if (f != kIdMapArea && !onSpot) return;
    uint32_t dir = 0;
    float dx = 0.0f, dy = 0.0f;
    if (p.pressed & PSP_CTRL_LEFT) dir = PSP_CTRL_LEFT, dx = -1.0f;
    else if (p.pressed & PSP_CTRL_RIGHT) dir = PSP_CTRL_RIGHT, dx = 1.0f;
    else if (p.pressed & PSP_CTRL_UP) dir = PSP_CTRL_UP, dy = -1.0f;
    else if (p.pressed & PSP_CTRL_DOWN) dir = PSP_CTRL_DOWN, dy = 1.0f;
    if (!dir) return;
    const Rect& r = w.mapOnScreen;
    float fx = winFocus_.cursorX(), fy = winFocus_.cursorY();  // from the focused spot, else the cursor
    if (onSpot) {
        fx = r.x + w.spots[f - kIdMapZone0].u * r.w;
        fy = r.y + w.spots[f - kIdMapZone0].v * r.h;
    }
    int best = -1;
    float bestScore = 1e9f;
    for (size_t k = 0; k < w.spots.size(); ++k) {
        if (onSpot && k == f - kIdMapZone0) continue;
        const float sx = r.x + w.spots[k].u * r.w - fx, sy = r.y + w.spots[k].v * r.h - fy;
        const float along = sx * dx + sy * dy, across = std::fabs(sx * dy) + std::fabs(sy * dx);
        if (along < 2.0f) continue;  // not that way
        const float score = along + across * 2.0f;
        if (score < bestScore) {
            bestScore = score;
            best = static_cast<int>(k);
        }
    }
    if (best < 0) return;
    winFocus_.setFocus(kIdMapZone0 + static_cast<uint32_t>(best));  // the cursor lands on it next frame
    p.pressed &= ~dir;
}

// Own rule for GetCurrentMapDungeonLevel: outdoors on a map with a terrain level, that level; else the first floor
// whose DungeonMap.dbc rectangle (its map bounds, wowdev) holds the player; else level 1. The D-pad's level list
// shows any other floor.
static int dungeonLevelAt(const fmt::DungeonMapView& v, bool indoors, float x, float y) {
    if (!v.h) return 0;
    if (v.h->terrainLevel && !indoors) return 1;
    for (uint32_t i = 0; i < v.h->floorCount; ++i) {
        const fmt::DungeonFloor& f = v.floors[i];
        if (f.locRight <= y && y <= f.locLeft && f.locBottom <= x && x <= f.locTop)
            return (v.h->terrainLevel ? 2 : 1) + static_cast<int>(i);
    }
    return 1;
}

// SetMapToCurrentZone inside an instance: the map's own WorldMapArea (upstream data_repository: the WorldMapArea rows
// of the current map) with its level (dungeonLevelAt). Own rule without one: the continent the instance belongs to
// (Map.dbc's corpse map, wowdev) as a whole, through that continent's data (its pak on the card), no player arrow
// (the player is not on that map).
void WorldScreen::openDungeonMap(MapUi& m, const glm::vec3& pos) {
    auto& w = m.world;
    w.dLevel = 0;
    const fmt::DungeonMapView& dv = m.dg.dmap;
    if (!dv.h) return;
    const uint32_t playerMap = session_.worldEntry().mapId;
    if (dv.h->wma) {
        for (int i = 0; i < static_cast<int>(w.zones.size()); ++i) {
            if (w.zones[i].wmaID != dv.h->wma) continue;
            w.state.setCurrentZoneIdx(i);
            w.state.setLevel(wmap::ViewLevel::ZONE);
            if (dv.levels() > 0) w.dLevel = dungeonLevelAt(dv, m.dg.group >= 0, pos.x, pos.y);
            break;
        }
        PSP_LOGI(pp::tag::Gfx, "worldmap_dungeon map=%u wma=%u levels=%d level=%d group=%d", static_cast<unsigned>(playerMap),
                 static_cast<unsigned>(dv.h->wma), dv.levels(), w.dLevel, m.dg.group);
        return;
    }
    const int32_t cm = dv.h->corpseMap;
    bool ok = false;
    if (cm >= 0 && contMapDir(static_cast<uint32_t>(cm)) && contAvailable(w, static_cast<uint32_t>(cm)) &&
        switchMapData(m, static_cast<uint32_t>(cm), playerMap)) {
        const int ci = wmap::continentZoneIndex(w.zones, static_cast<uint32_t>(cm));
        if (ci >= 0) {
            w.state.setContinentIdx(ci);
            w.state.setCurrentZoneIdx(ci);
            w.state.setLevel(wmap::ViewLevel::CONTINENT);
            ok = true;
        }
    }
    PSP_LOGI(pp::tag::Gfx, "worldmap_instance map=%u corpse_map=%d x=%.0f y=%.0f shown=%d", static_cast<unsigned>(playerMap),
             static_cast<int>(cm), dv.h->corpseX, dv.h->corpseY, ok ? 1 : 0);
}

void WorldScreen::openWorldMap() {
    if (!map_) initMap();
    MapUi& m = *map_;
    auto& w = m.world;
    if (m.mini < 0) m.mini = pp::configGetInt("worldmapmini", 0) != 0 ? 1 : 0;  // GetCVarBool("miniWorldMap")
    openWindow(kWinWorldMap);
    if (w.open) return;
    w.open = true;
    w.ddOpen = 0;  // WorldMapFrame_OnShow: CloseDropDownMenus
    playUiSound(UiSound::QuestLogOpen);  // WorldMapFrame_OnShow - igQuestLogOpen
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    loadFrameArt(w);  // the frame (also without the world map data)
    const MinimapStore::Span* s = m.storeOk ? m.store.find(MinimapStore::kWorldHeaderKey) : nullptr;
    if (s) {
        w.hdrBlob.resize(s->size);
        w.ok = m.store.readBlocking(s->offset, s->size, w.hdrBlob.data()) &&
               w.view.attach(w.hdrBlob.data(), static_cast<uint32_t>(w.hdrBlob.size()));
    }
    // which continents can be shown (their minimap pak on the card) and the levels
    // above them (data/worldmap.pak: its WorldMapContinent rows now, the art when a level is shown).
    w.contAvail.assign(w.contMaps.size(), 0);
    for (size_t i = 0; i < w.contMaps.size(); ++i) {
        const char* dir = contMapDir(w.contMaps[i]);
        char rel[64];
        std::snprintf(rel, sizeof(rel), "data/minimap_%s.pak", dir ? dir : "none");
        w.contAvail[i] = dir && fileOnCard(rel) ? 1 : 0;
    }
    w.levelsAvail = false;
    {
        char path[256];
        pp::fsPath(path, sizeof(path), "data/worldmap.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            readLevelHeader(pak, w);
            pak.close();
            w.levelsAvail = w.wl.h != nullptr;
        }
    }
    if (!w.ok) {
        PSP_LOGW(pp::tag::Fs, "worldmap_missing (no \"wmhdr\" in data/minimap_<map>.pak)");
        return;
    }
    w.view.buildZones(w.zones);
    appendSyntheticZones(w);  // upstream DataRepository's Cosmic / World zones
    w.shownMap = m.store.header().h->mapId;
    m.store.header().exploreFlags(w.exploreFlags);
    // the World / Cosmic levels as upstream's 3.3.5a DataRepository (buildCosmicView) - when
    // data/worldmap.pak is on the card (else the continent is the top, as before).
    w.state.setCosmicEnabled(w.levelsAvail);
    const glm::vec3 pos(playerX_, playerY_, playerZ_);  // server = upstream render space
    // upstream WorldMapFacade::render's virtual continent - the player's zone
    // shown under another map (WorldMapArea DisplayMapID) opens that continent's data, the player shifted onto
    // it (the zone's WorldMapTransforms offset, baked in its row there). PSP: DisplayMapID 0 counts too - upstream
    // takes 0 for "none", but map 530's four northern zones (two zones, a capital city, an island) are
    // DisplayMapID 0 in 3.3.5a and the client shows them on map 0's continent (-1 is "none" there). Needs the
    // display continent's pak on the card; else the player's own map, as before.
    w.vMap = ~0u;
    w.vOffX = w.vOffY = 0.0f;
    glm::vec3 viewPos = pos;
    {
        const uint32_t ownMap = w.shownMap;
        const int pz = wmap::findZoneForPlayer(w.zones, pos, session_.worldStateZoneId());
        const uint32_t disp = pz >= 0 ? w.zones[pz].displayMapID : ~0u;
        if (pz >= 0 && disp != ownMap && disp != ~0u && contMapDir(disp) && contAvailable(w, disp)) {
            const uint32_t wma = w.zones[pz].wmaID;
            if (switchMapData(m, disp, ownMap)) {
                for (const wmap::Zone& z : w.zones)
                    if (z.wmaID == wma && z.mapID == ownMap) {
                        w.vMap = disp;
                        w.vOffX = z.virtualOffsetWowX;
                        w.vOffY = z.virtualOffsetWowY;
                        break;
                    }
                if (w.vMap == ~0u) (void)switchMapData(m, ownMap, ownMap);  // not on that continent's list: back
            }
            if (w.vMap != ~0u) (void)shownPosition(w, ownMap, pos, viewPos);
            PSP_LOGI(pp::tag::Gfx, "worldmap_virtual wma=%u map=%u display=%u ok=%d off_x=%.0f off_y=%.0f",
                     static_cast<unsigned>(wma), static_cast<unsigned>(ownMap), static_cast<unsigned>(disp),
                     w.vMap != ~0u ? 1 : 0, w.vOffX, w.vOffY);
        }
    }
    showPlayerView(m, viewPos, session_.worldStateZoneId());  // WorldMapFrame_OnShow: SetMapToCurrentZone
    openDungeonMap(m, pos);  // SetMapToCurrentZone inside an instance
    updateExploration(m, session_, pos);
    w.nextExploreMs = pp::logUptimeMs() + 1000;
    uint32_t kb = 0, ms = 0;
    loadWorldArt(m, w.state.currentZoneIdx(), kb, ms);
    const int idx = w.state.currentZoneIdx();
    PSP_LOGI(pp::tag::Gfx, "worldmap_open zone=%u wma=%u level=%s zones=%u hdr_kb=%u kb=%u frame_kb=%u ms=%u",
             idx >= 0 ? static_cast<unsigned>(w.zones[idx].areaID) : 0u,
             idx >= 0 ? static_cast<unsigned>(w.zones[idx].wmaID) : 0u,
             w.state.currentLevel() == wmap::ViewLevel::ZONE ? "zone" : "continent",
             static_cast<unsigned>(w.zones.size()), static_cast<unsigned>(w.hdrBlob.size() / 1024u),
             static_cast<unsigned>(kb), static_cast<unsigned>(w.frame.bytes / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    // what the dropdown and the World level can show (1 = the continent's pak is on the card).
    char avail[16] = {};
    for (size_t i = 0; i < w.contAvail.size() && i + 1 < sizeof(avail); ++i) avail[i] = w.contAvail[i] ? '1' : '0';
    PSP_LOGI(pp::tag::Gfx, "worldmap_levels on=%d conts=%s map=%u", w.levelsAvail ? 1 : 0, avail,
             static_cast<unsigned>(w.shownMap));
    (void)ms;
}

bool WorldScreen::mapCloseDropDown() {
    if (!map_ || !map_->world.open || map_->world.ddOpen == 0 || !windowOpen(kWinWorldMap)) return false;
    map_->world.ddOpen = 0;
    return true;
}

bool WorldScreen::mapWindowAltAction(uint32_t alt) {
    const bool zoneSpot = alt >= kIdMapZone0 && alt < kIdMapZoneEnd;  // as the map under it
    if ((alt < kIdMapArea || alt >= kIdMapDdItemEnd) && !zoneSpot) return false;
    if (!map_) return false;
    auto& w = map_->world;
    if ((alt != kIdMapArea && !zoneSpot) || !w.open || !w.ok) return true;
    // WorldMapButton_OnClick, RightButton: CloseDropDownMenus, WorldMapZoomOutButton_OnClick.
    w.ddOpen = 0;
    // zone -> continent -> World -> Cosmic (was: zone -> continent only).
    if (zoomOutOnce(*map_)) viewChanged(*map_, session_, glm::vec3(playerX_, playerY_, playerZ_));
    return true;
}

bool WorldScreen::mapWindowAction(uint32_t act) {
    if (act != kIdMenuMap && act != kIdMapLevelDrop && (act < kIdZoomIn || act >= kIdMapDdItemEnd) &&
        (act < kIdMapZone0 || act >= kIdMapZoneEnd))  // + WorldMapLevelDropDown
        return false;
    if (!map_) initMap();
    MapUi& m = *map_;
    auto& w = m.world;
    const glm::vec3 pos(playerX_, playerY_, playerZ_);
    switch (act) {
        case kIdMenuMap:
            closeWindow(kWinMenu);  // the map covers it anyway
            openWorldMap();
            return true;
        case kIdZoomIn:
        case kIdZoomOut:  // upstream Minimap::zoomIn / zoomOut
            m.zoom = std::clamp(m.zoom + (act == kIdZoomIn ? -kZoomStep : kZoomStep), kZoomMin, kZoomMax);
            PSP_LOGI(pp::tag::Ctrl, "minimap_zoom radius=%d", static_cast<int>(m.zoom));
            // Settings survive a restart: kept like the client's CVar minimapZoom.
            pp::configSet("minimapzoom", std::to_string(static_cast<int>(m.zoom)));
            pp::configSave();
            return true;
        case kIdMapClose:  // UIPanelCloseButton: HideParentPanel
            w.ddOpen = 0;
            closeWindow(kWinWorldMap);
            return true;
        case kIdMapSize:  // WorldMapFrame_ToggleWindowSize - SetCVar("miniWorldMap"), the other layout
            m.mini = m.mini > 0 ? 0 : 1;
            w.ddOpen = 0;
            pp::configSet("worldmapmini", m.mini ? "1" : "0");
            pp::configSave();
            winFocus_.setFocus(kIdMapSize);
            PSP_LOGI(pp::tag::Ctrl, "worldmap_size mini=%d", m.mini);
            return true;
        default: break;
    }
    if (!w.open || !w.ok) return true;
    const int before = w.state.currentZoneIdx();
    const wmap::ViewLevel levelBefore = w.state.currentLevel();
    const uint32_t mapBefore = w.shownMap;  // another continent's zones can reuse the indices
    const uint32_t playerMap = session_.worldEntry().mapId;
    const int cont = w.state.continentIdx();
    // GetCurrentMapContinent() is 0 / WORLDMAP_COSMIC_ID on the World / Cosmic level.
    const bool aboveContinents =
        levelBefore == wmap::ViewLevel::WORLD || levelBefore == wmap::ViewLevel::COSMIC;
    if (act == kIdMapContDrop || act == kIdMapZoneDrop || act == kIdMapLevelDrop) {
        // UIDropDownMenuTemplate's button: ToggleDropDownMenu + igMainMenuOptionCheckBoxOn.
        const int list = act == kIdMapContDrop ? 1 : act == kIdMapZoneDrop ? 2 : 3;  // 3 = the levels
        playSoundByName("igMainMenuOptionCheckBoxOn");
        if (w.ddOpen == list) {
            w.ddOpen = 0;
            return true;
        }
        w.ddItems.clear();
        int checked = 0;
        if (list == 1) {  // WorldMapContinentsDropDown_Initialize: GetMapContinents
            const uint32_t shownMap = cont >= 0 && !aboveContinents ? w.zones[cont].mapID : ~0u;
            for (int i = 0; i < static_cast<int>(w.contMaps.size()) && i < kDdMaxItems; ++i) {
                w.ddItems.push_back(i);
                if (w.contMaps[i] == shownMap) checked = i;
            }
        } else if (list == 3) {  // WorldMapLevelDropDown_Initialize - 1..GetNumDungeonMapLevels()
            for (int i = 0; dungeonZoneShown(m) && i < m.dg.dmap.levels() && i < kDdMaxItems; ++i) {
                if (i + 1 == w.dLevel) checked = i;
                w.ddItems.push_back(i);
            }
        } else if (!aboveContinents) {  // WorldMapZoneDropDown_Initialize: GetMapZones(GetCurrentMapContinent())
            for (int z : zonesOnContinent(w, cont)) {
                if (static_cast<int>(w.ddItems.size()) >= kDdMaxItems) break;
                if (z == before && w.state.currentLevel() == wmap::ViewLevel::ZONE)
                    checked = static_cast<int>(w.ddItems.size());
                w.ddItems.push_back(z);
            }
        }
        if (w.ddItems.empty()) return true;
        w.ddOpen = list;
        w.ddLeftMs = 0;
        winFocus_.setFocus(kIdMapDdItem0 + static_cast<uint32_t>(checked));  // the cursor onto the list
        PSP_LOGI(pp::tag::Ctrl, "worldmap_dd list=%d items=%u", list, static_cast<unsigned>(w.ddItems.size()));
        return true;
    }
    if (act >= kIdMapDdItem0 && act < kIdMapDdItemEnd) {
        const int item = static_cast<int>(act - kIdMapDdItem0), list = w.ddOpen;
        w.ddOpen = 0;
        if (item >= static_cast<int>(w.ddItems.size()) || list == 0) return true;
        playSoundByName("UChatScrollButton");  // UIDropDownMenuButton_OnClick
        winFocus_.setFocus(list == 1 ? kIdMapContDrop : list == 2 ? kIdMapZoneDrop : kIdMapLevelDrop);
        if (list == 3) {  // WorldMapLevelButton_OnClick - SetDungeonMapLevel(id): that floor's art
            w.dLevel = w.ddItems[item] + 1;
            uint32_t kb = 0, ms = 0;
            loadWorldArt(m, w.state.currentZoneIdx(), kb, ms);
            PSP_LOGI(pp::tag::Ctrl, "worldmap_level_set level=%d kb=%u", w.dLevel, static_cast<unsigned>(kb));
            return true;
        }
        if (list == 1) {  // WorldMapContinentButton_OnClick: SetMapZoom(continent) - upstream showMap(c, 0)
            // any continent whose pak is on the card (another one's is opened for the window).
            (void)showContinent(m, w.contMaps[w.ddItems[item]], playerMap);
        } else if (cont >= 0 && !aboveContinents) {  // WorldMapZoneButton_OnClick: SetMapZoom(continent, zone)
            w.state.setContinentIdx(cont);
            (void)w.state.enterZone(w.ddItems[item]);
        }
    } else {
        w.ddOpen = 0;  // a click elsewhere: CloseDropDownMenus
        const bool zoneSpot = act >= kIdMapZone0 && act < kIdMapZoneEnd;
        if (act == kIdMapArea || zoneSpot) {  // WorldMapButton_OnClick, LeftButton: upstream InputAction::ZOOM_IN
            // not on the continent shown -> no zone; on the continent of
            // their virtual zone - shifted onto it.
            glm::vec3 shownPos = pos;
            const bool playerShownHere = shownPosition(w, playerMap, pos, shownPos);
            int playerZone = playerShownHere ? wmap::findZoneForPlayer(w.zones, shownPos, session_.worldStateZoneId()) : -1;
            int candidate = w.hoverZone;
            // A zone spot (the D-pad's stop) - that zone, whatever the cursor hovers.
            if (zoneSpot && act - kIdMapZone0 < w.spots.size()) candidate = w.spots[act - kIdMapZone0].zone;
            if (w.state.currentLevel() == wmap::ViewLevel::CONTINENT) {
                if (candidate >= 0 && !wmap::zoneBelongsToContinent(w.zones, candidate, w.state.continentIdx()))
                    candidate = -1;
                if (playerZone >= 0 && !wmap::zoneBelongsToContinent(w.zones, playerZone, w.state.continentIdx()))
                    playerZone = -1;
                (void)w.state.zoomIn(candidate, playerZone);
            } else if (w.state.currentLevel() == wmap::ViewLevel::WORLD) {
                // ProcessMapClick on the World level - SetMapZoom(the continent under it).
                const int row = zoneSpot && act - kIdMapZone0 < w.spots.size() ? w.spots[act - kIdMapZone0].zone
                                                                                : w.hoverCont;
                if (w.wl.h && row >= 0 && row < static_cast<int>(w.wl.h->continentCount))
                    (void)showContinent(m, w.wl.conts[row].mapId, playerMap);
            } else if (w.state.currentLevel() == wmap::ViewLevel::COSMIC) {
                // AzerothButton OnClick SetMapZoom(0) - upstream zoomIn COSMIC -> WORLD
                // (enterWorldView's zone); OutlandButton SetMapZoom(3) - map 530's continent, when on the card.
                const int k = zoneSpot && act - kIdMapZone0 < w.spots.size() ? w.spots[act - kIdMapZone0].zone
                                                                              : w.hoverCont;
                int nb = 0;
                const fmt::CosmicButton* b = fmt::cosmicButtons(nb);
                if (k >= 0 && k < nb && b[k].mapId == 0) {
                    if (w.state.zoomIn(-1, -1).changed) w.state.setCurrentZoneIdx(w.worldIdx);
                } else if (k >= 0 && k < nb) {
                    (void)showContinent(m, b[k].mapId, playerMap);
                }
            }
        } else if (act == kIdMapUp) {  // WorldMapZoomOutButton_OnClick
            (void)zoomOutOnce(m);  // zone -> continent -> World -> Cosmic
        }
    }
    if (w.state.currentZoneIdx() != before || w.state.currentLevel() != levelBefore || w.shownMap != mapBefore)
        viewChanged(m, session_, pos);
    return true;
}

bool WorldScreen::drawMapWindow(UiRenderer& ui, uint8_t win) {
    if (win != kWinWorldMap) return false;
    PROF_SCOPE("worldmap");
    if (!map_) initMap();
    MapUi& m = *map_;
    auto& w = m.world;
    auto& art = w.frame;
    loadFrameArt(w);
    loadNpcArt(false);  // UIPanelCloseButton, UIPanelButtonTemplate, the list's title highlight
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& small = kit_.font(kFontSmall);
    const Font& normal = kit_.font(kFontNormal);
    const Font& tiny = kit_.font(kFontTiny);
    const uint32_t now = pp::logUptimeMs();
    // The mini mode is a window like the others (drawWindows offsets it) - the cursor in its coordinates.
    if (m.mini < 0) m.mini = pp::configGetInt("worldmapmini", 0) != 0 ? 1 : 0;
    const bool mini = m.mini > 0;
    g_mapRect = mini ? kMiniMapRect : kMapRectFull;
    if (mini) w.ddOpen = 0;  // no dropdowns in the mini mode
    const float cx = winFocus_.cursorX() - ui.offsetX(), cy = winFocus_.cursorY() - ui.offsetY();

    // BlackoutWorld: the whole screen black behind the frame (not in the mini mode: the world shows).
    // PSP (no black bars at the sides - the game in the background): the frame is 352 of 480 px
    // wide, so the world shows at the sides, dimmed - wowee.cfg worldmapdim = the black's alpha (0..255, 255 = the
    // original's black, default 128); the HUD is hidden as the original hides UIParent (drawHudLayers).
    if (m.dim < 0) m.dim = std::clamp(pp::configGetInt("worldmapdim", 128), 0, 255);
    if (!mini && m.dim > 0)
        ui.fill(Rect{0.0f, 0.0f, static_cast<float>(pp::kScreenW), static_cast<float>(pp::kScreenH)},
                static_cast<uint32_t>(m.dim) << 24);
    if (focusable) winFocus_.addStop(kIdMapArea, g_mapRect);  // WorldMapButton (under the frame's widgets)

    const int idx = w.ok ? w.state.currentZoneIdx() : -1;
    const bool zoneLevel = w.ok && w.state.currentLevel() == wmap::ViewLevel::ZONE;
    const bool contLevel = w.ok && w.state.currentLevel() == wmap::ViewLevel::CONTINENT;
    const int cont = w.ok ? w.state.continentIdx() : -1;
    // the World / Cosmic levels; the player is on the map shown (or not).
    const bool worldLevel = w.ok && w.state.currentLevel() == wmap::ViewLevel::WORLD;
    const bool cosmicLevel = w.ok && w.state.currentLevel() == wmap::ViewLevel::COSMIC;
    const uint32_t playerMap = session_.worldEntry().mapId;
    // the continent of the player's virtual zone places them (and the units
    // around them, the same map) shifted by the zone's offset (shownPosition).
    const bool playerVirtual = w.vMap != ~0u && w.shownMap == w.vMap && w.shownMap != playerMap;
    const bool playerHere = w.shownMap == playerMap || playerVirtual;
    if (w.alt) w.alt->update(m.frame);  // another continent's pak: its file closes after 3 s without a read
    // On the continent view a small stop inside each zone (over the map's stop): the D-pad walks the
    // zones (the cursor lands inside one, so the highlight follows), Cross zooms in, the nub still hovers freely.
    if (contLevel && w.spotsContinent != cont) buildZoneSpots(w, cont);
    // on the World / Cosmic level the continents / the two buttons are the stops.
    if (worldLevel && w.spotsContinent != -2) buildLevelSpots(w, false);
    if (cosmicLevel && w.spotsContinent != -3) buildLevelSpots(w, true);
    w.mapOnScreen = Rect{g_mapRect.x + ui.offsetX(), g_mapRect.y + ui.offsetY(), g_mapRect.w, g_mapRect.h};
    if ((contLevel || worldLevel || cosmicLevel) && focusable && w.ddOpen == 0) {
        for (size_t k = 0; k < w.spots.size(); ++k) {
            const float sx = g_mapRect.x + w.spots[k].u * g_mapRect.w, sy = g_mapRect.y + w.spots[k].v * g_mapRect.h;
            winFocus_.addStop(kIdMapZone0 + static_cast<uint32_t>(k), Rect{sx - 5.0f, sy - 5.0f, 10.0f, 10.0f});
        }
    }
    std::vector<TipLine> tip;
    Rect tipOwner{};
    bool tipLeft = false;
    auto tipSide = [&](float x) { return x > g_mapRect.x + g_mapRect.w * 0.5f; };  // ANCHOR_LEFT / ANCHOR_RIGHT

    if (w.ok) {
        const glm::vec3 pos(playerX_, playerY_, playerZ_);
        if (now >= w.nextExploreMs) {  // exploration once a second (upstream: every frame)
            updateExploration(m, session_, pos);
            w.nextExploreMs = now + 1000;
        }
        // WorldMapHighlight: only on the continent view; the set is freed off it.
        if (contLevel && w.hlContinent != cont) loadHighlights(m, cont);
        else if (!contLevel && w.hlContinent >= 0) m.releaseHighlights();

        // WorldMapButton_OnUpdate: UpdateMapHighlight under the cursor (not under an open list).
        w.hoverZone = -1;
        w.hoverName.clear();
        w.areaName.clear();
        w.areaExplored = true;
        const bool overList = w.ddOpen != 0 && w.ddRect.contains(cx, cy);
        const bool over = focusable && g_mapRect.contains(cx, cy) && !overList;
        w.hoverCont = -1;
        if (over) {
            const float u = (cx - g_mapRect.x) / g_mapRect.w, v = (cy - g_mapRect.y) / g_mapRect.h;
            if (worldLevel) {  // UpdateMapHighlight on the World level - the continent, its name
                w.hoverCont = worldContinentAt(w, u * kMapW, v * kMapH);
                if (w.hoverCont >= 0) {
                    const uint32_t hm = w.wl.conts[w.hoverCont].mapId;
                    w.areaName = mapContName(w, hm);
                    w.areaExplored = contAvailable(w, hm);  // PSP: grey when its pak is not on the card
                }
            } else if (cosmicLevel) {  // the OutlandButton / AzerothButton under it (their highlight only)
                w.hoverCont = cosmicButtonAt(u * kMapW, v * kMapH);
            } else if (!zoneLevel) {
                w.hoverZone = zoneAtMapPoint(w, u, v);
                if (w.hoverZone >= 0) w.areaName = w.hoverName = zoneTitle(w, w.hoverZone);
            } else if (m.storeOk || w.alt) {
                subzoneAt(w, m.shownStore().header(), idx, u, v, w.areaName, w.areaExplored);
            }
        }
        // the D-pad's stop on a continent / button counts as the cursor over it.
        const uint32_t focused = winFocus_.focused();
        if ((worldLevel || cosmicLevel) && focusable && focused >= kIdMapZone0 && focused - kIdMapZone0 < w.spots.size()) {
            w.hoverCont = w.spots[focused - kIdMapZone0].zone;
            if (worldLevel) {
                const uint32_t hm = w.wl.conts[w.hoverCont].mapId;
                w.areaName = mapContName(w, hm);
                w.areaExplored = contAvailable(w, hm);
            }
        }

        // The map picture (CompositeRenderer): base tiles, explored overlays, fog tint.
        const float s = g_mapRect.w / kMapW;
        ui.fill(g_mapRect, 0xFF1F140Du);  // upstream kBackground (13, 20, 31)
        ui.pushClip(g_mapRect);
        bool anyArt = false;
        for (int n = 0; n < 12; ++n) {
            if (!w.haveBase[n]) continue;
            anyArt = true;
            const float cs = 256.0f * s;
            ui.image(w.base[n], Rect{g_mapRect.x + (n % 4) * cs, g_mapRect.y + (n / 4) * cs, cs, cs});
        }
        // the World / Cosmic level (WorldMapDetailTile1..12 = World<n> / Cosmic<n>) and the hovered
        // continent's <Folder>Highlight on its WorldMapContinent square (ADD) / the hovered button's highlight.
        if (worldLevel || cosmicLevel) {
            for (int n = 0; n < 12; ++n) {
                if (!w.haveLvlTile[n]) continue;
                anyArt = true;
                const float cs = 256.0f * s;
                ui.image(w.lvlTiles[n], Rect{g_mapRect.x + (n % 4) * cs, g_mapRect.y + (n / 4) * cs, cs, cs});
            }
            if (w.hoverCont >= 0 && w.hoverCont < static_cast<int>(w.lvlHl.size()) && w.lvlHl[w.hoverCont].pixels) {
                if (worldLevel) {
                    float x0, y0, bw, bh;
                    fmt::worldLevelBox(w.wl.conts[w.hoverCont], x0, y0, bw, bh);
                    const float side = fmt::worldLevelHighlightSide(w.wl.conts[w.hoverCont]);
                    ui.image(w.lvlHl[w.hoverCont], Rect{g_mapRect.x + x0 * s, g_mapRect.y + y0 * s, side * s, side * s},
                             0xFFFFFFFFu, Blend::Add);
                } else {
                    int nb = 0;
                    const fmt::CosmicButton* b = fmt::cosmicButtons(nb);
                    if (w.hoverCont < nb) {
                        const fmt::CosmicButton& cb = b[w.hoverCont];
                        ui.image(w.lvlHl[w.hoverCont],
                                 Rect{g_mapRect.x + cb.hlX * s, g_mapRect.y + cb.hlY * s, cb.hlW * s, cb.hlH * s}, 0xFFFFFFFFu,
                                 Blend::Add);
                    }
                }
            }
        }
        if (idx >= 0 && w.zones[idx].areaID != 0) {
            const auto& zone = w.zones[idx];
            bool anyExplored = false;
            for (int oi = 0; oi < static_cast<int>(zone.overlays.size()); ++oi) {
                if (!w.exploration.exploredOverlays().count(oi)) continue;
                anyExplored = true;
                if (oi >= static_cast<int>(w.haveOverlay.size()) || !w.haveOverlay[oi]) continue;
                const auto& ov = zone.overlays[oi];
                ui.image(w.overlays[oi], Rect{g_mapRect.x + ov.offsetX * s, g_mapRect.y + ov.offsetY * s,
                                               ov.texWidth * s, ov.texHeight * s});
            }
            if (w.exploration.hasServerMask() && !anyExplored && !zone.overlays.empty())
                ui.fill(g_mapRect, abgr(38, 38, 51, 140));  // upstream fog tint (0.15, 0.15, 0.2, 0.55)
        }
        // WorldMapHighlight (alphaMode ADD): the hovered zone's highlight over its WorldMapArea rect on
        // the continent (TexCoords 0..texPercentage = the baked content).
        if (contLevel && w.hoverZone >= 0 && w.hoverZone < static_cast<int>(w.hl.size()) &&
            w.hl[w.hoverZone].pixels) {
            bool isCont = false;
            const wmap::ZoneBounds cb = wmap::projectionBoundsFor(w.zones, cont, isCont);
            const float du = cb.locLeft - cb.locRight, dv = cb.locTop - cb.locBottom;
            if (std::abs(du) > 0.001f && std::abs(dv) > 0.001f) {
                const wmap::ZoneBounds& zb = w.zones[w.hoverZone].bounds;
                const float u0 = (cb.locLeft - zb.locLeft) / du, u1 = (cb.locLeft - zb.locRight) / du;
                const float v0 = (cb.locTop - zb.locTop) / dv, v1 = (cb.locTop - zb.locBottom) / dv;
                ui.image(w.hl[w.hoverZone], Rect{g_mapRect.x + u0 * g_mapRect.w, g_mapRect.y + v0 * g_mapRect.h,
                                                 (u1 - u0) * g_mapRect.w, (v1 - v0) * g_mapRect.h},
                         0xFFFFFFFFu, Blend::Add);
            }
        }
        ui.popClip();
        if (!anyArt)
            ui.textShadow(small, g_mapRect.x + g_mapRect.w * 0.5f, g_mapRect.y + g_mapRect.h * 0.5f - 6,
                          OWN_STR(MAP_ZONE_NOT_BAKED), kColGrey, Align::Center);

        // Marks: quest givers (upstream setQuestPois from getNpcQuestStatuses), then the corpse, the
        // group and the player.
        bool isContinent = false;
        wmap::ZoneBounds bounds = wmap::projectionBoundsFor(w.zones, idx, isContinent);
        // a dungeon level - DungeonMap.dbc's bounds of that floor (the floor's art is drawn to them).
        if (const fmt::DungeonFloor* fl = shownFloor(m)) {
            bounds.locLeft = fl->locLeft;
            bounds.locRight = fl->locRight;
            bounds.locTop = fl->locTop;
            bounds.locBottom = fl->locBottom;
            isContinent = false;
        }
        // a position of the player's map on the continent of their virtual zone.
        auto shifted = [&](const glm::vec3& p) { return glm::vec3(p.x + w.vOffX, p.y + w.vOffY, p.z); };
        auto toScreen = [&](const glm::vec3& p, float& sx, float& sy) {
            float u = 0.0f, v = 0.0f;
            if (worldLevel) {  // the player's map through its WorldMapContinent row
                // a virtual zone's player through its display continent's row (map 530's
                // northern zones on map 0's continent: the World level shows them there, as the client).
                const bool virt = w.vMap != ~0u;
                const glm::vec3 q = virt ? shifted(p) : p;
                if (!worldLevelPlayerUV(w, virt ? w.vMap : playerMap, q.x, q.y, u, v)) return false;
            } else if (cosmicLevel || !playerHere) {
                return false;  // no positions on the Cosmic map / on another continent
            } else {
                const glm::vec2 uv = wmap::renderPosToMapUV(playerVirtual ? shifted(p) : p, bounds, isContinent);
                if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return false;
                u = uv.x;
                v = uv.y;
            }
            sx = g_mapRect.x + u * g_mapRect.w;
            sy = g_mapRect.y + v * g_mapRect.h;
            return true;
        };
        // on the World level only the player's arrow (the quest givers, the corpse and the group
        // are marks of a continent or zone map).
        auto markScreen = [&](const glm::vec3& p, float& sx, float& sy) { return !worldLevel && toScreen(p, sx, sy); };
        auto hovered = [&](float x, float y, float r) {
            return focusable && !overList && std::fabs(cx - x) <= r && std::fabs(cy - y) <= r;
        };
        int marks = 0;
        for (const auto& [guid, status] : session_.getNpcQuestStatuses()) {
            const game::QuestGiverMarker mk = game::questGiverMarker(status);
            if (!mk.symbol || marks >= 24) continue;
            glm::vec3 p;
            float h = 0, sx = 0, sy = 0;
            if (!unitPosition(guid, p, h) || !markScreen(p, sx, sy)) continue;
            ui.textShadow(normal, sx, sy - 8, mk.symbol, mk.dim ? kColGrey : kColGold, Align::Center);
            ++marks;
        }
        // upstream CorpseMarkerLayer - the spirit healer while dead or a ghost
        // (SMSG_DEATH_RELEASE_LOC), the corpse while a ghost (ui_world_death.cpp draws them).
        // WorldMapCorpse / WorldMapDeathRelease (WorldMapCorpseTemplate: the POIIcons
        // tombstone, 16 units; PSP 10 px) and their tooltips CORPSE_RED / SPIRIT_HEALER_RELEASE_RED.
        {
            const auto life = session_.life();
            float gx = 0, gy = 0, kx = 0, ky = 0;
            const bool healer = life != pp::WorldSession::Life::Alive && session_.graveyardKnown() &&
                                markScreen(session_.graveyardPosition(), gx, gy);
            const bool corpse = life == pp::WorldSession::Life::Ghost && session_.corpseKnown() &&
                                markScreen(session_.corpsePosition(), kx, ky);
            if (art.corpse.pixels) {
                if (healer) ui.image(art.corpse, Rect{gx - 5.0f, gy - 5.0f, 10.0f, 10.0f});
                if (corpse) ui.image(art.corpse, Rect{kx - 5.0f, ky - 5.0f, 10.0f, 10.0f});
            } else if (healer || corpse) {  // raw quads (no art baked): flush the staged sprites first
                ui.endFrame();
                ui.beginFrame();
                if (healer) mapDrawHealerCross(gx, gy);
                if (corpse) mapDrawWorldCorpse(kx, ky);
                ui.endFrame();
                ui.beginFrame();
            }
            if (corpse && hovered(kx, ky, 5.0f)) {
                tip.push_back({pp::text::uiPlain(CLIENT_STR(CORPSE_RED)), kColRed2020});
                tipOwner = Rect{kx - 5.0f, ky - 5.0f, 10.0f, 10.0f};
                tipLeft = tipSide(kx);
            } else if (healer && hovered(gx, gy, 5.0f)) {
                tip.push_back({pp::text::uiPlain(CLIENT_STR(SPIRIT_HEALER_RELEASE_RED)), kColRed2020});
                tipOwner = Rect{gx - 5.0f, gy - 5.0f, 10.0f, 10.0f};
                tipLeft = tipSide(gx);
            }
        }
        // WorldMapUnit_OnEnter: the names of every unit under the cursor, one per line (the player first).
        std::vector<TipLine> units;
        Rect unitOwner{};
        bool unitLeft = false;
        float psx = 0, psy = 0;
        bool playerShown = false;
        if (idx >= 0) {
            bool onMap = true;
            if (isContinent && !worldLevel) {  // upstream PlayerMarkerLayer: only when on this continent
                const int pz = wmap::findZoneForPlayer(w.zones, playerVirtual ? shifted(pos) : pos,
                                                       session_.worldStateZoneId());
                onMap = pz >= 0 && wmap::zoneBelongsToContinent(w.zones, pz, idx);
            }
            playerShown = onMap && toScreen(pos, psx, psy);  // the World level - its transform
        }
        if (playerShown && hovered(psx, psy, 6.0f)) {  // WorldMapPlayer (16 x 16 units): UnitName("player")
            if (const game::Character* me = session_.activeCharacter()) units.push_back({me->name, kColWhite});
            unitOwner = Rect{psx - 6.0f, psy - 6.0f, 12.0f, 12.0f};
            unitLeft = tipSide(psx);
        }
        std::vector<wmap::PartyDot> party;
        collectPartyDots(session_, party);
        for (const wmap::PartyDot& d : party) {  // WorldMapParty1..4 (WorldMapPartyIcon, 16 units; PSP 8 px)
            float sx = 0, sy = 0;
            if (!markScreen(d.renderPos, sx, sy)) continue;
            if (art.party.pixels) ui.image(art.party, Rect{sx - 4.0f, sy - 4.0f, 8.0f, 8.0f});
            else ui.fill(Rect{sx - 2.0f, sy - 2.0f, 4.0f, 4.0f}, 0xFFFFB464u);
            if (hovered(sx, sy, 4.0f) && !d.name.empty()) {
                if (units.empty()) {
                    unitOwner = Rect{sx - 4.0f, sy - 4.0f, 8.0f, 8.0f};
                    unitLeft = tipSide(sx);
                }
                units.push_back({d.name, kColWhite});
            }
        }
        if (!units.empty()) {
            tip = std::move(units);
            tipOwner = unitOwner;
            tipLeft = unitLeft;
        }
        if (playerShown) {  // raw quad: flush the staged sprites first
            ui.endFrame();
            ui.beginFrame();
            // north up: server +X (north) is screen up, +Y (west) screen left
            // The arrow stays when the map window moves: raw GE vertices are screen
            // coordinates - add the window's offset (the mini mode is a movable window).
            mapDrawArrow(m.arrow.pixels ? &m.arrow : nullptr, psx + ui.offsetX(), psy + ui.offsetY(), -std::sin(facing_),
                         -std::cos(facing_), 16.0f);
            ui.endFrame();
            ui.beginFrame();
        }

        // WorldMapFrameAreaLabel (WorldMapTextFont, TOP +20,-20 of WorldMapFrameAreaFrame at TOP 0,-10).
        if (!w.areaName.empty()) {
            const Font& f = art.font.texture().pixels ? art.font : normal;
            textOutline(ui, f, g_mapRect.x + g_mapRect.w * 0.5f + 20.0f * kWmS, g_mapRect.y + 30.0f * kWmS,
                        w.areaName.c_str(), w.areaExplored ? kColWorldMapText : kColGrey);
        }
    } else {
        ui.fill(g_mapRect, 0xFF1F140Du);
        ui.textShadow(small, g_mapRect.x + 8, g_mapRect.y + 20, OWN_STR(WORLDMAP_NOT_BAKED1), kColRed);
        ui.textShadow(small, g_mapRect.x + 8, g_mapRect.y + 34, OWN_STR(WORLDMAP_NOT_BAKED2), kColRed);
    }

    // The mini mode's frame (UI-WorldMapSmall), its title, the close and the size-up buttons.
    if (mini) {
        if (art.mini.pixels) ui.image(art.mini, Rect{kMiniX, kMiniY, kMiniW, kMiniH});
        else kit_.panel(ui, Rect{kMiniX, kMiniY, kMiniW, kMiniH});
        // WorldMapFrameTitle: TOP of WorldMapDetailFrame 0,20 (its units) - in the title bar.
        ui.textShadow(small, g_mapRect.x + g_mapRect.w * 0.5f,
                      kMiniY + 10.0f * kMiniS - static_cast<float>(small.lineHeight()) * 0.5f, CLIENT_STR(WORLD_MAP),
                      kColGold, Align::Center);
        auto button = [&](uint32_t id, const Rect& r, const pp::Texture& up, const pp::Texture& down) {
            const bool f = focusable && winFocus_.addStop(id, r);
            const pp::Texture& t = f && winFocus_.isPressed(id) && down.pixels ? down : up;
            if (t.pixels) ui.image(t, r);
            if (f) {
                if (art.sizeHl.pixels && id == kIdMapSize) ui.image(art.sizeHl, r, 0xFFFFFFFFu, Blend::Add);
                else ui.fill(r, 0x40FFFFFFu);
            }
        };
        button(kIdMapClose, kMiniClose, npcArt_.close, npcArt_.closeDown);
        button(kIdMapSize, kMiniSize, art.bigger, art.biggerDown);  // WorldMapFrameSizeUpButton
        drawMapTooltip(ui, kit_, tipOwner, tipLeft, tip);  // WorldMapTooltip
        return true;
    }
    // The frame (WorldMapFrameTexture1..18): the top band, the sides beside the map, the bottom band.
    if (art.top.pixels) ui.image(art.top, Rect{kWmFrameX, 0.0f, kWmFrameW, kWmTopH});
    else ui.fill(Rect{kWmFrameX, 0.0f, kWmFrameW, kWmTopH}, 0xFF202020u);
    if (art.bottom.pixels) ui.image(art.bottom, Rect{kWmFrameX, g_mapRect.y + g_mapRect.h, kWmFrameW, kWmBottomH});
    if (art.sides.pixels) {
        ui.image(art.sides, Rect{kWmFrameX, g_mapRect.y, kWmSideL, g_mapRect.h}, SrcRect{0, 0, kWmSideL, g_mapRect.h});
        ui.image(art.sides, Rect{g_mapRect.x + g_mapRect.w, g_mapRect.y, kWmSideR, g_mapRect.h},
                 SrcRect{kWmSideL + 1.0f, 0, kWmSideR, g_mapRect.h});
    }
    // WorldMapFrameTitle (GameFontNormal, CENTER 0,372 = 12 units under the top).
    ui.textShadow(normal, kWmFrameX + kWmFrameW * 0.5f, 12.0f * kWmTy - static_cast<float>(normal.lineHeight()) * 0.5f,
                  CLIENT_STR(WORLD_MAP), kColGold, Align::Center);
    // WorldMapFrameCloseButton (UIPanelCloseButton).
    {
        const bool f = focusable && winFocus_.addStop(kIdMapClose, kCloseBtn);
        const pp::Texture& t = f && winFocus_.isPressed(kIdMapClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown
                                                                                                : npcArt_.close;
        if (t.pixels) ui.image(t, kCloseBtn);
        else ui.textShadow(small, kCloseBtn.x + kCloseBtn.w * 0.5f, kCloseBtn.y, "x", kColGold, Align::Center);
        if (f) ui.fill(kCloseBtn, 0x40FFFFFFu);
    }
    // WorldMapFrameSizeDownButton (UI-Panel-SmallerButton) - to the mini mode.
    {
        const bool f = focusable && winFocus_.addStop(kIdMapSize, kSizeBtn);
        const pp::Texture& t = f && winFocus_.isPressed(kIdMapSize) && art.smallerDown.pixels ? art.smallerDown : art.smaller;
        if (t.pixels) ui.image(t, kSizeBtn);
        else ui.textShadow(small, kSizeBtn.x + kSizeBtn.w * 0.5f, kSizeBtn.y, "-", kColGold, Align::Center);
        if (f) {
            if (art.sizeHl.pixels) ui.image(art.sizeHl, kSizeBtn, 0xFFFFFFFFu, Blend::Add);
            else ui.fill(kSizeBtn, 0x40FFFFFFu);
        }
    }
    // WorldMapContinentDropDown CONTINENT and WorldMapZoneDropDown ZONE (GameFontNormalSmall
    // labels at TOPLEFT 20,10; the text GameFontHighlightSmall, right-justified before the button).
    auto dropDown = [&](uint32_t id, const Rect& box, const char* label, const char* text, bool open) {
        drawDropDownBox(ui, art.dd, box);
        ui.textShadow(tiny, box.x + 4.0f, box.y - 4.0f, label, kColGold);
        const Rect btn{box.x + box.w - 17.0f, box.y + 2.0f, 14.0f, 14.0f};
        const bool f = w.ok && focusable && winFocus_.addStop(id, box);  // PSP: the whole box takes the click
        const bool down = (f && winFocus_.isPressed(id)) || open;
        const pp::Texture& bt = down && art.ddBtnDown.pixels ? art.ddBtnDown : art.ddBtn;
        if (bt.pixels) ui.image(bt, btn);
        if (f && art.ddHl.pixels) ui.image(art.ddHl, btn, 0xFFFFFFFFu, Blend::Add);
        if (text && text[0]) {
            const float tx0 = box.x + 7.0f, tw = btn.x - 2.0f - tx0;
            const float ty = box.y + (box.h - static_cast<float>(small.lineHeight())) * 0.5f;
            if (static_cast<float>(small.measure(text)) <= tw) ui.textShadow(small, btn.x - 2.0f, ty, text, kColWhite, Align::Right);
            else ui.textMarquee(small, Rect{tx0, ty, tw, static_cast<float>(small.lineHeight())}, text, kColWhite);
        }
    };
    // UIDropDownMenu_SetSelectedID / ClearAll: the continent on both views, the zone on a zone map.
    // ClearAll on the World / Cosmic level (GetCurrentMapContinent() 0 / WORLDMAP_COSMIC_ID).
    dropDown(kIdMapContDrop, kContBox, CLIENT_STR(CONTINENT),
             cont >= 0 && (zoneLevel || contLevel) ? continentTitle(w, cont) : "", w.ddOpen == 1);
    dropDown(kIdMapZoneDrop, kZoneBox, CLIENT_STR(ZONE), zoneLevel ? zoneTitle(w, idx) : "", w.ddOpen == 2);
    // WorldMapZoomOutButton ZOOM_OUT: IsZoomOutAvailable - up to the Cosmic level.
    drawPanelButton(ui, kIdMapUp, kZoomBtn, CLIENT_STR(ZOOM_OUT), w.ok && zoomOutAvailable(w), focusable);
    // WorldMapLevelDropDown (TOPRIGHT of the map -50, -35 units; PSP: inside the map's top right
    // corner) while the zone shown has levels (GetNumDungeonMapLevels > 0); no Up / Down buttons (PSP: the list).
    const bool levels = w.ok && dungeonZoneShown(m) && m.dg.dmap.levels() > 0;
    const Rect levelBox{g_mapRect.x + g_mapRect.w - 132.0f, g_mapRect.y + 10.0f, 126.0f, 18.0f};
    if (levels) dropDown(kIdMapLevelDrop, levelBox, "", levelTitle(m, w.dLevel).c_str(), w.ddOpen == 3);
    else if (w.ddOpen == 3) w.ddOpen = 0;

    // The open dropdown list (UIDropDownListTemplate): DropDownList1 under the box, dark dialog backdrop,
    // 16-unit buttons (PSP 12 px) - PSP: in columns when taller than the screen.
    w.ddRect = Rect{};
    if (w.ddOpen != 0 && w.ok && !w.ddItems.empty()) {
        const bool contList = w.ddOpen == 1;
        const bool levelList = w.ddOpen == 3;
        const Rect& box = contList ? kContBox : levelList ? levelBox : kZoneBox;
        std::vector<std::string> levelText;
        if (levelList)
            for (int it : w.ddItems) levelText.push_back(levelTitle(m, it + 1));
        const int n = static_cast<int>(w.ddItems.size());
        auto itemText = [&](int k) -> const char* {
            const int it = w.ddItems[k];
            if (contList) return it < static_cast<int>(w.contNames.size()) ? w.contNames[it].c_str() : "";
            if (levelList) return levelText[static_cast<size_t>(k)].c_str();
            return zoneTitle(w, it);
        };
        // PSP - a continent whose pak is not on the card cannot be shown (grey).
        auto itemEnabled = [&](int k) { return !contList || contAvailable(w, w.contMaps[w.ddItems[k]]); };
        auto itemChecked = [&](int k) {
            if (levelList) return w.ddItems[k] + 1 == w.dLevel;
            if (contList) return cont >= 0 && (zoneLevel || contLevel) && w.contMaps[w.ddItems[k]] == w.zones[cont].mapID;
            return zoneLevel && w.ddItems[k] == idx;
        };
        constexpr float kRowH = 12.0f, kPad = 7.0f, kCheckW = 12.0f;
        float textW = 0.0f;
        for (int k = 0; k < n; ++k) textW = std::max(textW, static_cast<float>(small.measure(itemText(k))));
        const float colW = std::min(textW + kCheckW + 6.0f, 200.0f);
        const float top = box.y + box.h;
        const int rowsMax = std::max(1, static_cast<int>((static_cast<float>(pp::kScreenH) - 2.0f - top - 2.0f * kPad) / kRowH));
        const int cols = (n + rowsMax - 1) / rowsMax;
        const int rows = (n + cols - 1) / cols;
        Rect r{box.x, top, colW * static_cast<float>(cols) + 2.0f * kPad, kRowH * static_cast<float>(rows) + 2.0f * kPad};
        r.x = std::clamp(r.x, 2.0f, static_cast<float>(pp::kScreenW) - 2.0f - r.w);
        w.ddRect = r;
        // UIDropDownMenu_OnUpdate: hidden UIDROPDOWNMENU_SHOW_TIME after the cursor left it.
        if (!focusable || r.contains(cx, cy)) {
            w.ddLeftMs = 0;
        } else if (w.ddLeftMs == 0) {
            w.ddLeftMs = now ? now : 1;
        } else if (now - w.ddLeftMs > kDropDownShowMs) {
            w.ddOpen = 0;
        }
        if (w.ddOpen != 0) {
            if (focusable) winFocus_.addBlocker(r);  // the list hides the map and the boxes under it
            if (art.ddBg.pixels) ui.tile(art.ddBg, Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xFFFFFFFFu);
            else ui.fill(Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xE0000000u);
            if (kit_.has(kTexDlgBorder)) ui.nineSlice(kit_.tex(kTexDlgBorder), r, 0.75f);
            for (int k = 0; k < n; ++k) {
                const Rect ir{r.x + kPad + static_cast<float>(k / rows) * colW, r.y + kPad + static_cast<float>(k % rows) * kRowH,
                              colW, kRowH};
                const bool on = itemEnabled(k);
                const bool f = on && focusable && winFocus_.addStop(kIdMapDdItem0 + static_cast<uint32_t>(k), ir);
                if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, ir, 0xFFFFFFFFu, Blend::Add);
                if (itemChecked(k)) {
                    if (art.ddCheck.pixels) ui.image(art.ddCheck, Rect{ir.x, ir.y + 1.0f, 10.0f, 10.0f});
                    else ui.textShadow(small, ir.x + 1.0f, ir.y, "*", kColGold);
                }
                ui.textShadow(small, ir.x + kCheckW, ir.y + (kRowH - static_cast<float>(small.lineHeight())) * 0.5f,
                              itemText(k), on ? kColWhite : kColDisableSmall);  // GameFontHighlightSmallLeft
            }
        }
    }

    drawMapTooltip(ui, kit_, tipOwner, tipLeft, tip);  // WorldMapTooltip
    return true;
}

}  // namespace wowee::ui::psp
