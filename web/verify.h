#ifndef verify_h_
#define verify_h_

#include "xslib/xstr.h"
#include "xslib/xbase64.h"
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define VERIFY_SALT_SIZE	sizeof(uint32_t)
#define VERIFY_TIME_SIZE	sizeof(uint32_t)
#define USER_MAX_SIZE		128
#define VERIFY_MAX_SIZE		(XBASE64_ENCODED_LEN(20 + VERIFY_SALT_SIZE + VERIFY_TIME_SIZE) + 1 + XBASE64_ENCODED_LEN(USER_MAX_SIZE) + 1)

#ifdef __cplusplus
extern "C" {
#endif


void verify_init();

int verify_generate(char *verify, const char *user, size_t len, int t);

int verify_check(const char *verify, size_t vlen, const char *user, size_t len);


#ifdef __cplusplus
}
#endif

#endif
