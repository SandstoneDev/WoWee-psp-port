// One fixed order for everything drawn over the world, until task
// A's layer manager (FrameXML strata/levels) replaces it. Hardware builds
// 62-64: "the whole HUD was piled up", "the Debug window was covered by the
// frames" - the unit frames had their own GE pass after the windows.
//
// Order (bottom to top), each its own GE pass:
//   3D world (WorldScreen::draw)
//   "hud"        death veil, NPC names, player name plates, target marker,
//                floating combat text (world-anchored, drawCombatUnder)
//   "frames"     player + target frames: the top row (y 2)
//   "chat"       chat box bottom left (x 4..234, y ..240), its scroll
//                buttons above it
//   "ui.windows" HUD lines (debug numbers under the frames, refused-attack
//                line, AFK, load status), then the window stack - the Debug
//                window last, on top - then the bar (Chat | Menu | Debug)
//                and the cursor
//   "settings"   the graphics menu (its own cursor)
// Input follows the same order: the focus ring hands the cursor to the last
// declared widget (ui_focus.cpp), Circle closes the Debug window first.
// Screen (480x272): HUD - frames top row (y 2..62), debug lines under them
// (y 64..100), refused-attack / AFK / status lines centred (y 104..148),
// chat bottom left (its buttons y 144..164, box ..240), bar bottom right
// (y 228..250); windows - Debug on the right (x 164..476, y 4..224), Menu on
// the left (x 6..156, y 18..142), the death window centred. HUD items do not
// overlap each other; windows cover the HUD, never the bar, and Debug and
// Menu never cover each other.
#include "ui/psp/ui_world_screen.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_prof.hpp"
#include "ui/psp/ui_login_screen.hpp"  // FrameStats

#include <algorithm>
#include <cstdio>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

constexpr uint32_t abgr(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (a << 24) | (b << 16) | (g << 8) | r; }

// HUD lines under the frames (frames: y 2..62).
constexpr float kHudLinesY = 64.0f;
// kErrorY (104) is the top of the UIErrorsFrame stack now (ui_world_errors.cpp kFrameTop).
constexpr float kAfkY = 120.0f;
constexpr float kStatusY = 136.0f;

} // namespace

// upstream GameScreen nameplates (game_screen_hud.cpp, "Name color
// communicates reaction too"): players - hostile red, else blue; NPCs -
// hostile red, unfriendly (reaction 3) orange, else yellow.
// wowee.cfg namecolors=1 (PSP): the original client's reaction colours - hostile
// red, unfriendly orange, neutral yellow, friendly green.
uint32_t WorldScreen::reactionColor(uint64_t guid) const {
    auto e = session_.entities().getEntity(guid);
    const bool hostile = session_.isHostileUnit(guid);
    if (e && e->getType() == game::ObjectType::PLAYER)
        return hostile ? abgr(220, 80, 80, 230) : abgr(102, 153, 255, 230);
    const int reaction = session_.unitReactionToPlayer(guid);
    if (nameColors_ == 1) {
        if (hostile || reaction <= 2) return abgr(255, 64, 64, 255);
        if (reaction == 3) return abgr(255, 128, 0, 255);
        if (reaction == 4) return abgr(255, 255, 0, 255);
        return abgr(64, 255, 64, 255);
    }
    if (!hostile && reaction == 3) return abgr(240, 130, 60, 230);
    return hostile ? abgr(220, 80, 80, 230) : abgr(240, 200, 100, 230);
}

// Name plates of other players (upstream shows names above units); moved
// here from WorldScreen::draw so the combat text is drawn over them.
void WorldScreen::drawPlayerPlates(UiRenderer& ui) {
    for (const auto& [guid, r] : remotes_) {
        if (!r.visible) continue;
        const float h = (r.model ? r.model->height() : 2.0f) + (r.mountReady ? r.riderHeight : 0.0f);  // mounts
        const glm::vec4 clip = viewProj_ * glm::vec4(r.pos + glm::vec3(0, 0, h + 0.4f) - camPosForHud_, 1.0f);
        if (clip.w < 0.5f) continue;
        const float sx = (clip.x / clip.w * 0.5f + 0.5f) * pp::kScreenW;
        const float sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * pp::kScreenH;
        if (sx < 0 || sx > pp::kScreenW || sy < 0 || sy > pp::kScreenH) continue;
        const std::string* name = session_.nameOf(guid);
        const uint32_t col = guid == session_.target() ? kColWhite : factionsOk_ ? reactionColor(guid) : kColGold;
        ui.textShadow(kit_.font(kFontSmall), sx, sy - 10, name ? name->c_str() : "?", col, Align::Center);
    }
}

