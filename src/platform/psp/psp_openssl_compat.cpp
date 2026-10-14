// OpenSSL-subset shim on mbedTLS 2.28 for the PSP build (see openssl_compat/).
//
// Only what src/auth needs: BIGNUM arithmetic for SRP6a, SHA1, MD5, HMAC-SHA1
// and RAND_bytes. Every function reports failure the OpenSSL way (0 / nullptr).

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>

#include <pspkerneltypes.h>
#include <pspthreadman.h>
#include <psputils.h>

#include <cstdlib>
#include <cstring>
#include <new>

struct bignum_st {
    mbedtls_mpi m;
};
struct bignum_ctx {
    int unused;
};
struct evp_md_st {
    mbedtls_md_type_t type;
};

namespace {

const evp_md_st kMd5{MBEDTLS_MD_MD5};
const evp_md_st kSha1{MBEDTLS_MD_SHA1};

bool g_drbgReady = false;
mbedtls_ctr_drbg_context g_drbg;

// Entropy for the DRBG seed. The PSP gives user code no hardware RNG (KIRK
// is kernel-only), so this mixes the microsecond clock, the firmware's
// Mersenne Twister seeded from it, and timing jitter of cache-missing loads.
// Adequate for an SRP ephemeral against a self-hosted server; revisit
// (kernel PRX + KIRK) before talking to public servers.
int pspEntropy(void*, unsigned char* out, size_t len) {
    SceKernelUtilsMt19937Context mt;
    sceKernelUtilsMt19937Init(&mt, static_cast<u32>(sceKernelGetSystemTimeWide()));
    for (size_t i = 0; i < len; ++i) {
        uint64_t t0 = sceKernelGetSystemTimeWide();
        volatile uint32_t sink = 0;
        for (int k = 0; k < 64 + static_cast<int>(t0 & 63); ++k) sink = sink + static_cast<uint32_t>(k * t0);
        uint64_t t1 = sceKernelGetSystemTimeWide();
        out[i] = static_cast<unsigned char>(sceKernelUtilsMt19937UInt(&mt) ^ (t1 - t0) ^ (t1 >> 3) ^ sink);
    }
    return 0;
}

bool drbg() {
    if (g_drbgReady) return true;
    mbedtls_ctr_drbg_init(&g_drbg);
    static const unsigned char pers[] = "wowee-psp";
    if (mbedtls_ctr_drbg_seed(&g_drbg, pspEntropy, nullptr, pers, sizeof(pers) - 1) != 0) return false;
    g_drbgReady = true;
    return true;
}

} // namespace

