// PSP build only - see bn.h.
#pragma once

#include <openssl/evp.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned char* HMAC(const EVP_MD* evp_md, const void* key, int key_len, const unsigned char* d,
                    size_t n, unsigned char* md, unsigned int* md_len);

#ifdef __cplusplus
}
#endif
