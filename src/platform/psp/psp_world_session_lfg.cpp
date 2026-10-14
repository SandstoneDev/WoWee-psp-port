// the Dungeon Finder's session half - upstream SocialHandler's LFG handlers and
// senders (src/game/social_handler.cpp handleLfg* / lfg*) without GameHandler, and the "lfg" entry of data/wotlk.pak
// (LFGDungeons.dbc / LFGDungeonGroup.dbc, psp_wotlk_db.hpp) that upstream reads from its DBC cache.
//
// What is upstream's, unchanged in what it reads and keeps: SMSG_LFG_JOIN_RESULT, _PLAYER_INFO (the random
// dungeons' rewards and the locks), _QUEUE_STATUS, _PROPOSAL_UPDATE (the 15-byte header and the members),
// _ROLE_CHECK_UPDATE (the dungeons, the member count), _UPDATE_PLAYER / _UPDATE_PARTY (the state from the body's
// flags, not from the update type), _PLAYER_REWARD, _BOOT_PROPOSAL_UPDATE, _TELEPORT_DENIED, _OFFER_CONTINUE,
// _ROLE_CHOSEN, SMSG_OPEN_LFG_DUNGEON_FINDER and the senders (CMSG_LFG_JOIN through LfgJoinPacket, _LEAVE,
// _SET_ROLES, _PROPOSAL_RESULT, _SET_BOOT_VOTE, _TELEPORT, CMSG_LFD_PLAYER/PARTY_LOCK_INFO_REQUEST); the events
// it fires (LFG_UPDATE ...) are queued for the world screen (takeLfgEvent).
//
// PSP differences (marked PSP: below):
//  - The lines are GlobalStrings ERR_LFG_* / ERR_PARTY_LFG_* / LFG_ROLE_CHECK_ROLE_CHOSEN, mapped from AzerothCore's
//    LfgJoinResult / LfgRoleCheckState / LfgUpdateType / LfgTeleportError by meaning, shown by the port's rule
//    (GameLine): a refused join red; the queue, role check, proposal and vote news in the chat as upstream's
//    lfgUpdateMessage / role check / boot lines; a refused teleport in the chat as upstream's
//    handleLfgTeleportDenied.
//    Upstream's English lines ("Dungeon Finder: ...") are not written.
//  - LfgUpdateType: AzerothCore's / TrinityCore's numbers (5 JOIN_QUEUE, 6 ROLECHECK_FAILED, 7 REMOVED_FROM_QUEUE,
//    8 PROPOSAL_FAILED, 9 PROPOSAL_DECLINED, 10 GROUP_FOUND, 12 ADDED_TO_QUEUE, 13 PROPOSAL_BEGIN, 14 UPDATE_STATUS,
//    15 GROUP_MEMBER_OFFLINE, 16 GROUP_DISBAND); upstream's table is one higher from 6 on. Only the wording and the
//    proposal state depend on them - the queue state comes from the body, as upstream.
//  - SMSG_LFG_PARTY_INFO is read (upstream skips it): the other members' locks for the lock tooltip.
//  - SMSG_LFG_COMMAND_RESULT of the calendar and the party half are elsewhere; IsPartyLFG is the group type's LFG
//    bit (SMSG_GROUP_LIST, the client's own test), not upstream's state.
//  - The roles are kept in wowee.cfg lfgroles (GetLFGRoles survives a relog in the client).
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_fs.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_strings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace wowee::platform::psp {

namespace {

// 3.3.5 LfgUpdateType (AzerothCore LFG.h, TrinityCore LFG.h).
enum : uint8_t {
    kUpdDefault = 0,
    kUpdLeaderUnk1 = 1,
    kUpdLeaveRaidBrowser = 2,
    kUpdJoinRaidBrowser = 3,
    kUpdRoleCheckAborted = 4,
    kUpdJoinQueue = 5,
    kUpdRoleCheckFailed = 6,
    kUpdRemovedFromQueue = 7,
    kUpdProposalFailed = 8,
    kUpdProposalDeclined = 9,
    kUpdGroupFound = 10,
    kUpdAddedToQueue = 12,
    kUpdProposalBegin = 13,
    kUpdUpdateStatus = 14,
    kUpdGroupMemberOffline = 15,
    kUpdGroupDisband = 16,
};

// 3.3.5 LfgProposalState.
enum : uint32_t { kProposalInitiating = 0, kProposalFailed = 1, kProposalSuccess = 2 };

// SMSG_LFG_JOIN_RESULT (AzerothCore LfgJoinResult) -> the refusal's line. 1 / 18 (FAILED: the role check) are
// told by the role check update; 3 makes no client reaction (AzerothCore's comment).
const char* joinError(uint32_t result) {
    switch (result) {
        case 2: return CLIENT_STR(ERR_LFG_GROUP_FULL);
        case 4: return CLIENT_STR(ERR_LFG_NO_LFG_OBJECT);
        case 5: return CLIENT_STR(ERR_LFG_NO_SLOTS_PLAYER);
        case 6: return CLIENT_STR(ERR_LFG_NO_SLOTS_PARTY);
        case 7: return CLIENT_STR(ERR_LFG_MISMATCHED_SLOTS);
        case 8: return CLIENT_STR(ERR_LFG_PARTY_PLAYERS_FROM_DIFFERENT_REALMS);
        case 9: return CLIENT_STR(ERR_LFG_MEMBERS_NOT_PRESENT);
        case 10: return CLIENT_STR(ERR_LFG_GET_INFO_TIMEOUT);
        case 11: return CLIENT_STR(ERR_LFG_INVALID_SLOT);
        case 12: return CLIENT_STR(ERR_LFG_DESERTER_PLAYER);
        case 13: return CLIENT_STR(ERR_LFG_DESERTER_PARTY);
        case 14: return CLIENT_STR(ERR_LFG_RANDOM_COOLDOWN_PLAYER);
        case 15: return CLIENT_STR(ERR_LFG_RANDOM_COOLDOWN_PARTY);
        case 16: return CLIENT_STR(ERR_LFG_TOO_MANY_MEMBERS);
        case 17: return CLIENT_STR(ERR_LFG_CANT_USE_DUNGEONS);
        default: return nullptr;
    }
}

// SMSG_LFG_TELEPORT_DENIED (AzerothCore LfgTeleportError) -> its line. PSP: by meaning.
const char* teleportError(uint32_t reason) {
    switch (reason) {
        case 1: return CLIENT_STR(ERR_PLAYER_DEAD);
        case 6: return CLIENT_STR(ERR_INVALID_TELEPORT_LOCATION);
        case 8: return CLIENT_STR(ERR_PARTY_LFG_TELEPORT_IN_COMBAT);
        case 5: return nullptr;  // "no client reaction"
        default: return CLIENT_STR(ERR_CLIENT_LOCKED_OUT);
    }
}

// "%s" with one argument; |3-N(x) (the ruRU declension of a name) -> x, as the PSP chat writes names.
std::string fmtName(const char* f, const std::string& name) {
    std::string out;
    for (const char* p = f; *p; ++p) {
        if (p[0] == '%' && p[1] == 's') {
            out += name;
            ++p;
        } else {
            out.push_back(*p);
        }
    }
    for (size_t at; (at = out.find("|3-")) != std::string::npos;) {
        const size_t open = out.find('(', at), close = out.find(')', at);
        if (open == std::string::npos || close == std::string::npos || close < open) break;
        out = out.substr(0, at) + out.substr(open + 1, close - open - 1) + out.substr(close + 1);
    }
    return out;
}

}  // namespace

// --- data/wotlk.pak -----------------------------------------------------------------------------------------------

bool wotlkPakOpen(Pak& pak) {
    char path[256];
    if (!fsPath(path, sizeof(path), "data/wotlk.pak")) return false;
    return pak.open(path);
}

void* wotlkPakRaw(Pak& pak, const char* name, uint32_t& size) {
    size = 0;
    const PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (blob) size = e->size;
    return blob;
}

bool wotlkPakTexture(Pak& pak, const char* name, Texture& out, uint32_t* bytes) {
    if (out.pixels) textureRelease(out);
    const PakEntry* e = pak.find(name);
    void* blob = e ? pak.load(e) : nullptr;
    if (!blob) return false;
    if (!textureFromBlob(blob, e->size, out, true)) {
        pakFree(blob);
        return false;
    }
    if (bytes) *bytes += e->size;
    return true;
}

bool LfgDb::load() {
    release();
    tried_ = true;
    Pak pak;
    if (!wotlkPakOpen(pak)) return false;
    uint32_t size = 0;
    void* blob = wotlkPakRaw(pak, "lfg", size);
    pak.close();
    if (!blob) return false;
    const auto* h = static_cast<const LfgPakHeader*>(blob);
    if (size < sizeof(LfgPakHeader) || std::memcmp(h->magic, "WLFG", 4) != 0 || h->version != 1 ||
        h->dungeonOff + h->dungeonCount * sizeof(LfgDungeonRec) > size ||
        h->groupOff + h->groupCount * sizeof(LfgGroupRec) > size || h->strOff + h->strSize > size || h->strSize == 0 ||
        static_cast<const char*>(blob)[h->strOff + h->strSize - 1] != '\0') {
        PSP_LOGW(tag::Net, "wotlk_lfg_bad size=%u", static_cast<unsigned>(size));
        pakFree(blob);
        return false;
    }
    blob_ = blob;
    size_ = size;
    hdr_ = h;
    const char* b = static_cast<const char*>(blob);
    dungeons_ = reinterpret_cast<const LfgDungeonRec*>(b + h->dungeonOff);
    groups_ = reinterpret_cast<const LfgGroupRec*>(b + h->groupOff);
    strings_ = b + h->strOff;
    strSize_ = h->strSize;
    PSP_LOGI(tag::Net, "wotlk_lfg dungeons=%u groups=%u kb=%u", static_cast<unsigned>(h->dungeonCount),
             static_cast<unsigned>(h->groupCount), static_cast<unsigned>((size + 1023) / 1024));
    return true;
}

void LfgDb::release() {
    if (blob_) pakFree(blob_);
    blob_ = nullptr;
    size_ = 0;
    hdr_ = nullptr;
    dungeons_ = nullptr;
    groups_ = nullptr;
    strings_ = nullptr;
    strSize_ = 0;
}

const LfgDungeonRec* LfgDb::dungeon(uint32_t id) const {
    if (!hdr_) return nullptr;
    const LfgDungeonRec* end = dungeons_ + hdr_->dungeonCount;
    const LfgDungeonRec* it =
        std::lower_bound(dungeons_, end, id, [](const LfgDungeonRec& r, uint32_t v) { return r.id < v; });
    return it != end && it->id == id ? it : nullptr;
}

const LfgGroupRec* LfgDb::group(uint32_t id) const {
    for (uint32_t i = 0; i < groupCount(); ++i)
        if (groups_[i].id == id) return &groups_[i];
    return nullptr;
}

const char* LfgDb::name(uint32_t dungeonId) const {
    const LfgDungeonRec* d = dungeon(dungeonId);
    return d ? str(d->name) : "";
}

LfgDb& WorldSession::lfgDb() {
    if (!lfgDb_.loaded() && !lfgDb_.tried()) lfgDb_.load();
    return lfgDb_;
}

void WorldSession::lfgReleaseDb() {
    if (lfgDb_.loaded()) PSP_LOGI(tag::Net, "wotlk_lfg_free kb=%u", static_cast<unsigned>((lfgDb_.bytes() + 1023) / 1024));
    lfgDb_.forget();  // a later lfgDb() reads it again
}

std::string WorldSession::lfgDungeonName(uint32_t dungeonId) { return lfgDb().name(dungeonId & 0x00FFFFFFu); }

// --- Events / lines -------------------------------------------------------------------------------------------------

void WorldSession::lfgPush(LfgEvent::Kind kind, uint32_t arg) {
    if (lfgEvents_.size() >= 32) lfgEvents_.pop_front();
    LfgEvent e;
    e.kind = kind;
    e.arg = arg;
    lfgEvents_.push_back(e);
}

bool WorldSession::takeLfgEvent(LfgEvent& out) {
    if (lfgEvents_.empty()) return false;
    out = lfgEvents_.front();
    lfgEvents_.pop_front();
    return true;
}

void WorldSession::lfgLine(const std::string& text, GameLine kind) {
    if (text.empty()) return;
    gameLine(kind, text);
}

bool WorldSession::lfgProposalAnswered() const {
    for (const auto& m : lfgProposalMembers_)
        if (m.isSelf) return m.answered;
    return false;
}

std::string WorldSession::lfgBootTargetName() const {
    if (lfgBoot_.victim == 0) return {};
    if (const std::string* n = nameOf(lfgBoot_.victim)) return *n;
    return {};
}

// --- Packets ---------------------------------------------------------------------------------------------------------

bool WorldSession::handleLfgPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    using game::LfgState;
    switch (op) {
        case LogicalOpcode::SMSG_LFG_JOIN_RESULT: {  // upstream handleLfgJoinResult: two uint32s
            ++lfgVersion_;
            if (!packet.hasRemaining(8)) return true;
            const uint32_t result = packet.readUInt32();
            const uint32_t state = packet.readUInt32();
            packet.skipAll();  // PSP: the lock block of a party refusal is not kept
            if (result == 0) {
                lfgState_ = static_cast<LfgState>(state);
            } else {
                if (const char* e = joinError(result)) lfgLine(e, GameLine::Refused);
            }
            if (++lfgLogs_ <= 60)
                PSP_LOGI(tag::Net, "lfg_join_result result=%u state=%u", static_cast<unsigned>(result),
                         static_cast<unsigned>(state));
            lfgPush(LfgEvent::Update);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_PLAYER_INFO: {  // upstream handleLfgPlayerInfo
            ++lfgVersion_;
            if (!packet.hasRemaining(1)) return true;
            const uint8_t randomCount = packet.readUInt8();
            lfgRewards_.clear();
            for (uint8_t i = 0; i < randomCount; ++i) {
                if (!packet.hasRemaining(4 + 1 + 4 * 4 + 1)) {
                    packet.skipAll();
                    return true;
                }
                game::LfgReward r;
                r.dungeonId = packet.readUInt32() & 0x00FFFFFFu;
                r.done = packet.readUInt8() != 0;
                r.money = packet.readUInt32();
                r.experience = packet.readUInt32();
                packet.readUInt32();
                packet.readUInt32();
                const uint8_t items = packet.readUInt8();
                for (uint8_t j = 0; j < items; ++j) {
                    if (!packet.hasRemaining(12)) {
                        packet.skipAll();
                        return true;
                    }
                    game::LfgRewardItem it;
                    it.itemId = packet.readUInt32();
                    it.displayInfoId = packet.readUInt32();
                    it.count = packet.readUInt32();
                    if (it.itemId) ensureItemInfo(it.itemId);  // PSP: the reward's name for the window
                    r.items.push_back(it);
                }
                lfgRewards_.push_back(std::move(r));
            }
            lfgLocks_.clear();
            if (packet.hasRemaining(4)) {
                const uint32_t lockCount = packet.readUInt32();
                for (uint32_t i = 0; i < lockCount && packet.hasRemaining(8); ++i) {
                    const uint32_t entry = packet.readUInt32();
                    lfgLocks_[entry & 0x00FFFFFFu] = packet.readUInt32();
                }
            }
            packet.skipAll();
            lfgPlayerInfoKnown_ = true;
            if (++lfgLogs_ <= 60)
                PSP_LOGI(tag::Net, "lfg_player_info random=%u locks=%u", static_cast<unsigned>(lfgRewards_.size()),
                         static_cast<unsigned>(lfgLocks_.size()));
            lfgPush(LfgEvent::RandomInfo);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_PARTY_INFO: {  // PSP: read (upstream skips it) - the members' locks
            ++lfgVersion_;
            lfgPartyLocks_.clear();
            if (!packet.hasRemaining(1)) return true;
            const uint8_t members = packet.readUInt8();
            for (uint8_t m = 0; m < members && packet.hasRemaining(12); ++m) {
                LfgPartyLock pl;
                pl.guid = packet.readUInt64();
                const uint32_t n = packet.readUInt32();
                for (uint32_t i = 0; i < n && packet.hasRemaining(8); ++i) {
                    const uint32_t entry = packet.readUInt32();
                    pl.locks[entry & 0x00FFFFFFu] = packet.readUInt32();
                }
                lfgPartyLocks_.push_back(std::move(pl));
            }
            packet.skipAll();
            lfgPush(LfgEvent::RandomInfo);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_QUEUE_STATUS: {  // upstream handleLfgQueueStatus: 31 bytes
            ++lfgVersion_;
            if (!packet.hasRemaining(31)) return true;
            LfgQueueStats& q = lfgQueue_;
            q.dungeonId = packet.readUInt32() & 0x00FFFFFFu;
            lfgDungeonId_ = q.dungeonId;
            q.avgWait = static_cast<int32_t>(packet.readUInt32());
            q.myWait = static_cast<int32_t>(packet.readUInt32());
            q.tankWait = static_cast<int32_t>(packet.readUInt32());
            q.healerWait = static_cast<int32_t>(packet.readUInt32());
            q.dpsWait = static_cast<int32_t>(packet.readUInt32());
            q.needTank = packet.readUInt8();
            q.needHealer = packet.readUInt8();
            q.needDps = packet.readUInt8();
            q.queuedSec = packet.readUInt32();
            q.receivedMs = logUptimeMs();
            q.known = true;
            lfgState_ = LfgState::Queued;
            lfgPush(LfgEvent::QueueStatus);
            lfgPush(LfgEvent::Update);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_PROPOSAL_UPDATE: {  // upstream handleLfgProposalUpdate
            ++lfgVersion_;
            if (!packet.hasRemaining(15)) return true;
            const uint32_t dungeonEntry = packet.readUInt32();
            const uint32_t proposalState = packet.readUInt8();
            const uint32_t proposalId = packet.readUInt32();
            const uint32_t encounters = packet.readUInt32();
            const bool silent = packet.readUInt8() != 0;
            const uint8_t groupSize = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            lfgProposalMembers_.clear();
            for (uint8_t i = 0; i < groupSize && packet.hasRemaining(9); ++i) {
                game::LfgProposalMember m;
                m.role = packet.readUInt32();
                m.isSelf = packet.readUInt8() != 0;
                m.inDungeon = packet.readUInt8() != 0;
                m.sameGroup = packet.readUInt8() != 0;
                m.answered = packet.readUInt8() != 0;
                m.accepted = packet.readUInt8() != 0;
                lfgProposalMembers_.push_back(m);
            }
            packet.skipAll();
            lfgDungeonId_ = dungeonEntry & 0x00FFFFFFu;
            lfgProposalEncounters_ = encounters;
            lfgProposalId_ = proposalId;
            switch (proposalState) {
                case kProposalInitiating: lfgState_ = LfgState::Proposal; break;
                case kProposalFailed:
                    lfgState_ = LfgState::Queued;
                    lfgProposalId_ = 0;
                    break;
                case kProposalSuccess:
                    lfgState_ = LfgState::InDungeon;
                    lfgProposalId_ = 0;
                    break;
                default: break;
            }
            if (++lfgLogs_ <= 60)
                PSP_LOGI(tag::Net, "lfg_proposal id=%u state=%u dungeon=%u members=%u silent=%d",
                         static_cast<unsigned>(proposalId), static_cast<unsigned>(proposalState),
                         static_cast<unsigned>(lfgDungeonId_), static_cast<unsigned>(groupSize), silent ? 1 : 0);
            lfgPush(LfgEvent::ProposalUpdate);
            // upstream: SHOW once per proposal (not when silent), FAILED / SUCCEEDED close the dialog.
            if (proposalState == kProposalInitiating) {
                if (!silent && lfgShownProposal_ != proposalId) {
                    lfgShownProposal_ = proposalId;
                    lfgPush(LfgEvent::ProposalShow);
                }
            } else if (proposalState == kProposalFailed) {
                lfgShownProposal_ = 0;
                lfgPush(LfgEvent::ProposalFailed);
            } else if (proposalState == kProposalSuccess) {
                lfgShownProposal_ = 0;
                lfgPush(LfgEvent::ProposalSucceeded);
            }
            return true;
        }
        case LogicalOpcode::SMSG_LFG_ROLE_CHECK_UPDATE: {  // upstream handleLfgRoleCheckUpdate
            ++lfgVersion_;
            if (!packet.hasRemaining(6)) return true;
            const uint32_t roleCheckState = packet.readUInt32();
            const bool initiating = packet.readUInt8() != 0;
            lfgRoleCheckDungeons_.clear();
            const uint8_t dungeonCount = packet.readUInt8();
            for (uint8_t i = 0; i < dungeonCount && packet.hasRemaining(4); ++i) {
                const uint32_t entry = packet.readUInt32();
                game::LfgRoleCheckDungeon d;
                d.dungeonId = entry & 0x00FFFFFFu;
                d.typeId = static_cast<uint8_t>((entry >> 24) & 0xFFu);
                lfgRoleCheckDungeons_.push_back(d);
            }
            // PSP: the members' roles too (whose role is missing: ERR_LFG_PLAYER_DECLINED_ROLE_CHECK).
            lfgRoleCheckRoles_.clear();
            const uint8_t members = packet.hasRemaining(1) ? packet.readUInt8() : 0;
            for (uint8_t i = 0; i < members && packet.hasRemaining(14); ++i) {
                const uint64_t guid = packet.readUInt64();
                packet.readUInt8();  // ready
                const uint32_t roles = packet.readUInt32();
                packet.readUInt8();  // level
                lfgRoleCheckRoles_.emplace_back(guid, roles);
            }
            packet.skipAll();
            (void)initiating;  // AzerothCore: (state == LFG_ROLECHECK_INITIALITING) - every update of a running check
            const LfgState before = lfgState_;
            // upstream: 1 finished -> queued, 2 beginning -> role check, anything else ends it (state none).
            switch (roleCheckState) {
                case 1: lfgState_ = LfgState::Queued; break;
                case 2:
                    lfgState_ = LfgState::RoleCheck;
                    if (before != LfgState::RoleCheck)  // PSP: once, at the start
                        lfgLine(CLIENT_STR(ERR_LFG_ROLE_CHECK_INITIATED), GameLine::News);
                    break;
                case 3:
                    lfgState_ = LfgState::None;
                    lfgLine(CLIENT_STR(ERR_LFG_ROLE_CHECK_FAILED_TIMEOUT), GameLine::News);
                    break;
                case 4:
                    lfgState_ = LfgState::None;
                    lfgLine(CLIENT_STR(ERR_LFG_ROLE_CHECK_FAILED_NOT_VIABLE), GameLine::News);
                    break;
                case 5:
                    lfgState_ = LfgState::None;
                    lfgLine(CLIENT_STR(ERR_LFG_ROLE_CHECK_ABORTED), GameLine::News);
                    break;
                case 6: {
                    lfgState_ = LfgState::None;
                    std::string who;
                    for (const auto& [guid, roles] : lfgRoleCheckRoles_)
                        if (roles < 2u) {  // PLAYER_ROLE_TANK: anything below is no role
                            const std::string* n = nameOf(guid);
                            who = n ? *n : std::string(CLIENT_STR(UNKNOWN));
                            break;
                        }
                    lfgLine(who.empty() ? std::string(CLIENT_STR(ERR_LFG_ROLE_CHECK_FAILED))
                                        : fmtName(CLIENT_STR(ERR_LFG_PLAYER_DECLINED_ROLE_CHECK), who),
                            GameLine::News);
                    break;
                }
                default: break;
            }
            if (++lfgLogs_ <= 60)
                PSP_LOGI(tag::Net, "lfg_role_check state=%u dungeons=%u members=%u", static_cast<unsigned>(roleCheckState),
                         static_cast<unsigned>(dungeonCount), static_cast<unsigned>(members));
            // PSP: LFG_ROLE_CHECK_SHOW once, when the check begins and our row has no role yet (every member's answer
            // repeats state 2 - LFG_ROLE_CHECK_UPDATE only; the leader's roles come with the join); LFG_ROLE_CHECK_HIDE
            // when it ends.
            if (lfgState_ == LfgState::RoleCheck && before != LfgState::RoleCheck) {
                uint32_t own = 0;
                for (const auto& [guid, roles] : lfgRoleCheckRoles_)
                    if (guid == playerGuid_) own = roles;
                if (own == 0) lfgPush(LfgEvent::RoleCheckShow);
            } else if (lfgState_ != LfgState::RoleCheck && before == LfgState::RoleCheck) {
                lfgPush(LfgEvent::RoleCheckHide);
            }
            lfgPush(LfgEvent::Update);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_UPDATE_PLAYER: {  // upstream handleLfgUpdatePlayer
            ++lfgVersion_;
            if (!packet.hasRemaining(2)) return true;
            const uint8_t updateType = packet.readUInt8();
            const bool extraInfo = packet.readUInt8() != 0;
            bool queued = false;
            if (extraInfo && packet.hasRemaining(4)) {
                queued = packet.readUInt8() != 0;
                packet.readUInt8();
                packet.readUInt8();
                lfgReadDungeonList(packet);
            }
            packet.skipAll();
            lfgApplyUpdate(updateType, extraInfo, queued, false);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_UPDATE_PARTY: {  // upstream handleLfgUpdateParty
            ++lfgVersion_;
            if (!packet.hasRemaining(2)) return true;
            const uint8_t updateType = packet.readUInt8();
            const bool extraInfo = packet.readUInt8() != 0;
            bool queued = false, joined = false;
            if (extraInfo && packet.hasRemaining(8)) {
                joined = packet.readUInt8() != 0;
                queued = packet.readUInt8() != 0;
                for (int i = 0; i < 5; ++i) packet.readUInt8();
                lfgReadDungeonList(packet);
            }
            packet.skipAll();
            lfgApplyUpdate(updateType, extraInfo, queued, joined && !queued);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_PLAYER_REWARD: {  // upstream handleLfgPlayerReward
            ++lfgVersion_;
            if (!packet.hasRemaining(4 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 1)) return true;
            game::LfgCompletionReward r;
            r.randomDungeonId = packet.readUInt32() & 0x00FFFFFFu;
            r.dungeonId = packet.readUInt32() & 0x00FFFFFFu;
            r.done = packet.readUInt8() != 0;
            packet.readUInt32();
            r.money = packet.readUInt32();
            r.xp = packet.readUInt32();
            packet.readUInt32();
            packet.readUInt32();
            const uint8_t n = packet.readUInt8();
            for (uint8_t i = 0; i < n && packet.hasRemaining(12); ++i) {
                game::LfgCompletionReward::Item it;
                it.itemId = packet.readUInt32();
                it.displayId = packet.readUInt32();
                it.count = packet.readUInt32();
                if (it.itemId) ensureItemInfo(it.itemId);
                r.items.push_back(it);
            }
            packet.skipAll();
            r.valid = true;
            lfgCompletionReward_ = r;
            lfgState_ = LfgState::FinishedDungeon;
            PSP_LOGI(tag::Net, "lfg_reward random=%u dungeon=%u money=%u xp=%u items=%u",
                     static_cast<unsigned>(r.randomDungeonId), static_cast<unsigned>(r.dungeonId),
                     static_cast<unsigned>(r.money), static_cast<unsigned>(r.xp), static_cast<unsigned>(n));
            lfgPush(LfgEvent::CompletionReward);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_BOOT_PROPOSAL_UPDATE: {  // upstream handleLfgBootProposalUpdate
            ++lfgVersion_;
            if (!packet.hasRemaining(27)) return true;
            LfgBoot b;
            b.inProgress = packet.readUInt8() != 0;
            b.didVote = packet.readUInt8() != 0;
            b.myVote = packet.readUInt8() != 0;
            b.victim = packet.readUInt64();
            b.totalVotes = packet.readUInt32();
            b.bootVotes = packet.readUInt32();
            b.timeLeft = packet.readUInt32();
            b.votesNeeded = packet.readUInt32();
            if (packet.getReadPos() < packet.getSize()) b.reason = packet.readString();
            packet.skipAll();
            b.receivedMs = logUptimeMs();
            if (b.victim && !nameOf(b.victim) && nameQueried_.insert(b.victim).second && socket_)
                socket_->send(game::NameQueryPacket::build(b.victim));  // upstream queryPlayerName
            if (b.inProgress) {
                lfgState_ = LfgState::Boot;
            } else {
                // The vote ended: ERR_PARTY_LFG_BOOT_VOTE_SUCCEEDED / _FAILED (news, in the chat) with the victim's name.
                std::string who;
                if (const std::string* n = nameOf(b.victim)) who = *n;
                if (!who.empty() || lfgBoot_.inProgress)
                    lfgLine(fmtName(b.bootVotes >= b.votesNeeded ? CLIENT_STR(ERR_PARTY_LFG_BOOT_VOTE_SUCCEEDED)
                                                                 : CLIENT_STR(ERR_PARTY_LFG_BOOT_VOTE_FAILED),
                                    who.empty() ? std::string(CLIENT_STR(UNKNOWN)) : who),
                            GameLine::News);
                b = LfgBoot{};
                lfgState_ = LfgState::InDungeon;
            }
            lfgBoot_ = b;
            if (++lfgLogs_ <= 60)
                PSP_LOGI(tag::Net, "lfg_boot progress=%d voted=%d votes=%u/%u", lfgBoot_.inProgress ? 1 : 0,
                         lfgBoot_.didVote ? 1 : 0, static_cast<unsigned>(lfgBoot_.bootVotes),
                         static_cast<unsigned>(lfgBoot_.votesNeeded));
            lfgPush(LfgEvent::BootUpdate);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_TELEPORT_DENIED: {  // upstream handleLfgTeleportDenied: a uint32
            if (!packet.hasRemaining(4)) return true;
            const uint32_t reason = packet.readUInt32();
            packet.skipAll();
            if (const char* t = teleportError(reason)) lfgLine(t, GameLine::News);  // upstream: chat
            PSP_LOGI(tag::Net, "lfg_teleport_denied reason=%u", static_cast<unsigned>(reason));
            return true;
        }
        case LogicalOpcode::SMSG_LFG_OFFER_CONTINUE: {  // upstream: the dungeon entry (id + type)
            uint32_t dungeonId = 0;
            if (packet.hasRemaining(4)) dungeonId = packet.readUInt32() & 0x00FFFFFFu;
            packet.skipAll();
            lfgPush(LfgEvent::OfferContinue, dungeonId);
            return true;
        }
        case LogicalOpcode::SMSG_LFG_ROLE_CHOSEN: {  // upstream: guid, ready, roles -> LFG_ROLE_CHECK_ROLE_CHOSEN
            if (!packet.hasRemaining(13)) {
                packet.skipAll();
                return true;
            }
            const uint64_t guid = packet.readUInt64();
            const bool ready = packet.readUInt8() != 0;
            const uint32_t roles = packet.readUInt32();
            packet.skipAll();
            // LFGEventFrame_OnEvent: ChatFrame_DisplayUsageError(format(LFG_ROLE_CHECK_ROLE_CHOSEN, player, roleList))
            // - the roles joined by PLAYER_LIST_DELIMITER (PSP: without the role icons).
            if (ready) {
                std::string list;
                for (const auto& [bit, word] : {std::pair<uint32_t, const char*>{2u, CLIENT_STR(TANK)},
                                                {4u, CLIENT_STR(HEALER)}, {8u, CLIENT_STR(DAMAGER)}})
                    if (roles & bit) {
                        if (!list.empty()) list += ", ";
                        list += word;
                    }
                const std::string* n = nameOf(guid);
                addSystemChatMessage(
                    CLIENT_FMT(LFG_ROLE_CHECK_ROLE_CHOSEN, n ? *n : std::string(CLIENT_STR(UNKNOWN)), list));
            }
            return true;
        }
        case LogicalOpcode::SMSG_LFG_DISABLED:  // upstream: a chat line of its own (the original has none)
            packet.skipAll();
            PSP_LOGI(tag::Net, "lfg_disabled");
            return true;
        case LogicalOpcode::SMSG_OPEN_LFG_DUNGEON_FINDER:  // upstream openLfgCallback
            packet.skipAll();
            lfgPush(LfgEvent::OpenFinder);
            return true;
        case LogicalOpcode::SMSG_LFG_UPDATE_SEARCH:
        case LogicalOpcode::SMSG_UPDATE_LFG_LIST:  // the raid browser (not ported - upstream skips them too)
            packet.skipAll();
            return true;
        default: return false;
    }
}

void WorldSession::lfgReadDungeonList(network::Packet& packet) {  // upstream readLfgDungeonList (+ the whole list)
    lfgQueuedList_.clear();
    if (!packet.hasRemaining(1)) return;
    const uint8_t count = packet.readUInt8();
    for (uint8_t i = 0; i < count && packet.hasRemaining(4); ++i) {
        const uint32_t id = packet.readUInt32() & 0x00FFFFFFu;
        if (i == 0) lfgDungeonId_ = id;
        lfgQueuedList_.push_back(id);
    }
}

// upstream applyLfgUpdate - the state from the flags; PSP: the AzerothCore update type numbers and the original's
// lines (ERR_LFG_*), said only when the state moves, as upstream.
void WorldSession::lfgApplyUpdate(uint8_t updateType, bool extraInfo, bool queued, bool inDungeon) {
    using game::LfgState;
    const LfgState previous = lfgState_;
    if (updateType == kUpdGroupFound || updateType == kUpdProposalBegin) {
        lfgState_ = LfgState::Proposal;
    } else if (!extraInfo) {
        switch (updateType) {
            case kUpdDefault:
            case kUpdLeaderUnk1:
            case kUpdGroupMemberOffline: break;
            default:
                lfgState_ = LfgState::None;
                lfgQueuedList_.clear();
                break;
        }
    } else if (queued) {
        lfgState_ = LfgState::Queued;
    } else if (inDungeon) {
        lfgState_ = LfgState::InDungeon;
    }
    if (lfgState_ != LfgState::Queued && lfgState_ != LfgState::Proposal) lfgQueue_.known = false;
    const char* line = nullptr;
    switch (updateType) {
        case kUpdJoinQueue:
        case kUpdAddedToQueue: line = CLIENT_STR(ERR_LFG_JOINED_QUEUE); break;
        case kUpdRemovedFromQueue: line = CLIENT_STR(ERR_LFG_LEFT_QUEUE); break;
        case kUpdProposalFailed: line = CLIENT_STR(ERR_LFG_PROPOSAL_FAILED); break;
        case kUpdProposalDeclined: {
            // _SELF when we did not accept it, _PARTY when somebody of our group did not.
            bool selfAccepted = false;
            for (const auto& m : lfgProposalMembers_)
                if (m.isSelf) selfAccepted = m.answered && m.accepted;
            line = selfAccepted ? CLIENT_STR(ERR_LFG_PROPOSAL_DECLINED_PARTY) : CLIENT_STR(ERR_LFG_PROPOSAL_DECLINED_SELF);
            break;
        }
        case kUpdJoinRaidBrowser: line = CLIENT_STR(ERR_LFG_JOINED_LIST); break;
        case kUpdLeaveRaidBrowser: line = CLIENT_STR(ERR_LFG_LEFT_LIST); break;
        default: break;
    }
    if (line && lfgState_ != previous) lfgLine(line, GameLine::News);
    if (++lfgLogs_ <= 60)
        PSP_LOGI(tag::Net, "lfg_update type=%u extra=%d queued=%d dungeon=%u state=%u", static_cast<unsigned>(updateType),
                 extraInfo ? 1 : 0, queued ? 1 : 0, static_cast<unsigned>(lfgDungeonId_),
                 static_cast<unsigned>(lfgState_));
    lfgPush(LfgEvent::Update);
}

// --- Senders (upstream SocialHandler::lfg*) ---------------------------------------------------------------------------

uint8_t WorldSession::lfgRoles() {
    if (!lfgRolesLoaded_) {
        lfgRolesLoaded_ = true;
        lfgRolesValue_ = static_cast<uint8_t>(configGetInt("lfgroles", 0) & 0x0F);
    }
    return lfgRolesValue_;
}

void WorldSession::lfgSetRoles(uint8_t roles) {
    roles &= 0x0F;
    lfgRolesLoaded_ = true;
    if (roles != lfgRolesValue_) {
        lfgRolesValue_ = roles;
        char v[8];
        std::snprintf(v, sizeof(v), "%u", static_cast<unsigned>(roles));
        configSet("lfgroles", v);
        configSave();
    }
    ++lfgVersion_;
    // upstream lfgSetRoles: CMSG_LFG_SET_ROLES (the server only reads it in a group: the role check's answer).
    if (state_ != State::InWorld || !socket_ || !isInGroup()) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_LFG_SET_ROLES));
    pkt.writeUInt8(roles);
    socket_->send(pkt);
}

void WorldSession::lfgJoin(const std::vector<uint32_t>& dungeonIds) {
    if (state_ != State::InWorld || !socket_ || dungeonIds.empty()) return;
    if (game::wireOpcode(game::LogicalOpcode::CMSG_LFG_JOIN) == 0xFFFF) return;
    // upstream offeredRoles(); PSP: the leader bit as the role buttons keep it (GetLFGRoles' first value).
    const uint8_t roles = lfgRoles();
    std::vector<uint32_t> sorted = dungeonIds;
    std::sort(sorted.begin(), sorted.end());
    socket_->send(game::LfgJoinPacket::build(sorted, roles));
    PSP_LOGI(tag::Net, "lfg_join dungeons=%u first=%u roles=%u", static_cast<unsigned>(sorted.size()),
             static_cast<unsigned>(sorted.front()), static_cast<unsigned>(roles));
}

void WorldSession::lfgLeave() {
    if (!socket_) return;
    lfgState_ = game::LfgState::None;  // upstream: none at once
    ++lfgVersion_;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_LFG_LEAVE);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt32(0);
    pkt.writeUInt32(0);
    pkt.writeUInt32(0);
    socket_->send(pkt);
    lfgPush(LfgEvent::Update);
    PSP_LOGI(tag::Net, "lfg_leave");
}

void WorldSession::lfgAnswerProposal(bool accept) {
    if (!socket_ || lfgProposalId_ == 0) return;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_LFG_PROPOSAL_RESULT);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt32(lfgProposalId_);
    pkt.writeUInt8(accept ? 1 : 0);
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "lfg_proposal_answer id=%u accept=%d", static_cast<unsigned>(lfgProposalId_), accept ? 1 : 0);
}

void WorldSession::lfgCompleteRoleCheck(bool accept) {
    // CompleteLFGRoleCheck: the roles answer the check (CMSG_LFG_SET_ROLES); a decline answers with no role
    // (the server's LFG_ROLECHECK_NO_ROLE).
    if (state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_LFG_SET_ROLES));
    pkt.writeUInt8(accept ? lfgRoles() : 0);
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "lfg_role_check_answer accept=%d roles=%u", accept ? 1 : 0, static_cast<unsigned>(lfgRoles()));
}

void WorldSession::lfgSetBootVote(bool vote) {
    if (!socket_) return;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_LFG_SET_BOOT_VOTE);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt8(vote ? 1 : 0);
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "lfg_boot_vote vote=%d", vote ? 1 : 0);
}

void WorldSession::lfgTeleport(bool out) {
    if (!socket_) return;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_LFG_TELEPORT);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt8(out ? 1 : 0);  // LFGTeleport(out): the server reads "out"
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "lfg_teleport out=%d", out ? 1 : 0);
}

void WorldSession::lfgRequestLockInfo() {  // upstream requestLfgPlayerLockInfo / requestLfgPartyLockInfo
    if (state_ != State::InWorld || !socket_) return;
    network::Packet a(game::wireOpcode(game::LogicalOpcode::CMSG_LFD_PLAYER_LOCK_INFO_REQUEST));
    socket_->send(a);
    if (isInGroup()) {
        network::Packet b(game::wireOpcode(game::LogicalOpcode::CMSG_LFD_PARTY_LOCK_INFO_REQUEST));
        socket_->send(b);
    }
}

}  // namespace wowee::platform::psp