extern "C" {

BIGNUM* BN_new(void) {
    auto* b = new (std::nothrow) bignum_st;
    if (b) mbedtls_mpi_init(&b->m);
    return b;
}

void BN_free(BIGNUM* a) {
    if (!a) return;
    mbedtls_mpi_free(&a->m);
    delete a;
}

BIGNUM* BN_dup(const BIGNUM* a) {
    if (!a) return nullptr;
    BIGNUM* r = BN_new();
    if (r && mbedtls_mpi_copy(&r->m, &a->m) != 0) {
        BN_free(r);
        return nullptr;
    }
    return r;
}

int BN_set_word(BIGNUM* a, BN_ULONG w) {
    // mbedtls_mpi_lset takes a signed limb; split to stay correct for w > INT_MAX.
    if (mbedtls_mpi_lset(&a->m, static_cast<mbedtls_mpi_sint>(w >> 16)) != 0) return 0;
    if (mbedtls_mpi_shift_l(&a->m, 16) != 0) return 0;
    return mbedtls_mpi_add_int(&a->m, &a->m, static_cast<mbedtls_mpi_sint>(w & 0xFFFFu)) == 0;
}

BIGNUM* BN_bin2bn(const unsigned char* s, int len, BIGNUM* ret) {
    BIGNUM* r = ret ? ret : BN_new();
    if (!r) return nullptr;
    if (mbedtls_mpi_read_binary(&r->m, s, static_cast<size_t>(len)) != 0) {
        if (!ret) BN_free(r);
        return nullptr;
    }
    return r;
}

int BN_num_bytes(const BIGNUM* a) { return static_cast<int>(mbedtls_mpi_size(&a->m)); }

int BN_bn2bin(const BIGNUM* a, unsigned char* to) {
    size_t n = mbedtls_mpi_size(&a->m);
    if (n && mbedtls_mpi_write_binary(&a->m, to, n) != 0) return 0;
    return static_cast<int>(n);
}

int BN_hex2bn(BIGNUM** a, const char* str) {
    if (!a || !str) return 0;
    if (!*a) *a = BN_new();
    if (!*a) return 0;
    if (mbedtls_mpi_read_string(&(*a)->m, 16, str) != 0) return 0;
    return static_cast<int>(std::strlen(str));
}

char* BN_bn2hex(const BIGNUM* a) {
    size_t olen = 0;
    mbedtls_mpi_write_string(&a->m, 16, nullptr, 0, &olen);  // asks for the size
    char* buf = static_cast<char*>(std::malloc(olen + 1));
    if (!buf) return nullptr;
    if (mbedtls_mpi_write_string(&a->m, 16, buf, olen + 1, &olen) != 0) {
        std::free(buf);
        return nullptr;
    }
    return buf;
}

int BN_add(BIGNUM* r, const BIGNUM* a, const BIGNUM* b) { return mbedtls_mpi_add_mpi(&r->m, &a->m, &b->m) == 0; }
int BN_sub(BIGNUM* r, const BIGNUM* a, const BIGNUM* b) { return mbedtls_mpi_sub_mpi(&r->m, &a->m, &b->m) == 0; }

int BN_mul(BIGNUM* r, const BIGNUM* a, const BIGNUM* b, BN_CTX*) {
    return mbedtls_mpi_mul_mpi(&r->m, &a->m, &b->m) == 0;
}

int BN_mod(BIGNUM* rem, const BIGNUM* a, const BIGNUM* m, BN_CTX*) {
    return mbedtls_mpi_mod_mpi(&rem->m, &a->m, &m->m) == 0;
}

int BN_mod_exp(BIGNUM* r, const BIGNUM* a, const BIGNUM* p, const BIGNUM* m, BN_CTX*) {
    // mbedtls requires an odd modulus - the SRP prime N is.
    return mbedtls_mpi_exp_mod(&r->m, &a->m, &p->m, &m->m, nullptr) == 0;
}

int BN_cmp(const BIGNUM* a, const BIGNUM* b) { return mbedtls_mpi_cmp_mpi(&a->m, &b->m); }
int BN_is_zero(const BIGNUM* a) { return mbedtls_mpi_cmp_int(&a->m, 0) == 0; }

BN_CTX* BN_CTX_new(void) { return new (std::nothrow) bignum_ctx{0}; }
void BN_CTX_free(BN_CTX* c) { delete c; }

int RAND_bytes(unsigned char* buf, int num) {
    if (num <= 0) return 1;
    if (!drbg()) return 0;
    return mbedtls_ctr_drbg_random(&g_drbg, buf, static_cast<size_t>(num)) == 0;
}

unsigned char* SHA1(const unsigned char* d, size_t n, unsigned char* md) {
    return mbedtls_sha1_ret(d, n, md) == 0 ? md : nullptr;
}

const EVP_MD* EVP_md5(void) { return &kMd5; }
const EVP_MD* EVP_sha1(void) { return &kSha1; }

int EVP_Digest(const void* data, size_t count, unsigned char* md, unsigned int* size, const EVP_MD* type,
               ENGINE*) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type->type);
    if (!info || mbedtls_md(info, static_cast<const unsigned char*>(data), count, md) != 0) return 0;
    if (size) *size = mbedtls_md_get_size(info);
    return 1;
}

unsigned char* HMAC(const EVP_MD* evp_md, const void* key, int key_len, const unsigned char* d, size_t n,
                    unsigned char* md, unsigned int* md_len) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(evp_md->type);
    if (!info || mbedtls_md_hmac(info, static_cast<const unsigned char*>(key), static_cast<size_t>(key_len), d, n,
                                 md) != 0) {
        return nullptr;
    }
    if (md_len) *md_len = mbedtls_md_get_size(info);
    return md;
}

} // extern "C"
