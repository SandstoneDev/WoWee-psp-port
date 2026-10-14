// talents and the spell book tabs for the PSP world session -
// upstream SpellHandler's talent half (src/game/spell_handler.cpp:
// handleTalentsInfo, MSG_TALENT_WIPE_CONFIRM, learnTalent,
// confirmTalentWipe / cancelTalentWipe, loadTalentDbc, the talent-rank part
// of handleLearnedSpell / handleRemovedSpell, resetTalentState for
// SMSG_TALENTS_INVOLUNTARILY_RESET) and getSpellBookTabs, without GameHandler.
// LearnTalentPacket is upstream's, unchanged (rank counted from one; the
// builder puts it on the wire from zero).
//
// PSP differences (marked PSP: below):
//   - Talent.dbc / TalentTab.dbc from spells.pak "talents", and only the
//     player's class kept (upstream keeps every class; the Lua API filters by
//     class anyway); without the entry (an old pak) the talent window says so;
//   - the prerequisites come from the 23-field Talent.dbc's own columns
//     (build_spells_pak.py --talent-prereq-cols; upstream's layout reads unused
//     rank columns, see there);
//   - glyphs are skipped (no glyph UI yet) and pre-3.3.5a sync is not built;
//   - messages in Russian; the chat line of an unspent-points reminder as
//     upstream's, once per login;
//   - getSpellBookTabs: "made" (a secondary skill with a recipe) = a spell with
//     SPELL_EFFECT_CREATE_ITEM - spells.pak has no reagents / created items.
#include "platform/psp/psp_world_session.hpp"

#include "game/update_field_table.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kCategoryClass = 7, kCategorySecondary = 9, kCategoryProfession = 11;  // SkillLine.dbc
constexpr uint8_t kEffectCreateItem = 24;                                                    // SPELL_EFFECT_CREATE_ITEM
constexpr float kTalentLearnTimeout = 3.0f;
}  // namespace

uint8_t WorldSession::getPlayerClass() const {
    const game::Character* me = activeCharacter();
    return me ? static_cast<uint8_t>(me->characterClass) : 0;
}

uint8_t WorldSession::getTalentRank(uint32_t talentId) const {
    const auto& learned = learnedTalents_[activeTalentSpec_];
    auto it = learned.find(talentId);
    return it != learned.end() ? it->second : 0;
}

const game::TalentEntry* WorldSession::getTalentEntry(uint32_t talentId) const {
    auto it = talentCache_.find(talentId);
    return it != talentCache_.end() ? &it->second : nullptr;
}

const game::TalentTabEntry* WorldSession::getTalentTabEntry(uint32_t tabId) const {
    auto it = talentTabCache_.find(tabId);
    return it != talentTabCache_.end() ? &it->second : nullptr;
}

uint32_t WorldSession::talentTabIconId(uint32_t tabId) const {
    auto it = talentTabIcons_.find(tabId);
    return it != talentTabIcons_.end() ? it->second : 0;
}

