// Weapons sheathed out of combat, drawn in combat.
//
// Own player, upstream Application::update "auto-unsheathe": while
// auto-attacking or meaning to (hasAutoAttackIntent) with a weapon that can
// be drawn (main hand, an off-hand weapon, bow/gun/thrown), sheathed weapons
// come out with the reach animation (AnimationController::
// playWeaponSheathAnimation: HIP_SHEATHE for a hip item, SHEATHE for the back,
// the ActivityFSM owns the one-shot) and loadEquippedWeapons re-attaches them
// (CharacterModel::setWeaponsSheathed).
// PSP deviations (a design decision):
//   - the character enters the world sheathed (upstream: drawn,
//     setWeaponsSheathed(false) at login) and puts the weapons away again
//     after sheathdelay seconds without combat or attack intent (upstream:
//     only the Z key does; the PSP has no key for it). wowee.cfg
//     sheathauto=0 = upstream (drawn at login, never sheathed by itself);
//   - other players follow their UNIT_FIELD_BYTES_2 sheath state (byte 0:
//     0 = sheathed, 1 = melee drawn, 2 = ranged drawn; AzerothCore SheathState,
//     Unit::SetSheath); upstream always draws them in hand. sheathremote=0 =
//     upstream. The field is not in upstream's update-field table: in
//     3.3.5a UNIT_FIELD_BYTES_2 = OBJECT_END + 0x74 = 122.
// The reach animations (89/90) are only played when the baked model has them
// (psp_char_conv --anims ... 89,90); without them the weapons just move.
//
// The own model uses the same three sheath states as the other players (one display path, the port's own rule) -
// updateOwnSheathState() turns the mode's / the automatic wish (ownSheathAway_) into the state the model shows and
// the server hears:
//   - 0 put away / 1 melee drawn: the wish; mounting puts drawn weapons away (upstream: "Mounting stows drawn
//     weapons, matching the original client"; PSP: the mode goes to travel with them - controls=2);
//   - 2 ranged drawn: a while after each own ranged auto-attack shot (Auto Shot / Shoot / Throw; upstream
//     meleeSwingCallback -> showRangedWeapon: the ranged weapon in hand), a melee swing ends it;
//   - fishing: the pole (the main hand's item) in hand for the channel (upstream: unsheathe + showFishingPole);
//   - casting otherwise changes nothing (upstream: spells are cast with the weapons as they are);
//   - CMSG_SET_SHEATHED on every change of the shown state, whatever moved it (the port's own rule: the server
//     hears exactly what the own model shows; wowee.cfg sheathsend, default 1; upstream sends nothing). AzerothCore
//     WorldSession::HandleSetSheathedOpcode takes 0..2 and sets UNIT_FIELD_BYTES_2 byte 0, which the other players
//     read;
//   - other players: setSheathState(byte 0) - the ranged state shows their ranged weapon in hand.
// The reach animation by where the item goes: Item.dbc SheatheType 3 (hip) -> HIP_SHEATHE, 1 / 2 / 4 (back) ->
// SHEATHE (upstream's choice by inventory type when the type is not known).
#include "ui/psp/ui_world_screen.hpp"

#include "game/inventory.hpp"
#include "game/spell_classification.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "rendering/animation/animation_ids.hpp"

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace anim = wowee::rendering::anim;
using Life = pp::WorldSession::Life;

namespace {

constexpr uint16_t kUnitFieldBytes2 = 122;  // PSP: 3.3.5a UpdateFields (see the file comment)
constexpr uint8_t kSheathStateUnarmed = 0;  // SHEATH_STATE_UNARMED: weapons put away
constexpr uint32_t kRangedHoldMs = 3500;    // in hand after a shot (Auto Shot: ~2-3 s apart)

// upstream AppearanceComposer::sheathSpot over weaponAttachment(true, ...):
// one-hand / main-hand -> hip, two-hand and shields -> back, else none.
enum class Spot : uint8_t { None, Hip, Back };
Spot sheathSpot(uint8_t invType) {
    switch (invType) {
        case game::InvType::ONE_HAND:
        case game::InvType::MAIN_HAND: return Spot::Hip;
        case game::InvType::TWO_HAND:
        case game::InvType::SHIELD: return Spot::Back;
        default: return Spot::None;
    }
}

// where it is put away - the Item.dbc SheatheType when known (3 hip, 1 / 2 / 4 back,
// else not shown put away), else upstream's inventory-type spot.
Spot sheathSpotOf(uint8_t sheathType, uint8_t invType) {
    switch (sheathType) {
        case rendering::psp::kSheathUnknown: return sheathSpot(invType);
        case rendering::psp::kSheathTypeOneHand: return Spot::Hip;
        case rendering::psp::kSheathTypeTwoHand:
        case rendering::psp::kSheathTypeStaff:
        case rendering::psp::kSheathTypeShield: return Spot::Back;
        default: return Spot::None;
    }
}

} // namespace

