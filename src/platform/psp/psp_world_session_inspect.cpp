// the inspect for the PSP world session - upstream
// SocialHandler's inspect half (src/game/social_handler.cpp: inspectUnit, handleInspectResults' 3.3.5a branch,
// requestInspectHonorData, the MSG_INSPECT_HONOR_STATS handler) without GameHandler.
//
// The layouts are the servers' (AzerothCore / TrinityCore 3.3.5 MiscHandler.cpp, Player.cpp, ArenaTeam.cpp):
//   - CMSG_INSPECT carries the guid as a uint64 (HandleInspectOpcode `recvData >> guid`). Upstream's InspectPacket
//     writes a packed guid on 3.3.5a, which the server reads past the end of: no answer. With it goes
//     CMSG_QUERY_INSPECT_ACHIEVEMENTS (upstream inspectUnit);
//   - SMSG_INSPECT_TALENT: the packed guid, the talents (unspent u32, group count u8, active group u8, per
//     group a u8 count of (talent u32, rank u8) and a u8 count of u16 glyphs), then the items: a u32 slot
//     mask and per set slot the entry u32, a u16 enchantment mask with a u16 per set bit, a u16, the creator's
//     packed guid and a u32 (Player::BuildEnchantmentsInfoData). Upstream stops after the enchantments and reads the
//     next item out of those bytes - every item after the first came out wrong;
//   - every answer is taken only for the player being inspected - upstream's MSG_INSPECT_HONOR_STATS check applied
//     to the talents and the arena teams too; a new player drops the talents, the honour and the arena teams;
//   - MSG_INSPECT_HONOR_STATS: guid, u8 rank, u16 today / u16 yesterday kills, u32 today / yesterday honour, u32
//     lifetime kills (GetInspectHonorData returns them in that sense) - upstream read the two
//     kill counts as one u32, the same bytes;
//   - MSG_INSPECT_ARENA_TEAMS (upstream: not handled; ArenaTeam::Inspect): guid, u8 slot (0..2), u32 team id,
//     rating, games, wins, played, personal rating; RequestInspectHonorData asks it beside the honour. A team id not
//     known yet is asked by CMSG_ARENA_TEAM_QUERY when it first shows up (the port's rule for any unknown id, as the
//     name and guild queries) - its name and colours for GetInspectArenaTeamData.
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_spell_db.hpp"
#include "platform/psp/psp_talent_format.hpp"

#include <algorithm>
#include <cstring>

namespace wowee::platform::psp {

namespace {
constexpr uint16_t kVisibleItemBase = 283;  // PLAYER_VISIBLE_ITEM_1_ENTRYID (3.3.5a), stride 2 (as psp_world_session.cpp)
}  // namespace

// NotifyInspect: CMSG_INSPECT with the uint64 guid and the achievement query (upstream inspectUnit), then the inspect
// guid (a new player clears the honour state, the arena teams and the talents).
void WorldSession::notifyInspect(uint64_t guid) {
    if (state_ != State::InWorld || !socket_ || guid == 0) return;
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_INSPECT));
    packet.writeUInt64(guid);  // PSP: the server's plain uint64 (upstream's InspectPacket: a packed guid on 3.3.5a)
    socket_->send(packet);
    queryInspectAchievements(guid);
    if (inspect_.guid != guid) {
        inspect_ = InspectResult{};
        inspect_.guid = guid;
        inspectTalentClass_ = 0;  // another player: his class's tree is read again when its tab shows
        std::vector<game::TalentTabEntry>().swap(inspectTabs_);
        std::vector<game::TalentEntry>().swap(inspectTalentList_);
    }
    ++inspect_.version;
    if (++inspectLogs_ <= 40) PSP_LOGI(tag::Net, "inspect_send guid=%llx", static_cast<unsigned long long>(guid));
}

