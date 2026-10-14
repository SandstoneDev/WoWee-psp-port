// Death -> ghost -> corpse run -> resurrection on the world screen.
//
// The original client (FrameXML UIParent.lua + StaticPopup.lua, ruRU GlobalStrings.lua):
//   - PLAYER_DEAD -> StaticPopup "DEATH": no title, DEATH_RELEASE_TIMER
//     counting GetReleaseTimeRemaining down (6 min, then the server
//     releases by itself; seconds below a minute), DEATH_RELEASE_NOTIMER when the time is
//     unknown (logged in dead), button DEATH_RELEASE -> RepopMe
//     (CMSG_REPOP_REQUEST + MSG_CORPSE_QUERY, upstream releaseSpirit). Not hideOnEscape.
//   - The server makes us a ghost and teleports us to the graveyard (MSG_MOVE_TELEPORT_ACK,
//     acknowledged by the session; applyTeleport puts the player there). The corpse is
//     marked on the minimap (upstream renderMinimapCorpseMarker: an X in a ring, an arrow
//     at the rim when it is off the disc) and on the world map (CorpseMarkerLayer: the
//     corpse X and the spirit healer's cross).
//   - CORPSE_IN_RANGE (ghost within 39 yd of the corpse; upstream announces the edge from
//     canReclaimCorpse) -> "RECOVER_CORPSE": RECOVER_CORPSE, while the
//     reclaim delay runs RECOVER_CORPSE_TIMER with the button off;
//     button ACCEPT -> RetrieveCorpse (CMSG_RECLAIM_CORPSE). Stays up after the
//     click (OnAccept returns 1) until CORPSE_OUT_OF_RANGE / alive.
//   - SMSG_RESURRECT_REQUEST -> ShowResurrectRequest: "RESURRECT" (sickness: RESURRECT_REQUEST),
//     "RESURRECT_NO_SICKNESS" (a timer: RESURRECT_REQUEST_NO_SICKNESS) or "RESURRECT_NO_TIMER";
//     StartDelay = GetCorpseRecoveryDelay (delay texts with the countdown, Accept off),
//     60 s after it the offer is declined; ACCEPT -> AcceptResurrect (CMSG_RESURRECT_RESPONSE 1),
//     DECLINE -> DeclineResurrect (0) and DEATH again while dead. It cancels DEATH.
//   - The spirit healer: its gossip option -> SMSG_SPIRIT_HEALER_CONFIRM -> CONFIRM_XP_LOSS ->
//     "XP_LOSS" (level 11+: sickness for GetResSicknessDuration, a second confirmation
//     CONFIRM_XP_LOSS_AGAIN) or "XP_LOSS_NO_SICKNESS_NO_DURABILITY" (level 10 and below);
//     ACCEPT -> AcceptXPLoss (CMSG_SPIRIT_HEALER_ACTIVATE), CANCEL / walking away
//     (CheckSpiritHealerDist) -> CloseGossip. The gossip window goes (HideUIPanel).
// PSP: each dialog is one small function on kit_.panel / kit_.button (drawDeathPopup,
// drawCorpsePopup, drawResurrectPopup, drawXpLossPopup) until the shared StaticPopup component
// takes them; a popup that needs an answer raises the cursor and takes the focus (Cross =
// click, START = Esc: only RESURRECT and XP_LOSS are hideOnEscape - START on them = Cancel).
// The ghost is drawn at half opacity (upstream setInstanceOpacity 0.5) under the pale veil.
#include "ui/psp/ui_teleport_hold.hpp"  // the landing hold
#include "ui/psp/ui_world_map.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"  // tphold
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_texture.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
using Life = pp::WorldSession::Life;

