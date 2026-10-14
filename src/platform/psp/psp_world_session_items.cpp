// Using items: the port's rules, from upstream WoWee (lua_UseContainerItem, InventoryHandler::dispatchUseItem /
// useItemById, targetGuidForUseItem, itemUseCooldown) and AzerothCore's public handlers.
//
//   UseContainerItem   (after the windows' branches, which are the screen's) upstream lua_UseContainerItem's order:
//                      an item that starts a quest -> CMSG_QUESTGIVER_QUERY_QUEST(item guid, quest) (AzerothCore
//                      HandleQuestgiverQueryQuestOpcode takes TYPEMASK_ITEM); a readable item (PageText /
//                      ITEM_FIELD_FLAG_READABLE) is read; an equippable item (the item query's InventoryType != 0,
//                      never a quest-class item) -> CMSG_AUTOEQUIP_ITEM; a lootable one (ITEM_FLAG_HAS_LOOT) or a
//                      wrapped gift -> CMSG_OPEN_ITEM(bag, slot) (AzerothCore HandleOpenItemOpcode); else the use.
//   the use            (upstream dispatchUseItem) no ON_USE spell - nothing (AzerothCore HandleUseItemOpcode ignores an
//                      unknown spell); an equippable item must be worn (AzerothCore: InventoryType != NON_EQUIP and
//                      not equipped -> EQUIP_ERR_ITEM_NOT_FOUND, said here first); bind on use not yet bound ->
//                      USE_BIND_CONFIRM; the mount being ridden dismounts; a glyph waits for a socket; then the cast.
//                      The level, class and other CanUseItem checks are the server's (its SMSG_INVENTORY_CHANGE_FAILURE).
//   the targets        what the spell asks for (psp_spell_targets.cpp, AzerothCore's explicit target mask) and what
//                      fills it (upstream targetGuidForUseItem): the unit named or the selection when it may take the
//                      spell (an enemy for an enemy spell, a non-hostile unit for a friendly one), else the caster;
//                      an item request waits for the next item clicked (SpellIsTargeting -> SpellTargetItem).
//   the request        CMSG_USE_ITEM: bag, slot, cast count 0 (upstream UseItemPacket), spell, item guid, glyph index,
//                      cast flags 0, targets.
//   the cooldowns      AzerothCore Spell::_cast starts the cooldown (SendSpellCooldown) as it sends SMSG_SPELL_GO, so an
//                      own SMSG_SPELL_GO whose cast item is ours starts it here: the item template's cooldown /
//                      category / category cooldown of that spell, Spell.dbc's when both cooldowns are -1, the spell's
//                      own time the category's when it has none (AzerothCore Player::AddSpellAndCategoryCooldowns);
//                      on hold until SMSG_COOLDOWN_EVENT for SpellCategory flag 0x4 (category 4, the potions) or
//                      SPELL_ATTR0 0x02000000 (AzerothCore SpellInfo::IsCooldownStartedOnEvent);
//                      SMSG_ITEM_COOLDOWN (item guid, spell): 30 s (AzerothCore Player::ApplyEquipCooldown);
//                      SMSG_COOLDOWN_EVENT starts what was on hold, SMSG_CLEAR_COOLDOWN drops it, the guid in them is
//                      not a filter (upstream handleCooldownEvent); SMSG_INITIAL_SPELLS carries the running ones.
//   GetItemCooldown    upstream itemUseCooldown: the item's use spell's cooldown; the category cooldown reaches every
//                      item spell of that category (AzerothCore AddSpellAndCategoryCooldowns).
// PSP (`PSP:` below): no item cursor (the next item clicked is the target), no ground cursor (a ground spell lands
// on the selection or on us), no gift wrapping, charters, reading; the GCD of item spells is not drawn
// (StartRecoveryCategory is not in spells.pak); a category from Spell.dbc (template category -1) is not known
// (spells.pak has no Category).
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/inventory_slots.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_strings.hpp"  // GlobalStrings keys

#include <algorithm>
#include <cstdio>

