// Inventory windows and our own equipment on the model.
//
// Temporary PSP windows (a debug check of the session's inventory model until
// FrameXML's CharacterFrame / ContainerFrame arrive with task A):
//   CHARACTER   the 19 worn slots (upstream PaperDollFrame layout: left
//               column head..wrists, right column hands..trinkets, weapons
//               at the bottom), name, level, money;
//   Bags        the backpack or one worn bag at a time (tabs: BACKPACK_TOOLTIP, 1..4 -
//               the tabs are the worn bag slots 19..22 as well), money.
// Buttons: SELECT -> the menu -> CHARACTER / Bags. In a window:
//   Cross   what a right click does upstream: a worn item -> unequipToBackpack,
//           an item in a bag -> UseContainerItem's order (readable / equippable
//           -> autoEquipItemBySlot|InBag, CMSG_AUTOEQUIP_ITEM: the server picks
//           the slot and swaps / openable / starts a quest / use), a bag tab
//           -> show that bag;
//   Square  pick up / put down (upstream's cursor item: PickupContainerItem /
//           PickupInventoryItem, PutItemInBackpack / PutItemInBag): the drop
//           sends CMSG_SWAP_ITEM, the server validates and merges stacks;
//   Circle  closes the top window (the bind-on-equip prompt = cancel).
// A focused item shows GameTooltip (ui_world_tooltip.cpp, the client's order).
//
// Own equipment (requirement: "change of equipment - on the model at
// once, now only after relog"): upstream GameScreen re-dresses the player
// when consumeOnlineEquipmentDirty() - geosets, textures, weapons, the combat
// loadout. Here syncOwnEquipment() builds the worn display ids from the
// inventory (wowee.cfg ownequip=1) or from our PLAYER_VISIBLE_ITEM fields
// (ownequip=2; 0 = CHAR_ENUM until relog, the old behaviour) and calls
// CharacterModel::setEquipment like syncRemotePlayers does for others.
#include "ui/psp/ui_world_screen.hpp"

#include "game/inventory_slots.hpp"
#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's GlobalStrings
#include "platform/psp/psp_prof.hpp"

#include <pspctrl.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids (batch 3 range C: 0x6000-0x60FF).
enum : uint32_t {
    kIdInvMenuChar = 0x6000,
    kIdInvMenuBags = 0x6001,
    kIdInvEquip0 = 0x6010,      // + equipment slot 0..22 (19..22 = the bag tabs)
    kIdInvBackpackTab = 0x6030,
    kIdInvSlot0 = 0x6040,       // + slot 0..35 of the shown container
    kIdInvConfirmYes = 0x6070,
    kIdInvConfirmNo = 0x6071,
    kIdInvToBags = 0x6072,
    kIdInvToChar = 0x6073,
};

constexpr uint32_t kStatusMs = 4000;
constexpr uint32_t kColHeld = 0xFF00E0FFu;   // yellow
constexpr uint32_t kColSlotEdge = 0xFF505050u;
constexpr uint16_t kVisibleItemBase = 283;   // PLAYER_VISIBLE_ITEM_1_ENTRYID (3.3.5a), stride 2
// PLAYER_FLAGS_HIDE_HELM / _HIDE_CLOAK (upstream entity_controller.cpp; CHAR_ENUM's
// CHARACTER_FLAG_HIDE_HELM / _HIDE_CLOAK are the same bits).
constexpr uint32_t kPlayerFlagHideHelm = 0x00000400;
constexpr uint32_t kPlayerFlagHideCloak = 0x00000800;

bool isEquipId(uint32_t id) { return id >= kIdInvEquip0 && id < kIdInvEquip0 + game::Inventory::NUM_EQUIP_SLOTS; }
bool isSlotId(uint32_t id) { return id >= kIdInvSlot0 && id < kIdInvSlot0 + game::Inventory::MAX_BAG_SIZE; }

// upstream ui::getQualityColor (ui_colors.hpp), as 0xAABBGGRR.
uint32_t qualityColor(game::ItemQuality q) {
    switch (q) {
        case game::ItemQuality::POOR: return 0xFF9E9E9Eu;
        case game::ItemQuality::COMMON: return 0xFFFFFFFFu;
        case game::ItemQuality::UNCOMMON: return 0xFF00FF1Fu;
        case game::ItemQuality::RARE: return 0xFFDE7000u;
        case game::ItemQuality::EPIC: return 0xFFED36A3u;
        case game::ItemQuality::LEGENDARY: return 0xFF0080FFu;
        case game::ItemQuality::ARTIFACT:
        case game::ItemQuality::HEIRLOOM: return 0xFF80CCE6u;
        default: return 0xFFFFFFFFu;
    }
}

// "12g 34s 56c": GOLD_AMOUNT_SYMBOL / SILVER_AMOUNT_SYMBOL / COPPER_AMOUNT_SYMBOL of the running language.
std::string formatMoney(uint64_t copper) {
    const unsigned g = static_cast<unsigned>(copper / 10000), s = static_cast<unsigned>((copper / 100) % 100),
                   c = static_cast<unsigned>(copper % 100);
    const char* gSym = CLIENT_STR(GOLD_AMOUNT_SYMBOL);
    const char* sSym = CLIENT_STR(SILVER_AMOUNT_SYMBOL);
    const char* cSym = CLIENT_STR(COPPER_AMOUNT_SYMBOL);
    if (g) return pp::text::textf("%u%s %u%s %u%s", g, gSym, s, sSym, c, cSym);
    if (s) return pp::text::textf("%u%s %u%s", s, sSym, c, cSym);
    return pp::text::textf("%u%s", c, cSym);
}

