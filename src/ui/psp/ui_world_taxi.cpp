// Flight paths - the world screen half.
//
// TaxiFrame.xml / TaxiFrame.lua of the original ruRU 3.3.5a client, at the PSP's frame scale 0.6 (the PSP screen
// is 272 px high; GossipFrame's scale), anchored like the client's UIPanel at the top left:
//   - the frame: UI-TaxiFrame-TopLeft / TopRight / BotLeft / BotRight (384 x 512 units, drawn part 350 x 440),
//     baked by tools/psp/build_ui_pak.py into the four pieces around the map (ui.pak taxi_top / taxi_bottom /
//     taxi_left / taxi_right); TaxiPortrait (58 x 58 at 8,-9) = the NPC's 3D portrait under the ring; TaxiMerchant
//     = UnitName("npc") in GameFontNormal (TOP 5,-17, 250 x 14); TaxiCloseButton (UIPanelCloseButton TOPRIGHT
//     -29,-8);
//   - TaxiMap (316 x 352 at TOP -13,-75) = Interface\TaxiFrame\TaxiMap<map> (data/taxi.pak taximap_<map>);
//   - TaxiFrame_OnEvent TAXIMAP_OPENED: the nodes of the map by TaxiNodeGetType - CURRENT (UI-Taxi-Icon-Green),
//     REACHABLE (-White), DISTANT (-Yellow), NONE not shown - at TaxiNodePosition from the map's BOTTOMLEFT, nodes
//     closer than half a button pushed apart as the Lua does (Ebon Hold / Light's Hope); the highlight
//     UI-Taxi-Icon-Highlight (32 x 32) on the node under the cursor;
//   - DrawOneHopLines on show: a UI-Taxi-Line to every node one hop away; none - ERR_TAXINOPATHS (red) and the
//     frame hides (CloseTaxiMap); TaxiNodeOnButtonEnter: GameTooltip ANCHOR_RIGHT with TaxiNodeName (white) and
//     the fare (SetTooltipMoney, TaxiNodeCost) and the route's lines for a REACHABLE node, TAXINODEYOUAREHERE
//     (0.5, 1, 0.5) and the one-hop lines for CURRENT; DrawRouteLine's geometry (the texture along the line,
//     TAXIROUTE_LINEFACTOR 32/30, 32 units wide);
//   - OnClick = TakeTaxiNode (WorldSession::activateTaxi), OnShow / OnHide sounds igMainMenuOpen / Close,
//     OnHide = CloseTaxiMap.
// The node semantics and positions are upstream's Lua API (lua_system_api.cpp: taxiNodeOrder = the map's nodes by
// id, TaxiNodeGetType, GetNumRoutes = hops of getTaxiRouteTo, TaxiGetSrc/DestX/Y, TaxiNodeCost); PSP:
// TaxiNodePosition is the original's - the map's WorldMapContinent TaxiMin / TaxiMax square (upstream projects
// through the continent's WorldMapArea rectangle, which is not the TaxiMap picture's frame: the nodes land off the
// art), west left, north up.
// The flight (WorldSession::taxiDrivesPlayer) moves the player while it lasts: no stick / jump / sit, the camera
// follows behind (ui_world_controls.cpp), the taxi heartbeat every 0.25 s (upstream HEARTBEAT_INTERVAL_TAXI);
// after it upstream Application's landing clamp: the player is kept still and put on the floor closest to where
// the flight left them (terrain, buildings, models probed 2 yd above), 2 s once a floor is found.
// GossipFrame_OnEvent: a gossip whose only entry is the taxi option (no quests) is chosen at once - the taxi map
// opens without the gossip, as in the client.
// PSP: the node's focus rect is 22 units (13 px) for the cursor (the button is 16); no OnLeave line reset (the
// original keeps the last lines too).
#include "ui/psp/ui_world_screen.hpp"

#include "game/protocol_constants.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_texture.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <pspgu.h>
#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids: the range 0x7A00-0x7AFF.
constexpr uint32_t kIdTaxiClose = 0x7A00, kIdTaxiNode0 = 0x7A10, kMaxTaxiButtons = 0xF0;

