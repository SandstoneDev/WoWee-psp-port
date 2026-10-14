// CalendarFrame of the original ruRU 3.3.5a client (the calendar add-on's .xml / .lua
// and its templates) over the session's calendar (psp_world_session_calendar.cpp: SMSG_CALENDAR_SEND_
// CALENDAR / _SEND_EVENT through upstream's game::parseCalendar*, CMSG_CALENDAR_GET_CALENDAR / _GET_EVENT / _EVENT_RSVP
// / _EVENT_SIGNUP) and data/wotlk.pak ("hol" Holidays / HolidayNames / HolidayDescriptions, "maps" Map, the art).
//
// CalendarFrame (659 x 630 at 0.6 x 0.41) - the minimap's GameTimeFrame (GameTimeFrame_OnClick: ToggleCalendar):
//   - the month (CalendarMonthName / CalendarYearName) with CalendarPrevMonthButton / CalendarNextMonthButton
//     (CalendarFrame_UpdateMonthOffsetButtons: upstream's CalendarGetMinDate / MaxCreateDate, 12 months each way), the
//     weekdays from Monday (CALENDAR_FIRST_WEEKDAY 2 of ruRU), the 42 day buttons (CalendarFrame_Update: the previous
//     and the next month's days darkened, today's frame with its glow, the selected day's highlight);
//   - a day's events (CalendarFrame_UpdateDayEvents, the rows of upstream's calendarEntriesForDay): the holidays
//     (Holidays.dbc names, CALENDAR_EVENTNAME_FORMAT_START / _END, the ongoing days without a line), the raid lockouts
//     (CALENDAR_EVENTNAME_FORMAT_RAID_LOCKOUT), the player's and the guild's events (the invite status' colour; the
//     moderator's blue),
//     the pending invite mark (EventNotification), the day's picture (CalendarFrame_UpdateDayTextures: a holiday's
//     Calendar_<name>Start / End, a raid / dungeon event's LFGIcon, the PvP / meeting / other icons; the ongoing art
//     of a holiday longer than 2 days), the "more" arrow; the tooltip (CalendarDayButton_OnEnter: FULLDATE and the
//     events with their times and who invited);
//   - CalendarFilterFrame CALENDAR_FILTERS: CALENDAR_FILTER_BATTLEGROUND / _DARKMOON / _RAID_LOCKOUTS / _RAID_RESETS /
//     _WEEKLY_HOLIDAYS (the holidays' CalendarFilterType 2 / 1 / 0 and the lockouts);
//   - the close button.
// CalendarViewEventFrame (Cross on a day with an event: CalendarFrame_OpenEvent -> CMSG_CALENDAR_GET_EVENT): the
// icon, the title (CALENDAR_VIEW_EVENTTITLE_LOCKED when locked), the type (CALENDAR_TYPE_RAID " - <dungeon>"), CALENDAR_EVENT_
// CREATORNAME, FULLDATE, the time, the description; Accept / CALENDAR_STATUS_TENTATIVE / Decline / DELETE
// (CalendarViewEventRSVP_Update: today or later, the status the player can change; a guild sign-up event:
// CALENDAR_SIGNUP / CALENDAR_STATUS_TENTATIVE / DELETE); the invite list (the name, the status in its colour).
// CalendarViewHolidayFrame (the name, the description over Calendar_DefaultHoliday) and CalendarViewRaidFrame
// (CALENDAR_RAID_LOCKOUT_DESCRIPTION). StaticPopup CALENDAR_ERROR (the server's CALENDAR_ERROR_*).
// PSP differences (PSP: below): no event creation / editing / copying, no context menus, no mass invite / class
// totals (no text entry, no right-click menus); the own events open the view frame too; Cross on a day opens its
// first event and, again, the next one (the original's event buttons are 5 px high here); up to 2 title-only lines a
// day (4 buttons of 12 units / 2 of 24 with the time); the dark days shaded flat (not the CalendarShadows edges); the
// event frames are a window at the right edge over the calendar (the two side by side do not fit) at 0.6 x 0.5, the
// invite rows 20 units; raid resets (calendarShowResets, off by default) are not drawn; the filters are the CVars'
// defaults each session; the invitees' classes are not known (no class in SMSG_CALENDAR_SEND_EVENT); the event flags
// by AzerothCore's CalendarFlags (0x10 invites locked, 0x40 a guild announcement, 0x400 a guild event - upstream's
// Lua API reads 0x1000 / type 4); the sign-up kind of a guild event from the own invite's status (the invite type is
// not on the wire).
#include "ui/psp/ui_world_screen.hpp"

#include "core/local_time.hpp"
#include "game/calendar_data.hpp"
#include "game/packed_time.hpp"
#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_display.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"  // the client's strings and the port's own
#include "platform/psp/psp_pak.hpp"
#include "platform/psp/psp_prof.hpp"
#include "platform/psp/psp_texture.hpp"
#include "platform/psp/psp_wotlk_db.hpp"
#include "ui/psp/ui_world_map.hpp"
#include "ui/psp/ui_world_sounds.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

// Widget ids: 0x7B80-0x7BFF (the achievements have 0x7B00-0x7B7F).
enum : uint32_t {
    kIdCalClose = 0x7B80,
    kIdCalPrev = 0x7B81,
    kIdCalNext = 0x7B82,
    kIdCalFilter = 0x7B83,
    kIdCalDay0 = 0x7B90,       // + 0..41
    kIdCalFilterDd0 = 0x7BC0,  // + 0..4
    kIdCalEvClose = 0x7BD0,
    kIdCalAccept = 0x7BD1,
    kIdCalTentative = 0x7BD2,
    kIdCalDecline = 0x7BD3,
    kIdCalRemove = 0x7BD4,
    kIdCalInvUp = 0x7BD5,
    kIdCalInvDown = 0x7BD6,
    kIdCalInv0 = 0x7BE0,  // + 0..13
    kIdCalErrOk = 0x7BF0,
};

// CalendarFrame: 659 x 624 at 0.6 x 0.41, centred at the top; the event frames 320 wide at 0.6 x 0.5, at the right.
constexpr float kSX = 0.6f, kSY = 0.41f, kCx = 42.0f, kCy = 8.0f;
float CX(float x) { return kCx + x * kSX; }
float CY(float y) { return kCy + y * kSY; }
Rect CR(float x, float y, float w, float h) { return Rect{CX(x), CY(y), w * kSX, h * kSY}; }
constexpr float kEX = 0.6f, kEY = 0.5f;
constexpr float kEvW = 320.0f;
float EX(float x) { return static_cast<float>(pp::kScreenW) - 4.0f - kEvW * kEX + x * kEX; }
float EY(float y) { return 10.0f + y * kEY; }
Rect ER(float x, float y, float w, float h) { return Rect{EX(x), EY(y), w * kEX, h * kEY}; }

constexpr int kFirstWeekday = 2;  // CALENDAR_FIRST_WEEKDAY (ruRU localization.lua): Monday
constexpr int kMaxDayEvents = 2;  // PSP (CALENDAR_DAYBUTTON_MAX_VISIBLE_EVENTS 4: 12-unit buttons, 5 px here)
constexpr float kInviteRowU = 20.0f;  // PSP: the invite list's rows (CalendarEventInviteListButtonTemplate 14 units)

// 0xAABBGGRR
constexpr uint32_t kGold = 0xFF00D1FFu, kWhite = 0xFFFFFFFFu, kGrey = 0xFF808080u, kGreen = 0xFF1AFF1Au,
                   kRed = 0xFF1A1AFFu, kOrange = 0xFF4080FFu;  // NORMAL / HIGHLIGHT / GRAY / GREEN / RED / ORANGE
constexpr uint32_t kModerator = 0xFFFFBF8Au;  // CALENDAR_EVENTCOLOR_MODERATOR (0.54, 0.75, 1.0)

// GlobalStrings.lua (strings.pak of the running language).
using pp::CS;
constexpr CS kMonthNames[12] = {CS::MONTH_JANUARY, CS::MONTH_FEBRUARY, CS::MONTH_MARCH,     CS::MONTH_APRIL,
                                CS::MONTH_MAY,     CS::MONTH_JUNE,     CS::MONTH_JULY,      CS::MONTH_AUGUST,
                                CS::MONTH_SEPTEMBER, CS::MONTH_OCTOBER, CS::MONTH_NOVEMBER, CS::MONTH_DECEMBER};
constexpr CS kWeekdayNames[7] = {CS::WEEKDAY_SUNDAY,   CS::WEEKDAY_MONDAY, CS::WEEKDAY_TUESDAY,
                                 CS::WEEKDAY_WEDNESDAY, CS::WEEKDAY_THURSDAY, CS::WEEKDAY_FRIDAY,
                                 CS::WEEKDAY_SATURDAY};  // 1 = Sunday
constexpr CS kFilterNames[5] = {CS::CALENDAR_FILTER_BATTLEGROUND, CS::CALENDAR_FILTER_DARKMOON,
                                CS::CALENDAR_FILTER_RAID_LOCKOUTS, CS::CALENDAR_FILTER_RAID_RESETS,
                                CS::CALENDAR_FILTER_WEEKLY_HOLIDAYS};
constexpr CS kTypeNames[5] = {CS::CALENDAR_TYPE_RAID, CS::CALENDAR_TYPE_DUNGEON, CS::CALENDAR_TYPE_PVP,
                              CS::CALENDAR_TYPE_MEETING, CS::CALENDAR_TYPE_OTHER};  // by the wire
constexpr CS kStatusNames[9] = {CS::CALENDAR_STATUS_INVITED,   CS::CALENDAR_STATUS_ACCEPTED, CS::CALENDAR_STATUS_DECLINED,
                                CS::CALENDAR_STATUS_CONFIRMED, CS::CALENDAR_STATUS_OUT,      CS::CALENDAR_STATUS_STANDBY,
                                CS::CALENDAR_STATUS_SIGNEDUP,  CS::CALENDAR_STATUS_NOT_SIGNEDUP,
                                CS::CALENDAR_STATUS_TENTATIVE};