// The first `n` codepoints of a UTF-8 string.
std::string firstChars(const std::string& s, int n) {
    const char* p = s.c_str();
    for (int i = 0; i < n && *p; ++i) utf8Next(&p);
    return std::string(s.c_str(), static_cast<size_t>(p - s.c_str()));
}

// Empty worn slot captions (upstream draws the slot's background art): the port's own short names.
const char* equipCaption(int slot) {
    using pp::PS;
    static const PS kCap[19] = {PS::INV_CAP_HEAD,     PS::INV_CAP_NECK,     PS::INV_CAP_SHOULDER, PS::INV_CAP_SHIRT,
                                PS::INV_CAP_CHEST,    PS::INV_CAP_WAIST,    PS::INV_CAP_LEGS,     PS::INV_CAP_FEET,
                                PS::INV_CAP_WRIST,    PS::INV_CAP_HANDS,    PS::INV_CAP_FINGER1,  PS::INV_CAP_FINGER2,
                                PS::INV_CAP_TRINKET1, PS::INV_CAP_TRINKET2, PS::INV_CAP_BACK,     PS::INV_CAP_MAINHAND,
                                PS::INV_CAP_OFFHAND,  PS::INV_CAP_RANGED,   PS::INV_CAP_TABARD};
    return slot >= 0 && slot < 19 ? pp::ps(kCap[slot]) : "";
}

// Paperdoll: left column, right column, weapons (upstream PaperDollFrame).
constexpr int kLeftCol[8] = {0, 1, 2, 14, 4, 3, 18, 8};
constexpr int kRightCol[8] = {9, 5, 6, 7, 10, 11, 12, 13};
constexpr int kWeapons[3] = {15, 16, 17};

} // namespace

// --- Per frame -------------------------------------------------------------------

void WorldScreen::updateInventory(float dtSec) {
    (void)dtSec;
    PROF_SCOPE("inventory");
    if (!invInit_) {
        invInit_ = true;
        ownEquipMode_ = std::clamp(pp::configGetInt("ownequip", 1), 0, 2);
        PSP_LOGI(pp::tag::Gfx, "inv_init ownequip=%d autostore=%d", ownEquipMode_, pp::configGetInt("invautostore", 1));
    }
    const uint32_t nowMs = pp::logUptimeMs();
    // Upstream's addon events: the Lua adapter will take these; for now the
    // windows react to the few they need.
    pp::WorldSession::InventoryEvent ev;
    while (session_.takeInventoryEvent(ev)) {
        fxForwardInventoryEvent(ev);  // framexml - the same addon event for Lua
        // (the per-rebuild BAG_UPDATE / UNIT_INVENTORY_CHANGED and the per-answer
        // GET_ITEM_INFO_RECEIVED would spend the log budget at login)
        const bool chatty = std::strcmp(ev.name, "BAG_UPDATE") == 0 ||
                            std::strcmp(ev.name, "UNIT_INVENTORY_CHANGED") == 0 ||
                            std::strcmp(ev.name, "GET_ITEM_INFO_RECEIVED") == 0;
        if (!chatty && ++invEventLogs_ <= 60)
            PSP_LOGI(pp::tag::Net, "inv_event name=%s arg=%d entry=%u count=%u", ev.name, static_cast<int>(ev.arg),
                     static_cast<unsigned>(ev.entry), static_cast<unsigned>(ev.count));
        if (std::strcmp(ev.name, "AUTOEQUIP_BIND_CONFIRM") == 0) {
            openWindow(kWinBindConfirm);
            winFocus_.setFocus(kIdInvConfirmNo);  // binding takes a deliberate second choice
        }
    }
    // Errors (SMSG_INVENTORY_CHANGE_FAILURE, "no room") on the window's status line.
    uint32_t errMs = 0;
    const std::string& err = session_.lastInventoryError(&errMs);
    if (errMs != invSeenErrorMs_) {
        invSeenErrorMs_ = errMs;
        invStatus_ = err;
        if (!err.empty()) {  // UI_ERROR_MESSAGE on the red line (the frames have no status line)
            combatError_ = err;
            combatErrorUntilMs_ = nowMs + 2000u;
        }
        invStatusMs_ = nowMs;
    }
    // The bind prompt closed with Circle = cancel; a prompt with nothing pending goes away.
    const bool confirmOpen = windowOpen(kWinBindConfirm);
    if (session_.pendingEquip().active && !confirmOpen) session_.cancelPendingEquip();
    if (confirmOpen && !session_.pendingEquip().active) closeWindow(kWinBindConfirm);
    deleteItemTick();  // DELETE_ITEM closed by START = ClearCursor
    if (!windowOpen(kWinCharacter) && !windowOpen(kWinBags) && !windowOpen(kWinDeleteItem)) invHeld_ = InvHeld{};
    syncOwnEquipment();
}

// --- Own equipment on the model ----------------------------------------------------

