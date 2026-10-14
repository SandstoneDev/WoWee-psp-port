// The class bars and the vehicles on the world screen, as the ruRU 3.3.5a
// client draws them (FrameXML BonusActionBarFrame.xml / .lua, MultiCastActionBarFrame.xml / .lua, TotemFrame.xml / .lua,
// MainMenuBar.lua (MainMenuBarVehicleLeaveButton), VehicleMenuBar.xml / .lua, UIParent.lua's frame positions) on the
// client's art (ui.pak "cb_*", tools/psp/build_ui_pak.py class_bar_art) and the session's state
// (psp_world_session_classbars.cpp: upstream, AzerothCore and the port's own rules):
//   - ShapeshiftBarFrame / PossessBarFrame / MultiCastActionBarFrame at MainMenuBar TOPLEFT + 30 (MainMenuBar's 0.5),
//     one at a time (PossessBar_Update, UIParent_ManageFramePositions' "HACK"s); MainMenuBarVehicleLeaveButton right of
//     the shown one; the pet bar moves right of them (PetActionBar_UpdatePositionValues), the chat up ("pet = 35");
//   - the override bar (GetBonusBarOffset 5: a possessed unit's / a vehicle's actions, a form's preset spells,
//     OverrideSpellData) is BonusActionBarFrame over MainMenuBar - the same art, its own twelve buttons;
//   - TotemFrame under PlayerFrame (TotemFrame_Update: shamans and death knights, TOTEM_PRIORITIES order, the timer
//     under each, the right click destroys the totem);
//   - VehicleMenuBar instead of MainMenuBar while UnitHasVehicleUI("player") (its skin by UnitVehicleSkin, the pitch
//     parts when IsVehicleAimAngleAdjustable), six VehicleMenuBarActionButtons (actions 121..126), the leave / pitch
//     buttons, the vehicle's health and power; VehicleSeatIndicator under the minimap (a seat: UnitSwitchToVehicleSeat);
//   - the player's model swap: UNIT_FIELD_DISPLAYID not the native display (a druid form, ghost wolf, a polymorph, a
//     ghost's wisp...) is drawn as that creature display (a driven unit of creatures_, mirroring player_'s
//     animation) - other players likewise; the vehicle seat: the player on the seat's attachment of the vehicle's
//     model, in the seat's ride animation; the unit we drive / possess is moved by the movement code (playerX_ = it,
//     our own body stays where it was while we possess).
// PSP differences (PSP: below): the keys (the doc's proposal); the flyout's frame is a plain panel; no micro buttons on
// VehicleMenuBar (their place stays empty - the menu is START); the pitch slider's marker is a red bar; the vehicle's
// power bar takes PowerBarColor of its power type (PowerDisplay.dbc is not read); no mouse look of vehicles' turrets.
#include "ui/psp/ui_world_screen.hpp"

#include "core/coordinates.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_classbar_parse.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_flight_rules.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <pspctrl.h>
#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace cb = wowee::platform::psp::cbar;
namespace pa = wowee::platform::psp::petact;
namespace anim = wowee::rendering::anim;
namespace fr = wowee::platform::psp::flight;

namespace {

// Widget ids: 0x8D00..0x8DFF.
enum : uint32_t {
    kIdForm0 = 0x8D00,       // ShapeshiftButton1..10
    kIdPossess0 = 0x8D10,    // PossessButton1..2
    kIdLeave = 0x8D12,       // MainMenuBarVehicleLeaveButton
    kIdCall = 0x8D20,        // MultiCastSummonSpellButton
    kIdCallArrow = 0x8D21,   // its flyout button
    kIdSlot0 = 0x8D22,       // MultiCastSlotButton1..4 (the action button on it)
    kIdSlotArrow0 = 0x8D26,  // MultiCastFlyoutFrameOpenButton over slot button 1..4
    kIdRecall = 0x8D2A,      // MultiCastRecallSpellButton
    kIdFlyout0 = 0x8D30,     // MultiCastFlyoutButton1..16
    kIdFlyoutClose = 0x8D40, // MultiCastFlyoutFrameCloseButton
    kIdTotem0 = 0x8D48,      // TotemFrameTotem1..4
    kIdBonus0 = 0x8D50,      // BonusActionButton1..12 (the override bar)
    kIdVehAction0 = 0x8D60,  // VehicleMenuBarActionButton1..6
    kIdVehLeave = 0x8D66,
    kIdVehPitchUp = 0x8D67,
    kIdVehPitchDown = 0x8D68,
    kIdVehSeat0 = 0x8D70,    // VehicleSeatIndicatorButton1..8
};

// MainMenuBar at 0.5 (ui_world_mainmenubar.cpp): its left 106, its bottom the screen's, 53 units tall.
constexpr float kBarS = 0.5f, kMmbLeft = 106.0f, kMmbBottom = 272.0f;
constexpr float kMmbTop = kMmbBottom - 53.0f * kBarS;
// A child of a frame at MainMenuBar TOPLEFT + 30 (ShapeshiftBarFrame, PossessBarFrame, MultiCastActionBarFrame): its
// BOTTOMLEFT (x, y) and size in units.
Rect barRect(float x, float y, float w, float h) {
    return Rect{kMmbLeft + (30.0f + x) * kBarS, kMmbTop - (y + h) * kBarS, w * kBarS, h * kBarS};
}
// ShapeshiftButtonN's x in its frame: 1 at 10 (12 when it is alone), 2 = 1's right + 8, the rest + 7.
float formButtonX(int i, int n) {
    if (n == 1) return 12.0f;
    if (i == 0) return 10.0f;
    return 48.0f + 37.0f * static_cast<float>(i - 1);
}
// The unit frames' 0.6 (ui_world_target.cpp kFrameScale) - TotemFrame hangs off PlayerFrame (2, 2).
constexpr float kUf = 0.6f;
constexpr uint8_t kClassDeathKnight = 6, kClassShaman = 7;
constexpr uint32_t kNpcFlagSpellClick = 0x01000000u;  // UNIT_NPC_FLAG_SPELLCLICK
constexpr uint64_t kMountGuidTag = 0x4D00000000000000ull;  // ui_world_mount.cpp: the creature key of a player's mount

// UI-TotemBar (128 x 256) parts, by 0-based totem slot (fire, earth, water, air) - MultiCastActionBarFrame.lua's
// SLOT_EMPTY_TCOORDS / SLOT_OVERLAY_TCOORDS / FLYOUT_UP_BUTTON_TCOORDS in texels.
constexpr SrcRect kSlotEmpty[4] = {{67, 100, 30, 30}, {66, 3, 30, 30}, {39, 209, 30, 30}, {66, 36, 30, 30}};
constexpr SrcRect kSlotOverlay[4] = {{36, 172, 34, 34}, {1, 172, 34, 34}, {1, 207, 34, 33}, {36, 137, 34, 34}};
constexpr SrcRect kFlyoutUp[4] = {{99, 122, 28, 18}, {99, 160, 28, 18}, {99, 199, 28, 18}, {99, 237, 28, 18}};
constexpr SrcRect kFlyoutUpSummon = {99, 84, 28, 18};
// ShapeshiftBar.png (64 x 128): ShapeshiftBarLeft 0 .. 0.734375 x 0 .. 0.296875, ShapeshiftBarRight 0.328125 .. 1 x
// 0.3125 .. 0.6015625.
constexpr SrcRect kSsLeft = {0, 0, 47, 38};
constexpr SrcRect kSsRight = {21, 40, 43, 37};
// VehicleSeats.png (128 x 32): the seat button's background, the player's / an ally's dot.
constexpr SrcRect kSeatBg = {0, 0, 25, 25};
constexpr SrcRect kSeatPlayer = {50, 0, 9, 9};
constexpr SrcRect kSeatAlly = {50, 10, 9, 9};
// PET_*_TEXTURE as SpellIcon ids (ui_world_pets.cpp): commands stay / follow / attack / dismiss, reactions.
constexpr uint32_t kCommandIcon[4] = {58, 179, 108, 87};
constexpr uint32_t kReactIcon[3] = {29, 28, 86};

// GlobalStrings.lua through strings.pak: CANCEL (PossessButton2), LEAVE_VEHICLE, AIM_UP, AIM_DOWN,
// MULTI_CAST_TOOLTIP_NO_TOTEM; SecondsToTimeAbbrev's *_ONELETTER_ABBR.

// VehicleMenuBar's skins (VehicleMenuBar.lua SkinsData): the overall widths and the parts the PSP draws itself
// (the rest is the baked "cb_veh_<m|n><pitch>" composite). Points: BOTTOMLEFT / BOTTOMRIGHT offsets in units.
struct VehSkin {
    float wPitch, wNoPitch;
    float pitchX, pitchUpY, pitchDownY, pitchW, pitchH;     // BOTTOMLEFT
    float leaveXr, leaveY, leaveW, leaveH;                  // BOTTOMRIGHT (x from the right edge, negative)
    float actXr, actY, actScale;                            // ActionButtonFrame's BOTTOMLEFT at the bar's BOTTOMRIGHT
    float sliderX, sliderY, sliderW, sliderH;               // BOTTOMLEFT
    float hpX, hpY, barW, barH;                             // HealthBar BOTTOMLEFT
    float mpXr, mpY;                                        // PowerBar BOTTOMRIGHT
    float ovlX, ovlY, ovlW, ovlH;                           // the bars' overlay at the bar's BOTTOMLEFT
};
constexpr VehSkin kSkins[2] = {
    // Natural (UiSkin 0)
    {942, 869, 166, 41, 3, 34, 35, -177, 15, 50, 47, -820, 26, 0.85f, 204, 0, 31, 82, 119, 3, 38, 74, -119, 3, -5, -9, 46, 105},
    // Mechanical (UiSkin 1, and the default)
    {970, 888, 146, 41, 3, 38, 36, -148, 18, 50, 47, -735, 15, 1.0f, 192, 3, 31, 80, 96, 10, 33, 72, -98, 10, -5, -9, 44, 92},
};
constexpr float kVehS = 0.49f;  // PSP: the vehicle bar's scale (970 units must fit the 480 px screen)

uint32_t powerColor(uint8_t type) {  // PowerBarColor (as ui_world_pets.cpp)
    switch (type) {
        case 0: return 0xFFFF0000u;
        case 1: return 0xFF0000FFu;
        case 2: return 0xFF4080FFu;
        case 3: return 0xFF00FFFFu;
        case 6: return 0xFFFFD100u;
        default: return 0xFFFF0000u;
    }
}

// SecondsToTimeAbbrev (AuraButton_UpdateDuration under a totem): minutes rounded up, seconds down.
void timeAbbrev(char* out, size_t n, uint32_t ms) {
    std::string t;
    if (ms >= 3600000u) t = CLIENT_FMT(HOUR_ONELETTER_ABBR, static_cast<unsigned>((ms + 3599999u) / 3600000u));
    else if (ms >= 60000u) t = CLIENT_FMT(MINUTE_ONELETTER_ABBR, static_cast<unsigned>((ms + 59999u) / 60000u));
    else t = CLIENT_FMT(SECOND_ONELETTER_ABBR, static_cast<unsigned>(ms / 1000u));
    std::snprintf(out, n, "%s", t.c_str());
}

bool loadTex(pp::Pak& pak, const char* name, pp::Texture& t, uint32_t& bytes) {
    const pp::PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!pp::textureFromBlob(blob, e->size, t, true)) {
        pp::pakFree(blob);
        return false;
    }
    t.name = "cbar";
    bytes += e->size;
    return true;
}