constexpr float kS = 0.6f;               // TaxiFrame units -> px (build_taxi_pak.py / build_ui_pak.py TAXI_S)
constexpr float kFx = 4.0f, kFy = 2.0f;  // TaxiFrame TOPLEFT 0,-104 -> the left, at the top (264 px tall)
constexpr float kMapX = 21.0f, kMapY = 75.0f, kMapW = 316.0f, kMapH = 352.0f;  // TaxiMap / TaxiRouteMap
constexpr float kButtonHalf = 8.0f;      // TAXI_BUTTON_HALF_WIDTH / _HEIGHT
constexpr float kLineFactor = 32.0f / 30.0f;  // TAXIROUTE_LINEFACTOR

Rect FR(float x, float y, float w, float h) { return Rect{kFx + x * kS, kFy + y * kS, w * kS, h * kS}; }

enum : uint8_t { kTypeNone = 0, kTypeCurrent, kTypeReachable, kTypeDistant };

void releaseTex(pp::Texture& t) {
    if (t.pixels) pp::textureRelease(t);
}

int16_t snapCoord(float v) {
    const float r = std::floor(v + 0.5f);
    return static_cast<int16_t>(std::clamp(r, -2048.0f, 2047.0f));
}

}  // namespace

struct WorldScreen::TaxiUi {
    pp::Texture top, bottom, left, right, icon[3], hl, line, map;
    uint32_t mapId = ~0u;
    bool artTried = false;
    uint32_t artBytes = 0;
    bool shown = false, raisedCursor = false, focusCurrent = false, checkedHops = false;
    // TaxiFrame_OnEvent TAXIMAP_OPENED: per node of the map (ascending id) its type, the button's centre (units from
    // TaxiRouteMap's BOTTOMLEFT, pushed apart) and TaxiNodePosition (the lines' ends, not pushed).
    std::vector<uint32_t> ids;
    std::vector<uint8_t> type;
    std::vector<float> bx, by, rx, ry;
    uint32_t layoutSig = ~0u;
    std::vector<std::pair<uint32_t, uint32_t>> lines;  // TaxiRoute textures shown: node id pairs
    uint32_t hoverNode = 0;                             // the last TaxiNodeOnButtonEnter
    uint32_t tipNode = 0;                               // the GameTooltip this frame (drawn after the windows)
    uint8_t tipType = kTypeNone;
    Rect tipAnchor{};
    uint32_t autoSig = 0, autoMs = 0;                   // GossipFrame_OnEvent's SelectGossipOption(1)
    bool flying = false, clamp = false;
    float clampTimer = 0.0f, clampWait = 0.0f, refZ = 0.0f;
    uint32_t nextHeartbeatMs = 0, logs = 0;
    void releaseArt() {
        for (pp::Texture* t : {&top, &bottom, &left, &right, &icon[0], &icon[1], &icon[2], &hl, &line, &map})
            releaseTex(*t);
        artTried = false;
        artBytes = 0;
        mapId = ~0u;
    }
    ~TaxiUi() { releaseArt(); }
};

void WorldScreen::destroyTaxiUi(TaxiUi* p) { delete p; }

WorldScreen::TaxiUi& WorldScreen::taxiUi() {
    if (!taxi_) taxi_ = std::unique_ptr<TaxiUi, void (*)(TaxiUi*)>(new TaxiUi, &WorldScreen::destroyTaxiUi);
    return *taxi_;
}

