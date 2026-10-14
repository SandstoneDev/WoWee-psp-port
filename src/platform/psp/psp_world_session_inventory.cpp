// inventory and bags for the PSP world session. GameHandler /
// InventoryHandler / EntityController are not built for the PSP (they pull in
// rendering, audio and the addon layer), so the steps the inventory needs are
// mirrored here from upstream, with upstream's packet builders, parsers and
// the game::Inventory model (src/game/inventory.cpp) unchanged:
//   - EntityController::trackItemOnCreate / updateItemOnValuesUpdate /
//     processOutOfRangeObjects / handleDestroyObject: ITEM and CONTAINER
//     objects (onlineItems_, containerContents_);
//   - EntityController::applyPlayerStatFields (coinage) and the own player's
//     slot fields: InventoryHandler::detectInventorySlotBases /
//     applyInventoryFields / extractContainerFields / buildItemDef /
//     rebuildOnlineInventory / fireBagUpdates;
//   - InventoryHandler::queryItemInfo / handleItemQueryResponse (the item
//     cache), SMSG_ITEM_PUSH_RESULT, SMSG_INVENTORY_CHANGE_FAILURE;
//   - InventoryHandler::autoEquipItemBySlot / autoEquipItemInBag /
//     equipPendingItem / equipItemToSlot / unequipToBackpack /
//     swapContainerItems / swapBagSlots / destroyItem / splitItem(To) /
//     readWireSlot / writeWireSlot / resolvePlayerEquipment.
// The names are GameHandler's, so the Lua API (task A) can call them as the
// upstream bindings call GameHandler; the addon events upstream fires are
// queued (takeInventoryEvent). The temporary windows are ui_world_inventory.cpp.
#include "platform/psp/psp_world_session.hpp"

#include "game/game_utils.hpp"
#include "game/inventory_slots.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's GlobalStrings

#include <algorithm>
#include <cstdio>

namespace wowee::platform::psp {

namespace {

using game::Inventory;
constexpr size_t kMaxInventoryEvents = 64;
constexpr uint32_t kItemFlagSoulbound = 0x1u;  // ITEM_FIELD_FLAGS bit (upstream buildItemDef)

uint64_t setGuidHalf(uint64_t guid, bool low, uint32_t val) {
    return low ? ((guid & 0xFFFFFFFF00000000ULL) | val) : ((guid & 0x00000000FFFFFFFFULL) | (uint64_t(val) << 32));
}

// SMSG_INVENTORY_CHANGE_FAILURE text, upstream's table code for code. PSP: the client's own ERR_* strings of the
// running language (strings.pak) where the code has one without arguments; upstream writes English.
// 1 (level) is the caller's (ERR_CANT_EQUIP_LEVEL_I with the level); the limit categories and the purchase level
// carry values the handler does not read - the port's own lines.
const char* inventoryErrorText(uint8_t error) {
    switch (error) {
        case 1: return OWN_STR(INV_ERR_LEVEL_HIGHER);
        case 2: return CLIENT_STR(ERR_CANT_EQUIP_SKILL);
        case 3: return CLIENT_STR(ERR_WRONG_SLOT);
        case 4: return CLIENT_STR(ERR_BAG_FULL);
        case 5: return CLIENT_STR(ERR_BAG_IN_BAG);
        case 6: return CLIENT_STR(ERR_TRADE_EQUIPPED_BAG);
        case 7: return CLIENT_STR(ERR_AMMO_ONLY);
        case 8: return CLIENT_STR(ERR_PROFICIENCY_NEEDED);
        case 9: return CLIENT_STR(ERR_NO_SLOT_AVAILABLE);
        case 10: return CLIENT_STR(ERR_CANT_EQUIP_EVER);
        case 11: return CLIENT_STR(ERR_CANT_EQUIP_EVER);
        case 12: return CLIENT_STR(ERR_NO_SLOT_AVAILABLE);
        case 13: return CLIENT_STR(ERR_2HANDED_EQUIPPED);
        case 14: return CLIENT_STR(ERR_2HSKILLNOTFOUND);
        case 15: return CLIENT_STR(ERR_WRONG_BAG_TYPE);
        case 16: return CLIENT_STR(ERR_WRONG_BAG_TYPE);
        case 17: return CLIENT_STR(ERR_ITEM_MAX_COUNT);
        case 18: return CLIENT_STR(ERR_NO_SLOT_AVAILABLE);
        case 19: return CLIENT_STR(ERR_CANT_STACK);
        case 20: return CLIENT_STR(ERR_NOT_EQUIPPABLE);
        case 21: return CLIENT_STR(ERR_CANT_SWAP);
        case 22: return CLIENT_STR(ERR_SLOT_EMPTY);
        case 23: return CLIENT_STR(ERR_ITEM_NOT_FOUND);
        case 24: return CLIENT_STR(ERR_DROP_BOUND_ITEM);
        case 25: return CLIENT_STR(ERR_OUT_OF_RANGE);
        case 26: return CLIENT_STR(ERR_TOO_FEW_TO_SPLIT);
        case 27: return CLIENT_STR(ERR_SPLIT_FAILED);
        case 28: return CLIENT_STR(ERR_SPELL_FAILED_REAGENTS_GENERIC);
        case 29: return CLIENT_STR(ERR_NOT_ENOUGH_MONEY);
        case 30: return CLIENT_STR(ERR_NOT_A_BAG);
        case 31: return CLIENT_STR(ERR_DESTROY_NONEMPTY_BAG);
        case 32: return CLIENT_STR(ERR_NOT_OWNER);
        case 33: return CLIENT_STR(ERR_ONLY_ONE_QUIVER);
        case 34: return CLIENT_STR(ERR_NO_BANK_SLOT);
        case 35: return CLIENT_STR(ERR_NO_BANK_HERE);
        case 36: return CLIENT_STR(ERR_ITEM_LOCKED);
        case 37: return CLIENT_STR(ERR_GENERIC_STUNNED);
        case 38: return CLIENT_STR(ERR_PLAYER_DEAD);
        case 39: return CLIENT_STR(ERR_CLIENT_LOCKED_OUT);
        case 40: return CLIENT_STR(ERR_INTERNAL_BAG_ERROR);
        case 41: return CLIENT_STR(ERR_ONLY_ONE_BOLT);
        case 42: return CLIENT_STR(ERR_ONLY_ONE_AMMO);
        case 43: return CLIENT_STR(ERR_CANT_WRAP_STACKABLE);
        case 44: return CLIENT_STR(ERR_CANT_WRAP_EQUIPPED);
        case 45: return CLIENT_STR(ERR_CANT_WRAP_WRAPPED);
        case 46: return CLIENT_STR(ERR_CANT_WRAP_BOUND);
        case 47: return CLIENT_STR(ERR_CANT_WRAP_UNIQUE);
        case 48: return CLIENT_STR(ERR_CANT_WRAP_BAGS);
        case 49: return CLIENT_STR(ERR_LOOT_GONE);
        case 50: return CLIENT_STR(ERR_INV_FULL);
        case 51: return CLIENT_STR(ERR_BANK_FULL);
        case 52: return CLIENT_STR(ERR_VENDOR_SOLD_OUT);
        case 53: return CLIENT_STR(ERR_BAG_FULL);
        case 54: return CLIENT_STR(ERR_ITEM_NOT_FOUND);
        case 55: return CLIENT_STR(ERR_CANT_STACK);
        case 56: return CLIENT_STR(ERR_BAG_FULL);
        case 57: return CLIENT_STR(ERR_VENDOR_SOLD_OUT);
        case 58: return CLIENT_STR(ERR_OBJECT_IS_BUSY);
        case 60: return CLIENT_STR(ERR_NOT_IN_COMBAT);
        case 61: return CLIENT_STR(ERR_NOT_WHILE_DISARMED);
        case 62: return CLIENT_STR(ERR_BAG_FULL);
        case 63: return CLIENT_STR(ERR_CANT_EQUIP_RANK);
        case 64: return CLIENT_STR(ERR_CANT_EQUIP_REPUTATION);
        case 65: return CLIENT_STR(ERR_TOO_MANY_SPECIAL_BAGS);
        case 66: return CLIENT_STR(ERR_LOOT_CANT_LOOT_THAT_NOW);
        case 67: return CLIENT_STR(ERR_ITEM_UNIQUE_EQUIPABLE);
        case 68: return CLIENT_STR(ERR_VENDOR_MISSING_TURNINS);
        case 69: return CLIENT_STR(ERR_NOT_ENOUGH_HONOR_POINTS);
        case 70: return CLIENT_STR(ERR_NOT_ENOUGH_ARENA_POINTS);
        case 71: return CLIENT_STR(ERR_ITEM_MAX_COUNT_SOCKETED);
        case 72: return CLIENT_STR(ERR_MAIL_BOUND_ITEM);
        case 73: return OWN_STR(INV_ERR_NO_SPLIT_PROSPECTING);
        case 75: return CLIENT_STR(ERR_ITEM_MAX_COUNT_EQUIPPED_SOCKETED);
        case 76: return CLIENT_STR(ERR_ITEM_UNIQUE_EQUIPPABLE_SOCKETED);
        case 77: return CLIENT_STR(ERR_TOO_MUCH_GOLD);
        case 78: return CLIENT_STR(ERR_NOT_DURING_ARENA_MATCH);
        case 79: return CLIENT_STR(ERR_TRADE_BOUND_ITEM);
        case 80: return CLIENT_STR(ERR_CANT_EQUIP_RATING);
        case 82: return CLIENT_STR(ERR_NOT_SAME_ACCOUNT);
        case 84: return OWN_STR(INV_ERR_CATEGORY_CARRY);
        case 85: return OWN_STR(INV_ERR_CATEGORY_SOCKETED);
        case 86: return CLIENT_STR(ERR_SCALING_STAT_ITEM_LEVEL_EXCEEDED);
        case 87: return OWN_STR(INV_ERR_PURCHASE_LEVEL);
        case 88: return CLIENT_STR(ERR_CANT_EQUIP_NEED_TALENT);
        case 89: return OWN_STR(INV_ERR_CATEGORY_EQUIPPED);
        // 59 is NONE and 81 asks for a bind confirmation (upstream: no text,
        // falls back to the generic line).
        default: return nullptr;
    }
}

} // namespace

// --- Lookups (GameHandler getters) ------------------------------------------------

const game::ItemQueryResponseData* WorldSession::getItemInfo(uint32_t entry) const {
    auto it = itemInfoCache_.find(entry);
    return it == itemInfoCache_.end() ? nullptr : &it->second;
}

uint64_t WorldSession::getMoneyCopper() const { return moneyCopper_; }

uint64_t WorldSession::getEquipSlotGuid(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(equipSlotGuids_.size())) return 0;
    return equipSlotGuids_[slot];
}

