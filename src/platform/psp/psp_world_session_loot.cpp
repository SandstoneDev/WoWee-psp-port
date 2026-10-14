// loot and loot money for the PSP world session.
//
// GameHandler / InventoryHandler are not built for the PSP (they pull in
// rendering, audio and the addon layer), so the loot half of upstream's
// InventoryHandler is mirrored here with its names, and upstream's packet
// builders and parsers unchanged (LootPacket, AutostoreLootItemPacket,
// LootMoneyPacket, LootReleasePacket, LootResponseParser):
//   - InventoryHandler::lootTarget / lootItem (+ the bind-on-pickup confirm) /
//     confirmPendingLoot / lootMoney / closeLoot / clearLootMoney
//   - handleLootResponse (money taken at once + the 0.4 s announce fallback,
//     auto-loot, an empty corpse closes itself), handleLootReleaseResponse,
//     handleLootRemoved (the last slot closes the loot)
//   - SMSG_LOOT_MONEY_NOTIFY / SMSG_LOOT_CLEAR_MONEY / SMSG_LOOT_ITEM_NOTIFY
//     and announceLootMoney ("Looted: ..." once per loot)
//   - EntityController's lootableCleared -> GameHandler::despawnCreatureLocally
//     (an emptied, unskinnable corpse is removed locally)
//   - Application::updateLootSparkles' corpse list (dead + UNIT_DYNFLAG_LOOTABLE)
// Group-loot rolls (SMSG_LOOT_START_ROLL / ROLL / ROLL_WON / ALL_PASSED /
// MASTER_LIST / SLOT_CHANGED) are logged only: no roll window on the PSP yet.
// Item names / quality come from the item query answers (the session sends
// CMSG_ITEM_QUERY_SINGLE for loot items like upstream queryItemInfo).
#include "platform/psp/psp_world_session.hpp"

#include "game/game_utils.hpp"
#include "game/item_text.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>

