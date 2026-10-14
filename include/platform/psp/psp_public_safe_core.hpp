// PSP: the "public safe" decision (docs/PSP_PORT_ARCHITECTURE.md) - header-only, no PSP calls, so the host test
// (tools/psp/host_tests/public_safe_host_test.sh) runs the same code.
//
// The debug and automation aids (GM window, devcmd.txt, the autopilot, autopad, the auto realm /
// character / world steps, autotarget, the debug disconnect) work only against a dev server. A dev server is an
// address in the wowee.cfg devhosts= list (default kDefaultDevHosts) that is ALSO a private / loopback IPv4 address:
// a public address written into devhosts is ignored, so no cfg key can unlock the aids on a public server. The address
// judged is the one the socket actually connected to (psp_socket_wrap.cpp), for the logon server and for the realm.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wowee::platform::psp::pubsafe {

// Loopback only: a LAN dev server is named in wowee.cfg devhosts= (e.g. devhosts=<the server's LAN address>,127.0.0.1).
inline constexpr const char* kDefaultDevHosts = "127.0.0.1,localhost";
inline constexpr int kMaxDevHosts = 16;

// Dotted IPv4 ("a.b.c.d") -> host-order value (192 << 24 | ...). Strict: four decimal parts 0..255, nothing else.
inline bool parseIpv4(const char* s, size_t n, uint32_t& out) {
    uint32_t v = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        if (i >= n || s[i] < '0' || s[i] > '9') return false;
        uint32_t p = 0;
        int digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            p = p * 10u + static_cast<uint32_t>(s[i] - '0');
            if (++digits > 3 || p > 255u) return false;
            ++i;
        }
        v = (v << 8) | p;
        ++parts;
        if (parts < 4) {
            if (i >= n || s[i] != '.') return false;
            ++i;
        }
    }
    if (i != n) return false;
    out = v;
    return true;
}
inline bool parseIpv4(const char* s, uint32_t& out) { return s && parseIpv4(s, std::strlen(s), out); }

// Loopback 127/8, RFC 1918 10/8, 172.16/12, 192.168/16, link-local 169.254/16.
inline bool isPrivateIpv4(uint32_t ip) {
    const uint32_t a = ip >> 24, b = (ip >> 16) & 0xFFu;
    return a == 127u || a == 10u || (a == 172u && b >= 16u && b <= 31u) || (a == 192u && b == 168u) ||
           (a == 169u && b == 254u);
}

inline void formatIpv4(uint32_t ip, char* out, size_t cap) {
    if (!out || cap == 0) return;
    char tmp[16];
    int n = 0;
    for (int k = 3; k >= 0; --k) {
        unsigned part = (ip >> (k * 8)) & 0xFFu;
        char d[3];
        int nd = 0;
        do {
            d[nd++] = static_cast<char>('0' + part % 10u);
            part /= 10u;
        } while (part);
        while (nd) tmp[n++] = d[--nd];
        if (k) tmp[n++] = '.';
    }
    tmp[n] = 0;
    size_t i = 0;
    for (; i + 1 < cap && tmp[i]; ++i) out[i] = tmp[i];
    out[i] = 0;
}

inline bool equalsNoCase(const char* a, size_t n, const char* b) {
    if (std::strlen(b) != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

struct DevHosts {
    uint32_t ips[kMaxDevHosts] = {};
    int count = 0;
    int ignoredPublic = 0;   // public addresses in the list (never dev)
    int ignoredInvalid = 0;  // names other than "localhost", malformed entries
};

// "a,b, c" -> the allowed addresses. "localhost" = 127.0.0.1; other names are not resolved (a name in the list could
// be pointed anywhere by DNS) - they count as invalid.
inline DevHosts parseDevHosts(const char* list) {
    DevHosts d;
    if (!list) return d;
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        const char* s = p;
        while (*p && *p != ',') ++p;
        size_t n = static_cast<size_t>(p - s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) --n;
        if (!n) continue;
        uint32_t ip = 0;
        if (equalsNoCase(s, n, "localhost")) ip = 0x7F000001u;
        else if (!parseIpv4(s, n, ip)) {
            ++d.ignoredInvalid;
            continue;
        }
        if (!isPrivateIpv4(ip)) {
            ++d.ignoredPublic;
            continue;
        }
        if (d.count < kMaxDevHosts) d.ips[d.count++] = ip;
    }
    return d;
}

enum class Reason : uint8_t { DevHost, NotInDevHosts, PublicAddress, Unresolved, NameAtBoot };

inline const char* reasonName(Reason r) {
    switch (r) {
        case Reason::DevHost: return "devhost";
        case Reason::NotInDevHosts: return "not_in_devhosts";
        case Reason::PublicAddress: return "public_address";
        case Reason::Unresolved: return "unresolved";
        case Reason::NameAtBoot: return "name_not_resolved_yet";
    }
    return "?";
}

// The address the socket connected to (0 = none known) against the list. True = dev (the aids may run).
inline bool decide(const DevHosts& d, uint32_t connectedIp, Reason& why) {
    if (connectedIp == 0) {
        why = Reason::Unresolved;
        return false;
    }
    if (!isPrivateIpv4(connectedIp)) {
        why = Reason::PublicAddress;
        return false;
    }
    for (int i = 0; i < d.count; ++i)
        if (d.ips[i] == connectedIp) {
            why = Reason::DevHost;
            return true;
        }
    why = Reason::NotInDevHosts;
    return false;
}

// At boot, before the network is up: the cfg server= text. An IPv4 literal or "localhost" is judged now; a DNS name
// stays undecided (the aids that start at boot - autopad, the autopilot - then do not run).
inline bool bootLiteral(const char* server, uint32_t& ip) {
    if (!server) return false;
    if (equalsNoCase(server, std::strlen(server), "localhost")) {
        ip = 0x7F000001u;
        return true;
    }
    return parseIpv4(server, ip);
}

// The latch: Unknown -> Dev / Public; Public never goes back (a later dev connect cannot unlock this run).
enum class State : uint8_t { Unknown, Dev, Public };

inline State latch(State cur, bool dev) {
    if (cur == State::Public) return State::Public;
    return dev ? State::Dev : State::Public;
}

}  // namespace wowee::platform::psp::pubsafe
