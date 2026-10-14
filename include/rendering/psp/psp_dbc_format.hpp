// Factions - client DBC tables baked for the PSP ("data/dbc.pak",
// tools/psp/converter/psp_dbc_conv.cpp). One reader and one hostility
// algorithm for the converter's --verify / --selftest and the PSP runtime
// (host + PSP, no platform includes).
//
// Entry "factions" (kind 3), magic "WFTP", version 1, little-endian:
//   FactionTableHeader                          16 B
//   FactionTemplateRow[templateCount]           36 B each, ascending id
//   FactionRow[factionCount]                    72 B each, ascending id; only the
//                                               rows with a ReputationListID
//                                               (the others carry no standing)
// Columns = Data/expansions/wotlk/dbc_layouts.json ("FactionTemplate",
// "Faction") + Faction.dbc ReputationClassMask 6..9 (upstream
// GameHandler::loadFactionNameCache comment): exactly what upstream
// game::buildFactionHostilityMap (src/game/faction_hostility.cpp) and
// GameHandler::factionBaseReputation read, nothing else.
//
// buildFactionReactions() below IS upstream buildFactionHostilityMap, step for
// step, over these rows instead of the DBC files (PSP: no AssetManager/DBC on
// the card). Its result is upstream's three maps (hostile, friendly,
// template -> ReputationListID) as one sorted vector (PSP: ~2k templates x 12 B
// instead of three unordered_maps).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wowee::rendering::psp {

struct FactionTableHeader {
    char magic[4];          // "WFTP"
    uint16_t version;       // 1
    uint16_t reserved;
    uint32_t templateCount;
    uint32_t factionCount;
};
static_assert(sizeof(FactionTableHeader) == 16, "WFTP header is 16 bytes");

// FactionTemplate.dbc: ID 0, Faction 1, FactionGroup 3, FriendGroup 4,
// EnemyGroup 5, Enemy0..3 6..9.
struct FactionTemplateRow {
    uint32_t id;
    uint32_t faction;       // parent Faction.dbc id
    uint32_t factionGroup;
    uint32_t friendGroup;
    uint32_t enemyGroup;
    uint32_t enemy[4];
};
static_assert(sizeof(FactionTemplateRow) == 36, "WFTP template row is 36 bytes");

// Faction.dbc: ID 0, ReputationListID 1, ReputationRaceMask0..3 2..5,
// ReputationClassMask0..3 6..9, ReputationBase0..3 10..13,
// ReputationFlags0..3 14..17.
struct FactionRow {
    uint32_t id;
    uint32_t repListId;
    uint32_t raceMask[4];
    uint32_t classMask[4];
    int32_t base[4];
    uint32_t flags[4];
};
static_assert(sizeof(FactionRow) == 72, "WFTP faction row is 72 bytes");

constexpr uint16_t kFactionTableVersion = 1;
constexpr uint32_t kNoRepList = 0xFFFFFFFFu;
// upstream GameHandler::FACTION_FLAG_* (game_handler.hpp; not includable on the PSP).
constexpr uint8_t kFactionFlagAtWar = 0x02;
constexpr uint8_t kFactionFlagPeaceForced = 0x10;

// A baked blob, checked (sizes, bounds, sort order). The rows point into it.
struct FactionTableView {
    const FactionTableHeader* h = nullptr;
    const FactionTemplateRow* templates = nullptr;
    const FactionRow* factions = nullptr;

