// PSP: the portable half of psp_integrity.hpp (no PSP calls - the host test links it).
#include "platform/psp/psp_integrity.hpp"

#include "platform/psp/psp_sha1_stream.hpp"

namespace wowee::platform::psp {

bool integrityChecksumStream(const std::array<uint8_t, 16>& salt, IntegrityRead read, void* ctx, uint8_t* buf, int cap,
                             uint8_t outChecksum[20], uint64_t* bytes) {
    HmacSha1Stream h;
    h.begin(salt.data(), salt.size());
    uint64_t total = 0;
    for (;;) {
        const int n = read(ctx, buf, cap);
        if (n < 0) return false;
        if (n == 0) break;
        h.update(buf, static_cast<size_t>(n));
        total += static_cast<uint64_t>(n);
    }
    h.final(outChecksum);
    if (bytes) *bytes = total;
    return true;
}

void integrityFinal(const std::vector<uint8_t>& clientPublicKeyA, const uint8_t checksum[20],
                    std::array<uint8_t, 20>& outHash) {
    Sha1Stream s;  // upstream: SHA1(A || checksum)
    s.update(clientPublicKeyA.data(), clientPublicKeyA.size());
    s.update(checksum, 20);
    s.final(outHash.data());
}

}  // namespace wowee::platform::psp