// CALENDAR_INVITESTATUS_INFO colours by the wire status (0 invited .. 8 tentative).
uint32_t statusColor(uint8_t st) {
    switch (st) {
        case 1:
        case 3:
        case 6: return kGreen;   // ACCEPTED, CONFIRMED, SIGNEDUP
        case 2:
        case 4: return kRed;     // DECLINED, OUT
        case 5:
        case 8: return kOrange;  // STANDBY, TENTATIVE
        default: return kGold;   // INVITED, NOT_SIGNEDUP
    }
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil).
int64_t dayNumber(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
int daysInMonth(int month, int year) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[(month - 1) % 12];
}
int weekdayOf(int y, int m, int d) {  // 1 = Sunday .. 7 = Saturday
    const int64_t n = dayNumber(y, m, d);  // 1970-01-01 was a Thursday (5)
    return static_cast<int>(((n % 7) + 7 + 4) % 7) + 1;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// GameTime_GetFormattedTime(hour, minute, true): TIMEMANAGER_TICKER_24HOUR "%02d:%02d", else TIME_TWELVEHOURAM / PM
// "%d:%02d AM" (wowee.cfg clock24, as the minimap's clock).
std::string formatTime(int hour, int minute) {
    static const bool military = pp::configGetInt("clock24", 1) != 0;
    char b[24];
    if (military) std::snprintf(b, sizeof(b), "%02d:%02d", hour, minute);
    else std::snprintf(b, sizeof(b), "%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
    return b;
}

// The calendar types of the player-made events (CalendarGetDayEvent's calendarType) by the event flags
// (AzerothCore CalendarFlags: 0x40 WITHOUT_INVITES = a guild announcement, 0x400 GUILD_EVENT).
enum : uint8_t { kTypePlayer = 0, kTypeGuildEvent, kTypeAnnouncement };
uint8_t calendarType(uint32_t flags) {
    if (flags & 0x40) return kTypeAnnouncement;
    return (flags & 0x400) ? kTypeGuildEvent : kTypePlayer;
}

// The player's invite to an event (SMSG_CALENDAR_SEND_CALENDAR's invite list), or nullptr.
const game::CalendarInvite* ownInvite(const game::CalendarData& data, uint64_t eventId) {
    for (const auto& i : data.invites)
        if (i.eventId == eventId) return &i;
    return nullptr;
}

// _CalendarFrame_IsSignUpEvent: GUILD_EVENT and CALENDAR_INVITETYPE_SIGNUP. PSP: the invite type is not on the wire -
// a guild event without an invite of ours or with a sign-up status (6 signed up, 7 not signed up, 8 tentative;
// CMSG_CALENDAR_EVENT_SIGNUP makes the invite with 6 / 8).
bool isSignUpEvent(uint32_t flags, const game::CalendarInvite* inv) {
    if (calendarType(flags) != kTypeGuildEvent) return false;
    return !inv || inv->status == 6 || inv->status == 7 || inv->status == 8;
}

}  // namespace

// --- State ----------------------------------------------------------------------------------------------------------------

struct WorldScreen::CalUi {
    pp::CalendarDb db;
    bool dbTried = false, lfgDbUsed = false;
    // Art (data/wotlk.pak) while the frame is open; the day pictures of the viewed month.
    pp::Texture frame, dayBg, dayHl, today, todayGlow, evBg, pending, more, evHl, prev, next, prevDis, nextDis, evPvp1,
        evPvp2, evMeeting, evOther, holidayInfo, ddBtn, ddBtnDn;
    bool artTried = false;
    uint32_t artBytes = 0;
    std::unordered_map<std::string, pp::Texture> pics;  // calh_* / lfgic_* of the viewed month and the open event
    std::vector<std::string> picsMissing;

    bool shown = false;
    int viewMonth = 0, viewYear = 0;            // CalendarFrame.viewedMonth / viewedYear
    int selDay = 0, selMonth = 0, selYear = 0;  // CalendarFrame.selectedDay / Month / Year
    bool filter[5] = {true, true, true, false, true};  // battleground, darkmoon, lockouts, resets, weekly (the CVars)
    bool ddOpen = false;
    struct Entry {
        uint8_t kind = 0;  // 1 holiday, 2 raid lockout, 3 event
        uint32_t index = 0;
        uint8_t seq = 0;   // 0 "", 1 START, 2 ONGOING, 3 END
        int seqDays = 0, hour = 0, minute = 0;
    };
    struct Day {
        int day = 0, offset = 0;  // the month offset: -1 previous, 0 viewed, 1 next
        std::vector<Entry> entries;
        bool pending = false;
        uint8_t cell = 0;          // the random(0,1) x / y cell of CalendarBackground
        std::string pic, picAlt;   // EventTexture (picAlt: the type's when the dungeon's is missing)
        bool picCrop = false;      // an event icon: TexCoords 0..0.7109375 (calh_* are baked cut)
        uint8_t picArt = 0;        // CALENDAR_EVENTTYPE_TEXTURES of the frame's art: 1 PvP01, 2 PvP02, 3 meeting, 4 other
        std::string overlay;       // OverlayFrameTexture (the first holiday's ONGOING, more than 2 days)
    };
    std::array<Day, 42> days{};
    uint64_t cacheKey = ~0ull;
    // The open event (CalendarFrame_OpenEvent): 0 none, 1 player / guild event, 2 holiday, 3 raid lockout.
    uint8_t openKind = 0;
    uint32_t openIndex = 0;
    uint64_t openEventId = 0;
    int openButton = -1, openEntry = -1;
    uint32_t openEventVersion = 0;
    bool waitingEvent = false;
    int inviteScroll = 0;
    std::deque<std::string> errors;
    std::string errorText;
    uint32_t requestMs = 0;
    uint32_t logs = 0;

    std::array<pp::Texture*, 20> textures() {
        return {&frame,  &dayBg,  &dayHl,     &today,   &todayGlow, &evBg,        &pending, &more,  &evHl,    &prev,
                &next,   &prevDis, &nextDis,  &evPvp1,  &evPvp2,    &evMeeting,   &evOther, &holidayInfo, &ddBtn, &ddBtnDn};
    }
    void releaseArt() {
        for (pp::Texture* t : textures())
            if (t->pixels) pp::textureRelease(*t);
        releasePictures();
        if (artTried) PSP_LOGI(pp::tag::Gfx, "cal_art_free kb=%u", static_cast<unsigned>((artBytes + 1023u) / 1024u));
        artTried = false;
        artBytes = 0;
        cacheKey = ~0ull;  // the pictures again with the next month view
    }
    void loadArt() {
        if (artTried) return;
        artTried = true;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        const std::pair<const char*, pp::Texture*> list[] = {
            {"cal_frame", &frame},          {"cal_daybg", &dayBg},         {"cal_dayhl", &dayHl},
            {"cal_today", &today},          {"cal_todayglow", &todayGlow}, {"cal_evbg", &evBg},
            {"cal_pending", &pending},      {"cal_more", &more},           {"cal_evhl", &evHl},
            {"cal_prev", &prev},            {"cal_next", &next},           {"cal_prev_dis", &prevDis},
            {"cal_next_dis", &nextDis},     {"cal_ev_pvp01", &evPvp1},     {"cal_ev_pvp02", &evPvp2},
            {"cal_ev_meeting", &evMeeting}, {"cal_ev_other", &evOther},    {"calh_calendar_defaultholiday", &holidayInfo},
            {"lfg_dd_btn", &ddBtn},         {"lfg_dd_btn_dn", &ddBtnDn}};
        for (const auto& [name, tex] : list) pp::wotlkPakTexture(pak, name, *tex, &artBytes);
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "cal_art frame=%d kb=%u", frame.pixels ? 1 : 0, static_cast<unsigned>((artBytes + 1023u) / 1024u));
    }
    // The pictures not read yet, in one open of the pak (PSP: an open costs 17-34 ms on the hardware).
    void loadPictures(const std::vector<std::string>& names) {
        std::vector<const std::string*> todo;
        for (const std::string& n : names)
            if (!n.empty() && !pics.count(n) && std::find(picsMissing.begin(), picsMissing.end(), n) == picsMissing.end() &&
                std::find_if(todo.begin(), todo.end(), [&](const std::string* t) { return *t == n; }) == todo.end())
                todo.push_back(&n);
        if (todo.empty()) return;
        pp::Pak pak;
        if (!pp::wotlkPakOpen(pak)) return;
        const uint32_t before = artBytes;
        for (const std::string* n : todo) {
            pp::Texture t;
            if (pp::wotlkPakTexture(pak, n->c_str(), t, &artBytes)) pics[*n] = t;
            else picsMissing.push_back(*n);
        }
        pak.close();
        PSP_LOGI(pp::tag::Gfx, "cal_pics n=%u kb=%u", static_cast<unsigned>(todo.size()),
                 static_cast<unsigned>((artBytes - before + 1023u) / 1024u));
    }
    const pp::Texture* cached(const std::string& name) const {
        auto it = pics.find(name);
        return it != pics.end() && it->second.pixels ? &it->second : nullptr;
    }
    const pp::Texture* picture(const std::string& name) {  // one picture now (the event frame's icon)
        loadPictures({name});
        return cached(name);
    }
    void releasePictures() {
        for (auto& [name, t] : pics)
            if (t.pixels) pp::textureRelease(t);
        pics.clear();
        picsMissing.clear();
    }
    ~CalUi() { releaseArt(); }
};

void WorldScreen::destroyCalUi(CalUi* p) { delete p; }

WorldScreen::CalUi& WorldScreen::calUi() {
    if (!cal_) cal_ = std::unique_ptr<CalUi, void (*)(CalUi*)>(new CalUi(), &WorldScreen::destroyCalUi);
    return *cal_;
}