uint64_t WorldSession::getBackpackItemGuid(int index) const {
    if (index < 0 || index >= static_cast<int>(backpackSlotGuids_.size())) return 0;
    return backpackSlotGuids_[index];
}

uint64_t WorldSession::getBagItemGuid(int bagIndex, int slotIndex) const {
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS || slotIndex < 0) return 0;
    const uint64_t bagGuid = equipSlotGuids_[Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex];
    if (bagGuid == 0) return 0;
    auto it = containerContents_.find(bagGuid);
    if (it == containerContents_.end() || slotIndex >= static_cast<int>(it->second.numSlots)) return 0;
    return it->second.slotGuids[slotIndex];
}

uint32_t WorldSession::getItemIdByGuid(uint64_t guid) const {
    auto it = onlineItems_.find(guid);
    return it == onlineItems_.end() ? 0u : it->second.entry;
}

uint64_t WorldSession::resolveOnlineItemGuid(uint32_t itemId) const {
    if (itemId == 0) return 0;
    uint64_t candidate = 0;
    for (const auto& [guid, info] : onlineItems_) {
        if (info.entry != itemId) continue;
        if (candidate != 0) return 0;  // ambiguous: item ids are templates, not identities
        candidate = guid;
    }
    return candidate;
}

bool WorldSession::resolvePlayerEquipment(uint32_t (&displayIds)[19], uint8_t (&invTypes)[19], int* unresolved) const {
    if (unresolved) *unresolved = 0;
    int resolved = 0;
    for (int slot = 0; slot < 19; ++slot) {
        displayIds[slot] = 0;
        invTypes[slot] = 0;
        const auto& equipped = inventory_->getEquipSlot(static_cast<game::EquipSlot>(slot));
        if (equipped.empty()) continue;
        uint32_t displayId = equipped.item.displayInfoId;
        uint8_t invType = equipped.item.inventoryType;
        if (displayId == 0) {
            auto infoIt = itemInfoCache_.find(equipped.item.itemId);
            if (infoIt == itemInfoCache_.end()) {
                if (unresolved) ++*unresolved;  // PSP: the caller waits for the answer
                continue;
            }
            displayId = infoIt->second.displayInfoId;
            invType = static_cast<uint8_t>(infoIt->second.inventoryType);
        }
        if (displayId == 0) continue;
        displayIds[slot] = displayId;
        invTypes[slot] = invType;
        ++resolved;
    }
    return resolved > 0;
}

// PSP: the own PLAYER_VISIBLE_ITEM_n entries (field 283 + 2 n), resolved by
// the item cache - the same path equipmentOf takes for other players. A
// fallback for a server that sends no item objects (wowee.cfg ownequip=2).
bool WorldSession::resolvePlayerVisibleEquipment(uint32_t (&displayIds)[19], uint8_t (&invTypes)[19],
                                                 int* unresolved) const {
    constexpr uint16_t kVisibleItemBase = 283;  // as psp_world_session.cpp (3.3.5a)
    if (unresolved) *unresolved = 0;
    int entries = 0, resolved = 0;
    for (int s = 0; s < 19; ++s) {
        displayIds[s] = 0;
        invTypes[s] = 0;
        const uint32_t entry = selfField(static_cast<uint16_t>(kVisibleItemBase + s * 2));
        if (!entry) continue;
        ++entries;
        auto it = itemInfoCache_.find(entry);
        if (it == itemInfoCache_.end()) {
            if (unresolved) ++*unresolved;
            continue;
        }
        displayIds[s] = it->second.displayInfoId;
        invTypes[s] = static_cast<uint8_t>(it->second.inventoryType);
        ++resolved;
    }
    return entries > 0 && resolved > 0;
}

// --- Events ---------------------------------------------------------------------

// Upstream fires addon events straight into Lua. PSP: queued for whoever
// reads them (the world screen now, the Lua adapter later); an event already
// waiting is not queued twice (a rebuild says BAG_UPDATE 0..11 every time).
void WorldSession::pushInventoryEvent(const char* name, int32_t arg, uint32_t entry, uint32_t count) {
    tutorialInventoryEvent(name, arg, entry);  // tutorials (LootedItem, FullBags)
    for (const auto& e : inventoryEvents_)
        if (e.name == name && e.arg == arg && e.entry == entry && e.count == count) return;
    if (inventoryEvents_.size() >= kMaxInventoryEvents) inventoryEvents_.pop_front();
    InventoryEvent ev;
    ev.name = name;
    ev.arg = arg;
    ev.entry = entry;
    ev.count = count;
    inventoryEvents_.push_back(ev);
}

bool WorldSession::takeInventoryEvent(InventoryEvent& out) {
    if (inventoryEvents_.empty()) return false;
    out = inventoryEvents_.front();
    inventoryEvents_.pop_front();
    return true;
}

const std::string& WorldSession::lastInventoryError(uint32_t* atMs) const {
    if (atMs) *atMs = inventoryErrorMs_;
    return inventoryError_;
}

// upstream addUIError + addSystemChatMessage (+ the error sound, wired by the
// sound task: UiSoundManager::playError).
void WorldSession::raiseInventoryError(const std::string& msg, int32_t code) {
    inventoryError_ = msg;
    inventoryErrorMs_ = logUptimeMs();
    queueSound(SessionSound::Error);  // UiSoundManager::playError
    pushInventoryEvent("UI_ERROR_MESSAGE", code);
    addSystemChatMessage(msg);
}

void WorldSession::fireBagUpdates() {
    // Every bag: the worn four and the bank bags after them, then the
    // character sheet's event (upstream InventoryHandler::fireBagUpdates).
    const int lastBankBag = Inventory::NUM_BAG_SLOTS + game::slots::bankBagCount();
    for (int bag = 0; bag <= lastBankBag; ++bag) pushInventoryEvent("BAG_UPDATE", bag);
    pushInventoryEvent("UNIT_INVENTORY_CHANGED");
}

// --- Packets ----------------------------------------------------------------------

bool WorldSession::handleInventoryPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE:
        case LogicalOpcode::SMSG_ITEM_QUERY_MULTIPLE_RESPONSE:
            handleItemQueryResponse(packet);
            return true;
        case LogicalOpcode::SMSG_ITEM_PUSH_RESULT:
            if (state_ == State::InWorld) handleItemPushResult(packet);
            return true;
        case LogicalOpcode::SMSG_INVENTORY_CHANGE_FAILURE:
            if (state_ == State::InWorld) handleInventoryChangeFailure(packet);
            return true;
        default:
            return false;
    }
}

