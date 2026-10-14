// Data types of the NPC layer (gossip, vendor, quests) of the PSP
// world session - psp_world_session_npc.cpp. Kept out of
// psp_world_session.hpp so the hook block there stays short.
//
// The quest log entry mirrors upstream QuestHandler::QuestLogEntry
// (include/game/quest_handler.hpp) field for field where the PSP fills the
// field, so task A's Lua-API adapter (GetQuestLogTitle, GetQuestLogLeaderBoard,
// GetNumQuestLeaderBoards...) reads the same names as upstream's lua_quest_api
// does from GameHandler::getQuestLog(). Fields upstream fills from DBCs the PSP
// does not bake (QuestSort names, QuestXP / QuestFactionReward values) are
// kept as the raw ids.
#pragma once

#include "game/world_packets.hpp"  // QuestRewardItem

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace wowee::platform::psp {

struct NpcQuestLogEntry {
    uint32_t questId = 0;
    std::string title;
    std::string objectives;
    std::string description;     // the quest giver's text (third string of the query)
    std::string completionText;  // shown once every objective is done
    uint32_t rewardSpellId = 0;
    uint32_t rewardXPId = 0;     // QuestXP.dbc column (not baked on the PSP)
    uint32_t rewardHonor = 0;
    uint32_t rewardTalents = 0;
    uint32_t rewardArenaPoints = 0;
    uint32_t rewardTitleId = 0;
    struct FactionReward { uint32_t factionId = 0; int32_t valueId = 0; int32_t override = 0; };
    std::array<FactionReward, 5> factionRewards{};
    int32_t level = 0;           // 0 = unknown, -1 = player-scaling
    int32_t zoneOrSort = 0;      // >0 AreaTable zone, <0 QuestSort (negated)
    bool complete = false;
    bool failed = false;
    // entry -> (count, required); game objects keyed by their positive entry
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> killCounts;
    std::unordered_map<uint32_t, uint32_t> itemCounts;
    std::unordered_map<uint32_t, uint32_t> requiredItemCounts;
    struct KillObjective {
        int32_t npcOrGoId = 0;   // <0 = game object
        uint32_t required = 0;
    };
    std::array<KillObjective, 4> killObjectives{};
    struct ItemObjective {
        uint32_t itemId = 0;
        uint32_t required = 0;
    };
    std::array<ItemObjective, 6> itemObjectives{};
    uint32_t sourceItemId = 0;
    int32_t rewardMoney = 0;
    std::array<game::QuestRewardItem, 4> rewardItems{};
    std::array<game::QuestRewardItem, 6> rewardChoiceItems{};
};

// PSP: what the NPC windows need of an item until task C's item cache lands
// (upstream GameHandler::getItemInfo -> ItemQueryResponseData).
struct NpcItemInfo {
    std::string name;
    uint32_t quality = 1;
    uint32_t sellPrice = 0;
    int32_t maxStack = 1;
};

}  // namespace wowee::platform::psp
