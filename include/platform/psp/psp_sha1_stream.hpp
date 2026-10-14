// PSP: streaming SHA-1 and HMAC-SHA1 (docs/PSP_PORT_ARCHITECTURE.md) - header-only, no library.
//
// The mbedTLS shim of the PSP build (src/platform/psp/openssl_compat) has only one-shot SHA1()/HMAC() over a buffer
// in memory; the logon integrity hash runs over the 7.7 MB executable on the card in pieces (psp_integrity.cpp), so
// it needs an incremental digest. Plain FIPS 180-1; the host test (warden_host_test.sh) checks it against the FIPS
// vectors, RFC 2202 and OpenSSL.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wowee::platform::psp {

class Sha1Stream {
public:
    Sha1Stream() { reset(); }

    void reset() {
        h_[0] = 0x67452301u;
        h_[1] = 0xEFCDAB89u;
        h_[2] = 0x98BADCFEu;
        h_[3] = 0x10325476u;
        h_[4] = 0xC3D2E1F0u;
        bytes_ = 0;
        used_ = 0;
    }

    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bytes_ += n;
        if (used_) {
            const size_t take = n < 64 - used_ ? n : 64 - used_;
            std::memcpy(block_ + used_, p, take);
            used_ += take;
            p += take;
            n -= take;
            if (used_ < 64) return;
            compress(block_);
            used_ = 0;
        }
        while (n >= 64) {
            compress(p);
            p += 64;
            n -= 64;
        }
        if (n) {
            std::memcpy(block_, p, n);
            used_ = n;
        }
    }

    void final(uint8_t out[20]) {
        const uint64_t bits = bytes_ * 8u;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (used_ != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
        }
    }

    static void digest(const void* data, size_t n, uint8_t out[20]) {
        Sha1Stream s;
        s.update(data, n);
        s.final(out);
    }

private:
    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void compress(const uint8_t* b) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(b[i * 4]) << 24) | (static_cast<uint32_t>(b[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(b[i * 4 + 2]) << 8) | static_cast<uint32_t>(b[i * 4 + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (bb & c) | (~bb & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = bb ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (bb & c) | (bb & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = bb ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(bb, 30);
            bb = a;
            a = t;
        }
        h_[0] += a;
        h_[1] += bb;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    uint32_t h_[5];
    uint64_t bytes_;
    uint8_t block_[64];
    size_t used_;
};

// HMAC-SHA1 (RFC 2104) over a stream: begin(key), update(...)*, final().
class HmacSha1Stream {
public:
    void begin(const uint8_t* key, size_t keyLen) {
        uint8_t k[64] = {};
        if (keyLen > 64) Sha1Stream::digest(key, keyLen, k);
        else if (keyLen) std::memcpy(k, key, keyLen);
        uint8_t pad[64];
        for (int i = 0; i < 64; ++i) pad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        inner_.reset();
        inner_.update(pad, 64);
        for (int i = 0; i < 64; ++i) opad_[i] = static_cast<uint8_t>(k[i] ^ 0x5C);
    }
    void update(const void* data, size_t n) { inner_.update(data, n); }
    void final(uint8_t out[20]) {
        uint8_t ih[20];
        inner_.final(ih);
        Sha1Stream o;
        o.update(opad_, 64);
        o.update(ih, 20);
        o.final(out);
    }

private:
    Sha1Stream inner_;
    uint8_t opad_[64] = {};
};

}  // namespace wowee::platform::psp
