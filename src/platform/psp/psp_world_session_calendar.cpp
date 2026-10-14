// the calendar's session half - upstream's calendar handlers
// (game_handler_packets.cpp: SMSG_CALENDAR_SEND_CALENDAR / _SEND_EVENT through game::parseCalendarSendCalendar /
// parseCalendarSendEvent, src/game/calendar_data.cpp, unchanged) and SocialHandler's calendar senders
// (requestCalendar, requestCalendarEvent, respondToCalendarInvite) without GameHandler; Holidays.dbc /
// HolidayNames.dbc / HolidayDescriptions.dbc / Map.dbc names from data/wotlk.pak ("hol", "maps").
//
// PSP differences:
//  - SMSG_CALENDAR_COMMAND_RESULT is read as AzerothCore writes it (u32, u8, the string argument, then the u32
//    error - CalendarMgr::SendCalendarCommandResult); upstream took the u8 for the result (always 0: nothing was ever
//    said). The error is the original's CALENDAR_UPDATE_ERROR: StaticPopup CALENDAR_ERROR with the CALENDAR_ERROR_*
//    string of its code (GlobalStrings; strings.pak of the running language) - takeCalendarError.
//  - The alerts that change the list (an invite, its status, an event updated / removed, a raid lockout) ask for the
//    calendar again while the window has it (the original's CALENDAR_UPDATE_EVENT_LIST); upstream writes English chat
//    lines for some of them, the original none.
//  - SMSG_CALENDAR_SEND_NUM_PENDING / _EVENT_INVITE_ALERT stay the minimap's (psp_world_session_minimap.cpp: the
//    GameTimeFrame count) - they are only peeked here (return false).
//  - The own invite is removed with CMSG_CALENDAR_EVENT_RSVP status 9 (CALENDAR_STATUS_REMOVED: the server lets an
//    invitee leave a locked event so), as CalendarContextInviteRemove does for an invitee.
#include "platform/psp/psp_world_session.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::platform::psp {

namespace {

// CALENDAR_ERROR_* (GlobalStrings, strings.pak) by AzerothCore CalendarError; the %s / %d arguments filled by the caller.
const char* calendarErrorText(uint32_t err) {
    switch (err) {
        case 1: return CLIENT_STR(CALENDAR_ERROR_GUILD_EVENTS_EXCEEDED);
        case 2: return CLIENT_STR(CALENDAR_ERROR_EVENTS_EXCEEDED);
        case 3: return CLIENT_STR(CALENDAR_ERROR_SELF_INVITES_EXCEEDED);
        case 4: return CLIENT_STR(CALENDAR_ERROR_OTHER_INVITES_EXCEEDED);
        case 5: return CLIENT_STR(CALENDAR_ERROR_PERMISSIONS);
        case 6: return CLIENT_STR(CALENDAR_ERROR_EVENT_INVALID);
        case 7: return CLIENT_STR(CALENDAR_ERROR_NOT_INVITED);
        case 10: return CLIENT_STR(CALENDAR_ERROR_ALREADY_INVITED_TO_EVENT_S);
        case 12: return CLIENT_STR(CALENDAR_ERROR_NOT_ALLIED);
        case 13: return CLIENT_STR(CALENDAR_ERROR_IGNORED);  // IGNORING_YOU_S
        case 14: return CLIENT_STR(CALENDAR_ERROR_INVITES_EXCEEDED);
        case 16: return CLIENT_STR(CALENDAR_ERROR_INVALID_DATE);
        case 17: return CLIENT_STR(CALENDAR_ERROR_INVALID_TIME);
        case 19: return CLIENT_STR(CALENDAR_ERROR_NEEDS_TITLE);
        case 20: return CLIENT_STR(CALENDAR_ERROR_EVENT_PASSED);
        case 21: return CLIENT_STR(CALENDAR_ERROR_EVENT_LOCKED);
        case 22: return CLIENT_STR(CALENDAR_ERROR_DELETE_CREATOR_FAILED);
        case 26: return CLIENT_STR(CALENDAR_ERROR_ARENA_EVENTS_EXCEEDED);
        case 27: return CLIENT_STR(CALENDAR_ERROR_RESTRICTED_LEVEL);
        case 29: return CLIENT_STR(CALENDAR_ERROR_NO_INVITE);
        case 36: return CLIENT_STR(CALENDAR_ERROR_EVENT_WRONG_SERVER);
        case 37: return CLIENT_STR(CALENDAR_ERROR_INVITE_WRONG_SERVER);
        case 38: return CLIENT_STR(CALENDAR_ERROR_NO_GUILD_INVITES);
        case 39: return CLIENT_STR(CALENDAR_ERROR_INVALID_SIGNUP);
        case 40: return CLIENT_STR(CALENDAR_ERROR_NO_MODERATOR);
        default: return CLIENT_STR(CALENDAR_ERROR_INTERNAL);  // 8 and the codes without a string
    }
}

}  // namespace

