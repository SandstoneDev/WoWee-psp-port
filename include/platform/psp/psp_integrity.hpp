// PSP: the logon integrity hash ("CRC hash" of AUTH_LOGON_PROOF) from the card (docs/PSP_PORT_ARCHITECTURE.md).
//
// Upstream (src/auth/integrity.cpp, build 12340): checksum = HMAC_SHA1(salt, client executable), hash = SHA1(A ||
// checksum), with the whole executable read into memory (7.7 MB, twice with the copy). The PSP streams the player's
// copy (data/client/) through a 16 KB buffer (psp_sha1_stream.hpp), keeps the checksum per salt - in memory and in
// a ".hmac" file beside it (salt, size, time of the file, checksum) - so the next login of this salt costs nothing.
// No executable: false, the proof goes without the hash as before (log "integrity ok=0 reason=missing").
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::platform::psp {

bool integrityHash(const std::array<uint8_t, 16>& salt, const std::vector<uint8_t>& clientPublicKeyA,
                   std::array<uint8_t, 20>& outHash, std::string& outError);

// The portable half (host test): checksum over a reader that fills buf and returns the bytes read (0 = end, < 0 =
// error); false on a read error.
using IntegrityRead = int (*)(void* ctx, uint8_t* buf, int cap);
bool integrityChecksumStream(const std::array<uint8_t, 16>& salt, IntegrityRead read, void* ctx, uint8_t* buf, int cap,
                             uint8_t outChecksum[20], uint64_t* bytes = nullptr);
void integrityFinal(const std::vector<uint8_t>& clientPublicKeyA, const uint8_t checksum[20],
                    std::array<uint8_t, 20>& outHash);

}  // namespace wowee::platform::psp