// PSP: the settings row "FPS and ping" (perfhud) - two short lines left of the minimap, under the
// screen's top edge. Ping: the original's MainMenuBarPerformanceBar colours (PERFORMANCEBAR_LOW_LATENCY 300,
// _MEDIUM_LATENCY 600: green / yellow / red); fps: green from 25, yellow from 15, red below.
void WorldScreen::drawPerfHud(UiRenderer& ui, const FrameStats& stats) {
    if (!perfHud_) return;
    const Font& fs = kit_.font(kFontSmall);
    constexpr float kRight = 372.0f;  // the minimap starts at x 376
    const uint32_t fpsCol = stats.fps >= 25.0f ? kColGreen : stats.fps >= 15.0f ? kColGold : kColRed;
    std::string line = OWN_FMT(HUD_PERF_FMT, static_cast<double>(stats.fps));
    ui.textShadow(fs, kRight, 4, line.c_str(), fpsCol, Align::Right);
    const uint32_t ms = session_.latencyMs();
    const uint32_t pingCol = ms == 0 ? kColGrey : ms < 300u ? kColGreen : ms < 600u ? kColGold : kColRed;
    line = ms ? OWN_FMT(HUD_PING_FMT, static_cast<unsigned>(ms)) : std::string(OWN_STR(HUD_PING_NONE));
    ui.textShadow(fs, kRight, 16, line.c_str(), pingCol, Align::Right);
}

// The debug numbers (wowee.cfg hudlines=0 hides them), the refused-attack
// line (upstream UIErrorsFrame), AFK and the load status - under the frames.
void WorldScreen::drawHudLines(UiRenderer& ui, const FrameStats& stats) {
    const auto& st = terrain_.stats();
    const auto& ms = models_.stats();
    if (hudLines_) {  // own strings (ps_b7_world_hud.inc HUD_*)
        std::string line = OWN_FMT(HUD_POS_FMT, name_, static_cast<unsigned>(session_.worldEntry().mapId), playerX_,
                                   playerY_, playerZ_);
        ui.textShadow(kit_.font(kFontSmall), 4, kHudLinesY, line.c_str(), kColWhite);
        line = OWN_FMT(HUD_STATS_FMT, stats.build, stats.fps, static_cast<unsigned>(stats.geWaitUs),
                       static_cast<unsigned>(st.tiles), static_cast<unsigned>(st.chunksDrawn),
                       static_cast<unsigned>(st.chunksDrawn + st.chunksCulled), static_cast<unsigned>(st.triangles));
        ui.textShadow(kit_.font(kFontSmall), 4, kHudLinesY + 12, line.c_str(), kColGrey);
        if (modelsOn_) {
            line = OWN_FMT(HUD_MODELS_FMT, static_cast<unsigned>(ms.drawn), static_cast<unsigned>(ms.instances),
                           static_cast<unsigned>(ms.triangles), static_cast<unsigned>(ms.batches),
                           static_cast<unsigned>(modelsCpuUs_), models_.viewDistance);
        } else {
            line = OWN_STR(HUD_MODELS_OFF);
        }
        ui.textShadow(kit_.font(kFontSmall), 4, kHudLinesY + 24, line.c_str(), kColGrey);
    }
    // Refused attacks (upstream raiseUiError -> UIErrorsFrame, top centre;
    // PSP: under the frames and the debug lines).
    // the UIErrorsFrame stack (ui_world_errors.cpp: up to 3 rows from
    // y 104, red errors and yellow info, 5 s + a 3 s fade) instead of the one red line; over the trainer /
    // talent frames it is drawn after the windows. The AFK and load status lines move under the stack.
    uiErrorsTick();
    if (!uiErrorsOverWindows()) drawUiErrors(ui);
    const float afkY = std::max(kAfkY, uiErrorsBottom());
    if (afk_) ui.textShadow(kit_.font(kFontNormal), pp::kScreenW * 0.5f, afkY, OWN_STR(HUD_AFK), kColGrey, Align::Center);
    if (!loadStatus_.empty())
        ui.textShadow(kit_.font(kFontNormal), pp::kScreenW * 0.5f, std::max(kStatusY, afkY + (afk_ ? 16.0f : 0.0f)),
                      loadStatus_.c_str(), kColRed, Align::Center);
}