void WorldSession::queryItemInfo(uint32_t entry, uint64_t guid) {
    // itemQueried_ is upstream's pendingItemQueries_ (and M10c's "asked" set).
    if (entry == 0 || itemInfoCache_.count(entry) || itemQueried_.count(entry)) return;
    if (state_ != State::InWorld || !socket_) return;
    itemQueried_.insert(entry);
    // Some cores reject CMSG_ITEM_QUERY_SINGLE with guid 0: send ours instead.
    socket_->send(game::ItemQueryPacket::build(entry, guid != 0 ? guid : playerGuid_));
}

void WorldSession::handleItemQueryResponse(network::Packet& packet) {
    PROF_SCOPE("inventory");
    game::ItemQueryResponseData d;
    if (!game::ItemQueryResponseParser::parse(packet, d)) {
        // Clear the pending query so it can be asked again (upstream).
        if (packet.getSize() >= 4) {
            packet.setReadPos(0);
            itemQueried_.erase(packet.readUInt32() & ~0x80000000u);
        }
        return;
    }
    // M10c (moved here from onPacket): other players' visible equipment reads
    // the display id and inventory type of every answer.
    if (d.entry != 0) {
        itemInfo_[d.entry] = ItemInfo{d.displayInfoId, static_cast<uint8_t>(d.inventoryType)};
        ++entityVersion_;  // looks may change
    }
    // Upstream caches valid answers only. PSP: the entry stays "asked", so an
    // item the server does not know is not asked again on every rebuild.
    if (!d.valid) return;
    const uint32_t entry = d.entry;
    const game::ItemQueryResponseData& info = itemInfoCache_[entry] = std::move(d);
    inventoryDirty_ = true;  // upstream: rebuildOnlineInventory() right here
    pushInventoryEvent("GET_ITEM_INFO_RECEIVED", static_cast<int32_t>(entry));
    // Deferred "you receive" lines that waited for this item's name.
    for (auto it = pendingItemPushNotifs_.begin(); it != pendingItemPushNotifs_.end();) {
        if (it->itemId != entry) {
            ++it;
            continue;
        }
        const std::string name = info.name.empty() ? OWN_FMT(INV_ITEM_ENTRY, static_cast<unsigned>(entry)) : info.name;
        const std::string link = game::buildItemLink(entry, info.quality, name);
        addSystemChatMessage(it->count > 1 ? CLIENT_FMT(LOOT_ITEM_PUSHED_SELF_MULTIPLE, link, static_cast<unsigned>(it->count))
                                           : CLIENT_FMT(LOOT_ITEM_PUSHED_SELF, link));
        pushInventoryEvent("ITEM_PUSH", it->bagButtonId, entry, it->count);
        it = pendingItemPushNotifs_.erase(it);
    }
}

// upstream SMSG_ITEM_PUSH_RESULT handler (3.3.5a layout).
void WorldSession::handleItemPushResult(network::Packet& packet) {
    // guid(8)+received(4)+created(4)+displayInChat(4)+bagSlot(1)+slot(4)+itemId(4)
    //   +suffixFactor(4)+randomPropertyId(4)+count(4)+countInInventory(4)
    if (!packet.hasRemaining(45)) return;
    const uint64_t guid = packet.readUInt64();
    if (guid != playerGuid_) return;
    packet.readUInt32();  // received
    packet.readUInt32();  // created
    packet.readUInt32();  // displayInChat
    const uint8_t bagSlot = packet.readUInt8();
    const uint32_t slot = packet.readUInt32();
    const uint32_t itemId = packet.readUInt32();
    packet.readUInt32();  // suffixFactor
    const int32_t randomProp = static_cast<int32_t>(packet.readUInt32());
    const uint32_t count = packet.readUInt32();
    if (++invPushLogs_ <= 60)
        PSP_LOGI(tag::Net, "item_push entry=%u count=%u slot=%u:%u", static_cast<unsigned>(itemId),
                 static_cast<unsigned>(count), static_cast<unsigned>(bagSlot), static_cast<unsigned>(slot));
    // The bag button the item landed in: 0 the backpack (255 on the wire),
    // worn bags 19..22 -> 20..23 (FrameXML's Bag0Slot..Bag3Slot ids).
    const int bagButtonId = (bagSlot == 255) ? 0 : (static_cast<int>(bagSlot) + 1);
    const auto* info = getItemInfo(itemId);
    if (!info || info->name.empty()) {
        pendingItemPushNotifs_.push_back(PendingItemPush{itemId, count, bagButtonId});
        ensureItemInfo(itemId);
        return;
    }
    // PSP: no ItemRandomProperties.dbc on the card - the suffix ("of the
    // Bear") is not appended (upstream getRandomPropertyName).
    (void)randomProp;
    const std::string link = game::buildItemLink(itemId, info->quality, info->name);
    addSystemChatMessage(count > 1 ? CLIENT_FMT(LOOT_ITEM_PUSHED_SELF_MULTIPLE, link, static_cast<unsigned>(count))
                                   : CLIENT_FMT(LOOT_ITEM_PUSHED_SELF, link));
    queueSound(SessionSound::LootItem);  // UiSoundManager::playLootItem
    fireBagUpdates();
    pushInventoryEvent("ITEM_PUSH", bagButtonId, itemId, count);
}

// upstream SMSG_INVENTORY_CHANGE_FAILURE handler.
void WorldSession::handleInventoryChangeFailure(network::Packet& packet) {
    if (packet.getRemainingSize() < 1) return;
    const uint8_t error = packet.readUInt8();
    if (error == 0) return;
    uint32_t requiredLevel = 0;
    if (packet.hasRemaining(17)) {
        packet.readUInt64();
        packet.readUInt64();
        packet.readUInt8();
        if (error == 1 && packet.hasRemaining(4)) requiredLevel = packet.readUInt32();
    }
    if (++invErrorLogs_ <= 60)
        PSP_LOGI(tag::Net, "inv_error code=%u level=%u", static_cast<unsigned>(error), static_cast<unsigned>(requiredLevel));
    pendingEquip_ = PendingEquip{};
    if (error == 1) {  // level requirement has its own line
        raiseInventoryError(requiredLevel > 0 ? CLIENT_FMT(ERR_CANT_EQUIP_LEVEL_I, static_cast<unsigned>(requiredLevel))
                                              : std::string(OWN_STR(INV_ERR_LEVEL_HIGHER)),
                            error);
        return;
    }
    const char* text = inventoryErrorText(error);
    // (upstream: error 50 while a loot window is open says "Cannot loot item:
    // Inventory is full." - the loot task owns that window.)
    raiseInventoryError(text ? std::string(text) : OWN_FMT(INV_ERR_CODE, static_cast<unsigned>(error)), error);
}

// --- Update objects ----------------------------------------------------------------

// Called first for every update block. Item and container objects are ours
// alone (upstream keeps them in onlineItems_, the PSP has no entity for
// them): true = handled. The own player's block only feeds the slot fields
// and the money here and goes on to the combat code (false).
bool WorldSession::inventoryUpdateBlock(const game::UpdateBlock& b) {
    using game::ObjectType;
    using game::UpdateType;
    const bool create = b.updateType == UpdateType::CREATE_OBJECT || b.updateType == UpdateType::CREATE_OBJECT2;
    if (create && (b.objectType == ObjectType::ITEM || b.objectType == ObjectType::CONTAINER)) {
        PROF_SCOPE("inventory");
        trackItemOnCreate(b);
        return true;
    }
    if (b.updateType == UpdateType::VALUES && (onlineItems_.count(b.guid) || containerContents_.count(b.guid))) {
        PROF_SCOPE("inventory");
        updateItemOnValues(b);
        return true;
    }
    if (playerGuid_ == 0 || b.guid != playerGuid_ || !(create || b.updateType == UpdateType::VALUES)) return false;
    PROF_SCOPE("inventory");
    // upstream applyPlayerStatFields: coinage (PLAYER_MONEY on a change).
    const uint16_t fCoinage = game::fieldIndex(game::UF::PLAYER_FIELD_COINAGE);
    auto coin = b.fields.find(fCoinage);
    if (fCoinage != 0xFFFF && coin != b.fields.end()) {
        if (coin->second != moneyCopper_) {
            moneyCopper_ = coin->second;
            pushInventoryEvent("PLAYER_MONEY");
            inventoryDirty_ = true;  // PSP: the inv_update line reports the money too
        }
    }
    detectInventorySlotBases(b.fields);
    if (applyInventoryFields(b.fields)) {
        inventoryDirty_ = true;
        if (!create) pushInventoryEvent("PLAYER_EQUIPMENT_CHANGED");
    }
    return false;
}

