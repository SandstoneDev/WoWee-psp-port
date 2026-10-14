// class trainers for the PSP world session - upstream
// InventoryHandler's trainer half (src/game/inventory_handler.cpp:
// handleTrainerList, trainSpell, closeTrainer, categorizeTrainerSpells,
// handleTrainerBuySucceeded / handleTrainerBuyFailed) and GameHandler's
// closeIfTooFar for the trainer, without GameHandler. Upstream's parser and
// builder are used unchanged (TrainerListParser, TrainerBuySpellPacket).
//
// The way in is batch 3's NPC gossip: the trainer option goes out as
// CMSG_GOSSIP_SELECT_OPTION and the server answers SMSG_TRAINER_LIST (a
// trainer without a gossip menu answers the hello with it directly).
//
// PSP differences (marked PSP: below):
//   - skill line names / categories from spells.pak "skills" (SpellDb)
//     instead of SkillLine.dbc / SkillLineAbility.dbc;
//   - client messages in Russian into the chat and the red error line (no
//     UIErrorsFrame; upstream's English otherwise);
//   - sounds through the session's queue (QuestActivate / Error), no error
//     speech.
#include "platform/psp/psp_world_session.hpp"

#include "core/coordinates.hpp"
#include "game/protocol_constants.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kSkillLineCategoryClass = 7;  // upstream SKILLLINE_CATEGORY_CLASS
const std::string kNoName;
}  // namespace

// One dispatch line in onPacket (before the spells handler: the talent part
// watches SMSG_LEARNED_SPELL without taking it).
bool WorldSession::handleTrainerPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_TRAINER_LIST: handleTrainerList(packet); return true;
        case LogicalOpcode::SMSG_TRAINER_BUY_SUCCEEDED: {
            if (!packet.hasRemaining(12)) return true;
            packet.readUInt64();
            const uint32_t spellId = packet.readUInt32();
            if (!hasKnownSpell(spellId)) {  // upstream addKnownSpell
                knownSpells_.insert(spellId);
                ++knownSpellsVersion_;
            }
            const std::string& name = spellDb_ ? spellDb_->name(spellId) : kNoName;
            // upstream "You have learned X." / "Spell learned." - the client's ERR_LEARN_SPELL_S.
            addSystemChatMessage(!name.empty() ? csf(CS::ERR_LEARN_SPELL_S, name) : std::string(ps(PS::SPELL_LEARNED)));
            queueSound(SessionSound::QuestActivate);  // upstream playQuestActivate
            ++trainerVersion_;                        // TRAINER_UPDATE, SPELLS_CHANGED
            pendingTrainerBuy_ = 0;
            PSP_LOGI(tag::Net, "trainer_buy spell=%u ok=1 err=0", static_cast<unsigned>(spellId));
            return true;
        }
        case LogicalOpcode::SMSG_TRAINER_BUY_FAILED: {
            if (!packet.hasRemaining(12)) return true;
            packet.readUInt64();
            const uint32_t spellId = packet.readUInt32();
            uint32_t errorCode = 0;
            if (packet.hasRemaining(4)) errorCode = packet.readUInt32();
            const std::string& spellName = spellDb_ ? spellDb_->name(spellId) : kNoName;
            // upstream "Cannot learn X (not enough money)" / "(requirements not met)".
            const std::string what = !spellName.empty() ? spellName : psf(PS::SPELL_NUMBER, spellId);
            const std::string msg = psf(errorCode == 1 ? PS::TRAINER_CANT_LEARN_MONEY : PS::TRAINER_CANT_LEARN_REQS, what);
            spellError(msg);  // upstream addUIError
            addSystemChatMessage(msg);
            queueSound(SessionSound::Error);  // upstream playError (+ error speech: not on the PSP)
            ++trainerVersion_;
            pendingTrainerBuy_ = 0;
            PSP_LOGI(tag::Net, "trainer_buy spell=%u ok=0 err=%u", static_cast<unsigned>(spellId),
                     static_cast<unsigned>(errorCode));
            return true;
        }
        default:
            return handleTalentPacket(op, packet);  // psp_world_session_talents.cpp
    }
}

void WorldSession::handleTrainerList(network::Packet& packet) {
    // PSP: 3.3.5a only (upstream isClassicLikeExpansion() = false here).
    if (!game::TrainerListParser::parse(packet, currentTrainerList_, false)) return;
    trainerWindowOpen_ = true;
    closeGossip();  // upstream owner_.closeGossip() - TRAINER_SHOW
    size_t available = 0, unavailable = 0, known = 0;
    for (const auto& sp : currentTrainerList_.spells) {
        if (sp.state == 0) ++available;
        else if (sp.state == 2) ++known;
        else ++unavailable;
    }
    if (spellDb_) {  // the rows' names / icons in one read (upstream loadSpellNameCache)
        std::vector<uint32_t> ids;
        ids.reserve(currentTrainerList_.spells.size());
        for (const auto& sp : currentTrainerList_.spells) ids.push_back(sp.spellId);
        spellDb_->prefetch(ids);
    }
    categorizeTrainerSpells();
    ++trainerVersion_;
    PSP_LOGI(tag::Net, "trainer_list n=%u npc=%08X type=%u avail=%u unavail=%u known=%u tabs=%u",
             static_cast<unsigned>(currentTrainerList_.spells.size()),
             static_cast<unsigned>(currentTrainerList_.trainerGuid & 0xFFFFFFFFu),
             static_cast<unsigned>(currentTrainerList_.trainerType), static_cast<unsigned>(available),
             static_cast<unsigned>(unavailable), static_cast<unsigned>(known), static_cast<unsigned>(trainerTabs_.size()));
}

