// The instance dialogs of the original (StaticPopup.lua / UIParent.lua 3.3.5a):
//   INSTANCE_BOOT (UIParent INSTANCE_BOOT_START / _STOP: SMSG_RAID_GROUP_ONLY) - INSTANCE_BOOT_TIMER with the seconds
//     (< 60: SECONDS, else the minutes rounded up, MINUTES, as StaticPopup_OnUpdate), no buttons, not closed by START;
//   INSTANCE_LOCK (INSTANCE_LOCK_START: SMSG_INSTANCE_LOCK_WARNING_QUERY) - INSTANCE_LOCK_TIMER (or
//     _PREVIOUSLY_SAVED) with the dungeon's name and SecondsToTime(ceil(left), nil, 1), ACCEPT /
//     INSTANCE_LEAVE -> RespondInstanceLock; at the timeout it only hides (the server binds).
// The session keeps the timers (psp_world_session_transfer.cpp); this file puts the dialogs in the window stack
// (ids 0xD0 / 0xD1, buttons 0x7D00..) and draws them with drawStaticPopup (ui_world_popup.cpp).
// PSP: no "bosses killed: %d/%d" line under INSTANCE_LOCK - the encounter total comes from DungeonEncounter.dbc,
// which the PSP does not carry (the server's mask of completed ones is kept: instanceLockCompletedMask()).
#include "ui/psp/ui_world_screen.hpp"

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_strings.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace wowee::ui::psp {

namespace pp = wowee::platform::psp;

namespace {

enum : uint32_t { kIdLockAccept = 0x7D00, kIdLockLeave = 0x7D01 };

// GlobalStrings.lua through strings.pak: INSTANCE_BOOT_TIMER, INSTANCE_LOCK_TIMER(_PREVIOUSLY_SAVED)
// (%1$s / %2$s), ACCEPT, INSTANCE_LEAVE; the |c..|r colour code is dropped (the popup text is drawn in one colour)
// and the enUS plural markup resolved (text::plural, psp_text_format.hpp).

// UIParent.lua SecondsToTime(seconds, nil, notAbbreviated = 1): D_DAYS / D_HOURS / D_MINUTES / D_SECONDS.
std::string secondsToTime(uint32_t s) {
    std::string out;
    int count = 0;
    auto add = [&](platform::psp::CS form, uint32_t v) {
        if (!out.empty()) out += ' ';
        out += platform::psp::csf(form, static_cast<unsigned>(v));
        ++count;
    };
    using platform::psp::CS;
    if (s >= 86400) {
        add(CS::D_DAYS, s / 86400);
        s %= 86400;
    }
    if (s >= 3600) {
        add(CS::D_HOURS, s / 3600);
        s %= 3600;
    }
    if (count < 2 && s >= 60) {
        add(CS::D_MINUTES, s / 60);
        s %= 60;
    }
    if (count < 2 && s > 0) add(CS::D_SECONDS, s);
    return out;
}

std::string g_popupText;  // the text drawStaticPopup reads this frame (one popup of these on screen at a time)

}  // namespace

void WorldScreen::updateInstancePopups() {
    // INSTANCE_BOOT_START / _STOP.
    const bool boot = session_.instanceBootSecondsLeft() > 0.0f;
    if (boot && !windowOpen(kWinInstanceBoot)) {
        openWindow(kWinInstanceBoot);
        PSP_LOGI(pp::tag::Ctrl, "instance_boot_popup secs=%.0f", static_cast<double>(session_.instanceBootSecondsLeft()));
    } else if (!boot && windowOpen(kWinInstanceBoot)) {
        closeWindow(kWinInstanceBoot);
    }
    // INSTANCE_LOCK_START / _STOP.
    const bool lock = session_.instanceLockSecondsLeft() > 0.0f;
    if (lock && !windowOpen(kWinInstanceLock)) {
        openWindow(kWinInstanceLock);
        if (uiMode_) winFocus_.setFocus(kIdLockAccept);
        PSP_LOGI(pp::tag::Ctrl, "instance_lock_popup secs=%.0f saved=%d", static_cast<double>(session_.instanceLockSecondsLeft()),
                 session_.instanceLockPreviouslySaved() ? 1 : 0);
    } else if (!lock && windowOpen(kWinInstanceLock)) {
        closeWindow(kWinInstanceLock);
    }
    // PSP: the cursor comes up (SELECT) with the dialog on top - the focus on ACCEPT (as PARTY_INVITE).
    if (uiMode_ && !instanceRaisedUi_ && lock && !windows_.empty() && windows_.back() == kWinInstanceLock)
        winFocus_.setFocus(kIdLockAccept);
    instanceRaisedUi_ = uiMode_;
}

bool WorldScreen::drawInstancePopup(UiRenderer& ui, uint8_t w) {
    if (w != kWinInstanceBoot && w != kWinInstanceLock) return false;
    StaticPopup sp;
    std::string buf;
    if (w == kWinInstanceBoot) {
        const uint32_t left = static_cast<uint32_t>(std::ceil(session_.instanceBootSecondsLeft()));
        if (left < 60) buf = CLIENT_FMT(INSTANCE_BOOT_TIMER, static_cast<int>(left), CLIENT_STR(SECONDS));
        else buf = CLIENT_FMT(INSTANCE_BOOT_TIMER, static_cast<int>((left + 59) / 60), CLIENT_STR(MINUTES));
    } else {
        pp::WorldSession::MapInfo m;
        pp::WorldSession::mapInfo(session_.worldEntry().mapId, m);
        const uint32_t left = static_cast<uint32_t>(std::ceil(session_.instanceLockSecondsLeft()));
        buf = pp::text::uiPlain(session_.instanceLockPreviouslySaved()
                                    ? CLIENT_FMT(INSTANCE_LOCK_TIMER_PREVIOUSLY_SAVED, m.name, secondsToTime(left))
                                    : CLIENT_FMT(INSTANCE_LOCK_TIMER, m.name, secondsToTime(left)));
        sp.button1 = CLIENT_STR(ACCEPT);
        sp.button1Id = kIdLockAccept;
        sp.button2 = CLIENT_STR(INSTANCE_LEAVE);
        sp.button2Id = kIdLockLeave;
    }
    g_popupText = pp::text::plural(buf);
    sp.text = g_popupText.c_str();
    sp.focusable = uiMode_ && !settings_.isOpen() && !windowOpen(kWinBindConfirm);
    drawStaticPopup(ui, sp);
    return true;
}

bool WorldScreen::instancePopupAction(uint32_t act) {
    if (act != kIdLockAccept && act != kIdLockLeave) return false;
    session_.respondInstanceLock(act == kIdLockAccept);  // RespondInstanceLock(true / false)
    if (windowOpen(kWinInstanceLock)) closeWindow(kWinInstanceLock);
    return true;
}

}  // namespace wowee::ui::psp