void WorldScreen::drawHudLayers(UiRenderer& ui, const FrameStats& stats) {
    PROF_SCOPE("hud");
    // Under the full-screen world map the HUD is hidden - the original hides UIParent for a
    // full-screen panel (BlackoutWorld covered it; now the dimmed world shows at the sides, ui_world_map.cpp).
    const bool hud = !fullMapOpen();
    if (hud) {
        PROF_GE_PASS("hud");
        ui.beginFrame();
        drawCombatUnder(ui);  // veil, NPC names, player plates, target marker, combat text
        drawLootHud(ui);       // loot - sparkle over lootable corpses
        drawQuestMarkers(ui);  // npc - ! / ? over questgivers
        drawGameObjectHud(ui);  // the ↑ glyph over the hovered object, quest sparkles
        // XP bar + MainMenuBar art (under the frames, tracker, chat); hidden under
        // VehicleMenuBar (MainMenuBar_ToVehicleArt)
        if (!session_.vehicleUi()) drawMainMenuBarArt(ui);
        ui.endFrame();
    }
    if (hud) {
        drawMinimap(ui);  // map - own GE pass "minimap", top right (x 376..480, y 2..124)
        drawUnitFrames(ui);  // own pass "frames" (skipped under the settings menu)
    }
    if (hud) {
        PROF_GE_PASS("watch");
        ui.beginFrame();
        drawWatchFrame(ui);  // WatchFrame under the minimap
        ui.endFrame();
    }
    if (hud) drawFrameXml(ui);  // framexml (own GE pass "ui.frames"; framexml=1 hides the frames above)
    if (hud) {
        PROF_GE_PASS("spells");
        ui.beginFrame();
        drawSpells(ui);  // spells: auras, cast bars, action bar (ui_world_spells.cpp), under chat and windows
        ui.endFrame();
    }
    if (hud) {
        PROF_GE_PASS("chat");
        ui.beginFrame();
        // UIParent_ManageFramePositions' ChatFrame1 "pet = 35" - the chat box goes up with the pet bar.
        const float lift = std::max(petChatLift(), classBarChatLift());  // the class bars too
        if (lift > 0.0f) {
            ui.setOffset(0.0f, -lift);
            winFocus_.setOffset(0.0f, -lift);
        }
        chat_.draw(ui, winFocus_, uiMode_ && !settings_.isOpen());
        if (lift > 0.0f) {
            ui.setOffset(0.0f, 0.0f);
            winFocus_.setOffset(0.0f, 0.0f);
        }
        ui.endFrame();
    }
    if (hud) drawGameObjectTip(ui);  // the hovered object's GameTooltip (own pass "gouse.tip")
    {
        PROF_GE_PASS("ui.windows");
        ui.beginFrame();
        drawHudLines(ui, stats);
        if (hud) drawPerfHud(ui, stats);  // perfhud: fps + ping
        if (hud) drawTutorialAlert(ui);  // tutorials (alert button, under the windows)
        drawWindows(ui);  // stack (Debug on top), bar, cursor
        ui.endFrame();
    }
    if (settings_.isOpen()) {
        PROF_GE_PASS("settings");
        PROF_SCOPE("settings.draw");
        ui.beginFrame();
        settings_.draw(ui, kit_, stats);
        ui.endFrame();
    }
}

} // namespace wowee::ui::psp