bool loopAnim(uint32_t a) {
    return a == anim::STAND || a == anim::WALK || a == anim::RUN || a == anim::SWIM || a == anim::SWIM_IDLE ||
           a == anim::JUMP || a == anim::FALL || a == anim::FLY_FORWARD || a == anim::FLY_IDLE ||
           a == anim::MOUNT || a == anim::READY_UNARMED || (a >= 25 && a <= 28);
}

}  // namespace

struct WorldScreen::ClassBarUi {
    // The bars' art: shapeshift / possess (while shown), the totem bar (while shown), TotemFrame (while a totem),
    // the vehicle bar (its skin while in the vehicle UI), the seat indicator (while in a vehicle with one).
    pp::Texture ssbar, ssmid, checked;
    uint32_t barBytes = 0;
    bool barTried = false;
    pp::Texture totemBar;
    uint32_t totemBarBytes = 0;
    bool totemBarTried = false;
    pp::Texture totemBorder, totemBg;
    uint32_t totemFrameBytes = 0;
    bool totemFrameTried = false;
    pp::Texture vehBar, vehOverlay, vehExit, vehPitchUp, vehPitchDown;
    uint32_t vehBytes = 0;
    int vehKey = -1;  // skin * 2 + pitch of what is loaded
    pp::Texture seats, seatBg;
    uint32_t seatBytes = 0, seatFor = 0;
    bool seatTried = false;
    // What shows this frame (updateClassBars).
    bool formsShown = false, possessShown = false, multiShown = false, leaveShown = false, overrideShown = false;
    bool vehicleUi = false;
    int multiSlots[4] = {-1, -1, -1, -1};  // the slot buttons' totem slots (TOTEM_PRIORITIES compacted), -1 none
    int multiCount = 0;
    int callPage = 0;  // MultiCastActionBarFrame.currentPage - 1
    int flyout = -2;   // MultiCastFlyoutFrame: -1 the calls, 0..3 a totem slot, -2 shut
    Rect flyoutAnchor{};
    // Tooltips (drawClassBarOverlay).
    uint32_t tipSpell = 0;
    Rect tipRect{};
    const char* tipText = nullptr;
    // The own model swap and the other players'.
    uint32_t swapDisplay = 0;
    bool swapReady = false;
    uint32_t swapAnim = ~0u;
    struct RemoteSwap {
        uint32_t display = 0;
        bool ready = false, tried = false;
    };
    std::unordered_map<uint64_t, RemoteSwap> remote;
    // The unit we move (a vehicle we drive, a possessed unit) and our body while we possess.
    uint64_t driving = 0;
    glm::vec3 home{0.0f};
    float homeFacing = 0.0f;
    bool homeValid = false;
    glm::vec3 seatPos{0.0f};
    float seatYaw = 0.0f;
    bool seatValid = false;
    uint32_t lastVehicleVersion = ~0u;
    uint32_t logs = 0;

    void loadBarArt() {
        barTried = true;
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        int ok = 0;
        if (pak.open(path)) {
            ok += loadTex(pak, "cb_ssbar", ssbar, barBytes) ? 1 : 0;
            ok += loadTex(pak, "cb_ssmid", ssmid, barBytes) ? 1 : 0;
            ok += loadTex(pak, "sb_checked", checked, barBytes) ? 1 : 0;
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "cbar_art ok=%d/3 kb=%u", ok, static_cast<unsigned>((barBytes + 1023u) / 1024u));
    }
    void releaseBarArt() {
        for (pp::Texture* t : {&ssbar, &ssmid, &checked})
            if (t->pixels) pp::textureRelease(*t);
        if (barTried) PSP_LOGI(pp::tag::Gfx, "cbar_art_free kb=%u", static_cast<unsigned>((barBytes + 1023u) / 1024u));
        barTried = false;
        barBytes = 0;
    }
    void loadOne(const char* name, pp::Texture& t, uint32_t& bytes) {
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        if (pak.open(path)) {
            loadTex(pak, name, t, bytes);
            pak.close();
        }
    }
    void releaseTotemBar() {
        if (totemBar.pixels) pp::textureRelease(totemBar);
        if (totemBarTried) PSP_LOGI(pp::tag::Gfx, "totembar_art_free kb=%u", static_cast<unsigned>((totemBarBytes + 1023u) / 1024u));
        totemBarTried = false;
        totemBarBytes = 0;
    }
    void releaseTotemFrame() {
        for (pp::Texture* t : {&totemBorder, &totemBg})
            if (t->pixels) pp::textureRelease(*t);
        if (totemFrameTried)
            PSP_LOGI(pp::tag::Gfx, "totemframe_art_free kb=%u", static_cast<unsigned>((totemFrameBytes + 1023u) / 1024u));
        totemFrameTried = false;
        totemFrameBytes = 0;
    }
    void loadVehArt(int key) {
        releaseVehArt();
        vehKey = key;
        static const char* const kBar[4] = {"cb_veh_n0", "cb_veh_n1", "cb_veh_m0", "cb_veh_m1"};
        char path[256];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        int ok = 0;
        if (pak.open(path)) {
            ok += loadTex(pak, kBar[key & 3], vehBar, vehBytes) ? 1 : 0;
            ok += loadTex(pak, (key >> 1) ? "cb_veh_ovl_m" : "cb_veh_ovl_n", vehOverlay, vehBytes) ? 1 : 0;
            ok += loadTex(pak, "cb_veh_exit", vehExit, vehBytes) ? 1 : 0;
            if (key & 1) {
                ok += loadTex(pak, "cb_veh_pup", vehPitchUp, vehBytes) ? 1 : 0;
                ok += loadTex(pak, "cb_veh_pdown", vehPitchDown, vehBytes) ? 1 : 0;
            }
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "vehbar_art skin=%d pitch=%d ok=%d kb=%u", key >> 1, key & 1, ok,
                 static_cast<unsigned>((vehBytes + 1023u) / 1024u));
    }
    void releaseVehArt() {
        for (pp::Texture* t : {&vehBar, &vehOverlay, &vehExit, &vehPitchUp, &vehPitchDown})
            if (t->pixels) pp::textureRelease(*t);
        if (vehKey >= 0) PSP_LOGI(pp::tag::Gfx, "vehbar_art_free kb=%u", static_cast<unsigned>((vehBytes + 1023u) / 1024u));
        vehKey = -1;
        vehBytes = 0;
    }
    void loadSeatArt(uint32_t indicator) {
        releaseSeatArt();
        seatTried = true;
        seatFor = indicator;
        char path[256], name[24];
        pp::fsPath(path, sizeof(path), "data/ui.pak");
        pp::Pak pak;
        int ok = 0;
        if (pak.open(path)) {
            ok += loadTex(pak, "cb_vseats", seats, seatBytes) ? 1 : 0;
            std::snprintf(name, sizeof(name), "cb_vind_%u", static_cast<unsigned>(indicator));
            ok += loadTex(pak, name, seatBg, seatBytes) ? 1 : 0;
            pak.close();
        }
        PSP_LOGI(pp::tag::Gfx, "vehseat_art ind=%u ok=%d kb=%u", static_cast<unsigned>(indicator), ok,
                 static_cast<unsigned>((seatBytes + 1023u) / 1024u));
    }
    void releaseSeatArt() {
        for (pp::Texture* t : {&seats, &seatBg})
            if (t->pixels) pp::textureRelease(*t);
        if (seatTried) PSP_LOGI(pp::tag::Gfx, "vehseat_art_free kb=%u", static_cast<unsigned>((seatBytes + 1023u) / 1024u));
        seatTried = false;
        seatFor = 0;
        seatBytes = 0;
    }
    ~ClassBarUi() {
        releaseBarArt();
        releaseTotemBar();
        releaseTotemFrame();
        releaseVehArt();
        releaseSeatArt();
    }

    // --- Helpers with the world screen's state (a nested class reaches its members) ---