// ClearInspectPlayer: the inspect's data stays (upstream's ClearInspectPlayer does nothing). PSP: the inspected
// class's talent tree is let go (memory); it is read again when the talent tab shows.
void WorldSession::clearInspectPlayer() {
    inspectTalentClass_ = 0;
    std::vector<game::TalentTabEntry>().swap(inspectTabs_);
    std::vector<game::TalentEntry>().swap(inspectTalentList_);
}

// InspectTalentFrame's data: TalentTab.dbc / Talent.dbc rows of a class from spells.pak "talents" (the format of
// loadTalentDbc, psp_talent_format.hpp), tabs by OrderIndex. Kept until the inspect is cleared.
bool WorldSession::loadInspectTalents(uint8_t classId) {
    if (classId == 0) return false;
    if (classId == inspectTalentClass_) return !inspectTabs_.empty();
    inspectTalentClass_ = classId;
    inspectTabs_.clear();
    inspectTalentList_.clear();
    std::vector<uint8_t> raw;
    if (!spellDb_ || !spellDb_->loaded() || !spellDb_->hasTalents() || !spellDb_->readTalents(raw) ||
        raw.size() < sizeof(TalentPakHeader))
        return false;
    TalentPakHeader h;
    std::memcpy(&h, raw.data(), sizeof(h));
    const size_t tabBytes = static_cast<size_t>(h.tabCount) * sizeof(TalentTabRec);
    const size_t talBytes = static_cast<size_t>(h.talentCount) * sizeof(TalentRec);
    if (std::memcmp(h.magic, "WTAL", 4) != 0 || h.version != 1 || h.tabRecSize != sizeof(TalentTabRec) ||
        h.talentRecSize != sizeof(TalentRec) || raw.size() < sizeof(h) + tabBytes + talBytes + 1)
        return false;
    const uint8_t* tabsP = raw.data() + sizeof(h);
    const uint8_t* talsP = tabsP + tabBytes;
    const char* strings = reinterpret_cast<const char*>(talsP + talBytes);
    const size_t strBytes = raw.size() - 1 - sizeof(h) - tabBytes - talBytes;
    auto str = [&](uint32_t off) { return off < strBytes ? std::string(strings + off) : std::string(); };
    const uint32_t classMask = 1u << (classId - 1);
    for (uint32_t i = 0; i < h.tabCount; ++i) {
        TalentTabRec r;
        std::memcpy(&r, tabsP + i * sizeof(TalentTabRec), sizeof(r));
        if (r.id == 0 || !(r.classMask & classMask) || r.petMask) continue;
        game::TalentTabEntry e;
        e.tabId = r.id;
        e.name = str(r.nameOff);
        e.classMask = r.classMask;
        e.orderIndex = r.orderIndex;
        e.backgroundFile = str(r.bgOff);
        inspectTabs_.push_back(std::move(e));
    }
    std::sort(inspectTabs_.begin(), inspectTabs_.end(),
              [](const auto& a, const auto& b) { return a.orderIndex < b.orderIndex; });
    for (uint32_t i = 0; i < h.talentCount; ++i) {
        TalentRec r;
        std::memcpy(&r, talsP + i * sizeof(TalentRec), sizeof(r));
        if (r.id == 0) continue;
        bool ours = false;
        for (const auto& t : inspectTabs_) ours = ours || t.tabId == r.tabId;
        if (!ours) continue;
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
        inspectTalentList_.push_back(e);
    }
    std::vector<uint32_t> ids;  // the buttons' names / icons: their first ranks, one read
    for (const auto& t : inspectTalentList_) ids.push_back(t.rankSpells[0]);
    if (!ids.empty()) spellDb_->prefetch(ids);
    PSP_LOGI(tag::Fs, "inspect_talents_load class=%u tabs=%u talents=%u", static_cast<unsigned>(classId),
             static_cast<unsigned>(inspectTabs_.size()), static_cast<unsigned>(inspectTalentList_.size()));
    return !inspectTabs_.empty();
}

const game::TalentEntry* WorldSession::inspectTalentEntry(uint32_t talentId) const {
    for (const auto& t : inspectTalentList_)
        if (t.talentId == talentId) return &t;
    return nullptr;
}