namespace {

// The window's art: the frame pieces, icons and line from ui.pak, the map's TaxiMap from taxi.pak.
void loadTaxiArt(WorldScreen::TaxiUi& t, uint32_t mapId) {
    if (t.artTried && t.mapId == mapId) return;
    t.releaseArt();
    t.artTried = true;
    t.mapId = mapId;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    int frame = 0, icons = 0;
    auto loadFrom = [&](pp::Pak& pak, const char* name, pp::Texture& tex) {
        const pp::PakEntry* e = pak.find(name);
        void* blob = e ? pak.load(e) : nullptr;
        if (!blob) return false;
        if (!pp::textureFromBlob(blob, e->size, tex, true)) {
            pp::pakFree(blob);
            return false;
        }
        t.artBytes += e->size;
        return true;
    };
    char path[256];
    pp::fsPath(path, sizeof(path), "data/ui.pak");
    pp::Pak ui;
    if (ui.open(path)) {
        frame += loadFrom(ui, "taxi_top", t.top) ? 1 : 0;
        frame += loadFrom(ui, "taxi_bottom", t.bottom) ? 1 : 0;
        frame += loadFrom(ui, "taxi_left", t.left) ? 1 : 0;
        frame += loadFrom(ui, "taxi_right", t.right) ? 1 : 0;
        icons += loadFrom(ui, "taxi_ic_cur", t.icon[0]) ? 1 : 0;    // UI-Taxi-Icon-Green
        icons += loadFrom(ui, "taxi_ic_reach", t.icon[1]) ? 1 : 0;  // UI-Taxi-Icon-White
        icons += loadFrom(ui, "taxi_ic_far", t.icon[2]) ? 1 : 0;    // UI-Taxi-Icon-Yellow
        icons += loadFrom(ui, "taxi_ic_hl", t.hl) ? 1 : 0;          // UI-Taxi-Icon-Highlight
        loadFrom(ui, "taxi_line", t.line);                          // UI-Taxi-Line
        ui.close();
    }
    pp::fsPath(path, sizeof(path), "data/taxi.pak");
    pp::Pak tp;
    if (tp.open(path)) {
        char name[32];
        std::snprintf(name, sizeof(name), "taximap_%u", static_cast<unsigned>(mapId));
        loadFrom(tp, name, t.map);
        tp.close();
    }
    PSP_LOGI(pp::tag::Gfx, "taxi_art frame=%d/4 icons=%d/4 line=%d map=%d kb=%u ms=%u", frame, icons,
             t.line.pixels ? 1 : 0, t.map.pixels ? 1 : 0, static_cast<unsigned>((t.artBytes + 1023u) / 1024u),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
}

// DrawRouteLine(T, "TaxiRouteMap", sx, sy, ex, ey, 32) - the texture along the line, TAXIROUTE_LINEFACTOR longer
// and wider than the line, as one textured quad (the GE draws it rotated: a strip, not a sprite). Screen px in,
// inside the caller's clip (UiRenderer::pushClip flushed its sprites first, so the order holds).
void drawRouteLine(const pp::Texture& tex, float sx, float sy, float ex, float ey, float widthPx) {
    const float dx = ex - sx, dy = ey - sy;
    const float l = std::sqrt(dx * dx + dy * dy);
    if (l < 0.5f || !tex.pixels) return;
    const float ux = dx / l, uy = dy / l, nx = -uy, ny = ux;
    const float hl = l * 0.5f * kLineFactor, hw = widthPx * 0.5f * kLineFactor;
    const float cx = (sx + ex) * 0.5f, cy = (sy + ey) * 0.5f;
    struct Vert {
        uint16_t u, v;
        uint32_t color;
        int16_t x, y, z, pad;
    };
    static_assert(sizeof(Vert) == 16, "GE vertex");
    auto* v = static_cast<Vert*>(sceGuGetMemory(4 * sizeof(Vert)));
    const uint16_t tw = tex.uw, th = tex.uh;
    auto put = [&](int i, float along, float across, uint16_t u, uint16_t tv) {
        v[i] = Vert{u, tv, 0xFFFFFFFFu, snapCoord(cx + ux * along + nx * across), snapCoord(cy + uy * along + ny * across),
                    0, 0};
    };
    put(0, -hl, -hw, 0, 0);
    put(1, -hl, hw, 0, th);
    put(2, hl, -hw, tw, 0);
    put(3, hl, hw, tw, th);
    sceKernelDcacheWritebackRange(v, 4 * sizeof(Vert));
    pp::textureBind(&tex);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, nullptr,
                   v);
}

}  // namespace

// --- The window and the session ------------------------------------------------------------

