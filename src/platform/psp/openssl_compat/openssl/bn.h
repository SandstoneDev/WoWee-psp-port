// PSP build only: the subset of OpenSSL's BIGNUM API that WoWee's auth layer
// uses, implemented on mbedTLS (pspdev ships mbedTLS 2.28, not OpenSSL).
// Lets src/auth/*.cpp compile unchanged. Implementation: psp_openssl_compat.cpp.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bignum_st BIGNUM;
typedef struct bignum_ctx BN_CTX;
typedef unsigned long BN_ULONG;

BIGNUM* BN_new(void);
void BN_free(BIGNUM* a);
BIGNUM* BN_dup(const BIGNUM* a);
int BN_set_word(BIGNUM* a, BN_ULONG w);
BIGNUM* BN_bin2bn(const unsigned char* s, int len, BIGNUM* ret);
int BN_bn2bin(const BIGNUM* a, unsigned char* to);
int BN_num_bytes(const BIGNUM* a);
int BN_hex2bn(BIGNUM** a, const char* str);
char* BN_bn2hex(const BIGNUM* a);

int BN_add(BIGNUM* r, const BIGNUM* a, const BIGNUM* b);
int BN_sub(BIGNUM* r, const BIGNUM* a, const BIGNUM* b);
int BN_mul(BIGNUM* r, const BIGNUM* a, const BIGNUM* b, BN_CTX* ctx);
// Note: returns the non-negative residue (OpenSSL's BN_nnmod behaviour).
// WoWee only reduces non-negative values, where the two agree.
int BN_mod(BIGNUM* rem, const BIGNUM* a, const BIGNUM* m, BN_CTX* ctx);
int BN_mod_exp(BIGNUM* r, const BIGNUM* a, const BIGNUM* p, const BIGNUM* m, BN_CTX* ctx);
int BN_cmp(const BIGNUM* a, const BIGNUM* b);
int BN_is_zero(const BIGNUM* a);

BN_CTX* BN_CTX_new(void);
void BN_CTX_free(BN_CTX* c);

#ifdef __cplusplus
}
#endif

#include <openssl/crypto.h>