// upstream SpellHandler::loadTalentDbc over spells.pak "talents".
bool WorldSession::loadTalentDbc() {
    if (talentDbcLoaded_) return !talentTabCache_.empty();
    const uint8_t classId = getPlayerClass();
    // Not an attempt yet (upstream: the assets are not there to read yet).
    if (!spellDb_ || !spellDb_->loaded() || classId == 0) return false;
    talentDbcLoaded_ = true;
    std::vector<uint8_t> raw;
    if (!spellDb_->hasTalents() || !spellDb_->readTalents(raw) || raw.size() < sizeof(TalentPakHeader)) {
        PSP_LOGW(tag::Fs, "talents_missing (rebake spells.pak: build_spells_pak.py)");
        return false;
    }
    TalentPakHeader h;
    std::memcpy(&h, raw.data(), sizeof(h));
    const size_t tabBytes = static_cast<size_t>(h.tabCount) * sizeof(TalentTabRec);
    const size_t talBytes = static_cast<size_t>(h.talentCount) * sizeof(TalentRec);
    if (std::memcmp(h.magic, "WTAL", 4) != 0 || h.version != 1 || h.tabRecSize != sizeof(TalentTabRec) ||
        h.talentRecSize != sizeof(TalentRec) || raw.size() < sizeof(h) + tabBytes + talBytes + 1) {
        PSP_LOGE(tag::Fs, "talents_bad ver=%u tab=%u rec=%u", static_cast<unsigned>(h.version),
                 static_cast<unsigned>(h.tabRecSize), static_cast<unsigned>(h.talentRecSize));
        return false;
    }
    const uint8_t* tabsP = raw.data() + sizeof(h);
    const uint8_t* talsP = tabsP + tabBytes;
    const char* strings = reinterpret_cast<const char*>(talsP + talBytes);
    const size_t strBytes = raw.size() - 1 - sizeof(h) - tabBytes - talBytes;  // raw ends with an extra 0
    auto str = [&](uint32_t off) { return off < strBytes ? std::string(strings + off) : std::string(); };
    const uint32_t classMask = 1u << (classId - 1);
    for (uint32_t i = 0; i < h.tabCount; ++i) {
        TalentTabRec r;
        std::memcpy(&r, tabsP + i * sizeof(TalentTabRec), sizeof(r));
        if (r.id == 0 || !(r.classMask & classMask)) continue;  // PSP: the player's class only
        game::TalentTabEntry e;
        e.tabId = r.id;
        e.name = str(r.nameOff);
        e.classMask = r.classMask;
        e.orderIndex = r.orderIndex;
        e.backgroundFile = str(r.bgOff);
        talentTabCache_[e.tabId] = std::move(e);
        talentTabIcons_[r.id] = r.iconId;
    }
    for (uint32_t i = 0; i < h.talentCount; ++i) {
        TalentRec r;
        std::memcpy(&r, talsP + i * sizeof(TalentRec), sizeof(r));
        if (r.id == 0 || talentTabCache_.count(r.tabId) == 0) continue;
        game::TalentEntry e;
        e.talentId = r.id;
        e.tabId = r.tabId;
        e.row = r.row;
        e.column = r.column;
        for (int k = 0; k < 5; ++k) e.rankSpells[k] = r.rankSpell[k];
        for (int p = 0; p < 3; ++p) {
            e.prereqTalent[p] = r.prereqTalent[p];
            e.prereqRank[p] = r.prereqRank[p];
        }
        e.maxRank = 0;
        for (int k = 0; k < 5; ++k)
            if (e.rankSpells[k] != 0) e.maxRank = static_cast<uint8_t>(k + 1);
        for (int k = 0; k < 5; ++k)
            if (e.rankSpells[k] != 0) talentRankSpells_[e.rankSpells[k]] = {e.talentId, static_cast<uint8_t>(k + 1)};
        talentCache_[e.talentId] = e;
    }
    // The talent buttons' names / icons: their first ranks, one read.
    std::vector<uint32_t> ids;
    for (const auto& [id, t] : talentCache_) ids.push_back(t.rankSpells[0]);
    if (!ids.empty()) spellDb_->prefetch(ids);
    PSP_LOGI(tag::Fs, "talents_load class=%u tabs=%u talents=%u kb=%u", static_cast<unsigned>(classId),
             static_cast<unsigned>(talentTabCache_.size()), static_cast<unsigned>(talentCache_.size()),
             static_cast<unsigned>(raw.size() / 1024u));
    ++talentVersion_;
    return !talentTabCache_.empty();
}

bool WorldSession::isTalentRankSpell(uint32_t spellId) {
    loadTalentDbc();
    return talentRankSpells_.count(spellId) != 0;
}

// upstream handleLearnedSpell's talent part: a rank spell moves the rank.
void WorldSession::talentLearnedSpell(uint32_t spellId) {
    loadTalentDbc();
    auto it = talentRankSpells_.find(spellId);
    if (it == talentRankSpells_.end()) return;
    learnedTalents_[activeTalentSpec_][it->second.first] = it->second.second;
    ++talentVersion_;  // CHARACTER_POINTS_CHANGED, PLAYER_TALENT_UPDATE
}

// upstream SpellHandler::learnTalent
void WorldSession::learnTalent(uint32_t talentId, uint32_t requestedRank) {
    if (state_ != State::InWorld || !socket_) return;
    socket_->send(game::LearnTalentPacket::build(talentId, requestedRank));
    pendingTalentId_ = talentId;  // PSP: for the talent_learn ok= log line
    pendingTalentRank_ = requestedRank;
    pendingTalentTimer_ = kTalentLearnTimeout;
    PSP_LOGI(tag::Net, "talent_learn_req id=%u rank=%u unspent=%u", static_cast<unsigned>(talentId),
             static_cast<unsigned>(requestedRank), static_cast<unsigned>(getUnspentTalentPoints()));
}

// upstream SpellHandler::confirmTalentWipe
void WorldSession::confirmTalentWipe() {
    if (!talentWipePending_) return;
    talentWipePending_ = false;
    if (state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::Opcode::MSG_TALENT_WIPE_CONFIRM));
    pkt.writeUInt64(talentWipeNpcGuid_);
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "talent_wipe confirm=1 cost=%u", static_cast<unsigned>(talentWipeCost_));
    addSystemChatMessage(ps(PS::TALENT_WIPE_CONFIRMED));  // upstream's line
    talentWipeNpcGuid_ = 0;
    talentWipeCost_ = 0;
}

