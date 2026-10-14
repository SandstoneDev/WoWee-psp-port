// map - the explored-area bits of our player (world map fog / overlays).
//
// Upstream SpellHandler::extractExploredZoneFields copies PLAYER_EXPLORED_ZONES_1..
// (128 words on 3.3.5a, packet parsers' exploredZonesCount) out of the player's update
// fields into GameHandler::playerExploredZones_ and sets hasPlayerExploredZones_ once any
// word arrived. PSP: the own player's fields already live in selfFields_ (the combat
// block keeps every field of every own update), so the words are read from there when
// asked - the world map asks about once a second while it is open.
#include "platform/psp/psp_world_session.hpp"

#include "game/update_field_table.hpp"

namespace wowee::platform::psp {

const std::vector<uint32_t>& WorldSession::getPlayerExploredZoneMasks() const {
    if (playerExploredZones_.size() != PLAYER_EXPLORED_ZONES_COUNT)
        playerExploredZones_.assign(PLAYER_EXPLORED_ZONES_COUNT, 0u);
    const uint16_t base = game::fieldIndex(game::UF::PLAYER_EXPLORED_ZONES_START);
    if (base == 0xFFFF) return playerExploredZones_;
    bool foundAny = false;
    for (size_t i = 0; i < PLAYER_EXPLORED_ZONES_COUNT; ++i) {
        auto it = selfFields_.find(static_cast<uint16_t>(base + i));
        if (it == selfFields_.end()) continue;
        playerExploredZones_[i] = it->second;
        foundAny = true;
    }
    if (foundAny) hasPlayerExploredZones_ = true;
    return playerExploredZones_;
}

bool WorldSession::hasPlayerExploredZoneMasks() const {
    (void)getPlayerExploredZoneMasks();
    return hasPlayerExploredZones_;
}

}  // namespace wowee::platform::psp