// --- data/wotlk.pak "hol" / "maps" ---------------------------------------------------------------------------------

bool CalendarDb::load() {
    release();
    Pak pak;
    if (!wotlkPakOpen(pak)) return false;
    hol_ = wotlkPakRaw(pak, "hol", holSize_);
    map_ = wotlkPakRaw(pak, "maps", mapSize_);
    pak.close();
    auto ok = [](void* b, uint32_t size, const char* magic, uint32_t rec) {
        if (!b || size < 12 || std::memcmp(b, magic, 4) != 0) return false;
        uint16_t count = 0;
        uint32_t strOff = 0;
        std::memcpy(&count, static_cast<const char*>(b) + 6, 2);
        std::memcpy(&strOff, static_cast<const char*>(b) + 8, 4);
        return 12u + count * rec <= strOff && strOff <= size && static_cast<const char*>(b)[size - 1] == '\0';
    };
    if (!ok(hol_, holSize_, "WHOL", sizeof(HolRec)) || !ok(map_, mapSize_, "WMAP", sizeof(MapRec))) {
        PSP_LOGW(tag::Net, "wotlk_cal_bad hol=%u maps=%u", static_cast<unsigned>(holSize_), static_cast<unsigned>(mapSize_));
        release();
        return false;
    }
    return true;
}

void CalendarDb::release() {
    if (hol_) pakFree(hol_);
    if (map_) pakFree(map_);
    hol_ = map_ = nullptr;
    holSize_ = mapSize_ = 0;
}

const HolRec* CalendarDb::holiday(uint32_t id) const {
    if (!hol_) return nullptr;
    uint16_t count = 0;
    std::memcpy(&count, static_cast<const char*>(hol_) + 6, 2);
    const auto* recs = reinterpret_cast<const HolRec*>(static_cast<const char*>(hol_) + 12);
    const HolRec* end = recs + count;
    const HolRec* it = std::lower_bound(recs, end, id, [](const HolRec& r, uint32_t v) { return r.id < v; });
    return it != end && it->id == id ? it : nullptr;
}

const char* CalendarDb::holidayStr(uint32_t off) const {
    if (!hol_) return "";
    uint32_t strOff = 0;
    std::memcpy(&strOff, static_cast<const char*>(hol_) + 8, 4);
    return strOff + off < holSize_ ? static_cast<const char*>(hol_) + strOff + off : "";
}

const char* CalendarDb::mapName(uint32_t mapId) const {
    if (!map_) return "";
    uint16_t count = 0;
    uint32_t strOff = 0;
    std::memcpy(&count, static_cast<const char*>(map_) + 6, 2);
    std::memcpy(&strOff, static_cast<const char*>(map_) + 8, 4);
    const auto* recs = reinterpret_cast<const MapRec*>(static_cast<const char*>(map_) + 12);
    const MapRec* end = recs + count;
    const MapRec* it = std::lower_bound(recs, end, mapId, [](const MapRec& r, uint32_t v) { return r.id < v; });
    if (it == end || it->id != mapId || strOff + it->name >= mapSize_) return "";
    return static_cast<const char*>(map_) + strOff + it->name;
}

uint16_t CalendarDb::mapType(uint32_t mapId) const {
    if (!map_) return 0;
    uint16_t count = 0;
    std::memcpy(&count, static_cast<const char*>(map_) + 6, 2);
    const auto* recs = reinterpret_cast<const MapRec*>(static_cast<const char*>(map_) + 12);
    const MapRec* end = recs + count;
    const MapRec* it = std::lower_bound(recs, end, mapId, [](const MapRec& r, uint32_t v) { return r.id < v; });
    return it != end && it->id == mapId ? it->type : 0;
}