namespace wowee::platform::psp {

namespace {

constexpr uint32_t kUnitFlagSkinnable = 0x04000000;  // upstream EntityController UNIT_FLAG_SKINNABLE
constexpr float kLootScanSec = 0.25f;                // lootable-corpse scan period (PSP)
constexpr size_t kMaxLootItemInfo = 512;             // PSP: bound for the name cache (upstream: unbounded)

unsigned lo32(uint64_t guid) { return static_cast<unsigned>(guid & 0xFFFFFFFFu); }

// PSP: upstream formatCopperAmount ("1g 5s 3c") with the client's coin letters
// (GOLD_AMOUNT_SYMBOL / SILVER_ / COPPER_, as the rest of the PSP chat); same
// rule - zero coins dropped, "0" + the copper letter for nothing.
std::string formatCopperShort(uint32_t amount) {
    const game::CoinAmount c = game::splitCopper(amount);
    std::string out;
    auto add = [&](uint32_t v, const char* unit) {
        if (!out.empty()) out += " ";
        out += std::to_string(v) + " " + unit;
    };
    if (c.gold > 0) add(c.gold, cs(CS::GOLD_AMOUNT_SYMBOL));
    if (c.silver > 0) add(c.silver, cs(CS::SILVER_AMOUNT_SYMBOL));
    if (c.copper > 0 || out.empty()) add(c.copper, cs(CS::COPPER_AMOUNT_SYMBOL));
    return out;
}

}  // namespace

// --- Packets ------------------------------------------------------------------------

bool WorldSession::handleLootPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    const bool inWorld = state_ == State::InWorld;
    switch (op) {
        case LogicalOpcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE: {
            // Peek only: the session's own case (displayId / inventory type
            // for equipment) still runs on the untouched packet.
            network::Packet copy = packet;
            game::ItemQueryResponseData d;
            if (game::ItemQueryResponseParser::parse(copy, d) && d.entry != 0) {
                if (lootItemInfo_.size() >= kMaxLootItemInfo && !lootItemInfo_.count(d.entry)) lootItemInfo_.clear();
                LootItemInfo& info = lootItemInfo_[d.entry];
                info.name = d.name;
                info.quality = static_cast<uint8_t>(std::min<uint32_t>(d.quality, 7u));
                info.bindType = static_cast<uint8_t>(d.bindType);
                for (const auto& it : currentLoot_.items)
                    if (it.itemId == d.entry) ++lootVersion_;
            }
            return false;
        }
        case LogicalOpcode::SMSG_LOOT_RESPONSE:
            if (inWorld) handleLootResponse(packet);
            return true;
        case LogicalOpcode::SMSG_LOOT_RELEASE_RESPONSE:
            if (inWorld) {
                const uint64_t guid = packet.hasRemaining(8) ? packet.readUInt64() : 0;
                PSP_LOGI(tag::Net, "loot_release guid=%08X src=server open=%d", lo32(guid), lootWindowOpen_ ? 1 : 0);
                handleLootReleaseResponse();
            }
            return true;
        case LogicalOpcode::SMSG_LOOT_REMOVED:
            if (inWorld && packet.hasRemaining(1)) handleLootRemoved(packet);
            return true;
        case LogicalOpcode::SMSG_LOOT_MONEY_NOTIFY: {
            if (!inWorld || !packet.hasRemaining(4)) return true;
            const uint32_t amount = packet.readUInt32();
            if (packet.hasRemaining(1)) (void)packet.readUInt8();  // soleLooter
            // upstream: playerMoneyCopper += amount (PLAYER_MONEY); the
            // PSP keeps the money in the own PLAYER_FIELD_COINAGE, and the
            // server's field update that follows overwrites it absolutely.
            const uint16_t fCoin = game::fieldIndex(game::UF::PLAYER_FIELD_COINAGE);
            if (fCoin != 0xFFFF && !selfFields_.empty()) selfFields_[fCoin] = selfField(fCoin) + amount;
            const uint64_t notifyGuid = pendingLootMoneyGuid_ != 0 ? pendingLootMoneyGuid_ : currentLoot_.lootGuid;
            pendingLootMoneyGuid_ = 0;
            pendingLootMoneyAmount_ = 0;
            pendingLootMoneyNotifyTimer_ = 0.0f;
            PSP_LOGI(tag::Net, "loot_money amount=%u guid=%08X", static_cast<unsigned>(amount), lo32(notifyGuid));
            announceLootMoney(notifyGuid, amount);
            // upstream: only a whole purse clears the coin (a group share
            // reaches everyone; SMSG_LOOT_CLEAR_MONEY names the loot itself).
            if (amount == currentLoot_.gold) clearLootMoney();
            ++lootVersion_;
            return true;
        }
        case LogicalOpcode::SMSG_LOOT_CLEAR_MONEY:
            if (inWorld) clearLootMoney();
            return true;
        case LogicalOpcode::SMSG_LOOT_ITEM_NOTIFY: {
            // objectGuid(8) + lootSlot (3.3.5a uint8, earlier uint32).
            if (!inWorld || !packet.hasRemaining(9)) return true;
            (void)packet.readUInt64();
            uint32_t lootSlot = 0;
            if (game::isActiveExpansion("wotlk")) {
                lootSlot = packet.readUInt8();
            } else {
                if (!packet.hasRemaining(4)) return true;
                lootSlot = packet.readUInt32();
            }
            for (const auto& it : currentLoot_.items) {
                if (it.slotIndex != static_cast<uint8_t>(lootSlot)) continue;
                const LootItemInfo* info = lootItemInfo(it.itemId);
                const std::string name =
                    info && !info->name.empty() ? info->name : psf(PS::ITEM_NUMBER, it.itemId);
                // upstream "You receive loot: [link]." (showLootSpam, on by default): LOOT_ITEM_SELF.
                addSystemChatMessage(csf(CS::LOOT_ITEM_SELF, "[" + name + "]"));
                break;
            }
            return true;
        }
        case LogicalOpcode::SMSG_LOOT_SLOT_CHANGED:
        case LogicalOpcode::SMSG_LOOT_START_ROLL:
        case LogicalOpcode::SMSG_LOOT_ALL_PASSED:
        case LogicalOpcode::SMSG_LOOT_ROLL:
        case LogicalOpcode::SMSG_LOOT_ROLL_WON:
        case LogicalOpcode::SMSG_LOOT_MASTER_LIST:
        case LogicalOpcode::SMSG_LOOT_LIST:
            // PSP: group loot has no window yet - logged only (the server
            // passes for us when a roll's countdown runs out).
            if (++lootLogs_ <= 60)
                PSP_LOGI(tag::Net, "loot_group op=0x%X bytes=%u", static_cast<unsigned>(packet.getOpcode()),
                         static_cast<unsigned>(packet.getSize()));
            return true;
        default:
            return false;
    }
}

