// Controls with modes (the approved scheme, docs/PSP_CONTROLS.md;
// wowee.cfg controls=2, controls=1 = the button scheme of builds 56-69 for A/B).
//
// The nub moves the character (camera-relative, a light tilt walks), R + nub
// turns the camera, R + up / down zooms. Two modes taken from the original client - the
// weapons: travel mode = sheathed (explore: jump, sit, mount, talk), combat mode = drawn
// (the face buttons are action buttons 1-4 directly, L + face 5-8, R + face
// 9-12). Combat draws the weapons, ~sheathdelay s of calm puts them away;
// D-pad down toggles by hand (the client's Z key). The mode shows only through the client's own
// UI: the weapons on the model and their sheath / unsheath sound
// (SoundEntries SHEATHING*), the crossed swords in the player frame's level
// badge (PlayerAttackIcon, UI-StateIcon), the action bar's hotkey glyphs and
// a yellow UIErrorsFrame info line (was a 1.5 s line in its place; modetext=0 turns it off).
//
// This layer turns the physical pad into actions and hands the older handlers
// (camera, target, spells, NPC, sitting, movement) a pad without the bits it
// consumed - they keep working unchanged for controls=1. One table (kActions)
// says what every button does in each mode: the action bar glyphs read it,
// and so will the tutorial windows (cloud batch 4, task C).
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_public_safe.hpp"  // PSP: automation / debug only on a dev server
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_vmath.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "ui/psp/ui_world_sounds.hpp"
#include "ui/psp/ui_teleport_hold.hpp"  // lifeWalks

#include <pspctrl.h>

#include <algorithm>
#include <cmath>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace anim = wowee::rendering::anim;
using Life = pp::WorldSession::Life;

namespace {

constexpr uint32_t kFace[4] = {PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE, PSP_CTRL_CIRCLE, PSP_CTRL_CROSS};
constexpr uint32_t kFaceAll = PSP_CTRL_SQUARE | PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS;
constexpr uint32_t kDpadAll = PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
constexpr uint32_t kAuraMounted = 78;  // SPELL_AURA_MOUNTED
constexpr float kStickDead = 0.22f, kWalkBelow = 0.62f;

float angleDiff(float a, float b) {
    float d = a - b;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return d;
}

}  // namespace

void WorldScreen::initControls() {
    controlsInit_ = true;
    controlsScheme_ = std::clamp(pp::configGetInt("controls", 2), 1, 2);
    modeTextOn_ = pp::configGetInt("modetext", 1) != 0;
    camFollow_ = pp::configGetInt("camfollow", 1) != 0;
    // The camera defaults: fov 90 -> 54 deg vertical, max distance 15, start 5.55 yd (camdist, tenths of a
    // yard: 56). The field of view and the distances await observation (round 2: camera_fov, ui_camera).
    camFovY_ = 0.6f * static_cast<float>(std::clamp(pp::configGetInt("fov", 90), 50, 120));
    camDistMax_ = static_cast<float>(std::clamp(pp::configGetInt("camdistmax", 15), 5, 50));
    distance_ = currentDist_ = shownDist_ =
        std::min(camDistMax_, 0.1f * static_cast<float>(std::clamp(pp::configGetInt("camdist", 56), 5, 500)));
    // VFPU math for the hot paths (psp_vmath.hpp; 0 = FPU / libm, A/B) + the
    // hardware timings of both paths once (log vmath_bench).
    pp::vmath::enabled = pp::configGetInt("vfpumath", 1) != 0;
    if (pp::vmath::enabled) pp::vmath::bench();  // vfpumath=0: no VFPU instruction at all
    PSP_LOGI(pp::tag::Ctrl, "controls scheme=%d modetext=%d camfollow=%d fovy=%.1f camdist=%.2f max=%.0f",
             controlsScheme_, modeTextOn_ ? 1 : 0, camFollow_ ? 1 : 0, camFovY_, distance_, camDistMax_);
}

// Weapons in hand (combat mode) or away (travel mode): the model, the reach animation and
// the sound of the weapon's material, then the mode line.
void WorldScreen::setCombatMode(bool on, const char* src) {
    if (combatMode_ == on) return;
    combatMode_ = on;
    // the mode line is a UIErrorsFrame info line (UI_INFO_MESSAGE, yellow,
    // in the stack with the errors, 5 s + a 3 s fade); was its own gold line for 1.5 s at the same place.
    if (modeTextOn_) uiInfo(on ? OWN_STR(MODE_WEAPON_DRAWN) : OWN_STR(MODE_WEAPON_SHEATHED));
    // Everything held at the switch stays dead until released (an X that
    // jumped must not cast the spell now on X).
    ctlMask_ |= lastRawHeld_ & (kFaceAll | kDpadAll | PSP_CTRL_START);
    if (player_.loaded()) {
        drawOwnWeapons(on);
        // SoundEntries SHEATHING*: a shield in the off hand sounds as a shield, the rest as metal.
        playSoundByName(on ? (ownOffHandShield() ? "SHEATHINGSHIELDUNSHEATHE" : "SHEATHINGMETALWEAPONUNSHEATHE")
                           : (ownOffHandShield() ? "SHEATHINGSHIELDSHEATHE" : "SHEATHINGMETALWEAPONSHEATHE"));
    }
    PSP_LOGI(pp::tag::Ctrl, "mode %s src=%s", on ? "combat" : "path", src);
}