namespace wowee::platform::psp {

namespace {

using game::Inventory;
namespace slots = game::slots;

constexpr uint32_t kItemFlagHasLoot = 0x00000004;   // ITEM_FLAG_HAS_LOOT (OPENABLE): clams, lockboxes, bags of loot
constexpr uint32_t kItemFlagWrapper = 0x00000200;   // ITEM_FLAG_WRAPPER: gift paper / a wrapped gift
constexpr uint32_t kItemFlagCharter = 0x00002000;   // ITEM_FLAG_IS_CHARTER (guild / arena charters)
constexpr uint32_t kFieldFlagSoulbound = 0x00000001;  // ITEM_FIELD_FLAGS
constexpr uint32_t kFieldFlagWrapped = 0x00000008;
constexpr uint32_t kFieldFlagReadable = 0x00000200;
constexpr uint32_t kCategoryCooldownOnEvent = 4;    // SpellCategory.dbc: the only row with flag 0x4 (the potions)
constexpr uint32_t kAttr0CooldownOnEvent = 0x02000000;  // SPELL_ATTR0_COOLDOWN_ON_EVENT (AzerothCore)
constexpr uint32_t kEquipCooldownMs = 30000;        // AzerothCore Player::ApplyEquipCooldown: 30 * IN_MILLISECONDS
constexpr size_t kMaxItemCooldowns = 64;
constexpr uint32_t kTriggerOnUse = 0;
constexpr uint8_t kItemClassQuest = 12;
constexpr uint8_t kItemClassConsumable = 0;
constexpr uint8_t kSpellFailedBadTargets = 11, kSpellFailedMoving = 51;

unsigned lo32(uint64_t g) { return static_cast<unsigned>(g & 0xFFFFFFFFu); }
bool isGameObjectGuid(uint64_t g) { return (g >> 48) == 0xF110u || (g >> 48) == 0xF120u; }
bool isItemGuid(uint64_t g) { return (g >> 48) == 0x4000u || (g >> 48) == 0x4100u; }

// Remaining milliseconds of a span that started at `start` (logUptimeMs clock), 0 when over.
uint32_t leftMs(uint32_t start, uint32_t dur, uint32_t now) {
    const uint32_t gone = now - start;
    return gone < dur ? dur - gone : 0;
}

}  // namespace

// --- Lookups ---------------------------------------------------------------------------------------------------

// The item at a wire position and its object guid (worn 0..18 / backpack 23..38 inside 0xFF, or a worn bag).
bool WorldSession::itemAtWire(uint8_t bag, uint8_t slot, game::ItemDef& def, uint64_t& guid) const {
    const Inventory& inv = *inventory_;
    guid = 0;
    if (bag == slots::kNoContainer) {
        if (slot < Inventory::NUM_EQUIP_SLOTS) {
            const auto& s = inv.getEquipSlot(static_cast<game::EquipSlot>(slot));
            if (s.empty()) return false;
            def = s.item;
            guid = def.guid ? def.guid : equipSlotGuids_[slot];
            return true;
        }
        const int index = static_cast<int>(slot) - slots::kBackpackFirst;
        if (index < 0 || index >= inv.getBackpackSize() || inv.getBackpackSlot(index).empty()) return false;
        def = inv.getBackpackSlot(index).item;
        guid = def.guid ? def.guid : getBackpackItemGuid(index);
        return true;
    }
    const int bagIndex = static_cast<int>(bag) - slots::kWornBagFirst;
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS || slot >= inv.getBagSize(bagIndex)) return false;
    const auto& s = inv.getBagSlot(bagIndex, slot);
    if (s.empty()) return false;
    def = s.item;
    guid = def.guid ? def.guid : getBagItemGuid(bagIndex, slot);
    return true;
}

int WorldSession::itemSpellIndex(const game::ItemQueryResponseData& info, uint32_t spellId) const {
    for (int i = 0; i < 5; ++i)
        if (info.spells[static_cast<size_t>(i)].spellId == spellId) return i;
    return -1;
}

// The use spell (asked with no effect): the first spell with SpellTrigger ON_USE.
uint32_t WorldSession::itemUseSpell(uint32_t entry) const {
    const auto* info = getItemInfo(entry);
    if (!info) return 0;
    for (const auto& sp : info->spells)
        if (sp.spellId != 0 && sp.spellTrigger == kTriggerOnUse) return sp.spellId;
    return 0;
}

uint32_t WorldSession::itemCount(uint32_t entry) const {
    if (entry == 0) return 0;
    const Inventory& inv = *inventory_;
    uint32_t n = 0;
    for (int s = 0; s < Inventory::FIRST_BAG_EQUIP_SLOT; ++s) {  // the worn items (not the worn bags)
        const auto& es = inv.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (!es.empty() && es.item.itemId == entry) n += std::max<uint32_t>(1, es.item.stackCount);
    }
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        const auto& bs = inv.getBackpackSlot(i);
        if (!bs.empty() && bs.item.itemId == entry) n += std::max<uint32_t>(1, bs.item.stackCount);
    }
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b)
        for (int i = 0; i < inv.getBagSize(b); ++i) {
            const auto& bs = inv.getBagSlot(b, i);
            if (!bs.empty() && bs.item.itemId == entry) n += std::max<uint32_t>(1, bs.item.stackCount);
        }
    return n;
}

bool WorldSession::itemEquipped(uint32_t entry) const {
    const Inventory& inv = *inventory_;
    for (int s = 0; s < Inventory::FIRST_BAG_EQUIP_SLOT; ++s) {
        const auto& es = inv.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (!es.empty() && es.item.itemId == entry) return true;
    }
    return false;
}