    bool attach(const void* blob, size_t size) {
        *this = FactionTableView{};
        if (!blob || size < sizeof(FactionTableHeader)) return false;
        const auto* hh = static_cast<const FactionTableHeader*>(blob);
        if (std::memcmp(hh->magic, "WFTP", 4) != 0 || hh->version != kFactionTableVersion) return false;
        const uint64_t need = sizeof(FactionTableHeader) + uint64_t{hh->templateCount} * sizeof(FactionTemplateRow) +
                              uint64_t{hh->factionCount} * sizeof(FactionRow);
        if (need > size || hh->templateCount > 100000u || hh->factionCount > 100000u) return false;
        const auto* t = reinterpret_cast<const FactionTemplateRow*>(hh + 1);
        const auto* f = reinterpret_cast<const FactionRow*>(t + hh->templateCount);
        for (uint32_t i = 1; i < hh->templateCount; ++i)
            if (t[i - 1].id >= t[i].id) return false;
        for (uint32_t i = 1; i < hh->factionCount; ++i)
            if (f[i - 1].id >= f[i].id) return false;
        h = hh;
        templates = t;
        factions = f;
        return true;
    }
    const FactionRow* faction(uint32_t id) const {
        if (!h) return nullptr;
        const FactionRow* end = factions + h->factionCount;
        const FactionRow* it =
            std::lower_bound(factions, end, id, [](const FactionRow& r, uint32_t v) { return r.id < v; });
        return it != end && it->id == id ? it : nullptr;
    }
    const FactionRow* factionByRepList(uint32_t repListId) const {
        if (!h || repListId == kNoRepList) return nullptr;
        for (uint32_t i = 0; i < h->factionCount; ++i)
            if (factions[i].repListId == repListId) return &factions[i];
        return nullptr;
    }
};

// One faction template as the player's race sees it (upstream: the entries of
// factionHostileMap_, factionFriendlyMap_ and factionTemplateRepList_).
struct FactionReaction {
    uint32_t id;
    uint32_t repListId;     // kNoRepList: the template's faction has no standing
    uint8_t hostile;
    uint8_t friendly;
    uint16_t pad;
};

struct FactionReactions {
    std::vector<FactionReaction> templates;  // ascending id
    uint32_t playerFtId = 0;
    uint32_t playerFriendGroup = 0, playerEnemyGroup = 0;
    uint32_t hostileCount = 0, friendlyCount = 0;
    uint32_t repFactions = 0, atWarCount = 0;  // factions with a standing / at war from the start

    const FactionReaction* find(uint32_t templateId) const {
        auto it = std::lower_bound(templates.begin(), templates.end(), templateId,
                                   [](const FactionReaction& r, uint32_t v) { return r.id < v; });
        return it != templates.end() && it->id == templateId ? &*it : nullptr;
    }
};

// upstream buildFactionHostilityMap race tables, unchanged.
inline uint32_t factionRaceMask(uint8_t playerRace) {
    if (playerRace >= 1 && playerRace <= 8) return 1u << (playerRace - 1);
    if (playerRace == 10) return 0x200;  // race 10
    if (playerRace == 11) return 0x400;  // race 11
    return 0;
}
inline uint32_t playerFactionTemplate(uint8_t playerRace) {
    switch (playerRace) {
        case 1: return 1;      // Human
        case 2: return 2;      // Orc
        case 3: return 3;      // Dwarf
        case 4: return 4;      // race 4
        case 5: return 5;      // Undead
        case 6: return 6;      // race 6
        case 7: return 115;    // Gnome
        case 8: return 116;    // Troll
        case 10: return 1610;  // race 10
        case 11: return 1629;  // race 11
        default: return 1;
    }
}