// The worn display ids as they are now; slots not known yet keep what `want`
// holds (the model's current dress). False = nothing to go on yet.
bool WorldScreen::resolveOwnEquipment(rendering::psp::Equipment& want, int& unresolved) {
    unresolved = 0;
    if (ownEquipMode_ == 0 || !session_.selfKnown()) return false;
    // SHOW_HELM / SHOW_CLOAK as the realm keeps them - our own
    // PLAYER_FLAGS (upstream EntityController reads them into helmVisible_ / cloakVisible_; the client adds no
    // head / back item while they are set). A switch is then an equipment change: syncOwnEquipment re-dresses.
    // Batch 5 merge: through isHelmVisible / isCloakVisible - the character window's check box
    // shows on the model at once (what was asked stands until the realm's flags say the same, upstream).
    want.hideHelm = !session_.isHelmVisible();
    want.hideCloak = !session_.isCloakVisible();
    if (ownEquipMode_ == 2) {
        // PSP: PLAYER_VISIBLE_ITEM_n of our own object, resolved by the item cache.
        for (int s = 0; s < 19; ++s) {
            const uint32_t entry = session_.ownField(static_cast<uint16_t>(kVisibleItemBase + s * 2));
            if (!entry) {
                want.displayId[s] = 0;
                want.invType[s] = 0;
                continue;
            }
            const auto* info = session_.getItemInfo(entry);
            if (!info) {
                session_.ensureItemInfo(entry);
                ++unresolved;
                continue;
            }
            want.displayId[s] = info->displayInfoId;
            want.invType[s] = static_cast<uint8_t>(info->inventoryType);
        }
        return true;
    }
    // upstream: the inventory's worn slots (resolvePlayerEquipment), after
    // the first rebuild that followed our own create block.
    if (session_.inventoryVersion() == 0) return false;
    const auto& inv = session_.getInventory();
    for (int s = 0; s < 19; ++s) {
        const uint64_t guid = session_.getEquipSlotGuid(s);
        const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (guid == 0) {
            want.displayId[s] = 0;
            want.invType[s] = 0;
            continue;
        }
        if (slot.empty()) {  // the item object has not arrived yet
            ++unresolved;
            continue;
        }
        uint32_t display = slot.item.displayInfoId;
        uint8_t invType = slot.item.inventoryType;
        if (display == 0) {
            const auto* info = session_.getItemInfo(slot.item.itemId);
            if (!info) {
                ++unresolved;
                continue;
            }
            display = info->displayInfoId;
            invType = static_cast<uint8_t>(info->inventoryType);
        }
        want.displayId[s] = display;
        want.invType[s] = invType;
    }
    return true;
}

// loadCharacter: dress from what we wear now instead of the CHAR_ENUM row
// (the Debug window's model reload used to put the login outfit back).
void WorldScreen::ownEquipmentOverride(rendering::psp::Equipment& equip) {
    // until our own update fields are in, the helm / cloak switches as SMSG_CHAR_ENUM
    // has them (the character flags carry the same bits as PLAYER_FLAGS).
    for (const auto& c : session_.characters())
        if (c.name == name_) {
            equip.hideHelm = (c.flags & kPlayerFlagHideHelm) != 0;
            equip.hideCloak = (c.flags & kPlayerFlagHideCloak) != 0;
        }
    rendering::psp::Equipment want = equip;
    int unresolved = 0;
    if (!resolveOwnEquipment(want, unresolved)) return;
    equip = want;
    invLoadoutDirty_ = true;
}

// upstream GameScreen: setEquippedWeaponType / setEquippedRangedType from the
// main hand, off hand and ranged slots.
void WorldScreen::applyOwnLoadout() {
    const auto& inv = session_.getInventory();
    const auto& eq = player_.equipment();
    rendering::WeaponLoadout lo;
    const auto& mh = inv.getEquipSlot(game::EquipSlot::MAIN_HAND);
    const auto& oh = inv.getEquipSlot(game::EquipSlot::OFF_HAND);
    const uint8_t mhType = mh.empty() ? eq.invType[15] : mh.item.inventoryType;
    const uint8_t ohType = oh.empty() ? eq.invType[16] : oh.item.inventoryType;
    lo.inventoryType = eq.displayId[15] ? mhType : 0;
    if (!mh.empty()) {
        lo.is2HLoose = mh.item.subclassName == "Polearm" || mh.item.subclassName == "Staff";
        lo.isFist = mh.item.subclassName == "Fist Weapon";
        lo.isDagger = mh.item.subclassName == "Dagger";
    }
    lo.hasOffHand = eq.displayId[16] && game::isOffHandWeaponInventoryType(ohType);
    lo.hasShield = eq.displayId[16] && ohType == game::InvType::SHIELD;
    if (lo.hasOffHand && !oh.empty()) {
        lo.offHandIsFist = oh.item.subclassName == "Fist Weapon";
        lo.offHandIsDagger = oh.item.subclassName == "Dagger";
    }
    animator_.setEquippedWeaponType(lo);
    const auto& rs = inv.getEquipSlot(game::EquipSlot::RANGED);
    const uint8_t rType = rs.empty() ? eq.invType[17] : rs.item.inventoryType;
    rendering::RangedWeaponType ranged = rendering::RangedWeaponType::NONE;
    if (eq.displayId[17]) {
        if (rType == game::InvType::RANGED_BOW)
            ranged = rs.item.subclassName == "Crossbow" ? rendering::RangedWeaponType::CROSSBOW
                                                         : rendering::RangedWeaponType::BOW;
        else if (rType == game::InvType::RANGED_GUN)
            ranged = rs.item.subclassName == "Wand" ? rendering::RangedWeaponType::WAND : rendering::RangedWeaponType::GUN;
        else if (rType == game::InvType::THROWN)
            ranged = rendering::RangedWeaponType::THROWN;
    }
    animator_.setEquippedRangedType(ranged);
}