// The inventory type of the item query (upstream lua_UseContainerItem); a quest-class item is never equipped.
bool WorldSession::itemEquippable(uint32_t entry) const {
    const auto* info = getItemInfo(entry);
    return info && info->inventoryType != 0 && info->itemClass != kItemClassQuest;
}

// IsConsumableAction: a consumable-class item (upstream lua_system_api.cpp IsConsumableAction).
bool WorldSession::itemConsumable(uint32_t entry) const {
    const auto* info = getItemInfo(entry);
    return info && info->itemClass == kItemClassConsumable;
}

bool WorldSession::itemStackable(uint32_t entry) const {  // IsStackableAction: Stackable > 1 (upstream)
    const auto* info = getItemInfo(entry);
    return info && info->maxStack > 1;
}

// --- Using ------------------------------------------------------------------------------------------------------

void WorldSession::itemUseError(const char* text) {
    if (!text || !*text) return;
    spellError(text);  // the red UIErrorsFrame line (takeSpellError)
    if (++itemUseLogs_ <= 200) PSP_LOGI(tag::Net, "item_use_refused text=%s", text);
}

WorldSession::ItemUse WorldSession::useContainerItem(int bag, int slot, uint64_t unitTarget) {
    if (state_ != State::InWorld || !socket_) return ItemUse::None;
    uint8_t wireBag = slots::kNoContainer, wireSlot = 0;
    if (bag == 0) {
        wireSlot = static_cast<uint8_t>(slots::kBackpackFirst + slot);
    } else {
        wireBag = static_cast<uint8_t>(slots::kWornBagFirst + bag - 1);
        wireSlot = static_cast<uint8_t>(slot);
    }
    game::ItemDef def;
    uint64_t guid = 0;
    if (!itemAtWire(wireBag, wireSlot, def, guid)) return ItemUse::None;
    const auto* info = getItemInfo(def.itemId);
    if (!info) {
        queryItemInfo(def.itemId, guid);
        return ItemUse::NoInfo;
    }
    auto online = guid ? onlineItems_.find(guid) : onlineItems_.end();
    const uint32_t fieldFlags = online != onlineItems_.end() ? online->second.flags : (def.soulbound ? kFieldFlagSoulbound : 0);
    // An item that starts a quest offers it; SMSG_QUESTGIVER_QUEST_DETAILS opens the quest window.
    if (info->startQuestId != 0 && guid != 0) {
        socket_->send(game::QuestgiverQueryQuestPacket::build(guid, info->startQuestId));
        PSP_LOGI(tag::Net, "item_quest entry=%u quest=%u guid=%08X", static_cast<unsigned>(def.itemId),
                 static_cast<unsigned>(info->startQuestId), lo32(guid));
        return ItemUse::Quest;
    }
    if (info->pageTextId != 0 || (fieldFlags & kFieldFlagReadable)) return ItemUse::Readable;
    if (itemEquippable(def.itemId)) {
        if (bag == 0) autoEquipItemBySlot(slot);
        else autoEquipItemInBag(bag - 1, slot);
        return ItemUse::Equip;
    }
    if ((info->itemFlags & kItemFlagHasLoot) || (fieldFlags & kFieldFlagWrapped)) {
        socket_->send(game::OpenItemPacket::build(wireBag, wireSlot));
        PSP_LOGI(tag::Net, "item_open bag=%u slot=%u entry=%u gift=%d", static_cast<unsigned>(wireBag),
                 static_cast<unsigned>(wireSlot), static_cast<unsigned>(def.itemId), (fieldFlags & kFieldFlagWrapped) ? 1 : 0);
        return ItemUse::Opened;
    }
    return useItemAt(wireBag, wireSlot, unitTarget, false);
}

