// the achievements' session half - upstream GameHandler::handleAllAchievementData,
// the SMSG_CRITERIA_UPDATE / SMSG_ACHIEVEMENT_DELETED / SMSG_CRITERIA_DELETED / SMSG_SERVER_FIRST_ACHIEVEMENT /
// SMSG_RESPOND_INSPECT_ACHIEVEMENTS handlers of game_handler_packets.cpp and SpellHandler::handleAchievementEarned,
// without GameHandler; the Achievement*.dbc side comes from data/wotlk.pak ("ach" index, "ac_<category>" names /
// descriptions / criteria - psp_wotlk_db.hpp), loaded on demand and never kept open.
//
// Unchanged in what they read: the earned list (id + packed date until -1), the criteria records
// (game::readCriteriaProgressTail: the counter and the player guid packed), the own earned achievement (packed guid,
// id, packed date) - which upstream announces as ACHIEVEMENT_EARNED and the world screen shows as
// AchievementAlertFrame (takeAchievementEarned).
// PSP differences:
//  - No chat line of our own for SMSG_ACHIEVEMENT_EARNED: upstream writes "Achievement earned: ..." (English); the
//    server also sends CHAT_MSG_ACHIEVEMENT (its broadcast text, "$a" = the link) and the original prints only that
//    (upstream's comment says the same for other players). The link's name comes from achievementName() (the chat
//    file asks for it) instead of "[Achievement #id]".
//  - SMSG_SERVER_FIRST_ACHIEVEMENT: the original's SERVER_FIRST_ACHIEVEMENT ruRU with the link, not upstream's English.
//  - SMSG_RESPOND_INSPECT_ACHIEVEMENTS: the earned list only (the comparison tab); upstream read the criteria block
//    with fixed-size counters (they are packed) - here the reader stops after the achievements.
#include "platform/psp/psp_world_session.hpp"

#include "game/achievement_criteria.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"
#include "platform/psp/psp_pak.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace wowee::platform::psp {

// --- data/wotlk.pak "ach" / "ac_<id>" -------------------------------------------------------------------------------

bool AchIndex::load() {
    release();
    tried_ = true;
    Pak pak;
    if (!wotlkPakOpen(pak)) return false;
    uint32_t size = 0;
    void* blob = wotlkPakRaw(pak, "ach", size);
    pak.close();
    if (!blob) return false;
    const auto* h = static_cast<const AchPakHeader*>(blob);
    if (size < sizeof(AchPakHeader) || std::memcmp(h->magic, "WACH", 4) != 0 || h->version != 1 ||
        h->catOff + h->categoryCount * sizeof(AchCatRec) > size ||
        h->achOff + h->achievementCount * sizeof(AchIdxRec) > size || h->strOff + h->strSize > size ||
        h->strSize == 0 || static_cast<const char*>(blob)[h->strOff + h->strSize - 1] != '\0') {
        PSP_LOGW(tag::Net, "wotlk_ach_bad size=%u", static_cast<unsigned>(size));
        pakFree(blob);
        return false;
    }
    blob_ = blob;
    size_ = size;
    hdr_ = h;
    const char* b = static_cast<const char*>(blob);
    cats_ = reinterpret_cast<const AchCatRec*>(b + h->catOff);
    recs_ = reinterpret_cast<const AchIdxRec*>(b + h->achOff);
    strings_ = b + h->strOff;
    strSize_ = h->strSize;
    return true;
}

void AchIndex::release() {
    if (blob_) pakFree(blob_);
    blob_ = nullptr;
    size_ = 0;
    hdr_ = nullptr;
    cats_ = nullptr;
    recs_ = nullptr;
    strings_ = nullptr;
    strSize_ = 0;
}

const AchCatRec* AchIndex::category(uint32_t id) const {
    for (uint32_t i = 0; i < categoryCount(); ++i)
        if (cats_[i].id == id) return &cats_[i];
    return nullptr;
}

const AchIdxRec* AchIndex::find(uint32_t id) const {
    if (!hdr_) return nullptr;
    const AchIdxRec* end = recs_ + hdr_->achievementCount;
    const AchIdxRec* it = std::lower_bound(recs_, end, id, [](const AchIdxRec& r, uint32_t v) { return r.id < v; });
    return it != end && it->id == id ? it : nullptr;
}