namespace {

// Focus ids next to the combat window's kIdRelease (0x5B01).
enum : uint32_t {
    kIdDeathRelease = 0x5B01,  // was kIdRelease (ui_world_combat.cpp)
    kIdCorpseAccept = 0x5B02,
    kIdResAccept = 0x5B03,
    kIdResDecline = 0x5B04,
    kIdXpAccept = 0x5B05,
    kIdXpCancel = 0x5B06,
};

constexpr float kReleaseSec = 360.0f;     // the client's release timer (the server repops at 6 min)
constexpr float kOfferTimeout = 60.0f;    // RESURRECT timeout
constexpr float kHealerDist = 15.0f;      // PSP: CheckSpiritHealerDist ~ NPC_INTERACT_MAX_DISTANCE
constexpr float kPopupW = 280.0f, kPopupTextW = 256.0f;

// GlobalStrings.lua through strings.pak: DEATH_*, RECOVER_CORPSE*, RESURRECT_REQUEST*,
// CONFIRM_XP_LOSS*, ACCEPT / DECLINE / CANCEL; the time units SECONDS / MINUTES ("|4..;" plural markup in enUS,
// resolved after the form by text::plural, psp_text_format.hpp).

// StaticPopup_OnUpdate: ceil(timeleft) seconds below a minute, else ceil(timeleft / 60) minutes.
void timeArgs(float sec, int& n, const char*& unit) {
    const int t = static_cast<int>(std::ceil(std::max(0.0f, sec)));
    if (t < 60) {
        n = t;
        unit = CLIENT_STR(SECONDS);
    } else {
        n = (t + 59) / 60;
        unit = CLIENT_STR(MINUTES);
    }
}

constexpr uint32_t abgr(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (a << 24) | (b << 16) | (g << 8) | r; }

void geLine(float x0, float y0, float x1, float y1, float thick, uint32_t col) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float nx = -dy / len * thick * 0.5f, ny = dx / len * thick * 0.5f;
    const MapGeVert v[4] = {{0, 0, col, x0 + nx, y0 + ny, 0}, {0, 0, col, x1 + nx, y1 + ny, 0},
                            {0, 0, col, x1 - nx, y1 - ny, 0}, {0, 0, col, x0 - nx, y0 - ny, 0}};
    mapGeFan(v, 4);
}

void geDisc(float cx, float cy, float r, uint32_t col) {
    constexpr int kSeg = 12;
    MapGeVert v[kSeg + 2];
    v[0] = {0, 0, col, cx, cy, 0};
    for (int k = 0; k <= kSeg; ++k) {
        const float a = k * 6.28318530718f / kSeg;
        v[k + 1] = {0, 0, col, cx + std::cos(a) * r, cy + std::sin(a) * r, 0};
    }
    mapGeFan(v, kSeg + 2);
}

}  // namespace

// --- Map marks (raw GE; ui_world_map.hpp) -------------------------------------------------

// upstream renderMinimapCorpseMarker: a dark disc, a pale ring, an X.
void mapDrawCorpseMark(float cx, float cy) {
    pp::textureBind(nullptr);
    constexpr float r = 5.0f;
    geDisc(cx, cy, r + 1.0f, abgr(0, 0, 0, 140));
    constexpr int kSeg = 12;
    for (int k = 0; k < kSeg; ++k) {
        const float a0 = k * 6.28318530718f / kSeg, a1 = (k + 1) * 6.28318530718f / kSeg;
        geLine(cx + std::cos(a0) * (r + 1.0f), cy + std::sin(a0) * (r + 1.0f), cx + std::cos(a1) * (r + 1.0f),
               cy + std::sin(a1) * (r + 1.0f), 1.5f, abgr(200, 200, 220, 220));
    }
    geLine(cx - 3.0f, cy - 3.0f, cx + 3.0f, cy + 3.0f, 1.5f, abgr(180, 180, 220, 255));
    geLine(cx + 3.0f, cy - 3.0f, cx - 3.0f, cy + 3.0f, 1.5f, abgr(180, 180, 220, 255));
}