// The use (upstream dispatchUseItem), from a bag, a worn slot, the action bar or the bind confirmation.
WorldSession::ItemUse WorldSession::useItemAt(uint8_t wireBag, uint8_t wireSlot, uint64_t unitTarget, bool confirmed) {
    if (state_ != State::InWorld || !socket_) return ItemUse::None;
    game::ItemDef def;
    uint64_t guid = 0;
    if (!itemAtWire(wireBag, wireSlot, def, guid) || guid == 0) return ItemUse::None;
    const auto* info = getItemInfo(def.itemId);
    if (!info) {
        queryItemInfo(def.itemId, guid);
        return ItemUse::NoInfo;
    }
    auto online = onlineItems_.find(guid);
    const uint32_t fieldFlags = online != onlineItems_.end() ? online->second.flags : (def.soulbound ? kFieldFlagSoulbound : 0);
    const bool log = ++itemUseLogs_ <= 200;
    if (log)
        PSP_LOGI(tag::Net, "item_use_click bag=%u slot=%u entry=%u class=%u flags=%X fflags=%X quest=%u",
                 static_cast<unsigned>(wireBag), static_cast<unsigned>(wireSlot), static_cast<unsigned>(def.itemId),
                 static_cast<unsigned>(info->itemClass), static_cast<unsigned>(info->itemFlags),
                 static_cast<unsigned>(fieldFlags), static_cast<unsigned>(info->startQuestId));
    if (info->itemFlags & kItemFlagCharter) return ItemUse::None;  // PSP: PetitionFrame - not on the PSP
    if ((info->itemFlags & kItemFlagWrapper) && !(fieldFlags & kFieldFlagWrapped)) return ItemUse::None;  // PSP: no wrapping
    const uint32_t spellId = itemUseSpell(def.itemId);
    if (spellId == 0) return ItemUse::None;  // AzerothCore HandleUseItemOpcode ignores a use without a known spell
    // AzerothCore HandleUseItemOpcode: an item with an inventory type is used only while worn (an equipment slot).
    if (info->inventoryType != 0 && !(wireBag == slots::kNoContainer && wireSlot < Inventory::FIRST_BAG_EQUIP_SLOT)) {
        itemUseError(CLIENT_STR(ERR_ITEM_NOT_FOUND));
        return ItemUse::Error;
    }
    PendingItemUse use;
    use.active = true;
    use.bag = wireBag;
    use.slot = wireSlot;
    use.itemGuid = guid;
    use.entry = def.itemId;
    use.spellId = spellId;
    use.unitTarget = unitTarget;
    // Bind on use (Bonding 3) and not bound yet: USE_BIND_CONFIRM -> StaticPopup USE_BIND (upstream dispatchUseItem).
    if (!confirmed && info->bindType == 3 && !(fieldFlags & kFieldFlagSoulbound)) {
        pendingUse_ = use;
        useBindPrompt_ = true;  // (not an addon event: the built-in popup answers it, as AUTOEQUIP_BIND)
        return ItemUse::Confirm;
    }
    // Upstream: the mount being ridden, used again from its item, dismounts (pre-3.3.5a mount items).
    if (isMounted() && spellId == mountAuraSpellId_) {
        dismount();
        return ItemUse::Sent;
    }
    if (info->itemClass == 16) return glyphUseItem(use);  // USE_GLYPH -> GlyphFrame's socket
    return castItemSpell(use, *info);
}

// The cast with the targets the spell asks for.
WorldSession::ItemUse WorldSession::castItemSpell(const PendingItemUse& use, const game::ItemQueryResponseData& info) {
    (void)info;
    const SpellInfo* si = spellInfo(use.spellId);
    // A spell with a cast time is refused on the move (SPELL_FAILED_MOVING), as castSpell (upstream) does.
    constexpr uint32_t kHorizontal = 0x1u | 0x2u | 0x4u | 0x8u;  // FORWARD, BACKWARD, STRAFE_LEFT / RIGHT
    if (si && si->rec.castMs > 0 && (lastOwnMove_.flags & kHorizontal) != 0) {
        const char* t = spellDb_ ? spellDb_->castFailText(kSpellFailedMoving) : nullptr;
        itemUseError(t ? t : CLIENT_STR(SPELL_FAILED_MOVING));
        return ItemUse::Error;
    }
    SpellCastTargets targets;  // mask 0: the caster
    if (!si) {
        // Upstream targetGuidForUseItem: a spell spells.pak does not know (a realm's own item) - the selection when
        // there is one, else the caster.
        const uint64_t t = use.unitTarget ? use.unitTarget : targetGuid_;
        if (t != 0 && t != playerGuid_) targets = SpellCastTargets::unit(t);
        sendUseItem(use, targets);
        return ItemUse::Sent;
    }
    const uint32_t req = spellTargetRequest(si->rec.targets, si->rec.implicitTargetA);
    const glm::vec3 own(lastOwnMove_.x, lastOwnMove_.y, lastOwnMove_.z);
    if (req & kTargetMaskUnitPick) {
        // Upstream targetGuidForUseItem: the unit named or the selection when it may take the spell, else the caster.
        const uint64_t cand = use.unitTarget ? use.unitTarget : targetGuid_;
        targets = SpellCastTargets::unit(cand != 0 && unitFitsTargetRequest(cand, req) ? cand : playerGuid_);
    } else if (req & (kTargetFlagItem | kTargetFlagGameObjectItem | kTargetFlagTradeItem)) {
        // SpellIsTargeting: the item waits for its target item (PSP: the next item clicked in a window).
        itemTargeting_ = use;
        itemTargeting_.active = true;
        PSP_LOGI(tag::Net, "item_use_pick entry=%u spell=%u", static_cast<unsigned>(use.entry),
                 static_cast<unsigned>(use.spellId));
        return ItemUse::PickItem;
    } else if (req & kTargetFlagGameObject) {
        if (!isGameObjectGuid(targetGuid_)) {  // PSP: the selected object (no cursor)
            itemUseError(CLIENT_STR(ERR_GENERIC_NO_TARGET));
            return ItemUse::Error;
        }
        targets = SpellCastTargets::gameObject(targetGuid_);
    } else if (req & kTargetFlagDestLocation) {
        // PSP: no ground cursor - the selected unit's feet, else ours.
        glm::vec3 p = own;
        if (targetGuid_ != 0)
            if (auto e = entities_.getEntity(targetGuid_))
                p = core::coords::canonicalToServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
        targets = SpellCastTargets::dest(p.x, p.y, p.z);
    } else if (req & (kTargetFlagCorpseAlly | kTargetFlagCorpseEnemy)) {
        const char* bad = spellDb_ ? spellDb_->castFailText(kSpellFailedBadTargets) : nullptr;
        itemUseError(bad ? bad : CLIENT_STR(SPELL_FAILED_BAD_TARGETS));  // PSP: no corpse pick
        return ItemUse::Error;
    }
    if (req & kTargetFlagSourceLocation) targets.withSource(own.x, own.y, own.z);  // own rule: from where we stand
    sendUseItem(use, targets);
    return ItemUse::Sent;
}

