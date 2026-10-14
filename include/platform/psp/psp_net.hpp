// PSP Wi-Fi bring-up: loads the network modules, connects to a saved
// network profile (Settings > Network Settings on the XMB) without a system
// dialog, and is polled once per frame so the UI keeps drawing while the
// access point is joined.
//
// The usual pattern (pspdev 15.2): sceUtilityLoadModule (not the deprecated
// LoadNetModule), an apctl
// handler for the real error codes, sceNetApctlConnect(profile) and polling
// for GOT_IP. Notes that cost time on other ports:
//  - stock firmware blocks WLAN above 222/266 MHz (0x80410D17); ARK lifts it;
//  - the PSP does not notice lost beacons - reconnect = disconnect first;
//  - threads made later have no cwd: pass absolute paths.
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

enum class NetState : uint8_t {
    Off,
    NoSwitch,     // WLAN switch is off
    NoProfile,    // no saved network profile
    Connecting,   // scanning / joining / getting IP
    Connected,    // got an IP
    Failed,
};

// profile: 1..N = that saved profile. 0 = pick by `ssid` when given, else the
// first valid profile. Every saved profile (index, SSID) is logged first.
bool netStart(int profile, const char* ssid = nullptr);
// Advances the connection state machine. Cheap; call every frame.
void netPoll();
void netStop();

NetState netState();
// Short English status for the log ("joining", "got_ip", "err=80410D17").
const char* netStatus();
const char* netSsid();
const char* netIp();
int netProfile();
// Last apctl error code (0 = none) and its SDK name, e.g.
// 0x80410106 -> "80211_NO_BSS" (the firmware's network error names).
uint32_t netLastError();
const char* netErrorName(uint32_t code);

} // namespace wowee::platform::psp