// upstream renderMinimapCorpseMarker, off the disc: a triangle at (ax, ay) pointing (nx, ny).
void mapDrawCorpsePointer(float ax, float ay, float nx, float ny) {
    pp::textureBind(nullptr);
    constexpr float kLen = 6.0f, kW = 3.5f;
    const uint32_t col = abgr(180, 180, 240, 230);
    const MapGeVert v[3] = {{0, 0, col, ax + nx * kLen, ay + ny * kLen, 0},
                            {0, 0, col, ax - ny * kW - nx * kLen * 0.4f, ay + nx * kW - ny * kLen * 0.4f, 0},
                            {0, 0, col, ax + ny * kW - nx * kLen * 0.4f, ay - nx * kW - ny * kLen * 0.4f, 0}};
    mapGeFan(v, 3);
}

// upstream CorpseMarkerLayer: the corpse (the fallback bone-white X: no corpse arrow art on
// the PSP) and the spirit healer (a pale cross over a dark one).
void mapDrawWorldCorpse(float cx, float cy) {
    pp::textureBind(nullptr);
    constexpr float R = 5.0f, T = 1.8f;
    geLine(cx - R, cy - R, cx + R, cy + R, T + 1.5f, abgr(0, 0, 0, 220));
    geLine(cx + R, cy - R, cx - R, cy + R, T + 1.5f, abgr(0, 0, 0, 220));
    geLine(cx - R, cy - R, cx + R, cy + R, T, abgr(230, 220, 200, 240));
    geLine(cx + R, cy - R, cx - R, cy + R, T, abgr(230, 220, 200, 240));
}

void mapDrawHealerCross(float gx, float gy) {
    pp::textureBind(nullptr);
    constexpr float H = 7.0f, W = 4.5f, T = 2.6f;
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t col = pass == 0 ? abgr(0, 0, 0, 200) : abgr(180, 215, 255, 245);
        const float th = pass == 0 ? T + 1.6f : T;
        geLine(gx, gy - H, gx, gy + H, th, col);
        geLine(gx - W, gy - H * 0.25f, gx + W, gy - H * 0.25f, th, col);
    }
}

// --- Popups: open / close ---------------------------------------------------------------------

void WorldScreen::showDeathPopup(uint8_t w, uint32_t focusId) {
    if (!windowOpen(w)) {
        openWindow(w);
        death_.pressSeen = false;
    }
    if (!uiMode_) {
        setUiMode(true);
        death_.raisedUi = true;
    }
    suppress_ = ~0u;  // whatever is held now stays dead until released (masked by held next frame)
    winFocus_.setFocus(focusId);
}

void WorldScreen::closeDeathPopup(uint8_t w) {
    if (!windowOpen(w)) return;
    closeWindow(w);
    if (!uiMode_) death_.raisedUi = false;
    if (death_.raisedUi && windows_.empty() && !settings_.isOpen()) {
        setUiMode(false);
        death_.raisedUi = false;
        suppress_ = ~0u;  // the Cross that closed it must not walk
    }
}

// StaticPopup: DEATH and RECOVER_CORPSE have no hideOnEscape - Esc does not close them.
// nor DUEL_OUTOFBOUNDS (timeout 10, no buttons) - START passes it by to the popup under it and Circle on
// its title does nothing (closeTitleUnderCursor asks this too); was: START closed it, updateTradeDuel opened it again
// the next frame, so for 10 s START reached no window under it.
bool WorldScreen::deathPopupKeepsOnEscape(uint8_t w) const {
    return w == kWinRelease || w == kWinRecoverCorpse || w == kWinDuelBounds;
}

// --- Teleport -----------------------------------------------------------------------------------