    // MainMenuBarVehicleLeaveButton (32 units): LEFT of PossessButton2 / the last ShapeshiftButton /
    // MultiCastActionBarFrame RIGHT + 30, else PossessBarFrame LEFT + 10 (MainMenuBarVehicleLeaveButton_Update).
    float leaveX(WorldScreen& w) const {
        if (possessShown) return 10.0f + 38.0f + 30.0f + 30.0f;
        if (formsShown) {
            const int n = static_cast<int>(w.session_.shapeshiftForms().size());
            return formButtonX(n - 1, n) + 30.0f + 30.0f;
        }
        if (multiShown) return 230.0f + 30.0f;
        return 10.0f;
    }
    // The seat of our vehicle in the world (server coordinates) and the passenger's yaw.
    bool seatWorld(WorldScreen& w, glm::vec3& out, float& yaw) {
        const uint64_t veh = w.session_.vehicleGuid();
        if (!veh) return false;
        const cb::VehicleSeatRec* seat = w.session_.vehicleSeat();
        const int32_t attach = seat ? seat->attachmentId : -1;
        const float passengerYaw = seat ? seat->passengerYaw : 0.0f;
        glm::vec3 off(seat ? seat->offset[0] : 0.0f, seat ? seat->offset[1] : 0.0f, seat ? seat->offset[2] : 0.0f);
        // A player's two-seat mount: the vehicle is their mount's unit (ui_world_mount.cpp).
        const bool playerVehicle = (veh >> 48) == 0;
        const uint64_t key = playerVehicle ? (kMountGuidTag | (veh & 0x00FFFFFFFFFFFFFFull)) : veh;
        float rh = 1.8f, vyaw = 0.0f;
        glm::vec3 p;
        bool ok = attach >= 0 && w.creatures_.attachPoint(key, attach, p, &rh, &vyaw);
        if (!ok && w.creatures_.attachPoint(key, -1, p, &rh, &vyaw)) {
            // PSP: an older creatures.pak without the vehicle seat points - the seat's offset + the rider height.
            p.z += rh;
            ok = true;
        }
        if (!ok) {
            float h = 0.0f;
            if (!w.unitPosition(veh, p, h)) return false;
            auto e = w.session_.entities().getEntity(veh);
            vyaw = e ? core::coords::canonicalToServerYaw(e->getOrientation()) : 0.0f;
            p.z += h * 0.6f;
        }
        const float c = std::cos(vyaw), s = std::sin(vyaw);
        out = p + glm::vec3(c * off.x - s * off.y, s * off.x + c * off.y, off.z);
        yaw = vyaw + passengerYaw;
        return true;
    }
    // The fallback when the creature lacks the animation player_ plays.
    uint32_t swapPick(WorldScreen& w, uint32_t display, uint32_t want) const {
        auto has = [&](uint32_t a) { return w.creatures_.displayHasAnimation(display, a); };
        if (has(want)) return want;
        switch (want) {
            case anim::RUN: return has(anim::WALK) ? anim::WALK : anim::STAND;
            case anim::SWIM: case anim::SWIM_IDLE: return has(anim::SWIM_IDLE) ? anim::SWIM_IDLE : anim::STAND;
            case anim::FLY_FORWARD: return has(anim::FLY_IDLE) ? anim::FLY_IDLE : has(anim::RUN) ? anim::RUN : anim::STAND;
            case anim::JUMP_START: case anim::JUMP: case anim::JUMP_END: case anim::FALL:
                return has(anim::JUMP) ? anim::JUMP : anim::STAND;
            default:
                if (want >= anim::ATTACK_UNARMED && want <= 21) {
                    for (uint32_t a : {anim::ATTACK_UNARMED, anim::ATTACK_1H, anim::ATTACK_2H})
                        if (has(a)) return a;
                }
                return anim::STAND;
        }
    }
};

void WorldScreen::destroyClassBarUi(ClassBarUi* p) { delete p; }

WorldScreen::ClassBarUi& WorldScreen::cbarUi() {
    if (!cbar_) cbar_ = std::unique_ptr<ClassBarUi, void (*)(ClassBarUi*)>(new ClassBarUi, &WorldScreen::destroyClassBarUi);
    return *cbar_;
}

// --- The model swap ----------------------------------------------------------------------------------------------------

bool WorldScreen::ownModelSwapped() const { return cbar_ && cbar_->swapReady && cbar_->swapDisplay != 0; }
uint32_t WorldScreen::ownSwapDisplay() const { return ownModelSwapped() ? cbar_->swapDisplay : 0u; }
bool WorldScreen::ownSwapHasAnimation(uint32_t animId) const {
    return ownModelSwapped() && creatures_.displayHasAnimation(cbar_->swapDisplay, animId);
}
void WorldScreen::ownSwapPlay(uint32_t animId, bool loop) {
    if (!ownModelSwapped()) return;
    ClassBarUi& c = *cbar_;
    creatures_.playDriven(session_.playerGuid(), c.swapPick(*this, c.swapDisplay, animId), loop);
    c.swapAnim = animId;
}

// --- Per frame ---------------------------------------------------------------------------------------------------------

void WorldScreen::updateClassBars(float dtSec) {
    PROF_SCOPE("classbars");
    ClassBarUi& c = cbarUi();
    session_.classBarsUpdate(dtSec);
    // Our TransportExit spline: off the vehicle, at its end (the server moves us there).
    glm::vec3 ep;
    float eo = 0.0f;
    if (session_.takeVehicleExitPlace(ep, eo)) {
        playerX_ = ep.x;
        playerY_ = ep.y;
        playerZ_ = shownZ_ = ep.z;
        if (eo != 0.0f) facing_ = eo;
        airborne_ = false;
        vz_ = 0.0f;
        moving_ = wantMove_ = false;
        c.seatValid = false;
        PSP_LOGI(pp::tag::Net, "vehicle_exit_place x=%.1f y=%.1f z=%.1f", ep.x, ep.y, ep.z);
    }
    // The mover: from a takeover on, the movement code moves that unit (playerX_ = it); our body stays (a possession)
    // or rides its seat (a vehicle).
    const uint64_t mover = session_.moverGuid();
    if (mover != c.driving) {
        if (c.driving == 0) {
            c.home = glm::vec3(playerX_, playerY_, playerZ_);
            c.homeFacing = facing_;
            c.homeValid = mover != session_.vehicleGuid();
            glm::vec3 p;
            float h = 0.0f;
            if (unitPosition(mover, p, h)) {
                playerX_ = p.x;
                playerY_ = p.y;
                playerZ_ = shownZ_ = p.z;
            }
            if (auto e = session_.entities().getEntity(mover)) facing_ = core::coords::canonicalToServerYaw(e->getOrientation());
        } else if (mover == 0) {
            if (c.homeValid && !session_.inVehicle()) {
                playerX_ = c.home.x;
                playerY_ = c.home.y;
                playerZ_ = shownZ_ = c.home.z;
                facing_ = c.homeFacing;
            }
            c.homeValid = false;
        }
        PSP_LOGI(pp::tag::Net, "mover_take %08X -> %08X possess=%d", static_cast<unsigned>(c.driving & 0xFFFFFFFFu),
                 static_cast<unsigned>(mover & 0xFFFFFFFFu), c.homeValid ? 1 : 0);
        c.driving = mover;
        moving_ = wantMove_ = false;
        airborne_ = false;
    }
    // The own swap (UNIT_FIELD_DISPLAYID 67 vs UNIT_FIELD_NATIVEDISPLAYID 68).
    const uint32_t display = session_.selfField(67), native = session_.selfField(68);
    const uint32_t want = (display && native && display != native && creatures_.isOpen()) ? display : 0u;
    if (want != c.swapDisplay) {
        c.swapDisplay = want;
        c.swapReady = want != 0 && creatures_.hasDisplay(want) && creatures_.preload(want);
        c.swapAnim = ~0u;
        PSP_LOGI(pp::tag::Gfx, "model_swap display=%u native=%u ok=%d", static_cast<unsigned>(display),
                 static_cast<unsigned>(native), c.swapReady ? 1 : 0);
    }
    if (ownModelSwapped()) {  // mirror player_'s animation (the character model keeps animating, undrawn)
        uint32_t cur = 0;
        float t = 0.0f, dur = 0.0f;
        if (ownFlying() && !mountActive_) {
            // In the air the swapped model flies on its own Fly / Hover (upstream's flying unit), whatever the
            // character model has.
            const uint32_t want =
                fr::flyAnim(wantMove_, [&](uint32_t id) { return creatures_.displayHasAnimation(c.swapDisplay, id); });
            if (want != c.swapAnim) ownSwapPlay(want, true);
        } else if (player_.getAnimationState(cur, t, dur) && cur != c.swapAnim) {
            ownSwapPlay(cur, loopAnim(cur));
        }
    }
    // The other players' swaps: one preload a frame.
    bool loaded = false;
    for (auto it = c.remote.begin(); it != c.remote.end();) {
        if (!remotes_.count(it->first)) it = c.remote.erase(it);
        else ++it;
    }
    for (auto& [guid, r] : remotes_) {
        auto e = session_.entities().getEntity(guid);
        if (!e || !r.model) continue;
        const uint32_t d = e->getField(67), n = e->getField(68);
        const uint32_t rw = (d && n && d != n && creatures_.isOpen()) ? d : 0u;
        ClassBarUi::RemoteSwap& rs = c.remote[guid];
        if (rs.display != rw) {
            rs = ClassBarUi::RemoteSwap{};
            rs.display = rw;
        }
        if (rw && !rs.tried && !loaded) {
            loaded = true;
            rs.tried = true;
            rs.ready = creatures_.hasDisplay(rw) && creatures_.preload(rw);
            if (++c.logs <= 40)
                PSP_LOGI(pp::tag::Gfx, "model_swap_remote guid=%08X display=%u ok=%d", static_cast<unsigned>(guid & 0xFFFFFFFFu),
                         static_cast<unsigned>(rw), rs.ready ? 1 : 0);
        }
    }
    // What shows (PossessBar_Update, ShapeshiftBar_Update, the multicast "HACK"s, MainMenuBarVehicleLeaveButton_Update).
    c.vehicleUi = session_.vehicleUi();
    c.possessShown = !c.vehicleUi && session_.possessBarVisible();
    c.formsShown = !c.vehicleUi && !c.possessShown && !session_.shapeshiftForms().empty();
    c.multiCount = 0;
    if (!c.vehicleUi && !c.possessShown && !c.formsShown) {
        for (int i = 0; i < cb::kTotemSlots; ++i) {
            const int slot = cb::kTotemPriority[i];
            if (!session_.multiCastSpells(slot).empty()) c.multiSlots[c.multiCount++] = slot;
        }
    }
    for (int i = c.multiCount; i < 4; ++i) c.multiSlots[i] = -1;
    c.multiShown = c.multiCount > 0;
    if (c.multiShown && !session_.multiCastCallKnown(c.callPage))
        for (int p = 0; p < 3; ++p)
            if (session_.multiCastCallKnown(p)) {
                c.callPage = p;
                break;
            }
    c.leaveShown = !c.vehicleUi && session_.inVehicle() && session_.canExitVehicle();
    c.overrideShown = !c.vehicleUi && session_.bonusBarOffset() == 5;
    if (!c.multiShown && windowOpen(kWinTotemFlyout)) closeWindow(kWinTotemFlyout);
    if (!windowOpen(kWinTotemFlyout)) c.flyout = -2;
    // Art on show, freed on hide.
    const bool barArt = c.formsShown || c.possessShown;
    if (barArt && !c.barTried) c.loadBarArt();
    else if (!barArt && c.barTried) c.releaseBarArt();
    if (c.multiShown && !c.totemBarTried) {
        c.totemBarTried = true;
        c.loadOne("cb_totembar", c.totemBar, c.totemBarBytes);
        PSP_LOGI(pp::tag::Gfx, "totembar_art ok=%d kb=%u", c.totemBar.pixels ? 1 : 0, static_cast<unsigned>((c.totemBarBytes + 1023u) / 1024u));
    } else if (!c.multiShown && c.totemBarTried) {
        c.releaseTotemBar();
    }
    bool anyTotem = false;
    for (int i = 0; i < cb::kTotemSlots; ++i) anyTotem = anyTotem || session_.totemSlot(i).active();
    if (anyTotem && !c.totemFrameTried) {
        c.totemFrameTried = true;
        c.loadOne("cb_totemborder", c.totemBorder, c.totemFrameBytes);
        c.loadOne("cb_totembg", c.totemBg, c.totemFrameBytes);
        PSP_LOGI(pp::tag::Gfx, "totemframe_art kb=%u", static_cast<unsigned>((c.totemFrameBytes + 1023u) / 1024u));
    } else if (!anyTotem && c.totemFrameTried) {
        c.releaseTotemFrame();
    }
    if (c.vehicleUi) {
        const int skin = session_.vehicleSkinNatural() ? 0 : 1;  // UnitVehicleSkin: Natural 0, else Mechanical
        const int key = skin * 2 + (session_.vehicleAimAdjustable() ? 1 : 0);
        if (key != c.vehKey) c.loadVehArt(key);
    } else if (c.vehKey >= 0) {
        c.releaseVehArt();
    }
    const cb::VehicleKit* kit = session_.vehicleKit();
    const uint32_t ind = kit ? kit->vehicle.uiIndicator : 0u;
    if (ind && ind != c.seatFor) c.loadSeatArt(ind);
    else if (!ind && c.seatTried) c.releaseSeatArt();
    if (session_.vehicleVersion() != c.lastVehicleVersion) {
        c.lastVehicleVersion = session_.vehicleVersion();
        PSP_LOGI(pp::tag::Net, "vehicle_ui in=%d ui=%d seat=%d drive=%d exit=%d switch=%d aim=%d offset=%d",
                 session_.inVehicle() ? 1 : 0, c.vehicleUi ? 1 : 0, static_cast<int>(session_.vehicleSeatIndex()),
                 session_.controllingVehicle() ? 1 : 0, session_.canExitVehicle() ? 1 : 0,
                 session_.canSwitchVehicleSeats() ? 1 : 0, session_.vehicleAimAdjustable() ? 1 : 0,
                 session_.bonusBarOffset());
    }
}