// What the own character holds. The model's equipment (what we wear now - the inventory
// re-dresses it), SMSG_CHAR_ENUM before the model is there.
void WorldScreen::ownWeaponTypes(uint8_t& mainInv, uint8_t& offInv, uint8_t& rangedInv) const {
    mainInv = offInv = rangedInv = 0;
    if (player_.loaded()) {
        const rendering::psp::Equipment& eq = player_.equipment();
        mainInv = eq.displayId[15] ? eq.invType[15] : 0;
        offInv = eq.displayId[16] ? eq.invType[16] : 0;
        rangedInv = eq.displayId[17] ? eq.invType[17] : 0;
        return;
    }
    for (const auto& c : session_.characters()) {
        if (c.name != name_) continue;
        auto inv = [&](size_t slot) {
            return c.equipment.size() > slot && !c.equipment[slot].isEmpty() ? c.equipment[slot].inventoryType
                                                                              : static_cast<uint8_t>(0);
        };
        mainInv = inv(15);
        offInv = inv(16);
        rangedInv = inv(17);
    }
}

bool WorldScreen::ownOffHandShield() const {
    uint8_t m = 0, o = 0, r = 0;
    ownWeaponTypes(m, o, r);
    return o == game::InvType::SHIELD;
}

// Controls (ui_world_controls.cpp, controls=2): the mode draws / puts away the
// weapons with the same reach as the automatic sheath below.
void WorldScreen::drawOwnWeapons(bool draw) {
    if (!player_.loaded() || ownSheathAway_ == !draw) return;  // the wish, not the model
    uint8_t mainInv = 0, offInv = 0, rangedInv = 0;
    ownWeaponTypes(mainInv, offInv, rangedInv);
    const rendering::psp::Equipment& eq = player_.equipment();
    Spot body = sheathSpotOf(eq.sheath[0], mainInv);
    if (body == Spot::None) body = sheathSpotOf(eq.sheath[1], offInv);
    uint32_t id = 0;
    // no reach on a mount (upstream: mounting stows them without one).
    if (body != Spot::None && player_.animated() && session_.life() == Life::Alive && !ownMountDisplay_) {
        id = body == Spot::Hip ? anim::HIP_SHEATHE : anim::SHEATHE;
        if (!player_.hasAnimation(id)) {
            id = body == Spot::Hip ? anim::SHEATHE : anim::HIP_SHEATHE;
            if (!player_.hasAnimation(id)) id = 0;
        }
        if (id) playOwnEmote(id, false);
    }
    ownSheathAway_ = !draw;  // the model follows in updateOwnSheathState
    updateOwnSheathState();
    if (++sheathLogs_ <= 60)
        PSP_LOGI(pp::tag::Gfx, "sheath state=%d src=mode anim=%u", draw ? 0 : 1, static_cast<unsigned>(id));
}