// PSP: checked every 250 ms; a re-dress reads the body layers from the
// Memory Stick (M10c), so with item answers still out it waits up to 2 s
// (as syncRemotePlayers does) and slots not known yet keep their dress.
void WorldScreen::syncOwnEquipment() {
    if (ownEquipMode_ == 0 || !player_.loaded() || !charPak_.isOpen()) return;
    const uint32_t nowMs = pp::logUptimeMs();
    if (nowMs < ownEquipCheckMs_) return;
    ownEquipCheckMs_ = nowMs + 250u;
    const rendering::psp::Equipment& worn = player_.equipment();
    rendering::psp::Equipment want = worn;
    int unresolved = 0;
    if (!resolveOwnEquipment(want, unresolved)) return;
    fillOwnWeaponVisuals(want);  // the weapons' sheath types and enchantment glows
    fillGuildEmblem(session_.guildId(), worn, want);  // the guild tabard's emblem (our guild's query)
    if (want == worn) {
        ownEquipChangeMs_ = 0;
        if (invLoadoutDirty_) {
            invLoadoutDirty_ = false;
            applyOwnLoadout();
        }
        return;
    }
    if (!ownEquipChangeMs_) ownEquipChangeMs_ = nowMs ? nowMs : 1u;
    if (unresolved && nowMs - ownEquipChangeMs_ < 2000u) return;
    for (int s = 0; s < 19; ++s) {
        if (want.displayId[s] == worn.displayId[s] && want.invType[s] == worn.invType[s]) continue;
        if (++equipLogs_ <= 60)
            PSP_LOGI(pp::tag::Gfx, "equip_change slot=%d display=%u was=%u inv=%u", s,
                     static_cast<unsigned>(want.displayId[s]), static_cast<unsigned>(worn.displayId[s]),
                     static_cast<unsigned>(want.invType[s]));
    }
    if ((want.hideHelm != worn.hideHelm || want.hideCloak != worn.hideCloak) && ++equipLogs_ <= 60)
        PSP_LOGI(pp::tag::Gfx, "equip_change hide_helm=%d hide_cloak=%d", want.hideHelm ? 1 : 0,
                 want.hideCloak ? 1 : 0);
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    const uint32_t reads0 = pp::pakReadCount();  // reads= (card reads of the re-dress)
    player_.setEquipment(charPak_, want, &items_);  // geosets, body texture, cloak, attachments
    ownEquipChangeMs_ = 0;
    invLoadoutDirty_ = false;
    applyOwnLoadout();
    PSP_LOGI(pp::tag::Gfx, "equip_apply src=%s unresolved=%d ms=%u reads=%u", ownEquipMode_ == 2 ? "visible" : "inventory",
             unresolved, static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u),
             static_cast<unsigned>(pp::pakReadCount() - reads0));
}

// --- Window input ----------------------------------------------------------------

// A window id -> the wire (container, slot) it names.
bool WorldScreen::inventoryIdToWire(uint32_t id, uint8_t& bag, uint8_t& slot) const {
    int bc = 0, bs = 0;
    if (bagSlotId(id, bc, bs)) {  // a ContainerFrame slot
        if (bc == 0) {
            if (bs >= game::Inventory::BACKPACK_SLOTS) return false;
            bag = game::slots::kNoContainer;
            slot = static_cast<uint8_t>(game::slots::backpackWireSlot(bs));
        } else {
            if (bs >= session_.getInventory().getBagSize(bc - 1)) return false;
            bag = static_cast<uint8_t>(game::slots::wornBagContainer(bc - 1));
            slot = static_cast<uint8_t>(bs);
        }
        return true;
    }
    if (isEquipId(id)) {
        bag = game::slots::kNoContainer;
        slot = static_cast<uint8_t>(id - kIdInvEquip0);
        return true;
    }
    if (isSlotId(id)) {
        const int i = static_cast<int>(id - kIdInvSlot0);
        if (invBag_ == 0) {
            if (i >= game::Inventory::BACKPACK_SLOTS) return false;
            bag = game::slots::kNoContainer;
            slot = static_cast<uint8_t>(game::slots::backpackWireSlot(i));
        } else {
            if (i >= session_.getInventory().getBagSize(invBag_ - 1)) return false;
            bag = static_cast<uint8_t>(game::slots::wornBagContainer(invBag_ - 1));
            slot = static_cast<uint8_t>(i);
        }
        return true;
    }
    return false;
}

const game::ItemDef* WorldScreen::inventoryItemAt(uint32_t id) const {
    const auto& inv = session_.getInventory();
    const game::ItemSlot* s = nullptr;
    int bc = 0, bs = 0;
    if (bagSlotId(id, bc, bs)) {  // a ContainerFrame slot
        if (bc == 0 && bs < game::Inventory::BACKPACK_SLOTS) s = &inv.getBackpackSlot(bs);
        else if (bc > 0 && bs < inv.getBagSize(bc - 1)) s = &inv.getBagSlot(bc - 1, bs);
        return s && !s->empty() ? &s->item : nullptr;
    }
    if (isEquipId(id)) {
        s = &inv.getEquipSlot(static_cast<game::EquipSlot>(id - kIdInvEquip0));
    } else if (isSlotId(id)) {
        const int i = static_cast<int>(id - kIdInvSlot0);
        s = invBag_ == 0 ? &inv.getBackpackSlot(i) : &inv.getBagSlot(invBag_ - 1, i);
    }
    return s && !s->empty() ? &s->item : nullptr;
}

// Square: pick up, then put down (upstream's cursor item and its drop rules).
void WorldScreen::inventoryWindowPad(const pp::PadState& pad) {
    if (!(pad.pressed & PSP_CTRL_SQUARE)) return;
    if (!windowOpen(kWinCharacter) && !windowOpen(kWinBags)) return;
    inventoryPickPut(winFocus_.focused());
}