// --- The hooks of the movement, the animation and the drawing ----------------------------------------------------------

bool WorldScreen::classBarMovement(float dtSec) {
    (void)dtSec;
    ClassBarUi& c = cbarUi();
    if (session_.moverGuid() != 0) return false;  // we move a unit: the movement code below moves it
    if (session_.inVehicle()) {
        // A passenger: the seat carries us (the server moves us with the vehicle; we send nothing).
        glm::vec3 seat;
        float yaw = 0.0f;
        if (c.seatValid) {
            seat = c.seatPos;
            yaw = c.seatYaw;
        } else if (!c.seatWorld(*this, seat, yaw)) {
            seat = glm::vec3(playerX_, playerY_, playerZ_);
            yaw = facing_;
        }
        playerX_ = seat.x;
        playerY_ = seat.y;
        playerZ_ = seat.z;
        facing_ = yaw;
        wantMove_ = moving_ = false;
        moveFlags_ = 0;
        airborne_ = false;
        vz_ = 0.0f;
        return true;
    }
    c.seatValid = false;
    if (session_.ownControlLost()) {  // the server moves us (charmed, feared...): no input, no packets
        wantMove_ = moving_ = false;
        moveFlags_ = 0;
        return true;
    }
    return false;
}

bool WorldScreen::classBarSeatAnimation() {
    ClassBarUi& c = cbarUi();
    if (c.homeValid) return true;  // possessing: the body keeps its pose (the channel), the possessed unit moves
    if (!session_.inVehicle()) return false;
    const cb::VehicleSeatRec* seat = session_.vehicleSeat();
    uint32_t a = seat && seat->rideAnimLoop != 0xFFFFFFFFu ? seat->rideAnimLoop : anim::MOUNT;
    if (!player_.hasAnimation(a)) a = player_.hasAnimation(anim::MOUNT) ? anim::MOUNT : anim::STAND;
    if (lastAnimRequest_ != a) {
        player_.playAnimation(a, true);
        lastAnimRequest_ = a;
        lastAnimLoop_ = true;
    }
    return true;
}

bool WorldScreen::classBarHidesPlayer() const {
    if (ownModelSwapped()) return true;
    if (!session_.inVehicle()) return false;
    const cb::VehicleSeatRec* seat = session_.vehicleSeat();
    return seat && (seat->flags & cb::kSeatHidePassenger);
}

glm::vec3 WorldScreen::classBarRiderPos() {
    ClassBarUi& c = cbarUi();
    if (c.homeValid) return glm::vec3(c.home.x, c.home.y, c.home.z);
    if (session_.inVehicle()) {
        glm::vec3 p;
        float yaw = 0.0f;
        if (c.seatWorld(*this, p, yaw)) {
            c.seatPos = p;
            c.seatYaw = yaw;
            c.seatValid = true;
            return p;
        }
    }
    return ownRiderPos();
}

float WorldScreen::classBarRiderYaw() const {
    if (!cbar_) return facing_;
    if (cbar_->homeValid) return cbar_->homeFacing;
    if (session_.inVehicle() && cbar_->seatValid) return cbar_->seatYaw;
    return facing_;
}

void WorldScreen::appendClassBarUnits(std::vector<rendering::psp::CreatureUnit>& out) {
    ClassBarUi& c = cbarUi();
    // The unit we move is drawn where the movement code has it (the server does not echo our own mover).
    if (c.driving)
        for (auto& u : out)
            if (u.guid == c.driving) {
                u.x = playerX_;
                u.y = playerY_;
                u.z = shownZ_;
                u.orientation = facing_;
                u.moving = wantMove_;
                u.walking = stickWalk_;
                u.speed = wantMove_ ? session_.serverRunSpeed() : 0.0f;
            }
    auto scaleOf = [](uint32_t bits) {
        float s = 1.0f;
        if (bits) std::memcpy(&s, &bits, sizeof(s));
        return s > 0.01f && s < 100.0f ? s : 1.0f;
    };
    if (ownModelSwapped()) {
        rendering::psp::CreatureUnit u;
        u.guid = session_.playerGuid();
        u.displayId = c.swapDisplay;
        glm::vec3 at = c.homeValid ? c.home : glm::vec3(playerX_, playerY_, shownZ_);
        if (session_.inVehicle() && c.seatValid) at = c.seatPos;
        u.x = at.x;
        u.y = at.y;
        u.z = at.z;
        u.orientation = classBarRiderYaw();
        u.scale = scaleOf(session_.selfField(game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X)));
        u.moving = wantMove_ && !c.homeValid;
        u.driven = true;  // its animation mirrors player_ (updateClassBars)
        out.push_back(u);
    }
    for (const auto& [guid, rs] : c.remote) {
        if (!rs.ready) continue;
        auto it = remotes_.find(guid);
        if (it == remotes_.end() || !it->second.hasCache) continue;
        const RemotePlayer& r = it->second;
        const float dx = r.pos.x - playerX_, dy = r.pos.y - playerY_;
        if (dx * dx + dy * dy > remoteView_ * remoteView_) continue;
        auto e = session_.entities().getEntity(guid);
        rendering::psp::CreatureUnit u;
        u.guid = guid;
        u.displayId = rs.display;
        u.x = r.pos.x;
        u.y = r.pos.y;
        u.z = r.pos.z;
        u.orientation = r.yaw;
        u.moving = r.movingNow;
        u.walking = r.walkingNow;
        if (u.moving && !u.walking) u.speed = session_.unitRunSpeed(guid);
        u.scale = e ? scaleOf(e->getField(game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X))) : 1.0f;
        out.push_back(u);
    }
}

bool WorldScreen::classBarRemoteHidden(uint64_t guid) const {
    if (!cbar_) return false;
    auto it = cbar_->remote.find(guid);
    return it != cbar_->remote.end() && it->second.ready;
}

// A remote passenger of a vehicle we see (its ONTRANSPORT seat): on the seat's attachment.
void WorldScreen::classBarRemoteSeat(uint64_t guid, glm::vec3& at, float& yaw) {
    (void)guid;
    (void)at;
    (void)yaw;
    // PSP: the other passengers are drawn where the server says they are (their world place); the seat attachment
    // is ours only (docs: the PSP differences).
}