// upstream processOutOfRangeObjects / handleDestroyObject: an item that goes
// out of range (sold, handed in) or is destroyed leaves the picture.
void WorldSession::inventoryForget(uint64_t guid) {
    containerContents_.erase(guid);
    if (onlineItems_.erase(guid)) inventoryDirty_ = true;
}

// upstream EntityController::trackItemOnCreate.
void WorldSession::trackItemOnCreate(const game::UpdateBlock& b) {
    using game::UF;
    const auto end = b.fields.end();
    auto entryIt = b.fields.find(game::fieldIndex(UF::OBJECT_FIELD_ENTRY));
    auto stackIt = b.fields.find(game::fieldIndex(UF::ITEM_FIELD_STACK_COUNT));
    auto durIt = b.fields.find(game::fieldIndex(UF::ITEM_FIELD_DURABILITY));
    auto maxDurIt = b.fields.find(game::fieldIndex(UF::ITEM_FIELD_MAXDURABILITY));
    // ITEM_FIELD_FLAGS = STACK_COUNT + 7; RANDOM_PROPERTIES_ID = DURABILITY - 1
    // on 3.3.5a (- 2 before); PROPERTY_SEED just before it; the enchantments
    // start at STACK_COUNT + 8 (permanent +0, temporary +3, sockets +6/+9/+12).
    const uint16_t stackField = game::fieldIndex(UF::ITEM_FIELD_STACK_COUNT);
    const uint16_t durField = game::fieldIndex(UF::ITEM_FIELD_DURABILITY);
    const uint16_t flagsField = stackField != 0xFFFF ? static_cast<uint16_t>(stackField + 7u) : 0xFFFFu;
    const uint16_t randPropField =
        durField != 0xFFFF ? static_cast<uint16_t>(durField - (game::isPreWotlk() ? 2u : 1u)) : 0xFFFFu;
    const uint16_t seedField =
        (randPropField != 0xFFFF && randPropField > 0) ? static_cast<uint16_t>(randPropField - 1u) : 0xFFFFu;
    const uint16_t enchBase = stackField != 0xFFFF ? static_cast<uint16_t>(stackField + 8u) : 0xFFFFu;
    auto find = [&](uint16_t f) { return f != 0xFFFF ? b.fields.find(f) : end; };
    auto flagsIt = find(flagsField);
    auto randPropIt = find(randPropField);
    auto seedIt = find(seedField);
    auto permEnchIt = find(enchBase);
    auto tempEnchIt = enchBase != 0xFFFF ? find(static_cast<uint16_t>(enchBase + 3u)) : end;
    auto sock1It = enchBase != 0xFFFF ? find(static_cast<uint16_t>(enchBase + 6u)) : end;
    auto sock2It = enchBase != 0xFFFF ? find(static_cast<uint16_t>(enchBase + 9u)) : end;
    auto sock3It = enchBase != 0xFFFF ? find(static_cast<uint16_t>(enchBase + 12u)) : end;
    if (entryIt != end && entryIt->second != 0) {
        // Keep what a partial create does not carry.
        auto prevIt = onlineItems_.find(b.guid);
        const OnlineItemInfo prev = prevIt != onlineItems_.end() ? prevIt->second : OnlineItemInfo{};
        OnlineItemInfo info = prev;
        info.entry = entryIt->second;
        if (stackIt != end) info.stackCount = stackIt->second;
        if (durIt != end) info.curDurability = durIt->second;
        if (maxDurIt != end) info.maxDurability = maxDurIt->second;
        if (permEnchIt != end) info.permanentEnchantId = permEnchIt->second;
        if (tempEnchIt != end) info.temporaryEnchantId = tempEnchIt->second;
        if (sock1It != end) info.socketEnchantIds[0] = sock1It->second;
        if (sock2It != end) info.socketEnchantIds[1] = sock2It->second;
        if (sock3It != end) info.socketEnchantIds[2] = sock3It->second;
        if (flagsIt != end) info.flags = flagsIt->second;
        if (randPropIt != end) info.randomPropertyId = static_cast<int32_t>(randPropIt->second);
        if (seedIt != end) info.suffixFactor = seedIt->second;
        const bool isNew = prevIt == onlineItems_.end();
        onlineItems_[b.guid] = info;
        // A re-sent create with a changed stack (AzerothCore, crafting) must
        // rebuild too, not only a brand-new item.
        const bool changed = isNew || info.stackCount != prev.stackCount ||
                             info.curDurability != prev.curDurability || info.maxDurability != prev.maxDurability ||
                             info.entry != prev.entry || info.flags != prev.flags ||
                             info.randomPropertyId != prev.randomPropertyId || info.suffixFactor != prev.suffixFactor ||
                             info.permanentEnchantId != prev.permanentEnchantId ||
                             info.temporaryEnchantId != prev.temporaryEnchantId ||
                             info.socketEnchantIds != prev.socketEnchantIds;
        if (changed) inventoryDirty_ = true;
        queryItemInfo(info.entry, b.guid);
    }
    if (b.objectType == game::ObjectType::CONTAINER) {
        extractContainerFields(b.guid, b.fields);
        inventoryDirty_ = true;
    }
}

// upstream EntityController::updateItemOnValuesUpdate.
void WorldSession::updateItemOnValues(const game::UpdateBlock& b) {
    using game::UF;
    bool changed = false;
    const uint16_t stackField = game::fieldIndex(UF::ITEM_FIELD_STACK_COUNT);
    const uint16_t durField = game::fieldIndex(UF::ITEM_FIELD_DURABILITY);
    const uint16_t maxDurField = game::fieldIndex(UF::ITEM_FIELD_MAXDURABILITY);
    const uint16_t numSlotsField = game::fieldIndex(UF::CONTAINER_FIELD_NUM_SLOTS);
    const uint16_t slot1Field = game::fieldIndex(UF::CONTAINER_FIELD_SLOT_1);
    const uint16_t enchBase = stackField != 0xFFFF ? static_cast<uint16_t>(stackField + 8u) : 0xFFFFu;
    const uint16_t flagsField = stackField != 0xFFFF ? static_cast<uint16_t>(stackField + 7u) : 0xFFFFu;
    const uint16_t randPropField =
        durField != 0xFFFF ? static_cast<uint16_t>(durField - (game::isPreWotlk() ? 2u : 1u)) : 0xFFFFu;
    const uint16_t seedField =
        (randPropField != 0xFFFF && randPropField > 0) ? static_cast<uint16_t>(randPropField - 1u) : 0xFFFFu;
    auto it = onlineItems_.find(b.guid);
    if (it != onlineItems_.end()) {
        OnlineItemInfo& info = it->second;
        auto set = [&changed](uint32_t& dst, uint32_t val) {
            if (dst != val) {
                dst = val;
                changed = true;
            }
        };
        for (const auto& [key, val] : b.fields) {
            if (key == stackField) {
                set(info.stackCount, val);
            } else if (key == durField) {
                const uint32_t prevDur = info.curDurability;
                set(info.curDurability, val);
                // Warn once when an equipped item drops below 20 %.
                const uint32_t maxDur = info.maxDurability;
                if (maxDur > 0 && val < maxDur / 5u && prevDur >= maxDur / 5u &&
                    std::find(equipSlotGuids_.begin(), equipSlotGuids_.end(), b.guid) != equipSlotGuids_.end()) {
                    const auto* q = getItemInfo(info.entry);
                    // PSP: the port's own line (upstream "%s is about to break!").
                    raiseInventoryError(q && !q->name.empty() ? OWN_FMT(INV_ITEM_BREAKING, q->name)
                                                              : std::string(OWN_STR(INV_WORN_ITEM_BREAKING)),
                                        0);
                }
            } else if (key == maxDurField) {
                set(info.maxDurability, val);
            } else if (enchBase != 0xFFFF && key == enchBase) {
                set(info.permanentEnchantId, val);
            } else if (enchBase != 0xFFFF && key == enchBase + 3u) {
                set(info.temporaryEnchantId, val);
            } else if (enchBase != 0xFFFF && key == enchBase + 6u) {
                set(info.socketEnchantIds[0], val);
            } else if (enchBase != 0xFFFF && key == enchBase + 9u) {
                set(info.socketEnchantIds[1], val);
            } else if (enchBase != 0xFFFF && key == enchBase + 12u) {
                set(info.socketEnchantIds[2], val);
            } else if (flagsField != 0xFFFF && key == flagsField) {
                set(info.flags, val);
            } else if (randPropField != 0xFFFF && key == randPropField) {
                if (info.randomPropertyId != static_cast<int32_t>(val)) {
                    info.randomPropertyId = static_cast<int32_t>(val);
                    changed = true;
                }
            } else if (seedField != 0xFFFF && key == seedField) {
                set(info.suffixFactor, val);
            }
        }
    }
    // Bag contents moved (upstream: entity type CONTAINER; here: a known container).
    if (containerContents_.count(b.guid)) {
        for (const auto& [key, val] : b.fields) {
            (void)val;
            if ((numSlotsField != 0xFFFF && key == numSlotsField) ||
                (slot1Field != 0xFFFF && key >= slot1Field && key < slot1Field + 72)) {
                changed = true;
                break;
            }
        }
        extractContainerFields(b.guid, b.fields);
    }
    if (changed) inventoryDirty_ = true;
}