// The right click (Circle) on a slot: a bag item -> UseContainerItem (equip, use, sell at a
// merchant); a worn item -> off to the backpack (PSP: the click takes it off). False = not ours.
bool WorldScreen::inventoryAltAction(uint32_t id) {
    int bc = 0, bs = 0;
    const bool slot = bagSlotId(id, bc, bs) || isSlotId(id);
    const bool worn = isEquipId(id) && static_cast<int>(id - kIdInvEquip0) < game::Inventory::FIRST_BAG_EQUIP_SLOT;
    if (!slot && !worn) return false;
    inventoryActivateSlot(id);
    return true;
}

// The left click / Square on a slot: pick it up, or put the held item there.
void WorldScreen::inventoryPickPut(uint32_t id) {
    if (itemUseTargetClick(id)) return;  // SpellTargetItem - the left click on the target item
    const auto& inv = session_.getInventory();
    const auto say = [this](const std::string& s) {
        invStatus_ = s;
        invStatusMs_ = pp::logUptimeMs();
    };
    if (!invHeld_.active) {
        uint8_t bag = 0, slot = 0;
        const game::ItemDef* item = inventoryItemAt(id);
        if (!item || !inventoryIdToWire(id, bag, slot)) return;
        invHeld_ = InvHeld{true, bag, slot};
        say(OWN_FMT(INV_PICKED_UP, item->name));
        return;
    }
    const InvHeld held = invHeld_;
    invHeld_ = InvHeld{};
    // upstream PutItemInBackpack: onto the backpack tab = the first free slot.
    if (id == kIdInvBackpackTab) {
        const int free = inv.findFreeBackpackSlot();
        if (free < 0) {
            say(OWN_STR(INV_BACKPACK_FULL));
            return;
        }
        session_.swapContainerItems(held.bag, held.slot, game::slots::kNoContainer,
                                    static_cast<uint8_t>(game::slots::backpackWireSlot(free)));
        say(OWN_STR(INV_MOVED_TO_BACKPACK));
        return;
    }
    uint8_t bag = 0, slot = 0;
    if (!inventoryIdToWire(id, bag, slot)) return;
    if (bag == held.bag && slot == held.slot) {
        say(OWN_STR(INV_PUT_BACK));
        return;
    }
    // upstream PutItemInBag: a bag tab (worn bag slot 19..22).
    const int bagSlotFirst = game::slots::kWornBagFirst;
    const bool dstBagTab = bag == game::slots::kNoContainer && slot >= bagSlotFirst &&
                           slot < bagSlotFirst + game::slots::kWornBagCount;
    if (dstBagTab) {
        const int dstIndex = slot - bagSlotFirst;
        const bool heldWornBag = held.bag == game::slots::kNoContainer && held.slot >= bagSlotFirst &&
                                 held.slot < bagSlotFirst + game::slots::kWornBagCount;
        if (heldWornBag) {  // two worn bags change places
            session_.swapBagSlots(held.slot - bagSlotFirst, dstIndex);
            say(OWN_STR(INV_BAGS_SWAPPED));
            return;
        }
        const int size = inv.getBagSize(dstIndex);
        if (size == 0) {  // an empty bag slot: equip the held bag there
            session_.swapContainerItems(held.bag, held.slot, game::slots::kNoContainer, slot);
            say(OWN_STR(INV_BAG_EQUIPPED));
            return;
        }
        for (int i = 0; i < size; ++i) {
            if (!inv.getBagSlot(dstIndex, i).empty()) continue;
            session_.swapContainerItems(held.bag, held.slot, static_cast<uint8_t>(game::slots::wornBagContainer(dstIndex)),
                                        static_cast<uint8_t>(i));
            say(OWN_STR(INV_MOVED_TO_BAG));
            return;
        }
        say(CLIENT_STR(ERR_BAG_FULL));
        return;
    }
    // Anywhere else: CMSG_SWAP_ITEM (onto a worn slot = equip it there; the
    // server says no with SMSG_INVENTORY_CHANGE_FAILURE if it does not fit).
    session_.swapContainerItems(held.bag, held.slot, bag, slot);
    say(OWN_STR(INV_MOVED));
}