void WorldScreen::updateTaxi(float dtSec) {
    TaxiUi& t = taxiUi();
    session_.updateTaxi(dtSec, glm::vec3(playerX_, playerY_, playerZ_));
    auto hidden = [&]() {  // TaxiFrame OnHide
        t.shown = false;
        playSoundByName("igMainMenuClose");
        PSP_LOGI(pp::tag::Gfx, "taxi_close freed_kb=%u", static_cast<unsigned>((t.artBytes + 1023u) / 1024u));
        t.releaseArt();
        std::vector<uint32_t>().swap(t.ids);
        std::vector<uint8_t>().swap(t.type);
        std::vector<float>().swap(t.bx);
        std::vector<float>().swap(t.by);
        std::vector<float>().swap(t.rx);
        std::vector<float>().swap(t.ry);
        t.lines.clear();
        if (t.raisedCursor) {
            t.raisedCursor = false;
            if (uiMode_ && windows_.empty() && !settings_.isOpen()) {
                setUiMode(false);
                suppress_ = ~0u;  // the Cross that chose a node must not jump
            }
        }
    };
    const bool want = session_.isTaxiWindowOpen();
    if (want && !t.shown) {  // TAXIMAP_OPENED
        openWindow(kWinTaxi);
        t.shown = true;
        t.layoutSig = ~0u;
        t.hoverNode = 0;
        t.focusCurrent = true;
        t.checkedHops = false;
        t.lines.clear();
        playSoundByName("igMainMenuOpen");
        if (!uiMode_) {
            setUiMode(true);
            t.raisedCursor = true;
            suppress_ = ~0u;  // everything held now stays dead until released
        }
        PSP_LOGI(pp::tag::Gfx, "taxi_window open nearest=%u nodes=%u", static_cast<unsigned>(session_.getTaxiCurrentNode()),
                 static_cast<unsigned>(session_.getTaxiNodes().size()));
    } else if (want && t.shown && !windowOpen(kWinTaxi)) {  // START / the close button: CloseTaxiMap
        session_.closeTaxi();
        hidden();
    } else if (!want && t.shown) {  // TAXIMAP_CLOSED (a flight asked for, too far, ...)
        closeWindow(kWinTaxi);
        hidden();
    }
}

// GossipFrame_OnEvent (GOSSIP_SHOW): no quests and one option that is not "gossip" - SelectGossipOption(1), the
// frame is not shown. Here only for the taxi option (icon 2); the others keep their window. While the answer is on
// its way the gossip stays hidden (3 s at most, then it shows as it is).
bool WorldScreen::taxiGossipAutoSelect() {
    TaxiUi& t = taxiUi();
    if (!session_.isGossipWindowOpen() || npc_.shown[0]) {
        t.autoSig = 0;
        return false;
    }
    const auto& g = session_.getCurrentGossip();
    if (!g.quests.empty() || g.options.size() != 1 || g.options[0].icon != 2) return false;
    const uint32_t sig = g.menuId ^ static_cast<uint32_t>(g.npcGuid) ^ (g.options[0].id << 24) ^ 0x7A000000u;
    const uint32_t nowMs = pp::logUptimeMs();
    if (t.autoSig != sig) {
        t.autoSig = sig;
        t.autoMs = nowMs;
        session_.selectGossipOption(g.options[0].id);
        PSP_LOGI(pp::tag::Net, "taxi_gossip_auto npc=%08X option=%u", static_cast<unsigned>(g.npcGuid & 0xFFFFFFFFu),
                 static_cast<unsigned>(g.options[0].id));
        return true;
    }
    return nowMs - t.autoMs <= 3000u;
}

bool WorldScreen::taxiLocksControls() const {
    return session_.isOnTaxiFlight() || session_.isTaxiActivationPending() || (taxi_ && taxi_->clamp);
}