// upstream InventoryHandler::trainSpell
void WorldSession::trainSpell(uint32_t spellId) {
    if (state_ != State::InWorld || !socket_) return;
    uint32_t spellCost = 0;
    for (const auto& spell : currentTrainerList_.spells) {
        if (spell.spellId == spellId) {
            spellCost = spell.spellCost;
            break;
        }
    }
    PSP_LOGI(tag::Net, "trainer_buy_req spell=%u cost=%u money=%u", static_cast<unsigned>(spellId),
             static_cast<unsigned>(spellCost), static_cast<unsigned>(getMoneyCopper()));
    socket_->send(game::TrainerBuySpellPacket::build(currentTrainerList_.trainerGuid, spellId));
    pendingTrainerBuy_ = spellId;
}

// upstream InventoryHandler::closeTrainer (TRAINER_CLOSED)
void WorldSession::closeTrainer() {
    trainerWindowOpen_ = false;
    currentTrainerList_ = game::TrainerListData{};
    trainerTabs_.clear();
    ++trainerVersion_;
}

const std::string& WorldSession::getSkillLineName(uint32_t skillLineId) const {
    const SpellDb::SkillLineInfo* s = spellDb_ ? spellDb_->skillLine(skillLineId) : nullptr;
    return s ? s->name : kNoName;
}

// upstream InventoryHandler::categorizeTrainerSpells: one tab per class skill
// line (category 7), sorted by name, then "General". PSP: the skill lines
// from spells.pak "skills"; without them everything is "General".
void WorldSession::categorizeTrainerSpells() {
    trainerTabs_.clear();
    std::map<uint32_t, std::vector<const game::TrainerSpell*>> specialtySpells;
    std::vector<const game::TrainerSpell*> generalSpells;
    for (const auto& spell : currentTrainerList_.spells) {
        const SpellInfo* info = spellDb_ ? spellDb_->get(spell.spellId) : nullptr;
        const uint32_t skillLineId = info ? info->rec.skillLine : 0;
        const SpellDb::SkillLineInfo* line = skillLineId && spellDb_ ? spellDb_->skillLine(skillLineId) : nullptr;
        if (line && line->category == kSkillLineCategoryClass) {
            specialtySpells[skillLineId].push_back(&spell);
            continue;
        }
        generalSpells.push_back(&spell);
    }
    auto name = [this](uint32_t id) -> const std::string& { return spellDb_ ? spellDb_->name(id) : kNoName; };
    auto byName = [&](const game::TrainerSpell* a, const game::TrainerSpell* b) {
        return name(a->spellId) < name(b->spellId);
    };
    std::vector<std::pair<std::string, std::vector<const game::TrainerSpell*>>> named;
    for (auto& [skillLineId, spells] : specialtySpells) {
        const std::string& n = getSkillLineName(skillLineId);
        std::sort(spells.begin(), spells.end(), byName);
        named.emplace_back(n.empty() ? std::string(ps(PS::TRAINER_SPECIALTY)) : n, std::move(spells));
    }
    std::sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [n, spells] : named) trainerTabs_.push_back({n, std::move(spells)});
    if (!generalSpells.empty()) {
        std::sort(generalSpells.begin(), generalSpells.end(), byName);
        trainerTabs_.push_back({cs(CS::GENERAL), std::move(generalSpells)});
    }
}

// upstream GameHandler::update closeIfTooFar (trainer) + the talent-learn watch.
void WorldSession::trainerUpdate(float dtSec, const glm::vec3& selfPos) {
    PROF_SCOPE("trainer");
    if (trainerWindowOpen_ && currentTrainerList_.trainerGuid != 0) {
        if (auto npc = entities_.getEntity(currentTrainerList_.trainerGuid)) {
            const glm::vec3 p = core::coords::canonicalToServer(glm::vec3(npc->getX(), npc->getY(), npc->getZ()));
            const float dx = selfPos.x - p.x, dy = selfPos.y - p.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > game::NPC_INTERACT_MAX_DISTANCE) {
                closeTrainer();
                PSP_LOGI(tag::Net, "npc_window_far dist=%.1f kind=trainer", distance);
            }
        }
    }
    if (pendingTalentId_ != 0) {
        pendingTalentTimer_ -= dtSec;
        if (pendingTalentTimer_ <= 0.0f) {  // no SMSG_TALENTS_INFO with the rank: the server said no
            PSP_LOGI(tag::Net, "talent_learn id=%u rank=%u ok=0 reason=timeout", static_cast<unsigned>(pendingTalentId_),
                     static_cast<unsigned>(pendingTalentRank_));
            pendingTalentId_ = 0;
        }
    }
}

}  // namespace wowee::platform::psp