// The units a request may take (upstream targetGuidForUseItem): any unit (UNIT, PASSENGER), a hostile one (ENEMY),
// one that is not hostile (ALLY / PARTY / RAID - the membership is the server's check), a companion (MINIPET).
bool WorldSession::unitFitsTargetRequest(uint64_t guid, uint32_t mask) const {
    if (guid == 0) return false;
    const bool self = guid == playerGuid_;
    if (!self) {
        auto e = entities_.getEntity(guid);
        if (!e || (e->getType() != game::ObjectType::UNIT && e->getType() != game::ObjectType::PLAYER)) return false;
    }
    if (mask & (kTargetFlagUnit | kTargetFlagUnitPassenger)) return true;
    if ((mask & kTargetFlagUnitEnemy) && !self && isHostileUnit(guid)) return true;
    if ((mask & (kTargetFlagUnitAlly | kTargetFlagUnitParty | kTargetFlagUnitRaid)) && (self || !isHostileUnit(guid)))
        return true;
    if ((mask & kTargetFlagUnitMinipet) && !self) return true;
    return false;
}

void WorldSession::sendUseItem(const PendingItemUse& use, const SpellCastTargets& targets) {
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_USE_ITEM);
    if (wire == 0xFFFF || !socket_) return;
    network::Packet p(wire);
    constexpr uint8_t kCastCount = 0;  // upstream UseItemPacket: the server only echoes it
    writeUseItemBody(p, use.bag, use.slot, kCastCount, use.spellId, use.itemGuid, 0, 0, targets);
    socket_->send(p);
    lastItemCast_ = use;
    lastItemCastMs_ = logUptimeMs();
    PSP_LOGI(tag::Net, "item_use bag=%u slot=%u entry=%u spell=%u count=%u mask=%X target=%08X",
             static_cast<unsigned>(use.bag), static_cast<unsigned>(use.slot), static_cast<unsigned>(use.entry),
             static_cast<unsigned>(use.spellId), static_cast<unsigned>(kCastCount), static_cast<unsigned>(targets.mask),
             lo32(targets.object ? targets.object : targets.item));
}

WorldSession::ItemUse WorldSession::confirmUseBind() {  // ConfirmBindOnUse
    const PendingItemUse pending = pendingUse_;
    pendingUse_ = PendingItemUse{};
    if (!pending.active) return ItemUse::None;
    game::ItemDef def;
    uint64_t guid = 0;
    if (!itemAtWire(pending.bag, pending.slot, def, guid) || guid != pending.itemGuid) return ItemUse::None;  // moved
    return useItemAt(pending.bag, pending.slot, pending.unitTarget, true);
}

WorldSession::ItemUse WorldSession::useOnItem(uint8_t wireBag, uint8_t wireSlot) {  // SpellTargetItem
    const PendingItemUse use = itemTargeting_;
    itemTargeting_ = PendingItemUse{};
    if (!use.active || state_ != State::InWorld || !socket_) return ItemUse::None;
    game::ItemDef def;
    uint64_t target = 0;
    if (!itemAtWire(wireBag, wireSlot, def, target) || target == 0) return ItemUse::None;
    game::ItemDef src;
    uint64_t srcGuid = 0;
    if (!itemAtWire(use.bag, use.slot, src, srcGuid) || srcGuid != use.itemGuid) return ItemUse::None;
    sendUseItem(use, SpellCastTargets::itemTarget(target));
    return ItemUse::Sent;
}