// upstream SpellHandler::handleTalentsInfo (talent type 0 = the player's own).
void WorldSession::handleTalentsInfo(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return;
    const uint8_t talentType = packet.readUInt8();
    if (talentType != 0) {  // pet talents: not modelled (upstream neither)
        packet.skipAll();
        return;
    }
    if (!packet.hasRemaining(6)) return;
    const uint32_t unspentTalents = packet.readUInt32();
    const uint8_t talentGroupCount = packet.readUInt8();
    uint8_t activeTalentGroup = packet.readUInt8();
    if (activeTalentGroup > 1) activeTalentGroup = 0;
    loadTalentDbc();
    activeTalentSpec_ = activeTalentGroup;
    for (uint8_t g = 0; g < talentGroupCount && g < 2; ++g) {
        if (!packet.hasRemaining(1)) break;
        const uint8_t talentCount = packet.readUInt8();
        learnedTalents_[g].clear();
        for (uint8_t t = 0; t < talentCount; ++t) {
            if (!packet.hasRemaining(5)) break;
            const uint32_t talentId = packet.readUInt32();
            const uint8_t rank = packet.readUInt8();
            learnedTalents_[g][talentId] = static_cast<uint8_t>(rank + 1u);
        }
        // PSP: glyphs read past, not kept (no glyph UI yet).
        if (!packet.hasRemaining(1)) break;
        const uint8_t glyphCount = packet.readUInt8();
        for (uint8_t gl = 0; gl < glyphCount && packet.hasRemaining(2); ++gl) packet.readUInt16();
    }
    unspentTalentPoints_[activeTalentGroup] = static_cast<uint8_t>(unspentTalents > 255 ? 255 : unspentTalents);
    ++talentVersion_;  // CHARACTER_POINTS_CHANGED, ACTIVE_TALENT_GROUP_CHANGED, PLAYER_TALENT_UPDATE
    PSP_LOGI(tag::Net, "talents_info points=%u specs=%u active=%u learned=%u", static_cast<unsigned>(unspentTalents),
             static_cast<unsigned>(talentGroupCount), static_cast<unsigned>(activeTalentGroup),
             static_cast<unsigned>(learnedTalents_[activeTalentGroup].size()));
    if (pendingTalentId_ != 0) {  // PSP: the answer to our CMSG_LEARN_TALENT
        const uint8_t have = getTalentRank(pendingTalentId_);
        PSP_LOGI(tag::Net, "talent_learn id=%u rank=%u ok=%d have=%u", static_cast<unsigned>(pendingTalentId_),
                 static_cast<unsigned>(pendingTalentRank_), have >= pendingTalentRank_ ? 1 : 0,
                 static_cast<unsigned>(have));
        pendingTalentId_ = 0;
    }
    if (!talentsInitialized_) {
        talentsInitialized_ = true;
        if (unspentTalents > 0)  // upstream "You have N unspent talent point(s)."
            addSystemChatMessage(psf(PS::TALENTS_UNSPENT, unspentTalents));
    }
}

bool WorldSession::handleTalentPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_TALENTS_INFO: handleTalentsInfo(packet); return true;
        case LogicalOpcode::MSG_TALENT_WIPE_CONFIRM: {
            if (!packet.hasRemaining(12)) {
                packet.skipAll();
                return true;
            }
            talentWipeNpcGuid_ = packet.readUInt64();
            talentWipeCost_ = packet.readUInt32();
            talentWipePending_ = true;  // CONFIRM_TALENT_WIPE
            PSP_LOGI(tag::Net, "talent_wipe ask npc=%08X cost=%u", static_cast<unsigned>(talentWipeNpcGuid_ & 0xFFFFFFFFu),
                     static_cast<unsigned>(talentWipeCost_));
            return true;
        }
        case LogicalOpcode::SMSG_TALENTS_INVOLUNTARILY_RESET: {  // upstream resetTalentState
            talentsInitialized_ = false;
            learnedTalents_[0].clear();
            learnedTalents_[1].clear();
            unspentTalentPoints_[0] = unspentTalentPoints_[1] = 0;
            activeTalentSpec_ = 0;
            ++talentVersion_;
            spellError(cs(CS::TALENTS_INVOLUNTARILY_RESET));  // upstream "Your talents have been reset by the server."
            addSystemChatMessage(cs(CS::TALENTS_INVOLUNTARILY_RESET));
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_LEARNED_SPELL: {
            // Watched, not taken: the spells handler (psp_world_session_spells.cpp)
            // still learns it. upstream handleLearnedSpell's talent-rank bookkeeping.
            if (packet.hasRemaining(4)) {
                network::Packet peek = packet;
                talentLearnedSpell(peek.readUInt32());
            }
            return false;
        }
        default:
            return false;
    }
}