// Cross on a slot.
bool WorldScreen::inventoryActivateSlot(uint32_t id) {
    const auto say = [this](const std::string& s) {
        invStatus_ = s;
        invStatusMs_ = pp::logUptimeMs();
    };
    // A ContainerFrame slot = that container's slot (the old single-bag ids below).
    int bc = 0, bs = 0;
    if (bagSlotId(id, bc, bs)) {
        invBag_ = bc;
        id = kIdInvSlot0 + static_cast<uint32_t>(bs);
    }
    if (itemUseTargetClick(id)) return true;  // SpellTargetItem (an item spell waits for an item)
    if (id == kIdInvBackpackTab) {
        if (windowOpen(kWinBags) && bagArt_.backpack.pixels) {  // the bag bar toggles the backpack
            bagOpen_[0] = !bagOpen_[0];
            if (!bagOpen_[0] && std::none_of(bagOpen_, bagOpen_ + 5, [](bool b) { return b; })) closeWindow(kWinBags);
            return true;
        }
        invBag_ = 0;
        return true;
    }
    if (isEquipId(id)) {
        const int s = static_cast<int>(id - kIdInvEquip0);
        if (s >= game::Inventory::FIRST_BAG_EQUIP_SLOT) {  // a bag tab: show that bag
            invBag_ = s - game::Inventory::FIRST_BAG_EQUIP_SLOT + 1;
            if (session_.getInventory().getBagSize(invBag_ - 1) == 0) {
                say(OWN_STR(INV_NO_BAG_IN_SLOT));
            } else if (windowOpen(kWinBags) && bagArt_.backpack.pixels) {  // ToggleBag
                bagOpen_[invBag_] = !bagOpen_[invBag_];
                if (std::none_of(bagOpen_, bagOpen_ + 5, [](bool b) { return b; })) closeWindow(kWinBags);
            }
            return true;
        }
        const game::ItemDef* item = inventoryItemAt(id);
        if (!item) return true;
        if (itemUseWornClick(s, *item)) return true;  // UseInventoryItem (a trinket); else take it off
        session_.unequipToBackpack(static_cast<game::EquipSlot>(s));
        say(OWN_FMT(INV_TAKING_OFF, item->name));
        return true;
    }
    if (!isSlotId(id)) return false;
    const game::ItemDef* item = inventoryItemAt(id);
    if (!item) return true;
    const int i = static_cast<int>(id - kIdInvSlot0);
    // UseContainerItem's window branches (mail / bank / guild bank / auction are other windows' handlers): the
    // merchant sells it (upstream lua_UseContainerItem), then the trade table takes it (PSP: own rule).
    if (session_.isVendorWindowOpen()) {
        uint64_t guid = item->guid;
        if (guid == 0) guid = invBag_ == 0 ? session_.getBackpackItemGuid(i) : session_.getBagItemGuid(invBag_ - 1, i);
        if (session_.sellItemByGuid(guid)) say(OWN_FMT(INV_SOLD, item->name));
        return true;
    }
    if (tradeTakesBagItem(id)) return true;  // trading - the item to the trade table
    // UseContainerItem after the window verbs: start the quest, read, equip, open (clams, lockboxes), use
    // (CMSG_USE_ITEM) - ui_world_item_use.cpp.
    return itemUseContainerClick(invBag_, i, *item);
}

bool WorldScreen::inventoryWindowAction(uint32_t act) {
    // The ContainerFrame ids (slots, close buttons).
    if (act >= kIdBagClose0 && act < kIdBagClose0 + 5) {
        bagOpen_[act - kIdBagClose0] = false;
        if (std::none_of(bagOpen_, bagOpen_ + 5, [](bool b) { return b; })) closeWindow(kWinBags);
        return true;
    }
    if (act >= kIdBagSlot0 && act < kIdBagSlot0 + 5 * 36) {
        inventoryPickPut(act);  // the left click picks up / puts down
        return true;
    }
    if (act < 0x6000 || act > 0x60FF) return false;
    // The left click on a worn item picks it up; on a bag button with an item held puts it in.
    if (isEquipId(act) && (static_cast<int>(act - kIdInvEquip0) < game::Inventory::FIRST_BAG_EQUIP_SLOT || invHeld_.active)) {
        inventoryPickPut(act);
        return true;
    }
    if (act == kIdInvBackpackTab && invHeld_.active) {
        inventoryPickPut(act);
        return true;
    }
    auto toggle = [this](uint8_t w, uint32_t focus) {
        if (windowOpen(w) && windows_.back() == w) {
            closeWindow(w);
            return;
        }
        closeWindow(kWinMenu);  // its buttons would sit under ours
        openWindow(w);
        winFocus_.setFocus(focus);
    };
    switch (act) {
        case kIdInvMenuChar: toggle(kWinCharacter, kIdInvEquip0); return true;
        case kIdInvMenuBags:  // OpenAllBags / CloseAllBags
            if (windowOpen(kWinBags) && windows_.back() == kWinBags) {
                closeWindow(kWinBags);
            } else {
                closeWindow(kWinMenu);
                openBags(true);
                openWindow(kWinBags);  // on top
                winFocus_.setFocus(kIdBagSlot0);
            }
            return true;
        case kIdInvToBags: openBags(true); openWindow(kWinBags); winFocus_.setFocus(kIdBagSlot0); return true;
        case kIdInvToChar: openWindow(kWinCharacter); winFocus_.setFocus(kIdInvEquip0); return true;
        case kIdInvConfirmYes:
            session_.equipPendingItem();
            closeWindow(kWinBindConfirm);
            return true;
        case kIdInvConfirmNo:
            session_.cancelPendingEquip();
            closeWindow(kWinBindConfirm);
            return true;
        default: break;
    }
    inventoryActivateSlot(act);
    return true;
}

// --- Drawing ---------------------------------------------------------------------

// Two buttons of the GameMenuFrame column (ui_world_windows.cpp drawGameMenu).
void WorldScreen::drawInventoryMenuRow(UiRenderer& ui, const Rect& character, const Rect& bags, bool focusable) {
    drawGameMenuButton(ui, kIdInvMenuChar, character, CLIENT_STR(CHARACTER), focusable);
    drawGameMenuButton(ui, kIdInvMenuBags, bags, CLIENT_STR(TUTORIAL_TITLE10), focusable);  // PSP (the bag bar): "Bags"
}