void WorldSession::calendarSignUp(uint64_t eventId, bool tentative) {  // CalendarEventSignUp (CMSG_CALENDAR_EVENT_SIGNUP)
    if (state_ != State::InWorld || !socket_ || eventId == 0) return;
    const uint16_t wire = game::wireOpcode(game::LogicalOpcode::CMSG_CALENDAR_EVENT_SIGNUP);
    if (wire == 0xFFFF) return;
    network::Packet pkt(wire);
    pkt.writeUInt64(eventId);
    pkt.writeUInt8(tentative ? 1 : 0);  // AzerothCore HandleCalendarEventSignup: eventId, bool tentative
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "calendar_signup event=%llu tentative=%d", static_cast<unsigned long long>(eventId), tentative ? 1 : 0);
}

// --- Packets ---------------------------------------------------------------------------------------------------------

bool WorldSession::handleCalendarPacket(game::LogicalOpcode op, network::Packet& packet) {
    using game::LogicalOpcode;
    auto refresh = [this](const char* why) {
        // PSP: the list is asked for again by the open window (CALENDAR_UPDATE_EVENT_LIST): calendarStale().
        if (!calendarStale_ && calendarKnown_ && ++calendarLogs_ <= 40) PSP_LOGI(tag::Net, "calendar_stale why=%s", why);
        calendarStale_ = true;
    };
    switch (op) {
        case LogicalOpcode::SMSG_CALENDAR_SEND_CALENDAR: {  // upstream: parseCalendarSendCalendar, whole or nothing
            game::CalendarData parsed;
            if (!game::parseCalendarSendCalendar(packet, parsed)) {
                PSP_LOGW(tag::Net, "calendar_send_calendar_bad size=%u", static_cast<unsigned>(packet.getSize()));
                packet.skipAll();
                return true;
            }
            calendarData_ = std::move(parsed);
            calendarKnown_ = true;
            calendarReceivedMs_ = logUptimeMs();
            ++calendarVersion_;
            if (++calendarLogs_ <= 40)
                PSP_LOGI(tag::Net, "calendar_list events=%u invites=%u lockouts=%u holidays=%u",
                         static_cast<unsigned>(calendarData_.events.size()),
                         static_cast<unsigned>(calendarData_.invites.size()),
                         static_cast<unsigned>(calendarData_.lockouts.size()),
                         static_cast<unsigned>(calendarData_.holidays.size()));
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_CALENDAR_SEND_EVENT: {  // upstream: parseCalendarSendEvent
            game::CalendarEventDetail parsed;
            if (!game::parseCalendarSendEvent(packet, parsed)) {
                PSP_LOGW(tag::Net, "calendar_send_event_bad");
                packet.skipAll();
                return true;
            }
            calendarEvent_ = std::move(parsed);
            calendarEventKnown_ = true;
            ++calendarEventVersion_;
            // The invitees' names (the invite list): asked for as the chat asks for a sender's.
            for (const auto& inv : calendarEvent_.invitees)
                if (inv.guid && !nameOf(inv.guid) && nameQueried_.insert(inv.guid).second && socket_)
                    socket_->send(game::NameQueryPacket::build(inv.guid));
            if (calendarEvent_.creatorGuid && !nameOf(calendarEvent_.creatorGuid) &&
                nameQueried_.insert(calendarEvent_.creatorGuid).second && socket_)
                socket_->send(game::NameQueryPacket::build(calendarEvent_.creatorGuid));
            if (++calendarLogs_ <= 40)
                PSP_LOGI(tag::Net, "calendar_event id=%llu invitees=%u", static_cast<unsigned long long>(calendarEvent_.eventId),
                         static_cast<unsigned>(calendarEvent_.invitees.size()));
            packet.skipAll();
            return true;
        }
        case LogicalOpcode::SMSG_CALENDAR_COMMAND_RESULT: {  // PSP: AzerothCore's layout (see the file comment)
            if (!packet.hasRemaining(5)) {
                packet.skipAll();
                return true;
            }
            packet.readUInt32();
            packet.readUInt8();
            const std::string arg = packet.hasData() ? packet.readString() : std::string();
            const uint32_t err = packet.hasRemaining(4) ? packet.readUInt32() : 0;
            packet.skipAll();
            if (err == 0) return true;
            std::string text = calendarErrorText(err);
            const size_t s = text.find("%s");
            if (s != std::string::npos) text.replace(s, 2, arg);
            const size_t d = text.find("%d");
            if (d != std::string::npos) text.replace(d, 2, err == 1 ? "100" : err == 2 ? "30" : "100");  // CalendarLimits
            if (calendarErrors_.size() < 4) calendarErrors_.push_back(text::plural(text));
            PSP_LOGI(tag::Net, "calendar_error err=%u", static_cast<unsigned>(err));
            return true;
        }
        case LogicalOpcode::SMSG_CALENDAR_SEND_NUM_PENDING:
        case LogicalOpcode::SMSG_CALENDAR_EVENT_INVITE_ALERT:
            refresh("invite_alert");
            return false;  // the minimap counts them (psp_world_session_minimap.cpp)
        case LogicalOpcode::SMSG_CALENDAR_EVENT_STATUS:
        case LogicalOpcode::SMSG_CALENDAR_EVENT_INVITE:
        case LogicalOpcode::SMSG_CALENDAR_EVENT_INVITE_REMOVED:
        case LogicalOpcode::SMSG_CALENDAR_EVENT_INVITE_REMOVED_ALERT:
        case LogicalOpcode::SMSG_CALENDAR_EVENT_INVITE_STATUS_ALERT:
        case LogicalOpcode::SMSG_CALENDAR_EVENT_MODERATOR_STATUS_ALERT:
        case LogicalOpcode::SMSG_CALENDAR_EVENT_REMOVED_ALERT:
        case LogicalOpcode::SMSG_CALENDAR_EVENT_UPDATED_ALERT:
        case LogicalOpcode::SMSG_CALENDAR_CLEAR_PENDING_ACTION:
        case LogicalOpcode::SMSG_CALENDAR_RAID_LOCKOUT_ADDED:
        case LogicalOpcode::SMSG_CALENDAR_RAID_LOCKOUT_REMOVED:
        case LogicalOpcode::SMSG_CALENDAR_RAID_LOCKOUT_UPDATED:
            packet.skipAll();
            refresh("alert");
            return true;
        default: return false;
    }
}

void WorldSession::calendarRequest() {  // upstream SocialHandler::requestCalendar
    if (state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_CALENDAR_GET_CALENDAR));
    socket_->send(pkt);
    network::Packet num(game::wireOpcode(game::LogicalOpcode::CMSG_CALENDAR_GET_NUM_PENDING));
    socket_->send(num);
    calendarStale_ = false;
    if (!calendarKnown_) PSP_LOGI(tag::Net, "calendar_request");
}