// upstream InventoryHandler::handleLootResponse.
void WorldSession::handleLootResponse(network::Packet& packet) {
    const bool wotlkLoot = game::isActiveExpansion("wotlk");
    // PSP: parsed into a local first. The realm's refusal is a short packet
    // (guid, LOOT_NONE, error - AzerothCore SendLootError) the parser rejects,
    // and upstream parsed straight into currentLoot_, which wiped a loot that
    // was open. Logged, since "nothing happened" is otherwise invisible.
    if (packet.getRemainingSize() == 10) {
        const size_t pos = packet.getReadPos();
        const uint64_t guid = packet.readUInt64();
        const unsigned type = packet.readUInt8();
        const unsigned err = packet.readUInt8();
        packet.setReadPos(pos);
        PSP_LOGW(tag::Net, "loot_error guid=%08X type=%u err=%u", lo32(guid), type, err);
    }
    game::LootResponseData data;
    if (!game::LootResponseParser::parse(packet, data, wotlkLoot)) return;
    currentLoot_ = std::move(data);
    const uint64_t lootGuid = currentLoot_.lootGuid;
    PSP_LOGI(tag::Net, "loot_open guid=%08X items=%u money=%u type=%u", lo32(lootGuid),
             static_cast<unsigned>(currentLoot_.items.size()), static_cast<unsigned>(currentLoot_.gold),
             static_cast<unsigned>(currentLoot_.lootType));
    // (upstream: the game-object gather / chest paths - no game objects on the PSP yet.)
    lootWindowOpen_ = true;
    ++lootVersion_;
    auto& localLoot = localLootState_[lootGuid];
    localLoot.data = currentLoot_;

    for (const auto& item : currentLoot_.items) queryLootItem(item.itemId);

    if (currentLoot_.gold > 0 && state_ == State::InWorld && socket_) {
        bool suppressFallback = false;
        auto cooldownIt = recentLootMoneyAnnounceCooldowns_.find(lootGuid);
        if (cooldownIt != recentLootMoneyAnnounceCooldowns_.end() && cooldownIt->second > 0.0f) suppressFallback = true;
        pendingLootMoneyGuid_ = suppressFallback ? 0 : lootGuid;
        pendingLootMoneyAmount_ = suppressFallback ? 0 : currentLoot_.gold;
        pendingLootMoneyNotifyTimer_ = suppressFallback ? 0.0f : 0.4f;
        // upstream: the money is taken at once; the coin stays in the window
        // until the server says it is taken.
        socket_->send(game::LootMoneyPacket::build());
    }

    if (autoLoot_ && state_ == State::InWorld && socket_ && !localLoot.itemAutoLootSent) {
        for (const auto& item : currentLoot_.items) {
            if (++lootLogs_ <= 60)
                PSP_LOGI(tag::Net, "loot_take slot=%u item=%u ok=1 auto=1", static_cast<unsigned>(item.slotIndex),
                         static_cast<unsigned>(item.itemId));
            socket_->send(game::AutostoreLootItemPacket::build(item.slotIndex));
        }
        localLoot.itemAutoLootSent = true;
    }

    // An empty corpse closes itself (items: from handleLootRemoved).
    if (lootWindowOpen_ && currentLoot_.items.empty() && currentLoot_.gold == 0) closeLoot();
}

// upstream InventoryHandler::handleLootReleaseResponse.
void WorldSession::handleLootReleaseResponse() {
    localLootState_.erase(currentLoot_.lootGuid);
    lootWindowOpen_ = false;
    pendingLootActive_ = false;
    currentLoot_ = game::LootResponseData{};
    ++lootVersion_;
}

// upstream InventoryHandler::handleLootRemoved. (The "Received item" line and
// its sound come with SMSG_ITEM_PUSH_RESULT - the inventory's, task C.)
void WorldSession::handleLootRemoved(network::Packet& packet) {
    const uint8_t slotIndex = packet.readUInt8();
    for (auto& it : currentLoot_.items) {
        if (it.slotIndex != slotIndex) continue;
        it.looted = true;
        // PSP: the corpse's remembered loot follows (upstream keeps the copy
        // taken at open; tab-cycling reads it).
        auto ls = localLootState_.find(currentLoot_.lootGuid);
        if (ls != localLootState_.end())
            for (auto& li : ls->second.data.items)
                if (li.slotIndex == slotIndex) li.looted = true;
        if (++lootLogs_ <= 60)
            PSP_LOGI(tag::Net, "loot_removed slot=%u item=%u", static_cast<unsigned>(slotIndex),
                     static_cast<unsigned>(it.itemId));
        break;
    }
    ++lootVersion_;
    if (lootWindowOpen_ && currentLoot_.nothingLeft()) closeLoot();
}