namespace {

// UnitFactionGroup("player") == "Alliance".
bool allianceSide(const pp::WorldSession& s) {
    const game::Character* me = s.activeCharacter();
    if (!me) return false;
    switch (me->race) {
        case game::Race::HUMAN:
        case game::Race::DWARF:
        case game::Race::NIGHT_ELF:
        case game::Race::GNOME:
        case game::Race::DRAENEI: return true;
        default: return false;
    }
}

// CalendarGetDate: the realm's date (SMSG_LOGIN_SETTIMESPEED, run on with the uptime), else the PSP's clock.
void calendarToday(const pp::WorldSession& s, int& y, int& m, int& d) {
    game::WowDate wd;
    uint32_t recvMs = 0;
    if (s.realmDate(wd, recvMs)) {
        y = wd.fullYear();
        m = wd.month;
        d = wd.day;
        int add = (wd.hour * 60 + wd.minute + static_cast<int>((pp::logUptimeMs() - recvMs) / 60000u)) / 1440;
        while (add-- > 0) {
            if (++d > daysInMonth(m, y)) {
                d = 1;
                if (++m > 12) {
                    m = 1;
                    ++y;
                }
            }
        }
        return;
    }
    const std::tm lt = core::localTime(std::time(nullptr));
    y = lt.tm_year + 1900;
    m = lt.tm_mon + 1;
    d = lt.tm_mday;
}

// CalendarFrame_UpdateMonthOffsetButtons: CalendarGetMinDate / CalendarGetMaxCreateDate (upstream: today -/+ 12
// months) bound the viewed month.
bool calMonthButtonEnabled(int viewYear, int viewMonth, int dir, const pp::WorldSession& s) {
    int y = 0, m = 0, d = 0;
    calendarToday(s, y, m, d);
    const int viewed = viewYear * 12 + (viewMonth - 1), now = y * 12 + (m - 1);
    return dir < 0 ? viewed > now - 12 : viewed < now + 12;
}

}  // namespace

// --- Per frame -------------------------------------------------------------------------------------------------------------

void WorldScreen::calToggle() {  // ToggleCalendar
    if (windowOpen(kWinCalendar)) {
        closeWindow(kWinCalendar);
        return;
    }
    calUi();
    openWindow(kWinCalendar);
}

void WorldScreen::calUpdate(float dtSec) {
    (void)dtSec;
    std::string err;
    while (session_.takeCalendarError(err)) {  // CALENDAR_UPDATE_ERROR -> StaticPopup_Show("CALENDAR_ERROR", message)
        CalUi& C = calUi();
        if (C.errors.size() < 4) C.errors.push_back(err);
    }
    if (!cal_) return;
    CalUi& C = *cal_;
    if (!C.errors.empty() && !windowOpen(kWinWotlkPopup)) {
        C.errorText = C.errors.front();
        C.errors.pop_front();
        openWindow(kWinWotlkPopup);
        winFocus_.setFocus(kIdCalErrOk);
        playSoundByName("igMainMenuOpen");
    }
    const bool open = windowOpen(kWinCalendar);
    if (open && !C.shown) {  // CalendarFrame_OnShow: today's month, OpenCalendar, igSpellBookOpen
        C.shown = true;
        int y = 0, m = 0, d = 0;
        calendarToday(session_, y, m, d);
        C.viewMonth = m;
        C.viewYear = y;
        C.openKind = 0;
        C.cacheKey = ~0ull;
        session_.calendarRequest();
        C.requestMs = pp::logUptimeMs();
        playSoundByName("igSpellBookOpen");
        winFocus_.setFocus(kIdCalDay0 + 10);
        PSP_LOGI(pp::tag::Gfx, "cal_open %d-%02d known=%d", y, m, session_.calendarKnown() ? 1 : 0);
    } else if (!open && C.shown) {  // CalendarFrame_OnHide
        C.shown = false;
        C.ddOpen = false;
        C.openKind = 0;
        closeWindow(kWinCalendarEvent);
        playSoundByName("igSpellBookClose");
    }
    if (open && session_.calendarStale() && pp::logUptimeMs() - C.requestMs > 2000u) {
        // CALENDAR_UPDATE_EVENT_LIST after an alert: the list again (the open event too).
        session_.calendarRequest();
        C.requestMs = pp::logUptimeMs();
        if (C.openKind == 1 && C.openEventId) session_.calendarRequestEvent(C.openEventId);
    }
    // CALENDAR_OPEN_EVENT: the event's details came - its frame.
    if (C.waitingEvent && session_.calendarEventVersion() != C.openEventVersion) {
        C.waitingEvent = false;
        C.openEventVersion = session_.calendarEventVersion();
        if (session_.calendarEventKnown() && session_.calendarEvent().eventId == C.openEventId) {
            C.inviteScroll = 0;
            if (!windowOpen(kWinCalendarEvent)) openWindow(kWinCalendarEvent);
            placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), kWinCalendarEvent), placeNext_.end());
            autoPos_.erase(kWinCalendarEvent);
        }
    }
    // Closed by Circle on its title (CalendarFrame_CloseEvent); not while the server's details are on the way.
    if (!windowOpen(kWinCalendarEvent) && C.openKind != 0 && !C.waitingEvent) C.openKind = 0;
    if (!open && C.artTried) C.releaseArt();
    if (!open && C.db.loaded()) {
        C.db.release();
        C.dbTried = false;
    }
    if (!open && C.lfgDbUsed) {
        C.lfgDbUsed = false;
        if (!windowOpen(kWinLfd)) session_.lfgReleaseDb();
    }
}

// --- Actions ----------------------------------------------------------------------------------------------------------------

bool WorldScreen::calCloseDropDown() {
    if (!cal_ || !cal_->ddOpen || !windowOpen(kWinCalendar)) return false;
    cal_->ddOpen = false;
    return true;
}

bool WorldScreen::calAltAction(uint32_t alt) { return calAction(alt); }  // PSP: no context menus - as the left click

bool WorldScreen::calAction(uint32_t act) {
    if (act < 0x7B80 || act > 0x7BFF) return false;
    CalUi& C = calUi();
    if (act == kIdCalErrOk) {
        closeWindow(kWinWotlkPopup);
        return true;
    }
    if (C.ddOpen) {  // CalendarFilterDropDown: keepShownOnClick (a click on an item toggles it, the list stays)
        if (act >= kIdCalFilterDd0 && act < kIdCalFilterDd0 + 5) {
            bool& f = C.filter[act - kIdCalFilterDd0];
            f = !f;
            C.cacheKey = ~0ull;
            playSoundByName(f ? "igMainMenuOptionCheckBoxOn" : "igMainMenuOptionCheckBoxOff");
            return true;
        }
        C.ddOpen = false;
        if (act == kIdCalFilter) return true;
    }
    const auto& data = session_.calendarData();
    switch (act) {
        case kIdCalClose: closeWindow(kWinCalendar); return true;
        case kIdCalPrev:
        case kIdCalNext: {  // CalendarFrame_OffsetMonth
            const int d = act == kIdCalNext ? 1 : -1;
            if (!calMonthButtonEnabled(C.viewYear, C.viewMonth, d, session_)) return true;
            C.viewMonth += d;
            if (C.viewMonth < 1) {
                C.viewMonth = 12;
                --C.viewYear;
            } else if (C.viewMonth > 12) {
                C.viewMonth = 1;
                ++C.viewYear;
            }
            C.cacheKey = ~0ull;
            C.releasePictures();
            C.openButton = -1;  // the open event's day button is another month's now (its frame stays)
            playSoundByName("igAbiliityPageTurn");
            return true;
        }
        case kIdCalFilter:
            C.ddOpen = true;
            playSoundByName("igMainMenuOptionCheckBoxOn");
            return true;
        case kIdCalEvClose:  // CalendarEventCloseButton_OnClick: CalendarFrame_CloseEvent
            C.openKind = 0;
            closeWindow(kWinCalendarEvent);
            playSoundByName("igMainMenuQuit");
            return true;
        case kIdCalInvUp: C.inviteScroll = std::max(0, C.inviteScroll - 1); return true;
        case kIdCalInvDown: ++C.inviteScroll; return true;
        case kIdCalAccept:
        case kIdCalTentative:
        case kIdCalDecline:
        case kIdCalRemove: {
            if (C.openKind != 1 || !session_.calendarEventKnown()) return true;
            const auto& ev = session_.calendarEvent();
            const game::CalendarInvite* inv = ownInvite(data, ev.eventId);
            if (isSignUpEvent(ev.flags, inv)) {
                // CalendarEventSignUp / CalendarEventTentative of a sign-up event (CMSG_CALENDAR_EVENT_SIGNUP);
                // CalendarRemoveEvent: the own sign-up out.
                if (act == kIdCalAccept || act == kIdCalTentative) session_.calendarSignUp(ev.eventId, act == kIdCalTentative);
                else if (act == kIdCalRemove && inv) session_.calendarRemoveOwnInvite(ev.eventId, inv->inviteId);
            } else if (inv) {
                // CalendarEventAvailable / Tentative / Decline (CMSG_CALENDAR_EVENT_RSVP: 1 accepted, 8 tentative,
                // 2 declined); CalendarRemoveEvent of an invitee: the own invite out.
                if (act == kIdCalAccept) session_.calendarRsvp(ev.eventId, inv->inviteId, 1);
                else if (act == kIdCalTentative) session_.calendarRsvp(ev.eventId, inv->inviteId, 8);
                else if (act == kIdCalDecline) session_.calendarRsvp(ev.eventId, inv->inviteId, 2);
                else session_.calendarRemoveOwnInvite(ev.eventId, inv->inviteId);
            }
            playSoundByName("igMainMenuOptionCheckBoxOn");
            return true;
        }
        default: break;
    }
    // A day (CalendarDayButton_OnClick): the first event opens (PSP: again - the next one), else the day is selected.
    if (act >= kIdCalDay0 && act < kIdCalDay0 + 42) {
        const int b = static_cast<int>(act - kIdCalDay0);
        CalUi::Day& day = C.days[static_cast<size_t>(b)];
        int month = C.viewMonth + day.offset, year = C.viewYear;
        if (month < 1) {
            month = 12;
            --year;
        } else if (month > 12) {
            month = 1;
            ++year;
        }
        const bool dayChanged = day.day != C.selDay || month != C.selMonth || year != C.selYear;
        C.selDay = day.day;
        C.selMonth = month;
        C.selYear = year;
        playSoundByName("igMainMenuOptionCheckBoxOn");
        std::vector<int> viewable;
        for (size_t i = 0; i < day.entries.size(); ++i)
            if (day.entries[i].seq != 2) viewable.push_back(static_cast<int>(i));
        if (viewable.empty()) {
            if (dayChanged && C.openKind) {
                C.openKind = 0;
                closeWindow(kWinCalendarEvent);
            }
            return true;
        }
        int pick = 0;
        if (C.openButton == b && C.openKind != 0) {
            for (size_t k = 0; k < viewable.size(); ++k)
                if (viewable[k] == C.openEntry) pick = static_cast<int>((k + 1) % viewable.size());
        }
        const CalUi::Entry& e = day.entries[static_cast<size_t>(viewable[static_cast<size_t>(pick)])];
        // A calendar that came after the last draw (the rows are rebuilt by the next one): the click is dropped.
        if ((e.kind == 1 && e.index >= data.holidays.size()) || (e.kind == 2 && e.index >= data.lockouts.size()) ||
            (e.kind == 3 && e.index >= data.events.size()))
            return true;
        C.openButton = b;
        C.openEntry = viewable[static_cast<size_t>(pick)];
        C.openIndex = e.index;
        C.inviteScroll = 0;
        if (e.kind == 3) {  // CalendarOpenEvent: the server's details (CALENDAR_OPEN_EVENT when they come)
            C.openKind = 1;
            C.openEventId = data.events[e.index].eventId;
            C.waitingEvent = true;
            C.openEventVersion = session_.calendarEventVersion();
            session_.calendarRequestEvent(C.openEventId);
            if (windowOpen(kWinCalendarEvent)) closeWindow(kWinCalendarEvent);
        } else {  // a holiday / a raid lockout: CalendarViewHolidayFrame / CalendarViewRaidFrame at once
            C.openKind = e.kind == 1 ? 2 : 3;
            if (!windowOpen(kWinCalendarEvent)) openWindow(kWinCalendarEvent);
            placeNext_.erase(std::remove(placeNext_.begin(), placeNext_.end(), kWinCalendarEvent), placeNext_.end());
            autoPos_.erase(kWinCalendarEvent);
        }
        return true;
    }
    return true;
}