void WorldSession::calendarRequestEvent(uint64_t eventId) {  // upstream requestCalendarEvent
    if (state_ != State::InWorld || !socket_ || eventId == 0) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_CALENDAR_GET_EVENT));
    pkt.writeUInt64(eventId);
    socket_->send(pkt);
}

void WorldSession::calendarRsvp(uint64_t eventId, uint64_t inviteId, uint32_t status) {  // respondToCalendarInvite
    if (state_ != State::InWorld || !socket_) return;
    network::Packet pkt(game::wireOpcode(game::LogicalOpcode::CMSG_CALENDAR_EVENT_RSVP));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt32(status);
    socket_->send(pkt);
    PSP_LOGI(tag::Net, "calendar_rsvp event=%llu status=%u", static_cast<unsigned long long>(eventId),
             static_cast<unsigned>(status));
}

void WorldSession::calendarRemoveOwnInvite(uint64_t eventId, uint64_t inviteId) {
    calendarRsvp(eventId, inviteId, 9);  // CALENDAR_STATUS_REMOVED
}

bool WorldSession::takeCalendarError(std::string& out) {
    if (calendarErrors_.empty()) return false;
    out = calendarErrors_.front();
    calendarErrors_.pop_front();
    return true;
}

}  // namespace wowee::platform::psp