// upstream InventoryHandler::detectInventorySlotBases.
void WorldSession::detectInventorySlotBases(const game::FlatFieldMap& fields) {
    if (invSlotBase_ >= 0 && packSlotBase_ >= 0) return;
    if (fields.empty()) return;
    std::vector<uint16_t> matchingPairs;
    // Primary signal: guid pairs naming item objects we hold.
    for (const auto& [idx, low] : fields) {
        if ((idx % 2) != 0) continue;
        auto itHigh = fields.find(static_cast<uint16_t>(idx + 1));
        if (itHigh == fields.end()) continue;
        const uint64_t guid = (uint64_t(itHigh->second) << 32) | low;
        if (guid != 0 && onlineItems_.count(guid)) matchingPairs.push_back(idx);
    }
    // Fallback (no item objects yet): any plausible non-trivial guid pair.
    if (matchingPairs.empty()) {
        for (const auto& [idx, low] : fields) {
            if ((idx % 2) != 0) continue;
            auto itHigh = fields.find(static_cast<uint16_t>(idx + 1));
            if (itHigh == fields.end()) continue;
            const uint64_t guid = (uint64_t(itHigh->second) << 32) | low;
            if (guid < 0x10000ull) continue;
            matchingPairs.push_back(idx);
        }
    }
    if (matchingPairs.empty()) return;
    std::sort(matchingPairs.begin(), matchingPairs.end());
    const int knownBase = static_cast<int>(game::fieldIndex(game::UF::PLAYER_FIELD_INV_SLOT_HEAD));
    constexpr int kStride = 2;
    bool allAlign = true;
    for (uint16_t p : matchingPairs) {
        if (p < knownBase || (p - knownBase) % kStride != 0) {
            allAlign = false;
            break;
        }
    }
    if (allAlign) {
        invSlotBase_ = knownBase;
    } else if (matchingPairs.size() >= 2) {
        const uint16_t lo = matchingPairs[0];
        for (int s = 0; s <= 22; s++) {
            const int candidate = lo - s * kStride;
            if (candidate < 0) break;
            bool ok = true;
            for (uint16_t p : matchingPairs) {
                const int off = p - candidate;
                if (off < 0 || off % kStride != 0 || off / kStride > 22) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                invSlotBase_ = candidate;
                break;
            }
        }
        if (invSlotBase_ < 0) invSlotBase_ = knownBase;
    } else {
        invSlotBase_ = knownBase;
    }
    packSlotBase_ = invSlotBase_ + Inventory::NUM_EQUIP_SLOTS * 2;
    PSP_LOGI(tag::Net, "inv_slot_base equip=%d pack=%d", invSlotBase_, packSlotBase_);
}

// upstream InventoryHandler::applyInventoryFields (buyback slots: the vendor
// task's; not tracked here).
bool WorldSession::applyInventoryFields(const game::FlatFieldMap& fields) {
    bool slotsChanged = false;
    const int equipBase = invSlotBase_ >= 0 ? invSlotBase_ : static_cast<int>(game::fieldIndex(game::UF::PLAYER_FIELD_INV_SLOT_HEAD));
    const int packBase = packSlotBase_ >= 0 ? packSlotBase_ : static_cast<int>(game::fieldIndex(game::UF::PLAYER_FIELD_PACK_SLOT_1));
    const int bankBase = static_cast<int>(game::fieldIndex(game::UF::PLAYER_FIELD_BANK_SLOT_1));
    const int bankBagBase = static_cast<int>(game::fieldIndex(game::UF::PLAYER_FIELD_BANKBAG_SLOT_1));
    // Slot counts from the field gap (Classic 24/6, 2.4.3 / 3.3.5a 28/7).
    if (bankBase != 0xFFFF && bankBagBase != 0xFFFF) {
        effectiveBankSlots_ = std::min((bankBagBase - bankBase) / 2, 28);
        effectiveBankBagSlots_ = (effectiveBankSlots_ <= 24) ? 6 : 7;
    }
    int keyringBase = static_cast<int>(game::fieldIndex(game::UF::PLAYER_FIELD_KEYRING_SLOT_1));
    if (keyringBase == 0xFFFF && bankBagBase != 0xFFFF) {
        // Bank bags, then 12 buyback slots (24 fields), then the keyring.
        keyringBase = bankBagBase + effectiveBankBagSlots_ * 2 + 24;
    }
    for (const auto& [key, val] : fields) {
        if (key >= equipBase && key <= equipBase + (Inventory::NUM_EQUIP_SLOTS * 2 - 1)) {
            const int slot = (key - equipBase) / 2;
            equipSlotGuids_[slot] = setGuidHalf(equipSlotGuids_[slot], (key - equipBase) % 2 == 0, val);
            slotsChanged = true;
        } else if (key >= packBase && key <= packBase + (Inventory::BACKPACK_SLOTS * 2 - 1)) {
            const int slot = (key - packBase) / 2;
            backpackSlotGuids_[slot] = setGuidHalf(backpackSlotGuids_[slot], (key - packBase) % 2 == 0, val);
            slotsChanged = true;
        } else if (keyringBase != 0xFFFF && key >= keyringBase &&
                   key <= keyringBase + (Inventory::KEYRING_SLOTS * 2 - 1)) {
            const int slot = (key - keyringBase) / 2;
            keyringSlotGuids_[slot] = setGuidHalf(keyringSlotGuids_[slot], (key - keyringBase) % 2 == 0, val);
            slotsChanged = true;
        }
        if (bankBase != 0xFFFF && key >= bankBase && key <= bankBase + (effectiveBankSlots_ * 2 - 1)) {
            const int slot = (key - bankBase) / 2;
            const uint64_t before = bankSlotGuids_[slot];
            bankSlotGuids_[slot] = setGuidHalf(before, (key - bankBase) % 2 == 0, val);
            slotsChanged = true;
            if (bankSlotGuids_[slot] != before) pushInventoryEvent("PLAYERBANKSLOTS_CHANGED", slot + 1);
        }
        if (bankBagBase != 0xFFFF && key >= bankBagBase && key <= bankBagBase + (effectiveBankBagSlots_ * 2 - 1)) {
            const int slot = (key - bankBagBase) / 2;
            const uint64_t before = bankBagSlotGuids_[slot];
            bankBagSlotGuids_[slot] = setGuidHalf(before, (key - bankBagBase) % 2 == 0, val);
            slotsChanged = true;
            if (bankBagSlotGuids_[slot] != before)
                pushInventoryEvent("PLAYERBANKSLOTS_CHANGED", effectiveBankSlots_ + slot + 1);
        }
    }
    return slotsChanged;
}

// upstream InventoryHandler::extractContainerFields.
void WorldSession::extractContainerFields(uint64_t containerGuid, const game::FlatFieldMap& fields) {
    const uint16_t numSlotsIdx = game::fieldIndex(game::UF::CONTAINER_FIELD_NUM_SLOTS);
    const uint16_t slot1Idx = game::fieldIndex(game::UF::CONTAINER_FIELD_SLOT_1);
    if (numSlotsIdx == 0xFFFF || slot1Idx == 0xFFFF) return;
    ContainerInfo& info = containerContents_[containerGuid];
    auto numIt = fields.find(numSlotsIdx);
    if (numIt != fields.end()) info.numSlots = std::min<uint32_t>(numIt->second, 36u);
    for (const auto& [key, val] : fields) {
        if (key < slot1Idx) continue;
        const int offset = key - slot1Idx;
        const int slot = offset / 2;
        if (slot >= 36) continue;
        info.slotGuids[slot] = setGuidHalf(info.slotGuids[slot], offset % 2 == 0, val);
    }
}