// UseAction on an item (upstream useItemById): the first one in the backpack, then the bags, then the worn slots -
// used, never equipped.
WorldSession::ItemUse WorldSession::useItemByEntry(uint32_t entry, uint64_t unitTarget) {
    if (entry == 0 || state_ != State::InWorld) return ItemUse::None;
    const Inventory& inv = *inventory_;
    for (int i = 0; i < inv.getBackpackSize(); ++i)
        if (!inv.getBackpackSlot(i).empty() && inv.getBackpackSlot(i).item.itemId == entry)
            return useItemAt(slots::kNoContainer, static_cast<uint8_t>(slots::kBackpackFirst + i), unitTarget);
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b)
        for (int i = 0; i < inv.getBagSize(b); ++i)
            if (!inv.getBagSlot(b, i).empty() && inv.getBagSlot(b, i).item.itemId == entry)
                return useItemAt(static_cast<uint8_t>(slots::kWornBagFirst + b), static_cast<uint8_t>(i), unitTarget);
    for (int s = 0; s < Inventory::NUM_EQUIP_SLOTS; ++s) {
        const auto& es = inv.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (!es.empty() && es.item.itemId == entry) return useItemAt(slots::kNoContainer, static_cast<uint8_t>(s), unitTarget);
    }
    return ItemUse::None;
}

namespace {
// upstream getItemIdForSpell: the slot's item has the spell as ON_USE or ON_NO_DELAY_USE.
bool slotCarriesSpell(const WorldSession& ws, const game::ItemSlot& s, uint32_t spellId) {
    if (s.empty()) return false;
    const auto* info = ws.getItemInfo(s.item.itemId);
    if (!info) return false;
    for (const auto& sp : info->spells)
        if (sp.spellId == spellId && (sp.spellTrigger == 0 || sp.spellTrigger == 5)) return true;
    return false;
}
}  // namespace

bool WorldSession::itemUseSpellCarried(uint32_t spellId) const {
    if (spellId == 0) return false;
    const Inventory& inv = *inventory_;
    for (int i = 0; i < inv.getBackpackSize(); ++i)
        if (slotCarriesSpell(*this, inv.getBackpackSlot(i), spellId)) return true;
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b)
        for (int i = 0; i < inv.getBagSize(b); ++i)
            if (slotCarriesSpell(*this, inv.getBagSlot(b, i), spellId)) return true;
    return false;
}

bool WorldSession::useItemForSpell(uint32_t spellId) {
    if (spellId == 0) return false;
    const Inventory& inv = *inventory_;
    auto carries = [&](const game::ItemSlot& s) { return slotCarriesSpell(*this, s, spellId); };
    for (int i = 0; i < inv.getBackpackSize(); ++i)
        if (carries(inv.getBackpackSlot(i))) {
            useItemAt(slots::kNoContainer, static_cast<uint8_t>(slots::kBackpackFirst + i));
            return true;
        }
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b)
        for (int i = 0; i < inv.getBagSize(b); ++i)
            if (carries(inv.getBagSlot(b, i))) {
                useItemAt(static_cast<uint8_t>(slots::kWornBagFirst + b), static_cast<uint8_t>(i));
                return true;
            }
    return false;
}

void WorldSession::castSpellAt(uint32_t spellId, const SpellCastTargets& targets) {
    if (state_ != State::InWorld || !socket_ || spellId == 0) return;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_CAST_SPELL);
    if (wire == 0xFFFF) return;
    network::Packet p(wire);
    writeCastSpellBody(p, ++castCount_, spellId, 0, targets);
    socket_->send(p);
    requestedSpellId_ = spellId;  // the instant-cast pose, as castSpell
    requestedAtMs_ = logUptimeMs();
    PSP_LOGI(tag::Net, "cast_spell_at id=%u count=%u mask=%X target=%08X", static_cast<unsigned>(spellId),
             static_cast<unsigned>(castCount_), static_cast<unsigned>(targets.mask),
             lo32(targets.object ? targets.object : targets.item));
}

// --- Cooldowns ---------------------------------------------------------------------------------------------------

void WorldSession::addItemCooldown(const ItemCooldown& cd) {
    const uint32_t now = logUptimeMs();
    itemCooldowns_.erase(std::remove_if(itemCooldowns_.begin(), itemCooldowns_.end(),
                                        [&](const ItemCooldown& e) {
                                            if (e.spellId == cd.spellId && e.entry == cd.entry) return true;
                                            return !e.onHold && leftMs(e.startMs, e.durationMs, now) == 0 &&
                                                   leftMs(e.catStartMs, e.catDurationMs, now) == 0;
                                        }),
                         itemCooldowns_.end());
    if (itemCooldowns_.size() >= kMaxItemCooldowns) itemCooldowns_.erase(itemCooldowns_.begin());
    itemCooldowns_.push_back(cd);
    ++itemUseVersion_;
}

