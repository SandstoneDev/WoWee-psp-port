// factions for the PSP world session - who is hostile to the
// player, as upstream decides it:
//   - game::buildFactionHostilityMap (src/game/faction_hostility.cpp): the
//     same function, over the rows psp_dbc_conv baked from
//     FactionTemplate.dbc / Faction.dbc (include/rendering/psp/
//     psp_dbc_format.hpp: buildFactionReactions), for our race;
//   - GameHandler::isHostileFaction / isFriendlyFaction / factionAtWarLive /
//     unitReactionToPlayer / factionBaseReputation (game_handler.hpp/.cpp);
//   - SocialHandler::handleInitializeFactions / handleSetFactionStanding /
//     handleSetFactionAtWar (social_handler.cpp): the standings and the
//     server's at-war flags, which override the starting-standing guess.
// Upstream stores a unit's hostility when it spawns and re-judges every unit
// when the at-war flags change (refreshUnitHostility); the PSP asks the maps
// when it needs the answer (the result is the same, and nothing is kept per
// unit).
// PSP: the table is read once at world entry from data/dbc.pak and the pak is
// closed at once (the card driver's open-file limit); the race map
// is ~12 B per template instead of upstream's three unordered_maps.
#include "platform/psp/psp_world_session.hpp"

#include "game/reputation_standing.hpp"
#include "game/update_field_table.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"

#include <pspthreadman.h>

#include <algorithm>
#include <cstring>

