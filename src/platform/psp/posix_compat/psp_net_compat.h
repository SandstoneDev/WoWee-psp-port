/* PSP build only: POSIX networking pieces pspdev's newlib does not provide,
 * force-included (-include) into upstream's socket sources so that
 * include/network/net_platform.hpp and the src/network sources compile unchanged.
 *
 *  - select()/fd_set/timeval live in <sys/select.h>/<sys/time.h>, which the
 *    POSIX branch of net_platform.hpp does not include itself;
 *  - getaddrinfo()/freeaddrinfo() do not exist - only gethostbyname() in
 *    libcglue - so a minimal IPv4, TCP-only version is provided here.
 *    Dotted quads are parsed locally (no DNS round trip, no resolver needed).
 */
#pragma once

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>

#ifndef WOWEE_PSP_HAVE_ADDRINFO
#define WOWEE_PSP_HAVE_ADDRINFO 1

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};

#ifndef EAI_NONAME
#define EAI_NONAME (-2)
#define EAI_MEMORY (-10)
#endif

static inline int wowee_psp_parse_ipv4(const char* s, unsigned long* out) {
    unsigned long v = 0;
    for (int part = 0; part < 4; ++part) {
        if (*s < '0' || *s > '9') return 0;
        unsigned long octet = 0;
        while (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (unsigned long)(*s - '0');
            if (octet > 255) return 0;
            ++s;
        }
        v = (v << 8) | octet;
        if (part < 3) {
            if (*s != '.') return 0;
            ++s;
        }
    }
    if (*s != '\0') return 0;
    *out = v;
    return 1;
}

static inline int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                              struct addrinfo** res) {
    (void)service;
    (void)hints;
    if (!node || !res) return EAI_NONAME;
    unsigned long hostOrder = 0;
    struct in_addr addr;
    if (wowee_psp_parse_ipv4(node, &hostOrder)) {
        addr.s_addr = htonl(hostOrder);
    } else {
        struct hostent* he = gethostbyname(node);
        if (!he || !he->h_addr_list || !he->h_addr_list[0]) return EAI_NONAME;
        memcpy(&addr, he->h_addr_list[0], sizeof(addr));
    }
    struct addrinfo* ai = (struct addrinfo*)calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
    if (!ai) return EAI_MEMORY;
    struct sockaddr_in* sin = (struct sockaddr_in*)(ai + 1);
    sin->sin_family = AF_INET;
    sin->sin_addr = addr;
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr*)sin;
    *res = ai;
    return 0;
}

static inline void freeaddrinfo(struct addrinfo* ai) { free(ai); }

#endif