// RequestInspectHonorData: MSG_INSPECT_HONOR_STATS (upstream requestInspectHonorData) and MSG_INSPECT_ARENA_TEAMS,
// each a uint64 guid (AzerothCore HandleInspectHonorStatsOpcode / HandleInspectArenaTeamsOpcode). Sent on every call:
// the frame calls it while HasInspectHonorData is false (InspectPVPFrame_OnShow).
void WorldSession::requestInspectHonorData() {
    if (state_ != State::InWorld || !socket_ || inspect_.guid == 0) return;
    for (game::Opcode op : {game::Opcode::MSG_INSPECT_HONOR_STATS, game::Opcode::MSG_INSPECT_ARENA_TEAMS}) {
        const uint16_t wire = game::wireOpcode(op);
        if (wire == 0xFFFF) continue;
        network::Packet packet(wire);
        packet.writeUInt64(inspect_.guid);
        socket_->send(packet);
    }
    if (++inspectLogs_ <= 40)
        PSP_LOGI(tag::Net, "inspect_honor_send guid=%llx", static_cast<unsigned long long>(inspect_.guid));
}

// The item tooltip of an inspected slot (kInspectItemGuid + slot): the inspect's enchantments when its items came,
// else what the visible item field shows (PLAYER_VISIBLE_ITEM_n_ENCHANTMENT: permanent | temporary << 16).
bool WorldSession::inspectEnchantments(uint64_t guid, uint32_t& permanent, uint32_t& temporary,
                                       std::array<uint32_t, 3>& sockets) const {
    if ((guid & 0xFFFF000000000000ull) != kInspectItemGuid) return false;
    const uint32_t slot = static_cast<uint32_t>(guid & 0xFFu);
    permanent = temporary = 0;
    sockets = {};
    if (slot >= 19) return true;
    if (inspect_.gearKnown && inspect_.slots[slot].entry != 0) {
        const InspectSlot& s = inspect_.slots[slot];
        permanent = s.enchant[0];
        temporary = s.enchant[1];
        for (int i = 0; i < 3; ++i) sockets[static_cast<size_t>(i)] = s.enchant[2 + i];
        return true;
    }
    if (auto e = entities_.getEntity(inspect_.guid)) {
        const uint32_t v = e->getField(static_cast<uint16_t>(kVisibleItemBase + slot * 2 + 1));
        permanent = v & 0xFFFFu;
        temporary = v >> 16;
    }
    return true;
}