// From updateMovement: the flight puts the player where the session's flight is; after it the landing clamp
// (upstream Application); while a flight is asked for the player stays (upstream: externalFollow).
bool WorldScreen::taxiMovement(float dtSec) {
    TaxiUi& t = taxiUi();
    const uint32_t nowMs = pp::logUptimeMs();
    auto still = [&]() {
        wantMove_ = false;
        moving_ = false;
        moveFlags_ = 0;
        jumpReq_ = false;
        mountJumpKey_ = mountJumpGo_ = false;
        animBackward_ = false;
        animStrafe_ = 0.0f;
    };
    glm::vec3 p;
    float o = 0.0f;
    if (session_.taxiDrivesPlayer(p, o)) {
        if (!t.flying) {
            t.flying = true;
            t.nextHeartbeatMs = 0;
            if (sitting_) setSitting(false, false);
            PSP_LOGI(pp::tag::Gfx, "taxi_fly mount=%u active=%d", static_cast<unsigned>(ownMountDisplay_),
                     mountActive_ ? 1 : 0);
        }
        playerX_ = p.x;
        playerY_ = p.y;
        playerZ_ = p.z;
        facing_ = o;
        airborne_ = false;
        vz_ = 0.0f;
        idleSec_ = 0.0f;  // no AFK sit in the air
        t.clamp = false;
        still();
        // upstream GameHandler::update: MSG_MOVE_HEARTBEAT every HEARTBEAT_INTERVAL_TAXI while on a taxi.
        if (nowMs >= t.nextHeartbeatMs) {
            t.nextHeartbeatMs = nowMs + static_cast<uint32_t>(game::HEARTBEAT_INTERVAL_TAXI * 1000.0f);
            sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, false);
        }
        return true;
    }
    t.flying = false;
    glm::vec3 land;
    if (session_.takeTaxiLanding(land)) {
        playerX_ = land.x;
        playerY_ = land.y;
        playerZ_ = land.z;
        airborne_ = false;
        vz_ = 0.0f;
        t.clamp = true;
        t.clampTimer = 2.0f;  // upstream: counts down only once a floor is found
        t.clampWait = 0.0f;
        t.refZ = land.z;
        still();
    }
    if (t.clamp) {
        // upstream landing clamp: the floor candidate closest to where the flight left the player, probed from a
        // fixed 2 yd above that height (WMO / M2, then terrain).
        float tz = 0.0f, mz = 0.0f;
        const bool haveT = terrain_.heightAt(playerX_, playerY_, tz);
        const bool haveM = models_.floorHeight(playerX_, playerY_, t.refZ + 2.0f, mz);
        float floor = 0.0f;
        const char* src = "none";
        if (haveM) {
            floor = mz;
            src = "model";
        }
        if (haveT && (!haveM || std::fabs(tz - t.refZ) < std::fabs(mz - t.refZ))) {
            floor = tz;
            src = "terrain";
        }
        if (haveT || haveM) {
            if (std::fabs(playerZ_ - floor) > 0.05f) {  // PSP: the feet on the floor (upstream +0.10)
                playerZ_ = floor;
                sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, false);
            }
            t.clampTimer -= dtSec;
        } else {
            t.clampWait += dtSec;  // no floor yet (the tiles stream in): keep the player still
        }
        if (t.clampTimer <= 0.0f || t.clampWait > 10.0f) {  // PSP: 10 s at most without a floor
            t.clamp = false;
            PSP_LOGI(pp::tag::Gfx, "taxi_land_clamp src=%s z=%.2f ref=%.2f wait=%.1f", src,
                     static_cast<double>(playerZ_), static_cast<double>(t.refZ), static_cast<double>(t.clampWait));
        }
        still();
        return true;
    }
    if (session_.isTaxiActivationPending()) {
        still();
        return true;
    }
    return false;
}

// --- Drawing ----------------------------------------------------------------------------------

