// PSP build only - see bn.h.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evp_md_st EVP_MD;
typedef struct engine_st ENGINE;

const EVP_MD* EVP_md5(void);
const EVP_MD* EVP_sha1(void);

int EVP_Digest(const void* data, size_t count, unsigned char* md, unsigned int* size,
               const EVP_MD* type, ENGINE* impl);

#ifdef __cplusplus
}
#endif