// upstream handleTeleportAck's half that moves the player: the position and facing taken,
// the movement flags cleared, MSG_MOVE_STOP + MSG_MOVE_HEARTBEAT from the new place (the
// session already acknowledged). Runs before updateMovement, so no heartbeat leaves from the
// old place after the acknowledgement. The tiles stream in around the new place.
void WorldScreen::applyTeleport(const glm::vec3& to, float orientation) {
    const float dx = to.x - playerX_, dy = to.y - playerY_;
    // world_flush: a teleport out of the stream window - the old place's world goes at once
    // (tpflush=1; on the emulator 16 teleports filled the world arena, the last place drew no houses / trees).
    const bool flush = pp::configGetInt("tpflush", 1) != 0 &&
                       teleportLeavesWindow(playerX_, playerY_, to.x, to.y,
                                            std::clamp(pp::configGetInt("streamradius", 1), 0, 1));
    playerX_ = to.x;
    playerY_ = to.y;
    playerZ_ = to.z;
    shownZ_ = to.z;
    facing_ = orientation;
    camYaw_ = orientation;
    sentFacing_ = orientation;
    airborne_ = false;
    vz_ = 0.0f;
    moving_ = false;
    moveFlags_ = 0;
    camInit_ = false;  // the camera snaps behind the player instead of flying there
    sendMove(game::Opcode::MSG_MOVE_STOP, false);
    sendMove(game::Opcode::MSG_MOVE_HEARTBEAT, false);
    nextHeartbeatMs_ = pp::logUptimeMs() + 500u;
    PSP_LOGI(pp::tag::Gfx, "teleport_applied x=%.1f y=%.1f z=%.1f dist=%.0f life=%u", to.x, to.y, to.z,
             std::sqrt(dx * dx + dy * dy), static_cast<unsigned>(session_.life()));
    if (flush) worldFlush("teleport", std::sqrt(dx * dx + dy * dy));
    stream_.markTeleport();  // stream_ready once the new window is in
    startTeleportHold("teleport");  // no fall before the floor under the new place is loaded
    pp::logFlush();
}

// ---- the landing hold (a GM teleport into a capital city used to land under its streets) ----

void WorldScreen::startTeleportHold(const char* what) {
    tpHold_.holdMs = static_cast<uint32_t>(std::clamp(pp::configGetInt("tphold", 8000), 0, 60000));
    if (!tpHold_.holdMs) return;
    tpHold_.active = true;
    tpHold_.startMs = pp::logUptimeMs();
    tpHold_.what = what;
}

// Per frame from updateMovement while active: the position stays where the server put it (no gravity, no fall, the
// input ignored, no heartbeat - MSG_MOVE_STOP + HEARTBEAT went with the teleport) until the floor is there.
bool WorldScreen::teleportHold() {
    if (!tpHold_.active) return false;
    const uint32_t now = pp::logUptimeMs();
    TeleportHoldState st;
    float tz = 0.0f;
    st.terrainLoaded = wmoOnly_ || terrain_.heightAt(playerX_, playerY_, tz);
    st.doodadsLoaded = wmoOnly_ || !stream_.active() || stream_.doodadsInAt(playerX_, playerY_);
    uint32_t colResident = 0, colWanted = 0;
    bigWmoLanding(playerX_, playerY_, playerZ_, st.insideBigWmo, st.wmoReady, colResident, colWanted);
    const float floor = floorAt(playerX_, playerY_, playerZ_ + 0.6f);
    st.floorNear = floor > -1e29f && floor >= playerZ_ - 2.0f;
    const TeleportHoldWhy why = teleportHoldDecide(st, now - tpHold_.startMs, tpHold_.holdMs);
    if (why == TeleportHoldWhy::kHold) {
        airborne_ = false;
        vz_ = 0.0f;
        moving_ = false;
        moveFlags_ = 0;
        wantMove_ = false;
        jumpReq_ = false;
        return true;
    }
    tpHold_.active = false;
    if (st.floorNear) {  // stand on it (the floor, a big WMO's when it is there - floorAt takes the highest)
        playerZ_ = floor;
        shownZ_ = floor;
    }
    if (++tpHold_.holds <= 60)
        PSP_LOGI(pp::tag::Gfx, "teleport_hold at=%s ms=%u why=%s terrain=%d doodads=%d wmo=%d col=%u/%u floor=%.1f z=%.1f",
                 tpHold_.what, static_cast<unsigned>(now - tpHold_.startMs),
                 why == TeleportHoldWhy::kFloor ? "floor" : "timeout", st.terrainLoaded ? 1 : 0, st.doodadsLoaded ? 1 : 0,
                 st.insideBigWmo ? (st.wmoReady ? 1 : 0) : -1, static_cast<unsigned>(colResident),
                 static_cast<unsigned>(colWanted), floor > -1e29f ? floor : 0.0f, playerZ_);
    return false;
}