// --- Drawing --------------------------------------------------------------------------------------------------------------

bool WorldScreen::drawCalendarFrame(UiRenderer& ui) {
    CalUi& C = calUi();
    C.loadArt();
    if (!C.frame.pixels) {
        closeWindow(kWinCalendar);
        uiError(OWN_STR(NO_WOTLK_ART));
        return true;
    }
    if (!C.db.loaded() && !C.dbTried) {
        C.dbTried = true;
        C.db.load();
    }
    PROF_SCOPE("calendar");
    loadNpcArt(false);
    const Font& fs = kit_.font(kFontSmall);
    const Font& fn = kit_.font(kFontNormal);
    const Font& ft = kit_.font(kFontTiny);
    const float th = static_cast<float>(ft.lineHeight());
    const bool focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    const bool live = focusable && !C.ddOpen;
    auto screenRect = [&](const Rect& r) { return Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h}; };
    const auto& data = session_.calendarData();
    int ty = 0, tm = 0, td = 0;
    calendarToday(session_, ty, tm, td);
    if (C.viewMonth == 0) {
        C.viewMonth = tm;
        C.viewYear = ty;
    }

    // The day list (CalendarFrame_Update), rebuilt when the calendar, the month or a filter changes.
    const uint64_t key = (static_cast<uint64_t>(session_.calendarVersion()) << 32) |
                         (static_cast<uint64_t>((C.viewYear * 12 + C.viewMonth) & 0xFFFF) << 16) |
                         (static_cast<uint64_t>(td & 31) << 8) |
                         static_cast<uint64_t>((C.filter[0] ? 1 : 0) | (C.filter[1] ? 2 : 0) | (C.filter[2] ? 4 : 0) |
                                               (C.filter[4] ? 16 : 0));
    if (key != C.cacheKey) {
        C.cacheKey = key;
        int pm = C.viewMonth - 1, py = C.viewYear, nm = C.viewMonth + 1, ny = C.viewYear;
        if (pm < 1) {
            pm = 12;
            --py;
        }
        if (nm > 12) {
            nm = 1;
            ++ny;
        }
        const int numDays = daysInMonth(C.viewMonth, C.viewYear), prevDays = daysInMonth(pm, py);
        const int firstWeekday = weekdayOf(C.viewYear, C.viewMonth, 1);
        // CalendarFrame_Update: mod(firstWeekday - CALENDAR_FIRST_WEEKDAY, 7) days of the previous month first.
        const int prevShown = ((firstWeekday - kFirstWeekday) % 7 + 7) % 7;
        uint32_t seed = static_cast<uint32_t>(C.viewYear * 12 + C.viewMonth) * 2654435761u;
        for (int b = 0; b < 42; ++b) {
            CalUi::Day& dd = C.days[static_cast<size_t>(b)];
            dd.entries.clear();
            seed = seed * 1103515245u + 12345u;
            dd.cell = static_cast<uint8_t>((seed >> 16) & 3);  // CalendarFrame_InitDay: random(0,1) x random(0,1)
            int y = C.viewYear, m = C.viewMonth, d = 0;
            if (b < prevShown) {
                dd.offset = -1;
                d = prevDays - prevShown + 1 + b;
                y = py;
                m = pm;
            } else if (b < prevShown + numDays) {
                dd.offset = 0;
                d = b - prevShown + 1;
            } else {
                dd.offset = 1;
                d = b - prevShown - numDays + 1;
                y = ny;
                m = nm;
            }
            dd.day = d;
            const int64_t wanted = dayNumber(y, m, d);
            // CalendarGetDayEvent's rows: the holidays (by the filters: CalendarFilterType 2 battleground, 1 darkmoon,
            // 0 weekly), the raid lockouts (calendarShowLockouts), the events by time.
            for (size_t h = 0; h < data.holidays.size(); ++h) {
                const game::CalendarHoliday& hol = data.holidays[h];
                const int ft2 = static_cast<int>(hol.calendarFilterType);
                if ((ft2 == 2 && !C.filter[0]) || (ft2 == 1 && !C.filter[1]) || (ft2 == 0 && !C.filter[4])) continue;
                for (int i = 0; i < game::CalendarHoliday::kNumDates; ++i) {
                    if (hol.dates[i] == 0) continue;
                    const game::WowDate st = game::unpackWowPackedTime(hol.dates[i]);
                    const int64_t first = dayNumber(st.fullYear(), st.month, st.day);
                    if (wanted < first) continue;
                    const uint32_t hours = i < game::CalendarHoliday::kNumDurations ? hol.durations[i] : 0;
                    const int64_t n = hours > 0 ? static_cast<int64_t>((hours + 23) / 24) : 1;
                    if (wanted >= first + n) continue;
                    CalUi::Entry e;
                    e.kind = 1;
                    e.index = static_cast<uint32_t>(h);
                    e.seqDays = static_cast<int>(n);
                    e.seq = n <= 1 ? 0 : wanted == first ? 1 : wanted == first + n - 1 ? 3 : 2;
                    e.hour = st.hour;
                    e.minute = st.minute;
                    dd.entries.push_back(e);
                    break;
                }
            }
            if (C.filter[2] && data.serverTimeUnix != 0) {
                for (size_t l = 0; l < data.lockouts.size(); ++l) {
                    const std::time_t expiry = static_cast<std::time_t>(data.serverTimeUnix) +
                                               static_cast<std::time_t>(data.lockouts[l].secondsRemaining);
                    const std::tm tmv = core::localTime(expiry);
                    if (dayNumber(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday) != wanted) continue;
                    CalUi::Entry e;
                    e.kind = 2;
                    e.index = static_cast<uint32_t>(l);
                    e.hour = tmv.tm_hour;
                    e.minute = tmv.tm_min;
                    dd.entries.push_back(e);
                }
            }
            std::vector<CalUi::Entry> evs;
            dd.pending = false;
            for (size_t e2 = 0; e2 < data.events.size(); ++e2) {
                const game::WowDate& w = data.events[e2].eventTime;
                if (w.day != d || w.month != m || w.fullYear() != y) continue;
                CalUi::Entry e;
                e.kind = 3;
                e.index = static_cast<uint32_t>(e2);
                e.hour = w.hour;
                e.minute = w.minute;
                evs.push_back(e);
                for (const auto& inv : data.invites)  // CalendarGetFirstPendingInvite: an invite not answered
                    if (inv.eventId == data.events[e2].eventId && inv.status == 0) dd.pending = true;
            }
            std::stable_sort(evs.begin(), evs.end(), [](const CalUi::Entry& a, const CalUi::Entry& b2) {
                return a.hour != b2.hour ? a.hour < b2.hour : a.minute < b2.minute;
            });
            dd.entries.insert(dd.entries.end(), evs.begin(), evs.end());
            // CalendarFrame_UpdateDayTextures' textures (_CalendarFrame_GetTextureFile): the first viewable event's -
            // a holiday's Calendar_<name>Start / End / "" (one day; the default holiday's at START / END without a
            // name), a raid / dungeon event's LFGIcon-<texture> (else LFGIcon-Raid / -Dungeon) - and the first
            // holiday's ONGOING overlay when it runs more than 2 days (CALENDAR_USE_SEQUENCE_FOR_OVERLAY_TEXTURE false).
            dd.pic.clear();
            dd.picAlt.clear();
            dd.picCrop = false;
            dd.picArt = 0;
            dd.overlay.clear();
            const CalUi::Entry* firstHoliday = nullptr;
            bool decided = false;
            for (const CalUi::Entry& e : dd.entries) {
                if (e.kind == 1 && !firstHoliday) firstHoliday = &e;
                if (e.seq == 2 || decided) continue;
                decided = true;  // the first viewable event's texture, or none (a raid lockout, a meeting...)
                if (e.kind == 1) {
                    const std::string& tex = data.holidays[e.index].textureFilename;
                    static constexpr const char* kSeq[4] = {"", "start", "ongoing", "end"};
                    if (!tex.empty()) dd.pic = "calh_" + lower(tex) + kSeq[e.seq];
                    else if (e.seq == 1 || e.seq == 3) dd.pic = "calh_calendar_defaultholiday";
                } else if (e.kind == 3) {
                    const game::CalendarEvent& ev = data.events[e.index];
                    if (ev.type == 0 || ev.type == 1) {
                        dd.picAlt = ev.type == 0 ? "lfgic_raid" : "lfgic_dungeon";
                        if (ev.dungeonId > 0) {
                            pp::LfgDb& ldb = session_.lfgDb();
                            C.lfgDbUsed = true;
                            const pp::LfgDungeonRec* drec = ldb.dungeon(static_cast<uint32_t>(ev.dungeonId));
                            if (drec && ldb.str(drec->texture)[0]) dd.pic = "lfgic_" + lower(ldb.str(drec->texture));
                        }
                        if (dd.pic.empty()) dd.pic = dd.picAlt;
                    } else if (ev.type == 2) {  // UI-Calendar-Event-PVP02 for the Alliance, PVP01 for the Horde
                        dd.picArt = allianceSide(session_) ? 2 : 1;
                    } else {
                        dd.picArt = ev.type == 3 ? 3 : 4;  // MeetingIcon, UI-Calendar-Event-Other
                    }
                    dd.picCrop = true;
                }
            }
            if (firstHoliday && firstHoliday->seqDays > 2 && !data.holidays[firstHoliday->index].textureFilename.empty())
                dd.overlay = "calh_" + lower(data.holidays[firstHoliday->index].textureFilename) + "ongoing";
        }
        {
            std::vector<std::string> want;
            for (const CalUi::Day& dd : C.days)
                for (const std::string* n : {&dd.pic, &dd.picAlt, &dd.overlay})
                    if (!n->empty()) want.push_back(*n);
            C.loadPictures(want);
        }
        if (++C.logs <= 20)
            PSP_LOGI(pp::tag::Gfx, "cal_month %d-%02d events=%u holidays=%u lockouts=%u", C.viewYear, C.viewMonth,
                     static_cast<unsigned>(data.events.size()), static_cast<unsigned>(data.holidays.size()),
                     static_cast<unsigned>(data.lockouts.size()));
    }

    auto entryTitle = [&](const CalUi::Entry& e) -> std::string {  // CALENDAR_CALENDARTYPE_NAMEFORMAT
        if (e.kind == 1) {
            const game::CalendarHoliday& hol = data.holidays[e.index];
            const pp::HolRec* hr = C.db.holiday(hol.id);
            std::string name = hr ? C.db.holidayStr(hr->name) : "";
            if (e.seq == 1) name = CLIENT_FMT(CALENDAR_EVENTNAME_FORMAT_START, name);
            if (e.seq == 3) name = CLIENT_FMT(CALENDAR_EVENTNAME_FORMAT_END, name);
            return name;
        }
        if (e.kind == 2) {  // CALENDAR_EVENTNAME_FORMAT_RAID_LOCKOUT, GetDungeonNameWithDifficulty
            const game::CalendarRaidLockout& lo = data.lockouts[e.index];
            const bool raid = C.db.mapType(lo.mapId) == 2;
            static constexpr CS kRaid[4] = {CS::RAID_DIFFICULTY1, CS::RAID_DIFFICULTY2, CS::RAID_DIFFICULTY3,
                                            CS::RAID_DIFFICULTY4};
            static constexpr CS kDungeon[2] = {CS::DUNGEON_DIFFICULTY1, CS::DUNGEON_DIFFICULTY2};
            const char* diff = pp::cs(raid ? kRaid[std::min<uint32_t>(lo.difficulty, 3)]
                                           : kDungeon[std::min<uint32_t>(lo.difficulty, 1)]);
            return CLIENT_FMT(CALENDAR_EVENTNAME_FORMAT_RAID_LOCKOUT,
                              CLIENT_FMT(DUNGEON_NAME_WITH_DIFFICULTY, C.db.mapName(lo.mapId), diff));
        }
        return data.events[e.index].title;
    };
    auto entryColor = [&](const CalUi::Entry& e) -> uint32_t {  // _CalendarFrame_GetEventColor
        if (e.kind != 3) return kWhite;  // HOLIDAY / RAID: HIGHLIGHT_FONT_COLOR
        const game::CalendarEvent& ev = data.events[e.index];
        for (const auto& inv : data.invites)
            if (inv.eventId == ev.eventId) return inv.rank >= 1 ? kModerator : statusColor(inv.status);
        return ev.creatorGuid == session_.playerGuid() ? kModerator : kGold;
    };

    // The frame (the borders, the weekday and month backgrounds), the title, the month buttons, the close button.
    ui.image(C.frame, CR(0, 0, 659, 630));
    ui.textShadow(fn, CX(326.5f), CY(19) - static_cast<float>(fn.lineHeight()) * 0.5f,
                  pp::cs(kMonthNames[(C.viewMonth + 11) % 12]),
                  kWhite, Align::Center);  // CalendarMonthName (GameFontHighlightLarge)
    {
        char b[8];
        std::snprintf(b, sizeof(b), "%d", C.viewYear);
        ui.textShadow(ft, CX(324.5f), CY(36) - th * 0.5f, b, kGold, Align::Center);  // CalendarYearName
    }
    for (int k = 0; k < 2; ++k) {  // CalendarPrevMonthButton / CalendarNextMonthButton
        const Rect r = k == 0 ? CR(223.5f, 4, 32, 32) : CR(397.5f, 4, 32, 32);
        const Rect sq{r.x + (r.w - 14) * 0.5f, r.y + (r.h - 14) * 0.5f, 14, 14};
        const uint32_t id = k == 0 ? kIdCalPrev : kIdCalNext;
        const bool enabled = calMonthButtonEnabled(C.viewYear, C.viewMonth, k == 0 ? -1 : 1, session_);
        const bool f = live && enabled && winFocus_.addStop(id, sq);
        const pp::Texture& t = enabled ? (k == 0 ? C.prev : C.next) : (k == 0 ? C.prevDis : C.nextDis);
        if (t.pixels) ui.image(t, sq);
        if (f) ui.fill(sq, 0x40FFFFFFu);
    }
    {
        const Rect cr{CX(648) - 9, CY(30) - 9, 18, 18};  // CalendarCloseButton (TOPRIGHT 5,-13)
        const bool f = live && winFocus_.addStop(kIdCalClose, cr);
        const pp::Texture& tx = f && winFocus_.isPressed(kIdCalClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (tx.pixels) ui.image(tx, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    }
    // CalendarFilterFrame (106 x 20 at TOPRIGHT -44,-21): CALENDAR_FILTERS and its dropdown button (PSP: 3 px taller each
    // way - 20 units at 0.41 are 8 px, under the tiny font's line).
    {
        const Rect fr{CX(509), CY(21) - 3.0f, 106.0f * kSX, 20.0f * kSY + 6.0f};
        const bool f = live && winFocus_.addStop(kIdCalFilter, fr);
        ui.fill(fr, 0x80000000u);
        ui.textShadow(ft, fr.x + fr.w - 14.0f, fr.y + (fr.h - th) * 0.5f, CLIENT_STR(CALENDAR_FILTERS), kWhite, Align::Right);
        const Rect b{fr.x + fr.w - 12.0f, fr.y + (fr.h - 10.0f) * 0.5f, 10.0f, 10.0f};
        const pp::Texture& dd = C.ddOpen && C.ddBtnDn.pixels ? C.ddBtnDn : C.ddBtn;
        if (dd.pixels) ui.image(dd, b);
        else ui.textShadow(ft, b.x + b.w * 0.5f, fr.y + (fr.h - th) * 0.5f, "v", kGold, Align::Center);
        if (f) ui.fill(fr, 0x30FFFFFFu);
    }
    // The weekday names (CalendarWeekday1..7Name: GameFontNormal) from Monday, each in its 91-unit column.
    for (int i = 1; i <= 7; ++i) {
        const int wd = (i - 2 + kFirstWeekday) % 7 + 1;  // _CalendarFrame_GetWeekdayIndex
        const Rect col = CR(12 + 91.0f * static_cast<float>(i - 1), 46, 91, 28);
        ui.pushClip(col);
        ui.textShadow(ft, col.x + col.w * 0.5f, col.y + (col.h - th) * 0.5f, pp::cs(kWeekdayNames[wd - 1]), kGold,
                      Align::Center);
        ui.popClip();
    }
    // The day buttons (91 x 91 from 12,74).
    const uint32_t now = pp::logUptimeMs();
    for (int b = 0; b < 42; ++b) {
        const CalUi::Day& dd = C.days[static_cast<size_t>(b)];
        const float bx = 12.0f + 91.0f * static_cast<float>(b % 7), by = 74.0f + 91.0f * static_cast<float>(b / 7);
        const Rect r = CR(bx, by, 91, 91);
        const bool f = live && winFocus_.addStop(kIdCalDay0 + static_cast<uint32_t>(b), r);
        if (C.dayBg.pixels) {  // the normal texture: one of the four CalendarBackground cells
            const float cw = static_cast<float>(C.dayBg.uw) * 0.5f, ch = static_cast<float>(C.dayBg.uh) * 0.5f;
            ui.image(C.dayBg, r, SrcRect{(dd.cell & 1) ? cw : 0.0f, (dd.cell & 2) ? ch : 0.0f, cw, ch});
        }
        int month = C.viewMonth + dd.offset, year = C.viewYear;
        if (month < 1) {
            month = 12;
            --year;
        } else if (month > 12) {
            month = 1;
            ++year;
        }
        // CalendarFrame_UpdateDayEvents' first pass: the viewable events (not ONGOING).
        int viewable = 0;
        for (const auto& e : dd.entries)
            if (e.seq != 2) ++viewable;
        // BORDER: the day's picture (EventTexture, TexCoords 0..0.7109375); ARTWORK: the event background; OVERLAY:
        // the pending invite; the overlay frame: the first holiday's ongoing art.
        if (viewable > 0) {
            const pp::Texture* t = dd.pic.empty() ? nullptr : C.cached(dd.pic);
            if (!t && !dd.picAlt.empty()) t = C.cached(dd.picAlt);
            if (dd.picArt) {
                const pp::Texture* art[4] = {&C.evPvp1, &C.evPvp2, &C.evMeeting, &C.evOther};
                t = art[dd.picArt - 1]->pixels ? art[dd.picArt - 1] : nullptr;
            }
            if (t && dd.picCrop)
                ui.image(*t, r, SrcRect{0, 0, static_cast<float>(t->uw) * 0.7109375f, static_cast<float>(t->uh) * 0.7109375f});
            else if (t)
                ui.image(*t, r);  // calh_* are baked cut
            if (C.evBg.pixels) ui.image(C.evBg, Rect{r.x, r.y + r.h - 86.0f * kSY, r.w, 86.0f * kSY});
        }
        if (dd.pending && C.pending.pixels)  // $parentPendingInviteTexture (71 x 91 at CENTER)
            ui.image(C.pending, Rect{r.x + (r.w - 71.0f * kSX) * 0.5f, r.y, 71.0f * kSX, r.h});
        if (!dd.overlay.empty())
            if (const pp::Texture* t = C.cached(dd.overlay)) ui.image(*t, r);
        // The event lines (CalendarFrame_UpdateDayEvents, from the bottom up). PSP: up to 2 lines in the tiny font
        // (the day is 37 px high; the original's 4 buttons of 12 units would be 5 px each), the title only.
        {
            std::vector<const CalUi::Entry*> lines;
            for (const auto& e : dd.entries)
                if (e.seq != 2 && lines.size() < static_cast<size_t>(kMaxDayEvents)) lines.push_back(&e);
            float ly = r.y + r.h - 2.0f - th * static_cast<float>(lines.size());
            for (const CalUi::Entry* e : lines) {
                // The open event's button: LockHighlight (EventHighlight, ADD).
                const int entry = static_cast<int>(e - dd.entries.data());
                if (C.openKind != 0 && C.openButton == b && C.openEntry == entry && C.evHl.pixels)
                    ui.image(C.evHl, Rect{r.x + 1, ly, r.w - 2, th}, 0xFFFFFFFFu, Blend::Add);
                ui.pushClip(Rect{r.x + 1, ly, r.w - 2, th});
                ui.textShadow(ft, r.x + 4.0f * kSX, ly, entryTitle(*e).c_str(), entryColor(*e));
                ui.popClip();
                ly += th;
            }
        }
        // $parentDateFrame (GameFontHighlight at TOPLEFT 5,-5) and $parentMoreEventsButton (TOPRIGHT -5,-7).
        char db2[4];
        std::snprintf(db2, sizeof(db2), "%d", dd.day);
        ui.textShadow(fs, r.x + 5.0f * kSX + 1.0f, r.y + 1.0f, db2, kWhite);
        if (viewable > kMaxDayEvents && C.more.pixels) ui.image(C.more, Rect{r.x + r.w - 13, r.y + 3, 10, 6});
        // $parentDarkFrame (the previous / the next month's days; PSP: a flat shade, not the CalendarShadows edges).
        if (dd.offset != 0) ui.fill(r, 0x99000000u);
        // CalendarTodayFrame (CurrentDay with the pulsing glow) and the selection / highlight (Highlights, ADD).
        if (dd.day == td && month == tm && year == ty) {
            if (C.today.pixels)
                ui.image(C.today, Rect{r.x + (r.w - 145.0f * kSX) * 0.5f, r.y + (r.h - 140.0f * kSY) * 0.5f, 145.0f * kSX,
                                       140.0f * kSY});
            if (C.todayGlow.pixels) {
                const float a = 0.5f + 0.5f * std::cos(static_cast<float>(now % 2000u) / 2000.0f * 6.2831853f);
                ui.image(C.todayGlow,
                         Rect{r.x + (r.w - 112.0f * kSX) * 0.5f + 2.0f * kSX, r.y + (r.h - 130.0f * kSY) * 0.5f - 15.0f * kSY,
                              112.0f * kSX, 130.0f * kSY},
                         (static_cast<uint32_t>(a * 255.0f) << 24) | 0x00FFFFFFu, Blend::Add);
            }
        }
        const bool selected = dd.day == C.selDay && month == C.selMonth && year == C.selYear;
        if ((selected || f) && C.dayHl.pixels)
            ui.image(C.dayHl, r, selected ? 0xFFFFFFFFu : 0x80FFFFFFu, Blend::Add);  // SELECTION_ALPHA 1 / HIGHLIGHT 0.5
        // CalendarDayButton_OnEnter: FULLDATE, then each viewable event with its time and who invited.
        if (f && viewable > 0) {
            std::vector<WotlkTipLine> t;
            const int wd = weekdayOf(year, month, dd.day);
            const std::string fdb = CLIENT_FMT(FULLDATE, pp::cs(kWeekdayNames[wd - 1]), pp::cs(kMonthNames[month - 1]),
                                               dd.day, year);
            t.push_back(WotlkTipLine{questText(fdb), kWhite});
            for (const auto& e : dd.entries) {
                if (e.seq == 2) continue;
                t.push_back(WotlkTipLine{" ", kWhite});
                WotlkTipLine l;
                l.text = entryTitle(e);
                l.color = entryColor(e);
                l.right = formatTime(e.hour, e.minute);
                l.rightColor = kWhite;
                t.push_back(l);
                if (e.kind == 3) {  // _CalendarFrame_IsPlayerCreatedEvent: who invited
                    const game::CalendarEvent& ev = data.events[e.index];
                    const game::CalendarInvite* inv = ownInvite(data, ev.eventId);
                    const uint8_t type = calendarType(ev.flags);
                    std::string who;
                    if (ev.creatorGuid == session_.playerGuid()) {
                        who = type == kTypeAnnouncement ? CLIENT_STR(CALENDAR_ANNOUNCEMENT_CREATEDBY_YOURSELF) : type == kTypeGuildEvent ? CLIENT_STR(CALENDAR_GUILDEVENT_INVITEDBY_YOURSELF) : CLIENT_STR(CALENDAR_EVENT_INVITEDBY_YOURSELF);
                    } else if (isSignUpEvent(ev.flags, inv)) {
                        const uint8_t st = inv ? inv->status : 7;
                        who = st == 6 || st == 7
                                  ? std::string(pp::cs(kStatusNames[st]))
                                  : CLIENT_FMT(CALENDAR_SIGNEDUP_FOR_GUILDEVENT_WITH_STATUS,
                                               pp::cs(kStatusNames[std::min<uint8_t>(st, 8)]));
                    } else {
                        const std::string* n = session_.nameOf(ev.creatorGuid);
                        const std::string nm = n ? *n : std::string(CLIENT_STR(UNKNOWN));
                        who = questText(type == kTypeAnnouncement ? CLIENT_FMT(CALENDAR_ANNOUNCEMENT_CREATEDBY_PLAYER, nm)
                                                                  : CLIENT_FMT(CALENDAR_EVENT_INVITEDBY_PLAYER, nm));
                    }
                    t.push_back(WotlkTipLine{who, kGold});
                }
            }
            wotlkTip(screenRect(r), std::move(t), b % 7 >= 4 ? kTipLeft : kTipRight);
        }
    }
    // The filter's list (CalendarFilterDropDown_Initialize: check boxes, keepShownOnClick).
    if (C.ddOpen) {
        std::vector<WotlkDdItem> items(5);
        for (int k = 0; k < 5; ++k) {
            items[static_cast<size_t>(k)].text = pp::cs(kFilterNames[k]);
            items[static_cast<size_t>(k)].checked = C.filter[k];
        }
        wotlkDropDownList(ui, CX(470), CY(45), items, kIdCalFilterDd0, focusable);
    }
    return true;
}

// CalendarViewEventFrame / CalendarViewHolidayFrame / CalendarViewRaidFrame (one window at the right edge).
bool WorldScreen::drawCalendarEventFrame(UiRenderer& ui) {
    CalUi& C = calUi();
    if (C.openKind == 0 || !windowOpen(kWinCalendar)) {
        closeWindow(kWinCalendarEvent);
        return true;
    }
    PROF_SCOPE("calendar.event");
    loadNpcArt(false);
    const Font& fs = kit_.font(kFontSmall);
    const Font& ft = kit_.font(kFontTiny);
    const float lh = static_cast<float>(fs.lineHeight()), th = static_cast<float>(ft.lineHeight());
    const bool focusable = uiMode_ && !settings_.isOpen();
    auto screenRect = [&](const Rect& r) { return Rect{r.x + ui.offsetX(), r.y + ui.offsetY(), r.w, r.h}; };
    const auto& data = session_.calendarData();
    auto closeButton = [&](const Rect& frame) {  // CalendarEventCloseButtonTemplate (TOPRIGHT)
        const Rect cr{frame.x + frame.w - 20, frame.y + 2, 18, 18};
        const bool f = focusable && winFocus_.addStop(kIdCalEvClose, cr);
        const pp::Texture& tx = f && winFocus_.isPressed(kIdCalEvClose) && npcArt_.closeDown.pixels ? npcArt_.closeDown : npcArt_.close;
        if (tx.pixels) ui.image(tx, cr);
        if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, cr, 0xFFFFFFFFu, Blend::Add);
    };

    if (C.openKind == 2 || C.openKind == 3) {
        const bool holiday = C.openKind == 2;
        if ((holiday && C.openIndex >= data.holidays.size()) || (!holiday && C.openIndex >= data.lockouts.size())) {
            closeWindow(kWinCalendarEvent);
            return true;
        }
        const float hU = holiday ? 320.0f : 150.0f;
        const Rect fr = ER(0, 0, kEvW, hU);
        drawDialogBox(ui, fr);
        std::string title, desc;
        if (holiday) {  // CalendarViewHolidayFrame_Update: the name, the description, Calendar_DefaultHoliday (INFO)
            const pp::HolRec* hr = C.db.holiday(data.holidays[C.openIndex].id);
            title = hr ? C.db.holidayStr(hr->name) : "";
            desc = hr ? C.db.holidayStr(hr->desc) : "";
            if (C.holidayInfo.pixels)
                ui.image(C.holidayInfo, Rect{fr.x + 12.0f * kEX, fr.y + 13.0f * kEY, fr.w - 23.0f * kEX, fr.h - 24.0f * kEY});
        } else {  // CalendarViewRaidFrame_Update: CALENDAR_RAID_LOCKOUT_DESCRIPTION
            const game::CalendarRaidLockout& lo = data.lockouts[C.openIndex];
            const bool raid = C.db.mapType(lo.mapId) == 2;
            static constexpr CS kRaid[4] = {CS::RAID_DIFFICULTY1, CS::RAID_DIFFICULTY2, CS::RAID_DIFFICULTY3,
                                            CS::RAID_DIFFICULTY4};
            static constexpr CS kDungeon[2] = {CS::DUNGEON_DIFFICULTY1, CS::DUNGEON_DIFFICULTY2};
            title = CLIENT_FMT(DUNGEON_NAME_WITH_DIFFICULTY, C.db.mapName(lo.mapId),
                               pp::cs(raid ? kRaid[std::min<uint32_t>(lo.difficulty, 3)]
                                           : kDungeon[std::min<uint32_t>(lo.difficulty, 1)]));
            const std::time_t expiry = static_cast<std::time_t>(data.serverTimeUnix) + static_cast<std::time_t>(lo.secondsRemaining);
            const std::tm tmv = core::localTime(expiry);
            desc = CLIENT_FMT(CALENDAR_RAID_LOCKOUT_DESCRIPTION, title, formatTime(tmv.tm_hour, tmv.tm_min));
        }
        drawDialogHeader(ui, fr, title.c_str());  // CalendarTitleFrameTemplate
        closeButton(fr);
        float y = fr.y + 30.0f * kEY;
        for (const std::string& l : wrapQuestText(fs, desc, 265.0f * kEX)) {
            if (y + lh > fr.y + fr.h - 8) break;
            ui.textShadow(fs, fr.x + 15.0f * kEX, y, l.c_str(), kWhite);  // GameFontHighlightLeft
            y += lh;
        }
        return true;
    }

    // --- CalendarViewEventFrame (CalendarViewEventFrame_Update) ------------------------------------------------------
    if (!session_.calendarEventKnown() || session_.calendarEvent().eventId != C.openEventId) {
        closeWindow(kWinCalendarEvent);  // "event was probably deleted"
        return true;
    }
    const game::CalendarEventDetail& ev = session_.calendarEvent();
    const bool locked = (ev.flags & 0x10) != 0;  // CALENDAR_FLAG_INVITES_LOCKED (AzerothCore CalendarFlags)
    const uint8_t type = calendarType(ev.flags);
    const game::CalendarInvite* inv = ownInvite(data, ev.eventId);
    const bool signUp = isSignUpEvent(ev.flags, inv);
    // GUILD_ANNOUNCEMENT: no invite list section (defaultHeight - its 320).
    const Rect fr = ER(0, 0, kEvW, type == kTypeAnnouncement ? 500.0f - 320.0f : 500.0f);
    drawDialogBox(ui, fr);
    drawDialogHeader(ui, fr, type == kTypeAnnouncement ? CLIENT_STR(CALENDAR_VIEW_ANNOUNCEMENT) : type == kTypeGuildEvent ? CLIENT_STR(CALENDAR_VIEW_GUILD_EVENT) : CLIENT_STR(CALENDAR_VIEW_EVENT));
    closeButton(fr);
    // CalendarViewEventIcon (60 x 60 at TOPLEFT 16,-26): LFGIcon-<texture> of the dungeon, else the type's texture
    // (SetDesaturation when locked; PSP: a grey tint).
    {
        const Rect ic{fr.x + 16.0f * kEX, fr.y + 26.0f * kEY, 60.0f * kEY, 60.0f * kEY};
        const pp::Texture* t = nullptr;
        std::string dungeon;
        if ((ev.type == 0 || ev.type == 1) && ev.dungeonId > 0) {
            pp::LfgDb& ldb = session_.lfgDb();
            C.lfgDbUsed = true;
            if (const pp::LfgDungeonRec* d = ldb.dungeon(static_cast<uint32_t>(ev.dungeonId))) {
                // GetDungeonNameWithDifficulty (DUNGEON_NAME_WITH_DIFFICULTY "%1$s (%2$s)"): RAID_DIFFICULTY1-2 /
                // DUNGEON_DIFFICULTY1-2 by LFGDungeons' difficulty.
                static constexpr CS kRaid[2] = {CS::RAID_DIFFICULTY1, CS::RAID_DIFFICULTY2};
                static constexpr CS kDungeon[2] = {CS::DUNGEON_DIFFICULTY1, CS::DUNGEON_DIFFICULTY2};
                const uint8_t df = std::min<uint8_t>(d->difficulty, 1);
                dungeon = CLIENT_FMT(DUNGEON_NAME_WITH_DIFFICULTY, ldb.str(d->name), pp::cs(ev.type == 0 ? kRaid[df] : kDungeon[df]));
                if (ldb.str(d->texture)[0]) t = C.picture("lfgic_" + lower(ldb.str(d->texture)));
            }
        }
        if (!t) {
            switch (ev.type) {  // CALENDAR_EVENTTYPE_TEXTURES
                case 0: t = C.picture("lfgic_raid"); break;
                case 1: t = C.picture("lfgic_dungeon"); break;
                case 2: t = allianceSide(session_) ? &C.evPvp2 : &C.evPvp1; break;
                case 3: t = &C.evMeeting; break;
                default: t = &C.evOther; break;
            }
        }
        if (t && t->pixels) ui.image(*t, ic, locked ? 0xFF8C8C8Cu : 0xFFFFFFFFu);
        // The title (GameFontNormal; locked: CALENDAR_VIEW_EVENTTITLE_LOCKED - grey, then its gold "(Locked)"),
        // CalendarViewEventTypeName (CALENDAR_VIEW_EVENTTYPE "%1$s - %2$s"), CalendarViewEventCreatorName, the date
        // (FULLDATE) and the time (GameFontHighlightSmall) - each under the previous one, 220 units wide.
        float x = ic.x + ic.w + 2.0f, y = ic.y;
        const float w = fr.x + fr.w - 8.0f - x;
        ui.pushClip(Rect{x, y, w, lh + th * 4});
        if (locked) {
            ui.textShadow(fs, x, y, ev.title.c_str(), kGrey);
            // CALENDAR_VIEW_EVENTTITLE_LOCKED "|cff7f7f7f%s|r |cffffd200(Locked)|r": its words after the title
            std::string lockedWord = pp::text::uiPlain(CLIENT_FMT(CALENDAR_VIEW_EVENTTITLE_LOCKED, ""));
            while (!lockedWord.empty() && lockedWord.front() == ' ') lockedWord.erase(lockedWord.begin());
            ui.textShadow(fs, x + static_cast<float>(fs.measure(ev.title.c_str()) + fs.measure(" ")), y,
                          lockedWord.c_str(), kGold);
        } else {
            ui.textShadow(fs, x, y, ev.title.c_str(), kGold);
        }
        y += lh;
        const std::string typeName =
            std::string(pp::cs(kTypeNames[std::min<uint8_t>(ev.type, 4)])) + (dungeon.empty() ? "" : " - " + dungeon);
        ui.textShadow(ft, x, y, typeName.c_str(), locked ? kGrey : kGold);
        y += th;
        const std::string* cn = session_.nameOf(ev.creatorGuid);
        const std::string creator = CLIENT_FMT(CALENDAR_EVENT_CREATORNAME, cn ? *cn : std::string(CLIENT_STR(UNKNOWN)));
        ui.textShadow(ft, x, y, creator.c_str(), locked ? kGrey : kGold);
        y += th;
        const game::WowDate& d = ev.eventTime;
        const int wd = weekdayOf(d.fullYear(), d.month, d.day);
        const std::string fdb = CLIENT_FMT(FULLDATE, pp::cs(kWeekdayNames[wd - 1]), pp::cs(kMonthNames[(d.month + 11) % 12]),
                                           d.day, d.fullYear());  // _CalendarFrame_GetFullDate
        ui.textShadow(ft, x, y, questText(fdb).c_str(), kWhite);
        y += th;
        ui.textShadow(ft, x, y, formatTime(d.hour, d.minute).c_str(), kWhite);
        ui.popClip();
    }
    // CalendarViewEventDescriptionContainer (287 x 65 at TOPLEFT 16,-95; GameFontNormalSmallLeft).
    {
        const Rect dr{fr.x + 16.0f * kEX, fr.y + 95.0f * kEY, 287.0f * kEX, 65.0f * kEY};
        if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{dr.x + 2, dr.y + 2, dr.w - 4, dr.h - 4}, 0xC0FFFFFFu);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), dr);
        float y = dr.y + 3.0f;
        ui.pushClip(Rect{dr.x + 3, dr.y + 2, dr.w - 6, dr.h - 4});
        for (const std::string& l : wrapQuestText(ft, ev.description, dr.w - 8.0f)) {
            ui.text(ft, dr.x + 4, y, l.c_str(), locked ? kGrey : kGold);
            y += th;
        }
        ui.popClip();
    }
    if (type == kTypeAnnouncement) return true;  // "guild wide events don't have invite lists"
    // CalendarViewEventInviteListSection (TOPLEFT 0,-170): the RSVP buttons, the divider, the invite list.
    const float sy = 170.0f;
    {
        int ty = 0, tm = 0, td = 0;
        calendarToday(session_, ty, tm, td);
        const game::WowDate& d = ev.eventTime;
        const bool todayOrLater = dayNumber(d.fullYear(), d.month, d.day) >= dayNumber(ty, tm, td);  // _IsTodayOrLater
        const uint8_t st = inv ? inv->status : 7;  // CALENDAR_INVITESTATUS_NOT_SIGNEDUP without an invite of ours
        bool accept = false, tentative = false, decline = false, remove = true;  // Remove: the XML's enabled default
        if (signUp) {  // CalendarViewEventRSVP_Update, sign up mode
            accept = tentative = !locked && todayOrLater && st == 7;
            remove = todayOrLater && st != 7;
        } else if (!locked && todayOrLater && inv &&
                   (st == 0 || st == 1 || st == 2 || st == 6 || st == 7 || st == 8)) {  // _CalendarFrame_CanInviteeRSVP
            accept = st != 1;
            tentative = st != 8;
            decline = st != 2;
        }
        // PSP: the own events open this frame too (no CalendarCreateEventFrame): Remove only removes an invite of ours
        // that is not the creator's (_CalendarFrame_CanRemoveEvent).
        if (!inv || inv->rank == 2) remove = false;
        const float bw = signUp ? 94.0f : 128.0f, bh = 22.0f;
        auto rsvp = [&](uint32_t id, float x, float y, const char* label, bool enabled, const char* tip) {
            const Rect r = ER(x, y, bw, bh);
            wotlkButton(ui, id, r, label, enabled, focusable);
            if (focusable && enabled && winFocus_.focused() == id)
                wotlkTip(screenRect(r), {WotlkTipLine{tip, kWhite}}, kTipLeft);  // the buttons' OnEnter (ANCHOR_LEFT)
        };
        if (signUp) {  // TOPLEFT 14,0 / TOP / TOPRIGHT -14,0 at 94 units
            rsvp(kIdCalAccept, 14, sy, CLIENT_STR(CALENDAR_SIGNUP), accept, CLIENT_STR(CALENDAR_TOOLTIP_SIGNUPBUTTON));
            rsvp(kIdCalTentative, (kEvW - bw) * 0.5f, sy, CLIENT_STR(CALENDAR_VIEW_EVENT_TENTATIVE), tentative, CLIENT_STR(CALENDAR_TOOLTIP_TENTATIVEBUTTON));
            rsvp(kIdCalRemove, kEvW - 14 - bw, sy, CLIENT_STR(CALENDAR_VIEW_EVENT_REMOVE), remove, CLIENT_STR(CALENDAR_TOOLTIP_REMOVESIGNUPBUTTON));
        } else {  // TOPRIGHT at TOP -10,4 / TOPLEFT at TOP 10,4 / TOPRIGHT at TOP -10,-26 / TOPLEFT at TOP 10,-26
            rsvp(kIdCalAccept, kEvW * 0.5f - 10 - bw, sy - 4, CLIENT_STR(ACCEPT), accept, CLIENT_STR(CALENDAR_TOOLTIP_AVAILABLEBUTTON));
            rsvp(kIdCalTentative, kEvW * 0.5f + 10, sy - 4, CLIENT_STR(CALENDAR_VIEW_EVENT_TENTATIVE), tentative, CLIENT_STR(CALENDAR_TOOLTIP_TENTATIVEBUTTON));
            rsvp(kIdCalDecline, kEvW * 0.5f - 10 - bw, sy + 26, CLIENT_STR(DECLINE), decline, CLIENT_STR(CALENDAR_TOOLTIP_DECLINEBUTTON));
            rsvp(kIdCalRemove, kEvW * 0.5f + 10, sy + 26, CLIENT_STR(CALENDAR_VIEW_EVENT_REMOVE), remove, CLIENT_STR(CALENDAR_TOOLTIP_REMOVEBUTTON));
        }
        // CalendarViewEventDivider (UI-DialogBox-Divider 303 x 16 at TOPLEFT 10,-50 / -30; PSP: a line).
        const Rect dv = ER(18, sy + (signUp ? 30.0f : 50.0f) + 7.0f, 287, 2);
        ui.fill(Rect{dv.x, dv.y, dv.w, 1.0f}, 0x60FFFFFFu);
    }
    // CalendarViewEventInviteList (CalendarEventInviteListTemplate 294 wide at TOP 0,-80 / -60, 230 / 250 high): the
    // name (CALENDAR_INVITELIST_CREATORNAME / _MODERATORNAME) and the status in its colour. PSP: 20-unit rows (14 are
    // 7 px here), no class column / sort buttons (no class on the wire).
    {
        const float listTop = sy + (signUp ? 60.0f : 80.0f), listH = signUp ? 250.0f : 230.0f;
        const int rows = std::max(1, static_cast<int>((listH - 8.0f) / kInviteRowU));
        const Rect lr = ER(13, listTop, 294, listH);
        if (kit_.has(kTexTipBg)) ui.tile(kit_.tex(kTexTipBg), Rect{lr.x + 2, lr.y + 2, lr.w - 4, lr.h - 4}, 0xC0FFFFFFu);
        if (kit_.has(kTexTipBorder)) ui.nineSlice(kit_.tex(kTexTipBorder), lr);
        const int n = static_cast<int>(ev.invitees.size());
        C.inviteScroll = std::clamp(C.inviteScroll, 0, std::max(0, n - rows));
        for (int v = 0; v < rows; ++v) {
            const int i = C.inviteScroll + v;
            if (i >= n) break;
            const game::CalendarEventInvitee& iv = ev.invitees[static_cast<size_t>(i)];
            const Rect rr = ER(16, listTop + 4 + kInviteRowU * static_cast<float>(v), 260, kInviteRowU);
            const bool f = focusable && winFocus_.addStop(kIdCalInv0 + static_cast<uint32_t>(v), rr);
            if (f && npcArt_.titleHl.pixels) ui.image(npcArt_.titleHl, rr, 0x80FFFFFFu, Blend::Add);
            const std::string* nm = session_.nameOf(iv.guid);
            std::string name = nm ? *nm : std::string(CLIENT_STR(UNKNOWN));
            if (iv.rank == 2) name = CLIENT_FMT(CALENDAR_INVITELIST_CREATORNAME, name);
            else if (iv.rank == 1) name = CLIENT_FMT(CALENDAR_INVITELIST_MODERATORNAME, name);
            ui.pushClip(Rect{rr.x, rr.y, rr.w * 0.6f, rr.h});
            ui.textShadow(ft, rr.x + 2, rr.y + (rr.h - th) * 0.5f, name.c_str(), kGold);
            ui.popClip();
            const uint8_t st = std::min<uint8_t>(iv.status, 8);
            ui.textShadow(ft, rr.x + rr.w - 2, rr.y + (rr.h - th) * 0.5f, pp::cs(kStatusNames[st]), statusColor(st),
                          Align::Right);
            if (f) {  // CalendarEventInviteListButton_OnEnter (PSP: the name, UNIT_LEVEL_TEMPLATE and the status)
                const std::string b = CLIENT_FMT(UNIT_LEVEL_TEMPLATE, iv.level);
                wotlkTip(screenRect(rr),
                         {WotlkTipLine{nm ? *nm : std::string(CLIENT_STR(UNKNOWN)), kWhite}, WotlkTipLine{b, kWhite},
                          WotlkTipLine{pp::cs(kStatusNames[st]), statusColor(st)}},
                         kTipLeft);
            }
        }
        if (n > rows) {  // the HybridScrollFrame's arrows
            const bool canUp = C.inviteScroll > 0, canDn = C.inviteScroll + rows < n;
            const Rect up{lr.x + lr.w - 13, lr.y + 3, 11, 11}, dn{lr.x + lr.w - 13, lr.y + lr.h - 14, 11, 11};
            const pp::Texture& tu = canUp ? npcArt_.scrUp : npcArt_.scrUpDis;
            const pp::Texture& td2 = canDn ? npcArt_.scrDown : npcArt_.scrDownDis;
            if (tu.pixels) ui.image(tu, up);
            if (td2.pixels) ui.image(td2, dn);
            if (focusable && canUp && winFocus_.addStop(kIdCalInvUp, up)) ui.fill(up, 0x40FFFFFFu);
            if (focusable && canDn && winFocus_.addStop(kIdCalInvDown, dn)) ui.fill(dn, 0x40FFFFFFu);
        }
    }
    return true;
}

// StaticPopupDialogs["CALENDAR_ERROR"]: the message, OKAY (hideOnEscape).
bool WorldScreen::drawCalendarPopup(UiRenderer& ui) {
    if (!cal_ || cal_->errorText.empty()) {
        closeWindow(kWinWotlkPopup);
        return true;
    }
    StaticPopup p;
    p.text = cal_->errorText.c_str();
    p.button1 = CLIENT_STR(OKAY);
    p.button1Id = kIdCalErrOk;
    p.focusable = uiMode_ && !settings_.isOpen();
    drawStaticPopup(ui, p);
    return true;
}

}  // namespace wowee::ui::psp