// An own SMSG_SPELL_GO of an item cast (AzerothCore Player::AddSpellAndCategoryCooldowns): the template's values for
// that spell; Spell.dbc's when both cooldowns are -1; the spell's own time is the category's when it has none.
void WorldSession::startItemCooldown(uint32_t entry, uint32_t spellId) {
    const auto* info = getItemInfo(entry);
    if (!info) return;
    const int idx = itemSpellIndex(*info, spellId);
    if (idx < 0) return;
    int32_t rec = -1, cat = 0, catRec = -1;
    if (const ItemTipExtra* x = itemTipExtra(entry)) {
        rec = x->spellCooldown[idx];
        cat = x->spellCategory[idx];
        catRec = x->spellCategoryCooldown[idx];
    }
    const SpellInfo* si = spellInfo(spellId);
    if (rec < 0 && catRec < 0) {
        cat = 0;  // PSP: Spell.dbc Category is not in spells.pak
        rec = si ? static_cast<int32_t>(si->rec.recoveryMs) : 0;
        catRec = si ? static_cast<int32_t>(si->rec.categoryRecoveryMs) : 0;
    }
    if (cat < 1) cat = 0;
    rec = std::max<int32_t>(rec, 0);
    catRec = std::max<int32_t>(catRec, 0);
    // On hold until SMSG_COOLDOWN_EVENT (AzerothCore SpellInfo::IsCooldownStartedOnEvent).
    const bool onHold = static_cast<uint32_t>(cat) == kCategoryCooldownOnEvent ||
                        (si && (si->rec.attributes & kAttr0CooldownOnEvent) != 0);
    if (rec == 0 && catRec == 0 && !onHold) return;
    const uint32_t now = logUptimeMs();
    ItemCooldown cd;
    cd.spellId = spellId;
    cd.entry = entry;
    cd.category = static_cast<uint32_t>(cat);
    cd.startMs = cd.catStartMs = now;
    cd.durationMs = static_cast<uint32_t>(rec > 0 ? rec : catRec);
    cd.catDurationMs = cat != 0 ? static_cast<uint32_t>(catRec) : 0u;
    cd.onHold = onHold;
    addItemCooldown(cd);
    if (++itemUseLogs_ <= 200)
        PSP_LOGI(tag::Net, "item_cooldown entry=%u spell=%u ms=%u cat=%u cat_ms=%u hold=%d", static_cast<unsigned>(entry),
                 static_cast<unsigned>(spellId), static_cast<unsigned>(cd.durationMs), static_cast<unsigned>(cd.category),
                 static_cast<unsigned>(cd.catDurationMs), onHold ? 1 : 0);
}

// GetItemCooldown (upstream itemUseCooldown): the cooldown of the item's use spell; a category cooldown reaches every
// item spell of that category (AzerothCore AddSpellAndCategoryCooldowns).
bool WorldSession::itemCooldown(uint32_t entry, float& remaining, float& total, bool& onHold) const {
    remaining = total = 0.0f;
    onHold = false;
    const uint32_t spell = itemUseSpell(entry);
    if (spell == 0 || itemCooldowns_.empty()) return false;
    uint32_t cat = 0;
    if (const auto* info = getItemInfo(entry)) {
        const int idx = itemSpellIndex(*info, spell);
        const ItemTipExtra* x = itemTipExtra(entry);
        if (x && idx >= 0 && x->spellCategory[idx] > 0) cat = static_cast<uint32_t>(x->spellCategory[idx]);
    }
    const uint32_t now = logUptimeMs();
    uint32_t bestLeft = 0, bestTotal = 0;
    for (const auto& e : itemCooldowns_) {
        const bool own = e.spellId == spell;
        const bool shared = cat != 0 && e.category == cat && e.catDurationMs > 0;
        if (!own && !shared) continue;
        if (e.onHold) {
            onHold = true;
            continue;
        }
        if (own) {
            const uint32_t l = leftMs(e.startMs, e.durationMs, now);
            if (l > bestLeft) { bestLeft = l; bestTotal = e.durationMs; }
        }
        if (shared) {
            const uint32_t l = leftMs(e.catStartMs, e.catDurationMs, now);
            if (l > bestLeft) { bestLeft = l; bestTotal = e.catDurationMs; }
        }
    }
    remaining = static_cast<float>(bestLeft) / 1000.0f;
    total = static_cast<float>(bestTotal) / 1000.0f;
    return onHold || bestLeft > 0;
}

bool WorldSession::takeStandState(uint8_t& state) {
    if (standStates_.empty()) return false;
    state = standStates_.front();
    standStates_.pop_front();
    return true;
}

// --- Packets -----------------------------------------------------------------------------------------------------

