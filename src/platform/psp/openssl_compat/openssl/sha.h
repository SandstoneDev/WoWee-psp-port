// PSP build only - see bn.h.
#pragma once

#include <stddef.h>

#define SHA_DIGEST_LENGTH 20

#ifdef __cplusplus
extern "C" {
#endif

unsigned char* SHA1(const unsigned char* d, size_t n, unsigned char* md);

#ifdef __cplusplus
}
#endif
