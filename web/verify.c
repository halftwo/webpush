#include "verify.h"
#include "xslib/xnet.h"
#include "xslib/sha1.h"
#include "xslib/cstr.h"
#include "xslib/uuid.h"
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>


static sha1_context _sha1_ctx;


typedef struct vstruct_t vstruct_t;
struct vstruct_t
{
	unsigned char sha1[20];
	unsigned char salt[VERIFY_SALT_SIZE];
	unsigned char time[VERIFY_TIME_SIZE];
};

#define VSTRUCT_B64_SIZE	XBASE64_ENCODED_LEN(sizeof(vstruct_t))

void verify_init()
{
	uuid_t secret;

	uuid_generate_random(secret);

	sha1_start(&_sha1_ctx);
	sha1_update(&_sha1_ctx, secret, sizeof(secret));
}

int verify_generate(char *verify, const char *user, size_t user_len, int t)
{
	vstruct_t vs;
	sha1_context ctx;
	int n;

	if (user_len > USER_MAX_SIZE)
		return -1;

	*(uint32_t *)vs.salt = random();
	*(uint32_t *)vs.time = t;
	xnet_msb(vs.time, sizeof(vs.time));

	memcpy(&ctx, &_sha1_ctx, sizeof(_sha1_ctx));
	sha1_update(&ctx, vs.salt, VERIFY_SALT_SIZE);
	sha1_update(&ctx, vs.time, VERIFY_TIME_SIZE);
	sha1_update(&ctx, user, user_len);
	sha1_finish(&ctx, vs.sha1);

	n = xbase64_encode(&url_xbase64, verify, &vs, sizeof(vs), XBASE64_NO_PADDING);
	verify[n++] = '.';
	n += xbase64_encode(&url_xbase64, verify + n, user, user_len, XBASE64_NO_PADDING);
	return n;
}

int verify_check(const char *verify, size_t size, const char *user, size_t user_len)
{
	vstruct_t vs;
	char vuser[USER_MAX_SIZE];
	const char *s;
	int m, n;
	sha1_context ctx;
	unsigned char sha1[20];

	if (user_len > USER_MAX_SIZE || size > VERIFY_MAX_SIZE)
		return 0;

	s = verify;
	m = XBASE64_ENCODED_LEN(sizeof(vs));
	if (size <= m || verify[m] != '.')
		return 0;

	n = xbase64_decode(&url_xbase64, &vs, s, m, 0);
	if (n != sizeof(vs))
		return 0;

	++m;
	s = verify + m;
	m = size - m ;
	if (m != XBASE64_ENCODED_LEN(user_len))
		return 0;

	n = xbase64_decode(&url_xbase64, &vuser, s, m, 0);
	if (n != user_len || memcmp(vuser, user, user_len) != 0)
		return 0;


	memcpy(&ctx, &_sha1_ctx, sizeof(_sha1_ctx));
	sha1_update(&ctx, vs.salt, VERIFY_SALT_SIZE);
	sha1_update(&ctx, vs.time, VERIFY_TIME_SIZE);
	sha1_update(&ctx, user, user_len);
	sha1_finish(&ctx, sha1);

	if (memcmp(sha1, vs.sha1, sizeof(sha1)) != 0)
		return 0;

	xnet_msb(vs.time, sizeof(vs.time));
	return *(uint32_t *)vs.time;
}

#ifdef TEST_VERIFY

int main(int argc, char** argv)
{
	int64_t uid = 33567;
	char verify[VERIFY_MAX_SIZE];
	char user[32];
	int len = snprintf(user, sizeof(user), "%jd", (intmax_t)uid);

	verify_init();
	len = verify_generate(verify, user, strlen(user), time(NULL));
	if (len <= 0)
	{
		printf("generate cookie for user(%jd) failed\n", uid);
		return -1;
	}

	int t = verify_check(verify, strlen(verify), user, strlen(user));
	if (t > 0 && (t + (3600 * 24) > time(NULL)))
	{
		printf("OK\n");
	}
	else
		printf("ERROR\n");

	return 0;
}

#endif
