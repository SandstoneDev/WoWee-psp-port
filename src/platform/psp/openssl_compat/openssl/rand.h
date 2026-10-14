// PSP build only - see bn.h.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// mbedTLS CTR_DRBG seeded from PSP timers (see psp_openssl_compat.cpp for
// the entropy caveat). Returns 1 on success.
int RAND_bytes(unsigned char* buf, int num);

#ifdef __cplusplus
}
#endif