namespace wowee::platform::psp {

namespace fmt = wowee::rendering::psp;

bool WorldSession::loadFactions() {
    const game::Character* me = activeCharacter();
    const uint8_t race = me ? static_cast<uint8_t>(me->race) : 0;
    const uint8_t cls = me ? static_cast<uint8_t>(me->characterClass) : 0;
    if (factionsLoaded() && race == factionRace_) return true;
    const uint64_t t0 = sceKernelGetSystemTimeWide();
    char path[256];
    fsPath(path, sizeof(path), "data/dbc.pak");
    Pak pak;
    if (!pak.open(path)) {
        PSP_LOGW(tag::Net, "factions_missing reason=nopak (targets: all units)");
        return false;
    }
    const PakEntry* e = pak.find("factions");
    void* blob = e ? pak.load(e) : nullptr;
    pak.close();  // one-off: never kept open (card open-file limit)
    fmt::FactionTableView v;
    if (!blob || !v.attach(blob, e->size)) {
        if (blob) pakFree(blob);
        PSP_LOGW(tag::Net, "factions_missing reason=%s (targets: all units)", e ? "bad" : "noentry");
        return false;
    }
    fmt::buildFactionReactions(v, race, factionReactions_);
    factionRows_.assign(v.factions, v.factions + v.h->factionCount);
    const uint32_t kb = e->size / 1024u;
    pakFree(blob);
    factionRace_ = race;
    factionClass_ = cls;
    const auto& m = factionReactions_;
    PSP_LOGI(tag::Net, "factions n=%u hostile=%u friendly=%u standings=%u atwar=%u race=%u ft=%u kb=%u ms=%u",
             static_cast<unsigned>(m.templates.size()), static_cast<unsigned>(m.hostileCount),
             static_cast<unsigned>(m.friendlyCount), static_cast<unsigned>(m.repFactions),
             static_cast<unsigned>(m.atWarCount), static_cast<unsigned>(race), static_cast<unsigned>(m.playerFtId), static_cast<unsigned>(kb),
             static_cast<unsigned>((sceKernelGetSystemTimeWide() - t0) / 1000u));
    return true;
}

// upstream GameHandler::factionAtWarLive.
int WorldSession::factionAtWarLive(uint32_t factionTemplateId) const {
    const fmt::FactionReaction* r = factionReactions_.find(factionTemplateId);
    if (!r || r->repListId == fmt::kNoRepList || r->repListId >= initialFactions_.size()) return -1;
    return (initialFactions_[r->repListId].flags & fmt::kFactionFlagAtWar) != 0 ? 1 : 0;
}

// upstream GameHandler::isHostileFaction: unknown templates are hostile.
bool WorldSession::isHostileFaction(uint32_t factionTemplateId) const {
    if (const int atWar = factionAtWarLive(factionTemplateId); atWar >= 0) return atWar == 1;
    const fmt::FactionReaction* r = factionReactions_.find(factionTemplateId);
    return r ? r->hostile != 0 : true;
}

// upstream GameHandler::isFriendlyFaction: unknown templates are not friendly.
bool WorldSession::isFriendlyFaction(uint32_t factionTemplateId) const {
    if (const int atWar = factionAtWarLive(factionTemplateId); atWar >= 0) return atWar == 0;
    const fmt::FactionReaction* r = factionReactions_.find(factionTemplateId);
    return r ? r->friendly != 0 : false;
}

uint32_t WorldSession::unitFactionTemplate(uint64_t guid) const {
    auto e = entities_.getEntity(guid);
    return e ? e->getField(game::fieldIndex(game::UF::UNIT_FIELD_FACTIONTEMPLATE)) : 0u;
}

bool WorldSession::isHostileUnit(uint64_t guid) const {
    if (guid == 0 || guid == playerGuid_) return false;
    if (duelOpponent(guid)) return true;  // duel teams
    if (!factionsLoaded()) return false;
    return isHostileFaction(unitFactionTemplate(guid));
}

// upstream GameHandler::unitReactionToPlayer.
int WorldSession::unitReactionToPlayer(uint64_t guid) const {
    if (duelOpponent(guid)) return 2;  // hostile while the duel runs
    const uint32_t ft = unitFactionTemplate(guid);
    const fmt::FactionReaction* r = factionReactions_.find(ft);
    if (r && r->repListId != fmt::kNoRepList && r->repListId < initialFactions_.size()) {
        const auto& standing = initialFactions_[r->repListId];
        // PSP: the earned standing is kept; upstream adds the base on arrival.
        int32_t whole = standing.standing;
        for (const fmt::FactionRow& f : factionRows_)
            if (f.repListId == r->repListId) {
                whole += fmt::factionBaseReputation(f, factionRace_, factionClass_);
                break;
            }
        int rank = game::reputationStandingFor(whole).id;
        if (standing.flags & fmt::kFactionFlagAtWar) rank = std::min(rank, 4);
        return rank;
    }
    if (isHostileFaction(ft)) return 2;
    return isFriendlyFaction(ft) ? 5 : 4;
}

float WorldSession::unitSplineSpeed(uint64_t guid) const {
    auto it = unitSplineSpeed_.find(guid);
    return it == unitSplineSpeed_.end() ? 0.0f : it->second;
}

// upstream SocialHandler faction opcodes (3.3.5a layouts).
bool WorldSession::handleFactionPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    static uint32_t logs = 0;
    switch (op) {
        case LogicalOpcode::SMSG_INITIALIZE_FACTIONS: {
            if (!packet.hasRemaining(4)) return true;
            const uint32_t count = packet.readUInt32();
            if (!packet.hasRemaining(static_cast<size_t>(count) * 5)) {
                packet.skipAll();
                return true;
            }
            initialFactions_.clear();
            initialFactions_.reserve(count);
            uint32_t atWar = 0;
            for (uint32_t i = 0; i < count; ++i) {
                FactionStandingInit fs;
                fs.flags = packet.readUInt8();
                fs.standing = static_cast<int32_t>(packet.readUInt32());
                if (fs.flags & fmt::kFactionFlagAtWar) ++atWar;
                initialFactions_.push_back(fs);
            }
            PSP_LOGI(tag::Net, "factions_init n=%u atwar=%u", static_cast<unsigned>(count), static_cast<unsigned>(atWar));
            ++factionVersion_;  // UPDATE_FACTION
            return true;
        }
        case LogicalOpcode::SMSG_SET_FACTION_STANDING: {
            // 3.3.5a: RAF bonus (float), showVisual (uint8), count, entries.
            if (!packet.hasRemaining(9)) return true;
            (void)packet.readFloat();
            (void)packet.readUInt8();
            const uint32_t count = std::min<uint32_t>(packet.readUInt32(), 128u);
            for (uint32_t i = 0; i < count && packet.hasRemaining(8); ++i) {
                const uint32_t repListId = packet.readUInt32();
                const int32_t standing = static_cast<int32_t>(packet.readUInt32());
                if (repListId < initialFactions_.size()) initialFactions_[repListId].standing = standing;
                ++factionVersion_;  // UPDATE_FACTION
                if (++logs <= 60)
                    PSP_LOGI(tag::Net, "faction_standing rep=%u standing=%d", static_cast<unsigned>(repListId),
                             static_cast<int>(standing));
            }
            return true;
        }
        case LogicalOpcode::SMSG_SET_FACTION_ATWAR: {
            if (!packet.hasRemaining(5)) {
                packet.skipAll();
                return true;
            }
            const uint32_t repListId = packet.readUInt32();
            const uint8_t setAtWar = packet.readUInt8();
            if (repListId < initialFactions_.size()) {
                if (setAtWar) initialFactions_[repListId].flags |= fmt::kFactionFlagAtWar;
                else initialFactions_[repListId].flags &= static_cast<uint8_t>(~fmt::kFactionFlagAtWar);
                ++factionVersion_;  // UPDATE_FACTION
            }
            if (++logs <= 60)
                PSP_LOGI(tag::Net, "faction_atwar rep=%u on=%u", static_cast<unsigned>(repListId),
                         static_cast<unsigned>(setAtWar));
            return true;
        }
        case LogicalOpcode::SMSG_SET_FACTION_VISIBLE: {
            // upstream SocialHandler::handleSetFactionVisible - a faction met for the first
            // time joins the reputation list (FACTION_FLAG_VISIBLE 0x01).
            if (!packet.hasRemaining(5)) {
                packet.skipAll();
                return true;
            }
            const uint32_t repListId = packet.readUInt32();
            const uint8_t visible = packet.readUInt8();
            if (repListId < initialFactions_.size()) {
                if (visible) initialFactions_[repListId].flags |= 0x01u;
                else initialFactions_[repListId].flags &= static_cast<uint8_t>(~0x01u);
                ++factionVersion_;
            }
            if (++logs <= 60)
                PSP_LOGI(tag::Net, "faction_visible rep=%u on=%u", static_cast<unsigned>(repListId),
                         static_cast<unsigned>(visible));
            return true;
        }
        default: return false;
    }
}

} // namespace wowee::platform::psp