// upstream SpellHandler::getSpellBookTabs: General first, then the class
// lines, professions and secondary skills with something made, alphabetical
// within each kind. Rebuilt when the known spells change.
const std::vector<WorldSession::SpellBookTab>& WorldSession::getSpellBookTabs() {
    if (spellBookTabsVersion_ == knownSpellsVersion_) return spellBookTabs_;
    PROF_SCOPE("trainer");
    spellBookTabsVersion_ = knownSpellsVersion_;
    spellBookTabs_.clear();
    if (!spellDb_ || !spellDb_->hasSkillLines()) {  // PSP: an old spells.pak - one tab, as before
        SpellBookTab all;
        all.name = cs(CS::GENERAL_SPELLS);
        all.iconId = spellDb_ ? spellDb_->generalTabIconId() : 0;
        all.spellIds.assign(knownSpells_.begin(), knownSpells_.end());
        spellBookTabs_.push_back(std::move(all));
        return spellBookTabs_;
    }
    auto lineOf = [&](uint32_t spellId) -> uint32_t {
        const SpellInfo* s = spellDb_->get(spellId);
        return s ? s->rec.skillLine : 0;
    };
    // PSP: "has a recipe" = creates an item (spells.pak has no reagents).
    auto lineHasRecipe = [&](uint32_t skillLine) {
        for (uint32_t known : knownSpells_) {
            const SpellInfo* s = spellDb_->get(known);
            if (!s || s->rec.skillLine != skillLine) continue;
            for (uint8_t e : s->rec.effect)
                if (e == kEffectCreateItem) return true;
        }
        return false;
    };
    auto earnsOwnTab = [&](uint32_t category, uint32_t skillLine) {
        if (category == kCategoryClass || category == kCategoryProfession) return true;
        if (category == kCategorySecondary) return lineHasRecipe(skillLine);
        return false;
    };
    std::map<uint32_t, std::vector<uint32_t>> bySkillLine;
    std::vector<uint32_t> general;
    for (uint32_t spellId : knownSpells_) {
        const uint32_t line = lineOf(spellId);
        const SpellDb::SkillLineInfo* sl = line ? spellDb_->skillLine(line) : nullptr;
        if (sl && earnsOwnTab(sl->category, line)) {
            bySkillLine[line].push_back(spellId);
            continue;
        }
        general.push_back(spellId);
    }
    auto byName = [&](uint32_t a, uint32_t b) { return spellDb_->name(a) < spellDb_->name(b); };
    if (!general.empty()) {
        std::sort(general.begin(), general.end(), byName);
        spellBookTabs_.push_back({cs(CS::GENERAL_SPELLS), spellDb_->generalTabIconId(), std::move(general)});
    }
    auto tabRank = [&](uint32_t line) {
        const SpellDb::SkillLineInfo* sl = spellDb_->skillLine(line);
        if (!sl) return 3;
        switch (sl->category) {
            case kCategoryClass: return 0;
            case kCategoryProfession: return 1;
            case kCategorySecondary: return 2;
            default: return 3;
        }
    };
    struct NamedTab {
        int rank;
        SpellBookTab tab;
    };
    std::vector<NamedTab> named;
    for (auto& [line, spells] : bySkillLine) {
        const SpellDb::SkillLineInfo* sl = spellDb_->skillLine(line);
        std::sort(spells.begin(), spells.end(), byName);
        SpellBookTab t;
        t.name = sl && !sl->name.empty() ? sl->name : std::string("Unknown");
        // upstream: the skill line's icon, the book as the fallback.
        t.iconId = sl && sl->iconId ? sl->iconId : spellDb_->generalTabIconId();
        t.spellIds = std::move(spells);
        named.push_back({tabRank(line), std::move(t)});
    }
    std::sort(named.begin(), named.end(), [](const NamedTab& a, const NamedTab& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.tab.name < b.tab.name;
    });
    for (auto& n : named) spellBookTabs_.push_back(std::move(n.tab));
    PSP_LOGI(tag::Gfx, "spellbook_tabs n=%u known=%u", static_cast<unsigned>(spellBookTabs_.size()),
             static_cast<unsigned>(knownSpells_.size()));
    return spellBookTabs_;
}

}  // namespace wowee::platform::psp