// upstream game::buildFactionHostilityMap (src/game/faction_hostility.cpp),
// the same steps in the same order; comments shortened.
inline void buildFactionReactions(const FactionTableView& v, uint8_t playerRace, FactionReactions& out) {
    out = FactionReactions{};
    if (!v.h) return;
    const uint32_t playerRaceMask = factionRaceMask(playerRace);
    const uint32_t playerFtId = playerFactionTemplate(playerRace);
    out.playerFtId = playerFtId;

    // The factions a player has a standing with, and whether they start at
    // war: Hostile or worse at the start, unless the flags force peace.
    struct RepFaction {
        uint32_t id, repListId;
        bool atWar;
    };
    std::vector<RepFaction> repFactions;  // ascending id (rows are)
    constexpr int32_t kHostileRankTop = -3001;  // Hostile runs -6000 to -3001
    for (uint32_t i = 0; i < v.h->factionCount; ++i) {
        const FactionRow& f = v.factions[i];
        if (f.repListId == kNoRepList) continue;  // no standing to have
        int32_t baseRep = 0;
        uint32_t repFlags = 0;
        for (int slot = 0; slot < 4; slot++) {
            if (f.raceMask[slot] & playerRaceMask) {
                baseRep = f.base[slot];
                repFlags = f.flags[slot];
                break;
            }
        }
        const bool atWar = (repFlags & kFactionFlagAtWar) != 0 ||
                           (baseRep <= kHostileRankTop && (repFlags & kFactionFlagPeaceForced) == 0);
        repFactions.push_back({f.id, f.repListId, atWar});
        if (atWar) ++out.atWarCount;
    }
    out.repFactions = static_cast<uint32_t>(repFactions.size());

    // The player's own template.
    uint32_t playerFriendGroup = 0, playerEnemyGroup = 0, playerFactionId = 0, playerFactionGroup = 0;
    for (uint32_t i = 0; i < v.h->templateCount; ++i) {
        const FactionTemplateRow& t = v.templates[i];
        if (t.id == playerFtId) {
            playerFriendGroup = t.friendGroup | t.factionGroup;
            playerFactionGroup = t.factionGroup;
            playerEnemyGroup = t.enemyGroup;
            playerFactionId = t.faction;
            break;
        }
    }
    out.playerFriendGroup = playerFriendGroup;
    out.playerEnemyGroup = playerEnemyGroup;

    out.templates.reserve(v.h->templateCount);
    for (uint32_t i = 0; i < v.h->templateCount; ++i) {
        const FactionTemplateRow& t = v.templates[i];
        // 1. Symmetric group check
        bool hostile = (t.enemyGroup & playerFriendGroup) != 0 || (t.factionGroup & playerEnemyGroup) != 0;
        // 2. Monster factionGroup bit (8)
        if (!hostile && (t.factionGroup & 8) != 0) hostile = true;
        // 3. Individual enemy faction IDs
        if (!hostile && playerFactionId > 0) {
            for (int e = 0; e < 4; e++) {
                if (t.enemy[e] == playerFactionId) {
                    hostile = true;
                    break;
                }
            }
        }
        // 4. Explicitly friendly (friendGroup includes the player): not hostile
        if (hostile && (t.friendGroup & playerFriendGroup) != 0) hostile = false;
        // Friendly is mutual regard, asked both ways, never for a hostile one.
        bool friendly = !hostile && (((t.friendGroup & playerFactionGroup) != 0) ||
                                     ((t.factionGroup & playerFriendGroup) != 0));
        // 5. A faction with a standing overrides all of it.
        uint32_t repListId = kNoRepList;
        auto rep = std::lower_bound(repFactions.begin(), repFactions.end(), t.faction,
                                    [](const RepFaction& r, uint32_t id) { return r.id < id; });
        if (rep != repFactions.end() && rep->id == t.faction) {
            hostile = rep->atWar;
            friendly = !hostile;
            repListId = rep->repListId;
        }
        out.templates.push_back({t.id, repListId, static_cast<uint8_t>(hostile ? 1 : 0),
                                 static_cast<uint8_t>(friendly ? 1 : 0), 0});
        if (hostile) ++out.hostileCount;
        if (friendly) ++out.friendlyCount;
    }
}

// upstream GameHandler::factionBaseReputation: the starting standing by race
// and class ("a slot for this race, or a class-only slot, whose class mask is
// this class or empty").
inline int32_t factionBaseReputation(const FactionRow& f, uint8_t race, uint8_t cls) {
    const uint32_t raceMask = race ? (1u << (race - 1)) : 0;
    const uint32_t classMask = cls ? (1u << (cls - 1)) : 0;
    for (int slot = 0; slot < 4; ++slot) {
        const uint32_t slotRaces = f.raceMask[slot];
        const uint32_t slotClasses = f.classMask[slot];
        if (((slotRaces & raceMask) != 0 || (slotRaces == 0 && slotClasses != 0)) &&
            ((slotClasses & classMask) != 0 || slotClasses == 0)) {
            return f.base[slot];
        }
    }
    return 0;
}

} // namespace wowee::rendering::psp