// --- Per frame (from updateCombat, before the movement) --------------------------------------------

void WorldScreen::updateDeath(const pp::PadState& pad, float dtSec) {
    (void)pad;
    glm::vec3 to;
    float o = 0.0f;
    if (session_.takeTeleport(to, o)) applyTeleport(to, o);
    const glm::vec3 self(playerX_, playerY_, playerZ_);
    const Life life = session_.life();
    if (life == Life::Alive && session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH)) > 0)
        death_.sawAlive = true;
    death_.nextY = 30.0f;  // StaticPopup1's place (TOP of UIParent, -135 of 768), the rest below it

    // Life edges: PLAYER_DEAD / PLAYER_ALIVE / PLAYER_UNGHOST (UIParent_OnEvent).
    if (static_cast<uint8_t>(life) != lastLife_) {
        const uint8_t from = lastLife_;
        lastLife_ = static_cast<uint8_t>(life);
        if (life == Life::Dead) {
            PSP_LOGI(pp::tag::Gfx, "player_dead x=%.1f y=%.1f z=%.1f", playerX_, playerY_, playerZ_);
            session_.stopAutoAttack();
            meleeSwingTimer_ = 0.0f;
            sitting_ = false;
            afk_ = false;
            repopSent_ = false;
            session_.markPlayerDead(self);  // upstream markPlayerDead: the corpse is where we fell
            death_.releaseLeft = death_.sawAlive ? kReleaseSec : -1.0f;
            death_.reshowSec = 0.0f;
            showDeathPopup(kWinRelease, kIdDeathRelease);
        } else {
            closeDeathPopup(kWinRelease);
            death_.reshowSec = 0.0f;
            if (life == Life::Ghost) {
                PSP_LOGI(pp::tag::Gfx, "player_ghost repop=%d corpse=%d", repopSent_ ? 1 : 0, session_.corpseKnown() ? 1 : 0);
            } else {
                PSP_LOGI(pp::tag::Gfx, "player_alive from=%u", static_cast<unsigned>(from));
                lastAnimRequest_ = UINT32_MAX;  // the FSM takes the model back
                if (player_.animated()) player_.playAnimation(rendering::anim::STAND, true);
                // PLAYER_ALIVE / PLAYER_UNGHOST: every death popup goes.
                closeDeathPopup(kWinRecoverCorpse);
                closeDeathPopup(kWinResurrect);
                closeDeathPopup(kWinXpLoss);
                death_.shownRes = death_.shownXp = false;
                death_.corpseInRange = false;
                session_.forgetResurrectOffer();
            }
        }
        pp::logFlush();
    }
    if (life == Life::Dead) {
        idleSec_ = 0.0f;  // no AFK sit on a corpse
        if (death_.releaseLeft > 0.0f) death_.releaseLeft = std::max(0.0f, death_.releaseLeft - dtSec);
        // PSP: dead with no popup up - a release or an accepted resurrection that nothing
        // answered (a lost packet, a caster gone): DEATH again after 5 s.
        if (!windowOpen(kWinRelease) && !windowOpen(kWinResurrect)) {
            death_.reshowSec += dtSec;
            if (death_.reshowSec > 5.0f) {
                death_.reshowSec = 0.0f;
                repopSent_ = false;
                PSP_LOGW(pp::tag::Net, "death_unanswered: DEATH again");
                showDeathPopup(kWinRelease, kIdDeathRelease);
            }
        }
    }

    // CORPSE_IN_RANGE / CORPSE_OUT_OF_RANGE (upstream: the edge of canReclaimCorpse).
    const bool inRange = life == Life::Ghost && session_.canReclaimCorpse(self);
    if (inRange != death_.corpseInRange) {
        death_.corpseInRange = inRange;
        PSP_LOGI(pp::tag::Net, "corpse_in_range=%d delay=%.0f", inRange ? 1 : 0, session_.corpseReclaimDelaySec());
        if (inRange) showDeathPopup(kWinRecoverCorpse, kIdCorpseAccept);
        else closeDeathPopup(kWinRecoverCorpse);
    }

    // RESURRECT_REQUEST / CONFIRM_XP_LOSS.
    const auto& offer = session_.resurrectOffer();
    if (offer.pending && offer.seq != death_.offerSeq) {
        death_.offerSeq = offer.seq;
        if (offer.spiritHealer) {
            death_.healer = offer.caster;
            death_.xpAgain = false;
            session_.closeGossip();  // HideUIPanel(GossipFrame)
            death_.shownXp = true;
            showDeathPopup(kWinXpLoss, kIdXpAccept);
        } else {
            closeDeathPopup(kWinRelease);  // "cancels = DEATH"
            death_.offerDelay = offer.hasTimer ? session_.corpseReclaimDelaySec() : 0.0f;
            death_.offerLeft = death_.offerDelay + kOfferTimeout;
            death_.shownRes = true;
            showDeathPopup(kWinResurrect, kIdResAccept);
        }
    }
    if (death_.shownRes) {
        bool cancel = false;
        if (!windowOpen(kWinResurrect)) {
            cancel = true;  // START (hideOnEscape) = OnCancel
        } else {
            death_.offerDelay = std::max(0.0f, death_.offerDelay - dtSec);
            death_.offerLeft -= dtSec;
            if (death_.offerLeft <= 0.0f) cancel = true;  // timeout -> OnCancel
        }
        if (!offer.pending || offer.spiritHealer) {
            death_.shownRes = false;
            closeDeathPopup(kWinResurrect);
        } else if (cancel) {
            death_.shownRes = false;
            closeDeathPopup(kWinResurrect);
            session_.declineResurrect();
            if (life == Life::Dead) showDeathPopup(kWinRelease, kIdDeathRelease);
        }
    }
    if (death_.shownXp) {
        bool cancel = !windowOpen(kWinXpLoss);  // START (hideOnEscape) = OnCancel -> CloseGossip
        glm::vec3 hp;
        float hh = 0.0f;
        if (!cancel && unitPosition(death_.healer, hp, hh)) {  // CheckSpiritHealerDist
            const glm::vec3 d = hp - self;
            if (glm::dot(d, d) > kHealerDist * kHealerDist) cancel = true;
        }
        if (!offer.pending || !offer.spiritHealer) {
            death_.shownXp = false;
            closeDeathPopup(kWinXpLoss);
        } else if (cancel) {
            death_.shownXp = false;
            closeDeathPopup(kWinXpLoss);
            session_.forgetResurrectOffer();
            session_.closeGossip();
        }
    }
}