bool WorldSession::handleInspectPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_INSPECT_TALENT: {
            if (!packet.hasFullPackedGuid()) return true;
            const uint64_t guid = packet.readPackedGuid();
            if (guid == 0 || guid != inspect_.guid) {
                packet.skipAll();
                return true;
            }
            // The talents and the items: psp_inspect_parse.hpp, the host test's code too.
            InspectResult& r = inspect_;
            InspectTalentRead read;
            const bool ok = readInspectTalent(packet, read);
            r.unspentTalents = read.unspent;
            r.talentGroups = read.groups;
            r.activeGroup = read.active;
            r.talentRanks = std::move(read.ranks);
            r.talentPoints = read.points;
            r.talentsKnown = read.talentsOk;
            if (read.gearOk) {
                r.slots = read.slots;
                r.gearKnown = true;
            }
            const int items = read.items;
            packet.skipAll();
            ++r.version;
            if (++inspectLogs_ <= 40)
                PSP_LOGI(tag::Net, "inspect_talent guid=%llx ok=%d talents=%u points=%u unspent=%u groups=%u items=%d",
                         static_cast<unsigned long long>(guid), ok ? 1 : 0, static_cast<unsigned>(r.talentRanks.size()),
                         static_cast<unsigned>(r.talentPoints), static_cast<unsigned>(r.unspentTalents),
                         static_cast<unsigned>(r.talentGroups), items);
            return true;
        }
        case LogicalOpcode::MSG_INSPECT_HONOR_STATS: {
            if (!packet.hasRemaining(8)) return true;
            const uint64_t guid = packet.readUInt64();
            if (guid == 0 || guid != inspect_.guid || !packet.hasRemaining(1 + 2 + 2 + 12)) {
                packet.skipAll();
                return true;
            }
            InspectResult& r = inspect_;
            r.lifetimeRank = packet.readUInt8();
            r.todayKills = packet.readUInt16();
            r.yesterdayKills = packet.readUInt16();
            r.todayHonor = packet.readUInt32();
            r.yesterdayHonor = packet.readUInt32();
            r.lifetimeKills = packet.readUInt32();
            r.honorKnown = true;
            ++r.version;
            packet.skipAll();
            if (++inspectLogs_ <= 40)
                PSP_LOGI(tag::Net, "inspect_honor guid=%llx today=%u/%u yesterday=%u/%u lifetime=%u",
                         static_cast<unsigned long long>(guid), static_cast<unsigned>(r.todayKills),
                         static_cast<unsigned>(r.todayHonor), static_cast<unsigned>(r.yesterdayKills),
                         static_cast<unsigned>(r.yesterdayHonor), static_cast<unsigned>(r.lifetimeKills));
            return true;
        }
        case LogicalOpcode::MSG_INSPECT_ARENA_TEAMS: {
            if (!packet.hasRemaining(9)) return true;
            const uint64_t guid = packet.readUInt64();
            const uint8_t slot = packet.readUInt8();
            if (guid == 0 || guid != inspect_.guid || slot > 2 || !packet.hasRemaining(24)) {
                packet.skipAll();
                return true;
            }
            InspectArenaTeam& t = inspect_.arena[slot];
            const uint32_t teamId = packet.readUInt32();
            if (teamId != t.teamId) t = InspectArenaTeam{};
            t.teamId = teamId;
            t.rating = packet.readUInt32();
            t.games = packet.readUInt32();
            t.wins = packet.readUInt32();
            t.played = packet.readUInt32();
            t.personalRating = packet.readUInt32();
            packet.skipAll();
            if (teamId != 0 && !t.infoKnown && socket_) {  // an unknown team: its name and colours asked
                const uint16_t wire = game::wireOpcode(game::Opcode::CMSG_ARENA_TEAM_QUERY);
                if (wire != 0xFFFF) {
                    network::Packet q(wire);
                    q.writeUInt32(teamId);
                    socket_->send(q);
                }
            }
            ++inspect_.version;
            if (++inspectLogs_ <= 40)
                PSP_LOGI(tag::Net, "inspect_arena guid=%llx slot=%u team=%u rating=%u games=%u wins=%u",
                         static_cast<unsigned long long>(guid), static_cast<unsigned>(slot),
                         static_cast<unsigned>(teamId), static_cast<unsigned>(t.rating), static_cast<unsigned>(t.games),
                         static_cast<unsigned>(t.wins));
            return true;
        }
        case LogicalOpcode::SMSG_ARENA_TEAM_QUERY_RESPONSE: {  // ArenaTeam::Query: only the inspected player's teams
            if (!packet.hasRemaining(4)) return false;
            const size_t start = packet.getReadPos();
            const uint32_t teamId = packet.readUInt32();
            InspectArenaTeam* t = nullptr;
            for (auto& a : inspect_.arena)
                if (teamId != 0 && a.teamId == teamId) t = &a;
            if (!t) {
                packet.setReadPos(start);
                return false;  // somebody else's question (none on the PSP yet)
            }
            t->name = packet.readString();
            if (packet.hasRemaining(24)) {
                t->type = packet.readUInt32();
                t->background = packet.readUInt32();
                t->emblem = packet.readUInt32();
                t->emblemColor = packet.readUInt32();
                t->border = packet.readUInt32();
                t->borderColor = packet.readUInt32();
            }
            t->infoKnown = true;
            ++inspect_.version;
            packet.skipAll();
            return true;
        }
        default: return false;
    }
}

}  // namespace wowee::platform::psp
