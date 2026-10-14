// PSP: "public safe" - psp_public_safe.hpp.
#include "platform/psp/psp_public_safe.hpp"

#include "platform/psp/psp_config.hpp"
#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_public_safe_core.hpp"

#include <string>

namespace wowee::platform::psp {

namespace {

namespace ps = pubsafe;

ps::DevHosts g_hosts;
bool g_hostsRead = false;
bool g_booted = false;
ps::State g_state = ps::State::Unknown;
uint32_t g_lastConnectIp = 0;

void readHosts() {
    if (g_hostsRead) return;
    g_hostsRead = true;
    const std::string list = configGet("devhosts", ps::kDefaultDevHosts);
    g_hosts = ps::parseDevHosts(list.c_str());
    PSP_LOGI(tag::Net, "public_safe_devhosts n=%d ignored_public=%d ignored_invalid=%d", g_hosts.count,
             g_hosts.ignoredPublic, g_hosts.ignoredInvalid);
}

void record(const char* at, uint32_t ip, bool dev, ps::Reason why) {
    const ps::State before = g_state;
    g_state = ps::latch(g_state, dev);
    char ipText[16];
    ps::formatIpv4(ip, ipText, sizeof(ipText));
    PSP_LOGI(tag::Net, "public_safe on=%d host=%s reason=%s at=%s", g_state == ps::State::Dev ? 0 : 1,
             ip ? ipText : "-", before == ps::State::Public && dev ? "latched" : ps::reasonName(why), at);
    logFlush();
}

}  // namespace

void publicSafeBoot(const char* serverOverride) {
    if (g_booted) return;  // the autopilot judges its own server= first (autotestBoot), main_psp.cpp after it
    g_booted = true;
    readHosts();
    const std::string server = serverOverride ? std::string(serverOverride) : configGet("server");
    uint32_t ip = 0;
    if (!ps::bootLiteral(server.c_str(), ip)) {
        PSP_LOGI(tag::Net, "public_safe on=1 host=- reason=%s at=boot", ps::reasonName(ps::Reason::NameAtBoot));
        return;  // stays Unknown: the logon connect decides
    }
    ps::Reason why = ps::Reason::Unresolved;
    const bool dev = ps::decide(g_hosts, ip, why);
    record("boot", ip, dev, why);
}

void publicSafeNoteConnect(uint32_t ipHostOrder) { g_lastConnectIp = ipHostOrder; }

void publicSafeOnConnect(const char* at, const char* hostText) {
    readHosts();
    const uint32_t ip = g_lastConnectIp;
    g_lastConnectIp = 0;
    ps::Reason why = ps::Reason::Unresolved;
    const bool dev = ps::decide(g_hosts, ip, why);
    record(at, ip, dev, why);
    (void)hostText;  // the name is in the connect log line already; the decision is by address only
}

bool devToolsAllowed() { return g_state == ps::State::Dev; }
bool publicSafeLocked() { return g_state == ps::State::Public; }

}  // namespace wowee::platform::psp