// upstream InventoryHandler::announceLootMoney: once per loot.
void WorldSession::announceLootMoney(uint64_t lootGuid, uint32_t amount) {
    if (amount == 0) return;
    auto it = localLootState_.find(lootGuid);
    if (it != localLootState_.end()) {
        if (it->second.moneyTaken) return;
        it->second.moneyTaken = true;
    }
    // upstream "Looted: " + formatCopperAmount; the client's YOU_LOOT_MONEY.
    addSystemChatMessage(csf(CS::YOU_LOOT_MONEY, formatCopperShort(amount)));
    // upstream UiSoundManager playLootCoinLarge() when amount >= 10000 (1 gold),
    // else playLootCoinSmall() (queued for the sound task).
    queueSound(amount >= 10000 ? SessionSound::LootCoinLarge : SessionSound::LootCoinSmall);
    if (lootGuid != 0) recentLootMoneyAnnounceCooldowns_[lootGuid] = 1.5f;
}

// upstream InventoryHandler::clearLootMoney: the coin is marked taken, not
// removed (it is display slot one); a money-only corpse is empty now.
void WorldSession::clearLootMoney() {
    if (!currentLoot_.goldLeft()) return;
    currentLoot_.goldLooted = true;
    auto it = localLootState_.find(currentLoot_.lootGuid);
    if (it != localLootState_.end()) it->second.data.goldLooted = true;
    ++lootVersion_;
    if (lootWindowOpen_ && currentLoot_.nothingLeft()) closeLoot();
}

// --- Requests -----------------------------------------------------------------------

// upstream InventoryHandler::lootTarget.
void WorldSession::lootTarget(uint64_t guid) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    currentLoot_.items.clear();
    if (++lootLogs_ <= 60) PSP_LOGI(tag::Net, "loot_request guid=%08X", lo32(guid));
    socket_->send(game::LootPacket::build(guid));
}

// upstream InventoryHandler::lootItem: a bind-on-pickup item waits for
// confirmPendingLoot (LOOT_BIND_CONFIRM in the Lua interface). Returns
// whether CMSG_AUTOSTORE_LOOT_ITEM went out.
bool WorldSession::lootItem(uint8_t slotIndex, bool confirmed) {
    if (state_ != State::InWorld || !socket_) return false;
    uint32_t itemId = 0;
    for (const auto& item : currentLoot_.items) {
        if (item.slotIndex != slotIndex) continue;
        itemId = item.itemId;
        const LootItemInfo* info = lootItemInfo(item.itemId);
        if (!confirmed && info && info->bindType == 1) {
            pendingLootActive_ = true;
            pendingLootSlot_ = slotIndex;
            if (++lootLogs_ <= 60)
                PSP_LOGI(tag::Net, "loot_take slot=%u item=%u ok=0 bop=1", static_cast<unsigned>(slotIndex),
                         static_cast<unsigned>(itemId));
            return false;
        }
        break;
    }
    pendingLootActive_ = false;
    if (++lootLogs_ <= 60)
        PSP_LOGI(tag::Net, "loot_take slot=%u item=%u ok=1", static_cast<unsigned>(slotIndex),
                 static_cast<unsigned>(itemId));
    socket_->send(game::AutostoreLootItemPacket::build(slotIndex));
    return true;
}

void WorldSession::confirmPendingLoot() {
    if (!pendingLootActive_) return;
    const uint8_t slot = pendingLootSlot_;
    pendingLootActive_ = false;
    lootItem(slot, true);
}

// upstream InventoryHandler::lootMoney.
void WorldSession::lootMoney() {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::LootMoneyPacket::build());
}

// upstream InventoryHandler::closeLoot: CMSG_LOOT_RELEASE, the window closes
// at once (the server's SMSG_LOOT_RELEASE_RESPONSE finds nothing open).
void WorldSession::closeLoot() {
    if (!lootWindowOpen_) return;
    const uint64_t lootGuid = currentLoot_.lootGuid;
    if (state_ == State::InWorld && socket_) socket_->send(game::LootReleasePacket::build(lootGuid));
    PSP_LOGI(tag::Net, "loot_release guid=%08X src=client", lo32(lootGuid));
    lootWindowOpen_ = false;
    pendingLootActive_ = false;
    currentLoot_ = game::LootResponseData{};
    ++lootVersion_;
}

bool WorldSession::hasLocalLootItems(uint64_t guid) const {
    auto it = localLootState_.find(guid);
    if (it == localLootState_.end()) return false;
    for (const auto& item : it->second.data.items)
        if (!item.looted) return true;
    return false;
}

