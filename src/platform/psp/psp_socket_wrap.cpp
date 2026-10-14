// Link-time wrapper around connect() (-Wl,--wrap=connect), PSP build only.
//
// Upstream's TCPSocket/WorldSocket start a non-blocking connect and accept
// only EINPROGRESS as "in progress" (include/network/net_platform.hpp). The
// PSP socket layer (seen on PPSSPP) reports EAGAIN for the same condition, so
// the connect was treated as failed. Mapping it here keeps upstream untouched.
// Every call logs the raw errno (connects are rare) so hardware behaviour
// is on record next to the emulator's: PPSSPP reports raw_errno=11.

#include "platform/psp/psp_log.hpp"
#include "platform/psp/psp_public_safe.hpp"  // PSP: the address actually connected to

#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>

extern "C" int __real_connect(int s, const struct sockaddr* addr, socklen_t len);

extern "C" int __wrap_connect(int s, const struct sockaddr* addr, socklen_t len) {
    // PSP (public safe): the logon / realm address the dev-server decision is made on - the one used here,
    // not the cfg text (a DNS name can point anywhere).
    uint32_t ip = 0;
    if (addr && len >= static_cast<socklen_t>(sizeof(sockaddr_in)) && addr->sa_family == AF_INET)
        ip = ntohl(reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr);
    wowee::platform::psp::publicSafeNoteConnect(ip);
    errno = 0;
    int r = __real_connect(s, addr, len);
    const int raw = errno;
    if (r < 0 && (raw == EAGAIN || raw == EWOULDBLOCK)) errno = EINPROGRESS;
    PSP_LOGI(wowee::platform::psp::tag::Net, "connect fd=%d r=%d raw_errno=%d errno=%d", s, r, raw, errno);
    return r;
}