bool WorldScreen::classBarInteract(uint64_t guid, uint32_t npcFlags) {
    if (!guid) return false;
    if ((guid >> 48) == 0) {
        // A player: the port's own rule - a member of our party / raid riding a vehicle (a two-seat mount) is
        // boarded with CMSG_PLAYER_VEHICLE_ENTER (AzerothCore VehicleHandler HandleEnterPlayerVehicle takes it only
        // from the same raid and checks the seat and the distance itself); anybody else: not handled here.
        if (guid == session_.playerGuid() || session_.unitVehicleId(guid) == 0 || !session_.partyMember(guid))
            return false;
        session_.enterPlayerVehicle(guid);
        return true;
    }
    if (!(npcFlags & kNpcFlagSpellClick)) return false;
    session_.spellClick(guid);  // UNIT_NPC_FLAG_SPELLCLICK -> CMSG_SPELLCLICK (AzerothCore HandleSpellClick)
    return true;
}

// --- The action bar hooks ----------------------------------------------------------------------------------------------

int WorldScreen::classBarBonusBase() const {
    if (!session_.selfKnown()) return -1;
    const int off = session_.bonusBarOffset();  // GetBonusBarOffset: page NUM_ACTIONBAR_PAGES + offset
    return off > 0 && off <= 5 ? (5 + off) * 12 : 0;
}

bool WorldScreen::classBarHidesMainBar() const {
    return session_.vehicleUi() || session_.bonusBarOffset() == 5;
}

// UseAction on actions 121..132 while the override fills them: the pet's action (CastPetAction - a possessed unit, a
// vehicle) or the spell (a form's preset, OverrideSpellData).
bool WorldScreen::classBarCastSlot(int slot) {
    if (slot < cb::kOverrideFirstSlot || slot >= cb::kOverrideFirstSlot + cb::kOverrideSlots) return false;
    if (session_.bonusBarOffset() != 5) return false;
    const int i = slot - cb::kOverrideFirstSlot;
    const uint32_t data = session_.overrideSlot(i);
    if (!data) return true;
    if (session_.overrideIsPet()) {
        if (i < pp::WorldSession::kPetSlots) session_.castPetAction(i);
    } else {
        session_.castSpell(data, 0, wantMove_ || moving_);
    }
    PSP_LOGI(pp::tag::Ctrl, "override_cast slot=%d data=%08X pet=%d", i, static_cast<unsigned>(data),
             session_.overrideIsPet() ? 1 : 0);
    return true;
}

float WorldScreen::classBarPetShift() const {
    if (!cbar_) return 0.0f;
    ClassBarUi& c = *cbar_;
    // PetActionBar_UpdatePositionValues: the leave button's / the last shapeshift button's / the multicast frame's
    // right + 20 (from MainMenuBar's left), else 36.
    float x = 36.0f;
    if (c.leaveShown) x = 30.0f + c.leaveX(const_cast<WorldScreen&>(*this)) + 32.0f + 20.0f;
    else if (c.formsShown) {
        const int n = static_cast<int>(const_cast<WorldScreen*>(this)->session_.shapeshiftForms().size());
        x = 30.0f + formButtonX(n - 1, n) + 30.0f + 20.0f;
    } else if (c.multiShown) x = 30.0f + 230.0f + 20.0f;
    return std::max(0.0f, (x - 36.0f) * kBarS);
}

float WorldScreen::classBarChatLift() const {
    if (!cbar_ || fullMapOpen()) return 0.0f;
    const ClassBarUi& c = *cbar_;
    return (c.formsShown || c.possessShown || c.multiShown || c.leaveShown) ? std::round(35.0f * kBarS) : 0.0f;
}

// --- Drawing -----------------------------------------------------------------------------------------------------------