bool AchCategory::load(uint32_t categoryId) {
    release();
    Pak pak;
    if (!wotlkPakOpen(pak)) return false;
    char name[24];
    std::snprintf(name, sizeof(name), "ac_%u", static_cast<unsigned>(categoryId));
    uint32_t size = 0;
    void* blob = wotlkPakRaw(pak, name, size);
    pak.close();
    if (!blob) return false;
    const auto* h = static_cast<const AchCatHeader*>(blob);
    const uint32_t body = sizeof(AchCatHeader) + h->count * sizeof(AchRec) + h->critCount * sizeof(AchCritRec);
    if (size < sizeof(AchCatHeader) || std::memcmp(h->magic, "WACC", 4) != 0 || h->version != 1 || body > size ||
        h->strOff < body || h->strOff >= size || static_cast<const char*>(blob)[size - 1] != '\0') {
        PSP_LOGW(tag::Net, "wotlk_ac_bad cat=%u size=%u", static_cast<unsigned>(categoryId), static_cast<unsigned>(size));
        pakFree(blob);
        return false;
    }
    blob_ = blob;
    size_ = size;
    id_ = categoryId;
    hdr_ = h;
    const char* b = static_cast<const char*>(blob);
    recs_ = reinterpret_cast<const AchRec*>(b + sizeof(AchCatHeader));
    crits_ = reinterpret_cast<const AchCritRec*>(b + sizeof(AchCatHeader) + h->count * sizeof(AchRec));
    strings_ = b + h->strOff;
    strSize_ = size - h->strOff;
    return true;
}

void AchCategory::release() {
    if (blob_) pakFree(blob_);
    blob_ = nullptr;
    size_ = 0;
    id_ = 0;
    hdr_ = nullptr;
    recs_ = nullptr;
    crits_ = nullptr;
    strings_ = nullptr;
    strSize_ = 0;
}

const AchRec* AchCategory::find(uint32_t achievementId) const {
    for (uint32_t i = 0; i < count(); ++i)
        if (recs_[i].id == achievementId) return &recs_[i];
    return nullptr;
}

// --- Packets ---------------------------------------------------------------------------------------------------------

