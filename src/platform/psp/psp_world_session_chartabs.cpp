// what the character window's Reputation and Skills tabs
// (ReputationFrame / SkillFrame, src/ui/psp/ui_world_char_tabs.cpp) read and
// send, as upstream does it:
//   - GameHandler::getFactionStanding: SMSG_INITIALIZE_FACTIONS' earned
//     standing + factionBaseReputation (the race / class start of Faction.dbc);
//   - GameHandler::setFactionAtWar / setFactionInactive / setWatchedFactionId
//     (CMSG_SET_FACTION_ATWAR / CMSG_SET_FACTION_INACTIVE /
//     CMSG_SET_WATCHED_FACTION, the flags updated at once - the server sends
//     no confirmation for the inactive flag);
//   - SpellHandler::extractSkillFields: the 128 skill slots of
//     PLAYER_SKILL_INFO_1_1.. (3 fields each: id | step << 16, value | max << 16,
//     temporary bonus | permanent bonus << 16);
//   - GameHandler::unlearnSkill (CMSG_UNLEARN_SKILL - the server takes only a
//     primary profession).
// PSP: the skills are re-read only after an update block touched the skill
// fields (upstream re-reads them on every own update); upstream's English
// "Your skill in X has increased to N." line is not written (the ruRU
// SKILL_RANK_UP needs the chat's declension). The watched faction is read from
// PLAYER_FIELD_WATCHED_FACTION_INDEX as the client's GetWatchedFactionInfo
// (upstream keeps its own copy, set on every change of a standing).
#include "platform/psp/psp_world_session.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"

namespace wowee::platform::psp {

namespace fmt = wowee::rendering::psp;

namespace {
// upstream GameHandler::FACTION_FLAG_INACTIVE (game_handler.hpp).
constexpr uint8_t kFactionFlagInactive = 0x20;
// UpdateFields.h 3.3.5a: PLAYER_FIELD_WATCHED_FACTION_INDEX = UNIT_END + 0x043A, the field before
// PLAYER_FIELD_COMBAT_RATING_1 (0x043B) - not in upstream's field table.
uint16_t watchedFactionField() {
    const uint16_t cr = game::fieldIndex(game::UF::PLAYER_FIELD_COMBAT_RATING_1);
    return cr == 0xFFFF ? 0xFFFF : static_cast<uint16_t>(cr - 1);
}
uint32_t g_tabLogs = 0;
}  // namespace

bool WorldSession::factionStandingAt(uint32_t repListId, uint8_t& flags, int32_t& standing) const {
    if (repListId >= initialFactions_.size()) return false;
    flags = initialFactions_[repListId].flags;
    standing = initialFactions_[repListId].standing;
    // PSP: the earned standing is kept; upstream adds the base when the packet arrives (the same whole).
    for (const fmt::FactionRow& f : factionRows_)
        if (f.repListId == repListId) {
            standing += fmt::factionBaseReputation(f, factionRace_, factionClass_);
            break;
        }
    return true;
}

// upstream GameHandler::setFactionAtWar.
void WorldSession::setFactionAtWar(uint32_t repListId, bool atWar) {
    if (repListId >= initialFactions_.size()) return;
    // The server forbids declaring war on some factions; don't fight the flag.
    if (atWar && (initialFactions_[repListId].flags & fmt::kFactionFlagPeaceForced) != 0) return;
    // Optimistic local update; the server echoes SMSG_SET_FACTION_ATWAR to confirm.
    if (atWar) initialFactions_[repListId].flags |= fmt::kFactionFlagAtWar;
    else initialFactions_[repListId].flags &= static_cast<uint8_t>(~fmt::kFactionFlagAtWar);
    ++factionVersion_;
    if (++g_tabLogs <= 40)
        PSP_LOGI(tag::Net, "rep_atwar_set rep=%u on=%u", static_cast<unsigned>(repListId), atWar ? 1u : 0u);
    if (state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_SET_FACTION_ATWAR));
    pkt.writeUInt32(repListId);
    pkt.writeUInt8(atWar ? 1u : 0u);
    socket_->send(pkt);
}

// upstream GameHandler::setFactionInactive.
void WorldSession::setFactionInactive(uint32_t repListId, bool inactive) {
    if (repListId >= initialFactions_.size()) return;
    // No SMSG confirmation is sent for inactive, so update the local flag directly.
    if (inactive) initialFactions_[repListId].flags |= kFactionFlagInactive;
    else initialFactions_[repListId].flags &= static_cast<uint8_t>(~kFactionFlagInactive);
    ++factionVersion_;
    if (++g_tabLogs <= 40)
        PSP_LOGI(tag::Net, "rep_inactive_set rep=%u on=%u", static_cast<unsigned>(repListId), inactive ? 1u : 0u);
    if (state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_SET_FACTION_INACTIVE));
    pkt.writeUInt32(repListId);
    pkt.writeUInt8(inactive ? 1u : 0u);
    socket_->send(pkt);
}

