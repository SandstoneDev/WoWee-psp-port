// The world screen half of the weapons as the original 3.3.5a client shows them
// (the placement: psp_weapon_placement.cpp equipmentAttachmentsBySheath, the sheath
// states: ui_world_sheath.cpp, the glows: src/rendering/psp/psp_weapon_fx.cpp).
//
// Every player's weapon slots get what the client knows of them from the player's visible items
// (PLAYER_VISIBLE_ITEM_n_ENTRYID / _ENCHANTMENT, stride 2 from field 283 in 3.3.5a):
//  - the item's Item.dbc SheatheType (items.pak's "u_" table, ItemBank::itemDisplay) - where it is put away;
//  - the glow its enchantments put on it: the temporary enchantment's ItemVisual when it has one set, else the
//    permanent one's (upstream AppearanceComposer::applyEnchantVisuals: the temporary one masks the permanent one)
//    through items.pak's "ivis" (SpellItemEnchantment -> ItemVisual). The own player the same way: the server writes
//    the same enchantment ids into the own player's visible-item fields (AzerothCore Player::SetVisibleItemSlot);
//    SMSG_CHAR_ENUM's ItemVisual until our fields come.
// The glows of the weapons drawn this frame (CharacterModel::weaponGlows): drawWeaponFx, after the characters.
// wowee.cfg: enchantfx (1; 0 = no glows, nothing allocated), enchantsprites, enchantfxinst, enchantfxdist,
// enchantfxkb, enchantfxpx (psp_weapon_fx.hpp); sheathclient, rangedback (ui_world_screen.cpp applySettings);
// sheathsend (ui_world_sheath.cpp).
#include "ui/psp/ui_world_screen.hpp"

#include "game/entity.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_system.hpp"
#include "rendering/psp/psp_weapon_fx.hpp"

#include <algorithm>
#include <new>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;
namespace rp = wowee::rendering::psp;

namespace {
constexpr uint16_t kVisibleItem1 = 283;  // PLAYER_VISIBLE_ITEM_1_ENTRYID (3.3.5a), _ENCHANTMENT + 1, stride 2
constexpr size_t kMaxGlowRequests = 32;  // weapons with a glow handed over a frame (~140 B each, reserved once)
}  // namespace

struct WorldScreen::WeaponFxUi {
    rp::WeaponGlows glows;
    std::vector<rp::WeaponGlows::Request> requests;  // reserved at open: no allocation in the frame
};

void WorldScreen::destroyWeaponFxUi(WeaponFxUi* p) { delete p; }

bool WorldScreen::weaponFxEnsure() {
    if (weaponFxState_ != 0) return weaponFxState_ > 0;
    if (!items_.isOpen()) return false;  // asked again once items.pak is open
    if (pp::configGetInt("enchantfx", 1) == 0) {
        weaponFxState_ = -1;
        PSP_LOGI(pp::tag::Gfx, "weaponfx off (wowee.cfg enchantfx=0)");
        return false;
    }
    auto* ui = new (std::nothrow) WeaponFxUi;
    if (!ui) {
        weaponFxState_ = -1;
        return false;
    }
    rp::WeaponFxConfig cfg;
    cfg.maxInstances = pp::configGetInt("enchantfxinst", 16);
    cfg.distance = static_cast<float>(pp::configGetInt("enchantfxdist", 30));
    cfg.cacheBytes = static_cast<uint32_t>(std::clamp(pp::configGetInt("enchantfxkb", 64), 16, 1024)) * 1024u;
    cfg.maxPx = pp::configGetInt("enchantfxpx", 32);
    if (!ui->glows.open(cfg, items_)) {
        delete ui;
        weaponFxState_ = -1;
        return false;
    }
    ui->requests.reserve(kMaxGlowRequests);
    weaponFx_ = std::unique_ptr<WeaponFxUi, void (*)(WeaponFxUi*)>(ui, &WorldScreen::destroyWeaponFxUi);
    weaponFxState_ = 1;
    pp::logMemory("weaponfx_open");
    return true;
}