bool WorldScreen::drawTaxiWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinTaxi) return false;
    PROF_SCOPE("taxi");  // one zone for the session and the window (the profiler has 80)
    TaxiUi& t = taxiUi();
    const uint32_t mapId = session_.taxiMapId() != ~0u ? session_.taxiMapId() : session_.worldEntry().mapId;
    loadTaxiArt(t, mapId);
    loadNpcArt(false);  // the close button, the coins (the NPC windows' small art)
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const auto& nodes = session_.getTaxiNodes();
    const uint32_t current = session_.getTaxiCurrentNode();

    // TaxiFrame_OnEvent TAXIMAP_OPENED: types and button positions (again when the session's map / mask changes).
    {
        uint32_t sig = current * 2654435761u ^ static_cast<uint32_t>(nodes.size());
        for (uint32_t i = 0; i < game::TLK_TAXI_MASK_SIZE; ++i)
            sig = sig * 31u + session_.getTaxiData().nodeMask[i];
        if (sig != t.layoutSig) {
            t.layoutSig = sig;
            t.ids.clear();
            t.type.clear();
            t.bx.clear();
            t.by.clear();
            t.rx.clear();
            t.ry.clear();
            float minX = 0, minY = 0, maxX = 1, maxY = 1;
            const bool bounds = session_.taxiMapBounds(minX, minY, maxX, maxY);
            for (const auto& n : nodes) {
                if (t.ids.size() >= kMaxTaxiButtons) break;
                uint8_t type = kTypeNone;
                if (n.id == current) type = kTypeCurrent;
                else if (!session_.isKnownTaxiNode(n.id)) type = kTypeNone;
                else type = session_.hasTaxiRouteTo(n.id) ? kTypeReachable : kTypeDistant;
                // TaxiNodePosition (PSP: the original's TaxiMin / TaxiMax square): x from the left, y from the bottom.
                const float u = bounds ? (maxY - n.y) / (maxY - minY) : 0.0f;
                const float v = bounds ? (n.x - minX) / (maxX - minX) : 0.0f;
                t.ids.push_back(n.id);
                t.type.push_back(type);
                t.rx.push_back(u * kMapW);
                t.ry.push_back(v * kMapH);
                t.bx.push_back(-1e9f);  // "no position" (the Lua's nil)
                t.by.push_back(-1e9f);
            }
            // The overlap pass of TaxiFrame_OnEvent (numValidFlightNodes > 1).
            int valid = 0;
            for (size_t i = 0; i < t.ids.size(); ++i) {
                if (t.type[i] == kTypeNone || !bounds) continue;
                ++valid;
                const float currX = t.rx[i], currY = t.ry[i];
                t.bx[i] = currX;
                t.by[i] = currY;
                if (valid > 1) {
                    for (size_t c = 0; c < i; ++c) {
                        if (t.bx[c] < -1e8f) continue;
                        const float checkX = t.bx[c], checkY = t.by[c];
                        if (currX > checkX - kButtonHalf && currX < checkX + kButtonHalf && currY > checkY - kButtonHalf &&
                            currY < checkY + kButtonHalf) {
                            t.bx[i] = currX + (currX - checkX) * 0.5f;
                            t.by[i] = currY + (currY - checkY) * 0.5f;
                            t.bx[c] = checkX + (checkX - currX) * 0.5f;
                            t.by[c] = checkY + (checkY - currY) * 0.5f;
                        }
                    }
                }
            }
            if (t.focusCurrent) {
                t.focusCurrent = false;
                for (size_t i = 0; i < t.ids.size(); ++i)
                    if (t.ids[i] == current) winFocus_.setFocus(kIdTaxiNode0 + static_cast<uint32_t>(i));
            }
            if (++t.logs <= 40)
                PSP_LOGI(pp::tag::Gfx, "taxi_layout map=%u nodes=%u shown=%d bounds=%d", static_cast<unsigned>(mapId),
                         static_cast<unsigned>(t.ids.size()), valid, bounds ? 1 : 0);
        }
    }
    // DrawOneHopLines: every node one hop away (GetNumRoutes == 1).
    auto oneHopLines = [&]() {
        t.lines.clear();
        int hops = 0;
        for (uint32_t id : t.ids) {
            const auto route = session_.getTaxiRouteTo(id);
            if (route.size() == 2) {
                t.lines.emplace_back(route[0], route[1]);
                ++hops;
            }
        }
        return hops;
    };
    if (!t.checkedHops) {  // OnShow
        t.checkedHops = true;
        if (oneHopLines() == 0) {
            uiError(CLIENT_STR(ERR_TAXINOPATHS));
            PSP_LOGI(pp::tag::Gfx, "taxi_no_paths nodes=%u", static_cast<unsigned>(nodes.size()));
            closeWindow(kWinTaxi);  // HideUIPanel(TaxiFrame) -> OnHide -> CloseTaxiMap (updateTaxi)
            return true;
        }
    }

    // --- The frame ---------------------------------------------------------------------------
    const uint64_t npcGuid = session_.getTaxiNpcGuid();
    if (const pp::Texture* face = portraitOf(npcGuid)) ui.imageCircle(*face, FR(8, 9, 58, 58));  // TaxiPortrait
    else ui.fillCircle(FR(10, 11, 54, 54), 0xFF101010u);
    if (t.top.pixels) {
        ui.image(t.top, FR(0, 0, 350, kMapY));
        ui.image(t.left, FR(0, kMapY, kMapX, kMapH));
        ui.image(t.right, FR(kMapX + kMapW, kMapY, 350 - kMapX - kMapW, kMapH));
        ui.image(t.bottom, FR(0, kMapY + kMapH, 350, 440 - kMapY - kMapH));
    } else {
        kit_.panel(ui, FR(0, 0, 350, 440));  // no baked frame (old ui.pak)
    }
    {  // TaxiMerchant: UnitName("npc"), GameFontNormal, TOP 5,-17 (250 x 14)
        UnitView u;
        const std::string name = unitView(npcGuid, u) && !u.name.empty() ? u.name : std::string();
        const Rect nr = FR(192 + 5 - 125, 17 - 2, 250, 18);
        const Font& nf = static_cast<float>(fn.measure(name.c_str())) <= nr.w ? fn : fs;
        if (static_cast<float>(nf.measure(name.c_str())) <= nr.w)
            ui.textShadow(nf, nr.x + nr.w * 0.5f, nr.y + (nr.h - nf.lineHeight()) * 0.5f, name.c_str(), kColGold,
                          Align::Center);
        else
            ui.textMarquee(fs, nr, name.c_str(), kColGold);
    }
    {  // TaxiCloseButton: UIPanelCloseButton (32 x 32) at TOPRIGHT -29,-8
        const Rect cr = Rect{kFx + (384 - 29 - 16) * kS - 10, kFy + (8 + 16) * kS - 10, 20, 20};
        const bool f = focusable && winFocus_.addStop(kIdTaxiClose, cr);
        const bool p = f && winFocus_.isPressed(kIdTaxiClose);
        const pp::Texture& ct = p && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (ct.pixels) ui.image(ct, cr);
        else ui.fill(cr, 0xFF202080u);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // TaxiMap (OVERLAY): the continent's TaxiMap picture.
    const Rect mapR = FR(kMapX, kMapY, kMapW, kMapH);
    if (t.map.pixels) ui.image(t.map, mapR);
    else ui.fill(mapR, 0xFF182028u);

    // TaxiRouteMap: the route lines (BACKGROUND of the child frame: over the map, under the buttons).
    auto mapPx = [&](float x, float yFromBottom, float& sx, float& sy) {  // TaxiRouteMap units -> screen px
        sx = (mapR.x + x * kS + ui.offsetX()) * ui.scale();
        sy = (mapR.y + (kMapH - yFromBottom) * kS + ui.offsetY()) * ui.scale();
    };
    auto indexOf = [&](uint32_t id) -> int {
        for (size_t i = 0; i < t.ids.size(); ++i)
            if (t.ids[i] == id) return static_cast<int>(i);
        return -1;
    };
    if (t.line.pixels && !t.lines.empty()) {
        ui.pushClip(mapR);  // flushes the sprites staged so far: the lines go over the map
        for (const auto& [a, b] : t.lines) {
            const int ia = indexOf(a), ib = indexOf(b);
            if (ia < 0 || ib < 0) continue;
            float sx, sy, ex, ey;
            mapPx(t.rx[ia], t.ry[ia], sx, sy);
            mapPx(t.rx[ib], t.ry[ib], ex, ey);
            drawRouteLine(t.line, sx, sy, ex, ey, 32.0f * kS * ui.scale());
        }
        ui.popClip();
    }

    // The node buttons (TaxiButtonTemplate 16 x 16, CENTER at TaxiMap BOTTOMLEFT + position).
    uint32_t hovered = 0;
    uint8_t hoveredType = kTypeNone;
    Rect hoveredRect{};
    for (size_t i = 0; i < t.ids.size(); ++i) {
        const uint8_t type = t.type[i];
        if (type == kTypeNone || t.bx[i] < -1e8f) continue;
        const float cx = mapR.x + t.bx[i] * kS, cy = mapR.y + (kMapH - t.by[i]) * kS;
        const Rect br{cx - 16 * kS * 0.5f, cy - 16 * kS * 0.5f, 16 * kS, 16 * kS};
        const Rect hit{cx - 22 * kS * 0.5f, cy - 22 * kS * 0.5f, 22 * kS, 22 * kS};  // PSP: a bigger target
        const uint32_t id = kIdTaxiNode0 + static_cast<uint32_t>(i);
        const bool f = focusable && winFocus_.addStop(id, hit);
        const pp::Texture& ic = t.icon[type == kTypeCurrent ? 0 : type == kTypeReachable ? 1 : 2];
        if (ic.pixels) ui.image(ic, br);
        else ui.fill(br, type == kTypeCurrent ? 0xFF30C030u : type == kTypeReachable ? 0xFFE0E0E0u : 0xFF20D0E0u);
        if (f) {
            if (t.hl.pixels) ui.image(t.hl, Rect{cx - 16 * kS, cy - 16 * kS, 32 * kS, 32 * kS}, 0xFFFFFFFFu, Blend::Add);
            hovered = t.ids[i];
            hoveredType = type;
            hoveredRect = Rect{br.x + ui.offsetX(), br.y + ui.offsetY(), br.w, br.h};
        }
    }
    // TaxiNodeOnButtonEnter (once per entered node): the lines; the tooltip every frame (after the windows).
    if (hovered != 0 && hovered != t.hoverNode) {
        if (hoveredType == kTypeReachable) {  // TaxiNodeSetCurrent + a line per hop
            const auto route = session_.getTaxiRouteTo(hovered);
            t.lines.clear();
            for (size_t k = 0; k + 1 < route.size(); ++k) t.lines.emplace_back(route[k], route[k + 1]);
        } else if (hoveredType == kTypeCurrent) {
            oneHopLines();
        }
    }
    t.hoverNode = hovered;  // OnLeave: GameTooltip:Hide() only
    t.tipNode = hovered;
    t.tipType = hoveredType;
    t.tipAnchor = hoveredRect;
    return true;
}