void WorldScreen::updateSheath(float dtSec) {
    if (!player_.loaded()) return;
    uint8_t mainInv = 0, offInv = 0, rangedInv = 0;
    ownWeaponTypes(mainInv, offInv, rangedInv);
    const bool hasOffHandWeapon = offInv != 0 && game::isOffHandWeaponInventoryType(offInv);
    const bool hasRangedWeapon = rangedInv == game::InvType::RANGED_BOW || rangedInv == game::InvType::RANGED_GUN ||
                                 rangedInv == game::InvType::THROWN;
    const bool hasDrawableWeapon = mainInv != 0 || hasOffHandWeapon || hasRangedWeapon;
    const rendering::psp::Equipment& eq = player_.equipment();

    // upstream playWeaponSheathAnimation: one reach both ways, by the item
    // on the body (the main hand's, else the off hand's); a model without
    // it just moves the weapons.
    auto reach = [&]() -> uint32_t {
        Spot body = sheathSpotOf(eq.sheath[0], mainInv);  // the SheatheType spot
        if (body == Spot::None) body = sheathSpotOf(eq.sheath[1], offInv);
        if (body == Spot::None || !player_.animated() || session_.life() != Life::Alive) return 0;
        uint32_t id = body == Spot::Hip ? anim::HIP_SHEATHE : anim::SHEATHE;
        if (!player_.hasAnimation(id)) {
            id = body == Spot::Hip ? anim::SHEATHE : anim::HIP_SHEATHE;
            if (!player_.hasAnimation(id)) return 0;
        }
        playOwnEmote(id, false);  // the ActivityFSM owns the one-shot, like upstream
        return id;
    };
    auto log = [&](const char* src, uint32_t animId) {
        if (++sheathLogs_ <= 60)
            PSP_LOGI(pp::tag::Gfx, "sheath state=%d src=%s anim=%u weapon=%d", ownSheathAway_ ? 1 : 0, src,
                     static_cast<unsigned>(animId), hasDrawableWeapon ? 1 : 0);
    };

    if (!sheathInit_) {
        sheathInit_ = true;
        ownSheathAway_ = sheathAuto_;  // upstream: false at login
        sheathSend_ = pp::configGetInt("sheathsend", 1) != 0;
        log("login", 0);
    }
    const bool attackWanted = session_.autoAttackRequested() || session_.autoAttacking();
    const bool fighting = attackWanted || session_.inCombat();
    calmSec_ = fighting ? 0.0f : calmSec_ + dtSec;
    // controls=2: the mode (ui_world_controls.cpp) draws and sheathes the own weapons.
    if (session_.life() == Life::Alive && controlsScheme_ < 2) {
        if (attackWanted && hasDrawableWeapon && ownSheathAway_) {
            const uint32_t id = reach();
            ownSheathAway_ = false;
            log("attack", id);
        } else if (sheathAuto_ && !fighting && calmSec_ >= sheathDelay_ && !ownSheathAway_) {
            const uint32_t id = hasDrawableWeapon ? reach() : 0u;
            ownSheathAway_ = true;
            log("calm", id);
        }
    }
    updateOwnSheathState();  // mount, ranged shots, fishing; the model and the server

    // Other players: their sheath state field (PSP, see the file comment).
    if (!sheathRemote_) return;
    for (auto& [guid, r] : remotes_) {
        if (!r.model) continue;
        auto e = session_.entities().getEntity(guid);
        if (!e) continue;
        // all three states (2 = the ranged weapon in hand; AzerothCore Player::SetSheath shows the ranged item).
        const uint8_t state = static_cast<uint8_t>(e->getField(kUnitFieldBytes2) & 0xFFu);
        const bool sheathed = state == kSheathStateUnarmed;
        RemoteSheath& rs = remoteSheath_[guid];
        if (rs.model == r.model.get() && rs.state == state) continue;
        rs.model = r.model.get();
        rs.sheathed = sheathed;
        rs.state = state;
        r.model->setSheathState(state);
        if (++sheathLogs_ <= 60)
            PSP_LOGI(pp::tag::Gfx, "sheath remote guid=%08X state=%u", static_cast<unsigned>(guid & 0xFFFFFFFFu),
                     static_cast<unsigned>(state));
    }
    for (auto it = remoteSheath_.begin(); it != remoteSheath_.end();)
        it = remotes_.count(it->first) ? std::next(it) : remoteSheath_.erase(it);
}

// the state the own model shows (see the file comment) and the server hears.
void WorldScreen::updateOwnSheathState() {
    if (!player_.loaded()) return;
    const uint32_t nowMs = pp::logUptimeMs();
    const bool alive = session_.life() == Life::Alive;
    // Mounting puts drawn weapons away, once (drawing them again on the mount by hand is allowed, as upstream).
    if (ownMountDisplay_ != ownMountSeen_) {
        ownMountSeen_ = ownMountDisplay_;
        if (ownMountDisplay_ && !ownSheathAway_) {
            if (controlsScheme_ >= 2 && combatMode_) {
                setCombatMode(false, "mount");  // -> drawOwnWeapons(false) -> here again
            } else {
                ownSheathAway_ = true;
                if (++sheathLogs_ <= 60) PSP_LOGI(pp::tag::Gfx, "sheath state=1 src=mount anim=0");
            }
        }
    }
    // Ranged shots: the ranged weapon in hand a while after each; a melee swing (auto-attack) ends it.
    if (session_.ownRangedShots() != ownRangedSeen_) {
        ownRangedSeen_ = session_.ownRangedShots();
        ownRangedUntilMs_ = nowMs + kRangedHoldMs;
    }
    if (session_.autoAttacking()) ownRangedUntilMs_ = 0;
    const bool ranged = alive && !ownMountDisplay_ && player_.equipment().displayId[17] && nowMs < ownRangedUntilMs_;
    const bool fishing =
        alive && session_.isChanneling() && game::spellclass::isFishingCast(session_.currentCastSpellId());
    uint8_t state = ownSheathAway_ ? rendering::psp::kSheathStateUnarmed : rendering::psp::kSheathStateMelee;
    if (ranged) state = rendering::psp::kSheathStateRanged;
    if (fishing) state = rendering::psp::kSheathStateMelee;
    if (state != ownSheathShown_) {
        ownSheathShown_ = state;
        player_.setSheathState(state);
        if (++sheathLogs_ <= 60)
            PSP_LOGI(pp::tag::Gfx, "sheath own state=%u ranged=%d fishing=%d mounted=%d", static_cast<unsigned>(state),
                     ranged ? 1 : 0, fishing ? 1 : 0, ownMountDisplay_ ? 1 : 0);
    }
    if (sheathSend_ && state != ownSheathSent_ && session_.state() == pp::WorldSession::State::InWorld) {
        ownSheathSent_ = state;
        session_.sendSetSheathed(state);
    }
}

} // namespace wowee::ui::psp