// The first mount spell the player knows (an aura SPELL_AURA_MOUNTED).
void WorldScreen::castMount() {
    uint32_t best = 0;
    for (uint32_t id : session_.knownSpells()) {
        const pp::SpellInfo* s = spellDb_.get(id);
        if (!s) continue;
        for (uint16_t a : s->rec.aura)
            if (a == kAuraMounted && (best == 0 || id < best)) best = id;
    }
    if (best == 0) {
        combatError_ = OWN_STR(NO_MOUNT);
        combatErrorUntilMs_ = pp::logUptimeMs() + 2000u;
        return;
    }
    if (sitting_) setSitting(false, false);
    session_.castSpell(best, 0, false);
}

const pp::PadState& WorldScreen::updateControls(const pp::PadState& pad, float dtSec) {
    if (!controlsInit_) initControls();
    if (controlsScheme_ < 2) return pad;
    PROF_SCOPE("controls");
    // Emulator smoke test (wowee.cfg autotarget=1, like autologin): the nearest
    // hostile becomes the target 10 s into the world, no pad needed.
    {
        static int autoTarget = -1;
        static uint32_t armMs = 0;
        if (autoTarget < 0) {
            autoTarget = pp::configGetInt("autotarget", 0);
            armMs = pp::logUptimeMs() + 10000u;
        }
        if (autoTarget > 0 && pp::logUptimeMs() >= armMs && !pp::devToolsAllowed()) {
            PSP_LOGW(pp::tag::Ctrl, "autotarget_refused public=1");  // PSP: automation only on a dev server
            autoTarget = 0;
        }
        if (autoTarget > 0 && pp::logUptimeMs() >= armMs) {
            const int mode = autoTarget;
            autoTarget = 0;
            tabTarget(1, mode > 1 ? 1 : 0);  // 2: any unit
            PSP_LOGI(pp::tag::Ctrl, "autotarget=%08X", static_cast<unsigned>(session_.target() & 0xFFFFFFFFu));
        }
    }
    lastRawHeld_ = pad.held;
    ctlMask_ &= pad.held;  // released buttons are live again
    const uint32_t live = pad.pressed & ~ctlMask_;
    const bool lHeld = (pad.held & PSP_CTRL_LTRIGGER) != 0;
    const bool rHeld = (pad.held & PSP_CTRL_RTRIGGER) != 0;
    const bool alive = session_.life() == Life::Alive;
    // PSP: a ghost walks and jumps too (upstream: only the corpse stays) - was `alive`.
    const bool walks = lifeWalks(static_cast<int>(session_.life()));
    flightControls(pad, combatMode_);  // the climb keys held (ascend ✕ / START, descend ○)

    // Stick: movement (camera-relative), or the camera while R is held.
    float sx = pad.ax, sy = pad.ay;
    const float mag = std::sqrt(sx * sx + sy * sy);
    stickMove_ = !rHeld && walks && mag > kStickDead && !taxiLocksControls();  // not in flight
    if (stickMove_) {
        const float k = (mag - kStickDead) / (1.0f - kStickDead) / mag;
        moveX_ = sx * k;
        moveY_ = sy * k;
        stickWalk_ = mag < kWalkBelow;
        autorun_ = false;
    } else {
        moveX_ = moveY_ = 0.0f;
        stickWalk_ = false;
    }
    if (mag > kStickDead || pad.held) {  // any input: not AFK (the handlers below see a filtered pad)
        idleSec_ = 0.0f;
        if (afk_) setSitting(false, false);
    }
    if (stickMove_ && sitting_) setSitting(false, false);

    // Mode: combat draws, calm puts away (a manual draw holds until the next calm after a fight).
    // on a mount being in combat alone does not draw them (upstream and the
    // client put them away for the ride; an attack still draws them).
    const bool fighting = (session_.inCombat() && !ownMountDisplay_) || session_.autoAttackRequested() ||
                          session_.autoAttacking();
    if (fighting) {
        modeCalm_ = 0.0f;
        modeManual_ = false;
        if (!combatMode_ && alive) setCombatMode(true, "combat");
    } else {
        modeCalm_ += dtSec;
        if (combatMode_ && !modeManual_ && modeCalm_ >= sheathDelay_) setCombatMode(false, "calm");
    }

    // Always.
    // R + up / down aim a vehicle whose aim can be adjusted (classBarKeys) - no zoom then.
    const bool aimKeys = session_.inVehicle() && session_.vehicleAimAdjustable();
    if (rHeld && (pad.held & PSP_CTRL_UP) && !aimKeys) {
        distance_ -= kZoomSpeed * dtSec;
        zoomDebt_ = 0.0f;
    }
    if (rHeld && (pad.held & PSP_CTRL_DOWN) && !aimKeys) {
        distance_ += kZoomSpeed * dtSec;
        zoomDebt_ = 0.0f;
    }
    if (!rHeld && !lHeld) {
        if (live & PSP_CTRL_DOWN) {
            modeManual_ = !combatMode_;
            modeCalm_ = 0.0f;
            setCombatMode(!combatMode_, "manual");
        }
        if (live & PSP_CTRL_UP) interactTarget();
        if (live & PSP_CTRL_RIGHT) tabTarget(1, combatMode_ ? 0 : 1);
        if (live & PSP_CTRL_LEFT) tabTarget(-1, combatMode_ ? 0 : 1);
    }
    if (lHeld && rHeld && (live & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))) autorun_ = !autorun_;
    // L + D-pad = the pet's buttons 1..4 while the pet bar shows; without a pet
    // L + Up picks the next object in reach - both approved, one key: the pet first.
    if (alive) petKeys(live, lHeld, rHeld);
    if (alive) classBarKeys(live, lHeld, rHeld);  // forms, totem calls, the vehicle / possession keys
    if (lHeld && !rHeld && (live & PSP_CTRL_UP) && !petBarShown()) cycleGameObjectHover();

    // Face buttons by mode (kActions in the header comment of docs/PSP_CONTROLS.md).
    if (alive) {
        const int base = mainBarBase();
        for (int i = 0; i < 4; ++i) {
            if (!(live & kFace[i])) continue;
            if (combatMode_) {
                castFromSlot(base + (rHeld ? 8 : lHeld ? 4 : 0) + i);
            } else if (lHeld && !rHeld) {
                castFromSlot(base + i);
            } else if (!rHeld && !taxiLocksControls()) {  // no jump / sit / mount in flight
                switch (kFace[i]) {
                    // PSP: upstream CameraController - nowJump needs !sitting, and stands up on any movement input or
                    // jump while sitting, as the original client (camera_controller.cpp ~2767 / ~3038): the jump key only
                    // stands up (the PSP used to stand up and jump in one press). Swimming / flying there is no sitting.
                    case PSP_CTRL_CROSS:
                        if (sitting_ && !flightTakesCircle()) setSitting(false, false);
                        else jumpReq_ = true;
                        break;
                    case PSP_CTRL_CIRCLE:  // swimming / flying it descends (SitStandOrDescendStart)
                        if (!flightTakesCircle()) setSitting(!sitting_, false);
                        break;
                    case PSP_CTRL_SQUARE: clearTarget(); break;
                    case PSP_CTRL_TRIANGLE: castMount(); break;
                    default: break;
                }
            }
        }
        if (combatMode_ && (live & PSP_CTRL_START)) {
            if (sitting_ && !flightTakesCircle()) setSitting(false, false);  // PSP: upstream - stands up, no jump
            else jumpReq_ = true;
        }
    }
    if (!alive && walks && !rHeld && !lHeld && (live & PSP_CTRL_CROSS) && !taxiLocksControls())
        jumpReq_ = true;  // PSP: a ghost's ✕ jumps (no spells, no sitting, no mount)
    if (!combatMode_ && (live & PSP_CTRL_START)) {  // Esc: the game menu
        setUiMode(true);
        openWindow(kWinMenu);
        ctlMask_ |= PSP_CTRL_START;
    }

    // Camera follows behind while walking with the stick (upstream camera
    // "follow" style; camfollow=0 = never).
    // (Running toward the camera used to shake it left and right:)
    // facing the camera the difference is ~180 deg and its sign flips frame to
    // frame. The pull is weighted by how much the run goes away from the
    // camera (cos of the difference): full straight ahead, none sideways or
    // toward it - the camera stays and the character runs into view.
    if (camFollow_ && stickMove_ && !rHeld) {
        const float d = angleDiff(facing_, camYaw_);
        const float weight = std::max(0.0f, std::cos(d));
        camYaw_ += d * (1.0f - std::exp(-dtSec * 1.6f * weight));
    }
    // in flight the camera follows behind the flight's heading (the client's camera follow while
    // the character moves); R + stick still looks around, released it comes back behind.
    if (camFollow_ && !rHeld && session_.isOnTaxiFlight()) camYaw_ += angleDiff(facing_, camYaw_) * (1.0f - std::exp(-dtSec * 1.6f));

    // What the older handlers see: no face buttons / D-pad / START / L (all
    // taken here), R only as the camera's drag, the stick only with R.
    ctlPad_ = pad;
    const uint32_t taken = kFaceAll | kDpadAll | PSP_CTRL_START | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER;
    ctlPad_.held &= ~taken;
    ctlPad_.pressed &= ~taken;
    ctlPad_.released &= ~taken;
    if (!rHeld) ctlPad_.ax = ctlPad_.ay = 0.0f;
    return ctlPad_;
}

// Mode line: a UIErrorsFrame info row now (setCombatMode -> uiInfo,
// drawn with the error stack, ui_world_errors.cpp); nothing is drawn here (the spells pass still calls it).
void WorldScreen::drawModeHud(UiRenderer& ui) { (void)ui; }

}  // namespace wowee::ui::psp