void WorldScreen::fillWeaponVisuals(rp::Equipment& eq, const uint32_t entry[3], const uint32_t enchant[3]) {
    const bool glows = weaponFxEnsure();
    for (int k = 0; k < 3; ++k) {
        eq.sheath[k] = rp::kSheathUnknown;
        eq.enchantVisual[k] = 0;
        if (!eq.displayId[15 + k] || !entry[k]) continue;
        uint32_t display = 0;
        uint8_t invType = 0, sheath = rp::kSheathUnknown;
        if (items_.isOpen() && items_.itemDisplay(entry[k], display, invType, sheath)) eq.sheath[k] = sheath;
        // The enchantments' glow on the hands' items only (main and off hand): upstream loadEquippedWeapons calls
        // applyEnchantVisuals in the hand loop only, not for the ranged item.
        if (glows && k < 2) eq.enchantVisual[k] = weaponFx_->glows.enchantVisual(enchant[k]);
    }
}

void WorldScreen::fillOwnWeaponVisuals(rp::Equipment& eq) {
    if (!session_.selfKnown()) {
        weaponFxEnsure();  // SMSG_CHAR_ENUM's visuals stay (loadCharacter); the sheath types come with our fields
        return;
    }
    uint32_t entry[3], enchant[3];
    for (int k = 0; k < 3; ++k) {
        entry[k] = session_.ownField(static_cast<uint16_t>(kVisibleItem1 + (15 + k) * 2));
        enchant[k] = session_.ownField(static_cast<uint16_t>(kVisibleItem1 + (15 + k) * 2 + 1));
    }
    fillWeaponVisuals(eq, entry, enchant);
}

// A new model of another player starts in the player's sheath state (UNIT_FIELD_BYTES_2 byte 0; sheathremote=0:
// drawn, upstream) - else it was dressed drawn and re-dressed a frame later by updateSheath.
void WorldScreen::initRemoteSheath(rp::CharacterModel& m, const game::Entity& e) {
    constexpr uint16_t kUnitFieldBytes2 = 122;  // 3.3.5a (ui_world_sheath.cpp)
    const uint8_t state = sheathRemote_ ? static_cast<uint8_t>(e.getField(kUnitFieldBytes2) & 0xFFu)
                                        : static_cast<uint8_t>(rp::kSheathStateMelee);
    m.setSheathState(state);
}

void WorldScreen::fillRemoteWeaponVisuals(const game::Entity& e, rp::Equipment& eq) {
    uint32_t entry[3], enchant[3];
    for (int k = 0; k < 3; ++k) {
        entry[k] = e.getField(static_cast<uint16_t>(kVisibleItem1 + (15 + k) * 2));
        enchant[k] = e.getField(static_cast<uint16_t>(kVisibleItem1 + (15 + k) * 2 + 1));
    }
    fillWeaponVisuals(eq, entry, enchant);
}

// The glows of the weapons drawn this frame: the own player's first, then the other players' (nearest first in
// WeaponGlows::frame).
void WorldScreen::drawWeaponFx(const rendering::psp::TerrainCamera& cam) {
    if (weaponFxState_ <= 0 || !weaponFx_) return;
    WeaponFxUi& u = *weaponFx_;
    u.requests.clear();
    rp::WeaponGlow g[4];
    auto add = [&](uint64_t owner, const rp::CharacterModel& m, const glm::vec3& at, bool own) {
        const size_t n = m.weaponGlows(g, 4);
        for (size_t i = 0; i < n && u.requests.size() < kMaxGlowRequests; ++i) {
            rp::WeaponGlows::Request r;
            r.owner = owner;
            r.glow = g[i];
            const glm::vec3 d = at - cam.pos;
            r.d2 = glm::dot(d, d);
            r.own = own;
            u.requests.push_back(r);
        }
    };
    if (player_.loaded()) add(session_.playerGuid(), player_, glm::vec3(playerX_, playerY_, playerZ_), true);
    for (auto& [guid, r] : remotes_)
        if (r.model && r.visible) add(guid, *r.model, r.pos, false);
    u.glows.frame(u.requests, cam, lastDt_);
}

}  // namespace wowee::ui::psp