bool WorldSession::handleItemUsePacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_ITEM_COOLDOWN: {  // item guid, spell -> 30 s (AzerothCore Player::ApplyEquipCooldown)
            if (!packet.hasRemaining(12)) return true;
            const uint64_t guid = packet.readUInt64();
            const uint32_t spellId = packet.readUInt32();
            const uint32_t entry = getItemIdByGuid(guid);
            if (entry != 0) {
                ItemCooldown cd;
                cd.spellId = spellId;
                cd.entry = entry;
                cd.startMs = cd.catStartMs = logUptimeMs();
                cd.durationMs = kEquipCooldownMs;
                addItemCooldown(cd);
            }
            if (++itemUseLogs_ <= 200)
                PSP_LOGI(tag::Net, "item_cooldown_equip guid=%08X entry=%u spell=%u", lo32(guid), static_cast<unsigned>(entry),
                         static_cast<unsigned>(spellId));
            return true;
        }
        case LogicalOpcode::SMSG_STANDSTATE_UPDATE: {  // upstream standStateCallback (the realm's SetStandState)
            if (!packet.hasRemaining(1)) return true;
            const uint8_t st = packet.readUInt8();
            if (standStates_.size() >= 8) standStates_.pop_front();
            standStates_.push_back(st);
            if (++itemUseLogs_ <= 200) PSP_LOGI(tag::Net, "stand_state_update state=%u", static_cast<unsigned>(st));
            return true;
        }
        case LogicalOpcode::SMSG_SPELL_GO: {  // peek: an own item cast starts the item's cooldown
            // (read in place and rewound - no copy of every SPELL_GO of a busy city)
            const size_t pos = packet.getReadPos();
            uint64_t castItem = 0, caster = 0;
            uint32_t spellId = 0;
            bool ok = packet.hasFullPackedGuid();
            if (ok) castItem = packet.readPackedGuid();
            ok = ok && packet.hasFullPackedGuid();
            if (ok) caster = packet.readPackedGuid();
            ok = ok && packet.hasRemaining(5);
            if (ok) {
                packet.readUInt8();  // cast count
                spellId = packet.readUInt32();
            }
            packet.setReadPos(pos);
            if (!ok || caster != playerGuid_ || castItem == caster || castItem == 0 || !isItemGuid(castItem)) return false;
            uint32_t entry = getItemIdByGuid(castItem);
            if (entry == 0 && lastItemCast_.itemGuid == castItem) entry = lastItemCast_.entry;  // used up already
            const auto* inf = entry != 0 ? getItemInfo(entry) : nullptr;
            if (inf && itemSpellIndex(*inf, spellId) >= 0) startItemCooldown(entry, spellId);
            return false;
        }
        case LogicalOpcode::SMSG_COOLDOWN_EVENT: {  // peek: what was on hold starts now
            // The guid after the spell is not a filter (upstream handleCooldownEvent reads and ignores it).
            const size_t pos = packet.getReadPos();
            if (!packet.hasRemaining(4)) return false;
            const uint32_t spellId = packet.readUInt32();
            packet.setReadPos(pos);
            const uint32_t now = logUptimeMs();
            for (auto& e : itemCooldowns_)
                if (e.spellId == spellId && e.onHold) {
                    e.onHold = false;
                    e.startMs = e.catStartMs = now;
                    ++itemUseVersion_;
                }
            return false;
        }
        case LogicalOpcode::SMSG_CLEAR_COOLDOWN: {  // peek: the cooldown cleared (any guid, as upstream handleClearCooldown)
            const size_t pos = packet.getReadPos();
            if (!packet.hasRemaining(4)) return false;
            const uint32_t spellId = packet.readUInt32();
            packet.setReadPos(pos);
            const size_t before = itemCooldowns_.size();
            itemCooldowns_.erase(std::remove_if(itemCooldowns_.begin(), itemCooldowns_.end(),
                                                [&](const ItemCooldown& e) { return e.spellId == spellId; }),
                                 itemCooldowns_.end());
            if (itemCooldowns_.size() != before) ++itemUseVersion_;
            return false;
        }
        case LogicalOpcode::SMSG_INITIAL_SPELLS: {  // peek: the running cooldowns of item spells (item + category)
            const size_t pos = packet.getReadPos();
            game::InitialSpellsData data;
            const bool parsed = game::InitialSpellsParser::parse(packet, data);
            packet.setReadPos(pos);
            if (!parsed) return false;
            const uint32_t now = logUptimeMs();
            int n = 0;
            for (const auto& c : data.cooldowns) {
                if (c.itemId == 0 || (c.cooldownMs == 0 && c.categoryCooldownMs == 0)) continue;
                ItemCooldown cd;
                cd.spellId = c.spellId;
                cd.entry = c.itemId;
                cd.category = c.categoryId;
                cd.startMs = cd.catStartMs = now;
                cd.durationMs = c.cooldownMs;
                cd.catDurationMs = c.categoryCooldownMs;
                addItemCooldown(cd);
                ++n;
            }
            PSP_LOGI(tag::Net, "item_cooldowns_initial n=%d", n);
            return false;
        }
        case LogicalOpcode::SMSG_LOGIN_VERIFY_WORLD:  // peek: a new character - nothing of the last one stays
            itemCooldowns_.clear();
            pendingUse_ = itemTargeting_ = lastItemCast_ = PendingItemUse{};
            standStates_.clear();
            useBindPrompt_ = false;
            ++itemUseVersion_;
            return false;
        default:
            return false;
    }
}

}  // namespace wowee::platform::psp
