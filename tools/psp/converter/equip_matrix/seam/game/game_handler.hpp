#pragma once
// PC seam for upstream's game::GameHandler - the few
// questions the equipment code asks it (the active character, the inventory,
// item info, the show-helm/cloak toggles, the entity manager for NPC virtual
// weapons). The matrix sets the answers; the real class (5 000 lines of
// network session) is not needed to dress a character.
#include "game/character.hpp"
#include "game/entity.hpp"
#include "game/game_services.hpp"
#include "game/inventory.hpp"
#include "core/logger.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace wowee {
namespace game {

class TransportManager;

// What getItemInfo answers (upstream: ItemQueryResponseData, the fields the equipment code reads).
struct ItemInfoSeam {
    bool valid = false;
    uint32_t entry = 0;
    uint32_t displayInfoId = 0;
    uint32_t itemClass = 0;
    uint32_t subClass = 0;
    uint32_t inventoryType = 0;
    std::string name;
};

struct GameObjectInfoSeam {
    bool valid = false;
    uint32_t type = 0;
    std::string name;
};

class GameHandler {
public:
    uint64_t getPlayerGuid() const { return playerGuid; }
    uint64_t getActiveCharacterGuid() const { return activeCharacterGuid; }
    const Character* getActiveCharacter() const { return hasActiveCharacter ? &activeCharacter : nullptr; }
    Inventory& getInventory() { return inventory; }
    const Inventory& getInventory() const { return inventory; }
    const ItemInfoSeam* getItemInfo(uint32_t itemId) const {
        auto it = itemInfo.find(itemId);
        return it != itemInfo.end() ? &it->second : nullptr;
    }
    bool isHelmVisible() const { return helmVisible; }
    bool isCloakVisible() const { return cloakVisible; }
    uint64_t getEquipSlotGuid(int) const { return 0; }
    std::pair<uint32_t, uint32_t> getItemEnchantIds(uint64_t) const { return {0u, 0u}; }
    EntityManager& getEntityManager() { return entityManager; }
    const EntityManager& getEntityManager() const { return entityManager; }

    // transports, taxis, maps, game objects: not part of a character's equipment
    TransportManager* getTransportManager() const { return nullptr; }
    template <typename... A> bool isTransportGuid(A&&...) const { return false; }
    template <typename... A> void notifyTransportSpawned(A&&...) {}
    template <typename... A> bool hasServerTransportUpdate(A&&...) const { return false; }
    template <typename... A> void completePlayerTransportWorldTransfer(A&&...) {}
    bool isOnTaxiFlight() const { return false; }
    uint32_t getCurrentMapId() const { return 0; }
    const GameObjectInfoSeam* getCachedGameObjectInfo(uint32_t) const { return nullptr; }

    // matrix: the answers
    uint64_t playerGuid = 0;
    uint64_t activeCharacterGuid = 0;
    bool hasActiveCharacter = false;
    Character activeCharacter;
    Inventory inventory;
    std::unordered_map<uint32_t, ItemInfoSeam> itemInfo;
    bool helmVisible = true;
    bool cloakVisible = true;
    EntityManager entityManager;
};

}  // namespace game
}  // namespace wowee