// upstream InventoryHandler::buildItemDef.
game::ItemDef WorldSession::buildItemDef(uint64_t guid, const OnlineItemInfo& online) {
    game::ItemDef def;
    def.itemId = online.entry;
    def.guid = guid;
    def.stackCount = online.stackCount;
    def.curDurability = online.curDurability;
    def.maxDurability = online.maxDurability;
    def.maxStack = 1;
    def.soulbound = (online.flags & kItemFlagSoulbound) != 0;
    def.randomPropertyId = online.randomPropertyId;
    def.suffixFactor = online.suffixFactor;
    auto infoIt = itemInfoCache_.find(online.entry);
    if (infoIt != itemInfoCache_.end()) {
        const auto& info = infoIt->second;
        def.name = info.name;
        def.quality = static_cast<game::ItemQuality>(info.quality);
        def.inventoryType = static_cast<uint8_t>(info.inventoryType);
        def.maxStack = static_cast<uint32_t>(std::max<int32_t>(1, info.maxStack));
        def.displayInfoId = info.displayInfoId;
        def.subclassName = info.subclassName;
        def.damageMin = info.damageMin;
        def.damageMax = info.damageMax;
        def.delayMs = info.delayMs;
        def.armor = info.armor;
        def.stamina = info.stamina;
        def.strength = info.strength;
        def.agility = info.agility;
        def.intellect = info.intellect;
        def.spirit = info.spirit;
        def.sellPrice = info.sellPrice;
        def.itemLevel = info.itemLevel;
        def.requiredLevel = info.requiredLevel;
        def.bindType = info.bindType;
        def.description = info.description;
        def.pageTextId = info.pageTextId;
        def.startQuestId = info.startQuestId;
        def.extraStats.clear();
        for (const auto& es : info.extraStats) def.extraStats.push_back({es.statType, es.statValue});
    } else {
        def.name = "Item " + std::to_string(def.itemId);
        queryItemInfo(def.itemId, guid);
    }
    // PSP: the rolled random-property stats (upstream getRandomStatBonuses,
    // ItemRandomProperties/Suffix + SpellItemEnchantment DBCs) are not added -
    // those tables are not on the card.
    return def;
}

// upstream InventoryHandler::rebuildOnlineInventory.
void WorldSession::rebuildOnlineInventory() {
    PROF_SCOPE("inventory");
    game::Inventory& inv = *inventory_;
    // PSP: cleared in place - upstream assigns a fresh Inventory(), a ~100 KB
    // temporary on the 256 KB main-thread stack here.
    for (int i = 0; i < Inventory::NUM_EQUIP_SLOTS; ++i) inv.setEquipSlot(static_cast<game::EquipSlot>(i), {});
    for (int i = 0; i < Inventory::BACKPACK_SLOTS; ++i) inv.clearBackpackSlot(i);
    for (int i = 0; i < Inventory::KEYRING_SLOTS; ++i) inv.setKeyringSlot(i, {});
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b) {
        for (int s = 0; s < inv.getBagSize(b); ++s) inv.setBagSlot(b, s, {});
        inv.setBagSize(b, 0);
        inv.setBagSpecial(b, false);
    }
    for (int i = 0; i < Inventory::BANK_SLOTS; ++i) inv.clearBankSlot(i);
    for (int b = 0; b < Inventory::BANK_BAG_SLOTS; ++b) {
        for (int s = 0; s < inv.getBankBagSize(b); ++s) inv.setBankBagSlot(b, s, {});
        inv.setBankBagSize(b, 0);
        inv.setBankBagItem(b, {});
    }
    auto itemAt = [this](uint64_t guid) -> const OnlineItemInfo* {
        if (guid == 0) return nullptr;
        auto it = onlineItems_.find(guid);
        return it == onlineItems_.end() ? nullptr : &it->second;
    };
    for (int i = 0; i < Inventory::NUM_EQUIP_SLOTS; i++)
        if (const auto* o = itemAt(equipSlotGuids_[i]))
            inv.setEquipSlot(static_cast<game::EquipSlot>(i), buildItemDef(equipSlotGuids_[i], *o));
    for (int i = 0; i < Inventory::BACKPACK_SLOTS; i++)
        if (const auto* o = itemAt(backpackSlotGuids_[i])) inv.setBackpackSlot(i, buildItemDef(backpackSlotGuids_[i], *o));
    for (int i = 0; i < Inventory::KEYRING_SLOTS; i++)
        if (const auto* o = itemAt(keyringSlotGuids_[i])) inv.setKeyringSlot(i, buildItemDef(keyringSlotGuids_[i], *o));
    // Bag contents (BAG1..BAG4 are equip slots 19..22).
    for (int bagIdx = 0; bagIdx < Inventory::NUM_BAG_SLOTS; bagIdx++) {
        const uint64_t bagGuid = equipSlotGuids_[Inventory::FIRST_BAG_EQUIP_SLOT + bagIdx];
        if (bagGuid == 0) continue;
        int numSlots = 0;
        auto contIt = containerContents_.find(bagGuid);
        if (contIt != containerContents_.end()) numSlots = static_cast<int>(contIt->second.numSlots);
        const game::ItemQueryResponseData* bagTemplate = nullptr;
        if (const auto* o = itemAt(bagGuid)) bagTemplate = getItemInfo(o->entry);
        if (numSlots <= 0 && bagTemplate) numSlots = static_cast<int>(bagTemplate->containerSlots);
        if (numSlots <= 0) continue;
        inv.setBagSize(bagIdx, numSlots);
        // Quivers (class 11) and profession bags (class 1, subclass != 0).
        inv.setBagSpecial(bagIdx, bagTemplate && (bagTemplate->itemClass == 11 ||
                                                  (bagTemplate->itemClass == 1 && bagTemplate->subClass != 0)));
        const auto bagEquip = static_cast<game::EquipSlot>(Inventory::FIRST_BAG_EQUIP_SLOT + bagIdx);
        if (!inv.getEquipSlot(bagEquip).empty()) {
            game::ItemDef bagDef = inv.getEquipSlot(bagEquip).item;
            bagDef.bagSlots = static_cast<uint32_t>(numSlots);
            inv.setEquipSlot(bagEquip, bagDef);
        }
        if (contIt == containerContents_.end()) continue;
        for (int s = 0; s < numSlots && s < 36; s++) {
            const uint64_t itemGuid = contIt->second.slotGuids[s];
            const auto* o = itemAt(itemGuid);
            if (!o) continue;
            game::ItemDef def = buildItemDef(itemGuid, *o);
            if (const auto* bi = getItemInfo(o->entry)) def.bagSlots = bi->containerSlots;
            inv.setBagSlot(bagIdx, s, def);
        }
    }
    // Bank slots and bank bags (upstream copies the same fields by hand;
    // buildItemDef gives the same item plus the guid).
    for (int i = 0; i < effectiveBankSlots_ && i < Inventory::BANK_SLOTS; i++) {
        const auto* o = itemAt(bankSlotGuids_[i]);
        if (!o) continue;
        game::ItemDef def = buildItemDef(bankSlotGuids_[i], *o);
        if (const auto* bi = getItemInfo(o->entry)) def.bagSlots = bi->containerSlots;
        inv.setBankSlot(i, def);
    }
    for (int bagIdx = 0; bagIdx < effectiveBankBagSlots_ && bagIdx < Inventory::BANK_BAG_SLOTS; bagIdx++) {
        const uint64_t bagGuid = bankBagSlotGuids_[bagIdx];
        if (bagGuid == 0) continue;
        int numSlots = 0;
        auto contIt = containerContents_.find(bagGuid);
        if (contIt != containerContents_.end()) numSlots = static_cast<int>(contIt->second.numSlots);
        if (const auto* o = itemAt(bagGuid)) {
            const auto* bi = getItemInfo(o->entry);
            if (numSlots <= 0 && bi) numSlots = static_cast<int>(bi->containerSlots);
            game::ItemDef bagDef;
            bagDef.itemId = o->entry;
            bagDef.stackCount = 1;
            bagDef.inventoryType = game::InvType::BAG;
            if (bi) {
                bagDef.name = bi->name;
                bagDef.quality = static_cast<game::ItemQuality>(bi->quality);
                bagDef.displayInfoId = bi->displayInfoId;
                bagDef.bagSlots = bi->containerSlots;
            } else {
                bagDef.name = "Bag";
                queryItemInfo(bagDef.itemId, bagGuid);
            }
            inv.setBankBagItem(bagIdx, bagDef);
        }
        if (numSlots <= 0) continue;
        inv.setBankBagSize(bagIdx, numSlots);
        if (contIt == containerContents_.end()) continue;
        for (int s = 0; s < numSlots && s < 36; s++) {
            const uint64_t itemGuid = contIt->second.slotGuids[s];
            const auto* o = itemAt(itemGuid);
            if (!o) continue;
            game::ItemDef def = buildItemDef(itemGuid, *o);
            if (const auto* bi = getItemInfo(o->entry)) def.bagSlots = bi->containerSlots;
            inv.setBankBagSlot(bagIdx, s, def);
        }
    }
    // Equipment "dirty" only when a worn display id (or enchant) changed.
    std::array<uint32_t, 19> displayIds{};
    bool enchantsChanged = false;
    for (int i = 0; i < 19; i++) {
        const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(i));
        if (!slot.empty()) displayIds[i] = slot.item.displayInfoId;
        uint64_t ench = 0;
        if (const auto* o = itemAt(equipSlotGuids_[i]))
            ench = (static_cast<uint64_t>(o->permanentEnchantId) << 32) | o->temporaryEnchantId;
        if (ench != lastEquipEnchantIds_[i]) {
            lastEquipEnchantIds_[i] = ench;
            enchantsChanged = true;
        }
    }
    if (displayIds != lastEquipDisplayIds_ || enchantsChanged) {
        lastEquipDisplayIds_ = displayIds;
        ++equipmentVersion_;
    }
    ++inventoryVersion_;
    fireBagUpdates();  // upstream: announced from here, once per rebuild
    // (upstream then reconciles "collect N" quest objectives from the bag
    // counts - the quest task's; Inventory::countItem is there for it.)
}