// upstream GameHandler::setWatchedFactionId (by ReputationListID here: -1 = unwatch).
void WorldSession::setWatchedFaction(int32_t repListId) {
    // PSP: the field the client reads (GetWatchedFactionInfo) set at once; the server sets it too.
    const uint16_t f = watchedFactionField();
    if (f != 0xFFFF && !selfFields_.empty()) selfFields_[f] = static_cast<uint32_t>(repListId);
    ++factionVersion_;
    if (++g_tabLogs <= 40) PSP_LOGI(tag::Net, "rep_watch_set rep=%d", static_cast<int>(repListId));
    if (state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_SET_WATCHED_FACTION));
    pkt.writeUInt32(static_cast<uint32_t>(repListId));
    socket_->send(pkt);
}

int32_t WorldSession::watchedFactionIndex() const {
    const uint16_t f = watchedFactionField();
    if (f == 0xFFFF) return -1;
    auto it = selfFields_.find(f);
    return it == selfFields_.end() ? -1 : static_cast<int32_t>(it->second);
}

// upstream SpellHandler::extractSkillFields (without the skill-up chat line, see the top).
void WorldSession::extractSkillFields() {
    const uint16_t start = game::fieldIndex(game::UF::PLAYER_SKILL_INFO_START);
    if (start == 0xFFFF) return;
    static constexpr int MAX_SKILL_SLOTS = 128;
    std::vector<PlayerSkill> newSkills;
    for (int slot = 0; slot < MAX_SKILL_SLOTS; slot++) {
        const uint16_t baseField = static_cast<uint16_t>(start + slot * 3);
        auto idIt = selfFields_.find(baseField);
        if (idIt == selfFields_.end()) continue;
        const uint16_t skillId = idIt->second & 0xFFFF;
        if (skillId == 0) continue;
        auto valIt = selfFields_.find(static_cast<uint16_t>(baseField + 1));
        if (valIt == selfFields_.end()) continue;
        const uint32_t raw1 = valIt->second;
        PlayerSkill skill;
        skill.skillId = skillId;
        skill.value = raw1 & 0xFFFF;
        skill.maxValue = (raw1 >> 16) & 0xFFFF;
        auto bonusIt = selfFields_.find(static_cast<uint16_t>(baseField + 2));
        if (bonusIt != selfFields_.end()) {
            skill.bonusTemp = bonusIt->second & 0xFFFF;
            skill.bonusPerm = (bonusIt->second >> 16) & 0xFFFF;
        }
        // upstream keys the map by skill id: a second slot with the same id replaces the first.
        bool replaced = false;
        for (PlayerSkill& s : newSkills)
            if (s.skillId == skillId) {
                s = skill;
                replaced = true;
            }
        if (!replaced) newSkills.push_back(skill);
    }
    bool skillsChanged = newSkills.size() != playerSkills_.size();
    for (size_t i = 0; !skillsChanged && i < newSkills.size(); ++i) {
        const PlayerSkill* old = nullptr;
        for (const PlayerSkill& s : playerSkills_)
            if (s.skillId == newSkills[i].skillId) old = &s;
        // PSP: any change of the bar counts (upstream: a new skill or a new value) - the tab shows max / bonuses.
        skillsChanged = !old || old->value != newSkills[i].value || old->maxValue != newSkills[i].maxValue ||
                        old->bonusTemp != newSkills[i].bonusTemp || old->bonusPerm != newSkills[i].bonusPerm;
    }
    playerSkills_ = std::move(newSkills);
    if (skillsChanged) {
        ++skillsVersion_;  // upstream fireAddonEvent("SKILL_LINES_CHANGED")
        if (++g_tabLogs <= 40)
            PSP_LOGI(tag::Net, "skills_extract n=%u version=%u", static_cast<unsigned>(playerSkills_.size()),
                     static_cast<unsigned>(skillsVersion_));
    }
}

const std::vector<WorldSession::PlayerSkill>& WorldSession::playerSkills() {
    if (skillsDirty_) {
        skillsDirty_ = false;
        extractSkillFields();
    }
    return playerSkills_;
}

// upstream GameHandler::unlearnSkill.
void WorldSession::unlearnSkill(uint32_t skillId) {
    if (skillId == 0 || state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_UNLEARN_SKILL));
    pkt.writeUInt32(skillId);
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "skill_unlearn skill=%u", static_cast<unsigned>(skillId));
}

}  // namespace wowee::platform::psp