void WorldScreen::drawClassBars(UiRenderer& ui) {
    ClassBarUi& c = cbarUi();
    c.tipSpell = 0;
    c.tipText = nullptr;
    if (fxHides(kFxHideBar) || fullMapOpen() || settings_.isOpen()) return;
    PROF_SCOPE("hud.classbars");
    const bool clickable = uiMode_;
    const Font& small = kit_.font(kFontSmall);
    const uint32_t now = pp::logUptimeMs();
    auto focusStop = [&](uint32_t id, const Rect& r) { return clickable && winFocus_.addStop(id, r); };
    auto frameOf = [](const Rect& r, float art, float button) {
        const float fs = r.w * art / button;
        return Rect{r.x + (r.w - fs) * 0.5f, r.y + (r.h - fs) * 0.5f, fs, fs};
    };
    auto spellButton = [&](uint32_t id, const Rect& r, uint32_t spell, bool active, bool castable, float artRatio) {
        const bool focused = focusStop(id, r);
        if (const pp::Texture* ic = spell ? spellDb_.icon(spell, spellFrame_) : nullptr)
            ui.image(*ic, r, castable ? 0xFFFFFFFFu : 0xFF666666u);
        else ui.fill(r, 0xFF303030u);
        if (spell) {
            float rem = session_.spellCooldown(spell), tot = session_.spellCooldownTotal(spell);
            if (session_.gcdRemaining() > rem) {
                rem = session_.gcdRemaining();
                tot = session_.gcdTotal();
            }
            drawCooldown(ui, r, rem, tot);
        }
        if (spellArt_.slot.pixels) ui.image(spellArt_.slot, frameOf(r, artRatio, 1.0f));
        if (active && c.checked.pixels) ui.image(c.checked, r, 0xFFFFFFFFu, Blend::Add);
        if (focused && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
        if (focused && spell) {
            c.tipSpell = spell;
            c.tipRect = r;
        }
        return focused;
    };

    // --- VehicleMenuBar (instead of MainMenuBar) ---
    if (c.vehicleUi) {
        const VehSkin& sk = kSkins[session_.vehicleSkinNatural() ? 0 : 1];
        const bool pitch = session_.vehicleAimAdjustable();
        const float W = pitch ? sk.wPitch : sk.wNoPitch;
        const float x0 = std::floor(240.0f - W * kVehS * 0.5f), y0 = kMmbBottom;
        auto BL = [&](float x, float y, float w, float h) { return Rect{x0 + x * kVehS, y0 - (y + h) * kVehS, w * kVehS, h * kVehS}; };
        auto BR = [&](float xr, float y, float w, float h) { return BL(W + xr - w, y, w, h); };
        // The composite: the skin's textures from 16 units under the bar to 112 above (tools: class_bar_art).
        if (c.vehBar.pixels) ui.image(c.vehBar, Rect{x0, y0 - 112.0f * kVehS, W * kVehS, 128.0f * kVehS});
        // Health / power (the vehicle unit's): vertical bars from the bottom, the tank overlay over them.
        UnitView v;
        const bool haveV = unitView(session_.vehicleGuid(), v);
        auto vbar = [&](const Rect& r, uint32_t cur, uint32_t max, uint32_t color) {
            ui.fill(r, 0x80000000u);
            if (max == 0) return;
            const float f = std::clamp(static_cast<float>(cur) / static_cast<float>(max), 0.0f, 1.0f);
            ui.fill(Rect{r.x, r.y + r.h * (1.0f - f), r.w, r.h * f}, color);
        };
        const Rect hp = BL(sk.hpX, sk.hpY, sk.barW, sk.barH), mp = BR(sk.mpXr, sk.mpY, sk.barW, sk.barH);
        if (haveV) {
            vbar(hp, v.dead ? 0 : v.hp, v.maxHp, 0xFF00FF00u);
            vbar(mp, v.power, v.maxPower, powerColor(v.powerType));
        }
        if (c.vehOverlay.pixels) {
            ui.image(c.vehOverlay, Rect{hp.x + sk.ovlX * kVehS, hp.y + hp.h - (sk.ovlY + sk.ovlH) * kVehS, sk.ovlW * kVehS, sk.ovlH * kVehS});
            ui.image(c.vehOverlay, Rect{mp.x + sk.ovlX * kVehS, mp.y + mp.h - (sk.ovlY + sk.ovlH) * kVehS, sk.ovlW * kVehS, sk.ovlH * kVehS});
        }
        // The six action buttons (VehicleActionButtonTemplate 60 x 60, + 2 apart, ActionButtonFrame's scale).
        const float bs = 60.0f * sk.actScale;
        for (int i = 0; i < 6; ++i) {
            const Rect r = BL(W + sk.actXr + static_cast<float>(i) * (bs + 2.0f * sk.actScale), sk.actY, bs, bs);
            const uint32_t data = session_.overrideSlot(i);
            const bool pet = session_.overrideIsPet();
            const bool focused = focusStop(kIdVehAction0 + static_cast<uint32_t>(i), r);
            if (data) {
                const pp::Texture* ic = nullptr;
                uint32_t spell = 0;
                if (!pet || pa::isSpell(data)) {
                    spell = pet ? pa::id(data) : data;
                    ic = spellDb_.icon(spell, spellFrame_);
                } else if (pa::type(data) == pa::kTypeCommand && pa::id(data) < 4) {
                    ic = spellDb_.iconById(kCommandIcon[pa::id(data)], spellFrame_);
                } else if (pa::type(data) == pa::kTypeReaction && pa::id(data) < 3) {
                    ic = spellDb_.iconById(kReactIcon[pa::id(data)], spellFrame_);
                }
                const bool usable = !pet || session_.petSlotUsable(i);
                if (ic) ui.image(*ic, r, usable ? 0xFFFFFFFFu : 0xFF666666u);
                else ui.fill(r, 0xFF303030u);
                if (spell) {
                    float tot = 0.0f;
                    const float rem = pet ? session_.petSpellCooldown(spell, &tot) : session_.spellCooldown(spell);
                    drawCooldown(ui, r, rem, pet ? tot : session_.spellCooldownTotal(spell));
                    if (focused) {
                        c.tipSpell = spell;
                        c.tipRect = r;
                    }
                }
                if (spellArt_.slot.pixels) ui.image(spellArt_.slot, frameOf(r, 66.0f, 36.0f));
            }
            if (focused && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
            // PSP: the keys (controls=2) as the main bar's glyphs - combat mode: 1-4 face, 5-6 L + face; travel mode: 1-4 L + face.
            if (!clickable && controlsScheme_ >= 2 && data) {
                static constexpr PadGlyph kFace[4] = {kPadSquare, kPadTriangle, kPadCircle, kPadCross};
                const int mod = combatMode_ ? (i < 4 ? 0 : 1) : (i < 4 ? 1 : -1);
                if (mod >= 0) {
                    const Rect g{r.x + r.w - 8.0f, r.y - 3.0f, 10.0f, 10.0f};
                    kit_.glyph(ui, kFace[i % 4], g);
                    if (mod == 1) kit_.glyph(ui, kPadL, Rect{g.x - 7.0f, g.y + 1.0f, 8.0f, 8.0f});
                }
            }
        }
        // The leave button and the pitch buttons / slider.
        if (session_.canExitVehicle()) {
            const Rect lr = BR(sk.leaveXr, sk.leaveY, sk.leaveW, sk.leaveH);
            const bool f = focusStop(kIdVehLeave, lr);
            if (c.vehExit.pixels) ui.image(c.vehExit, lr);
            if (f) {
                if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, lr, 0xFFFFFFFFu, Blend::Add);
                c.tipText = CLIENT_STR(LEAVE_VEHICLE);
                c.tipRect = lr;
            }
            if (!clickable && controlsScheme_ >= 2) {
                kit_.glyph(ui, kPadL, Rect{lr.x + lr.w - 14.0f, lr.y - 3.0f, 8.0f, 8.0f});
                kit_.glyph(ui, kPadDown, Rect{lr.x + lr.w - 7.0f, lr.y - 4.0f, 9.0f, 9.0f});
            }
        }
        if (pitch) {
            const Rect up = BL(sk.pitchX, sk.pitchUpY, sk.pitchW, sk.pitchH), dn = BL(sk.pitchX, sk.pitchDownY, sk.pitchW, sk.pitchH);
            const bool fu = focusStop(kIdVehPitchUp, up), fd = focusStop(kIdVehPitchDown, dn);
            if (c.vehPitchUp.pixels) ui.image(c.vehPitchUp, up);
            if (c.vehPitchDown.pixels) ui.image(c.vehPitchDown, dn);
            if ((fu || fd) && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, fu ? up : dn, 0xFFFFFFFFu, Blend::Add);
            if (fu || fd) {
                c.tipText = fu ? CLIENT_STR(AIM_UP) : CLIENT_STR(AIM_DOWN);
                c.tipRect = fu ? up : dn;
            }
            // VehicleMenuBarPitch_OnUpdate: the marker at the normalized angle. PSP: a red bar.
            const cb::VehicleKit* kit = session_.vehicleKit();
            const float lo = kit ? kit->vehicle.pitchMin : 0.0f, hi = kit ? kit->vehicle.pitchMax : 1.0f;
            const float f = hi > lo ? std::clamp((session_.vehicleAimAngle() - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
            const Rect sl = BL(sk.sliderX, sk.sliderY, sk.sliderW, sk.sliderH);
            ui.fill(Rect{sl.x + 2.0f, sl.y + sl.h * (1.0f - f) - 1.0f, sl.w - 4.0f, 3.0f}, 0xFF0000FFu);
        }
    } else {
        // --- The override bar: BonusActionBarFrame over MainMenuBar (actions 121..132) ---
        if (c.overrideShown) {
            const bool pet = session_.overrideIsPet();
            for (int i = 0; i < 12; ++i) {
                const Rect r = mmbButton(i);
                const uint32_t data = session_.overrideSlot(i);
                const bool focused = focusStop(kIdBonus0 + static_cast<uint32_t>(i), r);
                if (data) {
                    const pp::Texture* ic = nullptr;
                    uint32_t spell = 0;
                    if (!pet || pa::isSpell(data)) {
                        spell = pet ? pa::id(data) : data;
                        ic = spell ? spellDb_.icon(spell, spellFrame_) : nullptr;
                    } else if (pa::type(data) == pa::kTypeCommand && pa::id(data) < 4) {
                        ic = spellDb_.iconById(kCommandIcon[pa::id(data)], spellFrame_);
                    } else if (pa::type(data) == pa::kTypeReaction && pa::id(data) < 3) {
                        ic = spellDb_.iconById(kReactIcon[pa::id(data)], spellFrame_);
                    }
                    const bool usable = !pet || session_.petSlotUsable(i);
                    if (ic) ui.image(*ic, r, usable ? 0xFFFFFFFFu : 0xFF666666u);
                    else if (spell || pet) ui.fill(r, 0xFF303030u);
                    if (spell) {
                        float tot = 0.0f;
                        const float rem = pet ? session_.petSpellCooldown(spell, &tot) : session_.spellCooldown(spell);
                        drawCooldown(ui, r, rem, pet ? tot : session_.spellCooldownTotal(spell));
                        if (focused) {
                            c.tipSpell = spell;
                            c.tipRect = r;
                        }
                    }
                    if (spellArt_.slot.pixels && (ic || spell)) ui.image(spellArt_.slot, frameOf(r, 66.0f, 36.0f));
                    // The pet's attack in effect: checked, flashing red (as the pet bar).
                    if (pet && pa::type(data) == pa::kTypeCommand && pa::id(data) == pa::kAttack && session_.petAttacking() &&
                        spellArt_.red.pixels && ((now / 400u) & 1u) == 0)
                        ui.image(spellArt_.red, frameOf(r, 66.0f, 36.0f), 0xFFFFFFFFu, Blend::Add);
                }
                if (focused && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
                // The keys (as drawActionBar): combat mode 1-4 face, 5-8 L + face, 9-12 R + face; travel mode 1-4 L + face.
                if (!clickable && controlsScheme_ >= 2 && data) {
                    static constexpr PadGlyph kFace[4] = {kPadSquare, kPadTriangle, kPadCircle, kPadCross};
                    const int page = i / 4;
                    const int mod = combatMode_ ? page : (page == 0 ? 1 : -1);
                    if (mod >= 0) {
                        const Rect g{r.x + r.w - 7.0f, r.y - 4.0f, 10.0f, 10.0f};
                        kit_.glyph(ui, kFace[i % 4], g);
                        if (mod > 0) kit_.glyph(ui, mod == 1 ? kPadL : kPadR, Rect{g.x - 7.0f, g.y + 1.0f, 8.0f, 8.0f});
                    }
                }
            }
        }
        // --- PossessBarFrame (PossessButton1: the spell; PossessButton2: cancel) ---
        if (c.possessShown) {
            const bool cancel = session_.possessCanCancel();
            if (c.ssbar.pixels) {
                ui.image(c.ssbar, barRect(0, 0, 47, 38), kSsLeft);
                if (cancel) ui.image(c.ssbar, barRect(47, 0, 43, 38), kSsRight);
            }
            const uint32_t spell = session_.possessSpell();
            spellButton(kIdPossess0, barRect(10, 3, 30, 30), spell, false, true, 64.0f / 30.0f);
            if (cancel) {
                const Rect r = barRect(48, 3, 30, 30);
                const bool f = focusStop(kIdPossess0 + 1, r);
                // GetPossessInfo(2): the possession spell's own icon (upstream lua_system_api.cpp).
                if (const pp::Texture* ic = spell ? spellDb_.icon(spell, spellFrame_) : nullptr) ui.image(*ic, r);
                else ui.fill(r, 0xFF303030u);
                if (spellArt_.slot.pixels) ui.image(spellArt_.slot, frameOf(r, 64.0f, 30.0f));
                if (f) {
                    if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
                    c.tipText = CLIENT_STR(CANCEL);
                    c.tipRect = r;
                }
                if (!clickable && controlsScheme_ >= 2) {
                    kit_.glyph(ui, kPadL, Rect{r.x + r.w - 14.0f, r.y - 3.0f, 8.0f, 8.0f});
                    kit_.glyph(ui, kPadDown, Rect{r.x + r.w - 7.0f, r.y - 4.0f, 9.0f, 9.0f});
                }
            }
        }
        // --- ShapeshiftBarFrame ---
        if (c.formsShown) {
            const std::vector<uint32_t>& forms = session_.shapeshiftForms();
            const int n = std::min(static_cast<int>(forms.size()), 10);
            if (c.ssbar.pixels) {
                ui.image(c.ssbar, barRect(0, 0, 47, 38), kSsLeft);
                float rightX = n == 1 ? 12.0f : 47.0f;
                if (n > 2 && c.ssmid.pixels) {
                    // ShapeshiftBarMiddle: 37 x (n - 2) wide, its texture repeated (TexCoord 0 .. n - 2).
                    for (int k = 0; k < n - 2; ++k)
                        ui.image(c.ssmid, barRect(47.0f + 37.0f * static_cast<float>(k), 0, 37, 38),
                                 SrcRect{0, 0, static_cast<float>(c.ssmid.uw), static_cast<float>(c.ssmid.uh)});
                    rightX = 47.0f + 37.0f * static_cast<float>(n - 2);
                }
                ui.image(c.ssbar, barRect(rightX, 0, 43, 38), kSsRight);
            }
            for (int i = 0; i < n; ++i) {
                const uint32_t spell = forms[static_cast<size_t>(i)];
                const bool active = session_.shapeshiftFormActive(spell);
                const uint8_t use = actionUsable(game::ActionBarSlot{game::ActionBarSlot::SPELL, spell, 0.0f, 0.0f});
                const Rect r = barRect(formButtonX(i, n), 3, 30, 30);
                spellButton(kIdForm0 + static_cast<uint32_t>(i), r, spell, active, active || use == 0, 64.0f / 30.0f);
            }
            if (!clickable && controlsScheme_ >= 2 && n > 1) {  // PSP: R + left / right (the doc's proposal)
                const Rect a = barRect(formButtonX(0, n), 3, 30, 30);
                kit_.glyph(ui, kPadR, Rect{a.x - 9.0f, a.y + 2.0f, 8.0f, 8.0f});
                kit_.glyph(ui, kPadLeft, Rect{a.x - 9.0f, a.y + 9.0f, 8.0f, 8.0f});
            }
        }
        // --- MultiCastActionBarFrame ---
        if (c.multiShown) {
            const bool tb = c.totemBar.pixels != nullptr;
            // The summon button (the page's call) and its flyout button.
            const bool anyCall = session_.multiCastCallKnown(0) || session_.multiCastCallKnown(1) || session_.multiCastCallKnown(2);
            if (anyCall) {
                const Rect r = barRect(3, 3, 30, 30);
                const uint32_t call = cb::kCallSpells[c.callPage];
                const bool f = spellButton(kIdCall, r, call, false, true, 64.0f / 30.0f);
                if (f || c.flyout == -1) {
                    const Rect ar = barRect(4, 33, 28, 18);
                    focusStop(kIdCallArrow, ar);
                    if (tb) ui.image(c.totemBar, ar, kFlyoutUpSummon);
                }
                if (!clickable && controlsScheme_ >= 2) {
                    kit_.glyph(ui, kPadR, Rect{r.x + r.w - 14.0f, r.y - 3.0f, 8.0f, 8.0f});
                    kit_.glyph(ui, kPadRight, Rect{r.x + r.w - 7.0f, r.y - 4.0f, 9.0f, 9.0f});
                }
            }
            const auto& bar = session_.actionBar();
            for (int k = 0; k < c.multiCount; ++k) {
                const int slot = c.multiSlots[k];
                const Rect r = barRect(36.0f + 38.0f * static_cast<float>(k), 3, 30, 30);
                const int action = cb::multiCastAction(c.callPage, slot);
                const game::ActionBarSlot& s = bar[static_cast<size_t>(action)];
                const uint32_t spell = s.type == game::ActionBarSlot::SPELL ? s.id : 0u;
                const bool focused = focusStop(kIdSlot0 + static_cast<uint32_t>(k), r);
                if (spell) {
                    if (const pp::Texture* ic = spellDb_.icon(spell, spellFrame_)) ui.image(*ic, r);
                    drawCooldown(ui, r, session_.spellCooldown(spell), session_.spellCooldownTotal(spell));
                } else if (tb) {
                    ui.image(c.totemBar, r, kSlotEmpty[slot]);
                }
                if (tb) {
                    const float o = 34.0f * kBarS;
                    ui.image(c.totemBar, Rect{r.x + (r.w - o) * 0.5f, r.y + (r.h - o) * 0.5f, o, o}, kSlotOverlay[slot]);
                }
                if (focused && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
                if (focused) {
                    if (spell) c.tipSpell = spell;
                    else c.tipText = CLIENT_STR(MULTI_CAST_TOOLTIP_NO_TOTEM);
                    c.tipRect = r;
                }
                if (focused || c.flyout == slot) {  // MultiCastFlyoutFrameOpenButton over the slot under the cursor
                    const Rect ar = barRect(36.0f + 38.0f * static_cast<float>(k) + 1.0f, 33, 28, 18);
                    focusStop(kIdSlotArrow0 + static_cast<uint32_t>(k), ar);
                    if (tb) ui.image(c.totemBar, ar, kFlyoutUp[slot]);
                }
            }
            if (session_.hasKnownSpell(cb::kRecallSpell)) {
                const Rect r = barRect(36.0f + 38.0f * static_cast<float>(c.multiCount), 3, 30, 30);
                spellButton(kIdRecall, r, cb::kRecallSpell, false, true, 64.0f / 30.0f);
            }
        }
        // --- MainMenuBarVehicleLeaveButton ---
        if (c.leaveShown) {
            const Rect r = barRect(c.leaveX(*this), 1, 32, 32);
            const bool f = focusStop(kIdLeave, r);
            if (c.vehExit.pixels) ui.image(c.vehExit, r);
            else ui.fill(r, 0xFF303030u);
            if (f) {
                if (spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
                c.tipText = CLIENT_STR(LEAVE_VEHICLE);
                c.tipRect = r;
            }
            if (!clickable && controlsScheme_ >= 2) {
                kit_.glyph(ui, kPadL, Rect{r.x + r.w - 14.0f, r.y - 3.0f, 8.0f, 8.0f});
                kit_.glyph(ui, kPadDown, Rect{r.x + r.w - 7.0f, r.y - 4.0f, 9.0f, 9.0f});
            }
        }
    }
    // --- VehicleSeatIndicator (TOPRIGHT of MinimapCluster's bottom: x 416..480 under the minimap disc) ---
    if (session_.inVehicle() && c.seatTried) {
        const cb::VehicleKit* kit = session_.vehicleKit();
        const Rect box{480.0f - 64.0f - 2.0f, 126.0f, 64.0f, 64.0f};
        if (c.seatBg.pixels) ui.image(c.seatBg, box);
        if (kit) {
            for (const cb::IndicatorSeatRec& s : kit->indicatorSeats) {
                const int seat = static_cast<int>(s.seatIndex) - 1;
                const float bw = 25.0f * kBarS;
                const Rect r{box.x + s.x * 64.0f - bw * 0.5f, box.y + s.y * 64.0f - bw * 0.5f, bw, bw};
                const bool f = focusStop(kIdVehSeat0 + static_cast<uint32_t>(seat & 7), r);
                if (c.seats.pixels) ui.image(c.seats, r, kSeatBg);
                const uint64_t who = session_.vehicleSeatOccupant(seat);
                const float d = 10.0f * kBarS;
                const Rect dot{r.x + (r.w - d) * 0.5f, r.y + (r.h - d) * 0.5f, d, d};
                if (who && c.seats.pixels) ui.image(c.seats, dot, who == session_.playerGuid() ? kSeatPlayer : kSeatAlly);
                if (f && c.seats.pixels) ui.image(c.seats, dot, kSeatPlayer, 0x80FFFFFFu, Blend::Add);
            }
        }
    }
    (void)small;
}

// TotemFrame (TotemFrame_Update): the totems in TOTEM_PRIORITIES order, compacted, under PlayerFrame.
void WorldScreen::drawTotemFrame(UiRenderer& ui) {
    if (!cbar_ || settings_.isOpen()) return;
    ClassBarUi& c = *cbar_;
    if (!c.totemFrameTried) return;
    const uint8_t cls = session_.getPlayerClass();
    const bool petShown = session_.petUnit() != nullptr;
    float fx = 2.0f + 99.0f * kUf, fy = 2.0f + (100.0f - 38.0f) * kUf;  // PlayerFrame BOTTOMLEFT + 99, 38
    if (cls == kClassDeathKnight) {
        fx = 2.0f + (petShown ? 28.0f : 65.0f) * kUf;
        fy = 2.0f + (petShown ? 75.0f : 55.0f) * kUf;
    } else if (petShown && cls != kClassShaman) {
        return;
    }
    const Font& small = kit_.font(kFontSmall);
    const bool clickable = uiMode_;
    int k = 0;
    for (int i = 0; i < cb::kTotemSlots; ++i) {
        const int slot = cb::kTotemPriority[i];
        const pp::WorldSession::TotemSlotState& t = session_.totemSlot(slot);
        if (!t.active() || !t.durationMs) continue;
        const float bx = fx + 33.0f * kUf * static_cast<float>(k), bs = 37.0f * kUf;
        const Rect b{bx, fy, bs, bs};
        ++k;
        const bool f = clickable && winFocus_.addStop(kIdTotem0 + static_cast<uint32_t>(slot), b);
        auto centered = [&](float units) { const float s = units * kUf; return Rect{b.x + (b.w - s) * 0.5f, b.y + (b.h - s) * 0.5f, s, s}; };
        if (c.totemBg.pixels) ui.image(c.totemBg, centered(34));
        const Rect icon = centered(22);
        if (const pp::Texture* ic = spellDb_.icon(t.spellId, spellFrame_)) ui.image(*ic, icon);
        // CooldownFrame_SetTimer(start, duration) on a reverse Cooldown: the elapsed part dark.
        const uint32_t left = session_.totemTimeLeftMs(slot);
        drawCooldown(ui, icon, static_cast<float>(t.durationMs - std::min(left, t.durationMs)) / 1000.0f,
                     static_cast<float>(t.durationMs) / 1000.0f);
        if (c.totemBorder.pixels) ui.image(c.totemBorder, centered(38));
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, icon, 0xFFFFFFFFu, Blend::Add);
        if (f) {
            c.tipSpell = t.spellId;
            c.tipRect = b;
        }
        char buf[24];
        timeAbbrev(buf, sizeof(buf), left);
        ui.textShadow(small, b.x + b.w * 0.5f, b.y + b.h - 1.0f, buf, kColGold, Align::Center);
    }
}

// MultiCastFlyoutFrame: the slot's totems (a "none" first) or the known calls, up from the button.
bool WorldScreen::drawClassBarWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinTotemFlyout) return false;
    ClassBarUi& c = cbarUi();
    if (c.flyout == -2 || !c.multiShown) return true;
    std::vector<uint32_t> entries;
    if (c.flyout == -1) {
        for (int p = 0; p < 3; ++p)
            if (session_.multiCastCallKnown(p)) entries.push_back(cb::kCallSpells[p]);
    } else {
        entries.push_back(0);  // the slot emptied (SetMultiCastSpell(action, nil))
        for (uint32_t s : session_.multiCastSpells(c.flyout)) entries.push_back(s);
    }
    const float bs = 24.0f * kBarS + 2.0f, gap = 2.0f * kBarS;
    const float n = static_cast<float>(entries.size());
    const float colW = 32.0f * kBarS + 2.0f;
    const Rect panel{c.flyoutAnchor.x + (c.flyoutAnchor.w - colW) * 0.5f, c.flyoutAnchor.y - 4.0f - n * (bs + gap) - 9.0f,
                     colW, n * (bs + gap) + 9.0f};
    ui.fill(panel, 0xC0000000u);  // PSP: a plain panel (MultiCastFlyoutFrame's top / middle textures)
    const bool f0 = winFocus_.addStop(kIdFlyoutClose, Rect{panel.x, panel.y + panel.h - 9.0f, panel.w, 9.0f});
    if (c.totemBar.pixels)
        ui.image(c.totemBar, Rect{panel.x + 1.0f, panel.y + panel.h - 9.0f, panel.w - 2.0f, 9.0f}, SrcRect{72, 69, 20, 10},
                 f0 ? 0xFFFFFFFFu : 0xC0FFFFFFu);
    for (size_t i = 0; i < entries.size(); ++i) {
        const Rect r{panel.x + (panel.w - bs) * 0.5f, panel.y + panel.h - 9.0f - (static_cast<float>(i) + 1.0f) * (bs + gap), bs, bs};
        const bool f = winFocus_.addStop(kIdFlyout0 + static_cast<uint32_t>(i), r);
        if (entries[i]) {
            if (const pp::Texture* ic = spellDb_.icon(entries[i], spellFrame_)) ui.image(*ic, r);
            if (f) {
                c.tipSpell = entries[i];
                c.tipRect = r;
            }
        } else if (c.totemBar.pixels && c.flyout >= 0) {
            ui.image(c.totemBar, r, kSlotEmpty[c.flyout]);
        }
        if (f && spellArt_.hilite.pixels) ui.image(spellArt_.hilite, r, 0xFFFFFFFFu, Blend::Add);
    }
    return true;
}

// After the windows: a bar button's GameTooltip (a spell: the spell tooltip; else one line).
void WorldScreen::drawClassBarOverlay(UiRenderer& ui) {
    if (!cbar_ || !uiMode_ || settings_.isOpen()) return;
    ClassBarUi& c = *cbar_;
    if (c.tipSpell) {
        tipSpellId_ = c.tipSpell;  // drawSpellTooltips (next) draws it
        tipSpellRect_ = c.tipRect;
    } else if (c.tipText) {
        kit_.tooltip(ui, c.tipRect, c.tipText, nullptr);
    }
}

// --- Clicks and keys -----------------------------------------------------------------------------------------------------

bool WorldScreen::classBarWindowAction(uint32_t act) {
    if (act < 0x8D00 || act > 0x8DFF) return false;
    ClassBarUi& c = cbarUi();
    const std::vector<uint32_t>& forms = session_.shapeshiftForms();
    auto openFlyout = [&](int which, const Rect& anchor) {  // MultiCastFlyoutFrame_ToggleFlyout
        if (windowOpen(kWinTotemFlyout) && c.flyout == which) {
            closeWindow(kWinTotemFlyout);
            c.flyout = -2;
            return;
        }
        c.flyout = which;
        c.flyoutAnchor = anchor;
        if (!windowOpen(kWinTotemFlyout)) openWindow(kWinTotemFlyout);
        winFocus_.setFocus(kIdFlyout0);
    };
    if (act >= kIdForm0 && act < kIdForm0 + 10) {  // ShapeshiftBar_ChangeForm -> CastShapeshiftForm
        const int i = static_cast<int>(act - kIdForm0);
        if (i < static_cast<int>(forms.size())) session_.castShapeshiftForm(i);
        return true;
    }
    if (act == kIdPossess0 + 1) {
        session_.cancelPossess();
        return true;
    }
    if (act == kIdLeave || act == kIdVehLeave) {
        session_.vehicleExit();
        return true;
    }
    if (act == kIdVehPitchUp || act == kIdVehPitchDown) {  // VehicleAimUpStart / DownStart: a step per press (PSP)
        session_.vehicleAim(act == kIdVehPitchUp ? 0.05f : -0.05f);
        return true;
    }
    if (act >= kIdVehAction0 && act < kIdVehAction0 + 6) {
        castFromSlot(cb::kOverrideFirstSlot + static_cast<int>(act - kIdVehAction0));
        return true;
    }
    if (act >= kIdBonus0 && act < kIdBonus0 + 12) {
        castFromSlot(cb::kOverrideFirstSlot + static_cast<int>(act - kIdBonus0));
        return true;
    }
    if (act >= kIdVehSeat0 && act < kIdVehSeat0 + 8) {  // VehicleSeatIndicatorButton_OnClick (LeftButton)
        session_.vehicleSwitchSeat(static_cast<int>(act - kIdVehSeat0));
        return true;
    }
    if (act == kIdCall) {  // MultiCastSummonSpellButton_OnClick: CastSpellByID(the page's call)
        session_.castSpell(cb::kCallSpells[c.callPage], 0, wantMove_ || moving_);
        return true;
    }
    if (act == kIdCallArrow) {
        openFlyout(-1, barRect(3, 3, 30, 30));
        return true;
    }
    if (act == kIdRecall) {
        session_.castSpell(cb::kRecallSpell, 0, wantMove_ || moving_);
        return true;
    }
    if (act >= kIdSlot0 && act < kIdSlot0 + 4) {  // MultiCastActionButton: UseAction (the slot's totem of the page)
        const int k = static_cast<int>(act - kIdSlot0);
        if (k < c.multiCount) {
            const int action = cb::multiCastAction(c.callPage, c.multiSlots[k]);
            const game::ActionBarSlot& s = session_.actionBar()[static_cast<size_t>(action)];
            if (s.type == game::ActionBarSlot::SPELL && s.id) session_.castSpell(s.id, 0, wantMove_ || moving_);
            else openFlyout(c.multiSlots[k], barRect(36.0f + 38.0f * static_cast<float>(k), 3, 30, 30));
        }
        return true;
    }
    if (act >= kIdSlotArrow0 && act < kIdSlotArrow0 + 4) {
        const int k = static_cast<int>(act - kIdSlotArrow0);
        if (k < c.multiCount) openFlyout(c.multiSlots[k], barRect(36.0f + 38.0f * static_cast<float>(k), 3, 30, 30));
        return true;
    }
    if (act == kIdFlyoutClose) {
        closeWindow(kWinTotemFlyout);
        c.flyout = -2;
        return true;
    }
    if (act >= kIdFlyout0 && act < kIdFlyout0 + 16) {  // MultiCastFlyoutButton_OnClick
        const int i = static_cast<int>(act - kIdFlyout0);
        if (c.flyout == -1) {  // ChangeMultiCastActionPage
            int seen = 0;
            for (int p = 0; p < 3; ++p)
                if (session_.multiCastCallKnown(p) && seen++ == i) c.callPage = p;
        } else if (c.flyout >= 0) {
            const std::vector<uint32_t>& spells = session_.multiCastSpells(c.flyout);
            const uint32_t spell = i == 0 ? 0u : (i - 1 < static_cast<int>(spells.size()) ? spells[static_cast<size_t>(i - 1)] : 0u);
            session_.setMultiCastSpell(cb::multiCastAction(c.callPage, c.flyout), spell);
        }
        closeWindow(kWinTotemFlyout);
        c.flyout = -2;
        return true;
    }
    return true;  // the tooltip-only parts
}

// Circle = the right click: DestroyTotem (TotemButton_OnClick), a slot's flyout, the seat indicator's eject
// (VehicleSeatIndicatorButton_OnClick RightButton: the driver's EJECT_PASSENGER).
bool WorldScreen::classBarAltAction(uint32_t alt) {
    if (alt < 0x8D00 || alt > 0x8DFF) return false;
    if (alt >= kIdTotem0 && alt < kIdTotem0 + 4) {
        session_.destroyTotem(static_cast<int>(alt - kIdTotem0));
        return true;
    }
    if (alt >= kIdSlot0 && alt < kIdSlot0 + 4) return classBarWindowAction(kIdSlotArrow0 + (alt - kIdSlot0));
    if (alt == kIdCall) return classBarWindowAction(kIdCallArrow);
    if (alt >= kIdVehSeat0 && alt < kIdVehSeat0 + 8) {
        const uint64_t who = session_.vehicleSeatOccupant(static_cast<int>(alt - kIdVehSeat0));
        if (who && who != session_.playerGuid() && session_.controllingVehicle()) session_.ejectPassenger(who);
        return true;
    }
    return classBarWindowAction(alt);
}

// controls=2 (controls - a proposal): L + down = leave the vehicle / cancel the possession;
// L + left / right = the previous / next seat; R + left / right = the previous / next form of the shapeshift bar, a
// shaman's multicast bar: R + right casts the page's call, R + left turns the page; R + up / down (held) = the vehicle's
// aim while it can be adjusted (the zoom is not touched then - ui_world_controls.cpp asks classBarAimKeys... see below).
void WorldScreen::classBarKeys(uint32_t live, bool lHeld, bool rHeld) {
    if (!cbar_) return;
    ClassBarUi& c = *cbar_;
    const bool inVeh = session_.inVehicle();
    if (lHeld && !rHeld) {
        if (live & PSP_CTRL_DOWN) {
            if (inVeh && session_.canExitVehicle()) session_.vehicleExit();
            else if (c.possessShown) session_.cancelPossess();
        }
        if (inVeh && (live & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) session_.vehicleNextSeat((live & PSP_CTRL_RIGHT) != 0);
    }
    if (rHeld && !lHeld) {
        if (c.formsShown && (live & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
            const std::vector<uint32_t>& forms = session_.shapeshiftForms();
            const int n = static_cast<int>(forms.size());
            int cur = -1;
            for (int i = 0; i < n; ++i)
                if (session_.shapeshiftFormActive(forms[static_cast<size_t>(i)])) cur = i;
            const int step = (live & PSP_CTRL_RIGHT) ? 1 : -1;
            const int next = cur < 0 ? (step > 0 ? 0 : n - 1) : (cur + step + n) % n;
            if (n > 0 && next != cur) session_.castShapeshiftForm(next);
            PSP_LOGI(pp::tag::Ctrl, "form_key cur=%d next=%d", cur, next);
        } else if (c.multiShown) {
            if (live & PSP_CTRL_RIGHT) session_.castSpell(cb::kCallSpells[c.callPage], 0, wantMove_ || moving_);
            if (live & PSP_CTRL_LEFT)
                for (int k = 1; k <= 3; ++k) {
                    const int p = (c.callPage + k) % 3;
                    if (session_.multiCastCallKnown(p)) {
                        c.callPage = p;
                        break;
                    }
                }
        }
        if (inVeh && session_.vehicleAimAdjustable()) {
            if (lastRawHeld_ & PSP_CTRL_UP) session_.vehicleAim(lastDt_ * 0.6f);
            if (lastRawHeld_ & PSP_CTRL_DOWN) session_.vehicleAim(-lastDt_ * 0.6f);
        }
    }
}

}  // namespace wowee::ui::psp