// --- Drawing ---------------------------------------------------------------------------------

// The StaticPopup box: centred, the text wrapped and centred, room for one row of buttons.
Rect WorldScreen::deathPopupBox(UiRenderer& ui, const char* text, float& buttonY) {
    const Font& f = kit_.font(kFontNormal);
    const std::vector<std::string> lines = wrapQuestText(f, text, kPopupTextW);
    const float lh = static_cast<float>(f.lineHeight());
    const float h = 12.0f + lines.size() * lh + 8.0f + 22.0f + 10.0f;
    const Rect r{(pp::kScreenW - kPopupW) * 0.5f, death_.nextY, kPopupW, h};
    death_.nextY += h + 4.0f;
    kit_.panel(ui, r);
    float y = r.y + 12.0f;
    for (const auto& l : lines) {
        ui.textShadow(f, r.x + r.w * 0.5f, y, l.c_str(), kColWhite, Align::Center);
        y += lh;
    }
    buttonY = y + 8.0f;
    return r;
}

bool WorldScreen::drawDeathWindow(UiRenderer& ui, uint8_t w) {
    switch (w) {
        case kWinRelease: drawDeathPopup(ui); break;
        case kWinRecoverCorpse: drawCorpsePopup(ui); break;
        case kWinResurrect: drawResurrectPopup(ui); break;
        case kWinXpLoss: drawXpLossPopup(ui); break;
        default: return false;
    }
    for (uint32_t id : {kIdDeathRelease, kIdCorpseAccept, kIdResAccept, kIdResDecline, kIdXpAccept, kIdXpCancel})
        if (winFocus_.isPressed(id)) death_.pressSeen = true;
    return true;
}

