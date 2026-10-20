#include "platform/psp/psp_net.hpp"

#include "platform/psp/psp_log.hpp"

#include <pspkerneltypes.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspthreadman.h>
#include <psputility.h>
#include <psputility_modules.h>
#include <psputility_netparam.h>
#include <pspwlan.h>

#include <cstdio>
#include <cstring>

namespace wowee::platform::psp {

namespace {

constexpr uint64_t kConnectTimeoutUs = 60ull * 1000 * 1000;

NetState g_state = NetState::Off;
bool g_modulesLoaded = false;
bool g_libsUp = false;
int g_handler = -1;
int g_profile = 0;
int g_lastApState = -1;
volatile int g_lastApError = 0;
uint64_t g_connectStart = 0;
char g_status[48] = "off";
char g_ssid[40] = "";
char g_ip[20] = "";
uint32_t g_lastError = 0;

void setStatus(const char* s) { std::snprintf(g_status, sizeof(g_status), "%s", s); }

void apctlHandler(int oldState, int newState, int event, int error, void*) {
    (void)oldState;
    (void)newState;
    (void)event;
    if (error) g_lastApError = error;
}

bool loadModules() {
    if (g_modulesLoaded) return true;
    int r1 = sceUtilityLoadModule(PSP_MODULE_NET_COMMON);
    int r2 = sceUtilityLoadModule(PSP_MODULE_NET_INET);
    // 0x80111102: already loaded - fine.
    bool ok = (r1 >= 0 || r1 == static_cast<int>(0x80111102)) && (r2 >= 0 || r2 == static_cast<int>(0x80111102));
    PSP_LOGI(tag::Net, "load_modules common=%08X inet=%08X ok=%d", static_cast<unsigned>(r1),
             static_cast<unsigned>(r2), ok ? 1 : 0);
    g_modulesLoaded = ok;
    return ok;
}

bool initLibs() {
    if (g_libsUp) return true;
    int r = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (r < 0 && r != static_cast<int>(0x80110001)) {  // already initialised
        PSP_LOGE(tag::Net, "sceNetInit err=%08X", static_cast<unsigned>(r));
        return false;
    }
    r = sceNetInetInit();
    if (r < 0) PSP_LOGW(tag::Net, "sceNetInetInit err=%08X", static_cast<unsigned>(r));
    r = sceNetApctlInit(0x1800, 48);
    if (r < 0) PSP_LOGW(tag::Net, "sceNetApctlInit err=%08X", static_cast<unsigned>(r));
    r = sceNetResolverInit();
    if (r < 0) PSP_LOGW(tag::Net, "sceNetResolverInit err=%08X", static_cast<unsigned>(r));
    g_handler = sceNetApctlAddHandler(apctlHandler, nullptr);
    g_libsUp = true;
    return true;
}

// a connection still under way (the login cancelled at the Wi-Fi stage) makes the next connect
// fail with 80410A04 until it is dropped - drop it first, wait up to 1 s for "disconnected".
void apctlReset() {
    int st = 0;
    if (sceNetApctlGetState(&st) == 0 && st != PSP_NET_APCTL_STATE_DISCONNECTED) {
        PSP_LOGI(tag::Net, "apctl_reset state=%d", st);
        sceNetApctlDisconnect();
        for (int i = 0; i < 50 && sceNetApctlGetState(&st) == 0 && st != PSP_NET_APCTL_STATE_DISCONNECTED; ++i)
            sceKernelDelayThread(20000);
    }
}

// Logs every saved profile and picks one: the SSID match when asked for,
// else the first valid profile. The first valid one is not necessarily the
// network in range - a PSP-3000 run once picked an old "PLAYSTATION"
// profile and failed with 80211_NO_BSS.
int chooseProfile(const char* wantSsid) {
    int first = 0, match = 0;
    for (int i = 1; i <= 10; ++i) {
        if (sceUtilityCheckNetParam(i) != 0) continue;
        netData d;
        char ssid[40] = "";
        if (sceUtilityGetNetParam(i, PSP_NETPARAM_SSID, &d) == 0) std::snprintf(ssid, sizeof(ssid), "%.39s", d.asString);
        PSP_LOGI(tag::Net, "profile index=%d ssid=%s", i, ssid);
        if (!first) first = i;
        if (!match && wantSsid && *wantSsid && std::strcmp(ssid, wantSsid) == 0) match = i;
    }
    if (wantSsid && *wantSsid && !match) PSP_LOGW(tag::Net, "profile_ssid_not_saved ssid=%s", wantSsid);
    return match ? match : first;
}

} // namespace

const char* netErrorName(uint32_t code) {
    switch (code) {
        case 0x80410106: return "80211_NO_BSS";           // SSID not in range
        case 0x80410107: return "80211_NO_AVAIL_BSS";
        case 0x80410502: return "DHCP_NO_SERVER";
        case 0x80410505: return "DHCP_GET_NAK";
        case 0x80410A05: return "APCTL_NOT_IN_BSS";
        case 0x80410A06: return "APCTL_WLAN_SWITCH_OFF";
        case 0x80410A07: return "APCTL_WLAN_BEACON_LOST";
        case 0x80410A08: return "APCTL_WLAN_DISASSOCIATION";  // often a wrong key
        case 0x80410A09: return "APCTL_ID_NOT_FOUND";
        case 0x80410A0A: return "APCTL_WLAN_SUSPENDED";
        case 0x80410A0B: return "APCTL_TIMEOUT";
        case 0x80410D17: return "WLAN_INHIBITED";          // clock too high on stock fw
        default: return "";
    }
}

uint32_t netLastError() { return g_lastError; }

bool netStart(int profile, const char* ssid) {
    g_lastApError = 0;
    g_ssid[0] = g_ip[0] = '\0';
    if (sceWlanGetSwitchState() == 0) {
        g_state = NetState::NoSwitch;
        setStatus("wlan_switch_off");
        PSP_LOGW(tag::Net, "wlan_switch=off");
        return false;
    }
    if (!loadModules() || !initLibs()) {
        g_state = NetState::Failed;
        setStatus("init_failed");
        return false;
    }
    g_lastError = 0;
    g_profile = profile > 0 ? profile : chooseProfile(ssid);
    if (g_profile <= 0 || sceUtilityCheckNetParam(g_profile) != 0) {
        g_state = NetState::NoProfile;
        setStatus("no_profile");
        PSP_LOGW(tag::Net, "profile=none requested=%d", profile);
        return false;
    }
    netData d;
    if (sceUtilityGetNetParam(g_profile, PSP_NETPARAM_SSID, &d) == 0) {
        std::snprintf(g_ssid, sizeof(g_ssid), "%.39s", d.asString);
    }
    apctlReset();
    int r = sceNetApctlConnect(g_profile);
    PSP_LOGI(tag::Net, "apctl_connect profile=%d ssid=%s r=%08X", g_profile, g_ssid, static_cast<unsigned>(r));
    if (r < 0) {
        g_state = NetState::Failed;
        std::snprintf(g_status, sizeof(g_status), "connect_err=%08X", static_cast<unsigned>(r));
        return false;
    }
    g_state = NetState::Connecting;
    g_lastApState = -1;
    g_connectStart = sceKernelGetSystemTimeWide();
    setStatus("joining");
    return true;
}

void netPoll() {
    if (g_state != NetState::Connecting) return;
    int st = 0;
    if (sceNetApctlGetState(&st) < 0) return;
    if (st != g_lastApState) {
        static const char* const kNames[] = {"disconnected", "scanning", "joining", "getting_ip",
                                             "got_ip", "eap_auth", "key_exchange"};
        const char* name = (st >= 0 && st <= 6) ? kNames[st] : "unknown";
        PSP_LOGI(tag::Net, "apctl_state=%s", name);
        setStatus(name);
        g_lastApState = st;
    }
    if (st == PSP_NET_APCTL_STATE_GOT_IP) {
        union SceNetApctlInfo info;
        if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) {
            std::snprintf(g_ip, sizeof(g_ip), "%s", info.ip);
        }
        g_state = NetState::Connected;
        uint32_t ms = static_cast<uint32_t>((sceKernelGetSystemTimeWide() - g_connectStart) / 1000u);
        PSP_LOGI(tag::Net, "connected ip=%s ms=%u", g_ip, static_cast<unsigned>(ms));
        return;
    }
    bool timedOut = sceKernelGetSystemTimeWide() - g_connectStart > kConnectTimeoutUs;
    if (g_lastApError || timedOut) {
        g_lastError = static_cast<uint32_t>(g_lastApError);
        std::snprintf(g_status, sizeof(g_status), "err=%08X %s%s", static_cast<unsigned>(g_lastError),
                      netErrorName(g_lastError), timedOut ? " timeout" : "");
        PSP_LOGE(tag::Net, "apctl_failed %s", g_status);
        sceNetApctlDisconnect();
        g_state = NetState::Failed;
    }
}