void WorldScreen::drawItemSlot(UiRenderer& ui, uint32_t id, const Rect& r, const game::ItemDef* item,
                               const char* emptyLabel, bool focusable) {
    const bool focused = focusable && winFocus_.addStop(id, r);
    const Font& fs = kit_.font(kFontSmall);
    ui.fill(r, 0xC0181818u);
    uint8_t hb = 0, hs = 0;
    const bool held = invHeld_.active && inventoryIdToWire(id, hb, hs) && hb == invHeld_.bag && hs == invHeld_.slot;
    // The item's icon (itemicons.pak) in the UI-Quickslot2 frame, as
    // ItemButtonTemplate - no quality border in 3.3.5; the old look without the pak.
    const pp::Texture* icon = item ? itemIcons().byDisplay(item->displayInfoId, spellFrame_) : nullptr;
    const uint32_t edge = held ? kColHeld : item ? qualityColor(item->quality) : kColSlotEdge;
    const float t = held ? 2.0f : 1.0f;
    if (!icon || held) {
        ui.fill(Rect{r.x, r.y, r.w, t}, edge);
        ui.fill(Rect{r.x, r.y + r.h - t, r.w, t}, edge);
        ui.fill(Rect{r.x, r.y, t, r.h}, edge);
        ui.fill(Rect{r.x + r.w - t, r.y, t, r.h}, edge);
    }
    const float ty = r.y + (r.h - fs.lineHeight()) * 0.5f;
    if (item) {
        if (icon) {
            ui.image(*icon, Rect{r.x + 1, r.y + 1, r.w - 2, r.h - 2}, held ? 0xFF808080u : 0xFFFFFFFFu);
            if (spellArt_.slot.pixels) {
                const float c = r.w * 64.0f / 37.0f;
                ui.image(spellArt_.slot, Rect{r.x + r.w * 0.5f - c * 0.5f, r.y + r.h * 0.5f - c * 0.5f, c, c});
            }
        } else {
            const std::string cap = firstChars(item->name, 3);
            ui.textShadow(fs, r.x + r.w * 0.5f, ty - (item->stackCount > 1 ? 3.0f : 0.0f), cap.c_str(),
                          qualityColor(item->quality), Align::Center);
        }
        if (item->stackCount > 1) {
            char n[12];
            std::snprintf(n, sizeof(n), "%u", static_cast<unsigned>(item->stackCount));
            ui.textShadow(fs, r.x + r.w - 2, r.y + r.h - fs.lineHeight(), n, kColWhite, Align::Right);
        }
    } else if (emptyLabel && *emptyLabel) {
        ui.text(fs, r.x + r.w * 0.5f, ty, emptyLabel, 0xFF707070u, Align::Center);
    }
    if (focused) {
        ui.fill(r, 0x40FFFFFFu);
        if (item) {
            invTipId_ = id;
            invTipRect_ = Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h};  // screen: drawn after the windows
        }
    }
}

// drawItemTooltip is GameTooltip now - ui_world_tooltip.cpp.