// StaticPopupDialogs["DEATH"]: no title, the release countdown, DEATH_RELEASE.
void WorldScreen::drawDeathPopup(UiRenderer& ui) {
    std::string text;
    if (death_.releaseLeft < 0.0f) {
        text = CLIENT_STR(DEATH_RELEASE_NOTIMER);
    } else {
        int n = 0;
        const char* unit = "";
        timeArgs(death_.releaseLeft, n, unit);
        text = pp::text::plural(CLIENT_FMT(DEATH_RELEASE_TIMER, n, unit));
    }
    float by = 0.0f;
    const Rect r = deathPopupBox(ui, text.c_str(), by);
    kit_.button(ui, winFocus_, kIdDeathRelease, Rect{r.x + (r.w - 128.0f) * 0.5f, by, 128, 22},
                CLIENT_STR(DEATH_RELEASE));
}

// StaticPopupDialogs["RECOVER_CORPSE"]: RECOVER_CORPSE / the reclaim delay, ACCEPT.
void WorldScreen::drawCorpsePopup(UiRenderer& ui) {
    std::string text;
    const float delay = session_.corpseReclaimDelaySec();
    if (delay > 0.0f) {
        int n = 0;
        const char* unit = "";
        timeArgs(delay, n, unit);
        text = pp::text::plural(CLIENT_FMT(RECOVER_CORPSE_TIMER, n, unit));
    } else {
        text = CLIENT_STR(RECOVER_CORPSE);
    }
    float by = 0.0f;
    const Rect r = deathPopupBox(ui, text.c_str(), by);
    kit_.button(ui, winFocus_, kIdCorpseAccept, Rect{r.x + (r.w - 128.0f) * 0.5f, by, 128, 22}, CLIENT_STR(ACCEPT),
                delay <= 0.0f);
}

// StaticPopupDialogs["RESURRECT" / "RESURRECT_NO_SICKNESS" / "RESURRECT_NO_TIMER"].
void WorldScreen::drawResurrectPopup(UiRenderer& ui) {
    const auto& offer = session_.resurrectOffer();
    const char* name = offer.name.empty() ? "?" : offer.name.c_str();
    std::string text;
    const bool delayed = offer.hasTimer && death_.offerDelay > 0.0f;
    if (delayed) {
        int n = 0;
        const char* unit = "";
        timeArgs(death_.offerDelay, n, unit);
        text = pp::text::plural(offer.sickness ? CLIENT_FMT(RESURRECT_REQUEST_TIMER, name, n, unit)
                                           : CLIENT_FMT(RESURRECT_REQUEST_NO_SICKNESS_TIMER, name, n, unit));
    } else {
        text = offer.sickness ? CLIENT_FMT(RESURRECT_REQUEST, name) : CLIENT_FMT(RESURRECT_REQUEST_NO_SICKNESS, name);
    }
    float by = 0.0f;
    const Rect r = deathPopupBox(ui, text.c_str(), by);
    const float bx = r.x + (r.w - 2 * 120.0f - 8.0f) * 0.5f;
    kit_.button(ui, winFocus_, kIdResAccept, Rect{bx, by, 120, 22}, CLIENT_STR(ACCEPT), !delayed);
    kit_.button(ui, winFocus_, kIdResDecline, Rect{bx + 128.0f, by, 120, 22}, CLIENT_STR(DECLINE));
}