bool WorldSession::handleAchievementPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_ALL_ACHIEVEMENT_DATA: {  // upstream handleAllAchievementData
            achEarned_.clear();
            while (packet.hasRemaining(4)) {
                const uint32_t id = packet.readUInt32();
                if (id == 0xFFFFFFFFu) break;
                if (!packet.hasRemaining(4)) break;
                achEarned_[id] = packet.readUInt32();
            }
            achCriteria_.clear();
            while (packet.hasRemaining(4)) {
                const uint32_t id = packet.readUInt32();
                if (id == 0xFFFFFFFFu) break;
                game::CriteriaProgressRecord rec;
                if (!game::readCriteriaProgressTail(packet, rec)) break;
                CriteriaProgress& p = achCriteria_[id];
                p.counter = rec.counter;
                p.flags = rec.flags;
                p.date = rec.date;
                p.elapsed = rec.timeElapsed;
            }
            packet.skipAll();
            achKnown_ = true;
            ++achVersion_;
            PSP_LOGI(tag::Net, "ach_all earned=%u criteria=%u", static_cast<unsigned>(achEarned_.size()),
                     static_cast<unsigned>(achCriteria_.size()));
            return true;
        }
        case LogicalOpcode::SMSG_CRITERIA_UPDATE: {  // upstream: the id, then the shared record
            if (!packet.hasRemaining(4)) return true;
            const uint32_t id = packet.readUInt32();
            game::CriteriaProgressRecord rec;
            if (game::readCriteriaProgressTail(packet, rec)) {
                CriteriaProgress& p = achCriteria_[id];
                // A timed criteria whose window ran out reads as zero (upstream).
                p.counter = rec.flags == 1 ? 0 : rec.counter;
                p.flags = rec.flags;
                p.date = rec.date;
                p.elapsed = rec.timeElapsed;
                ++achVersion_;
                if (++achLogs_ <= 60)
                    PSP_LOGI(tag::Net, "ach_criteria id=%u counter=%u", static_cast<unsigned>(id),
                             static_cast<unsigned>(p.counter));
            }
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_ACHIEVEMENT_EARNED: {  // upstream SpellHandler::handleAchievementEarned
            if (!packet.hasFullPackedGuid()) return true;
            const uint64_t guid = packet.readPackedGuid();
            if (!packet.hasRemaining(8)) return true;
            const uint32_t id = packet.readUInt32();
            const uint32_t date = packet.readUInt32();
            packet.skipAll();
            if (guid == playerGuid_) {
                achEarned_[id] = date;
                ++achVersion_;
                if (achEarnedQueue_.size() < 8) achEarnedQueue_.push_back(id);
                PSP_LOGI(tag::Net, "ach_earned id=%u", static_cast<unsigned>(id));
            }
            return true;
        }
        case LogicalOpcode::SMSG_ACHIEVEMENT_DELETED:  // upstream: the id out of the earned list
            if (packet.hasRemaining(4)) {
                achEarned_.erase(packet.readUInt32());
                ++achVersion_;
            }
            packet.skipAll();
            return true;
        case LogicalOpcode::SMSG_CRITERIA_DELETED:
            if (packet.hasRemaining(4)) {
                achCriteria_.erase(packet.readUInt32());
                ++achVersion_;
            }
            packet.skipAll();
            return true;
        case LogicalOpcode::SMSG_SERVER_FIRST_ACHIEVEMENT: {  // name, guid, id, link flag
            if (!packet.hasData()) return true;
            const std::string who = packet.readString();
            if (packet.hasRemaining(12)) {
                packet.readUInt64();
                const uint32_t id = packet.readUInt32();
                // SERVER_FIRST_ACHIEVEMENT ("%s ... $a", PLAYER_SERVER_FIRST_ACHIEVEMENT with a player link: on the
                // PSP the name is plain text either way).
                std::string line = cs(CS::SERVER_FIRST_ACHIEVEMENT);
                if (const size_t at = line.find("%s"); at != std::string::npos) line.replace(at, 2, who);
                if (const size_t at = line.find("$a"); at != std::string::npos)
                    line.replace(at, 2, "[" + achievementName(id) + "]");
                addSystemChatMessage(line);
            }
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_RESPOND_INSPECT_ACHIEVEMENTS: {  // upstream: packed guid + the earned list
            if (!packet.hasFullPackedGuid()) return true;
            const uint64_t guid = packet.readPackedGuid();
            std::unordered_map<uint32_t, uint32_t> earned;
            while (packet.hasRemaining(4)) {
                const uint32_t id = packet.readUInt32();
                if (id == 0xFFFFFFFFu || !packet.hasRemaining(4)) break;
                earned[id] = packet.readUInt32();
            }
            packet.skipAll();  // PSP: the criteria block is not needed by the comparison tab
            if (guid) {
                // PSP: the earned lists of at most 4 inspected players - each is a map of up to
                // ~1000 achievements; a long session of inspecting would otherwise grow the heap without end.
                if (achInspected_.size() >= 4 && !achInspected_.count(guid)) achInspected_.clear();
                achInspected_[guid] = std::move(earned);
                ++achInspectVersion_;
                PSP_LOGI(tag::Net, "ach_inspect guid=%llx earned=%u", static_cast<unsigned long long>(guid),
                         static_cast<unsigned>(achInspected_[guid].size()));
            }
            return true;
        }
        default: return false;
    }
}

bool WorldSession::takeAchievementEarned(uint32_t& id) {
    if (achEarnedQueue_.empty()) return false;
    id = achEarnedQueue_.front();
    achEarnedQueue_.pop_front();
    return true;
}

// The achievement's name (Achievement.dbc Title_lang ruRU) for a chat link: the index finds its category, the
// category's entry has the name. Kept in a small cache (the chat asks once per line).
std::string WorldSession::achievementName(uint32_t id) {
    auto it = achNameCache_.find(id);
    if (it != achNameCache_.end()) return it->second;
    std::string name;
    AchIndex index;
    if (index.load()) {
        if (const AchIdxRec* r = index.find(id)) {
            AchCategory cat;
            if (cat.load(r->category))
                if (const AchRec* a = cat.find(id)) name = cat.str(a->name);
        }
    }
    if (name.empty()) name = "Achievement #" + std::to_string(id);  // upstream's fallback (no wotlk.pak)
    if (achNameCache_.size() > 64) achNameCache_.clear();
    achNameCache_[id] = name;
    return name;
}

void WorldSession::queryInspectAchievements(uint64_t guid) {  // upstream QueryInspectAchievementsPacket
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    socket_->send(game::QueryInspectAchievementsPacket::build(guid));
    PSP_LOGI(tag::Net, "ach_inspect_query guid=%llx", static_cast<unsigned long long>(guid));
}

const std::unordered_map<uint32_t, uint32_t>* WorldSession::inspectedAchievements(uint64_t guid) const {
    auto it = achInspected_.find(guid);
    return it != achInspected_.end() ? &it->second : nullptr;
}

}  // namespace wowee::platform::psp