bool WorldScreen::drawInventoryWindow(UiRenderer& ui, uint8_t w) {
    if (w != kWinCharacter && w != kWinBags && w != kWinBindConfirm) return false;
    PROF_SCOPE("inventory.draw");
    // Two windows at once: every one of them takes the focus (FocusRing
    // drops what another window covers); the bind prompt is modal.
    const bool focusable = uiMode_ && !settings_.isOpen() && (w == kWinBindConfirm || !windowOpen(kWinBindConfirm));
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const auto& inv = session_.getInventory();
    const uint32_t nowMs = pp::logUptimeMs();
    char line[96];
    auto drawStatus = [&](float x, float y) {
        if (invStatus_.empty() || nowMs - invStatusMs_ > kStatusMs) return;
        ui.textShadow(fs, x, y, invStatus_.c_str(), 0xFF60C0FFu);
    };
    if (w == kWinBindConfirm) {
        // StaticPopupDialogs["AUTOEQUIP_BIND"] / ["EQUIP_BIND"] - EQUIP_NO_DROP, OKAY / CANCEL
        // (OnAccept EquipPendingItem, OnCancel CancelPendingEquip; START = hideOnEscape: cancel).
        StaticPopup p;
        p.text = CLIENT_STR(EQUIP_NO_DROP);
        p.button1 = CLIENT_STR(OKAY);
        p.button1Id = kIdInvConfirmYes;
        p.button2 = CLIENT_STR(CANCEL);
        p.button2Id = kIdInvConfirmNo;
        p.focusable = focusable;
        drawStaticPopup(ui, p);
        return true;
    }
    if (w == kWinCharacter && drawPaperDoll(ui, focusable)) {  // CharacterFrame / PaperDollFrame
    } else if (w == kWinCharacter) {
        const Rect r{4, 4, 204, 250};
        kit_.panel(ui, r);
        ui.textShadow(fn, r.x + r.w * 0.5f, r.y + 8, CLIENT_STR(CHARACTER), kColGold, Align::Center);
        constexpr float kS = 24.0f, kStep = 26.0f;
        const float y0 = r.y + 26, xl = r.x + 8, xr = r.x + r.w - 8 - kS;
        for (int i = 0; i < 8; ++i) {
            const int sl = kLeftCol[i], sr = kRightCol[i];
            drawItemSlot(ui, kIdInvEquip0 + sl, Rect{xl, y0 + i * kStep, kS, kS},
                         inventoryItemAt(kIdInvEquip0 + sl), equipCaption(sl), focusable);
            drawItemSlot(ui, kIdInvEquip0 + sr, Rect{xr, y0 + i * kStep, kS, kS},
                         inventoryItemAt(kIdInvEquip0 + sr), equipCaption(sr), focusable);
        }
        const float wx = r.x + (r.w - 3 * kStep + 2) * 0.5f;
        for (int i = 0; i < 3; ++i) {
            const int s = kWeapons[i];
            drawItemSlot(ui, kIdInvEquip0 + s, Rect{wx + i * kStep, y0 + 7 * kStep, kS, kS},
                         inventoryItemAt(kIdInvEquip0 + s), equipCaption(s), focusable);
        }
        // Middle column: who, level, money.
        const float cx = r.x + r.w * 0.5f;
        ui.textShadow(fs, cx, y0 + 2, name_.c_str(), kColGold, Align::Center);
        std::snprintf(line, sizeof(line), "%s",
                      CLIENT_FMT(UNIT_LEVEL_TEMPLATE,
                                 static_cast<unsigned>(session_.selfField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL))))
                          .c_str());
        ui.textShadow(fs, cx, y0 + 16, line, kColWhite, Align::Center);
        ui.textShadow(fs, cx, y0 + 36, CLIENT_STR(MONEY_COLON), 0xFFC0C0C0u, Align::Center);
        ui.textShadow(fs, cx, y0 + 50, formatMoney(session_.getMoneyCopper()).c_str(), kColGold, Align::Center);
        int worn = 0;
        for (int s = 0; s < 19; ++s)
            if (!inv.getEquipSlot(static_cast<game::EquipSlot>(s)).empty()) ++worn;
        std::snprintf(line, sizeof(line), "%s", OWN_FMT(INV_WORN_COUNT, worn).c_str());
        ui.textShadow(fs, cx, y0 + 70, line, 0xFFC0C0C0u, Align::Center);
        if (focusable) kit_.button(ui, winFocus_, kIdInvToBags, Rect{cx - 40, y0 + 5 * kStep, 80, 22}, CLIENT_STR(TUTORIAL_TITLE10));
        drawStatus(r.x + 8, r.y + r.h - 14);
    } else if ((loadBagArt(), bagArt_.backpack.pixels != nullptr)) {  // ContainerFrame
        drawContainerFrames(ui, focusable);
    } else {  // kWinBags
        const Rect r{246, 4, 230, 218};
        kit_.panel(ui, r);
        const int bagSize = invBag_ == 0 ? game::Inventory::BACKPACK_SLOTS : inv.getBagSize(invBag_ - 1);
        if (invBag_ == 0) {
            std::snprintf(line, sizeof(line), "%s", CLIENT_STR(BACKPACK_TOOLTIP));
        } else {
            const auto& bagItem = inv.getEquipSlot(static_cast<game::EquipSlot>(game::Inventory::FIRST_BAG_EQUIP_SLOT + invBag_ - 1));
            std::snprintf(line, sizeof(line), "%s", bagItem.empty() ? OWN_STR(INV_NO_BAG) : bagItem.item.name.c_str());
        }
        ui.textShadow(fn, r.x + r.w * 0.5f, r.y + 8, line, kColGold, Align::Center);
        // Tabs: the backpack, then the four worn bag slots (these are the
        // equipment slots 19..22 too: Square moves bags, Cross opens one).
        float tx = r.x + 8;
        const float ty = r.y + 26;
        if (focusable) {
            kit_.button(ui, winFocus_, kIdInvBackpackTab, Rect{tx, ty, 58, 20}, CLIENT_STR(BACKPACK_TOOLTIP));
        } else {
            ui.fill(Rect{tx, ty, 58, 20}, 0xC0303030u);
            ui.textShadow(fs, tx + 29, ty + 5, CLIENT_STR(BACKPACK_TOOLTIP), kColGold, Align::Center);
        }
        if (invBag_ == 0) ui.fill(Rect{tx + 4, ty + 20, 50, 2}, kColGold);
        tx += 62;
        for (int b = 0; b < 4; ++b) {
            const uint32_t id = kIdInvEquip0 + game::Inventory::FIRST_BAG_EQUIP_SLOT + b;
            char cap[4];
            std::snprintf(cap, sizeof(cap), "%d", b + 1);
            const Rect tr{tx, ty, 36, 20};
            drawItemSlot(ui, id, tr, inventoryItemAt(id), cap, focusable);
            if (invBag_ == b + 1) ui.fill(Rect{tx + 4, ty + 20, 28, 2}, kColGold);
            tx += 38;
        }
        constexpr float kS = 24.0f, kStep = 27.0f;
        const float gx = r.x + 8, gy = r.y + 52;
        for (int i = 0; i < bagSize && i < game::Inventory::MAX_BAG_SIZE; ++i) {
            const uint32_t id = kIdInvSlot0 + i;
            drawItemSlot(ui, id, Rect{gx + (i % 8) * kStep, gy + (i / 8) * kStep, kS, kS}, inventoryItemAt(id), "",
                         focusable);
        }
        if (bagSize == 0) ui.textShadow(fs, r.x + r.w * 0.5f, gy + 20, OWN_STR(INV_NO_BAG_IN_SLOT), kColGrey, Align::Center);
        int freeSlots = 0;
        for (int i = 0; i < inv.getBackpackSize(); ++i)
            if (inv.getBackpackSlot(i).empty()) ++freeSlots;
        for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b)
            for (int i = 0; i < inv.getBagSize(b); ++i)
                if (inv.getBagSlot(b, i).empty()) ++freeSlots;
        std::snprintf(line, sizeof(line), "%s", OWN_FMT(INV_FREE_SLOTS, freeSlots).c_str());
        ui.textShadow(fs, r.x + 8, r.y + r.h - 30, line, 0xFFC0C0C0u);
        ui.textShadow(fs, r.x + r.w - 8, r.y + r.h - 30, formatMoney(session_.getMoneyCopper()).c_str(), kColGold,
                      Align::Right);
        if (focusable) kit_.button(ui, winFocus_, kIdInvToChar, Rect{r.x + r.w * 0.5f - 34, r.y + r.h - 33, 68, 18},
                                   CLIENT_STR(CHARACTER));
        drawStatus(r.x + 8, r.y + r.h - 14);
    }
    return true;  // the tooltip after all the windows (drawWindows)
}

} // namespace wowee::ui::psp