bool netPrepare() {
    g_lastApError = 0;
    g_lastError = 0;
    g_ssid[0] = g_ip[0] = '\0';
    if (sceWlanGetSwitchState() == 0) {
        g_state = NetState::NoSwitch;
        setStatus("wlan_switch_off");
        PSP_LOGW(tag::Net, "wlan_switch=off");
        return false;
    }
    if (!loadModules() || !initLibs()) {
        g_state = NetState::Failed;
        setStatus("init_failed");
        return false;
    }
    apctlReset();
    g_state = NetState::Off;
    setStatus("dialog");
    return true;
}

bool netApConnected() {
    if (!g_libsUp) return false;
    int st = 0;
    return sceNetApctlGetState(&st) == 0 && st == PSP_NET_APCTL_STATE_GOT_IP;
}

void netAdoptConnection(const char* via) {
    union SceNetApctlInfo info;
    g_ssid[0] = g_ip[0] = '\0';
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID, &info) == 0)  // ssid[32] need not end in a NUL
        std::snprintf(g_ssid, sizeof(g_ssid), "%.32s", reinterpret_cast<const char*>(info.ssid));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) std::snprintf(g_ip, sizeof(g_ip), "%s", info.ip);
    g_profile = 0;
    g_lastError = 0;
    g_state = NetState::Connected;
    setStatus("got_ip");
    PSP_LOGI(tag::Net, "connected ip=%s ssid=%s via=%s", g_ip, g_ssid, via ? via : "");
}

void netStop() {
    if (!g_libsUp) return;
    sceNetApctlDisconnect();
    if (g_handler >= 0) sceNetApctlDelHandler(g_handler);
    sceNetResolverTerm();
    sceNetApctlTerm();
    sceNetInetTerm();
    sceNetTerm();
    g_libsUp = false;
    g_handler = -1;
    g_state = NetState::Off;
    setStatus("off");
    PSP_LOGI(tag::Net, "stopped");
}

NetState netState() { return g_state; }
const char* netStatus() { return g_status; }
const char* netSsid() { return g_ssid; }
const char* netIp() { return g_ip; }
int netProfile() { return g_profile; }

} // namespace wowee::platform::psp