const WorldSession::LootItemInfo* WorldSession::lootItemInfo(uint32_t entry) const {
    auto it = lootItemInfo_.find(entry);
    return it == lootItemInfo_.end() ? nullptr : &it->second;
}

// upstream queryItemInfo(itemId, 0): once per entry (the session's shared
// "queried" set - the equipment path uses it too).
void WorldSession::queryLootItem(uint32_t entry) {
    if (entry == 0 || !socket_ || lootItemInfo_.count(entry)) return;
    if (!itemQueried_.insert(entry).second) return;
    socket_->send(game::ItemQueryPacket::build(entry, playerGuid_));
}

// --- Per frame (the world screen calls it) ------------------------------------------

void WorldSession::updateLoot(float dtSec, bool despawnLooted) {
    if (state_ != State::InWorld) return;
    // upstream tickLootMoneyFallback: a server that sends no money notify.
    if (pendingLootMoneyNotifyTimer_ > 0.0f) {
        pendingLootMoneyNotifyTimer_ -= dtSec;
        if (pendingLootMoneyNotifyTimer_ <= 0.0f) {
            pendingLootMoneyNotifyTimer_ = 0.0f;
            announceLootMoney(pendingLootMoneyGuid_, pendingLootMoneyAmount_);
            pendingLootMoneyGuid_ = 0;
            pendingLootMoneyAmount_ = 0;
        }
    }
    // (upstream decays a same-named map of GameHandler's, so the
    // InventoryHandler's never shrinks; the PSP decays the one it reads.)
    for (auto it = recentLootMoneyAnnounceCooldowns_.begin(); it != recentLootMoneyAnnounceCooldowns_.end();) {
        it->second -= dtSec;
        if (it->second <= 0.0f) it = recentLootMoneyAnnounceCooldowns_.erase(it);
        else ++it;
    }

    // PSP: upstream reacts per update block (EntityController: LOOTABLE went
    // away on a VALUES update -> lootableCleared); the PSP scans the units
    // four times a second instead of hooking the shared update path.
    lootScanTimer_ -= dtSec;
    if (lootScanTimer_ > 0.0f) return;
    lootScanTimer_ = kLootScanSec;
    const uint16_t fHealth = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
    const uint16_t fMaxHealth = game::fieldIndex(game::UF::UNIT_FIELD_MAXHEALTH);
    const uint16_t fDyn = game::fieldIndex(game::UF::UNIT_DYNAMIC_FLAGS);
    const uint16_t fFlags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
    std::vector<uint64_t> now;
    now.reserve(lootable_.size() + 4);
    for (const auto& [guid, e] : entities_.getEntities()) {
        if (!e || e->getType() != game::ObjectType::UNIT) continue;
        const uint32_t dyn = e->getField(fDyn);
        if (!(dyn & game::UNIT_DYNFLAG_LOOTABLE)) continue;
        if (!game::isUnitCorpseState(e->getField(fHealth), e->getField(fMaxHealth), dyn)) continue;
        now.push_back(guid);
        if (std::find(lootable_.begin(), lootable_.end(), guid) == lootable_.end() && ++lootableLogs_ <= 60)
            PSP_LOGI(tag::Net, "lootable guid=%08X entry=%u", lo32(guid),
                     static_cast<unsigned>(e->getField(game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY))));
    }
    std::vector<uint64_t> cleared;
    for (uint64_t guid : lootable_)
        if (std::find(now.begin(), now.end(), guid) == now.end()) cleared.push_back(guid);
    lootable_.swap(now);
    if (!despawnLooted) return;
    // upstream: lootableCleared && health == 0 && !SKINNABLE ->
    // despawnCreatureLocally (the server's DESTROY comes later with the
    // corpse's decay; a looted body is not left lying about till then).
    for (uint64_t guid : cleared) {
        auto e = entities_.getEntity(guid);
        if (!e || e->getType() != game::ObjectType::UNIT) continue;
        if (e->getField(fHealth) != 0 || (e->getField(fFlags) & kUnitFlagSkinnable)) continue;
        if (e->getField(fDyn) & game::UNIT_DYNFLAG_LOOTABLE) continue;
        if (autoAttackTarget_ == guid) stopAutoAttack();  // (the world screen drops a target that is gone)
        removeEntity(guid);
        PSP_LOGI(tag::Net, "loot_despawn guid=%08X", lo32(guid));
    }
}

}  // namespace wowee::platform::psp