bool WorldScreen::taxiWindowAction(uint32_t act) {
    if (!taxi_ || !windowOpen(kWinTaxi)) return false;
    TaxiUi& t = *taxi_;
    if (act == kIdTaxiClose) {
        closeWindow(kWinTaxi);  // OnHide -> CloseTaxiMap (updateTaxi)
        return true;
    }
    if (act < kIdTaxiNode0 || act >= kIdTaxiNode0 + kMaxTaxiButtons) return false;
    const size_t i = act - kIdTaxiNode0;
    if (i >= t.ids.size()) return true;
    PSP_LOGI(pp::tag::Ctrl, "taxi_take node=%u type=%u cost=%u", static_cast<unsigned>(t.ids[i]),
             static_cast<unsigned>(t.type[i]), static_cast<unsigned>(session_.getTaxiCostTo(t.ids[i])));
    session_.activateTaxi(t.ids[i]);  // TakeTaxiNode(self:GetID())
    return true;
}

// GameTooltip (ANCHOR_RIGHT of the node button): TaxiNodeName white, then the fare's MoneyFrame (REACHABLE) or
// TAXINODEYOUAREHERE in 0.5, 1, 0.5 (CURRENT).
void WorldScreen::drawTaxiTooltip(UiRenderer& ui) {
    if (!taxi_ || !windowOpen(kWinTaxi) || !uiMode_) return;
    TaxiUi& t = *taxi_;
    if (t.tipNode == 0) return;
    const auto* node = session_.taxiNode(t.tipNode);
    if (!node) return;
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const char* name = session_.taxiNodeName(*node);
    const char* here = CLIENT_STR(TAXINODEYOUAREHERE);
    const uint32_t cost = t.tipType == kTypeReachable ? session_.getTaxiCostTo(t.tipNode) : 0u;
    float w = static_cast<float>(fn.measure(name));
    float h = 8.0f + static_cast<float>(fn.lineHeight());
    if (t.tipType == kTypeCurrent) {
        w = std::max(w, static_cast<float>(fs.measure(here)));
        h += static_cast<float>(fs.lineHeight());
    } else if (t.tipType == kTypeReachable) {
        w = std::max(w, 70.0f);
        h += static_cast<float>(fs.lineHeight()) + 1.0f;
    }
    Rect r{t.tipAnchor.x + t.tipAnchor.w, t.tipAnchor.y - h, w + 14.0f, h};  // ANCHOR_RIGHT: above-right
    if (r.x + r.w > pp::kScreenW - 2) r.x = t.tipAnchor.x - r.w;
    r.x = std::max(2.0f, r.x);
    r.y = std::clamp(r.y, 2.0f, std::max(2.0f, pp::kScreenH - r.h - 2));
    if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{r.x + 3, r.y + 3, r.w - 6, r.h - 6}, 0xF0FFFFFFu);
    else ui.fill(r, 0xF0100000u);
    if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), r);
    float y = r.y + 4;
    ui.text(fn, r.x + 7, y, name, kColWhite);
    y += static_cast<float>(fn.lineHeight());
    if (t.tipType == kTypeCurrent) ui.text(fs, r.x + 7, y, here, 0xFF80FF80u);
    else if (t.tipType == kTypeReachable) drawMoneyFrame(ui, r.x + 7, y, cost, false);  // SetTooltipMoney
}

}  // namespace wowee::ui::psp