// Once per frame: the batch rebuild (upstream rebuilds per packet / query
// answer; PSP: at most once a frame, the rebuild copies ~500 slots).
void WorldSession::inventoryUpdate() {
    if (!inventoryDirty_) return;
    PROF_SCOPE("inventory");
    inventoryDirty_ = false;
    // upstream finalizeUpdateObjectBatch: late slot-base detection once items are known.
    if (invSlotBase_ < 0 && !selfFields_.empty() && !onlineItems_.empty()) {
        game::FlatFieldMap all;
        all.reserve(selfFields_.size());
        for (const auto& [k, v] : selfFields_) all.insert_or_assign(k, v);
        detectInventorySlotBases(all);
        if (invSlotBase_ >= 0) applyInventoryFields(all);
    }
    rebuildOnlineInventory();
    // inv_update when the summary moved (bags worn, items held, money).
    const auto& inv = *inventory_;
    int bags = 0, items = 0;
    for (int i = 0; i < Inventory::NUM_EQUIP_SLOTS; ++i)
        if (!inv.getEquipSlot(static_cast<game::EquipSlot>(i)).empty()) ++items;
    for (int i = 0; i < Inventory::BACKPACK_SLOTS; ++i)
        if (!inv.getBackpackSlot(i).empty()) ++items;
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b) {
        if (inv.getBagSize(b) > 0) ++bags;
        for (int s = 0; s < inv.getBagSize(b); ++s)
            if (!inv.getBagSlot(b, s).empty()) ++items;
    }
    if ((bags != invLastBags_ || items != invLastItems_ || moneyCopper_ != invLastMoney_) && ++invLogs_ <= 60) {
        PSP_LOGI(tag::Net, "inv_update bags=%d items=%d money=%u objects=%u cache=%u", bags, items,
                 static_cast<unsigned>(moneyCopper_), static_cast<unsigned>(onlineItems_.size()),
                 static_cast<unsigned>(itemInfoCache_.size()));
    }
    invLastBags_ = bags;
    invLastItems_ = items;
    invLastMoney_ = moneyCopper_;
}

// --- Requests (upstream InventoryHandler verbs) ------------------------------------

bool WorldSession::equipWouldBindFromBackpack(int backpackIndex) const {
    if (backpackIndex < 0 || backpackIndex >= inventory_->getBackpackSize()) return false;
    const auto& slot = inventory_->getBackpackSlot(backpackIndex);
    return !slot.empty() && slot.item.wouldBindOnEquip();
}

bool WorldSession::equipWouldBindFromBag(int bagIndex, int slotIndex) const {
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS) return false;
    if (slotIndex < 0 || slotIndex >= inventory_->getBagSize(bagIndex)) return false;
    const auto& slot = inventory_->getBagSlot(bagIndex, slotIndex);
    return !slot.empty() && slot.item.wouldBindOnEquip();
}

// CMSG_AUTOEQUIP_ITEM: the server picks the slot (a ring into the free
// finger, a one-hander into the main hand, ...) and swaps out what is there.
void WorldSession::autoEquipItemBySlot(int backpackIndex, bool confirmed) {
    if (backpackIndex < 0 || backpackIndex >= inventory_->getBackpackSize()) return;
    const auto& slot = inventory_->getBackpackSlot(backpackIndex);
    if (slot.empty()) return;
    const uint8_t wireSlot = static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + backpackIndex);
    if (!confirmed && slot.item.wouldBindOnEquip()) {
        pendingEquip_ = PendingEquip{true, false, 0, backpackIndex, wireSlot};
        pushInventoryEvent("AUTOEQUIP_BIND_CONFIRM", wireSlot);
        return;
    }
    if (state_ == State::InWorld && socket_) {
        socket_->send(game::AutoEquipItemPacket::build(0xFF, wireSlot));
        if (++invSwapLogs_ <= 60)
            PSP_LOGI(tag::Net, "inv_swap src=255:%u dst=auto ok=1 item=%u", static_cast<unsigned>(wireSlot),
                     static_cast<unsigned>(slot.item.itemId));
    }
}

void WorldSession::autoEquipItemInBag(int bagIndex, int slotIndex, bool confirmed) {
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS) return;
    if (slotIndex < 0 || slotIndex >= inventory_->getBagSize(bagIndex)) return;
    if (!confirmed && equipWouldBindFromBag(bagIndex, slotIndex)) {
        pendingEquip_ = PendingEquip{true, true, bagIndex, slotIndex, static_cast<uint8_t>(slotIndex)};
        pushInventoryEvent("AUTOEQUIP_BIND_CONFIRM", slotIndex);
        return;
    }
    if (state_ == State::InWorld && socket_) {
        const uint8_t bag = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex);
        socket_->send(game::AutoEquipItemPacket::build(bag, static_cast<uint8_t>(slotIndex)));
        if (++invSwapLogs_ <= 60)
            PSP_LOGI(tag::Net, "inv_swap src=%u:%d dst=auto ok=1 item=%u", static_cast<unsigned>(bag), slotIndex,
                     static_cast<unsigned>(inventory_->getBagSlot(bagIndex, slotIndex).item.itemId));
    }
}

void WorldSession::equipPendingItem() {
    const PendingEquip pending = pendingEquip_;
    pendingEquip_ = PendingEquip{};
    if (!pending.active) return;
    if (pending.fromBag) autoEquipItemInBag(pending.bag, pending.slot, true);
    else autoEquipItemBySlot(pending.slot, true);
}

// CMSG_AUTOEQUIP_ITEM_SLOT: item guid + the 0-based equipment slot.
void WorldSession::equipItemToSlot(uint64_t itemGuid, uint8_t equipSlot) {
    if (itemGuid == 0 || state_ != State::InWorld || !socket_) return;
    if (equipSlot >= static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT)) return;  // AzerothCore drops it
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_AUTOEQUIP_ITEM_SLOT);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt64(itemGuid);
    pkt.writeUInt8(equipSlot);
    socket_->send(pkt);
    if (++invSwapLogs_ <= 60) PSP_LOGI(tag::Net, "inv_swap src=guid dst=255:%u ok=1", static_cast<unsigned>(equipSlot));
}