// StaticPopupDialogs["XP_LOSS" / "XP_LOSS_NO_SICKNESS_NO_DURABILITY"] (UIParent CONFIRM_XP_LOSS:
// GetResSicknessDuration - AzerothCore's resurrection sickness: none up to level 10, a minute per
// level above 10, 10 minutes from level 20).
void WorldScreen::drawXpLossPopup(UiRenderer& ui) {
    const uint32_t level = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
    std::string text;
    if (level <= 10) {
        text = CLIENT_STR(CONFIRM_XP_LOSS_NO_SICKNESS_NO_DURABILITY);
    } else {
        const std::string dur = pp::text::plural(CLIENT_FMT(MINUTES_ABBR, static_cast<int>(level < 20 ? level - 10 : 10)));
        text = death_.xpAgain ? CLIENT_FMT(CONFIRM_XP_LOSS_AGAIN, dur) : CLIENT_FMT(CONFIRM_XP_LOSS, dur);
    }
    float by = 0.0f;
    const Rect r = deathPopupBox(ui, text.c_str(), by);
    const float bx = r.x + (r.w - 2 * 120.0f - 8.0f) * 0.5f;
    kit_.button(ui, winFocus_, kIdXpAccept, Rect{bx, by, 120, 22}, CLIENT_STR(ACCEPT));
    kit_.button(ui, winFocus_, kIdXpCancel, Rect{bx + 128.0f, by, 120, 22}, CLIENT_STR(CANCEL));
}

// --- Clicks ------------------------------------------------------------------------------------

bool WorldScreen::deathWindowAction(uint32_t act) {
    if (act < kIdDeathRelease || act > kIdXpCancel) return false;
    // FocusRing activates on release: a Cross held since before the popup came up is
    // suppressed while down, but its release would still click.
    if (!death_.pressSeen) return true;
    const glm::vec3 self(playerX_, playerY_, playerZ_);
    switch (act) {
        case kIdDeathRelease:  // RepopMe
            if (session_.life() == Life::Dead) {
                session_.releaseSpirit();
                repopSent_ = true;
                death_.reshowSec = 0.0f;
            }
            closeDeathPopup(kWinRelease);
            break;
        case kIdCorpseAccept:  // RetrieveCorpse; the popup stays (OnAccept returns 1)
            if (session_.corpseReclaimDelaySec() <= 0.0f) session_.reclaimCorpse(self);
            break;
        case kIdResAccept:  // AcceptResurrect
            if (death_.offerDelay > 0.0f) break;
            session_.acceptResurrect();
            death_.shownRes = false;
            closeDeathPopup(kWinResurrect);
            break;
        case kIdResDecline:  // DeclineResurrect, DEATH again while dead
            session_.declineResurrect();
            death_.shownRes = false;
            closeDeathPopup(kWinResurrect);
            if (session_.life() == Life::Dead) showDeathPopup(kWinRelease, kIdDeathRelease);
            break;
        case kIdXpAccept: {
            const uint32_t level = session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
            if (level > 10 && !death_.xpAgain) {  // OnAccept with data: the second question, stays up
                death_.xpAgain = true;
                death_.pressSeen = false;
                break;
            }
            session_.acceptResurrect();  // AcceptXPLoss -> CMSG_SPIRIT_HEALER_ACTIVATE
            death_.shownXp = false;
            closeDeathPopup(kWinXpLoss);
            break;
        }
        case kIdXpCancel:  // OnCancel -> CloseGossip
            session_.forgetResurrectOffer();
            session_.closeGossip();
            death_.shownXp = false;
            closeDeathPopup(kWinXpLoss);
            break;
        default: break;
    }
    return true;
}

}  // namespace wowee::ui::psp
