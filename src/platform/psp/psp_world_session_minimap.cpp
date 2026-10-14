// the notices the minimap cluster shows (src/ui/psp/ui_world_minimap.cpp) -
// MiniMapMailFrame and GameTimeFrame of the original (Minimap.xml, GameTime.lua).
//
// GameHandler is not built for the PSP; these are its handlers, unchanged in what they read:
//  - SMSG_RECEIVED_MAIL / MSG_QUERY_NEXT_MAIL_TIME: upstream InventoryHandler::handleReceivedMail /
//    handleQueryNextMailTime / setHasNewMail - new mail = the answer's float >= 0 and its count > 0.
//    PSP: setHasNewMail writes no chat line and plays no sound - upstream's "You have new mail." and its
//    whisper sound are its own; the original only shows MiniMapMailFrame (UPDATE_PENDING_MAIL).
//    PSP: the answer's entries after the count (u64 player sender, u32 other sender, u32 type,
//    u32 stationery, f32 delay - AzerothCore / TrinityCore HandleQueryNextMailTime) are kept as
//    GetLatestThreeSenders (MinimapMailFrameUpdate: HAVE_MAIL_FROM + the names); upstream reads the
//    senders from the mailbox list, which is only filled at a mailbox. The query goes out once in world
//    and after each SMSG_RECEIVED_MAIL (upstream never sends it; on AzerothCore the login
//    SMSG_RECEIVED_MAIL alone lights the envelope but names nobody).
//  - SMSG_CALENDAR_SEND_NUM_PENDING (u32) / SMSG_CALENDAR_EVENT_INVITE_ALERT (+1): upstream
//    calendarPendingInvites_ (game_handler_packets.cpp) without its English chat lines (the original
//    only flashes GameTimeFrame). PSP: CMSG_CALENDAR_GET_NUM_PENDING once in world - upstream asks when its
//    calendar window opens; the PSP has no CalendarFrame, and GameTimeFrame reads the count from the
//    start (PLAYER_ENTERING_WORLD).
//  - CalendarGetDate: the realm's date from the packed time of SMSG_LOGIN_SETTIMESPEED /
//    SMSG_GAMETIME_SET / _UPDATE (unpackWowPackedTime), peeked - the packet goes on to handleTimePacket.
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_log.hpp"

namespace wowee::platform::psp {

namespace {
constexpr uint32_t kMailQueryGapMs = 2000;
constexpr uint32_t kMaxSenders = 3;  // GetLatestThreeSenders
}  // namespace

bool WorldSession::handleMinimapPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    switch (op) {
        case LogicalOpcode::SMSG_LOGIN_SETTIMESPEED:
        case LogicalOpcode::SMSG_GAMETIME_SET:
        case LogicalOpcode::SMSG_GAMETIME_UPDATE:
            if (packet.hasRemaining(4)) {
                const size_t pos = packet.getReadPos();
                realmDate_ = game::unpackWowPackedTime(packet.readUInt32());
                packet.setReadPos(pos);
                realmDateKnown_ = true;
                realmDateMs_ = logUptimeMs();
            }
            return false;  // handleTimePacket reads it
        case LogicalOpcode::SMSG_RECEIVED_MAIL:  // upstream handleReceivedMail (u32 / float, unread)
            packet.skipAll();
            setHasNewMail(true);
            queryNextMailTime();  // PSP: who it is from (see the file comment)
            return true;
        case LogicalOpcode::MSG_QUERY_NEXT_MAIL_TIME: {
            if (!packet.hasRemaining(8)) return true;  // upstream: needs 8 bytes
            const float nextTime = packet.readFloat();
            const uint32_t count = packet.readUInt32();
            mailSenderGuids_.clear();
            for (uint32_t i = 0; i < count && packet.hasRemaining(28); ++i) {
                const uint64_t guid = packet.readUInt64();
                (void)packet.readUInt32();  // non-player sender (auction house, creature...)
                (void)packet.readUInt32();  // messageType
                (void)packet.readUInt32();  // stationery
                (void)packet.readFloat();   // delivery delay
                if (guid == 0 || mailSenderGuids_.size() >= kMaxSenders) continue;
                mailSenderGuids_.push_back(guid);
                if (names_.find(guid) == names_.end() && nameQueried_.insert(guid).second && socket_)
                    socket_->send(game::NameQueryPacket::build(guid));
            }
            packet.skipAll();
            if (++minimapNoticeLogs_ <= 40)
                PSP_LOGI(tag::Net, "minimap_mail_time next=%.0f count=%u senders=%u", static_cast<double>(nextTime),
                         static_cast<unsigned>(count), static_cast<unsigned>(mailSenderGuids_.size()));
            setHasNewMail(nextTime >= 0.0f && count > 0);  // upstream handleQueryNextMailTime
            return true;
        }
        case LogicalOpcode::SMSG_CALENDAR_SEND_NUM_PENDING:
            if (packet.hasRemaining(4)) {
                calendarPendingInvites_ = packet.readUInt32();
                if (++minimapNoticeLogs_ <= 40)
                    PSP_LOGI(tag::Net, "minimap_calendar pending=%u", static_cast<unsigned>(calendarPendingInvites_));
            }
            packet.skipAll();
            return true;
        case LogicalOpcode::SMSG_CALENDAR_EVENT_INVITE_ALERT:
            packet.skipAll();
            if (calendarPendingInvites_ < 255) ++calendarPendingInvites_;  // upstream
            if (++minimapNoticeLogs_ <= 40)
                PSP_LOGI(tag::Net, "minimap_calendar invite pending=%u", static_cast<unsigned>(calendarPendingInvites_));
            return true;
        default: return false;
    }
}

void WorldSession::setHasNewMail(bool value) {
    if (value != hasNewMail_ && ++minimapNoticeLogs_ <= 40)
        PSP_LOGI(tag::Net, "minimap_mail new=%d", value ? 1 : 0);
    hasNewMail_ = value;
    if (!value) mailSenderGuids_.clear();
}

std::vector<std::string> WorldSession::latestMailSenders() const {
    std::vector<std::string> out;
    for (uint64_t guid : mailSenderGuids_) {
        auto it = names_.find(guid);
        if (it != names_.end() && !it->second.empty()) out.push_back(it->second);
    }
    return out;
}

void WorldSession::queryNextMailTime() {
    if (state_ != State::InWorld || !socket_) return;
    const uint32_t now = logUptimeMs();
    if (lastMailQueryMs_ != 0 && now - lastMailQueryMs_ < kMailQueryGapMs) return;
    lastMailQueryMs_ = now ? now : 1;
    network::Packet q(game::wireOpcode(game::LogicalOpcode::MSG_QUERY_NEXT_MAIL_TIME));
    socket_->send(q);
}

void WorldSession::requestMinimapNotices() {
    if (minimapNoticesAsked_ || state_ != State::InWorld || !socket_) return;
    minimapNoticesAsked_ = true;
    queryNextMailTime();
    network::Packet q(game::wireOpcode(game::LogicalOpcode::CMSG_CALENDAR_GET_NUM_PENDING));
    socket_->send(q);
    PSP_LOGI(tag::Net, "minimap_notices asked mail=1 calendar=1");
}

bool WorldSession::realmDate(game::WowDate& date, uint32_t& receivedMs) const {
    if (!realmDateKnown_) return false;
    date = realmDate_;
    receivedMs = realmDateMs_;
    return true;
}

}  // namespace wowee::platform::psp