// upstream unequipToBackpack: CMSG_SWAP_ITEM into the first free backpack slot.
void WorldSession::unequipToBackpack(game::EquipSlot equipSlot) {
    if (state_ != State::InWorld || !socket_) return;
    const uint8_t srcSlot = static_cast<uint8_t>(equipSlot);
    const int freeSlot = inventory_->findFreeBackpackSlot();
    if (freeSlot < 0) {
        // PSP (wowee.cfg invautostore=1, default): with the backpack full, ask
        // the server to store it anywhere - CMSG_AUTOSTORE_BAG_ITEM with the
        // "any bag" destination, as the original client's PutItemInBackpack does.
        // Upstream (invautostore=0) refuses with the line below.
        if (configGetInt("invautostore", 1) != 0) {
            network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_AUTOSTORE_BAG_ITEM));
            pkt.writeUInt8(0xFF);  // source container: the player's slots
            pkt.writeUInt8(srcSlot);
            pkt.writeUInt8(0xFF);  // destination: any bag (NULL_BAG)
            socket_->send(pkt);
            if (++invSwapLogs_ <= 60)
                PSP_LOGI(tag::Net, "inv_swap src=255:%u dst=autostore ok=1", static_cast<unsigned>(srcSlot));
            return;
        }
        if (++invSwapLogs_ <= 60) PSP_LOGI(tag::Net, "inv_swap src=255:%u dst=none ok=0", static_cast<unsigned>(srcSlot));
        raiseInventoryError(OWN_STR(INV_NO_ROOM_UNEQUIP), -1);
        return;
    }
    const uint8_t dstSlot = static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + freeSlot);
    socket_->send(game::SwapItemPacket::build(0xFF, dstSlot, 0xFF, srcSlot));
    if (++invSwapLogs_ <= 60)
        PSP_LOGI(tag::Net, "inv_swap src=255:%u dst=255:%u ok=1", static_cast<unsigned>(srcSlot),
                 static_cast<unsigned>(dstSlot));
}

// Model slots by the wire's numbering (backpack = container 0xFF, slots 23..;
// worn bag = container 19.., slots from 0). False for bank/keyring pairs.
bool WorldSession::readWireSlot(uint8_t container, uint8_t slot, game::ItemDef& out) const {
    const auto& inv = *inventory_;
    if (container == game::slots::kNoContainer) {
        const int index = static_cast<int>(slot) - game::slots::kBackpackFirst;
        if (index < 0 || index >= inv.getBackpackSize()) return false;
        out = inv.getBackpackSlot(index).item;
        return true;
    }
    const int bagIndex = static_cast<int>(container) - game::slots::kWornBagFirst;
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS) return false;
    if (static_cast<int>(slot) >= inv.getBagSize(bagIndex)) return false;
    out = inv.getBagSlot(bagIndex, static_cast<int>(slot)).item;
    return true;
}

bool WorldSession::writeWireSlot(uint8_t container, uint8_t slot, const game::ItemDef& item) {
    auto& inv = *inventory_;
    if (container == game::slots::kNoContainer) {
        const int index = static_cast<int>(slot) - game::slots::kBackpackFirst;
        if (index < 0 || index >= inv.getBackpackSize()) return false;
        return inv.setBackpackSlot(index, item);
    }
    const int bagIndex = static_cast<int>(container) - game::slots::kWornBagFirst;
    if (bagIndex < 0 || bagIndex >= Inventory::NUM_BAG_SLOTS) return false;
    if (static_cast<int>(slot) >= inv.getBagSize(bagIndex)) return false;
    return inv.setBagSlot(bagIndex, static_cast<int>(slot), item);
}

// CMSG_SWAP_ITEM, and the move applied to the model at once (both sides read
// before either is written). The server merges stacks of one item itself.
void WorldSession::swapContainerItems(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag, uint8_t dstSlot,
                                      bool applyLocally) {
    if (!socket_ || !socket_->isConnected()) return;
    socket_->send(game::SwapItemPacket::build(dstBag, dstSlot, srcBag, srcSlot));
    bool local = false;
    if (applyLocally) {
        game::ItemDef from, to;
        if (readWireSlot(srcBag, srcSlot, from) && readWireSlot(dstBag, dstSlot, to)) {
            writeWireSlot(srcBag, srcSlot, to);
            writeWireSlot(dstBag, dstSlot, from);
            fireBagUpdates();
            local = true;
        }
    }
    if (++invSwapLogs_ <= 60)
        PSP_LOGI(tag::Net, "inv_swap src=%u:%u dst=%u:%u ok=1 local=%d", static_cast<unsigned>(srcBag),
                 static_cast<unsigned>(srcSlot), static_cast<unsigned>(dstBag), static_cast<unsigned>(dstSlot),
                 local ? 1 : 0);
}

void WorldSession::swapBagSlots(int srcBagIndex, int dstBagIndex) {
    if (srcBagIndex < 0 || srcBagIndex > 3 || dstBagIndex < 0 || dstBagIndex > 3) return;
    if (srcBagIndex == dstBagIndex) return;
    auto& inv = *inventory_;
    const auto srcEquip = static_cast<game::EquipSlot>(static_cast<int>(game::EquipSlot::BAG1) + srcBagIndex);
    const auto dstEquip = static_cast<game::EquipSlot>(static_cast<int>(game::EquipSlot::BAG1) + dstBagIndex);
    const game::ItemDef srcItem = inv.getEquipSlot(srcEquip).item;
    const game::ItemDef dstItem = inv.getEquipSlot(dstEquip).item;
    inv.setEquipSlot(srcEquip, dstItem);
    inv.setEquipSlot(dstEquip, srcItem);
    inv.swapBagContents(srcBagIndex, dstBagIndex);
    pushInventoryEvent("BAG_CLOSED", srcBagIndex + 1);
    pushInventoryEvent("BAG_CLOSED", dstBagIndex + 1);
    fireBagUpdates();
    if (socket_ && socket_->isConnected()) {
        const uint8_t srcSlot = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + srcBagIndex);
        const uint8_t dstSlot = static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + dstBagIndex);
        socket_->send(game::SwapItemPacket::build(255, dstSlot, 255, srcSlot));
        if (++invSwapLogs_ <= 60)
            PSP_LOGI(tag::Net, "inv_swap src=255:%u dst=255:%u ok=1 bags=1", static_cast<unsigned>(srcSlot),
                     static_cast<unsigned>(dstSlot));
    }
}

// CMSG_DESTROYITEM: bag, slot, count (0 = the whole stack).
void WorldSession::destroyItem(uint8_t bag, uint8_t slot, uint8_t count) {
    if (state_ != State::InWorld || !socket_) return;
    uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_DESTROYITEM);
    if (wire == 0xFFFF) wire = 0x111;  // upstream writes 0x111 itself
    network::Packet packet(wire);
    packet.writeUInt8(bag);
    packet.writeUInt8(slot);
    packet.writeUInt32(static_cast<uint32_t>(count));
    socket_->send(packet);
    PSP_LOGI(tag::Net, "inv_destroy bag=%u slot=%u count=%u", static_cast<unsigned>(bag), static_cast<unsigned>(slot),
             static_cast<unsigned>(count));
}

void WorldSession::splitItemTo(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag, uint8_t dstSlot, uint8_t count) {
    if (state_ != State::InWorld || !socket_ || count == 0) return;
    socket_->send(game::SplitItemPacket::build(srcBag, srcSlot, dstBag, dstSlot, count));
    if (++invSwapLogs_ <= 60)
        PSP_LOGI(tag::Net, "inv_swap src=%u:%u dst=%u:%u ok=1 split=%u", static_cast<unsigned>(srcBag),
                 static_cast<unsigned>(srcSlot), static_cast<unsigned>(dstBag), static_cast<unsigned>(dstSlot),
                 static_cast<unsigned>(count));
}

// No destination: the first free slot, backpack first, then the worn bags.
void WorldSession::splitItem(uint8_t srcBag, uint8_t srcSlot, uint8_t count) {
    const int freeBp = inventory_->findFreeBackpackSlot();
    if (freeBp >= 0) {
        splitItemTo(srcBag, srcSlot, 0xFF, static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + freeBp), count);
        return;
    }
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; b++) {
        for (int s = 0; s < inventory_->getBagSize(b); s++) {
            if (!inventory_->getBagSlot(b, s).empty()) continue;
            splitItemTo(srcBag, srcSlot, static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + b),
                        static_cast<uint8_t>(s), count);
            return;
        }
    }
    raiseInventoryError(OWN_STR(INV_NO_ROOM_SPLIT), -1);
}

// the same table for a mail's MAIL_ERR_EQUIP_ERROR (SMSG_SEND_MAIL_RESULT carries an
// InventoryResult after it when an attachment cannot go into the bags).
std::string WorldSession::inventoryErrorString(uint8_t code) const {
    const char* text = inventoryErrorText(code);
    return text ? std::string(text) : OWN_FMT(INV_ERR_CODE, static_cast<unsigned>(code));
}

} // namespace wowee::platform::psp
